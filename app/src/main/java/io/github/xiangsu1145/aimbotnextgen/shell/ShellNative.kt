package io.github.xiangsu1145.aimbotnextgen.shell

import android.view.Surface

/**
 * Native input surface used ONLY by the shell daemon (see [ShellServerEntry]).
 *
 * These entry points live in libaimbotng.so but are never called from the app
 * process: everything here needs either /dev/input (read + EVIOCGRAB) or
 * /dev/uinput (write), and the app UID is allowed neither. The daemon runs as
 * `app_process` under shell UID 2000 — a member of the `input` and `uhid`
 * groups — which is exactly the permission set the previous project's Shizuku
 * user service had.
 *
 * Native side: cpp/input/touch_reader.* (panel), cpp/input/uinput_inject.*
 * (virtual device), cpp/input/input_jni.cpp (this surface).
 */
object ShellNative {

    /**
     * Loads the shared library from an absolute path.
     *
     * The daemon has no APK context, so the library must exist on disk — see
     * `packaging { jniLibs { useLegacyPackaging = true } }` in build.gradle.kts,
     * which makes the installer extract it into `nativeLibraryDir`. The library
     * links its C++ runtime statically, so this needs no LD_LIBRARY_PATH.
     */
    fun load(libraryPath: String) {
        System.load(libraryPath)
    }

    /**
     * Loads every .so the inference layer expects for every (Runtime, Ep)
     * pair that ships in this build.
     *
     * Calls into the cpp side's [allPairs()] + [runtimeLibraries()] and
     * dlopen()s each entry from the APK's lib dir by absolute path. Needed
     * because the daemon runs in Android's compat library namespace (clns-1),
     * which cannot resolve SONAMEs that only live in jniLibs; absolute-path
     * dlopen registers the SONAME on clns-1 so subsequent DT_NEEDED
     * resolution works.
     *
     * Best-effort: missing files are logged inside native, not surfaced as
     * errors. The caller can use [inferDescribe] after a model load to see
     * what bound and what didn't. Returns the count of dlopen() calls made
     * (already-resident libraries are still counted).
     */
    external fun preloadAllDaemonLibraries(): Int

    /**
     * Prints the whole MediaTek-APU picture, once, whatever the UI is doing.
     *
     * Called at daemon boot rather than only from the model-load path. The
     * model-load path is too late to diagnose with: the "Neuron (APU)" row can
     * only be picked if it is not greyed out, so on a device where the APU is
     * unreachable the user never gets far enough to load a model — and the
     * capture therefore contains a greyed-out row and nothing explaining it.
     *
     * Cheap and idempotent: the underlying probes are cached, so calling this
     * early costs one round of dlopens that the first real use would have paid
     * for anyway.
     */
    external fun neuronDiagnosis()

    // ── Physical panel (exclusive) ───────────────────────────────────────────

    /** Opens + decodes the real touch panel. `rotation` is 0..3. */
    external fun readerInit(width: Int, height: Int, rotation: Int): Boolean
    external fun readerClose()
    external fun readerIsReady(): Boolean
    external fun readerSetScreenParams(width: Int, height: Int, rotation: Int)

    /** Takes the panel exclusively — the system stops seeing physical touches. */
    external fun readerGrab(): Boolean
    external fun readerUngrab()
    external fun readerIsGrabbed(): Boolean

    /** When on, physical fingers are mirrored back out through uinput. */
    external fun readerSetSink(enabled: Boolean)
    external fun readerGetSink(): Boolean

    /**
     * Rectangles of the ImGui menu, flattened as `[x, y, w, h, ...]` in screen
     * pixels. A gesture that starts inside one of them is swallowed: it reaches
     * ImGui but is NOT mirrored back through uinput, so the app underneath does
     * not receive it too. Pass an empty array to restore full pass-through.
     */
    external fun readerSetRegions(rects: IntArray)
    external fun readerGetRegionCount(): Int

    /** Blocking wait: >0 pointers changed, 0 timeout, <0 error. */
    external fun readerPoll(timeoutMs: Int): Int

    /** int[n*3]: [id, x, y, ...] in screen pixels for the current orientation. */
    external fun readerReadPointers(): IntArray

    external fun readerGetMaxX(): Int
    external fun readerGetMaxY(): Int

    /** The panel the reader opened, so uinput clones that exact device. */
    external fun readerGetPanelPath(): String

    // ── Virtual touchscreen (uinput) ─────────────────────────────────────────

    external fun uinputSetSourcePanel(path: String)
    external fun uinputInit(width: Int, height: Int): Boolean
    external fun uinputClose()
    external fun uinputIsReady(): Boolean
    external fun uinputSetScreenParams(width: Int, height: Int, landscape: Boolean)

    /**
     * Consecutive frames the kernel refused since the last one landed. Read by
     * the daemon's status ticker so a "his touch died" report that reaches us
     * as a logcat can tell "we never wrote" from "we wrote and it was ignored".
     */
    external fun uinputWriteFailures(): Int

    external fun uinputDown(slot: Int, id: Int, x: Int, y: Int)
    external fun uinputMove(slot: Int, x: Int, y: Int)
    external fun uinputUp(slot: Int)
    external fun uinputMirrorClear()

