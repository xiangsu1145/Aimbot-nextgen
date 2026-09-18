// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of session.h
//
//  A worker thread, a one-slot mailbox, and a done flag. That is the whole
//  mechanism; the rest is bookkeeping.
//
//  The mailbox holds exactly one frame. submit() overwrites whatever is in it
//  and bumps a counter if the worker had not picked that one up yet. Nothing
//  queues, and that is deliberate: at 100+ fps produced and maybe 30 inferences
//  a second affordable, a queue would grow without bound and every result would
//  describe a screen that has since moved on. Dropping is the feature.
//
//  The copy rule is the one the rest of the project already uses: a FrameView is
//  only valid inside the call it was passed to, so submit() memcpy()s into the
//  slot rather than keeping the pointer. The capture buffer belongs to the frame
//  thread and is overwritten by the next frame regardless of who is reading.
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/session.h"

#include "inference/shared_region.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace {

using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::milliseconds;

/// How often the rate window is closed. Short enough to react to a backend that
/// has just changed speed, long enough that the number does not flicker.
constexpr auto kRateWindow = Ms(1000);

}  // namespace

struct Session::Impl {
    // ── Configuration, written by callers ──────────────────────────────────
    mutable std::mutex cfgMutex;
    Roi      roi;
    ResultFn onResult;
    bool     wants = false;        // a model is installed and someone is reading

    // ── The mailbox ────────────────────────────────────────────────────────
    //
    // Two buffers, not one, and that is what removes both copies. A single slot
    // forces a memcpy on the way in (so the capture thread can overwrite its
    // own buffer the moment submit() returns) and a second on the way out (so
    // the capture thread can publish the next frame while the worker is still
    // reading the previous one). With two buffers the worker owns one outright
    // for the duration of a run and submit() writes the other, so neither side
    // has to copy and neither can disturb the other.
    //
    // `buf[i]` is what the pixels arrive in, `owner` is whichever buffer the
    // worker currently holds (-1 when it holds neither). submit() writes
    // `owner ^ 1` when a buffer is owned and round-robins otherwise.
    //
    // The buffers are shared memory when the platform grants it and heap
    // vectors when it does not; `shm` is the difference between the two. See
    // shared_region.h for why the fallback has to exist.
    std::mutex              slotMutex;
    std::condition_variable slotCv;
    SharedRegion            shm[2];
    std::vector<uint8_t>    heap[2];       // used when shm[i].ok() is false
    size_t                  slotBytes = 0; // the capacity both were made for
    int                     owner = -1;    // buffer the worker is reading, or -1
    int                     nextSeq = 0;   // which buffer the next publish uses

    /// Which buffer submit() wrote last. The worker reads this rather than
    /// deriving it from nextSeq: nextSeq has already been advanced to the
    /// *following* buffer by the time the worker wakes, and a worker that
    /// reconstructs "the one published" by inverting it is one edit away from
    /// reading the other one — which is an all-zero frame and a pipeline that
    /// silently detects nothing. Recorded explicitly, it cannot drift.
    int                     published = -1;

    FrameView               slotView;      // describes the buffer last published
    uint64_t                slotSeq = 0;   // bumps per publish
    uint64_t                takenSeq = 0;  // last one the worker picked up
    bool                    stop = false;

    /// The pixels of buffer `i` at the current size, or null when it is not
    /// allocated. Callers must hold slotMutex.
    uint8_t* bytesOf(int i) {
        if (i < 0 || i > 1) return nullptr;
        return shm[i].ok() ? static_cast<uint8_t*>(shm[i].data()) : heap[i].data();
    }

    /// Makes both buffers at least `bytes` large. Returns false only when the
    /// allocation failed outright, which leaves the previous size in place.
    /// Callers must hold slotMutex.
    bool ensureCapacity(size_t bytes);

    std::thread worker;

    // ── Engine, swapped by load()/unload() ─────────────────────────────────
    // Held by shared_ptr so the worker can keep using the old engine to the end
    // of a frame while load() replaces it, instead of the alternative: joining
    // the worker on every confidence change.
    std::mutex                    engineMutex;
    std::shared_ptr<Engine>       engine;
    ModelSpec                     loadedSpec;
    std::atomic<bool>             loaded{false};

    // ── Statistics ─────────────────────────────────────────────────────────
    mutable std::mutex statsMutex;
    Session::Stats     stats;
    Clock::time_point  windowStart = Clock::now();
    uint64_t           windowRuns  = 0;

    /// The worker's whole loop.
    ///
    /// A member rather than a free function because `Impl` is private to
    /// Session: a free function taking `Session::Impl*` is an access violation,
    /// and widening the class's interface to allow it would be worse than the
    /// five lines this costs.
    void run();
};

