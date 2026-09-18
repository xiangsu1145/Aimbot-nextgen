package io.github.xiangsu1145.aimbotnextgen.shell

import android.content.Context
import android.content.SharedPreferences
import android.graphics.Point
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Process
import android.util.Log
import android.view.Display
import android.view.WindowManager
import io.github.xiangsu1145.aimbotnextgen.adb.AdbClient
import io.github.xiangsu1145.aimbotnextgen.adb.AdbKey
import io.github.xiangsu1145.aimbotnextgen.adb.AdbMdns
import io.github.xiangsu1145.aimbotnextgen.adb.AdbPairingClient
import io.github.xiangsu1145.aimbotnextgen.adb.AdbPairingState
import kotlinx.coroutines.*
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.util.concurrent.CopyOnWriteArrayList

private const val TAG = "ShellManager"

/**
 * Path the daemon writes its 127.0.0.1 port number to. The app cannot read it
 * directly (untrusted_app has no SELinux open on shell_data_file), so it asks
 * the open ADB session — which runs as shell UID — to `cat` it.
 *
 * The file lives for the whole life of the daemon and is deleted only when the
 * daemon exits, so its presence means "a daemon was here". That is NOT the same
 * as "a daemon is here": a daemon killed with SIGKILL never gets to delete it
 * and the file keeps advertising a dead port. Anything that trusts this file
 * must pair it with `pidof aimbot_shell` — see [liveDaemonPort].
 */
private const val DAEMON_PORT_FILE = "/data/local/tmp/aimbot_shell.port"

/**
 * Where the daemon's stdout+stderr goes.
 *
 * It used to be /dev/null, which threw away the only evidence there is when
 * the launch itself gets lost — and that is precisely the failure we most need
 * to see. Read back and surfaced in the error message when no port appears.
 */
private const val DAEMON_LOG_FILE = "/data/local/tmp/aimbot_daemon.log"

/**
 * How long to wait for a launched daemon to publish its port, and how often to
 * re-issue the launch while waiting.
 *
 * The launch is re-issued because it is the step that occasionally gets lost
 * outright: an `app_process` started over the wireless ADB session sometimes
 * never reaches main() at all — no log line, no tombstone, no process. Waiting
 * harder does not help a launch that never happened; launching again does.
 */
private const val kPublishWindowMs = 8_000L
private const val kRelaunchIntervalMs = 1_500L

/**
 * Appended to every launch command: the shell that spawns the daemon must stay
 * alive for a moment after spawning it.
 *
 * Measured on the device, this is the whole difference between a daemon that
 * lives and one that vanishes:
 *
 *   adb shell "setsid sleep 300 &"              → sleep is dead a second later
 *   adb shell "setsid sleep 300 & sleep 4"      → sleep survives, and outlives
 *                                                 the shell that started it
 *
 * The daemon itself is no different — foregrounded it starts perfectly, and
 * `setsid` does not save it. What kills it is a race: when the shell that ran
 * the command exits, adbd tears that session down and takes the just-spawned
 * background process with it, before `setsid` has moved it out of the way. Give
 * the shell a few seconds and the race is over — the daemon is established and
 * ignores the teardown entirely.
 *
 * That is why launches here are not "fire and forget": without this, roughly
 * nine launches in ten produced no process at all, no error, no tombstone.
 */
private const val kLaunchLinger = "sleep 3"

/**
 * System library directories handed to the daemon as `java.library.path`.
 *
 * The daemon's native code dlopens vendor libraries (MediaTek's apuware stack,
 * Qualcomm's DSP RPC …) from inside the JVM's classloader namespace, `clns-1`.
 * That namespace is isolated: an *absolute* path has to pass its permitted-path
 * check, and /system/lib64, /system_ext/lib64 and /vendor/lib64 are not on it,
 * so every such dlopen was refused.
 *
 * A bare soname takes a different route — the linker resolves it against the
 * namespace's *search* paths, and a hit is loaded without the permitted-path
 * check. Those search paths are the APK's lib dir plus java.library.path, so
 * listing the system directories here is what makes vendor libraries reachable.
 *
 * `/apex/com.android.runtime/lib64/bionic` is the one that is easy to miss:
 * /system/lib64/libdl_android.so is a symlink into it, libdl_android.so is a
 * dependency of the whole VNDK/HIDL stack, and the apex is not permitted
 * either. Measured on a OnePlus OPD2404 (Android 15): with these directories,
 * libcdsprpc.so, libvndksupport.so and android.hidl.base@1.0.so all load;
 * dropping /system/lib64 from the list breaks ld-android.so and fails again.
 */
private const val kSystemLibraryPath =
    "/apex/com.android.runtime/lib64/bionic:/system/lib64:/system_ext/lib64:" +
            "/vendor/lib64:/vendor/lib64/hw:/vendor/lib64/egl:/odm/lib64:/product/lib64"

/**
 * Killing by pid, not by `pkill -f aimbot_shell`.
 *
 * `pkill -f` matches on the whole command line — and the shell running the
 * pkill command has `aimbot_shell` in *its* command line too, so it signals
 * itself and dies before finishing. Which is how a "kill the daemon" step ends
 * up killing nothing but its own shell, and how a stray `rm -f` appended after
 * it silently never runs. `pidof` matches the process *name*, which only the
 * daemon has.
 */
private const val kKillDaemon = "for p in \$(pidof aimbot_shell); do kill \$p; done"
private const val kKillDaemonHard = "for p in \$(pidof aimbot_shell); do kill -9 \$p; done"

/**
 * A one-line launcher the app writes once per attempt, so the launch itself
 * travels as `sh /data/local/tmp/aimbot_launch.sh` instead of a ~250-character
 * command line.
 *
 * Two reasons. First, the long form carries the APK path, the native lib dir
 * and a class name in the ADB message payload; the short form carries none of
 * it, so nothing about the command can be mangled in transit. Second, the
 * script is the single place the launch exists, which makes the device side of
 * a failed launch inspectable by hand (`sh /data/local/tmp/aimbot_launch.sh`).
 */
private const val DAEMON_LAUNCH_SCRIPT = "/data/local/tmp/aimbot_launch.sh"

class ShellManager(private val context: Context) {

    /** Physical touch phase, matching AimbotNg's touch action constants. */
    enum class TouchPhase(val wireName: String, val action: Int) {
        DOWN("D", 0),
        MOVE("M", 1),
        UP("U", 2)
    }

    enum class ShellState {
        IDLE, DISCOVERING, CONNECTING, RUNNING, ERROR
    }

    data class ShellStatus(
        val state: ShellState = ShellState.IDLE,
        val message: String = "",
        val adbPort: Int = -1,
        val isPairing: Boolean = false
    )

