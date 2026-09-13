// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer::runtime — implementation of model_runtime.h
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/model_runtime.h"

#include "capture/capture.h"
#include "inference/backend.h"
#include "inference/model_store.h"
#include "inference/session.h"

#include <android/log.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace runtime {
namespace {

std::mutex      g_mutex;         // guards everything below
Snapshot        g_snapshot;
std::string     g_describe;
std::atomic<bool> g_wants{false};

/// Created on first use, not at load.
///
/// A Session starts a worker thread, and this library is dlopen()ed by the
/// daemon at a point of its own choosing — starting threads from a static
/// constructor is the kind of thing that works until the load order changes.
/// Leaked rather than owned because it lives for the process by design and a
/// destructor would only run after the worker it is tearing down has already
/// been joined once.
Session& session() {
    static Session* s = new Session();
    return *s;
}

/// The pixels of the newest capture frame, owned here because capture hands out
/// copies into a caller-owned buffer and the session copies again on submit.
std::vector<uint8_t> g_frameBuf;
uint64_t             g_lastFrameId = 0;

/// Resolves the pair an entry should run on.
///
/// The store's Engine enum is the *user's* vocabulary and is deliberately small;
/// the pair is the implementation's. This is the one place the two meet, so the
/// mapping is written out rather than derived — a table that says what it means
/// is worth more than a function that computes something plausible.
bool resolvePair(model::Engine engine, Runtime& outRuntime, Ep& outEp,
                 std::string& why) {
    switch (engine) {
        case model::Engine::OnnxRuntime:
            outRuntime = Runtime::OnnxRuntime;
            // The user explicitly chose XNNPACK in the Add-Model / Settings
            // dialog. We do not auto-decide here — the menu is the authority.
            outEp = Ep::Xnnpack;
            return true;
        case model::Engine::OnnxCpu:
            outRuntime = Runtime::OnnxRuntime;
            // Plain ORT/MLAS CPU — no XNNPACK. For FP16 and other models that
            // misbehave under XNNPACK (dense false detections).
            outEp = Ep::Cpu;
            return true;
        case model::Engine::QnnHtp:
            outRuntime = Runtime::LiteRT;
            outEp = Ep::Htp;
            // The LiteRtEngine is responsible for actually running the graph.
            // It uses the HTP runtime, the per-SoC skel, and the system
            // layer — all pulled in by runtimeLibraries() / preloadAll().
            // If the device lacks a usable skel (caps.isQualcomm == false
            // or htpArch == 0) buildDelegate() will return null with a clear
            // reason; the menu already greys the row when epAvailable()
            // reports false. We don't second-guess here.
            return true;
        case model::Engine::Nnapi:
            outRuntime = Runtime::LiteRT;
            outEp = Ep::Nnapi;
            return true;
        case model::Engine::Cpu:
            outRuntime = Runtime::LiteRT;
            outEp = Ep::Cpu;
            return true;
        case model::Engine::TfliteXnnpack:
            outRuntime = Runtime::LiteRT;
            outEp = Ep::Xnnpack;
            return true;
        case model::Engine::TfliteGpu:
            outRuntime = Runtime::LiteRT;
            outEp = Ep::Gpu;
            return true;
        default:
            why = "this model has no runtime selected";
            return false;
    }
    why = "this runtime has no engine in this build yet";
    return false;
}

/// Converts a store entry into what an engine needs. The engine layer must not
/// see model_store, so this is the only crossing point.
ModelSpec toSpec(const model::Entry& e) {
    ModelSpec spec;
    spec.path       = e.path;
    spec.classes    = e.classes;
    spec.inputSize  = e.inputSize;
    spec.confidence = e.confidence;
    spec.cpuThreads = e.cpuThreads;   // ONNX Runtime intra-op threads (1–8)
    spec.htpPerfMode = e.htpPerfMode;  // HTP performance-mode vote (QNN only)
    spec.cacheToken = model::baseName(e.path);
    return spec;
}

/// The session's result callback. Runs on the worker thread, so it only takes
/// the lock, writes, and returns — anything longer would become a frame rate
/// limit on the pipeline itself.
void onResult(const std::vector<Box>& boxes, const Roi& roi, const Timing& timing) {
    int cropX = 0, cropY = 0;
    const bool haveOrigin = capture::cropOrigin(cropX, cropY);

    std::vector<Box> screen;
    screen.reserve(boxes.size());
    for (const Box& b : boxes) {
        Box s = b;
        if (haveOrigin) {
            s.x1 += static_cast<float>(cropX);
            s.y1 += static_cast<float>(cropY);
            s.x2 += static_cast<float>(cropX);
            s.y2 += static_cast<float>(cropY);
        }
        screen.push_back(s);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_snapshot.detections = std::move(screen);
    g_snapshot.timing = timing;
    g_snapshot.boxesIn = roi;
}

}  // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────

Status start() {
    const int id = model::loadedId();
    if (id == 0) return Status::bad("no model is marked as loaded");

    // The store is read outside the lock: model::all() takes its own, and
    // holding both would put the menu's list mutations behind an inference
    // status check for no reason.
    model::Entry entry;
    bool found = false;
    for (const model::Entry& e : model::all()) {
        if (e.id == id) { entry = e; found = true; break; }
    }
    if (!found) return Status::bad("the loaded model is no longer in the list");

    Runtime rt = Runtime::Unknown;
    Ep ep = Ep::Default;
    std::string why;
    if (!resolvePair(entry.engine, rt, ep, why)) {
        LOGW("cannot run '%s': %s", entry.name.c_str(), why.c_str());
        return Status::bad(why);
    }

    // Registered before the load, not after: the worker starts producing the
    // moment the engine is installed, and a callback attached afterwards would
    // miss the first frame or two for no reason.
    session().onResult(onResult);

    const Status s = session().load(toSpec(entry), rt, ep);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_snapshot.detections.clear();
    g_snapshot.timing = Timing{};
    g_snapshot.boxesIn = Roi{};
    g_snapshot.classes = entry.classes;
    if (s.ok) {
        g_snapshot.active = true;
        g_snapshot.status.clear();
        g_wants.store(true);
        // The engine's own account of what it bound: tensor shapes, class count,
        // which provider answered. Captured once here because it describes the
        // model rather than the run, and because a bug report needs it and a
        // per-frame path should not be building strings.
        g_describe = session().describe();
        // Publish the detected input dtype back to the store so the model list
        // can show fp16 / fp32 / int8 for an entry after it has been loaded once.
        model::setType(entry.id, session().inputDtypeName());
        LOGI("inference started on %s: %s", pairLabel(rt, ep), g_describe.c_str());
    } else {
        g_snapshot.active = false;
        g_snapshot.status = s.message;
        g_wants.store(false);
        g_describe = "failed: " + s.message;
    }
    return s;
}

void stop() {
    g_wants.store(false);
    session().unload();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_snapshot = Snapshot{};
    g_describe = "stopped";
    LOGI("inference stopped");
}

bool running() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snapshot.active;
}