/// Grows both buffers to `bytes`, preferring shared memory.
///
/// Allocating shared memory is not cheap — an fd, a name registration and an
/// mmap — so it happens once per distinct size and never on the steady path.
/// The size only changes when the crop size does, which is a menu action.
bool Session::Impl::ensureCapacity(size_t bytes) {
    if (bytes == 0) return false;
    if (slotBytes >= bytes && bytesOf(0) && bytesOf(1)) return true;

    // A different size needs both buffers rebuilt: the old ones describe the
    // old geometry, and a frame read through a stale size is a torn frame.
    for (int i = 0; i < 2; ++i) shm[i].reset();
    for (int i = 0; i < 2; ++i) heap[i].clear();
    slotBytes = 0;

    // Shared memory first. It is not a way to avoid the copy — the producer's
    // pixels are a heap pointer and have to be memcpy'd in either way. What it
    // buys is a stable, page-aligned pair the worker can hold while the producer
    // writes the other one, and a mapping that survives a producer that is
    // still filling its buffer.
    bool shared = true;
    for (int i = 0; i < 2; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "aimbotng_slot_%d", i);
        if (!shm[i].create(key, bytes)) { shared = false; break; }
    }
    if (!shared) {
        // Either the first failed or the second did. Drop whatever we got so
        // the buffers cannot end up half-shared and half-heap.
        for (int i = 0; i < 2; ++i) shm[i].reset();
        LOGW("session: shared frame memory unavailable, falling back to copies");
    }

    // Allocated whether or not the shared pair came up, and deliberately so.
    // It costs 2 x bytes once, at startup, and in exchange `bytesOf()` can
    // never hand back a null slot: a partially-created pair leaves one index
    // empty, and the previous version of this function only filled the heap
    // when the *whole* shared attempt had failed, so that half-state fell
    // through to a null and was reported as "could not allocate" instead of
    // "fell back". The fallback is the ordinary path on a device with no
    // ashmem at all; it should not depend on how far the other path got.
    for (int i = 0; i < 2; ++i) heap[i].assign(bytes, 0);

    if (!bytesOf(0) || !bytesOf(1)) {
        LOGW("session: could not allocate %zu byte frame buffers", bytes);
        return false;
    }

    slotBytes = bytes;
    nextSeq = 0;
    owner = -1;
    published = -1;
    LOGI("session: frame buffers %zu bytes x2 (%s)", bytes,
         shared ? "shared memory" : "heap, copying");
    return true;
}

void Session::Impl::run() {
    uint64_t seq = 0;
    int w = 0, h = 0, stride = 0;
    const uint8_t* pixels = nullptr;

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(slotMutex);
            slotCv.wait(lock, [&] { return stop || slotSeq != takenSeq; });
            if (stop) return;

            seq = slotSeq;
            takenSeq = seq;

            // Claim the buffer submit() just wrote — read, not derived. See the
            // comment on `published` for why reconstructing it from nextSeq is
            // the bug this replaces.
            owner = published;

            w = slotView.width;
            h = slotView.height;
            stride = slotView.rowStrideBytes;
            pixels = static_cast<const uint8_t*>(slotView.pixels);
        }

        if (pixels == nullptr) continue;

        std::shared_ptr<Engine> eng;
        {
            std::lock_guard<std::mutex> lock(engineMutex);
            eng = engine;
        }
        if (!eng) {
            // Nothing to run it on. Hand the buffer back so the next submit()
            // does not keep working around a phantom owner.
            std::lock_guard<std::mutex> lock(slotMutex);
            owner = -1;
            continue;
        }

        FrameView view;
        view.pixels = const_cast<uint8_t*>(pixels);
        view.width = w;
        view.height = h;
        view.rowStrideBytes = stride;

        Roi roi;
        ResultFn cb;
        {
            std::lock_guard<std::mutex> lock(cfgMutex);
            roi = this->roi;
            cb = onResult;
        }

        const std::vector<Box> boxes = eng->run(view, roi);
        const Timing timing = eng->lastTiming();
        const bool ok = eng->lastStatus().ok;

        static int s_runs = 0;
        if (++s_runs % 30 == 1) {
            LOGI("worker ran #%d: %zu boxes (best %.3f) pre=%.2f infer=%.2f post=%.2f ms ok=%d",
                 s_runs, boxes.size(),
                 boxes.empty() ? 0.0f : std::max_element(boxes.begin(), boxes.end(),
                     [](const Box& a, const Box& b){ return a.score < b.score; })->score,
                 timing.preMs, timing.inferMs, timing.postMs, ok ? 1 : 0);
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex);
            stats.ran++;
            if (!ok) stats.failed++;
            stats.last = timing;
            if (timing.totalMs() > stats.worst.totalMs()) stats.worst = timing;
            stats.activeName = eng->activeName();
            windowRuns++;

            const auto now = Clock::now();
            const auto elapsed = now - windowStart;
            if (elapsed >= kRateWindow) {
                const double secs = std::chrono::duration<double>(elapsed).count();
                stats.fps = secs > 0.0 ? static_cast<double>(windowRuns) / secs : 0.0;
                windowRuns = 0;
                windowStart = now;
            }
        }

        if (cb) cb(boxes, roi, timing);

        // The run is over and nothing else reads these pixels, so the buffer
        // goes back into the pool. Only after the callback: a consumer that
        // keeps a pointer into the frame (to draw it, say) would otherwise see
        // it rewritten underneath it.
        {
            std::lock_guard<std::mutex> lock(slotMutex);
            owner = -1;
        }
    }
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

