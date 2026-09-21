package io.github.xiangsu1145.aimbotnextgen.shell

import android.graphics.PixelFormat
import android.graphics.Rect
import android.os.Binder
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.util.Size
import android.view.Surface
import java.lang.reflect.Method

/**
 * Owns the menu's layer on the display, from inside the shell daemon.
 *
 * ── Why there is no window here ──────────────────────────────────────────────
 *
 * The obvious way to float a UI over other apps is `WindowManager.addView`. It is
 * not available to us, and not for a permission reason: `ActivityThread.systemMain()`
 * does give us a system Context, but WMS then refuses to open a session because a
 * raw `app_process` pid was never registered with ActivityManager —
 *
 *     mProcess = mAtmService.mProcessMap.getProcess(mPid);
 *     if (mProcess == null) throw new IllegalStateException("Unknown pid=" + ...)
 *
 * — so *no* window type or flag can be added, ever. (Verified on this device.)
 *
 * So this bypasses WMS completely and talks to SurfaceFlinger directly. The shell
 * UID holds ACCESS_SURFACE_FLINGER, which is the only thing needed to build a
 * layer and composite it above other apps. Nothing in the app asks for any
 * permission, and the platform's `FLAG_NOT_TOUCHABLE → alpha 0.8` cap — which is
 * what used to dim the board — cannot apply, because there is no window attribute
 * to cap. The board's own alpha is now the only alpha that exists.
 *
 * ── What that buys, concretely ───────────────────────────────────────────────
 *
 *  • No SYSTEM_ALERT_WINDOW, no foreground service, no overlay window at all.
 *  • No 0.8 cap and no "full-screen touchable window eats the whole device".
 *  • `setSkipScreenshot(true)` works — it is silently ignored for a normal app
 *    UID, but this layer belongs to the shell process — so the menu is invisible
 *    to screenshots, screen recording and casting while still fully visible on
 *    the panel to the eye, and to HDMI out.
 *  • The layer survives Android 12+'s *untrusted touch* rule, because it now
 *    carries an explicit input window that says "not an occluder". See
 *    [applyInputWindowInfo] — without it, every touch that passes through the
 *    board is dropped by InputDispatcher on strict ROMs.
 *
 * ── Geometry notes (both learned the hard way) ───────────────────────────────
 *
 *  • `setGeometry`'s `displayFrame` decides where the layer actually lands. It
 *    OVERRIDES `setPosition`: with `(0,0,w,h)` the layer is pinned to the top-left
 *    corner no matter what `setPosition` says. The offset has to go here.
 *  • Every handle is held in a field on purpose. Dropping the last reference lets
 *    the finaliser destroy the layer and the menu vanishes.
 */
object ShellLayerHost {

    private const val TAG = "aimbot_layer"

    /** Layer name as it appears in `dumpsys SurfaceFlinger --list`. */
    private const val LAYER_NAME = "aimbot-ui"

    @Volatile private var session: Any? = null
    @Volatile private var surfaceControl: Any? = null
    @Volatile private var surface: Surface? = null
    @Volatile private var running = false

    /**
     * Whether the layer is hidden from screenshots / recordings / casts.
     *
     * Defaults to `true` — that is what the menu has been doing since this
     * host was built, and "Anti Screenshot" only makes sense as the safe-by-
     * default state. The ImGui Settings page's switch flips it through
     * [setSkipScreenshot]; the value is also read by [applyGeometry] so a
     * layer built while the switch is off still comes out the right way.
     */
    @Volatile
    private var skipScreenshot: Boolean = true

    fun isRunning(): Boolean = running