bool wantsFrames() { return g_wants.load(std::memory_order_relaxed); }

// ── The frame pump ──────────────────────────────────────────────────────────

void pump() {
    if (!g_wants.load(std::memory_order_relaxed)) return;

    int w = 0, h = 0;
    if (!capture::peekSize(w, h)) return;
    if (w <= 0 || h <= 0) return;

    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    if (g_frameBuf.size() != bytes) g_frameBuf.resize(bytes);

    int gotW = 0, gotH = 0;
    uint64_t id = g_lastFrameId;
    if (!capture::copyLatestInto(g_frameBuf.data(), g_frameBuf.size(), gotW, gotH, id)) {
        return;   // nothing new since last time; not an error
    }
    g_lastFrameId = id;

    FrameView view;
    view.pixels = g_frameBuf.data();
    view.width = gotW;
    view.height = gotH;
    view.rowStrideBytes = gotW * 4;   // capture hands out tightly packed rows

    session().submit(view);
}

// ── Reporting ───────────────────────────────────────────────────────────────

Snapshot snapshot() {
    // Session's counters are read before the lock, not under it. The worker's
    // result callback takes g_mutex, so holding g_mutex while reaching into the
    // session would put a lock ordering between the two that nothing needs.
    const Session::Stats st = session().stats();

    std::lock_guard<std::mutex> lock(g_mutex);
    Snapshot copy = g_snapshot;
    copy.fps     = st.fps;
    copy.ran     = st.ran;
    copy.dropped = st.dropped;
    copy.failed  = st.failed;
    if (copy.backend.empty()) copy.backend = st.activeName;
    return copy;
}