Session::Session() : impl_(std::make_unique<Impl>()) {
    impl_->worker = std::thread(&Impl::run, impl_.get());
    LOGI("session worker started");
}

Session::~Session() {
    if (impl_) {
        {
            std::lock_guard<std::mutex> lock(impl_->slotMutex);
            impl_->stop = true;
        }
        impl_->slotCv.notify_all();
        if (impl_->worker.joinable()) impl_->worker.join();
    }
}

Status Session::load(const ModelSpec& spec, Runtime runtime, Ep ep) {
    LoadedEngine loaded = createAndLoad(spec, runtime, ep);
    if (!loaded.ok()) {
        // The failed load leaves no engine installed. Keeping the previous one
        // would mean a broken model silently keeps running the old one, and the
        // user sees a menu that accepted their change and a detector that did
        // not.
        {
            std::lock_guard<std::mutex> lock(impl_->engineMutex);
            impl_->engine.reset();
        }
        {
            std::lock_guard<std::mutex> lock(impl_->cfgMutex);
            impl_->wants = false;
        }
        impl_->loaded.store(false);
        LOGW("load failed: %s", loaded.status.cstr());
        return loaded.status;
    }

    std::shared_ptr<Engine> shared(std::move(loaded.engine));
    {
        std::lock_guard<std::mutex> lock(impl_->engineMutex);
        impl_->engine = shared;
        impl_->loadedSpec = spec;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->cfgMutex);
        impl_->wants = true;
    }
    impl_->loaded.store(true);

    {
        std::lock_guard<std::mutex> lock(impl_->statsMutex);
        impl_->stats = Stats{};
        impl_->windowStart = Clock::now();
        impl_->windowRuns = 0;
        impl_->stats.activeName = shared->activeName();
    }

    LOGI("session loaded: %s", shared->describe().c_str());
    return Status::good();
}

void Session::unload() {
    {
        std::lock_guard<std::mutex> lock(impl_->engineMutex);
        impl_->engine.reset();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->cfgMutex);
        impl_->wants = false;
    }
    impl_->loaded.store(false);
    LOGI("session unloaded");
}

bool Session::running() const { return impl_->loaded.load(); }

// ── Configuration ───────────────────────────────────────────────────────────

void Session::setRoi(const Roi& roi) {
    std::lock_guard<std::mutex> lock(impl_->cfgMutex);
    impl_->roi = roi;
}

void Session::setConfidence(float confidence) {
    std::shared_ptr<Engine> engine;
    {
        std::lock_guard<std::mutex> lock(impl_->engineMutex);
        engine = impl_->engine;
        if (engine) impl_->loadedSpec.confidence = confidence;
    }
    if (!engine) return;

    // Handed to the engine rather than reloading the model: this is called while
    // a slider is being dragged, and rebuilding an ONNX Runtime session per
    // frame of a drag would stall the menu for a second at a time.
    engine->setConfidence(confidence);
}

// ── Frames ──────────────────────────────────────────────────────────────────