    private val _listeners = CopyOnWriteArrayList<(ShellStatus) -> Unit>()
    private val _outputListeners = CopyOnWriteArrayList<(String) -> Unit>()
    private val _touchListeners = CopyOnWriteArrayList<(TouchPhase, Int, Int) -> Unit>()
    private val _outputLines = CopyOnWriteArrayList<String>()
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private var adbKey: AdbKey = AdbKey(context)
    @Volatile var currentStatus = ShellStatus()
        private set
    private var connectJob: Job? = null
    private var shellJob: Job? = null
    private var activeAdbClient: AdbClient? = null
    private var daemonSocket: Socket? = null
    private var daemonIn: BufferedReader? = null
    private var daemonOut: BufferedWriter? = null
    private var daemonReaderThread: Thread? = null
    /** Port the daemon was last reached on. Reused verbatim by a reconnect:
     *  the daemon keeps listening on it for its whole life now, so a dropped
     *  session does not need the whole launch dance again. */
    private var daemonPort: Int = -1
    /** Set the moment stop() is requested so a late-completing startShellServer
     *  coroutine never reports RUNNING after we have already stopped. */
    private var stopRequested = false

    /** Survives process death: "user pressed 启动 once, expects it to come back". */
    private val prefs: SharedPreferences =
        context.getSharedPreferences("aimbot_shell_prefs", Context.MODE_PRIVATE)

    /** Last port the daemon connected through; reported in error status only. */
    private var lastAdbPort: Int = -1

    /** Pushes the app into the power-save temp whitelist every 25s while the
     *  daemon is up. Belt to the foreground-service braces: even if some OEM
     *  freezes the service notification, the whitelist call keeps the TCP
     *  socket out of doze for at least 30s. */
    private var whitelistJob: Job? = null

    /** Watches the panel for a rotation; see [startDisplayWatcher]. */
    private var displayListener: DisplayManager.DisplayListener? = null

    private val geometryHandler = Handler(Looper.getMainLooper())

    /**
     * True only while the daemon has CONFIRMED that it really holds the touch
     * panel exclusively (EVIOCGRAB) — i.e. the last `OK:grabbed=` said 1.
     *
     * Purely informational now. It used to drive an interlock in the overlay
     * service, which kept a full-screen TOUCHABLE window alive (to dodge the
     * platform's 0.8 alpha cap) only while the grab was provably held — because
     * such a window would otherwise swallow every touch on the device. That whole
     * problem is gone: the menu is a shell-owned SurfaceFlinger layer with no
     * input channel, so it can neither be alpha-capped nor steal a touch. The flag
     * is kept because it is a genuinely useful diagnostic (a grab can be lost
     * silently when the device re-enumerates the panel).
     *
     * Deliberately NOT "the daemon process is alive". Cleared on CLOSE, on stream
     * loss and on stop().
     */
    @Volatile
    var panelGrabbed: Boolean = false
        private set

    val outputLines: List<String> get() = _outputLines.toList()

    fun addListener(listener: (ShellStatus) -> Unit) {
        _listeners.add(listener)
    }

    fun removeListener(listener: (ShellStatus) -> Unit) {
        _listeners.remove(listener)
    }

    fun addOutputListener(listener: (String) -> Unit) {
        _outputListeners.add(listener)
    }

    /** Physical touches decoded by the daemon (screen pixels). */
    fun addTouchListener(listener: (TouchPhase, Int, Int) -> Unit) {
        _touchListeners.add(listener)
    }

    fun removeTouchListener(listener: (TouchPhase, Int, Int) -> Unit) {
        _touchListeners.remove(listener)
    }

    /**
     * User-initiated start. This — and only this — clears a previous stop().
     *
     * The autonomous paths (reconnect → relaunch) must NOT clear it. Doing so
     * is what made 停止 look broken: the user pressed stop, the reconnect loop
     * decided to relaunch, the relaunch cleared the flag, and the whole stack
     * came back to life by itself — after which pressing stop again just
     * repeated the cycle.
     */
    fun startDiscovery() {
        stopRequested = false
        beginDiscovery()
    }

    /** The mDNS scan itself, with no opinion about the stop flag. */
    private fun beginDiscovery() {
        Log.i(TAG, "startDiscovery: scanning for wireless ADB port (mDNS)")
        updateStatus(ShellStatus(state = ShellState.DISCOVERING, message = "正在发现 ADB 端口..."))
        var mdnsRef: AdbMdns? = null
        val mdns = AdbMdns(context, AdbMdns.TLS_CONNECT, onPortFound = { port ->
            Log.i(TAG, "mDNS resolved ADB port=$port")
            updateStatus(ShellStatus(state = ShellState.DISCOVERING, message = "发现 ADB 端口: $port", adbPort = port))
            mdnsRef?.stop()
            connectToAdb(port)
        }, onServiceLost = {
            Log.w(TAG, "mDNS service lost (wireless ADB turned off?)")
            updateStatus(ShellStatus(state = ShellState.ERROR, message = "ADB 服务丢失"))
        })
        mdnsRef = mdns
        mdns.start()
    }

    /**
     * Idempotent entry point used by the auto-launch paths (boot receiver,
     * Activity "启动" tap, AppBar shortcut). Only acts when the manager is
     * sitting idle or has surfaced an error — a live session stays untouched.
     */
    fun startIfIdle() {
        Log.i(TAG, "startIfIdle: current state=${currentStatus.state}")
        if (currentStatus.state == ShellState.IDLE ||
            currentStatus.state == ShellState.ERROR) {
            startDiscovery()
        }
    }

    fun connectToAdb(port: Int) {
        Log.i(TAG, "connectToAdb port=$port")
        connectJob?.cancel()
        lastAdbPort = port
        connectJob = scope.launch {
            updateStatus(ShellStatus(state = ShellState.CONNECTING, message = "正在连接 ADB (端口 $port)...", adbPort = port))
            try {
                val adbClient = AdbClient("127.0.0.1", port, adbKey)
                adbClient.connect()
                activeAdbClient = adbClient
                Log.i(TAG, "ADB TCP connected to 127.0.0.1:$port")
                appendOutput("ADB 连接成功")
                updateStatus(ShellStatus(state = ShellState.CONNECTING, message = "正在启动 Shell 服务器...", adbPort = port))
                startShellServer(adbClient, port)
            } catch (e: Exception) {
                Log.e(TAG, "ADB connection to 127.0.0.1:$port failed", e)
                appendOutput("连接失败: ${e.message}")
                updateStatus(ShellStatus(state = ShellState.ERROR, message = "连接失败: ${e.message}", adbPort = port))
            }
        }
    }

