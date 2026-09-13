package io.github.xiangsu1145.aimbotnextgen.overlay

import android.content.Context
import io.github.xiangsu1145.aimbotnextgen.shell.ShellController

/**
 * Owns the ImGui menu lifecycle, as seen from the app.
 *
 * The menu is **not** rendered here any more. It is a shell-owned SurfaceFlinger
 * layer built by the daemon (see `shell/ShellLayerHost`), and the renderer runs
 * inside the daemon's process. Moving it there was the whole point: the layer is
 * created directly on SurfaceFlinger rather than as a `TYPE_APPLICATION_OVERLAY`
 * window, which is possible because the shell UID holds ACCESS_SURFACE_FLINGER —
 * and that in turn removes three problems at once:
 *
 *  • no `SYSTEM_ALERT_WINDOW`, so no manual permission grant and no foreground
 *    service;
 *  • no window attributes, so the platform's `FLAG_NOT_TOUCHABLE → alpha 0.8` cap
 *    cannot apply and the board is drawn at whatever alpha it asks for;
 *  • no full-screen TOUCHABLE window, so the device's own touch is never eaten
 *    while the grab is not held.
 *
 * All this controller does is tell the daemon to turn the layer on or off, and
 * remember the answer. It stays process-wide ([get]) so the state survives an
 * Activity recreation while the daemon keeps rendering.
 */
class ImguiController private constructor(private val context: Context) {

    interface Listener {
        fun onImguiRunningChanged(running: Boolean)
    }

    var listener: Listener? = null

    var isRunning: Boolean = false
        private set

    /** Asks the daemon to build the menu layer and start the renderer. */
    fun start() {
        if (isRunning) return
        val shell = ShellController.get(context)
        if (!shell.isRunning) return
        shell.sendCommand("UI_ON")
        isRunning = true
        listener?.onImguiRunningChanged(true)
    }

    /** Asks the daemon to drop the renderer and destroy the layer. */
    fun stop() {
        if (!isRunning) return
        isRunning = false
        ShellController.get(context).sendCommand("UI_OFF")
        listener?.onImguiRunningChanged(false)
    }

    /**
     * The shell went away, and the layer went with it (it belongs to the daemon's
     * process). Stop pretending the menu is up, but send nothing — there is no
     * daemon left to talk to.
     */
    fun reset() {
        if (!isRunning) return
        isRunning = false
        listener?.onImguiRunningChanged(false)
    }

    /** Detaches the Activity listener. The daemon keeps rendering. */
    fun detach() {
        listener = null
    }

    companion object {
        @Volatile
        private var shared: ImguiController? = null

        /** The process-wide controller, created on first use. */
        fun get(context: Context): ImguiController =
            shared ?: synchronized(this) {
                shared ?: ImguiController(context.applicationContext).also { shared = it }
            }
    }
}