    /**
     * Toggles the layer's screenshot-block flag without rebuilding it.
     *
     * Going through a dedicated [android.view.SurfaceControl.Transaction]
     * (rather than [stop] + [start]) means the renderer keeps drawing, the
     * layer's z-order and geometry are not touched, and the change lands in
     * the next SurfaceFlinger frame — a toggle you can verify by taking a
     * screenshot before and after.
     *
     * Called from two places: the ImGui Settings page (via the C++ sync hook
     * that eventually reaches [setSkipScreenshot] through JNI), and the
     * implicit read in [applyGeometry] for fresh layers.
     *
     * @return true when the flag was applied to the running layer, false
     *         when the layer is not up (the value was still recorded and
     *         will be honoured on the next build), or when the platform
     *         has no `setSkipScreenshot` method at all (older OEM images
     *         silently skip it — same fallback as the original geometry
     *         transaction).
     */
    @JvmStatic
    @Synchronized
    fun setSkipScreenshot(on: Boolean): Boolean {
        skipScreenshot = on
        val sc = surfaceControl ?: return false
        return try {
            val txnCls = Class.forName("android.view.SurfaceControl\$Transaction")
            val txn = txnCls.getDeclaredConstructor().newInstance()
            invoke(txnCls, txn, "setSkipScreenshot", sc, on)
            invoke(txnCls, txn, "apply")
            true
        } catch (t: Throwable) {
            Log.w(TAG, "setSkipScreenshot($on) failed", t)
            false
        }
    }

    /**
     * Builds the layer, points the renderer at it and starts drawing.
     *
     * @param w,h buffer size in pixels; the caller passes the real display
     *            geometry, because a full-screen layer must match the panel or
     *            the Vulkan swapchain and the touch coordinate space disagree.
     * @return `on` / `already-running` / `fail:<reason>`
     */
    @Synchronized
    fun start(w: Int, h: Int, report: (String) -> Unit): String {
        if (running) return "already-running"
        return try {
            // Timed because this is the other half of "the daemon is up but the
            // menu never appears": `daemon ready` only proves the TCP port is
            // listening, and everything the user actually sees is built here —
            // SurfaceControl reflection, the transaction, the Vulkan device and
            // the render thread's first frame. Each phase reports its own cost,
            // so a capture shows which one stalled.
            val t0 = SystemClock.elapsedRealtime()
            var tPrev = t0
            val timed: (String) -> Unit = { line ->
                val now = SystemClock.elapsedRealtime()
                report("$line [+${now - tPrev}ms / total ${now - t0}ms]")
                tPrev = now
            }
            build(w, h, timed)
            running = true
            "on"
        } catch (t: Throwable) {
            Log.e(TAG, "layer build failed", t)
            report("LAYER FAIL ${t.javaClass.simpleName}: ${t.message}")
            teardown()
            "fail:${t.message}"
        }
    }

    /** Stops the renderer and destroys the layer. Idempotent. */
    @Synchronized
    fun stop(): String {
        if (!running) return "not-running"
        running = false
        runCatching { ShellNative.uiStop() }
            .onFailure { Log.w(TAG, "uiStop failed", it) }
        teardown()
        return "off"
    }

    /**
     * Rebuilds the layer at a new size. This is what a rotation needs.
     *
     * There is no in-place resize worth doing here. The buffer size is baked in
     * when the layer is built, and the renderer takes *both* its Vulkan swapchain
     * extent and ImGui's `DisplaySize` from that buffer — so a rotated panel
     * leaves the menu compositing a surface of the wrong shape, which is what
     * tears it apart on screen. Re-running the build is the only path that also
     * redoes the swapchain, and it is the path already known to work.
     *
     * The renderer is stopped first on purpose: its render thread owns the
     * Vulkan device and the old Surface, so it has to be joined before either is
     * let go. `running` deliberately stays true throughout — a finger arriving
     * mid-rebuild is queued and then discarded by `ShellNative.uiStop`, not
     * misdelivered.
     *
     * @return `on` / `not-running` / `fail:<reason>`
     */
    @Synchronized
    fun resize(w: Int, h: Int, report: (String) -> Unit): String {
        if (!running) return "not-running"
        runCatching { ShellNative.uiStop() }
            .onFailure { Log.w(TAG, "uiStop failed during resize", it) }
        teardown()
        return try {
            build(w, h, report)
            "on"
        } catch (t: Throwable) {
            Log.e(TAG, "layer rebuild failed", t)
            report("LAYER FAIL ${t.javaClass.simpleName}: ${t.message}")
            teardown()
            running = false
            "fail:${t.message}"
        }
    }