    /**
     * Launches the privileged daemon and connects to it directly. ADB is used
     * once, briefly, for the launch itself — runtime communication never goes
     * through adbd (Shizuku-style).
     *
     * Architecture:
     *   1. mDNS discovers the wireless ADB port → opens an ADB TLS connection
     *   2. `adb shell "(app_process ... ShellServerEntry ...)&` backgrounds
     *      the daemon via a subshell + `&`; the shell returns immediately
     *   3. The daemon binds a 127.0.0.1 TCP port in its own (uid 2000) process
     *      and writes the port number to [DAEMON_PORT_FILE] for the app to find
     *   4. The app reads the port, connects via [Socket] on loopback
     *   5. All subsequent communication flows over that socket — adb is gone
     *
     * Stable because the daemon survives `adb shell` exit (adbd only SIGHUPs
     * the foreground shell, not its backgrounded children), and the
     * app↔daemon link is a loopback TCP socket that does not traverse NAT
     * (so the OEM idle timeouts that plague WiFi ADB do not apply here).
     *
     * Why loopback TCP and not `LocalSocket("@name")`: Android's SELinux
     * policy blocks `untrusted_app` from connecting to abstract-domain sockets
     * owned by `shell`. The daemon ends up bound, but the app's `connect()`
     * gets `Connection refused` — verified on stock AOSP and OPPO ColorOS.
     * `/data/local/tmp/` is also off-limits for the app UID (drwxrwx--x
     * shell shell), so we cannot put a filesystem-domain socket there either.
     * 127.0.0.1 has neither problem.
     */
    private fun startShellServer(adbClient: AdbClient, port: Int) {
        shellJob = scope.launch {
            try {
                if (stopRequested) return@launch
                val info = context.applicationInfo
                val apkPath = info.sourceDir
                val libDir = info.nativeLibraryDir

                // Rebindable: a desynced ADB stream is unrecoverable (see
                // adbCommandFailed), so the loop below may replace this with a
                // freshly dialled connection rather than retrying into a broken
                // one until the deadline expires.
                var adb = adbClient

                // ── 1. Is a daemon already up? ───────────────────────────────
                //
                // The port file lives for the whole life of the daemon (it is
                // deleted only at exit), so it is a reliable handshake — but
                // only if we also prove a daemon process exists. A daemon
                // killed with SIGKILL leaves the file behind pointing at a dead
                // port, and the app cannot tell the difference on its own
                // (untrusted_app has no SELinux open on shell_data_file, so
                // this goes through ADB, which runs as shell UID).
                val existing = liveDaemonPort(adb)
                if (existing != null) {
                    Log.i(TAG, "a daemon is already running on $existing — attaching instead of launching")
                    appendOutput("守护进程已在运行 ($existing)，直接连接")
                    if (connectToDaemon(existing, port)) return@launch
                    Log.w(TAG, "the daemon on $existing did not answer — treating it as gone")
                } else if (portFilePresent(adb)) {
                    // Stale file from a daemon that was killed. Left in place it
                    // poisons the poll below: we would read the dead port, latch
                    // onto it, and spend the whole window failing to connect to
                    // it while the fresh daemon publishes a different one. That
                    // is one of the two ways "press 启动 five times" happened.
                    adbShellText(adb, "rm -f $DAEMON_PORT_FILE")
                    Log.i(TAG, "removed a stale $DAEMON_PORT_FILE")
                }

                // ── 2. Launch, and keep launching until a port appears ──────
                //
                // Re-issuing is safe: a daemon that finds the port file already
                // served by a live process exits immediately (see
                // ShellServerEntry.alreadyRunningPort()), so this can never
                // produce two daemons fighting over the touch panel.
                //
                // `setsid (...) &` puts the daemon in its own session AND
                // backgrounds it. The session move is the load-bearing part:
                // without it, the daemon is in the adb shell's process group,
                // and on OPPO ColorOS the adb shell's exit kills every process
                // in that group within a second — even `nohup` does not help,
                // because it is not SIGHUP that kills them but the cgroup
                // cleanup. `setsid` detaches the daemon from that group, so it
                // survives the shell exit cleanly and outlasts the adb session.
                // Its output goes to DAEMON_LOG_FILE rather than /dev/null, so a
                // launch that fails leaves evidence behind instead of nothing.
                val cmd = "(setsid /system/bin/app_process -Djava.class.path='$apkPath' -Djava.library.path='$kSystemLibraryPath' /system/bin " +
                        "--nice-name=aimbot_shell " +
                        "io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry '$libDir' " +
                        ">$DAEMON_LOG_FILE 2>&1) & " + kLaunchLinger

                // Same launch, as a script on the device. Written once here;
                // re-run below as a three-word command. If the write does not
                // land we fall back to the long inline form, so this is an
                // optimisation, never a new failure mode.
                //
                // The trailing `sleep` is the load-bearing part — see
                // kLaunchLinger. It has to live inside the payload that the
                // launching shell runs, which is why it is part of both forms.
                val scriptLine = "setsid /system/bin/app_process " +
                        "-Djava.class.path='$apkPath' -Djava.library.path='$kSystemLibraryPath' /system/bin " +
                        "--nice-name=aimbot_shell " +
                        "io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry '$libDir' " +
                        ">$DAEMON_LOG_FILE 2>&1 &\n" + kLaunchLinger
                adbShellText(adb, "printf '%s\\n' \"$scriptLine\" > $DAEMON_LAUNCH_SCRIPT")
                val scriptReady =
                    adbShellText(adb, "test -s $DAEMON_LAUNCH_SCRIPT && echo yes").contains("yes")
                val launchCmd = if (scriptReady) "sh $DAEMON_LAUNCH_SCRIPT" else cmd
                Log.i(TAG, "launch command: " +
                        (if (scriptReady) "sh $DAEMON_LAUNCH_SCRIPT" else "inline (script write failed)"))

                // ── The launch goes over a persistent `sh` session ──────────
                //
                // Deliberately not `adb shell "<command>"`. That one-shot path —
                // a ~250-character command packed into the OPEN payload of a
                // stream that is then closed — proved unreliable on this device:
                // across three rounds of logs, launches issued through it
                // frequently never executed at all, with no error anywhere (no
                // AdbException, nothing from adbd, no `app_process`, no
                // tombstone), while the identical command typed into an adb
                // shell always worked.
                //
                // A session is a different path end to end. The OPEN payload is
                // the two characters "sh"; the command then travels as ordinary
                // stdin (WRTE), which adbd acknowledges with OKAY — traffic the
                // session reader has always handled correctly. And the session
                // stays open for the whole attempt, so there is no "close a
                // socket that still has unread data" window either, which is a
                // known way to lose unread bytes to a RST.
                val sessionClient = freshAdbClient(port)
                var launcher: AdbClient.ShellSession? = null
                if (sessionClient != null) {
                    launcher = try {
                        sessionClient.openShellSession("sh", onData = {}, onClosed = {})
                    } catch (t: Throwable) {
                        Log.w(TAG, "could not open a shell session for the launch: " +
                                "${t.javaClass.simpleName}: ${t.message}")
                        null
                    }
                }
                Log.i(TAG, "launch channel: " +
                        if (launcher != null) "persistent sh session" else "one-shot adb shell")

                var lastLaunchAt = 0L
                var publishedPort: Int? = null
                val publishDeadline = System.currentTimeMillis() + kPublishWindowMs
                while (System.currentTimeMillis() < publishDeadline) {
                    if (stopRequested) return@launch

                    // Anything that comes back as a failure means the stream is
                    // out of phase, and no amount of retrying fixes that — throw
                    // the connection away and dial a new one.
                    if (adbCommandFailed) {
                        adbCommandFailed = false
                        val fresh = freshAdbClient(port)
                        if (fresh != null) {
                            Log.w(TAG, "ADB stream desynced — re-dialled ADB, continuing")
                            runCatching { adb.close() }
                            adb = fresh
                            activeAdbClient = fresh
                        }
                    }

                    if (lastLaunchAt == 0L ||
                        System.currentTimeMillis() - lastLaunchAt >= kRelaunchIntervalMs) {
                        if (lastLaunchAt == 0L) Log.i(TAG, "launching daemon")
                        // Re-issuing is safe: a daemon that finds the port file
                        // already served by a live process exits at once.
                        val s = launcher
                        if (s != null && !s.isClosed) s.writeLine(launchCmd)
                        else adbShellText(adb, launchCmd)
                        lastLaunchAt = System.currentTimeMillis()
                    }
                    val p = readPortFile(adb)
                    if (p != null) { publishedPort = p; break }
                    try { delay(150L) } catch (_: CancellationException) { return@launch }
                }

                // The launch channel has done its job either way.
                runCatching { launcher?.close() }
                runCatching { sessionClient?.close() }

                if (publishedPort == null) {
                    val why = adbShellText(adb, "tail -n 15 $DAEMON_LOG_FILE 2>/dev/null")
                    // `pidof` separates the two halves of this failure, which
                    // look identical from the outside: "the daemon never ran"
                    // and "the daemon ran but we cannot see its port".
                    val alive = adbShellText(adb, "pidof aimbot_shell")
                    Log.e(TAG, "daemon never published a port within ${kPublishWindowMs}ms; " +
                               "aimbot_shell=${alive.ifBlank { "(none)" }}, log: ${why.ifBlank { "(empty)" }}")
                    appendOutput("启动失败: 守护进程未在 ${kPublishWindowMs / 1000}s 内公布端口")
                    appendOutput("守护进程: ${if (alive.isBlank()) "未启动" else "在跑 (pid=$alive)"}")
                    if (why.isNotBlank()) appendOutput(why)
                    updateStatus(ShellStatus(
                        state = ShellState.ERROR,
                        message = "启动失败: 守护进程未公布端口",
                        adbPort = port
                    ))
                    return@launch
                }
                Log.i(TAG, "daemon published port=$publishedPort")

                connectToDaemon(publishedPort, port)
            } catch (e: Exception) {
                if (stopRequested) return@launch
                Log.e(TAG, "Shell server start failed", e)
                appendOutput("启动失败: ${e.message}")
                updateStatus(ShellStatus(state = ShellState.ERROR, message = "启动失败: ${e.message}"))
            }
        }
    }

