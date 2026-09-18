// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng input — JNI surface for the shell daemon
//
//  The privileged half of the input stack. It is loaded by ShellServerEntry,
//  which runs as `app_process` under the shell UID (2000, member of the
//  `input` and `uhid` groups) and therefore may read /dev/input and open
//  /dev/uinput. The app process cannot, so it never touches these functions —
//  it drives the daemon over the ADB shell stream instead.
//
//  Bound to io.github.xiangsu1145.aimbotnextgen.shell.ShellNative.
// ─────────────────────────────────────────────────────────────────────────────
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <unistd.h>
#include <sys/types.h>

#include <cstdio>
#include <string>
#include <vector>

#include "capture/capture.h"
#include "config/config_manager.h"
#include "inference/engine.h"
#include "inference/libpath.h"
#include "inference/model_runtime.h"
#include "inference/model_store.h"
#if AIMBOTNG_HAVE_NEURON
#include "inference/neuron_engine.h"
#endif
#include "input/exit_request.h"
#include "input/inject_backend.h"
#include "input/skip_screenshot.h"
#include "touch_reader.h"
#include "uinput_inject.h"
#include "ui/gui/aimbot_ui.h"
#include "ui/gui/sections/model_section.h"
#include "ui/gui/sections/touch_section.h"

#define PREFIX(name) Java_io_github_xiangsu1145_aimbotnextgen_shell_ShellNative_##name

