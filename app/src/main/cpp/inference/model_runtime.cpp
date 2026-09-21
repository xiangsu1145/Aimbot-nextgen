// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer::runtime — implementation of model_runtime.h
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/model_runtime.h"

#include "capture/capture.h"
#include "inference/backend.h"
#include "inference/model_store.h"
#include "inference/session.h"
#include "ui/gui/notify.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace runtime {
namespace {

using Clock = std::chrono::steady_clock;

std::mutex      g_mutex;         // guards everything below
Snapshot        g_snapshot;
std::string     g_describe;
std::atomic<bool> g_wants{false};

/// Lifecycle state of the runtime. Three bits, three responsibilities:
///
///   * `g_loadedId` — which model entry the engine currently holds. The
///     start() fast path and prepare() both use it to skip redundant work;
///     zero means "no model is loaded".
///
///   * `g_engineReady` — the model + backend delegate are initialised. This
///     is what `arm()` and `disarm()` operate on; it does NOT imply the
///     worker is consuming frames. The capture-side gate (`wantsFrames()`)
///     reads `g_wants`, not this.
///
///   * `g_pendingArm` — arm() was called before prepare() finished. When
///     prepare() succeeds it honours the intent, so the "user flipped the
///     switch on the same frame Load returned" race does not silently lose
///     the first inference.
int             g_loadedId    = 0;
bool            g_engineReady = false;
bool            g_pendingArm  = false;

/// True while prepareAsync()'s worker is inside prepare(). Guards against a
/// second spawn when the held-finger path calls it on every frame the finger
/// stays down: without it, a 20-second APU compile would be started twenty
/// times a second, each one racing the others over the session's engine slot.
std::atomic<bool> g_preparing{false};

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
/// copies into a caller-owned buffer.
std::vector<uint8_t> g_frameBuf;
uint64_t             g_lastFrameId = 0;

/// The frame geometry the session's Roi was last set from.
///
/// Tracked so setRoi() is called when the size actually changes rather than on
/// every pump(): setRoi takes the session's config lock, and taking a lock at
/// screen rate to write an unchanged value is the kind of thing that shows up
/// as lock contention long after anyone remembers writing it.
bool g_roiSynced = false;
int  g_roiW = 0;
int  g_roiH = 0;

/// Resolves the pair an entry should run on.
///
/// The store's Engine enum is the *user's* vocabulary and is deliberately small;
/// the pair is the implementation's. This is the one place the two meet, so the
/// mapping is written out rather than derived — a table that says what it means
/// is worth more than a function that computes something plausible.
bool resolvePair(model::Engine engine, Runtime& outRuntime, Ep& outEp,
                 std::string& why) {
    LOGD("resolvePair: in engine=%d (%s)", static_cast<int>(engine),
         model::engineLabel(engine));
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
        case model::Engine::LiteRtNpu:
            // LiteRT 2.x vendor dispatch is disabled in this branch (it
            // crashes on the current MediaTek SKU and is not being fixed).
            // Drop to the plain LiteRT CPU backend so the model still loads.
            // resolvePair() is the one place this swap is meaningful; the menu
            // no longer offers this engine (see model_store.cpp).
            LOGW("resolvePair: Engine::LiteRtNpu → fallback to LiteRT/CPU "
                 "(LiteRT 2.x vendor dispatch disabled in this branch)");
            outRuntime = Runtime::LiteRT;
            outEp      = Ep::Cpu;
            return true;
#if AIMBOTNG_HAVE_NEUROPILOT
        case model::Engine::NeuroPilot:
            outRuntime = Runtime::LiteRT;
            // Nor this: MediaTek's own TFLite fork, dlopened from
            // /system_ext/lib64 by NeuroPilotEngine. Third runtime, same
            // .tflite file — see neuropilot_engine.h.
            outEp = Ep::NeuroPilot;
            return true;
#endif
        // Temporarily commented out the AIMBOTNG_HAVE_NEURON gate so Neuron
        // is reachable from resolvePair() unconditionally, matching the
        // backup build.
        // #if AIMBOTNG_HAVE_NEURON
        case model::Engine::Neuron:
            outRuntime = Runtime::LiteRT;
            // Not NeuroPilot either: no MediaTek TFLite fork. This is Google's
            // TFLite (libtensorflowlite_jni.so, the same library the CPU path
            // uses) with our own delegate moving supported subgraphs onto the
            // APU through the Neuron adapter. Same .tflite file — see
            // neuron_engine.h.
            LOGD("resolvePair: Engine::Neuron → (LiteRT, Neuron)");
            outEp = Ep::Neuron;
            return true;
        // #endif
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
    g_snapshot.detSeq++;   // same lock as the list it describes — see the note in the header
    g_snapshot.timing = timing;
    g_snapshot.boxesIn = roi;
}

}  // namespace