void Session::submit(const FrameView& frame) {
    if (!frame.valid()) {
        std::lock_guard<std::mutex> lock(impl_->statsMutex);
        impl_->stats.invalid++;
        return;
    }

    const size_t bytes = static_cast<size_t>(frame.rowStrideBytes) *
                         static_cast<size_t>(frame.height);
    bool copied = false;
    {
        std::lock_guard<std::mutex> lock(impl_->slotMutex);
        if (impl_->stop) return;

        if (impl_->slotSeq != impl_->takenSeq) {
            // The worker never got to the previous frame. Counting it here is
            // what makes the drop rate visible instead of mysterious.
            std::lock_guard<std::mutex> slock(impl_->statsMutex);
            impl_->stats.dropped++;
        }

        if (!impl_->ensureCapacity(bytes)) {
            std::lock_guard<std::mutex> slock(impl_->statsMutex);
            impl_->stats.invalid++;
            return;
        }

        // Where this frame goes. When the worker owns a buffer we must not
        // write it, so the other one — and only if that one is also free, since
        // owning one buffer means the other holds whatever is still in flight.
        // Otherwise alternate, which is what makes the two views coherent.
        int dst = impl_->nextSeq;
        if (impl_->owner == dst) dst ^= 1;

        uint8_t* out = impl_->bytesOf(dst);
        if (out == nullptr) {
            std::lock_guard<std::mutex> slock(impl_->statsMutex);
            impl_->stats.invalid++;
            return;
        }

        // Copy unless the source *is* the destination.
        //
        // This used to be `copied = !shm[dst].ok()`, on the assumption that a
        // shared buffer needs no copy. It does. The shared region is a page we
        // asked the kernel for; the producer's pixels are an ordinary heap
        // pointer on the other side of the JNI boundary. Two different physical
        // pages, and nothing about one being "shared" puts the frame in the
        // other. Skipping the copy there publishes a freshly zeroed page, which
        // is exactly the all-zero input this condition produced: the menu showed
        // a live preview (it reads g_frame directly) while inference saw nothing.
        //
        // The real question is never what kind of memory it is, but whether
        // anyone has to move bytes into it. If a future caller hands us a view
        // that already points at the slot, this is the line that notices.
        copied = (frame.pixels != out);
        if (copied) {
            if (frame.rowStrideBytes == frame.width * 4) {
                memcpy(out, frame.pixels, bytes);
            } else {
                // Row-by-row when the source is padded: a single memcpy would
                // fold the padding into the image.
                const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
                for (int y = 0; y < frame.height; ++y) {
                    memcpy(out + static_cast<size_t>(y) * rowBytes,
                           frame.pixels + static_cast<size_t>(y) * frame.rowStrideBytes,
                           rowBytes);
                }
            }
        }

        impl_->slotView.pixels = out;
        impl_->slotView.width = frame.width;
        impl_->slotView.height = frame.height;
        impl_->slotView.rowStrideBytes = frame.rowStrideBytes;
        impl_->published = dst;      // what the worker will read
        impl_->slotSeq++;
        impl_->nextSeq = dst ^ 1;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->statsMutex);
        impl_->stats.submitted++;
        if (copied) impl_->stats.copied++;
    }
    impl_->slotCv.notify_one();
}

bool Session::wantsFrames() const {
    std::lock_guard<std::mutex> lock(impl_->cfgMutex);
    return impl_->wants;
}

void Session::onResult(ResultFn fn) {
    std::lock_guard<std::mutex> lock(impl_->cfgMutex);
    impl_->onResult = std::move(fn);
}

// ── Statistics ──────────────────────────────────────────────────────────────

Session::Stats Session::stats() const {
    std::lock_guard<std::mutex> lock(impl_->statsMutex);
    return impl_->stats;
}

std::string Session::describe() const {
    std::shared_ptr<Engine> engine;
    {
        std::lock_guard<std::mutex> lock(impl_->engineMutex);
        engine = impl_->engine;
    }
    return engine ? engine->describe() : std::string("no engine loaded");
}

std::string Session::inputDtypeName() const {
    std::shared_ptr<Engine> engine;
    {
        std::lock_guard<std::mutex> lock(impl_->engineMutex);
        engine = impl_->engine;
    }
    return engine ? engine->inputDtypeName() : std::string("");
}

std::string Session::lastError() const {
    std::shared_ptr<Engine> engine;
    {
        std::lock_guard<std::mutex> lock(impl_->engineMutex);
        engine = impl_->engine;
    }
    if (!engine) return "no engine loaded";
    const Status& s = engine->lastStatus();
    return s.ok ? std::string() : s.message;
}

std::string Session::statusLine() const {
    const Stats s = stats();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s  %.1f fps  %.1f/%.1f/%.1f ms  ran=%llu dropped=%llu failed=%llu",
             s.activeName.empty() ? "(idle)" : s.activeName.c_str(), s.fps,
             static_cast<double>(s.last.preMs), static_cast<double>(s.last.inferMs),
             static_cast<double>(s.last.postMs),
             (unsigned long long)s.ran, (unsigned long long)s.dropped,
             (unsigned long long)s.failed);
    return buf;
}

}  // namespace infer
}  // namespace aimbotng
