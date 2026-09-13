package io.github.xiangsu1145.aimbotnextgen.shell

import android.graphics.PixelFormat
import android.graphics.Rect
import android.util.Log
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
            build(w, h, report)
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

    /** Calls a hidden method by name + arity, so exact signatures need not match. */
    private fun invoke(cls: Class<*>, target: Any, name: String, vararg args: Any?): Any? {
        val m: Method = cls.declaredMethods.firstOrNull {
            it.name == name && it.parameterTypes.size == args.size
        } ?: throw NoSuchMethodException("$name/${args.size}")
        m.isAccessible = true
        return m.invoke(target, *args)
    }
}