    // ── Internals ────────────────────────────────────────────────────────────

    private fun build(w: Int, h: Int, report: (String) -> Unit) {
        val scCls = Class.forName("android.view.SurfaceControl")
        val builderCls = Class.forName("android.view.SurfaceControl\$Builder")
        val sessionCls = Class.forName("android.view.SurfaceSession")

        val s = sessionCls.getDeclaredConstructor().newInstance()
        session = s

        val builder = builderCls.getDeclaredConstructor(sessionCls).newInstance(s)
        invoke(builderCls, builder, "setName", LAYER_NAME)
        invoke(builderCls, builder, "setBufferSize", w, h)
        invoke(builderCls, builder, "setFormat", PixelFormat.RGBA_8888)
        // Keep the layer's own alpha: the board is translucent black on purpose,
        // and a layer forced opaque would drop that and show black instead.
        runCatching { invoke(builderCls, builder, "setOpaque", false) }

        val sc = builderCls.getMethod("build").invoke(builder)
        surfaceControl = sc

        applyGeometry(sc, w, h, report)

        report("LAYER up ${w}x$h")

        @Suppress("UNCHECKED_CAST")
        val sf = Class.forName("android.view.Surface")
            .getDeclaredConstructor(scCls).newInstance(sc) as Surface
        surface = sf

        // The renderer sizes itself in physical units, so it needs the panel's
        // density before its first frame. Read here rather than passed down the
        // UI_ON command: density is a property of this display, and resize()
        // rebuilds through this same path so a rotation always re-reports it.
        ShellNative.uiSetDensity(SysDisplay.density())

        if (!ShellNative.uiStart(sf)) {
            error("uiStart returned false (Vulkan init failed — see logcat 'AimbotNg')")
        }

        // The crop and frame above describe the size we *asked* for. Put them on
        // the size the buffer actually came out as instead: SurfaceFlinger fits a
        // buffer into its frame, so if the two disagree it scales x and y by
        // different factors and the menu is drawn squashed — the portrait bug.
        // (The renderer no longer claims pre-rotation, so they should now always
        // agree; this is the belt to that pair of braces, and it reports when the
        // assumption breaks so the log shows it.)
        val real = ShellNative.uiWindowSize()
        if (real.size == 2 && real[0] > 0 && real[1] > 0 && (real[0] != w || real[1] != h)) {
            report("LAYER buffer is ${real[0]}x${real[1]}, asked for ${w}x$h — re-framing")
            applyGeometry(sc, real[0], real[1], report)
        }

        report("LAYER renderer started")
    }

    /**
     * Points the layer at the top-left `w`×`h` rectangle of the display.
     *
     * Re-applied rather than written once, because it has to be able to follow a
     * corrected buffer size — see [build].
     */
    private fun applyGeometry(sc: Any, w: Int, h: Int, report: (String) -> Unit) {
        val txnCls = Class.forName("android.view.SurfaceControl\$Transaction")
        val txn = txnCls.getDeclaredConstructor().newInstance()
        // No parent token is reachable on Android 15, so the layer is left as a
        // root layer on the default layer stack and simply lifted above every
        // app window by its z-order instead.
        runCatching { invoke(txnCls, txn, "setLayerStack", sc, 0) }
        invoke(txnCls, txn, "setLayer", sc, Int.MAX_VALUE)
        // displayFrame, not setPosition — see the class docs.
        invoke(txnCls, txn, "setGeometry", sc,
            Rect(0, 0, w, h), Rect(0, 0, w, h), 0)
        // The menu's screenshot-block flag — defaults ON (preserved across
        // upgrades), but live-toggleable from the ImGui Settings page so a
        // user can see the menu in screenshots when they need to. The
        // runCatching covers the OEMs that don't expose this method.
        runCatching { invoke(txnCls, txn, "setSkipScreenshot", sc, skipScreenshot) }
            .onFailure { report("LAYER skipScreenshot unavailable: ${it.javaClass.simpleName}") }
        // Declare the layer's input window *before* the transaction is applied, so
        // it lands in the same commit as the geometry. This is the fix for
        // Android 12+'s "Untrusted touch due to occlusion" — see the function.
        applyInputWindowInfo(txnCls, txn, sc, w, h, report)
        invoke(txnCls, txn, "show", sc)
        txnCls.getMethod("apply").invoke(txn)
    }