    /**
     * Reads lines from the daemon's TCP socket and dispatches them.
     * Replaces the old ADB session's onData callback.
     */
    private fun startDaemonReader() {
        daemonReaderThread = Thread({
            Log.i(TAG, "daemon reader thread started")
            try {
                while (!stopRequested) {
                    val line = daemonIn?.readLine() ?: break
                    if (line.isBlank()) continue
                    handleDaemonLine(line.trim())
                }
            } catch (t: Throwable) {
                if (!stopRequested) {
                    Log.w(TAG, "daemon reader ended: ${t.javaClass.simpleName}: ${t.message}")
                }
            }
            // The link dropped. This is expected rather than exceptional: the
            // app sits behind a game, so the OS freezes it and the daemon's
            // liveness probe eventually drops the session. The daemon itself
            // stays up, so the fix is to reconnect — not to start over.
            if (!stopRequested) {
                Log.w(TAG, "direct TCP connection lost — reconnecting to 127.0.0.1:$daemonPort")
                appendOutput("连接断开，正在重连守护进程…")
                scheduleReconnect()
            }
            Log.i(TAG, "daemon reader thread stopped")
        }, "aimbot-daemon-reader")
        daemonReaderThread?.isDaemon = true
        daemonReaderThread?.start()
    }

    // ── Reconnect ──────────────────────────────────────────────────────────
    //
    // The daemon outlives the connection now, so a dropped session is cheap to
    // recover from: dial the same loopback port again and carry on. That
    // matters most right after a freeze — the app thaws, this coroutine
    // finally gets to run, and the session is rebuilt without the user
    // touching anything.
    //
    // Only if the port stays dead do we fall back to launching a new daemon,
    // and even then it is tried a bounded number of times. An unbounded
    // "relaunch → drop → relaunch" loop is its own outage.

    private var reconnectJob: Job? = null

    /** How long to keep dialling before deciding the daemon itself is gone. */
    private val kReconnectWindowMs = 60_000L
    private val kReconnectIntervalMs = 3_000L

    /** Full relaunches allowed before we give up and stay in ERROR. */
    private val kMaxFullRestarts = 3
    private var fullRestarts = 0

    private fun scheduleReconnect() {
        if (stopRequested) return
        if (daemonPort <= 0) {
            Log.w(TAG, "no known daemon port — cannot reconnect")
            reportDisconnected()
            return
        }
        reconnectJob?.cancel()
        reconnectJob = scope.launch {
            updateStatus(ShellStatus(
                state = ShellState.CONNECTING,
                message = "正在重连守护进程 (127.0.0.1:$daemonPort)…",
                adbPort = lastAdbPort
            ))
            val deadline = System.currentTimeMillis() + kReconnectWindowMs
            while (isActive && !stopRequested && System.currentTimeMillis() < deadline) {
                delay(kReconnectIntervalMs)
                if (stopRequested) return@launch

                // A daemon that was killed leaves its port file behind, so
                // dialling that port forever is pointless. Ask the device
                // whether the process still exists and abandon the port
                // immediately if it does not — otherwise a killed daemon costs
                // a full minute of retries per relaunch round, and the whole
                // thing looks like it is hung.
                val alive = activeAdbClient?.let { adbShellText(it, "pidof aimbot_shell") }.orEmpty()
                if (alive.isBlank()) {
                    Log.w(TAG, "no aimbot_shell process — daemon is gone, not merely unreachable")
                    break
                }

                val sock = try {
                    Socket("127.0.0.1", daemonPort).apply {
                        soTimeout = 0
                        tcpNoDelay = true
                    }
                } catch (t: Throwable) {
                    Log.v(TAG, "reconnect attempt failed: ${t.javaClass.simpleName}")
                    continue
                }
                onReconnected(sock)
                return@launch
            }
            if (stopRequested) return@launch
            Log.w(TAG, "daemon port stayed dead for ${kReconnectWindowMs / 1000}s — " +
                    "the daemon process is probably gone")
            maybeRelaunchDaemon()
        }
    }

    /** Wires a freshly connected socket back up, without a second OPEN. */
    private fun onReconnected(sock: Socket) {
        fullRestarts = 0
        daemonSocket = sock
        daemonIn = BufferedReader(InputStreamReader(sock.getInputStream(), StandardCharsets.UTF_8))
        daemonOut = BufferedWriter(OutputStreamWriter(sock.getOutputStream(), StandardCharsets.UTF_8))
        Log.i(TAG, "reconnected to daemon on 127.0.0.1:$daemonPort")
        updateStatus(ShellStatus(
            state = ShellState.RUNNING,
            message = "已重新连接守护进程 (127.0.0.1:$daemonPort)",
            adbPort = lastAdbPort
        ))
        appendOutput("已重新连接守护进程")
        startDaemonReader()
        // Geometry only. OPEN is deliberately not repeated: the daemon kept
        // the grab and the menu across the gap, and re-running OPEN would
        // needlessly tear the panel down and take it again.
        pushGeometry()
    }