    // ── Injection backend (uinput / InputManager) ────────────────────────────
    //
    // Which of the two carries a touch out of this process. Both share the
    // reader, the grab and the menu rectangles; only the last hop differs, so
    // this is a mode on the native injector rather than a second implementation.
    // See cpp/input/inject_backend.h for why switching tears the virtual device
    // down.

    /** [INJECT_BACKEND_UINPUT] or [INJECT_BACKEND_INPUT_MANAGER]. */
    external fun injectSetBackend(backend: Int): Boolean

    external fun injectGetBackend(): Int

    /**
     * Whether the *selected* backend can actually deliver a frame right now.
     *
     * Distinct from [injectGetBackend], and the distinction is the point: the
     * InputManager backend can be selected and still be refused by the platform
     * (a missing `INJECT_EVENTS` grant), which must be reported rather than
     * papered over.
     */
    external fun injectIsReady(): Boolean

    /** One-line reason the injection path is unusable, or "" when it is fine. */
    external fun injectLastError(): String

    /**
     * Reads config.json and applies the parts of it that must be in force before
     * the first OPEN — today the injection backend, which decides whether a
     * virtual touchscreen is created at all.
     *
     * Called once when the daemon starts: ui::start() loads the config too, but
     * that happens when the *menu* opens, long after the panel may already have
     * been taken. Returns the resulting backend's readiness.
     */
    external fun configLoad(): Int

    /** Values for [injectSetBackend] / [injectGetBackend]; mirror inject_backend.h. */
    const val INJECT_BACKEND_UINPUT = 0
    const val INJECT_BACKEND_INPUT_MANAGER = 1

    /** Injection slot / tracking id (see UINPUT_SLOT_PRIMARY in uinput_inject.h). */
    const val INJECT_SLOT = 8
    const val INJECT_ID = 1000

    // ── Renderer (hosted here, in the daemon) ────────────────────────────────
    //
    // The ImGui/Vulkan renderer lives in this same process now. [ShellLayerHost]
    // builds the layer on SurfaceFlinger and hands its Surface down through
    // [uiStart]; from then on the reader thread feeds fingers straight into ImGui
    // with [uiTouch] and the swallow-rectangles come straight back out of
    // [uiRegions]. No app process, no window, no ADB round-trip in the loop.
    //
    // Touch actions match AimbotNg.TOUCH_*: 0 down / 1 move / 2 up / 3 cancel.

    /**
     * Reports the panel density (`densityDpi`, as [SysDisplay.density] reads it)
     * so the renderer can size itself in physical units instead of raw pixels.
     *
     * Must reach the renderer before [uiStart] — ImGui reads it while it
     * initialises. [ShellLayerHost] calls it on every build, so a rotation
     * (which rebuilds the layer) always re-reports the current value.
     */
    external fun uiSetDensity(dpi: Int)

    /** Binds the renderer to a layer's Surface and starts its render thread. */
    external fun uiStart(surface: Surface): Boolean
    external fun uiStop()
    external fun uiIsRunning(): Boolean

    /**
     * `[width, height]` of the surface buffer the renderer draws into, or `[0, 0]`.
     *
     * This is the buffer that exists, which is what the layer's crop and display
     * frame have to describe. They are set from the size the host *asked* for,
     * and the two are not always the same number — when they differ
     * SurfaceFlinger scales the buffer into the frame (x and y by different
     * factors), which shows up as a squashed menu. See [ShellLayerHost].
     */
    external fun uiWindowSize(): IntArray

    /** Feeds one physical finger into ImGui, in screen pixels. */
    external fun uiTouch(action: Int, x: Float, y: Float)

    /** Live ImGui window rectangles as a flat `[x, y, w, h, ...]` array. */
    external fun uiRegions(): IntArray

    const val UI_TOUCH_DOWN = 0
    const val UI_TOUCH_MOVE = 1
    const val UI_TOUCH_UP = 2
    const val UI_TOUCH_CANCEL = 3

    // ── Capture (frames on their way to the renderer) ────────────────────────
    //
    // The producer is [ScreenCapture], on the Java side, because building a
    // virtual display needs the framework's DisplayManager and this process has
    // no Context. Only the pixels cross here — see cpp/capture/capture.h for
    // what happens on the other side.

    /**
     * Hands one RGBA frame over.
     *
     * [buffer] is the Image plane's direct [java.nio.ByteBuffer] — the copy into
     * native has to happen while the caller still holds the Image, because the
     * reader recycles that buffer as soon as it is closed. [rowStride] is in
     * bytes, and is frequently larger than `srcWidth * 4`.
     *
     * [srcWidth] x [srcHeight] is the whole screen; the `side x side` square at
     * ([cropX], [cropY]) is what native keeps. Cropping on that side of the
     * boundary is what lets the virtual display stay at full size — which is
     * the only way the platform hands over unscaled pixels — without paying to
     * copy a 2K frame across.
     */
    external fun capturePushFrame(
        buffer: java.nio.ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        cropX: Int,
        cropY: Int,
        side: Int
    )