    /** Drops the layer and everything that keeps it alive. Never throws. */
    private fun teardown() {
        surfaceControl?.let { sc ->
            runCatching { sc.javaClass.getMethod("release").invoke(sc) }
                .onFailure { Log.w(TAG, "release failed", it) }
        }
        session?.let { s ->
            runCatching { (s as? AutoCloseable)?.close() }
            runCatching { s.javaClass.getMethod("kill").invoke(s) }
        }
        surface = null
        surfaceControl = null
        session = null
    }

    // ── Untrusted touch (Android 12+ occlusion) ──────────────────────────────

    /**
     * Gives the layer an explicit input window that says *"I must not be counted
     * as an occluder"*, which is what lets touches pass through the board again.
     *
     * ── The problem ──────────────────────────────────────────────────────────
     *
     * Android 12+ drops any touch that is occluded by a window the platform does
     * not trust (`touchOcclusionMode = BLOCK_UNTRUSTED` is the default). A layer
     * built straight on SurfaceFlinger never had an input window of its own, so
     * it got that default — and since it is full-screen and opaque, InputDispatcher
     * rejected *every* touch underneath it, injected ones included:
     *
     *     Dropping untrusted touch event due to occlusion by aimbot-ui
     *
     * Touches delivered to the menu itself keep working, because the menu never
     * goes through the dispatcher — it reads `/dev/input` and feeds ImGui
     * directly. "The board clicks, nothing else does" is that bug's fingerprint.
     *
     * ── Why this knob and no other ───────────────────────────────────────────
     *
     * `SurfaceFlinger/Layer.cpp`:
     *
     *     void Layer::fillTouchOcclusionMode(WindowInfo& info) {
     *         sp<Layer> p = sp<Layer>::fromExisting(this);      // ← starts at SELF
     *         while (p && !p->hasInputInfo()) p = p->mDrawingParent.promote();
     *         if (p) info.touchOcclusionMode = p->mDrawingState.inputInfo.touchOcclusionMode;
     *     }
     *
     *     bool Layer::hasInputInfo() const { return mDrawingState.inputInfo.token != nullptr; }
     *
     * The walk starts at the layer itself, so the value we set is the value that
     * wins — **provided `hasInputInfo()` is true**. That is why `token` below is
     * not optional: without it the loop climbs to a parent this root layer does
     * not have, writes nothing, and the layer silently stays on BLOCK_UNTRUSTED
     * even though we "set" a different mode. Everything else here is detail.
     *
     * Two assumptions this deliberately does *not* make:
     *  • alpha — the warning has no `(obscuring opacity = …, maximum allowed = …)`
     *    suffix, so this is the hard-block branch; fading the board would only
     *    change how it looks and fix nothing.
     *  • touchableRegion — the region is irrelevant to the judgement (the layer
     *    was already `<empty>` while occluding), so shrinking or punching holes
     *    in it changes nothing either.
     *
     * Nothing about the layer's looks is touched: no alpha, no geometry, no
     * buffer, no z-order. Only the input metadata the compositor forwards.
     *
     * Everything is best-effort by design: a field this OEM build does not have
     * costs that one field (logged), never the layer.
     */
    private fun applyInputWindowInfo(
        txnCls: Class<*>,
        txn: Any,
        sc: Any,
        w: Int,
        h: Int,
        report: (String) -> Unit,
    ) {
        val handle: Any
        val notes: String
        try {
            val built = buildInputWindowHandle(w, h, report)
            handle = built.first
            notes = built.second
        } catch (t: Throwable) {
            Log.w(TAG, "input-window handle build failed", t)
            report("LAYER input-window unavailable: ${t.javaClass.simpleName}: ${t.message}")
            return
        }

        try {
            invokeBest(txnCls, txn, "setInputWindowInfo", sc, handle)
            // Logged straight to logcat as well as to the app: the socket copy is
            // what the app shows, but a field report is read out of logcat, and
            // this line is the one that says whether the fix is live.
            Log.i(TAG, "input window: $notes")
            report("LAYER input-window set: $notes")
        } catch (t: Throwable) {
            Log.w(TAG, "setInputWindowInfo failed", t)
            report("LAYER setInputWindowInfo failed: ${t.javaClass.simpleName}: ${t.message}")
        }

        // The same bit WindowManager sets on its own overlays. Some builds honour
        // it and skip the occlusion check outright — an independent second way
        // out, so ask for it too.
        runCatching { invokeBest(txnCls, txn, "setTrustedOverlay", sc, true) }
            .onFailure { Log.i(TAG, "setTrustedOverlay not available (${it.javaClass.simpleName})") }
    }