    private fun maybeRelaunchDaemon() {
        if (stopRequested) return
        if (fullRestarts >= kMaxFullRestarts) {
            Log.w(TAG, "gave up after $fullRestarts relaunch attempts — staying disconnected")
            reportDisconnected()
            return
        }
        fullRestarts++
        Log.i(TAG, "relaunching the daemon (attempt $fullRestarts of $kMaxFullRestarts)")
        appendOutput("守护进程无响应，尝试重启 ($fullRestarts/$kMaxFullRestarts)")
        runCatching {
            daemonSocket?.close()
            daemonIn?.close()
            daemonOut?.close()
        }
        daemonSocket = null
        daemonIn = null
        daemonOut = null
        if (AdbPairingState.isPaired(context)) {
            beginDiscovery()
        } else {
            Log.w(TAG, "not paired — cannot relaunch the daemon")
            reportDisconnected()
        }
    }

    private fun reportDisconnected() {
        appendOutput("Shell 守护进程已断开")
        updateStatus(ShellStatus(
            state = ShellState.ERROR,
            message = "Shell 守护进程已断开",
            adbPort = lastAdbPort
        ))
    }

    /** Hands the daemon its display geometry, then tells it to grab + read. */
    private fun sendInitCommands() {
        val (w, h, rotation) = displayGeometry()
        sendCommand("SET_RESOLUTION $w $h $rotation")
        sendCommand("OPEN")
    }

    // ── Display geometry -> daemon ───────────────────────────────────────────

    /** Coalesces a burst of display callbacks into one geometry push. */
    private val geometryPush = Runnable { pushGeometry() }

