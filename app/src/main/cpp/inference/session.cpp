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
    std::mutex              slotMutex;
    std::condition_variable slotCv;
    std::vector<uint8_t>    slot;          // owns the pixels
    FrameView               slotView;      // points into `slot`
    uint64_t                slotSeq = 0;   // bumps per publish
    uint64_t                takenSeq = 0;  // last one the worker picked up
    bool                    stop = false;

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

void Session::Impl::run() {
    std::vector<uint8_t> local;

    for (;;) {
        uint64_t seq = 0;
        int w = 0, h = 0, stride = 0;
        {
            std::unique_lock<std::mutex> lock(slotMutex);
            slotCv.wait(lock, [&] { return stop || slotSeq != takenSeq; });
            if (stop) return;

            seq = slotSeq;
            takenSeq = seq;

            w = slotView.width;
            h = slotView.height;
            stride = slotView.rowStrideBytes;

            // Copied out under the lock, then run without it: holding the slot
            // lock across an inference would block the capture thread's submit()
            // for the length of a frame, which is the one thing submit() exists
            // not to do.
            if (local.size() != slot.size()) local.resize(slot.size());
            memcpy(local.data(), slot.data(), slot.size());
        }

        std::shared_ptr<Engine> eng;
        {
            std::lock_guard<std::mutex> lock(engineMutex);
            eng = engine;
        }
        if (!eng) continue;

        FrameView view;
        view.pixels = local.data();
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
    {
        std::lock_guard<std::mutex> lock(impl_->slotMutex);
        if (impl_->stop) return;

        if (impl_->slotSeq != impl_->takenSeq) {
            // The worker never got to the previous frame. Counting it here is
            // what makes the drop rate visible instead of mysterious.
            std::lock_guard<std::mutex> slock(impl_->statsMutex);
            impl_->stats.dropped++;
        }

        if (impl_->slot.size() != bytes) impl_->slot.resize(bytes);
        memcpy(impl_->slot.data(), frame.pixels, bytes);

        impl_->slotView.pixels = impl_->slot.data();
        impl_->slotView.width = frame.width;
        impl_->slotView.height = frame.height;
        impl_->slotView.rowStrideBytes = frame.rowStrideBytes;
        impl_->slotSeq++;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->statsMutex);
        impl_->stats.submitted++;
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