    /**
     * Builds the platform's input window object, filling only the fields that
     * matter, and reports which of them this build actually has.
     *
     * @return the object to hand to `setInputWindowInfo`, plus a one-line summary
     *         of what went in — the summary is what the daemon log shows, and it
     *         is the thing to compare against `dumpsys input` on a strict device.
     */
    private fun buildInputWindowHandle(w: Int, h: Int, report: (String) -> Unit): Pair<Any, String> {
        val cls = HANDLE_CLASSES.firstNotNullOfOrNull { runCatching { Class.forName(it) }.getOrNull() }
            ?: error("no InputWindowHandle class on this build")

        // The application handle is part of the window's identity to the
        // dispatcher. A real one (rather than null) keeps the JNI-side conversion
        // on a path that is known to be exercised by WindowManager every day.
        val appCls = runCatching { Class.forName("android.view.InputApplicationHandle") }.getOrNull()
        val appHandle = appCls?.let { c ->
            runCatching {
                c.getConstructor(android.os.IBinder::class.java, String::class.java,
                    java.lang.Long.TYPE).newInstance(Binder(), LAYER_NAME, 5_000L)
            }.getOrNull()
        }

        val handle = runCatching {
            cls.getConstructor(appCls, java.lang.Integer.TYPE).newInstance(appHandle, 0)
        }.getOrElse { cls.getDeclaredConstructor().newInstance() }

        val occlusion = resolveOcclusionAllow(report)
        val inputConfig = resolveInputConfig(report)

        val values = linkedMapOf<String, Any?>(
            // ★ The one that decides whether any of the rest is even read —
            //   see the note above `Layer::hasInputInfo()`.
            "token" to Binder(),
            "name" to LAYER_NAME,
            "packageName" to LAYER_PACKAGE,
            "ownerUid" to Process.myUid(),
            "ownerPid" to Process.myPid(),
            "displayId" to 0,                 // layer stack 0 — see applyGeometry
            // Never intercept. The menu reads /dev/input itself, and everything
            // that is not the menu must keep going where it always went.
            "layoutParamsFlags" to (0x00000010 or 0x00000020), // NOT_TOUCHABLE | NOT_TOUCH_MODAL
            "inputConfig" to inputConfig,
            "touchOcclusionMode" to occlusion,
            "trustedOverlay" to true,
            "scaleFactor" to 1.0f,            // 0 would be a zero-sized input window
            "alpha" to 1.0f,                  // SurfaceFlinger re-reads the layer's own
            "canOccludePresentation" to false,
            "replaceTouchableRegionWithCrop" to false,
        )

        val applied = ArrayList<String>()
        val missing = ArrayList<String>()
        for ((name, value) in values) {
            if (value == null) { missing += name; continue }
            if (setMember(handle, name, value)) applied += "$name=$value" else missing += name
        }

        // `frame` and `touchableRegion` are final Rects/Regions: mutate in place.
        // An empty touchable region is exactly what we want — it is the state the
        // layer was already in, and it is not what decides the occlusion.
        runCatching {
            (cls.getField("frame").get(handle) as Rect).set(0, 0, w, h)
            applied += "frame=[0,0,$w,$h]"
        }
        // `contentSize` is an immutable Size, so it needs the field route.
        setMember(handle, "contentSize", Size(w, h))

        val note = "${cls.simpleName}[" + applied.joinToString(" ") +
            if (missing.isEmpty()) "]" else "] missing=${missing.joinToString(",")}"
        return handle to note
    }