    /**
     * Pushes the panel geometry to the daemon whenever the device rotates.
     *
     * The daemon reads the display for itself these days (`SysDisplay`, polled
     * by its geometry watcher) and *prefers* its own reading, so this is the
     * fallback — for a build where that read fails, and for the moment of
     * connect, so the first layer is not built a poll late.
     *
     * It is deliberately not the primary source. This process is an app: it is
     * not guaranteed to be told about a rotation while it is not in front, and
     * once cached it can be frozen and told nothing at all — which is how the
     * menu ended up built for the previous orientation and left there.
     *
     * The menu's layer bakes its buffer size in when it is built, and both the
     * Vulkan swapchain and ImGui's DisplaySize are taken from that buffer, so a
     * rotation has to reach the daemon one way or another. `SET_RESOLUTION` is
     * what makes it rebuild the layer at the new shape.
     */
    private fun startDisplayWatcher() {
        if (displayListener != null) return
        val dm = context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager
        if (dm == null) {
            Log.w(TAG, "no DisplayManager — the menu will not follow a rotation")
            return
        }
        val listener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(displayId: Int) = Unit
            override fun onDisplayRemoved(displayId: Int) = Unit
            override fun onDisplayChanged(displayId: Int) {
                if (displayId != Display.DEFAULT_DISPLAY) return
                // This fires once per rotation, and it can land before the new
                // DisplayInfo is visible to getRealSize — so wait a beat and
                // coalesce instead of sampling the middle of the change.
                geometryHandler.removeCallbacks(geometryPush)
                geometryHandler.postDelayed(geometryPush, 250L)
            }
        }
        // The Handler is mandatory, not a style choice: this runs on the IO
        // scope, which has no Looper, and DisplayManager registers a null one
        // with `new Handler()` on the calling thread — which throws there.
        dm.registerDisplayListener(listener, geometryHandler)
        displayListener = listener
    }

    private fun stopDisplayWatcher() {
        val listener = displayListener ?: return
        displayListener = null
        geometryHandler.removeCallbacks(geometryPush)
        runCatching {
            (context.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager)
                ?.unregisterDisplayListener(listener)
        }
    }

    /**
     * Re-sends the panel geometry. The daemon re-lays-out a running menu only if
     * the numbers actually changed, so a duplicate push is harmless.
     */
    private fun pushGeometry() {
        val (w, h, rotation) = displayGeometry()
        Log.i(TAG, "display changed -> ${w}x$h rot=$rotation")
        sendCommand("SET_RESOLUTION $w $h $rotation")
    }

    // ── Daemon -> app stream ─────────────────────────────────────────────────

    private fun handleDaemonLine(line: String) {
        when {
            line == "READY" -> appendOutput("守护进程就绪 (UID 2000)")
            // The daemon's liveness probe. Answer it so the session is not
            // dropped while this process is alive but not sending anything.
            line == "PING?" -> sendCommand("PONG")
            // Where the daemon stands after a reconnect: its menu and grab
            // outlived the gap, so adopt them instead of assuming a fresh start.
            line.startsWith("STATE ") -> handleStateLine(line.substring(6))
            line.startsWith("OK:") -> handleOkPayload(line.substring(3))
            line == "OK" -> Unit
            line.startsWith("ERR:") -> appendOutput("守护进程错误: ${line.substring(4)}")
            line.startsWith("TOUCH ") -> handleTouchLine(line)
            line == "BYE" -> handleDaemonBye()
            else -> appendOutput(line)
        }
    }

    /**
     * The daemon quit because the user asked it to from inside the menu
     * (Settings → 退出并恢复触摸), and it said so before going.
     *
     * That distinction is the whole point. To us a daemon exit normally looks
     * like one thing only — the socket dropped — which is indistinguishable
     * from a crash, and the answer to a crash is [scheduleReconnect] plus
     * [maybeRelaunchDaemon]. Left alone those would bring the daemon back and
     * take the panel again seconds after the user pressed the button whose
     * entire purpose was to get the panel released, leaving them with the same
     * frozen input and no second chance: it would look like the button lied.
     *
     * So this does what [stop] does to *our* state and nothing to the daemon —
     * there is nothing left to talk to. Deliberately not [stop]: that writes
     * DESTROY down the socket from inside the reader loop that owns it.
     */
    private fun handleDaemonBye() {
        Log.i(TAG, "daemon sent BYE — menu-requested exit, staying down")
        stopRequested = true
        clearWasRunning()
        reconnectJob?.cancel()
        reconnectJob = null
        fullRestarts = 0
        stopWhitelistLoop()
        stopHeartbeat()
        stopDisplayWatcher()
        panelGrabbed = false
        appendOutput("守护进程已退出，触摸已恢复")
        updateStatus(ShellStatus(state = ShellState.IDLE, message = "已停止"))
        // The sockets are closing anyway: this line arrived on the reader
        // thread, whose loop is about to find EOF and run its own cleanup.
    }

    /**
     * `OK:<payload>` replies. `grabbed=` is consumed silently (it is polled about
     * once a second, so logging each one would bury the log); the rest is shown
     * as before.
     */
    private fun handleOkPayload(payload: String) {
        if (payload.startsWith("grabbed=")) {
            // Tracked for diagnostics only — the grab state is polled about once
            // a second and announcing every transition just fills the log.
            payload.removePrefix("grabbed=").toIntOrNull()?.let { panelGrabbed = it != 0 }
            return
        }
        appendOutput("OK $payload")
    }

    /**
     * Adopts the daemon's live state after a reconnect (`STATE ui=… grabbed=…`).
     *
     * Without this the app would come back from a freeze believing the panel
     * was idle and the menu closed, while the daemon had been holding both the
     * whole time.
     */
    private fun handleStateLine(payload: String) {
        val ui = payload.split(' ').firstOrNull { it.startsWith("ui=") }
            ?.removePrefix("ui=")?.toIntOrNull()
        val grabbed = payload.split(' ').firstOrNull { it.startsWith("grabbed=") }
            ?.removePrefix("grabbed=")?.toIntOrNull()
        if (grabbed != null) {
            // State only — same reasoning as handleOkPayload(): the transition
            // itself is not worth a line in the output panel.
            panelGrabbed = grabbed != 0
        }
        Log.i(TAG, "daemon state: ui=$ui grabbed=$grabbed")
    }

    private fun handleTouchLine(line: String) {
        val parts = line.split(' ')
        if (parts.size < 4) return
        val phase = when (parts[1]) {
            "D" -> TouchPhase.DOWN
            "M" -> TouchPhase.MOVE
            "U" -> TouchPhase.UP
            else -> return
        }
        val x = parts[2].toIntOrNull() ?: return
        val y = parts[3].toIntOrNull() ?: return
        _touchListeners.forEach { it(phase, x, y) }
    }

    /**
     * Sends one protocol line to the daemon via the direct TCP socket.
     *
     * Safe to call from any thread: writing to the TCP socket is blocking
     * network I/O, so it is always dispatched onto the manager's IO scope.
     */
    fun sendCommand(command: String) {
        scope.launch {
            try {
                val out = daemonOut ?: return@launch
                synchronized(out) {
                    out.write(command)
                    out.write('\n'.code)
                    out.flush()
                }
            } catch (t: Throwable) {
                // A BufferedWriter throws here where the old PrintStream
                // swallowed it, so a broken link is actually noticed — the
                // daemon drops us, and the reader thread's reconnect takes it
                // from there.
                Log.w(TAG, "sendCommand failed: ${t.javaClass.simpleName}: ${t.message}")
            }
        }
    }

    fun pair(port: Int, pairCode: String) {
        updateStatus(ShellStatus(state = ShellState.CONNECTING, message = "正在配对...", adbPort = port, isPairing = true))
        scope.launch {
            try {
                val client = AdbPairingClient("127.0.0.1", port, pairCode, adbKey)
                val success = client.start()
                client.close()
                if (success) {
                    appendOutput("配对成功")
                    updateStatus(ShellStatus(state = ShellState.IDLE, message = "配对成功，正在连接...", adbPort = port))
                    delay(500)
                    connectToAdb(port)
                } else {
                    appendOutput("配对失败")
                    updateStatus(ShellStatus(state = ShellState.ERROR, message = "配对失败", adbPort = port))
                }
            } catch (e: Exception) {
                appendOutput("配对失败: ${e.message}")
                updateStatus(ShellStatus(state = ShellState.ERROR, message = "配对失败: ${e.message}", adbPort = port))
            }
        }
    }

    fun stop() {
        Log.i(TAG, "stop() called (wasRunning=${prefs.getBoolean(KEY_WAS_RUNNING, false)})")

        // Both captured BEFORE the teardown below.
        //
        // The old code read currentStatus.adbPort inside the coroutine that
        // does the killing — while updateStatus(IDLE) at the end of this very
        // function resets adbPort to -1, synchronously. The race therefore
        // usually went the wrong way and the whole pkill was skipped, which is
        // exactly why 停止 left a daemon behind.
        val adbPortForKill = lastAdbPort
        val outForDestroy = daemonOut

        stopRequested = true
        clearWasRunning()
        reconnectJob?.cancel()
        reconnectJob = null
        fullRestarts = 0
        stopWhitelistLoop()
        stopHeartbeat()
        stopDisplayWatcher()
        panelGrabbed = false

        // Cancel coroutines.
        connectJob?.cancel()
        shellJob?.cancel()

        // Ask the daemon to shut down on its own terms first: DESTROY makes it
        // hand the panel back and delete its port file before exiting. Done
        // synchronously and before the socket is closed — a few bytes to
        // loopback, and after the close there is nothing to write to.
        if (outForDestroy != null) {
            runCatching {
                synchronized(outForDestroy) {
                    outForDestroy.write("DESTROY")
                    outForDestroy.write('\n'.code)
                    outForDestroy.flush()
                }
                Log.i(TAG, "sent DESTROY to the daemon")
            }.onFailure { Log.w(TAG, "could not send DESTROY: ${it.message}") }
        }

        // Close the direct TCP connection to the daemon.
        // Close socket first — this unblocks daemonReaderThread's readLine()
        // so it releases the InputStreamReader lock that BufferedReader.close()
        // needs. Closing BufferedReader first would deadlock on that lock.
        runCatching { daemonSocket?.close() }
        runCatching { daemonIn?.close() }
        runCatching { daemonOut?.close() }
        daemonIn = null
        daemonOut = null
        daemonSocket = null
        daemonReaderThread = null

        // DESTROY is a request, not a guarantee — there may be no live socket
        // at all (app restarted, session already dropped) or a leftover daemon
        // from an earlier run. Verify and, if needed, kill over ADB.
        scope.launch {
            killResidualDaemon(adbPortForKill)
            runCatching { activeAdbClient?.close() }
            activeAdbClient = null
        }

        updateStatus(ShellStatus(state = ShellState.IDLE, message = "已停止"))
        appendOutput("Shell 服务已停止")
    }

    /**
     * Makes sure no daemon process survives this app.
     *
     * Killing is not optional. A live daemon holds the touch panel exclusively
     * (EVIOCGRAB), so an app that says "stopped" while a daemon lives on is a
     * device with a captured panel owned by a process nobody is talking to.
     *
     * The commands are deliberately short (a long one-shot command is the
     * unreliable case on this device — see startShellServer) and the result is
     * verified with `pidof` instead of assumed.
     */
    private suspend fun killResidualDaemon(adbPort: Int) {
        val client = (if (adbPort > 0) freshAdbClient(adbPort) else null)
            ?: activeAdbClient
            ?: run {
                Log.w(TAG, "no ADB connection — cannot confirm the daemon is gone; " +
                           "a leftover daemon would keep the panel grabbed")
                return
            }
        val owned = client !== activeAdbClient
        try {
            repeat(3) { attempt ->
                val alive = adbShellText(client, "pidof aimbot_shell").trim()
                if (alive.isEmpty()) {
                    adbShellText(client, "rm -f $DAEMON_PORT_FILE")
                    Log.i(TAG, "no daemon process left (checked ${attempt + 1}x)")
                    return
                }
                Log.w(TAG, "daemon still alive (pid=$alive) — killing, attempt ${attempt + 1}")
                adbShellText(client, kKillDaemon)
                try { delay(400L) } catch (_: CancellationException) { return }
            }
            // SIGTERM did not take. The panel grab is reason enough to force it.
            adbShellText(client, kKillDaemonHard)
            try { delay(400L) } catch (_: CancellationException) { return }
            val left = adbShellText(client, "pidof aimbot_shell").trim()
            adbShellText(client, "rm -f $DAEMON_PORT_FILE")
            if (left.isEmpty()) {
                Log.i(TAG, "daemon killed with SIGKILL")
            } else {
                Log.e(TAG, "daemon survived SIGKILL (pid=$left)")
                appendOutput("警告: 守护进程仍在运行 (pid=$left)")
            }
        } finally {
            if (owned) runCatching { client.close() }
        }
    }

    /** Current display size + rotation, for the panel coordinate mapping. */
    @Suppress("DEPRECATION")
    private fun displayGeometry(): Triple<Int, Int, Int> {
        return try {
            val wm = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
            val display = wm.defaultDisplay
            val size = Point()
            display.getRealSize(size)
            Triple(size.x, size.y, display.rotation)
        } catch (e: Exception) {
            Log.w(TAG, "displayGeometry failed, falling back to 1080x1920", e)
            Triple(1080, 1920, 0)
        }
    }

    /**
     * Polls the daemon's port file (via the open ADB session) until it shows
     * a parseable integer.
     *
     * The file is written *before* the daemon sits on `accept()` (see
     * [ShellServerEntry.main]) and then deleted the moment the first client
     * connects — so the only window in which it exists and is readable is the
     * one we care about. A 4s deadline matches the connect deadline below;
     * anything past that and the daemon is wedged.
     *
     * Why not read the file from the app directly: `/data/local/tmp/` is
     * owned by `shell:shell` with mode `drwxrwx--x`, and the daemon's file
     * ends up 0644. DAC-wise, that allows an `open(O_RDONLY)` by any other
     * UID (the directory's `--x` lets the path resolve). SELinux-wise, the
     * `untrusted_app` domain has no `open` permission on `shell_data_file`,
     * so the `open()` returns EACCES — verified on stock AOSP and OPPO
     * ColorOS.
     *
     * The ADB session is still open at this point and `shellCommand(...)`
     * runs as the shell UID, so a `cat` over ADB sidesteps the SELinux check
     * entirely. Each `shellCommand` call is a synchronous blocking read of a
     * full shell stream — so we poll with a tight deadline rather than
     * spinning on it.
     *
     * If the read comes back null we surface "启动失败: 守护进程未公布端口", and
     * the user retries. We do not silently fall back to a guessed port — that
     * would obscure the actual failure.
     */
    // ── Talking to the device over ADB ─────────────────────────────────────
    //
    // Everything here exists because the app cannot read /data/local/tmp
    // itself: its SELinux domain has no `open` on shell_data_file. The ADB
    // session runs as shell UID, so a shell command is the only way through.

    /** Runs `cmd` over ADB and returns its combined output, trimmed. */
    private suspend fun adbShellText(adbClient: AdbClient, cmd: String): String {
        val buf = StringBuilder()
        try {
            adbClient.shellCommand(cmd) { chunk -> buf.append(String(chunk, Charsets.UTF_8)) }
            adbCommandFailed = false
        } catch (t: Throwable) {
            adbCommandFailed = true
            Log.w(TAG, "adb shell failed [$cmd]: ${t.javaClass.simpleName}: ${t.message}")
        }
        return buf.toString().trim()
    }

    /**
     * Set when an ADB shell command throws.
     *
     * The ADB stream is strictly ordered, so one badly handled message leaves
     * every later command on that connection reading one message out of phase —
     * permanently, not just for the next call. Retrying on a desynced
     * connection can never succeed, so the launch loop watches this flag and
     * dials a fresh connection instead of spinning until the deadline.
     */
    private var adbCommandFailed = false

    /** A brand-new ADB connection, or null if it cannot be established. */
    private suspend fun freshAdbClient(port: Int): AdbClient? = try {
        AdbClient("127.0.0.1", port, adbKey).also { it.connect() }
    } catch (t: Throwable) {
        Log.w(TAG, "could not re-dial ADB on $port: ${t.javaClass.simpleName}: ${t.message}")
        null
    }

    /** One read of the port file. Null when it is absent, empty or unparseable. */
    private suspend fun readPortFile(adbClient: AdbClient): Int? {
        val text = adbShellText(adbClient, "cat $DAEMON_PORT_FILE 2>/dev/null")
        if (text.isEmpty()) return null
        val port = text.lineSequence().map { it.trim() }.firstOrNull { it.isNotEmpty() }?.toIntOrNull()
        if (port == null) {
            Log.w(TAG, "port file had unparseable content: \"$text\"")
            return null
        }
        return if (port in 1..65535) port else null
    }

    /** Whether the port file exists at all — including a stale one. */
    private suspend fun portFilePresent(adbClient: AdbClient): Boolean =
        adbShellText(adbClient, "test -f $DAEMON_PORT_FILE && echo yes").contains("yes")

    /**
     * The port of a daemon that is genuinely running, or null.
     *
     * The file alone is not enough: a daemon killed with SIGKILL never gets to
     * delete it, so the file keeps advertising a port nobody listens on. Hence
     * the `pidof` — a daemon process has to exist for the answer to count.
     */
    private suspend fun liveDaemonPort(adbClient: AdbClient): Int? {
        val text = adbShellText(
            adbClient,
            "P=\$(cat $DAEMON_PORT_FILE 2>/dev/null); " +
                "if [ -n \"\$P\" ] && pidof aimbot_shell >/dev/null 2>&1; then echo \$P; fi"
        )
        val port = text.lineSequence().map { it.trim() }.firstOrNull { it.isNotEmpty() }?.toIntOrNull()
            ?: return null
        return if (port in 1..65535) port else null
    }

    /**
     * Attaches to a daemon that is already listening and wires the session up.
     * Reports its own failure and returns false if the port never answers.
     */
    private suspend fun connectToDaemon(publishedPort: Int, adbPort: Int): Boolean {
        // The daemon may take a moment after writing the port to actually enter
        // accept(); a couple of seconds is normal on a busy device.
        var sock: Socket? = null
        val connectDeadline = System.currentTimeMillis() + 4_000L
        var lastError: Throwable? = null
        while (System.currentTimeMillis() < connectDeadline) {
            try {
                sock = Socket("127.0.0.1", publishedPort).apply {
                    soTimeout = 0
                    tcpNoDelay = true
                }
                break
            } catch (t: Throwable) {
                lastError = t
                if (stopRequested) return false
                try { delay(150L) } catch (_: CancellationException) { return false }
            }
        }
        if (sock == null) {
            val msg = lastError?.message ?: lastError?.javaClass?.simpleName ?: "unknown"
            Log.e(TAG, "could not connect to 127.0.0.1:$publishedPort within 4s: $msg")
            appendOutput("启动失败: 无法连接 daemon (4s) — $msg")
            updateStatus(ShellStatus(
                state = ShellState.ERROR,
                message = "启动失败: 无法连接 daemon",
                adbPort = adbPort
            ))
            return false
        }
        Log.i(TAG, "Socket connect succeeded")

        daemonPort = publishedPort
        daemonSocket = sock
        daemonIn = BufferedReader(InputStreamReader(sock.getInputStream(), StandardCharsets.UTF_8))
        daemonOut = BufferedWriter(OutputStreamWriter(sock.getOutputStream(), StandardCharsets.UTF_8))

        Log.i(TAG, "connected to daemon via 127.0.0.1:$publishedPort")
        updateStatus(ShellStatus(
            state = ShellState.RUNNING,
            message = "Shell 服务已就绪 (UID 2000, 127.0.0.1:$publishedPort)",
            adbPort = adbPort
        ))
        appendOutput("Shell 守护进程已启动 (loopback)")
        markWasRunning()
        startWhitelistLoop()
        startHeartbeat()

        startDaemonReader()
        sendInitCommands()
        startDisplayWatcher()
        return true
    }

    fun appendOutput(line: String) {
        _outputLines.add(line)
        if (_outputLines.size > 100) {
            _outputLines.removeAt(0)
        }
        _outputListeners.forEach { it(line) }
    }

    private fun updateStatus(status: ShellStatus) {
        val prev = currentStatus
        currentStatus = status
        // Log every state transition at INFO so the timeline is visible in
        // logcat. Same-state updates are noisy and dropped.
        if (prev.state != status.state || prev.message != status.message) {
            Log.i(TAG, "state ${prev.state} -> ${status.state} :: ${status.message}")
        }
        _listeners.forEach { it(status) }
    }

    fun destroy() {
        scope.cancel()
        runCatching { daemonSocket?.close() }
    }

    // ── Heartbeat ─────────────────────────────────────────────────────────
    //
    // The TCP socket is held open by an idle connection: nothing flows unless
    // either side speaks first. On aggressive OEM ROMs (OPPO ColorOS in
    // particular) doze can leave the app process frozen with the socket stuck,
    // and the reader will not detect that until silence plus a TCP keep-alive
    // window — which is minutes long.
    //
    // The connection is loopback now, so the old WiFi NAT idle-timeout fear is
    // gone; what remains is the app-side freeze. Sending a cheap PING every
    // 25s keeps traffic flowing on the wire and out of any short-lived
    // maintenance window. The daemon already supports PING (see
    // ShellServerEntry); we just add the periodic caller here.

    private var heartbeatJob: Job? = null

    private fun startHeartbeat() {
        if (heartbeatJob != null) return
        heartbeatJob = scope.launch {
            Log.i(TAG, "heartbeat started (every 25s)")
            while (isActive && !stopRequested) {
                try {
                    if (daemonOut != null) {
                        sendCommand("PING")
                        Log.v(TAG, "heartbeat -> PING sent")
                    }
                } catch (t: Throwable) {
                    Log.w(TAG, "heartbeat write failed: ${t.message}")
                }
                try {
                    delay(25_000L)
                } catch (_: CancellationException) {
                    break
                }
            }
            Log.i(TAG, "heartbeat stopped")
        }
    }

    private fun stopHeartbeat() {
        heartbeatJob?.cancel()
        heartbeatJob = null
    }

    // ── Persistence across process death ───────────────────────────────────

    private fun markWasRunning() {
        prefs.edit().putBoolean(KEY_WAS_RUNNING, true).apply()
    }

    private fun clearWasRunning() {
        prefs.edit().putBoolean(KEY_WAS_RUNNING, false).apply()
    }

    /** True if the user started the shell at least once this install. */
    fun wasRunning(): Boolean = prefs.getBoolean(KEY_WAS_RUNNING, false)

    // ── Power-save whitelist push ──────────────────────────────────────────
    //
    // Foreground service is the primary defence, but the system is allowed to
    // freeze even a foreground service under sustained memory pressure on some
    // OEM ROMs. addPowerSaveTempWhitelistApp is the second line: every 25s we
    // re-arm a 30s window for our own UID, which keeps TCP recv ticking through
    // a doze cycle that would otherwise stall the socket for minutes.
    //
    // Reflection on the hidden IDeviceIdleController — the public
    // ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS does not apply to one's own
    // package, so a one-shot permission ask does not work; only the system
    // shell version does. The call is best-effort: any failure is logged and
    // swallowed, since the foreground service is still up.

    private fun startWhitelistLoop() {
        if (whitelistJob != null) return
        Log.i(TAG, "whitelist loop starting (push every 25s)")
        whitelistJob = scope.launch {
            while (isActive && !stopRequested) {
                whitelistSelf()
                try {
                    delay(25_000L)
                } catch (_: CancellationException) {
                    break
                }
            }
            Log.i(TAG, "whitelist loop stopped")
        }
    }

    private fun stopWhitelistLoop() {
        Log.i(TAG, "stopWhitelistLoop")
        whitelistJob?.cancel()
        whitelistJob = null
    }

    private fun whitelistSelf() {
        runCatching {
            // android.os.ServiceManager is @hide. Going through the public
            // Context.DEVICE_IDLE_SERVICE entry point keeps us on the supported
            // API surface — it returns the same binder either way. The
            // constant's value is "deviceidle" (the service name registered by
            // system_server) and has been stable since API 23.
            val binder = context.getSystemService("deviceidle") as? IBinder
                ?: run {
                    Log.w(TAG, "whitelistSelf: getSystemService(\"deviceidle\") returned null")
                    return@runCatching
                }
            val stubCls = Class.forName("android.os.IDeviceIdleController\$Stub")
            val stub = stubCls.getMethod("asInterface", IBinder::class.java).invoke(null, binder)
            // Signature across API levels: addPowerSaveTempWhitelistApp(String, long, int, int, String).
            // We ignore userId (API 30+) and reason (API 33+) by passing 0/0 — the
            // overloads are only present on those levels; the matcher below picks
            // the right one by arity.
            val pkg = context.packageName
            // userId from myUid is uid / 100000 (PER_USER_RANGE); avoids the
            // @hide UserHandle.getUserId. We only ever run as the primary user
            // anyway.
            val userId = Process.myUid() / 100000
            for (m in stub.javaClass.methods) {
                if (m.name != "addPowerSaveTempWhitelistApp") continue
                val params = m.parameterTypes
                if (params.size == 3 &&
                    params[0] == String::class.java &&
                    params[1] == Long::class.javaPrimitiveType &&
                    params[2] == Int::class.javaPrimitiveType) {
                    m.invoke(stub, pkg, 30_000L, userId)
                    Log.v(TAG, "whitelistSelf: 3-arg overload succeeded")
                    return@runCatching
                }
                if (params.size == 5 &&
                    params[0] == String::class.java &&
                    params[1] == Long::class.javaPrimitiveType &&
                    params[2] == Int::class.javaPrimitiveType &&
                    params[3] == Int::class.javaPrimitiveType &&
                    params[4] == String::class.java) {
                    m.invoke(stub, pkg, 30_000L, userId, 316, "shell")
                    Log.v(TAG, "whitelistSelf: 5-arg overload succeeded")
                    return@runCatching
                }
            }
            Log.w(TAG, "whitelistSelf: no matching addPowerSaveTempWhitelistApp overload " +
                    "(API level ${android.os.Build.VERSION.SDK_INT})")
        }.onFailure {
            Log.w(TAG, "whitelistSelf failed: ${it.javaClass.simpleName}: ${it.message}")
        }
    }

    companion object {
        private const val KEY_WAS_RUNNING = "was_running"
    }
}