std::string timingText() {
    // Same reason as snapshot(): read the session first, then take our own lock.
    const Session::Stats st = session().stats();

    std::lock_guard<std::mutex> lock(g_mutex);
    char buf[160];

    if (!g_snapshot.active) {
        if (g_snapshot.status.empty()) return "inference off";
        // Cut long vendor messages to something a header line can hold; the full
        // text is in the log and in describe().
        snprintf(buf, sizeof(buf), "%.44s", g_snapshot.status.c_str());
        return buf;
    }

    if (st.ran == 0) return "warming up";

    // Total first because it is the number that answers "is this fast enough".
    snprintf(buf, sizeof(buf), "%.1f ms | %.0f fps",
             static_cast<double>(st.last.totalMs()), st.fps);
    return buf;
}

std::string timingTextDetailed() {
    const Session::Stats st = session().stats();

    bool active = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        active = g_snapshot.active;
    }
    // Delegated outside the lock: timingText() takes it too, and holding it
    // across the call would be a self-deadlock rather than a race.
    if (!active || st.ran == 0) return timingText();

    char buf[192];
    // The split is the follow-up question to the total: which stage is slow
    // decides whether to change the crop size, the model, or the decoder.
    snprintf(buf, sizeof(buf), "%.1f ms | %.0f fps | pre %.1f / infer %.1f / post %.1f",
             static_cast<double>(st.last.totalMs()), st.fps,
             static_cast<double>(st.last.preMs), static_cast<double>(st.last.inferMs),
             static_cast<double>(st.last.postMs));
    return buf;
}

std::string describe() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_describe;
}

std::string statusLine() {
    const Session::Stats st = session().stats();
    // Read outside our lock: the engine's status is the session's business and
    // the worker writes it while we are here.
    const std::string err = session().lastError();

    std::lock_guard<std::mutex> lock(g_mutex);
    char buf[512];
    // The backend name comes off the session's own stats rather than off
    // g_snapshot: that field is filled in by snapshot(), which this function
    // does not call, so reading it here reported "-" for a pipeline that was
    // running perfectly well.
    const char* backend = !st.activeName.empty() ? st.activeName.c_str()
                         : (g_snapshot.backend.empty() ? "-" : g_snapshot.backend.c_str());
    // submitted and the capture rate are in here because "the detector ran four
    // times in twelve seconds" has two very different causes — the engine being
    // slow, or the pump not delivering frames — and the counters are the only
    // thing that tells them apart.
    snprintf(buf, sizeof(buf),
             "active=%d backend=%s boxes=%zu ran=%llu submitted=%llu dropped=%llu "
             "failed=%llu invalid=%llu frames=%d/s last=%.1f/%.1f/%.1f ms fps=%.1f%s%s%s%s",
             g_snapshot.active ? 1 : 0, backend,
             g_snapshot.detections.size(),
             (unsigned long long)st.ran, (unsigned long long)st.submitted,
             (unsigned long long)st.dropped, (unsigned long long)st.failed,
             (unsigned long long)st.invalid, capture::frameRate(),
             static_cast<double>(st.last.preMs), static_cast<double>(st.last.inferMs),
             static_cast<double>(st.last.postMs), st.fps,
             g_snapshot.status.empty() ? "" : " status=",
             g_snapshot.status.empty() ? "" : g_snapshot.status.c_str(),
             err.empty() ? "" : " lastError=", err.c_str());
    return buf;
}

}  // namespace runtime
}  // namespace infer
}  // namespace aimbotng