    /**
     * Resolves `touchOcclusionMode = ALLOW` by name.
     *
     * The constant moves around between API levels (`android.gui.TouchOcclusionMode`
     * on 13+, an `android.view` copy on 12, and some builds only expose the
     * `TOUCH_OCCLUSION_*` spelling). A wrong literal here would silently mean
     * `BLOCK_UNTRUSTED` — the bug — so the value is looked up, and only then
     * allowed to fall back.
     */
    private fun resolveOcclusionAllow(report: (String) -> Unit): Int {
        for (clsName in OCCLUSION_CLASSES) {
            val cls = runCatching { Class.forName(clsName) }.getOrNull() ?: continue
            for (fieldName in OCCLUSION_FIELDS) {
                val v = runCatching { cls.getField(fieldName).get(null) }.getOrNull() ?: continue
                val n = when (v) {
                    is Number -> v.toInt()
                    is Enum<*> -> v.ordinal // AIDL enums are declared in value order
                    else -> continue
                }
                report("LAYER occlusion ALLOW: $clsName.$fieldName = $n")
                Log.i(TAG, "occlusion ALLOW = $n (from $clsName.$fieldName)")
                return n
            }
        }
        report("LAYER occlusion ALLOW: no constant found, falling back to $OCCLUSION_ALLOW_FALLBACK")
        Log.i(TAG, "occlusion ALLOW = $OCCLUSION_ALLOW_FALLBACK (fallback)")
        return OCCLUSION_ALLOW_FALLBACK
    }

    /**
     * Resolves the input-config flag bits (not touchable / not focusable / no
     * input channel / trusted overlay).
     *
     * Returns null when no constants can be read, in which case the field is left
     * alone on purpose: the layer already lets everything through (empty touchable
     * region), and guessing bit values here could accidentally make it *touchable*
     * — which would be far worse than the bug being fixed.
     */
    private fun resolveInputConfig(report: (String) -> Unit): Int? {
        for (clsName in INPUT_CONFIG_CLASSES) {
            val cls = runCatching { Class.forName(clsName) }.getOrNull() ?: continue
            var flags = 0
            var found = 0
            for (flag in INPUT_CONFIG_FLAGS) {
                val v = runCatching { cls.getField(flag).get(null) }.getOrNull()
                if (v is Number) { flags = flags or v.toInt(); found++ }
            }
            if (found > 0) {
                report("LAYER inputConfig: $clsName $found/${INPUT_CONFIG_FLAGS.size}" +
                    " = 0x${Integer.toHexString(flags)}")
                Log.i(TAG, "inputConfig = 0x${Integer.toHexString(flags)} (from $clsName)")
                return flags
            }
        }
        report("LAYER inputConfig: constants unreadable — leaving the field alone")
        Log.i(TAG, "inputConfig: constants unreadable — field left alone")
        return null
    }