extern "C" {

// ── Physical panel reader ────────────────────────────────────────────────────

JNIEXPORT jboolean JNICALL
PREFIX(readerInit)(JNIEnv*, jobject, jint w, jint h, jint rotation) {
    return reader_init(w, h, rotation) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
PREFIX(readerClose)(JNIEnv*, jobject) {
    reader_close();
}

JNIEXPORT jboolean JNICALL
PREFIX(readerIsReady)(JNIEnv*, jobject) {
    return reader_is_ready() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
PREFIX(readerSetScreenParams)(JNIEnv*, jobject, jint w, jint h, jint rotation) {
    reader_set_screen_params(w, h, rotation);
}

JNIEXPORT jboolean JNICALL
PREFIX(readerGrab)(JNIEnv*, jobject) {
    return reader_grab() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
PREFIX(readerUngrab)(JNIEnv*, jobject) {
    reader_ungrab();
}

JNIEXPORT jboolean JNICALL
PREFIX(readerIsGrabbed)(JNIEnv*, jobject) {
    return reader_is_grabbed() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
PREFIX(readerSetSink)(JNIEnv*, jobject, jboolean enabled) {
    reader_set_sink(enabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
PREFIX(readerGetSink)(JNIEnv*, jobject) {
    return reader_get_sink() ? JNI_TRUE : JNI_FALSE;
}

/// Flat [x, y, w, h, ...] list of the rectangles that swallow touches; an empty
/// array clears them (full pass-through).
JNIEXPORT void JNICALL
PREFIX(readerSetRegions)(JNIEnv* env, jobject, jintArray rects) {
    if (!rects) {
        reader_set_regions(nullptr, 0);
        return;
    }
    jsize length = env->GetArrayLength(rects);
    const jsize capacity = READER_MAX_REGIONS * 4;
    if (length > capacity) length = capacity;
    if (length <= 0) {
        reader_set_regions(nullptr, 0);
        return;
    }
    jint buffer[READER_MAX_REGIONS * 4];
    env->GetIntArrayRegion(rects, 0, length, buffer);
    reader_set_regions(buffer, length / 4);
}

JNIEXPORT jint JNICALL
PREFIX(readerGetRegionCount)(JNIEnv*, jobject) {
    return reader_get_region_count();
}

/// >0 pointers changed, 0 timeout, <0 error.
JNIEXPORT jint JNICALL
PREFIX(readerPoll)(JNIEnv*, jobject, jint timeoutMs) {
    return reader_poll(timeoutMs);
}

/// Returns int[n*3]: [id0, x0, y0, id1, x1, y1, ...] in screen pixels.
JNIEXPORT jintArray JNICALL
PREFIX(readerReadPointers)(JNIEnv* env, jobject) {
    ReaderPointer ptrs[READER_MAX_POINTERS];
    int count = reader_read_pointers(ptrs, READER_MAX_POINTERS);

    jintArray result = env->NewIntArray(count * 3);
    if (!result) return nullptr;

    jint buf[READER_MAX_POINTERS * 3];
    for (int i = 0; i < count; ++i) {
        buf[i * 3 + 0] = ptrs[i].id;
        buf[i * 3 + 1] = static_cast<jint>(ptrs[i].x);
        buf[i * 3 + 2] = static_cast<jint>(ptrs[i].y);
    }
    if (count > 0) env->SetIntArrayRegion(result, 0, count * 3, buf);
    return result;
}

JNIEXPORT jint JNICALL
PREFIX(readerGetMaxX)(JNIEnv*, jobject) { return reader_get_max_x(); }

JNIEXPORT jint JNICALL
PREFIX(readerGetMaxY)(JNIEnv*, jobject) { return reader_get_max_y(); }

JNIEXPORT jstring JNICALL
PREFIX(readerGetPanelPath)(JNIEnv* env, jobject) {
    return env->NewStringUTF(reader_get_panel_path());
}

// ── Virtual touchscreen (uinput) ─────────────────────────────────────────────

JNIEXPORT void JNICALL
PREFIX(uinputSetSourcePanel)(JNIEnv* env, jobject, jstring path) {
    if (!path) {
        uinput_set_source_panel(nullptr);
        return;
    }
    const char* c = env->GetStringUTFChars(path, nullptr);
    uinput_set_source_panel(c);
    env->ReleaseStringUTFChars(path, c);
}

JNIEXPORT jboolean JNICALL
PREFIX(uinputInit)(JNIEnv*, jobject, jint w, jint h) {
    return uinput_init(w, h) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
PREFIX(uinputClose)(JNIEnv*, jobject) {
    uinput_close();
}

JNIEXPORT jboolean JNICALL
PREFIX(uinputIsReady)(JNIEnv*, jobject) {
    return uinput_is_ready() ? JNI_TRUE : JNI_FALSE;
}

/// Exposed only so the daemon's periodic status line can print it next to the
/// grab and layer state — see ShellServerEntry.startStatusTicker().
JNIEXPORT jint JNICALL
PREFIX(uinputWriteFailures)(JNIEnv*, jobject) {
    return uinput_write_failures();
}

// ── Injection backend (uinput / InputManager) ────────────────────────────────
//
// Selection is made in the menu (Touch page → 触摸方式) and persisted, so the
// pick reaches the daemon in two ways: live, by the sync layer calling
// inject_set_backend() straight from the render thread, and at startup, by
// configLoad() below pushing the saved value before anything is opened.
//
// Switching is destructive — see input/inject_backend.h — so the return value
// matters: 0 means "selected, but not usable", which the daemon logs rather than
// hides.

JNIEXPORT jboolean JNICALL
PREFIX(injectSetBackend)(JNIEnv*, jobject, jint backend) {
    return inject_set_backend(backend) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
PREFIX(injectGetBackend)(JNIEnv*, jobject) {
    return inject_get_backend();
}

/// True when the *selected* backend can deliver a frame. For InputManager that
/// is the Kotlin side's own answer, asked for here rather than cached: it can
/// change (a SecurityException invalidates it) and this call is rare — once per
/// OPEN, not once per frame.
JNIEXPORT jboolean JNICALL
PREFIX(injectIsReady)(JNIEnv*, jobject) {
    if (inject_get_backend() != INJECT_BACKEND_INPUT_MANAGER) {
        return inject_is_ready() ? JNI_TRUE : JNI_FALSE;
    }
    return aimbotng::input::imBridgeIsReady() ? JNI_TRUE : JNI_FALSE;
}

/// Why the injection path is not working, in one line, for the log. Empty when
/// it is working.
JNIEXPORT jstring JNICALL
PREFIX(injectLastError)(JNIEnv* env, jobject) {
    const char* err = aimbotng::input::imBridgeLastError();
    return env->NewStringUTF(err != nullptr ? err : "");
}

/// Reads config.json and applies the parts of it that have to be in force
/// before the first OPEN — currently just the injection backend.
///
/// ui::start() already loads the config, but it runs when the *menu* opens,
/// which is after the daemon has already talked to the app and possibly taken
/// the panel. The saved backend decides whether a virtual device is created at
/// all, so it cannot wait for the menu.
JNIEXPORT jint JNICALL
PREFIX(configLoad)(JNIEnv*, jobject) {
    aimbotng::config::load();
    const int saved = aimbotng::ui::sections::g_pageTouch.injectBackend.value;
    return inject_set_backend(saved);
}

JNIEXPORT void JNICALL
PREFIX(uinputSetScreenParams)(JNIEnv*, jobject, jint w, jint h, jboolean landscape) {
    uinput_set_screen_params(w, h, landscape == JNI_TRUE);
}

JNIEXPORT void JNICALL
PREFIX(uinputDown)(JNIEnv*, jobject, jint slot, jint id, jint x, jint y) {
    uinput_down(slot, id, x, y);
}

JNIEXPORT void JNICALL
PREFIX(uinputMove)(JNIEnv*, jobject, jint slot, jint x, jint y) {
    uinput_move(slot, x, y);
}

JNIEXPORT void JNICALL
PREFIX(uinputUp)(JNIEnv*, jobject, jint slot) {
    uinput_up(slot);
}

JNIEXPORT void JNICALL
PREFIX(uinputMirrorClear)(JNIEnv*, jobject) {
    uinput_mirror_clear();
}

// ── Renderer, hosted by the daemon ───────────────────────────────────────────
//
// This is the whole point of running the UI here instead of in the app: the
// layer the menu draws into is built by ShellLayerHost straight on
// SurfaceFlinger (no WindowManager), and that is only possible — and only
// meaningful — from the shell process. Owning the layer from UID 2000 is what
// lets it carry setSkipScreenshot (system-only, silently ignored for a normal
// app) and what makes SYSTEM_ALERT_WINDOW, the platform's 0.8 alpha cap and
// "a full-screen overlay swallows every touch" all irrelevant.
//
// Consequence worth knowing: the renderer, the touch reader and the uinput
// mirror all live in ONE process now, so touches go straight into ImGui and the
// swallow-rectangles are read straight back out — no line protocol hop.

/// Binds the renderer to a layer's Surface and starts the render thread.
JNIEXPORT jboolean JNICALL
PREFIX(uiStart)(JNIEnv* env, jobject, jobject surface) {
    if (!surface) return JNI_FALSE;
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return JNI_FALSE;
    aimbotng::ui::setWindow(window);  // takes ownership of the reference
    return aimbotng::ui::start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
PREFIX(uiStop)(JNIEnv*, jobject) {
    aimbotng::ui::stop();
}

JNIEXPORT jboolean JNICALL
PREFIX(uiIsRunning)(JNIEnv*, jobject) {
    return aimbotng::ui::isRunning() ? JNI_TRUE : JNI_FALSE;
}

/// [width, height] of the surface buffer the renderer draws into, or [0, 0].
/// The layer host re-frames the layer onto this after binding a Surface, so the
/// crop / display frame can never disagree with the buffer — see
/// aimbotng::ui::windowSize.
JNIEXPORT jintArray JNICALL
PREFIX(uiWindowSize)(JNIEnv* env, jobject) {
    int w = 0;
    int h = 0;
    aimbotng::ui::windowSize(&w, &h);
    jintArray result = env->NewIntArray(2);
    if (!result) return nullptr;
    const jint values[2] = {w, h};
    env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

/// Feeds one physical finger straight into ImGui. Same action codes as
/// AimbotNg.TOUCH_* (0 down / 1 move / 2 up / 3 cancel), screen pixels.
JNIEXPORT void JNICALL
PREFIX(uiTouch)(JNIEnv*, jobject, jint action, jfloat x, jfloat y) {
    aimbotng::ui::onTouch(action, x, y);
}

/// Flat [x, y, w, h, ...] rectangles of the live ImGui windows, which the daemon
/// withholds from the uinput mirror. Empty array = everything passes through.
JNIEXPORT jintArray JNICALL
PREFIX(uiRegions)(JNIEnv* env, jobject) {
    constexpr int kMaxRects = 8;
    int rects[kMaxRects * 4];
    const int count = aimbotng::ui::getInteractiveRegions(rects, kMaxRects);
    jintArray result = env->NewIntArray(count > 0 ? count * 4 : 0);
    if (!result) return nullptr;
    if (count > 0) env->SetIntArrayRegion(result, 0, count * 4, rects);
    return result;
}

// ── Capture ──────────────────────────────────────────────────────────────────
//
// The frame producer lives on the Java side (a virtual display feeding an
// ImageReader — see ScreenCapture.kt), because building one needs the
// framework's DisplayManager and the daemon has no Context. What crosses here
// is only the plane's address: the Image owns that buffer and recycles it as
// soon as it is closed, so the copy into native happens while the caller is
// still holding it.

/// Hands one RGBA frame over. `buffer` is the ImageReader plane's direct
/// ByteBuffer; `rowStride` is in bytes, and may exceed `srcWidth * 4`. The
/// buffer holds the whole screen — the square the menu asked for is cut out of
/// it natively (see capture::pushFrame).
JNIEXPORT void JNICALL
PREFIX(capturePushFrame)(JNIEnv* env, jobject, jobject buffer,
                         jint srcWidth, jint srcHeight, jint rowStride,
                         jint cropX, jint cropY, jint side) {
    if (buffer == nullptr) return;
    void* pixels = env->GetDirectBufferAddress(buffer);
    if (pixels == nullptr) return;  // not a direct buffer — nothing we can read
    aimbotng::capture::pushFrame(pixels, srcWidth, srcHeight, rowStride,
                                 cropX, cropY, side);
}

/// Whether the menu wants frames right now (its capture switch).
JNIEXPORT jboolean JNICALL
PREFIX(captureWanted)(JNIEnv*, jobject) {
    return aimbotng::capture::enabled() ? JNI_TRUE : JNI_FALSE;
}

/// Whether the menu is actually sampling them (its capture page is open).
///
/// The switch and the reader are different questions, and only the second one
/// is worth building a mirror for. The switch stays on for the whole session,
/// so honouring it alone kept a full-screen 2K virtual display alive — and a
/// Java Image per frame flowing through the pump — during the many minutes
/// when no frame had a reader.
JNIEXPORT jboolean JNICALL
PREFIX(captureConsuming)(JNIEnv*, jobject) {
    return aimbotng::capture::consuming() ? JNI_TRUE : JNI_FALSE;
}

/// Whether anything is reading frames — the half of the supervisor's decision
/// that is not the capture switch.
///
/// The supervisor asks this one alongside `captureWanted()` and holds a mirror
/// open on the AND. The split is what makes the switch honest: were this
/// `captureConsuming()` alone, holding to infer would keep a mirror alive with
/// the switch off, so turning capture off changed nothing — the bug this flag
/// was added to close.
///
/// It counts inference as a reader. The question here is "is anyone sampling
/// pixels", and a running model is — excluding it is what made the detector need
/// the Capture page open, because with that page closed the supervisor saw no
/// reader, stopped the mirror, and left inference with no frame source.
JNIEXPORT jboolean JNICALL
PREFIX(capturePreviewWanted)(JNIEnv*, jobject) {
    return aimbotng::capture::previewWanted() ? JNI_TRUE : JNI_FALSE;
}

/// Side of the square the menu asked for, in pixels.
JNIEXPORT jint JNICALL
PREFIX(captureWantedSize)(JNIEnv*, jobject) {
    return aimbotng::capture::size();
}

/// Reports whether a producer is live, so the preview can tell "off" apart from
/// "on, but nothing has come back yet".
JNIEXPORT void JNICALL
PREFIX(captureSetRunning)(JNIEnv*, jobject, jboolean on) {
    aimbotng::capture::setRunning(on != 0);
}

/// Tells the capture module that the producer is gone and every frame it produced
/// is dead. Called on the same teardown path as captureSetRunning(false), and it
/// has to *precede* it in effect: the frame id is process-lifetime, so without
/// this the next mirror's first frame reads as "new" to a consumer that already
/// saw the previous mirror's last frame, and the detector runs on a picture of a
/// display that no longer exists.
JNIEXPORT void JNICALL
PREFIX(captureInvalidateFrames)(JNIEnv*, jobject) {
    aimbotng::capture::invalidateFrames();
}

// ── Model store ────────────────────────────────────────────────────────
//
// The list the Add-Model page edits. Persisted to /data/local/tmp on every
// mutation, so an `app_process` restart loses nothing.
//
/// Reads the full list as a flat String[] of JSON-like rows. Each row is
/// `id|name|path|kind|engine|inputSize|loaded|cls\x1fcls...|confidence`. The
/// shell daemon → App bridge prefers stringly typed pieces (everything else
/// uses tab-separated text): readable in dumpsys logs and impossible to
/// misparse. Confidence is last because the class list is variable-length —
/// anything added later must go after it too, or old rows shift.
JNIEXPORT jobjectArray JNICALL
PREFIX(modelAll)(JNIEnv* env, jobject) {
    using aimbotng::model::Entry;
    auto entries = aimbotng::model::all();
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(entries.size()), strCls, nullptr);
    for (jsize i = 0; i < static_cast<jsize>(entries.size()); ++i) {
        char line[1100];
        const Entry& e = entries[i];
        char *p = line;
        p += snprintf(p, sizeof(line), "%d\t%s\t%s\t%d\t%d\t%d\t%d\t",
                      e.id, e.name.c_str(), e.path.c_str(),
                      static_cast<int>(e.kind), static_cast<int>(e.engine),
                      e.inputSize, e.loaded ? 1 : 0);
        for (size_t j = 0; j < e.classes.size(); ++j) {
            if (j != 0) *p++ = '\x1f';
            const char* cname = e.classes[j].c_str();
            int n = snprintf(p, sizeof(line) - (p - line), "%s", cname);
            p += n;
        }
        int n = snprintf(p, sizeof(line) - (p - line), "\t%.3f", e.confidence);
        p += n;
        *p = '\0';
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(line));
    }
    return arr;
}

JNIEXPORT jint JNICALL
PREFIX(modelAdd)(JNIEnv* env, jobject,
                jstring jName, jstring jPath, jint kindInt, jint engineInt,
                jint inputSize) {
    using aimbotng::model::Entry;
    using aimbotng::model::Kind;
    using aimbotng::model::Engine;
    Entry e;
    auto copyTo = [env](jstring js, std::string& dst) {
        if (js == nullptr) return;
        const char* c = env->GetStringUTFChars(js, nullptr);
        dst = c ? c : "";
        if (c) env->ReleaseStringUTFChars(js, c);
    };
    copyTo(jName, e.name);
    copyTo(jPath, e.path);
    e.kind = static_cast<Kind>(kindInt);
    e.engine = static_cast<Engine>(engineInt);
    e.inputSize = inputSize;
    return static_cast<jint>(aimbotng::model::add(e));
}

JNIEXPORT jboolean JNICALL
PREFIX(modelRemove)(JNIEnv*, jobject, jint id) {
    return aimbotng::model::remove(id) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
PREFIX(modelToggleLoaded)(JNIEnv*, jobject, jint id) {
    return aimbotng::model::toggleLoaded(id) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
PREFIX(modelLoadedId)(JNIEnv*, jobject) {
    return static_cast<jint>(aimbotng::model::loadedId());
}

JNIEXPORT jboolean JNICALL
PREFIX(modelSetClasses)(JNIEnv* env, jobject, jint id, jobjectArray jClasses) {
    std::vector<std::string> cls;
    if (jClasses != nullptr) {
        const jsize n = env->GetArrayLength(jClasses);
        cls.reserve(n);
        for (jsize i = 0; i < n; ++i) {
            jstring s = static_cast<jstring>(env->GetObjectArrayElement(jClasses, i));
            const char* c = env->GetStringUTFChars(s, nullptr);
            cls.push_back(c ? c : "");
            if (c) env->ReleaseStringUTFChars(s, c);
        }
    }
    return aimbotng::model::setClasses(id, cls) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
PREFIX(modelLoadFromDisk)(JNIEnv*, jobject) {
    aimbotng::model::loadFromDisk();
}

// ── Inference, for scripts ─────────────────────────────────────────────────
//
// The Model page's switch is a control the daemon draws itself, onto a
// SurfaceFlinger layer. Neither `adb shell input` (InputDispatcher) nor the
// daemon's own uinput injection reaches it, and the EVIOCGRAB'd panel path
// needs a finger. So a script that wants to prove the whole inference chain
// works — load a model, see frames arrive, get boxes back — has no way in
// unless one is provided here.
//
// These set the *switch*, not the runtime. The switch stays the single author
// of whether inference runs; a second entry point would let the UI read OFF
// while a model was loaded and running, which is precisely the kind of
// disagreement that is invisible until it matters.

JNIEXPORT void JNICALL
PREFIX(modelSwitchSet)(JNIEnv*, jobject, jboolean on) {
    aimbotng::ui::sections::setModelSwitch(on == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
PREFIX(modelSwitchGet)(JNIEnv*, jobject) {
    return aimbotng::ui::sections::modelSwitch() ? JNI_TRUE : JNI_FALSE;
}

/// Live state of the pipeline: running or not, backend, counters, box count,
/// stage timings. Built on demand — not called from the frame loop.
JNIEXPORT jstring JNICALL
PREFIX(inferStatus)(JNIEnv* env, jobject) {
    const std::string s = aimbotng::infer::runtime::statusLine();
    return env->NewStringUTF(s.c_str());
}

/// What the loaded model bound: tensor shapes, class count, execution provider.
/// Empty-ish until a load has succeeded.
JNIEXPORT jstring JNICALL
PREFIX(inferDescribe)(JNIEnv* env, jobject) {
    const std::string s = aimbotng::infer::runtime::describe();
    return env->NewStringUTF(s.c_str());
}

// (The "requests" JNI surface used to ferry file-picker / IME asks between
//  the menu and the App process. With both the system file picker and the
//  in-Activity IME retired — the menu now draws its own file browser and the
//  model name + class names were dropped from the Add-Model dialog — the
//  whole request bus is gone, and so are the JNI entry points that drove it.)

// ── Daemon preload ──────────────────────────────────────────────────────────
//
// preloadAllDaemonLibraries() walks every (Runtime, Ep) pair the cpp side
// knows about, asks libpath.cpp for the library list, and dlopen()s every
// entry. Absolute-path dlopen bypasses clns-1's SONAME resolution — that
// is the whole point of this dance: the daemon runs in Android's compat
// library namespace (clns-1) and would otherwise fail to resolve SONAMEs
// that only exist in jniLibs (libQnn*.so, libtensorflowlite_jni.so, etc.).
//
// Best-effort: a missing file is logged inside preload() and the gap is
// surfaced later by the QNN delegate itself when it tries to dlopen the
// skel it needs. We do not abort the daemon here — the CPU path can still
// serve the menu even if QNN is broken.
//
// The Kotlin side stays out of the enum business entirely: the cpp side is
// the single source of truth for both the pair table (engine.h) and the
// library list (libpath.cpp).
JNIEXPORT jint JNICALL
PREFIX(preloadAllDaemonLibraries)(JNIEnv*, jobject) {
    // A build marker, before anything else has a chance to fail.
    //
    // "The diagnosis did not run" and "the APK on this device does not contain
    // the diagnosis yet" produce byte-identical logs, and telling them apart
    // cost a full round trip. One line with the compile date settles it. It is
    // logged under the inference tag on purpose, so the same
    // `-s AimbotInfer:V` filter that finds the APU diagnosis finds this too.
    __android_log_print(ANDROID_LOG_INFO, "AimbotInfer",
                        "daemon libs: enter (built %s %s)", __DATE__, __TIME__);

    int total = 0;
    int pairCount = 0;
    const aimbotng::infer::Pair* pairs = aimbotng::infer::allPairs(pairCount);
    for (int i = 0; i < pairCount; ++i) {
        int libCount = 0;
        const char* const* names =
            aimbotng::infer::runtimeLibraries(pairs[i].runtime, pairs[i].ep, libCount);
        if (names == nullptr || libCount <= 0) continue;
        for (int j = 0; j < libCount; ++j) {
            const char* name = names[j];
            if (name == nullptr) continue;
            std::string why;
            (void)aimbotng::infer::preload(name, &why);
            ++total;
        }
    }

    // One APU diagnosis per daemon start, before anything has a chance to ask.
    //
    // It belongs here and not in the menu. The menu's probe is reached from
    // engineRowDisabled(), which only runs when the Model page draws — so a
    // capture taken from any other page, or from a session where the menu was
    // never opened, contains a greyed-out "Neuron (APU)" row and no reason for
    // it anywhere in the log. The probe had not failed; it had never run, and
    // nothing said so. Running it at startup makes every capture answer the
    // question whether or not the UI was touched, and it warms the cached
    // probe result so the Model page never pays for it.
    //
    // Has to be after the preloads above: those are what put /system_ext/lib64
    // and /vendor/lib64 on this process's search path, and the adapter's own
    // by-name lookups depend on it.
    // Named for where it runs, not for what it is: there is a second call at
    // daemon boot (neuronDiagnosis), and two identically-labelled blocks in one
    // capture are worse than none.
#if AIMBOTNG_HAVE_NEURON
    aimbotng::infer::logNeuronDiagnosis("after runtime preload");
#endif

    return total;
}

// ── APU diagnosis on demand ────────────────────────────────────────────────
//
// The same call preloadAllDaemonLibraries() makes, exposed so the daemon boot
// path can make it too. It matters that this is reachable without loading a
// model: a greyed-out "Neuron (APU)" row is exactly the situation in which no
// model can be loaded through that engine, so a diagnosis that only runs on
// model load can never explain the one failure it exists to explain.
JNIEXPORT void JNICALL
PREFIX(neuronDiagnosis)(JNIEnv*, jobject) {
#if AIMBOTNG_HAVE_NEURON
    aimbotng::infer::logNeuronDiagnosis("daemon boot");
#endif
}

// ── Process detach ─────────────────────────────────────────────────────────
//
// Detachment used to be done in native code: `nativeDetach` did `fork()` +
// `setsid()` so the daemon survived an adbd session close. The fork killed
// libbinder (ProcessState aborts in any process that touches binder after a
// fork), so the child could not read the display or build a SurfaceFlinger
// layer, and the post-fork copies had to be hosed down with hand-offs.
//
// Shizuku's design doesn't have any of this. It backgrounds the daemon with
// a plain `(cmd) &` in the shell — adbd only SIGHUPs the foreground shell
// process, not its backgrounded children — and uses no fork at all. The
// daemon runs as a normal long-lived shell-UID process, talks to libbinder
// for everything, and the app talks to the daemon over an abstract Unix
// socket. That is what this project does now, and nativeDetach is gone.

// ── JVM cache + reverse bridge (C++ → Kotlin) ───────────────────────────────
//
// The render thread lives off in its own world — ImGui frames, Vulkan
// commands, nothing in the JVM — but the ImGui Settings page's "Anti
// Screenshot" switch needs to flip a SurfaceControl bit on the Kotlin side
// (the SurfaceControl handle is built and held by ShellLayerHost via
// reflection; the native side has no reference to it).
//
// JNI_OnLoad stashes the JavaVM. applySkipScreenshot() below attaches the
// caller's thread on first use, looks up the ShellLayerHost class + its
// @JvmStatic setSkipScreenshot once, and calls it. Exceptions are caught
// and turned into `false` so a faulty bridge can never bring the render
// thread down — the worst that happens is one toggle that lands flat.

namespace {
JavaVM* g_jvm = nullptr;
jclass   g_skipClass = nullptr;   // io/github/xiangsu1145/aimbotnextgen/shell/ShellLayerHost
jmethodID g_skipMid = nullptr;    // static boolean setSkipScreenshot(boolean)
jclass   g_exitClass = nullptr;   // io/github/xiangsu1145/aimbotnextgen/shell/ShellServerEntry
jmethodID g_exitMid = nullptr;    // static void requestExitFromMenu()
}  // namespace

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

}  // extern "C"

namespace aimbotng {
namespace input {

namespace {
constexpr const char* kLayerHostClass =
    "io/github/xiangsu1145/aimbotnextgen/shell/ShellLayerHost";

/// The daemon's entry point — owns `running`, which is what the menu's 退出 row
/// has to clear. See input/exit_request.h for why that round-trip exists.
constexpr const char* kEntryClass =
    "io/github/xiangsu1145/aimbotnextgen/shell/ShellServerEntry";

// Look up ShellLayerHost.setSkipScreenshot(Z)Z once and cache. Returns false
// (and clears any pending JNI exception) if the bridge is unreachable for
// any reason — the caller treats that as "skip this toggle, keep going".
bool ensureBridge(JNIEnv* env) {
    if (g_skipClass == nullptr) {
        jclass local = env->FindClass(kLayerHostClass);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return false;
        }
        if (local == nullptr) return false;
        g_skipClass = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    if (g_skipMid == nullptr) {
        g_skipMid = env->GetStaticMethodID(g_skipClass, "setSkipScreenshot", "(Z)Z");
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            g_skipMid = nullptr;
            return false;
        }
    }
    return true;
}
}  // namespace

bool applySkipScreenshot(bool on) {
    if (g_jvm == nullptr) {
        // JNI_OnLoad never ran — should not happen on the daemon, but
        // bail safely rather than crash the render thread.
        return false;
    }
    JNIEnv* env = nullptr;
    const jint status = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    bool attached = false;
    if (status == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            return false;
        }
        attached = true;
    } else if (status != JNI_OK || env == nullptr) {
        return false;
    }

    bool ok = false;
    if (ensureBridge(env)) {
        const jboolean result = env->CallStaticBooleanMethod(
            g_skipClass, g_skipMid, on ? JNI_TRUE : JNI_FALSE);
        if (!env->ExceptionCheck()) {
            ok = (result == JNI_TRUE);
        } else {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
    // Intentionally NOT detaching on the render thread: it lives for the
    // daemon's lifetime, and leaving the JVM reference registered is what
    // makes the next toggle free.
    (void)attached;
    return ok;
}

bool requestDaemonExit() {
    // Both outcomes are worth a line. This is the last step the menu can see:
    // if it reports failure, the fault is the bridge (nothing else even tried),
    // and if it reports success but nothing happens afterwards, everything
    // downstream of it — the Kotlin half below — is where the exit died.
    if (g_jvm == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "AimbotInput",
                            "exit: no JavaVM cached — JNI_OnLoad never ran");
        return false;
    }

    JNIEnv* env = nullptr;
    const jint status = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, "AimbotInput",
                                "exit: could not attach the render thread");
            return false;
        }
    } else if (status != JNI_OK || env == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "AimbotInput",
                            "exit: could not get a JNIEnv (status=%d)", status);
        return false;
    }

    if (g_exitClass == nullptr) {
        jclass local = env->FindClass(kEntryClass);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            __android_log_print(ANDROID_LOG_ERROR, "AimbotInput",
                                "exit: %s not reachable from this thread", kEntryClass);
            return false;
        }
        if (local == nullptr) return false;
        g_exitClass = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    if (g_exitMid == nullptr) {
        g_exitMid = env->GetStaticMethodID(g_exitClass, "requestExitFromMenu", "()V");
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            g_exitMid = nullptr;
            __android_log_print(ANDROID_LOG_ERROR, "AimbotInput",
                                "exit: ShellServerEntry.requestExitFromMenu not found");
            return false;
        }
    }

    env->CallStaticVoidMethod(g_exitClass, g_exitMid);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, "AimbotInput",
                        "exit: handed to ShellServerEntry (menu 退出 -> releasing the panel)");
    return true;
}

// ── Reverse bridge: native → InputManagerInjector (Kotlin) ───────────────────
//
// The third user of this JVM handle (after setSkipScreenshot and
// requestExitFromMenu) and the busiest: the InputManager backend calls in once
// per injected frame, and frames come from both the reader's poll thread (the
// physical-finger mirror) and the render thread (aim and trigger). Both are
// long-lived, so each attaches once and stays attached.

namespace {
constexpr const char* kInjectorClass =
    "io/github/xiangsu1145/aimbotnextgen/inject/InputManagerInjector";

constexpr int kImMaxPointers = 10;

jclass    g_imClass      = nullptr;
jmethodID g_imInitMid    = nullptr;   // static boolean init()
jmethodID g_imPushMid    = nullptr;   // static void pushFrame(int[], int[], int[], int)
jmethodID g_imReleaseMid = nullptr;   // static void releaseAll()
jmethodID g_imReadyMid   = nullptr;   // static boolean isReady()
jmethodID g_imErrorMid   = nullptr;   // static String lastError()

// Reused for every frame. Allocating three arrays per frame would be a JNI
// allocation and three copies at 100+ Hz for no benefit: the Kotlin side reads
// them synchronously inside the call, before this returns.
jintArray g_imIds = nullptr;
jintArray g_imXs  = nullptr;
jintArray g_imYs  = nullptr;

/// Last error, copied out of Java so the caller can have a plain `const char*`
/// that outlives the JNI call. Only ever read for a log line.
char g_imLastError[256] = {};
bool g_imEverInitialised = false;

JNIEnv* imEnv() {
    if (g_jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    const jint status = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    } else if (status != JNI_OK) {
        return nullptr;
    }
    return env;
}

/// Resolves the class and its five statics once. Returns false (with any pending
/// exception cleared) when the bridge cannot be built at all.
bool imEnsure(JNIEnv* env) {
    if (g_imClass == nullptr) {
        jclass local = env->FindClass(kInjectorClass);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return false;
        }
        if (local == nullptr) return false;
        g_imClass = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    if (g_imInitMid != nullptr) return true;

    g_imInitMid    = env->GetStaticMethodID(g_imClass, "init", "()Z");
    g_imPushMid    = env->GetStaticMethodID(g_imClass, "pushFrame", "([I[I[II)V");
    g_imReleaseMid = env->GetStaticMethodID(g_imClass, "releaseAll", "()V");
    g_imReadyMid   = env->GetStaticMethodID(g_imClass, "isReady", "()Z");
    g_imErrorMid   = env->GetStaticMethodID(g_imClass, "lastError", "()Ljava/lang/String;");
    if (env->ExceptionCheck() || g_imInitMid == nullptr || g_imPushMid == nullptr ||
        g_imReleaseMid == nullptr || g_imReadyMid == nullptr || g_imErrorMid == nullptr) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        g_imInitMid = g_imPushMid = g_imReleaseMid = g_imReadyMid = g_imErrorMid = nullptr;
        __android_log_print(ANDROID_LOG_ERROR, "AimbotInput",
                            "inject: InputManagerInjector is missing a static method — "
                            "the backend cannot be driven from native");
        return false;
    }
    return true;
}

bool imEnsureArrays(JNIEnv* env) {
    if (g_imIds != nullptr) return true;
    jintArray ids = env->NewIntArray(kImMaxPointers);
    jintArray xs  = env->NewIntArray(kImMaxPointers);
    jintArray ys  = env->NewIntArray(kImMaxPointers);
    if (ids == nullptr || xs == nullptr || ys == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, "AimbotInput",
                            "inject: could not allocate the frame arrays");
        return false;
    }
    g_imIds = static_cast<jintArray>(env->NewGlobalRef(ids));
    g_imXs  = static_cast<jintArray>(env->NewGlobalRef(xs));
    g_imYs  = static_cast<jintArray>(env->NewGlobalRef(ys));
    env->DeleteLocalRef(ids);
    env->DeleteLocalRef(xs);
    env->DeleteLocalRef(ys);
    return true;
}
}  // namespace

bool imBridgeInit() {
    JNIEnv* env = imEnv();
    if (env == nullptr || !imEnsure(env)) return false;

    const jboolean ok = env->CallStaticBooleanMethod(g_imClass, g_imInitMid);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    g_imEverInitialised = true;
    return ok == JNI_TRUE;
}

void imBridgePushFrame(const int* ids, const int* xs, const int* ys, int n) {
    if (n < 0) n = 0;
    if (n > kImMaxPointers) n = kImMaxPointers;

    JNIEnv* env = imEnv();
    if (env == nullptr || !imEnsure(env) || !imEnsureArrays(env)) return;

    if (n > 0) {
        env->SetIntArrayRegion(g_imIds, 0, n, ids);
        env->SetIntArrayRegion(g_imXs, 0, n, xs);
        env->SetIntArrayRegion(g_imYs, 0, n, ys);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return;
        }
    }
    env->CallStaticVoidMethod(g_imClass, g_imPushMid, g_imIds, g_imXs, g_imYs, n);
    if (env->ExceptionCheck()) {
        // Not fatal and not repeated here: the Kotlin side already reports the
        // interesting failures (a refused injection) itself, with the hint that
        // makes them actionable. Throwing out of the reader thread would take
        // the panel's grab with it.
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void imBridgeReleaseAll() {
    JNIEnv* env = imEnv();
    if (env == nullptr || !imEnsure(env)) return;
    if (!g_imEverInitialised) return;   // never set up, so nothing can be down
    env->CallStaticVoidMethod(g_imClass, g_imReleaseMid);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

bool imBridgeIsReady() {
    JNIEnv* env = imEnv();
    if (env == nullptr || !imEnsure(env)) return false;
    const jboolean ok = env->CallStaticBooleanMethod(g_imClass, g_imReadyMid);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    return ok == JNI_TRUE;
}

const char* imBridgeLastError() {
    g_imLastError[0] = '\0';
    JNIEnv* env = imEnv();
    if (env == nullptr || !imEnsure(env)) return g_imLastError;

    jstring text = static_cast<jstring>(env->CallStaticObjectMethod(g_imClass, g_imErrorMid));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return g_imLastError;
    }
    if (text != nullptr) {
        const char* utf = env->GetStringUTFChars(text, nullptr);
        if (utf != nullptr) {
            strncpy(g_imLastError, utf, sizeof(g_imLastError) - 1);
            env->ReleaseStringUTFChars(text, utf);
        }
        env->DeleteLocalRef(text);
    }
    return g_imLastError;
}

}  // namespace input
}  // namespace aimbotng