// ── Availability ─────────────────────────────────────────────────────────────

bool engineAvailable(model::Engine engine) {
    Runtime rt = Runtime::Unknown;
    Ep ep = Ep::Default;
    std::string why;
    if (!resolvePair(engine, rt, ep, why)) return false;
    return epAvailable(rt, ep);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

Status prepare() {
    const int id = model::loadedId();
    if (id == 0) return Status::bad("no model is marked as loaded");

    // Fast path: same model already prepared. A per-frame caller of arm()
    // therefore cannot trigger a graph recompile, and the Settings page's
    // triggered-mode loop — which arms on every edge — stays cheap.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_engineReady && g_loadedId == id) return Status::good();
        // Reset state before the (potentially long) load. A concurrent arm()
        // sets g_pendingArm, which we honour on success below.
        g_engineReady = false;
        g_pendingArm  = false;
    }

    // The store is read outside the lock: model::all() takes its own, and
    // holding both would put the menu's list mutations behind an inference
    // status check for no reason.
    model::Entry entry;
    bool found = false;
    for (const model::Entry& e : model::all()) {
        if (e.id == id) { entry = e; found = true; break; }
    }
    if (!found) return Status::bad("the loaded model is no longer in the list");

    LOGI("prepare: model #%d '%s' stored engine=%d (%s)",
         entry.id, entry.name.c_str(),
         static_cast<int>(entry.engine), model::engineLabel(entry.engine));

    Runtime rt = Runtime::Unknown;
    Ep ep = Ep::Default;
    std::string why;
    if (!resolvePair(entry.engine, rt, ep, why)) {
        LOGW("cannot prepare '%s': %s", entry.name.c_str(), why.c_str());
        return Status::bad(why);
    }

    // Registered before the load, not after: the worker starts producing the
    // moment the engine is installed, and a callback attached afterwards would
    // miss the first frame or two for no reason.
    session().onResult(onResult);

    // ── Progress reporting ──────────────────────────────────────────────────
    // The bar is deliberately coarse and deliberately stops short of 100%.
    // There are two distinct halves to a load and only one of them is knowable
    // from here:
    //
    //   * what this process does — resolve the backend, open the model file,
    //     create the delegate, allocate tensors. Those are discrete steps and
    //     each one can be marked as it is reached.
    //   * what the vendor compiler does — inside AllocateTensors for the APU,
    //     inside the QNN/HTP compile for that backend. It is a single opaque
    //     call that can take twenty seconds, and there is no callback into it.
    //
    // So the discrete steps carry the bar to 80% and the last 20% is the
    // vendor's. That is not a decoration: a bar that estimates its way to 95%
    // and then sits there for twenty seconds reads as a hang, while a bar that
    // stops at four fifths and waits reads as "the slow part is happening".
    const char* kTag = ui::notify::kCompileTag;
    ui::notify::publishProgress(kTag, 0.30f);
    LOGI("prepare: backend resolved to %s, opening model", pairLabel(rt, ep));

    const Status s = session().load(toSpec(entry), rt, ep);
    ui::notify::publishProgress(kTag, 0.80f);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_snapshot.detections.clear();
    g_snapshot.detSeq++;   // the list changed, so consumers must re-read it
    g_snapshot.timing = Timing{};
    g_snapshot.boxesIn = Roi{};
    g_snapshot.classes = entry.classes;
    if (s.ok) {
        g_snapshot.status.clear();
        g_engineReady       = true;
        g_loadedId          = id;
        // g_snapshot.active is left as-is on purpose: prepare() must not flip
        // it on, because the only thing that decides whether UI / aim / trigger
        // are allowed to read detections is whether the worker is *consuming*
        // frames. arm() owns that bit; if a caller armed before prepare()
        // returned, g_pendingArm will be honoured below and the active bit
        // will be set there. If not, prepare() leaves the snapshot in a quiet
        // state, so the moment the caller arms, the transition is visible.
        g_snapshot.active   = g_wants.load(std::memory_order_relaxed);
        // The engine's own account of what it bound: tensor shapes, class count,
        // which provider answered. Captured once here because it describes the
        // model rather than the run, and because a bug report needs it and a
        // per-frame path should not be building strings.
        g_describe = session().describe();
        // Publish the detected input dtype back to the store so the model list
        // can show fp16 / fp32 / int8 for an entry after it has been loaded once.
        model::setType(entry.id, session().inputDtypeName());
        // If arm() was called while we were compiling the graph (typical when
        // the user flips the switch on the same frame Load returns), honour
        // the intent now so the first inference is not silently lost.
        if (g_pendingArm) {
            g_wants.store(true, std::memory_order_relaxed);
            g_snapshot.active = true;
            g_pendingArm = false;
        }
        LOGI("inference prepared on %s: %s", pairLabel(rt, ep), g_describe.c_str());
    } else {
        g_snapshot.active = false;
        g_snapshot.status = s.message;
        g_loadedId        = 0;
        g_describe        = "failed: " + s.message;
    }
    return s;
}