    /**
     * Writes one field on a hidden platform class: public field, then private
     * field, then a `setXxx` setter. Returns false when the build simply has no
     * such member; never throws, and refuses to touch final fields.
     */
    private fun setMember(target: Any, name: String, value: Any): Boolean {
        val cls = target.javaClass
        val field = runCatching { cls.getField(name) }.getOrNull()
            ?: runCatching { cls.getDeclaredField(name) }.getOrNull()
        if (field != null && !java.lang.reflect.Modifier.isFinal(field.modifiers)) {
            val ok = runCatching {
                field.isAccessible = true
                field.set(target, value)
            }.isSuccess
            if (ok) return true
        }
        val setterName = "set" + name.replaceFirstChar { it.uppercaseChar() }
        val m = runCatching {
            cls.methods.firstOrNull { it.name == setterName && it.parameterTypes.size == 1 }
        }.getOrNull()
        if (m != null) {
            return runCatching {
                m.isAccessible = true
                m.invoke(target, value)
            }.isSuccess
        }
        return false
    }

    /** Calls a hidden method by name + arity, so exact signatures need not match. */
    private fun invoke(cls: Class<*>, target: Any, name: String, vararg args: Any?): Any? {
        val m: Method = cls.declaredMethods.firstOrNull {
            it.name == name && it.parameterTypes.size == args.size
        } ?: throw NoSuchMethodException("$name/${args.size}")
        m.isAccessible = true
        return m.invoke(target, *args)
    }

    /**
     * Like [invoke], but only accepts the overload whose parameters actually take
     * [args] — needed where a name has several same-arity overloads.
     */
    private fun invokeBest(cls: Class<*>, target: Any, name: String, vararg args: Any?): Any? {
        val m = cls.declaredMethods.firstOrNull { c ->
            c.name == name && c.parameterTypes.size == args.size &&
                c.parameterTypes.indices.all { accepts(c.parameterTypes[it], args[it]) }
        } ?: throw NoSuchMethodException("$name/$args")

        m.isAccessible = true
        return m.invoke(target, *args)
    }

    private fun accepts(param: Class<*>, arg: Any?): Boolean {
        if (arg == null) return !param.isPrimitive
        val boxed = when (param.name) {
            "boolean" -> java.lang.Boolean::class.java
            "int" -> java.lang.Integer::class.java
            "long" -> java.lang.Long::class.java
            "float" -> java.lang.Float::class.java
            "double" -> java.lang.Double::class.java
            "short" -> java.lang.Short::class.java
            "byte" -> java.lang.Byte::class.java
            "char" -> java.lang.Character::class.java
            else -> param
        }
        return boxed.isInstance(arg)
    }

    // ── Untrusted-touch constants ────────────────────────────────────────────

    /** Carriers of the input window, newest first-seen name first. */
    private val HANDLE_CLASSES = listOf(
        "android.view.InputWindowHandle",   // 12–15, the one WindowManager itself uses
        "android.window.InputWindowHandle",
        "android.window.InputWindowInfo",
    )

    /** Carriers of the `ALLOW` constant, per API level. */
    private val OCCLUSION_CLASSES = listOf(
        "android.gui.TouchOcclusionMode",
        "android.view.TouchOcclusionMode",
        "android.window.InputWindowInfo",
        "android.view.InputWindowHandle",
    )

    private val OCCLUSION_FIELDS = listOf("ALLOW", "TOUCH_OCCLUSION_ALLOW")

    /** Last resort only; a wrong value here is the bug, so it never wins. */
    private const val OCCLUSION_ALLOW_FALLBACK = 2

    /** Carriers of the input-config flag bits, per API level. */
    private val INPUT_CONFIG_CLASSES = listOf(
        "android.os.InputConfig",
        "android.view.InputWindowHandle\$InputConfig",
        "android.window.InputWindowHandle\$InputConfig",
        "android.window.InputWindowInfo\$InputConfig",
    )

    private val INPUT_CONFIG_FLAGS =
        listOf("NOT_FOCUSABLE", "NOT_TOUCHABLE", "NO_INPUT_CHANNEL", "TRUSTED_OVERLAY")

    /** Cosmetic: only surfaces in `dumpsys input`. */
    private const val LAYER_PACKAGE = "io.github.xiangsu1145.aimbotnextgen"
}
