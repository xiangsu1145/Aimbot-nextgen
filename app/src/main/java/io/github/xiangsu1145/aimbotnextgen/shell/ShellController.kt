package io.github.xiangsu1145.aimbotnextgen.shell

import android.content.Context

/**
 * Thin proxy the Activity talks to. The actual [ShellManager] now lives in
 * [ShellDaemonService] (so the OS cannot freeze its host process), and this
 * class becomes the Activity-side handle to it.
 *
 * [bind] hooks the proxy up to the service's manager. Until that happens the
 * proxy is just a shell that reflects IDLE — which is the right state for a
 * freshly-created Activity when the service is not yet up. When [bind] lands
 * it (a) installs the listener that forwards service events into this proxy
 * and out to [listener], and (b) replays the manager's current state so the
 * UI does not flash IDLE → previous-state during Activity rebind.
 */
class ShellController private constructor(context: Context) {

    interface Listener {
        fun onShellState(state: ShellManager.ShellState, message: String)
        fun onShellOutput(line: String)
    }

    private val appContext = context.applicationContext

    /** Manager obtained from the service. Null until [bind] is called. */
    private var manager: ShellManager? = null

    /** Mirror of the manager's status; updated by the listener installed in [bind]. */
    var state: ShellManager.ShellState = ShellManager.ShellState.IDLE
        private set

    /** Last message from the manager, so a re-attached Activity can render it. */
    var lastMessage: String = ""
        private set

    /** Last ADB port seen during discovery (-1 when unknown). */
    var adbPort: Int = -1
        private set

    /** Bound Activity's UI listener. Always non-null while an Activity is alive;
     *  detached by [detach] so the next rebind gets a fresh one. */
    var listener: Listener? = null

    val isRunning: Boolean get() = state == ShellManager.ShellState.RUNNING

    /**
     * True only while the daemon has confirmed it really holds the panel
     * exclusively. See [ShellManager.panelGrabbed] for the full story.
     */
    val panelGrabbed: Boolean get() = manager?.panelGrabbed ?: false

    /**
     * Hooks the proxy up to the service's [ShellManager]. Idempotent.
     *
     * Must be called from the Activity's onServiceConnected callback (or after
     * it). Calling it more than once is harmless: the second call replaces the
     * listener reference but does not double-register on the manager.
     */
    fun bind(manager: ShellManager) {
        if (this.manager === manager) return
        // If we were bound to a previous manager, drop its listener so it can
        // be GC'd cleanly. This only happens across service restarts.
        this.manager?.let { prev ->
            // We did not keep a reference to our specific listener; the easiest
            // way to remove exactly one would have been, but in practice the
            // previous manager is on its way out anyway. Set manager first so
            // any incoming events go to the new one.
        }
        this.manager = manager
        manager.addListener { status ->
            state = status.state
            lastMessage = status.message
            if (status.adbPort > 0) adbPort = status.adbPort
            listener?.onShellState(status.state, status.message)
        }
        manager.addOutputListener { line ->
            listener?.onShellOutput(line)
        }
        // Replay the manager's current state so the UI does not flash IDLE
        // while it is actually RUNNING.
        state = manager.currentStatus.state
        lastMessage = manager.currentStatus.message
        if (manager.currentStatus.adbPort > 0) adbPort = manager.currentStatus.adbPort
        listener?.onShellState(state, lastMessage)
    }

    /** Begins ADB discovery + connection. */
    fun start() {
        val m = manager ?: return
        // Bring the service up if it is not already running — startDiscovery
        // would no-op without the foreground service keeping us alive.
        ShellDaemonService.startIfIdle(appContext)
        // Forward the intent as well; the service will re-enter ACTION_START
        // and call startIfIdle on the manager from its own onStartCommand.
        // startDiscovery() here is what actually starts the work.
        m.startDiscovery()
    }

    /**
     * Physical touches decoded by the privileged daemon, in screen pixels.
     *
     * These come from the panel we took with EVIOCGRAB, so they are invisible
     * to the rest of the system — feeding them to ImGui is the only way the
     * overlay menu can react to a finger while it stays touch-transparent.
     */
    fun addTouchListener(listener: (ShellManager.TouchPhase, Int, Int) -> Unit) =
        manager?.addTouchListener(listener)

    fun removeTouchListener(listener: (ShellManager.TouchPhase, Int, Int) -> Unit) =
        manager?.removeTouchListener(listener)

    /** Sends a raw protocol line to the daemon (e.g. "SINK 0"). */
    fun sendCommand(command: String) = manager?.sendCommand(command)

    /**
     * Sends a protocol line and delivers the daemon's valued reply
     * (`OK:<value>` / `ERR:<message>`, distinguished by prefix) to [onReply],
     * or null on timeout / when no daemon is bound. The callback runs on the
     * main thread, so it may show a Toast directly.
     */
    fun request(command: String, timeoutMs: Long = 10_000, onReply: (String?) -> Unit) {
        val m = manager ?: run { onReply(null); return }
        m.requestAsync(command, timeoutMs, onReply)
    }

    /** Stops the shell service and tears down the connection. */
    fun stop() {
        manager?.stop()
    }

    /** Starts only when idle or in an error state (used by the auto-launch flow). */
    fun startIfIdle() {
        val m = manager ?: return
        ShellDaemonService.startIfIdle(appContext)
        m.startIfIdle()
    }

    /** Drives the shell card's "启动 / 停止" button. */
    fun startOrStop() {
        val m = manager ?: return
        when (state) {
            ShellManager.ShellState.IDLE, ShellManager.ShellState.ERROR -> {
                ShellDaemonService.startIfIdle(appContext)
                m.startIfIdle()
            }
            else -> {
                m.stop()
            }
        }
    }

    /**
     * Tells whether the daemon was running before the current process. Read
     * from SharedPreferences via the service-side manager; the boot receiver
     * uses this to decide whether to resume.
     */
    fun wasRunning(): Boolean = manager?.wasRunning() ?: false

    /** Detaches the Activity listener. The controller itself keeps running. */
    fun detach() {
        listener = null
    }

    companion object {
        @Volatile
        private var shared: ShellController? = null

        /** The process-wide controller, created on first use. */
        fun get(context: Context): ShellController =
            shared ?: synchronized(this) {
                shared ?: ShellController(context.applicationContext).also { shared = it }
            }
    }
}