void arm() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_engineReady) {
        // Engine not ready yet — arm() is racing prepare() (typical when the
        // user flips the switch on the same frame Load returns). Record the
        // intent so prepare() can apply it on completion.
        g_pendingArm = true;
        return;
    }
    if (g_wants.load(std::memory_order_relaxed)) return;   // already armed
    g_wants.store(true, std::memory_order_relaxed);
    // The "active" bit is the worker's view, not the engine's: as soon as it
    // is consuming frames the snapshot is live, and the UI / aim / trigger
    // paths are allowed to read it.
    //
    // Detections are cleared here, and the old comment claiming they need not
    // be was wrong in a way the user could see. "The worker has not produced
    // anything yet on a fresh arm" is true of the *worker*, but the snapshot is
    // read by the renderer from the moment `active` goes true, and `active` goes
    // true right here — one frame of "armed but nothing computed yet" during
    // which the previous cycle's boxes were still in the vector. Press, release,
    // press, and the second press opens on the first press's results: boxes in
    // the wrong place, easing out as if they were fresh, which is exactly the
    // "old frame then a new one a moment later" the menu was showing.
    //
    // It bites hardest on the hold-to-infer path, where the arm/disarm cycle is
    // as fast as the finger. The clear is a vector::clear() on a payload that is
    // about to be overwritten anyway.
    g_snapshot.active = true;
    g_snapshot.detections.clear();
    // Bump the sequence: the list just changed (emptied). Without this a
    // consumer keyed on freshness sees the same sequence as the previous
    // session and keeps its stale state — which is the "boxes in the wrong
    // place, easing out as if they were fresh" failure this clear exists to
    // prevent.
    g_snapshot.detSeq++;
    g_snapshot.status.clear();
    LOGI("inference armed");
}

void disarm() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pendingArm = false;
    if (!g_wants.load(std::memory_order_relaxed) && !g_snapshot.active) return;  // already disarmed
    g_wants.store(false, std::memory_order_relaxed);
    // The "active" bit goes off with the worker: the moment the user releases
    // the trigger area, the UI must stop drawing the last frame's boxes —
    // they belong to a frame whose result is no longer being refreshed, so
    // leaving them on screen looks like a stuck/lagging detector. Clearing
    // the vector is cheap and removes the ambiguity entirely.
    g_snapshot.active = false;
    g_snapshot.detections.clear();
    g_snapshot.status.clear();
    LOGI("inference disarmed");
}

Status start() {
    Status s = prepare();
    if (s.ok) arm();
    return s;
}

void stop() {
    // disarm() before unload() so the worker cannot submit a frame to an
    // engine that is being torn down. Both are idempotent.
    g_wants.store(false, std::memory_order_relaxed);
    session().unload();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_snapshot    = Snapshot{};
    g_describe    = "stopped";
    g_loadedId    = 0;
    g_engineReady = false;
    g_pendingArm  = false;
    LOGI("inference stopped");
}

bool running() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snapshot.active;
}

bool engineReady() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_engineReady;
}