    /** Whether the menu's capture switch is on. Polled by [ScreenCapture]. */
    external fun captureWanted(): Boolean

    /**
     * Whether the menu is actually sampling frames — its capture page is the
     * one on screen. Distinct from [captureWanted]: the switch stays on all
     * session, but a mirror is only worth building while something reads it.
     */
    external fun captureConsuming(): Boolean

    /**
     * Whether anyone is reading frames — the half of the supervisor's decision
     * that is not [captureWanted].
     *
     * It counts inference. The question it answers is "is anyone sampling
     * pixels", and a running model is — the supervisor holds a mirror open on
     * `captureWanted() && capturePreviewWanted()`, so excluding inference here
     * left the detector with no frame source whenever the Capture page was
     * closed, which presented as "inference only runs with that page open".
     *
     * It is still not the same flag as the switch, which is what keeps "capture
     * off" meaningful: a reader with the switch off still fails the AND.
     */
    external fun capturePreviewWanted(): Boolean

    /** Side of the square the menu asked for, in pixels. */
    external fun captureWantedSize(): Int

    /** Reports whether a producer is live, so the preview can say which it is. */
    external fun captureSetRunning(running: Boolean)

    /**
     * Declares every frame produced so far dead, because the producer is gone.
     *
     * Called by [ScreenCapture.stop], right before [captureSetRunning]`(false)`.
     * The frame generation is process-lifetime and never rewound, so without
     * this the next mirror's first frame reads as "new" to a consumer that had
     * already seen the previous mirror's last one — and the detector then runs
     * on a picture of a display that no longer exists, carrying the crop origin
     * of a screen that may since have changed shape. That is the "the boxes are
     * old and sit in the wrong place" failure, and it only became reachable once
     * inference could source its own frames.
     */
    external fun captureInvalidateFrames()

    // ── Model store (Add-Model page) ─────────────────────────────────────────
    //
    // The list the Add-Model page edits. Persisted to /data/local/tmp/aimbot_models.tsv
    // on every mutation, so an `app_process` restart loses nothing. Loaded once
    // by [ShellServerEntry] at boot.
    //
    // Row wire format on `modelAll()`:
    // `id\tname\tpath\tkind\tengine\tinputSize\tloaded\tcls1\x1fcls2...\tconfidence`
    // (confidence last — the class list is variable-length, so every field
    //  added later has to go after it or old rows shift by one)

    /** Loads the persisted list into the daemon's in-memory model list. */
    external fun modelLoadFromDisk()

    /**
     * Points the persisted list at `<dir>/aimbot_models.tsv` — the App's shared
     * models directory, which both this shell-uid process and the App can read
     * and write. Must be called before [modelLoadFromDisk]; a call after the
     * first load is ignored. A list that predates the move (still in
     * /data/local/tmp) is adopted on first load and migrated by the first save.
     */
    external fun modelSetStoreDir(dir: String)

    /**
     * Registers the model file at [path] with the C++ defaults (default engine
     * for its kind, confidence 0.5, threads 1, HTP perf 1, probed input size /
     * class count / tensor type). Idempotent per path. Returns the entry id,
     * or -1 when the path is not a .onnx/.tflite file.
     */
    external fun modelAddDefault(path: String): Int

    // ── Inference, driven from a script ────────────────────────────────────
    // The Model page's switch is a control the daemon paints onto its own
    // SurfaceFlinger layer, so `adb shell input` cannot reach it and neither can
    // the daemon's uinput injection — only a finger on the EVIOCGRAB'd panel can.
    // These set the same switch a finger would, which keeps one authority over
    // whether inference runs.

    /** Flips the Model page's inference switch. */
    external fun modelSwitchSet(on: Boolean)

    /** The switch's current position. */
    external fun modelSwitchGet(): Boolean

    /**
     * Live pipeline state: active, backend, counters, box count, stage timings.
     * Built on demand, so it is safe to call from a script but not per frame.
     */
    external fun inferStatus(): String

    /** What the loaded model bound — tensor shapes, classes, execution provider. */
    external fun inferDescribe(): String

    /** Returns every row, format above. Empty list = no models. */
    external fun modelAll(): Array<String>

    /** Adds an entry. `kindInt`/`engineInt` are the ENUM values from native. */
    external fun modelAdd(
        name: String, path: String, kindInt: Int, engineInt: Int, inputSize: Int
    ): Int

    external fun modelRemove(id: Int): Boolean

    /** Load/unload toggle; only one is loaded at a time. */
    external fun modelToggleLoaded(id: Int): Boolean

    external fun modelLoadedId(): Int
    external fun modelSetClasses(id: Int, classes: Array<String>): Boolean

    // ── External requests (file picker / keyboard) ────────────────────────────
    //
    // The request bus used to ferry file-picker / IME asks between the menu
    // and the App process. It is retired now: the menu draws its own file
    // browser in-overlay, and the model-name / class-name text fields were
    // dropped from the Add-Model dialog, so there is nothing left for the
    // App side to launch. The pump and relay in [ShellServerEntry] are gone
    // with it.
}