void prepareAsync() {
    // One compile at a time, and no compile at all if one has already landed.
    // The caller is typically a per-frame edge handler, so this is the hot
    // guard — a compare-exchange, not a lock, because the failing case (already
    // preparing) must be free.
    if (g_engineReady) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_engineReady && g_loadedId == model::loadedId()) return;
    }
    bool expected = false;
    if (!g_preparing.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel)) {
        return;   // already compiling
    }

    const int wantId = model::loadedId();
    LOGI("async prepare requested for model #%d", wantId);

    // Detached on purpose. The thread owns nothing the caller needs and the
    // caller is a frame handler that must not join it; the result is published
    // through the snapshot like every other load, and a failure is reported
    // through the toast below. The alternative — a persistent worker joined at
    // shutdown — buys ordering guarantees this path does not need, because
    // prepare() itself is internally serialised by the session.
    std::thread([wantId]() {
        // Shared with the Model page's own compile path so the two coalesce
        // into a single toast: whichever one the user triggers, and whichever
        // one finishes, there is one bar on screen.
        const char* kTag = ui::notify::kCompileTag;
        ui::notify::post(kTag, ui::notify::Kind::Info, "模型编译中  正在准备引擎",
                         0.0f /* no auto-expiry: ended explicitly below */,
                         0.05f);

        // Progress is faked in four coarse jumps, one per phase of prepare().
        // It is not a lie told for effect: the phases are real (resolve the
        // backend, load the graph, build the delegate, allocate tensors) and
        // the only part whose duration is unknowable from here is the vendor
        // compiler's, which is what the last 20% is reserved for.
        ui::notify::publishProgress(kTag, 0.20f);

        const auto t0 = Clock::now();
        const Status s = prepare();
        const double ms =
            std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        g_preparing.store(false, std::memory_order_release);

        if (s.ok) {
            LOGI("async prepare done in %.0fms", ms);
            ui::notify::publishProgress(kTag, 1.0f);
            ui::notify::post(kTag, ui::notify::Kind::Success, "模型编译完成", 2.5f, 1.0f);
            // Honor an arm() that arrived while the graph was being built. The
            // user's finger is very likely still down — that is the whole point
            // of this path — so the first inference should land immediately
            // rather than on the next press. prepare() itself applies
            // `g_pendingArm`; this covers the case where the caller armed
            // between prepare() returning and this line running.
            if (g_pendingArm) arm();
        } else {
            LOGW("async prepare failed: %s", s.cstr());
            ui::notify::post(kTag, ui::notify::Kind::Error,
                             ("模型编译失败: " + s.message).c_str(), 8.0f, -1.0f);
            ui::notify::publishProgress(kTag, -1.0f);
        }
    }).detach();
}

bool wantsFrames() { return g_wants.load(std::memory_order_relaxed); }

void invalidateEngine() {
    // The session's engine is torn down here rather than inside prepare() so the
    // cost is paid on the UI thread at the moment of the change (a few ms —
    // unloading a delegate) and not folded into the compile the user is about to
    // wait on, where it would be indistinguishable from the vendor compiler
    // being slow.
    //
    // Not disarm() first: disarm() takes g_mutex, and so does this. Order the
    // writes directly instead — g_wants going false is what stops the pump on
    // the next frame, and session().unload() is already safe to call while the
    // worker is between frames (the Session serialises it, see session.cpp).
    g_wants.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_engineReady = false;
        g_pendingArm  = false;
        g_snapshot.active = false;
        g_snapshot.detections.clear();
        g_snapshot.status.clear();
        // g_loadedId is left alone on purpose. The model is still the one the
        // store marks as loaded — only its *engine* is stale. Clearing it would
        // make the next prepare() report "no model is marked as loaded", which
        // is a lie the user would have to debug through the toast.
    }

    session().unload();
    LOGI("engine invalidated — the next prepare() will rebuild it");
}

// ── The frame pump ──────────────────────────────────────────────────────────

void pump() {
    if (!g_wants.load(std::memory_order_relaxed)) return;

    int w = 0, h = 0;
    if (!capture::peekSize(w, h)) return;
    if (w <= 0 || h <= 0) return;

    // Tell the engine what the crop is. The frames arrive already cropped to a
    // centred square by the capture side, and the Roi is how the preprocessor
    // learns that — without it the crop is treated as the whole frame and the
    // letterbox scales it a second time to the tensor size, which is both a
    // wasted resample and an aspect distortion.
    //
    // Set from here rather than from the menu because this is the one place
    // that already knows both halves: the size the capture side actually
    // produced (w, h, which is authoritative even if a slider is mid-drag) and
    // the session that will consume it. A menu-side setter would have to
    // reconstruct the origin the same way and could disagree with the frame.
    //
    // Empty Roi means "whole frame", which is exactly right here — the crop is
    // the frame as far as inference is concerned, and the origin is added back
    // in onResult() when the boxes are mapped to screen coordinates. Cheaper
    // than tracking the origin twice and far harder to get inconsistent.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_roiSynced || g_roiW != w || g_roiH != h) {
            session().setRoi(Roi::whole());
            g_roiW = w;
            g_roiH = h;
            g_roiSynced = true;
            LOGI("frame geometry changed to %dx%d, roi reset to whole frame", w, h);
        }
    }

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
