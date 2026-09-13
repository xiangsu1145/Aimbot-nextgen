package io.github.xiangsu1145.aimbotnextgen.shell

import android.os.Process
import android.util.Log
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.charset.StandardCharsets
import kotlin.system.exitProcess

/**
 * Privileged input daemon, started by [ShellManager] via a one-shot
 * `adb shell "(app_process ... ShellServerEntry <libDir>)&"` — the subshell
 * backgrounding is what keeps the daemon alive once the shell exits, no fork
 * or adb-side keepalive involved.
 *
 * It runs in the **shell domain** (UID 2000). That is the only place in this
 * project where /dev/input can be read and EVIOCGRAB taken, and where
 * /dev/uinput can be opened — the app UID can do none of those.
 *
 * It is a direct port of the previous project's design
 * (G:\ai\Aimbot-ai\android-client: `root_daemon.cpp` + `touch_core.cpp`):
 * the daemon owns the exclusive panel grab, reads the physical fingers,
 * mirrors them back out through uinput (so the app underneath keeps working),
 * and reports them upstream over a line protocol on an abstract Unix socket.
 * The app then feeds those coordinates to the ImGui menu.
 *
 * ── Protocol ─────────────────────────────────────────────────────────────────
 *   client → daemon, one command per line:
 *     SET_RESOLUTION <w> <h> <rotation>   display size + rotation 0..3; a change
 *                                          re-lays-out a running menu
 *     OPEN                                init uinput, grab panel, start reader
 *     CLOSE                               release everything
 *     SINK <0|1>                           mirror physical touches to uinput?
 *     UI_ON / UI_OFF                       build / drop the menu's own layer and
 *                                          run the ImGui renderer against it
 *     DOWN <x> <y> | MOVE <x> <y> | UP    inject through the virtual device
 *     TAP <x> <y> [durationMs]
 *     GRABBED                              is the panel held exclusively right now?
 *     PING
 *     DESTROY                             close + exit
 *
 *   daemon → client, one line each:
 *     READY
 *     OK | OK:<value> | ERR:<message>
 *     LAYER ...                            layer/renderer progress diagnostics
 *     TOUCH <D|M|U> <x> <y>               physical finger went down / moved / up
 *
 * All diagnostics go to logcat, never to stdout, so the stream stays clean.
 *
 * ── Where the menu is drawn ──────────────────────────────────────────────────
 *
 * The ImGui renderer runs HERE, not in the app. [ShellLayerHost] builds the layer
 * straight on SurfaceFlinger (no WindowManager, so no overlay permission and no
 * platform alpha cap) and hands its Surface to the renderer through
 * `ShellNative.uiStart`. Because the renderer, the touch reader and the uinput
 * mirror all share this process, a finger goes from the panel into ImGui
 * directly and the swallow-rectangles are read straight back out — no line
 * protocol in the touch loop.
 */
object ShellServerEntry {

    private const val TAG = "aimbot_shell"

    /**
     * Where the daemon publishes its listening TCP port.
     *
     * Abstract Unix sockets (`LocalServerSocket(name)`) are blocked by SELinux
     * between `shell` and `untrusted_app` domains on most vendor builds (the
     * connect fails with `Connection refused` even though the daemon is
     * listening), so we use a 127.0.0.1 TCP port instead. Loopback does not
     * traverse NAT, so the OEM-side idle timeouts that plague WiFi ADB do not
     * apply. The file is the only handshake between daemon and app — both ends
     * are the only readers, both run in the same shell-uid-or-app process
     * group, no other app cares.
     */
    private const val PORT_FILE = "/data/local/tmp/aimbot_shell.port"

    /** Consecutive failed polls the reader tolerates before it gives up. */
    private const val kReaderMaxFailures = 100

    // ── Session (one client connection) timeouts ────────────────────────────
    //
    // The app has to sit in the background while a game is in front, which on
    // ColorOS means it gets frozen: the process stays, its threads stop. A
    // frozen client never reads and never sends its 25 s heartbeat, so the
    // only way this side notices is a read deadline.
    //
    // 30 s comfortably clears the app's 25 s heartbeat on a healthy link but is
    // short enough to matter; two in a row (60 s of silence) before we drop the
    // client. Dropping it costs nothing now — see main().

    /** How long a single blocking read may go unanswered. */
    private const val kReadTimeoutMs = 30_000

    /** Consecutive unanswered read deadlines before the client is dropped. */
    private const val kMaxIdleTimeouts = 2

    /**
     * Bounded queue for outbound protocol lines. When the client's TCP socket
     * stalls the stream blocks the calling thread — which would freeze the touch
     * reader, geometry watcher, and everything else. The sender thread drains
     * this queue with a timeout; callers never block.
     */
    private val sendQueue = java.util.concurrent.LinkedBlockingDeque<String>(128)
    @Volatile private var senderThread: Thread? = null

    @Volatile private var running = true
    @Volatile private var readerThread: Thread? = null

    // ── Client session ──────────────────────────────────────────────────────
    //
    // The connection is a *session*, not the daemon's lifetime. It comes and
    // goes: the app is backgrounded, frozen, thawed, restarted, and each time
    // it comes back it should find the daemon still holding the panel and the
    // menu still on screen. So the socket and everything scoped to it is kept
    // apart from the panel grab, the layer and the reader, which outlive it.

    /** Guards [clientSocket] / [sessionId] handover between sessions. */
    private val sessionLock = Any()

    /** The currently served client, or null while waiting for one. */
    @Volatile private var clientSocket: Socket? = null

    /** Bumped per accepted connection so a stale sender thread cannot touch
     *  the new one's writer (its loop would otherwise write into a closed
     *  socket and drop a client that had only just connected). */
    @Volatile private var sessionId = 0

    /** Set when the served connection is known dead; ends the session loop. */
    @Volatile private var sessionDead = false

    /** Port this daemon is listening on, reported when a session ends. */
    @Volatile private var listeningPort = 0

    /** Asks the reader thread to leave its loop (set by stopReaderThread). */
    @Volatile private var readerStop = false

    /** Publishes the live ImGui rectangles to the uinput mirror. */
    @Volatile private var regionThread: Thread? = null
    @Volatile private var regionStop = false

    // Display geometry. Set by SET_RESOLUTION when the app sends it, but owned
    // by startGeometryWatcher() — the app's copy is only a fallback, because a
    // backgrounded app can be an orientation behind. See that function.
    @Volatile private var screenW = 0
    @Volatile private var screenH = 0
    @Volatile private var rotation = 0

    /**
     * The port of a daemon that is already up, or null if the coast is clear.
     *
     * Binding port 0 always succeeds, so nothing stops a second daemon from
     * starting — and that used to be the normal outcome whenever the app was
     * killed and relaunched: it could not know a daemon was still alive, so it
     * launched another. Two daemons both take the panel grab and both mirror
     * every finger, which shows up as doubled or dropped touches.
     *
     * The port file is the handshake: if something answers on it, that is the
     * existing daemon and this process has no business running.
     */
    private fun alreadyRunningPort(): Int? {
        val port = try {
            java.io.File(PORT_FILE).readText().trim().toIntOrNull()
        } catch (_: Throwable) {
            null
        }
        if (port == null || port !in 1..65535) return null
        return try {
            Socket(java.net.InetAddress.getByName("127.0.0.1"), port).use { port }
        } catch (_: Throwable) {
            null  // stale file from a daemon that is gone
        }
    }

    /**
     * Preloads every .so the inference layer is going to need.
     *
     * Two passes, in this order:
     *
     *  1. `libc++_shared.so` — it has to come first because
     *     `libqnn_tflite_delegate_jni.so`'s DT_NEEDED list pulls it in.
     *     The clns-1 namespace refuses access to /odm/lib64 (where it lives
     *     on some vendor builds), so we ship a copy in the APK's jniLibs
     *     (NDK 29's) and load it by absolute path.
     *
     *  2. The DT_NEEDED siblings of `libaimbotng.so` — the vendor .so files
     *     that `libaimbotng.so` itself pulls in via DT_NEEDED
     *     (libtensorflowlite_jni.so, libQnnTFLiteDelegate.so,
     *     libqnn_tflite_delegate_jni.so). Hardcoded here because we have to
     *     do this *before* `System.load(libaimbotng.so)` runs, and at that
     *     point JNI through libaimbotng.so is not available yet — chicken
     *     and egg. The Runtime-specific stack (libQnnHtp.so, the per-arch
     *     V{arch}Skel, libQnnSystem.so, …) is loaded AFTER libaimbotng.so
     *     is in via [ShellNative.preloadAllDaemonLibraries].
     *
     * The reason this dance exists at all: the daemon runs under `app_process`
     * in Android's compat library namespace (clns-1), which only resolves
     * SONAMEs that come from `<uses-library>` declarations and the system
     * public-libraries list. Every SONAME that only lives in jniLibs is
     * invisible to DT_NEEDED resolution — `System.load(libaimbotng.so)`
     * aborts with `library "libtensorflowlite_jni.so" not found in namespace
     * clns-1`. Absolute-path loads bypass that lookup: the dynamic linker
     * opens the file directly and registers the SONAME on clns-1, so
     * subsequent DT_NEEDED resolution can find it.
     *
     * The app process does not need any of this: its classloader namespace
     * is built from jniLibs directly.
     */
    private fun preloadDaemonDeps(libDir: String) {
        // Pass 1: libc++_shared.so — see the comment above.
        val cppShared = "$libDir/libc++_shared.so"
        if (java.io.File(cppShared).exists()) {
            try {
                System.load(cppShared)
                Log.i(TAG, "preloaded $cppShared")
            } catch (t: Throwable) {
                Log.w(TAG, "preload failed: $cppShared -> ${t.javaClass.simpleName}: ${t.message}")
            }
        } else {
            Log.w(TAG, "preload skip: $cppShared missing — copy libc++_shared.so into jniLibs")
        }

        // Pass 2: libaimbotng.so's DT_NEEDED siblings. They must be in the
        // namespace BEFORE libaimbotng.so is loaded or the linker aborts
        // with "library ... not found in namespace clns-1".
        //
        // Keep this in lock-step with CMakeLists.txt's target_link_libraries
        // (libaimbotng.so). Adding a new DT_NEEDED entry there means adding
        // it here too. The current set is the full jniLibs slice of the
        // QNN / TFLite stack: TFLite runtime + TFLite delegate + QNN
        // backend + QNN HTP runtime + QNN system layer + QNN HTP extensions.
        val deps = arrayOf(
            "libtensorflowlite_jni.so",
            "libQnnTFLiteDelegate.so",
            "libqnn_tflite_delegate_jni.so",
            "libQnnHtp.so",
            "libQnnHtpPrepare.so",
            "libQnnHtpNetRunExtensions.so",
            "libQnnSystem.so",
        )
        for (name in deps) {
            val path = "$libDir/$name"
            try {
                System.load(path)
                Log.i(TAG, "preloaded $path")
            } catch (t: Throwable) {
                Log.w(TAG, "preload failed: $path -> ${t.javaClass.simpleName}: ${t.message}")
            }
        }
    }

    /**
     * Post-libaimbotng preload — the runtime-specific stack that is NOT in
     * libaimbotng.so's DT_NEEDED list. This is loaded only when an actual
     * model is about to run, because the QNN HTP skels are large and most
     * sessions never go past the CPU path.
     *
     * Called from the model-load path, not from the daemon boot path.
     */
    fun preloadRuntimeStack(): Int {
        return try {
            ShellNative.preloadAllDaemonLibraries()
        } catch (t: Throwable) {
            Log.w(TAG, "preloadRuntimeStack failed: ${t.javaClass.simpleName}: ${t.message}")
            -1
        }
    }

    @JvmStatic
    fun main(args: Array<String>) {
        Log.i(TAG, "daemon starting uid=${Process.myUid()} pid=${Process.myPid()}")

        val existing = alreadyRunningPort()
        if (existing != null) {
            Log.i(TAG, "a daemon is already serving 127.0.0.1:$existing — this one exits")
            exitProcess(0)
        }

        val libDir = args.firstOrNull()
        if (libDir.isNullOrBlank()) {
            replyErr("native library directory argument is missing")
            return
        }
        // The app process has jniLibs on its classloader namespace, so DT_NEEDED
        // resolution just works. The daemon does not: clns-1 only sees
        // <uses-library> + system public libs. preloadDaemonDeps() opens the
        // vendor .so files by absolute path first, which makes their SONAMEs
        // resolvable when System.load(libaimbotng.so) walks its DT_NEEDED list.
        // See preloadDaemonDeps() for the long version.
        preloadDaemonDeps(libDir)
        try {
            ShellNative.load("$libDir/libaimbotng.so")
        } catch (t: Throwable) {
            Log.e(TAG, "failed to load libaimbotng.so from $libDir", t)
            replyErr("loadLibrary failed: ${t.message}")
            return
        }

        ShellNative.modelLoadFromDisk()

        // Bind a 127.0.0.1 TCP port (loopback — no NAT, no OEM timeout) and
        // publish the port number so the app can find us. The shell launched
        // us with `(cmd) &`, so we are already detached from any foreground
        // adbd session; the only thing the app needs is this port.
        val serverSocket: ServerSocket = try {
            ServerSocket(0, 8, java.net.InetAddress.getByName("127.0.0.1"))
        } catch (t: Throwable) {
            Log.e(TAG, "could not bind 127.0.0.1:0", t)
            exitProcess(2)
        }
        val port = serverSocket.localPort
        listeningPort = port
        runCatching { java.io.File(PORT_FILE).writeText(port.toString()) }
            .onFailure { Log.w(TAG, "could not write $PORT_FILE", it) }
        Log.i(TAG, "listening on 127.0.0.1:$port, pid=${Process.myPid()}")

        // The listening socket stays open for the daemon's whole life. It used
        // to be closed the instant the first client arrived, which meant a
        // dropped connection could never be replaced: the only recovery was to
        // kill and relaunch the whole daemon, taking the panel grab and the
        // menu down with it.
        serverSocket.soTimeout = 0

        startCaptureSupervisor()
        startGeometryWatcher()
        clearRelayScratch()

        Log.i(TAG, "daemon ready (uid=${Process.myUid()} pid=${Process.myPid()}), awaiting a client")

        // ── One iteration per client ────────────────────────────────────────
        //
        // Losing a client is not a reason to die. The app lives in the
        // background while a game is in front, so it *will* be frozen and its
        // socket *will* go quiet — that is the normal case, not a fault. When
        // it happens the daemon keeps the grab, keeps the menu on screen and
        // goes back to waiting, so the app can simply reconnect and take over
        // where it left off. Only DESTROY (running = false) ends the loop, and
        // only then is the panel handed back.
        while (running) {
            val client = try {
                serverSocket.accept()
            } catch (t: Throwable) {
                if (!running) break
                Log.e(TAG, "TCP accept failed", t)
                // A transient failure should not spin the CPU.
                runCatching { Thread.sleep(500) }
                continue
            }
            serveClient(client)
            if (running) {
                Log.i(TAG, "session ended — still up on 127.0.0.1:$port, " +
                        "menu=${ShellLayerHost.isRunning()}, " +
                        "grabbed=${ShellNative.readerIsGrabbed()}")
            }
        }

        shutdown()
        runCatching { serverSocket.close() }
        runCatching { java.io.File(PORT_FILE).delete() }
        Log.i(TAG, "daemon exiting")
        exitProcess(0)
    }

    /**
     * Serves one client until its connection dies, then returns.
     *
     * Everything that belongs to the connection dies with it. Everything that
     * belongs to the daemon — the panel grab, the reader, the menu layer, the
     * capture supervisor — is deliberately left alone, because the app being
     * frozen is not the same as the user having asked for the menu to close.
     */
    private fun serveClient(client: Socket) {
        val mySession: Int
        synchronized(sessionLock) {
            mySession = ++sessionId
            clientSocket = client
            sessionDead = false
            sendQueue.clear()
        }

        client.soTimeout = kReadTimeoutMs
        client.tcpNoDelay = true

        val tcpIn = BufferedReader(InputStreamReader(client.getInputStream(), StandardCharsets.UTF_8))
        val tcpOut = BufferedWriter(OutputStreamWriter(client.getOutputStream(), StandardCharsets.UTF_8))

        startTcpSenderThread(tcpOut, mySession)

        Log.i(TAG, "client connected from ${client.inetAddress}")
        send("READY")
        // A reconnecting app has to be told where things stand — its own idea
        // of the menu is from before it was frozen, or from a previous process.
        send("STATE ui=${if (ShellLayerHost.isRunning()) 1 else 0} " +
                "grabbed=${if (ShellNative.readerIsGrabbed()) 1 else 0}")

        var idleTimeouts = 0
        while (running && !sessionDead) {
            val line: String? = try {
                tcpIn.readLine()
            } catch (t: SocketTimeoutException) {
                // Nothing arrived inside the deadline. The socket is still
                // valid — only the read gave up — so this is a liveness probe,
                // not an error: nudge the client and see whether the write
                // side survives. A frozen app neither reads nor answers, and
                // once its receive buffer is full the write fails too, which
                // is the earliest possible proof it is gone.
                idleTimeouts++
                Log.w(TAG, "client silent for ${idleTimeouts * kReadTimeoutMs / 1000}s " +
                        "(frozen or gone) — probing")
                send("PING?")
                if (idleTimeouts >= kMaxIdleTimeouts) {
                    Log.w(TAG, "client failed ${idleTimeouts} probes — dropping it")
                    null
                } else {
                    continue
                }
            } catch (t: Throwable) {
                Log.e(TAG, "socket read failed", t)
                null
            }

            if (line == null) break
            if (line.isBlank()) continue
            idleTimeouts = 0
            try {
                handle(line.trim())
            } catch (t: Throwable) {
                Log.e(TAG, "command failed: $line", t)
                replyErr(t.message ?: "command failed")
            }
        }

        endSession(client)
    }

    /**
     * Closes a finished session. Idempotent, and safe to call for a connection
     * that is no longer the current one.
     */
    private fun endSession(client: Socket) {
        sessionDead = true
        synchronized(sessionLock) {
            if (clientSocket === client) clientSocket = null
        }
        sendQueue.clear()
        runCatching { client.close() }
            .onFailure { Log.w(TAG, "client close failed", it) }
    }

    /**
     * Drops the live client because the link to it is broken.
     *
     * This is what turns a death that used to take a quarter of an hour into
     * one that takes a moment. Waiting for the kernel to give up on TCP
     * retransmission meant the daemon sat on a dead socket — still pushing
     * TOUCH lines into a buffer nobody was draining — until it finally
     * surfaced as `Connection timed out`, by which time the whole session was
     * poisoned. A failed write is proof enough; act on it.
     */
    private fun killSession(reason: String) {
        if (sessionDead) return
        Log.w(TAG, "dropping client: $reason")
        val client = synchronized(sessionLock) { clientSocket }
        if (client != null) endSession(client)
    }

    // ── Command dispatch ─────────────────────────────────────────────────────

    private fun handle(cmd: String) {
        val parts = cmd.split(' ')
        when (parts[0]) {
            // Panel size + rotation. The app sends this when it connects and
            // whenever its own display listener fires — but it is a hint, not the
            // source of truth, because a backgrounded app can be an orientation
            // behind. startGeometryWatcher() reads the display directly and this
            // handler prefers that reading.
            //
            // Applying geometry is not enough on its own: an already-running menu
            // has its size baked into the layer, the swapchain and ImGui's
            // DisplaySize, so a real change has to rebuild the layer too. That
            // rebuild is what makes a rotation land correctly instead of leaving
            // the menu at the old shape and place.
            "SET_RESOLUTION" -> {
                if (parts.size < 4) {
                    replyErr("usage: SET_RESOLUTION <w> <h> <rotation>")
                    return
                }
                val toldW = parts[1].toIntOrNull() ?: screenW
                val toldH = parts[2].toIntOrNull() ?: screenH
                val toldRot = parts[3].toIntOrNull() ?: rotation

                // The app's numbers are a fallback, not the truth. It has no way
                // to know a rotation that happened while it was not in front, so
                // it can be an orientation behind; this process can read the
                // display, so when it can, its own reading wins and the app's is
                // only reported when the two disagree.
                val own = SysDisplay.defaultGeometry()
                if (own != null && (own.first != toldW || own.second != toldH)) {
                    Log.i(TAG, "app sent ${toldW}x$toldH rot=$toldRot, display says " +
                            "${own.first}x${own.second} rot=${own.third}")
                }
                val (w, h, rot) = own ?: Triple(toldW, toldH, toldRot)

                val changed = w != screenW || h != screenH || rot != rotation
                applyGeometry(w, h, rot)
                if (changed && ShellLayerHost.isRunning()) {
                    val status = ShellLayerHost.resize(w, h) { line -> send(line) }
                    Log.i(TAG, "menu re-laid-out for ${w}x$h: $status")
                    send("LAYER resize=$status")
                }
                replyOk()
            }

            "OPEN" -> handleOpen()

            "CLOSE" -> {
                stopReaderThread()
                ShellNative.readerUngrab()
                ShellNative.readerClose()
                ShellNative.uinputClose()
                // The panel is back with the system, so the overlay must stop
                // being touchable at once — say so instead of a bare OK.
                replyOkValue("grabbed=0")
            }

            "SINK" -> {
                val on = parts.getOrNull(1) != "0"
                ShellNative.readerSetSink(on)
                replyOk()
            }

            "DOWN" -> {
                val (x, y) = parsePair(parts) ?: return replyErr("usage: DOWN <x> <y>")
                ShellNative.uinputDown(ShellNative.INJECT_SLOT, ShellNative.INJECT_ID, x, y)
                replyOk()
            }

            "MOVE" -> {
                val (x, y) = parsePair(parts) ?: return replyErr("usage: MOVE <x> <y>")
                ShellNative.uinputMove(ShellNative.INJECT_SLOT, x, y)
                replyOk()
            }

            "UP" -> {
                ShellNative.uinputUp(ShellNative.INJECT_SLOT)
                replyOk()
            }

            "TAP" -> {
                val (x, y) = parsePair(parts) ?: return replyErr("usage: TAP <x> <y> [durationMs]")
                val duration = parts.getOrNull(3)?.toLongOrNull() ?: 8L
                ShellNative.uinputDown(ShellNative.INJECT_SLOT, ShellNative.INJECT_ID, x, y)
                Thread.sleep(duration)
                ShellNative.uinputUp(ShellNative.INJECT_SLOT)
                replyOk()
            }

            // Feeds the menu's own input path directly, the same call the panel
            // reader makes for a real finger. uinput-based injection cannot reach
            // ImGui — the reader holds an EVIOCGRAB on the panel, so synthetic
            // events go to InputDispatcher and stop there — which makes this the
            // only way to drive the menu from a script. Used by tools/ to
            // reproduce touch-dependent behaviour (rate throttling, hit tests)
            // without a hand on the screen.
            "UITOUCH" -> {
                if (parts.size < 4) {
                    replyErr("usage: UITOUCH <DOWN|MOVE|UP> <x> <y>")
                    return
                }
                val action = when (parts[1].uppercase()) {
                    "DOWN" -> ShellNative.UI_TOUCH_DOWN
                    "MOVE" -> ShellNative.UI_TOUCH_MOVE
                    "UP"   -> ShellNative.UI_TOUCH_UP
                    else   -> return replyErr("usage: UITOUCH <DOWN|MOVE|UP> <x> <y>")
                }
                val x = parts[2].toIntOrNull() ?: return replyErr("bad x")
                val y = parts[3].toIntOrNull() ?: return replyErr("bad y")
                feedImGui(action, x, y)
                replyOk()
            }

            "PING" -> replyOk()

            // Scripted control of the Model page's inference switch.
            //
            // Same reason UITOUCH exists: the switch is an ImGui control painted
            // onto our own layer, so neither InputDispatcher (`adb shell input`)
            // nor our uinput injection can reach it — only a finger on the panel
            // we hold EVIOCGRAB on. Without this there is no way to exercise the
            // inference chain from a script.
            //
            // It drives the switch rather than the runtime on purpose. The switch
            // stays the single authority; two entry points would let the UI read
            // OFF while a model was loaded and running.
            //
            //   INFER ON     flip the switch on  (starts the loaded model)
            //   INFER OFF    flip it off
            //   INFER ?      OK:<live status line>
            //   INFER DESC   OK:<what the loaded model bound>
            "INFER" -> {
                when (parts.getOrNull(1)?.uppercase()) {
                    "ON"   -> { ShellNative.modelSwitchSet(true);  replyOk() }
                    "OFF"  -> { ShellNative.modelSwitchSet(false); replyOk() }
                    "?"    -> replyOkValue(ShellNative.inferStatus())
                    "DESC" -> replyOkValue(ShellNative.inferDescribe())
                    else   -> replyErr("usage: INFER ON|OFF|?|DESC")
                }
            }

            // Answer to the daemon's own liveness probe (see serveClient).
            // Deliberately silent: it arrives every 30 s while the app is
            // healthy, and acknowledging it would just put another line on the
            // wire for no reader.
            "PONG" -> Unit

            // Live read of the native grab state, not a cached one: the overlay
            // keeps a full-screen TOUCHABLE window only for as long as this says
            // 1, so a grab lost after OPEN (device re-plugged, reader reset,
            // kernel dropped it) must be visible to the app within a heartbeat.
            "GRABBED" -> replyOkValue(
                "grabbed=${if (ShellNative.readerIsGrabbed()) 1 else 0}"
            )

            // ── Menu layer + renderer (see ShellLayerHost) ───────────────────
            // Builds the shell-owned layer on SurfaceFlinger, starts the ImGui
            // renderer against its Surface, and begins feeding the menu's live
            // rectangles back into the uinput mirror. Progress streams back as
            // plain "LAYER ..." lines.
            "UI_ON" -> {
                val w = if (screenW > 0) screenW else 1080
                val h = if (screenH > 0) screenH else 1920
                val status = ShellLayerHost.start(w, h) { line -> send(line) }
                if (status == "on") startRegionPump()
                replyOkValue("ui=$status")
            }

            "UI_OFF" -> {
                stopRegionPump()
                runCatching { ShellNative.readerSetRegions(IntArray(0)) }
                replyOkValue("ui=${ShellLayerHost.stop()}")
            }

            "DESTROY" -> {
                // Reply before tearing anything down: the confirmation rides
                // out on the sender thread, and the caller is waiting on it.
                replyOk()
                runCatching { Thread.sleep(150) }
                running = false
            }

            else -> replyErr("unknown command: ${parts[0]}")
        }
    }

    // ── Menu geometry → uinput mirror ────────────────────────────────────────

    /**
     * Keeps the swallow-rectangles in step with the live ImGui layout.
     *
     * The renderer and uinput now share this process, so there is no protocol
     * hop: the pump simply reads the menu's rectangles out of the renderer and
     * hands them to the mirror, which withholds any gesture that *starts* inside
     * one of them. A finger on the board therefore drives the menu and never
     * reaches the app underneath, while everything else still passes straight
     * through. An empty list (menu closed) means full pass-through.
     */
    // ── Capture supervisor ───────────────────────────────────────────────────
    //
    // The menu authors two values — a switch and a crop size — but it writes
    // them into native memory from the render thread, which cannot call into
    // Java. Turning them into an actual virtual display therefore needs
    // something in between, and this is it: a slow poll that makes the producer
    // match what the menu asked for.
    //
    // The two are not equally expensive. The switch builds or drops a
    // full-screen mirror; the crop size only moves a rectangle inside the
    // frame thread, so a slider drag costs nothing to rebuild.
    //
    // Deliberately not a command on the stdin protocol. The switch is flipped
    // inside a Vulkan frame; routing that out through stdout and back in as a
    // command would round-trip through the app to arrive at a decision the menu
    // already made.

    /** How often the supervisor compares the menu's wishes against reality. */
    private val kCapturePollMs = 400L

    /** Back-off after a failed start, so a refusal does not spam the log. */
    private val kCaptureRetryMs = 3_000L

    @Volatile private var captureThread: Thread? = null
    @Volatile private var captureStop = false

    private fun startCaptureSupervisor() {
        if (captureThread != null) return
        captureStop = false
        val t = Thread({
            Log.i(TAG, "capture supervisor started")
            while (running && !captureStop) {
                try {
                    // The switch says the user has capture turned on; the
                    // reader says whether anyone is looking. Both have to hold
                    // before a mirror is worth building.
                    //
                    // Acting on the switch alone kept a full-screen 2K virtual
                    // display and its frame pump running for entire sessions in
                    // which not one frame was sampled: ~80% of a core and a GC
                    // cycle every other second, spent producing frames that
                    // were counted and then thrown away.
                    val switch = ShellNative.captureWanted()
                    val reading = ShellNative.captureConsuming()
                    val wanted = switch && reading
                    val size = ShellNative.captureWantedSize()
                    if (wanted) {
                        if (!ScreenCapture.isAlive() || ScreenCapture.needsRestart()) {
                            Log.i(TAG, "capture: building mirror (switch=$switch, reading=$reading, " +
                                    "alive=${ScreenCapture.isAlive()}, " +
                                    "needsRestart=${ScreenCapture.needsRestart()}, crop ${size}x$size)")
                            if (!ScreenCapture.start(size)) {
                                // Do not retry four times a second — whatever
                                // went wrong (permission, driver, an unreadable
                                // display) will not fix itself that fast, and
                                // the log would be unreadable.
                                Thread.sleep(kCaptureRetryMs)
                            }
                        } else {
                            // The mirror is up; a new crop side is just a
                            // rectangle, so the display is left alone.
                            ScreenCapture.setCrop(size)
                        }
                    } else if (ScreenCapture.activeSize() != 0) {
                        ScreenCapture.stop()
                    }
                } catch (t: Throwable) {
                    Log.e(TAG, "capture supervisor failed", t)
                }
                try {
                    Thread.sleep(kCapturePollMs)
                } catch (ignored: InterruptedException) {
                    break
                }
            }
            Log.i(TAG, "capture supervisor stopped")
        }, "aimbot-capture-sup")
        t.isDaemon = true
        captureThread = t
        t.start()
    }

    private fun stopCaptureSupervisor() {
        captureStop = true
        captureThread = null
        runCatching { ScreenCapture.stop() }
    }

    // ── Display geometry, read by the daemon itself ──────────────────────────

    /** How often the daemon re-reads the panel geometry. */
    private val kGeometryPollMs = 500L

    @Volatile private var geometryThread: Thread? = null

    /**
     * Keeps this process's idea of the panel in step with the panel.
     *
     * The app also pushes geometry (`SET_RESOLUTION`), and that is still honoured
     * when the daemon cannot read the display itself — but it cannot be trusted.
     * A process that is not in front is not guaranteed to be told about a
     * rotation, and a frozen one is not running at all, so its idea of the screen
     * can be a whole orientation behind. That is exactly what showed up as a menu
     * built for the previous orientation and then left there: off-centre, and
     * "fixed" by opening the app, which is what finally updated its display info.
     *
     * This process has no such problem — it is long-lived and never frozen — so
     * it reads the display itself. Half a second is the trade: quick enough that
     * a rotation lands about when the user finishes turning the phone, slow
     * enough to be invisible in a process already drawing at 120 fps.
     */
    private fun startGeometryWatcher() {
        if (geometryThread != null) return
        val t = Thread({
            Log.i(TAG, "geometry watcher started")
            while (running) {
                try {
                    val geom = SysDisplay.defaultGeometry()
                    if (geom != null) {
                        val (w, h, rot) = geom
                        if (w != screenW || h != screenH || rot != rotation) {
                            Log.i(TAG, "display is ${w}x$h rot=$rot " +
                                    "(was ${screenW}x$screenH rot=$rotation)")
                            applyGeometry(w, h, rot)
                            if (ShellLayerHost.isRunning()) {
                                val status = ShellLayerHost.resize(w, h) { line -> send(line) }
                                Log.i(TAG, "menu re-laid-out for ${w}x$h: $status")
                                send("LAYER resize=$status")
                            }
                        }
                    }
                } catch (e: Throwable) {
                    Log.e(TAG, "geometry poll failed", e)
                }
                try {
                    Thread.sleep(kGeometryPollMs)
                } catch (ignored: InterruptedException) {
                    break
                }
            }
            Log.i(TAG, "geometry watcher stopped")
        }, "aimbot-geometry")
        t.isDaemon = true
        geometryThread = t
        t.start()
    }

    private fun stopGeometryWatcher() {
        geometryThread = null
    }

    private fun startRegionPump() {
        // `!= null` is not liveness. The pump's loop is bounded by the menu layer
        // being up, so it returns on its own the moment that layer goes away —
        // and a thread object for a run that has already finished is still a
        // non-null reference. Reusing it would leave the pump permanently
        // "already running", which means the rectangles stop being refreshed
        // while the menu is on screen and, worse, keep whatever the last run
        // published: the mirror then carries on swallowing touches inside a
        // layout that is no longer there.
        val existing = regionThread
        if (existing != null && existing.isAlive) return
        regionStop = false
        val tickMs = 1000L / 30
        val t = Thread({
            Log.i(TAG, "region pump started")
            try {
                while (running && !regionStop && ShellLayerHost.isRunning()) {
                    try {
                        ShellNative.readerSetRegions(ShellNative.uiRegions())
                    } catch (t: Throwable) {
                        Log.e(TAG, "region pump failed", t)
                        break
                    }
                    try {
                        Thread.sleep(tickMs)
                    } catch (ignored: InterruptedException) {
                        break
                    }
                }
            } finally {
                // Whatever ends this loop — menu off, layer lost, an exception —
                // the menu is gone, so nothing may keep swallowing touches.
                // Leaving the last rectangles behind is the difference between
                // "the menu closed" and "the phone stopped taking touch".
                runCatching { ShellNative.readerSetRegions(IntArray(0)) }
                if (Thread.currentThread() === regionThread) regionThread = null
                Log.i(TAG, "region pump stopped (regions cleared)")
            }
        }, "aimbot-regions")
        t.isDaemon = true
        regionThread = t
        t.start()
    }

    private fun stopRegionPump() {
        val t = regionThread ?: return
        regionThread = null
        regionStop = true
        runCatching { t.join(200) }
    }

    // ── Display geometry ─────────────────────────────────────────────────────

    /**
     * Stores the current display geometry and hands it to everything that maps
     * coordinates with it. Cheap and safe to call at any time: each native side
     * is only touched once it is actually up.
     */
    private fun applyGeometry(w: Int, h: Int, rot: Int) {
        screenW = w
        screenH = h
        rotation = rot
        if (ShellNative.readerIsReady()) {
            ShellNative.readerSetScreenParams(w, h, rot)
        }
        if (ShellNative.uinputIsReady()) {
            ShellNative.uinputSetScreenParams(w, h, w > h)
        }
    }

    private fun handleOpen() {
        if (screenW <= 0 || screenH <= 0) {
            replyErr("SET_RESOLUTION must be sent before OPEN")
            return
        }

        // Order matters: the reader enumerates /dev/input BEFORE we create the
        // virtual device, otherwise it could pick our own clone. uinput is then
        // told exactly which panel to clone.
        if (!ShellNative.readerIsReady()) {
            if (!ShellNative.readerInit(screenW, screenH, rotation)) {
                replyErr("readerInit failed (no touch panel?)")
                return
            }
        }
        ShellNative.readerSetScreenParams(screenW, screenH, rotation)

        ShellNative.uinputSetSourcePanel(ShellNative.readerGetPanelPath())
        if (!ShellNative.uinputIsReady()) {
            if (!ShellNative.uinputInit(screenW, screenH)) {
                ShellNative.readerClose()
                replyErr("uinputInit failed (cannot open /dev/uinput?)")
                return
            }
        }
        ShellNative.uinputSetScreenParams(screenW, screenH, screenW > screenH)

        // Physical touches are mirrored back out so the game still receives
        // them, while we read a copy for the ImGui menu.
        ShellNative.readerSetSink(true)

        val grabbed = ShellNative.readerGrab()
        Log.i(TAG, "OPEN: panel=${ShellNative.readerGetPanelPath()} " +
                "abs=${ShellNative.readerGetMaxX()}x${ShellNative.readerGetMaxY()} grabbed=$grabbed")

        startReaderThread()
        replyOkValue("grabbed=${if (grabbed) 1 else 0}")
    }

    // ── Physical touch → upstream ────────────────────────────────────────────

    /**
     * Drives the physical panel reader.
     *
     * This thread is the only thing holding the panel grab open, so anything that
     * ends it also ends touch for the whole device — unless the grab is handed
     * back on the way out. It therefore:
     *
     *  * contains per-iteration failures instead of unwinding: one bad frame is
     *    not a reason to take the phone's touchscreen away;
     *  * tolerates a run of failed polls rather than treating a single `-1` as
     *    fatal (EINTR and a momentarily busy panel both surface that way);
     *  * always releases the grab when it leaves, by any path, so the worst case
     *    is "the menu stopped responding" rather than "the screen stopped
     *    responding".
     */
    private fun startReaderThread() {
        if (readerThread != null) return
        readerStop = false
        val t = Thread({
            Log.i(TAG, "reader thread started")
            var lastCount = 0
            var lastX = 0
            var lastY = 0
            var failedPolls = 0
            try {
                while (running && !readerStop) {
                    val changed = try {
                        ShellNative.readerPoll(50)
                    } catch (t: Throwable) {
                        Log.e(TAG, "readerPoll failed", t)
                        -1
                    }

                    if (changed < 0) {
                        if (++failedPolls >= kReaderMaxFailures) {
                            Log.e(TAG, "reader poll failed $failedPolls times — giving up")
                            break
                        }
                        Thread.sleep(20)
                        continue
                    }
                    failedPolls = 0
                    if (changed == 0) continue

                    val data = try {
                        ShellNative.readerReadPointers()
                    } catch (t: Throwable) {
                        Log.e(TAG, "readerReadPointers failed", t)
                        continue
                    }
                    val count = data.size / 3

                    if (count > 0) {
                        val x = data[1]
                        val y = data[2]
                        // Down when the panel had no finger before, move otherwise.
                        // Skip sub-pixel noise so we do not spam the stream.
                        if (lastCount == 0) {
                            send("TOUCH D $x $y")
                            feedImGui(ShellNative.UI_TOUCH_DOWN, x, y)
                        } else if (x != lastX || y != lastY) {
                            send("TOUCH M $x $y")
                            feedImGui(ShellNative.UI_TOUCH_MOVE, x, y)
                        }
                        lastX = x
                        lastY = y
                    } else if (lastCount > 0) {
                        send("TOUCH U $lastX $lastY")
                        feedImGui(ShellNative.UI_TOUCH_UP, lastX, lastY)
                    }
                    lastCount = count
                }
            } catch (t: Throwable) {
                Log.e(TAG, "reader loop aborted", t)
            } finally {
                // However the loop ended, the panel goes back to the system. While
                // it is grabbed this thread is the device's only source of touch,
                // so walking away still holding it would leave the phone with no
                // touch at all.
                runCatching { ShellNative.readerUngrab() }
                    .onFailure { Log.w(TAG, "reader exit ungrab failed", it) }
                Log.i(TAG, "reader thread stopped (panel released)")
            }
        }, "aimbot-touch")
        t.isDaemon = true
        readerThread = t
        t.start()
    }

    private fun stopReaderThread() {
        val t = readerThread ?: return
        readerThread = null
        readerStop = true
        // readerPoll uses a 50 ms timeout, so it returns on its own.
        runCatching { t.join(500) }
    }

    /**
     * Hands one physical finger to the renderer, which now lives in this process.
     *
     * A plain call, no protocol: the panel reader and the ImGui renderer are in
     * the same process, so there is nothing in between them. Guarded on the host
     * state so events are simply dropped while no menu is up.
     */
    private fun feedImGui(action: Int, x: Int, y: Int) {
        if (!ShellLayerHost.isRunning()) return
        runCatching { ShellNative.uiTouch(action, x.toFloat(), y.toFloat()) }
            .onFailure { Log.w(TAG, "uiTouch failed", it) }
    }

    // ── Protocol helpers ─────────────────────────────────────────────────────

    private fun parsePair(parts: List<String>): Pair<Int, Int>? {
        if (parts.size < 3) return null
        val x = parts[1].toIntOrNull() ?: return null
        val y = parts[2].toIntOrNull() ?: return null
        return x to y
    }

    private fun send(line: String) {
        // With no session there is nobody to tell. Dropping here is what keeps
        // the touch reader from filling a queue that will never be drained —
        // which is exactly the shape the old "daemon died after a few minutes"
        // bug had.
        if (sessionDead) return
        // Non-blocking: if the queue is full, drop the message rather than
        // freezing the calling thread. A full queue means the peer has stopped
        // reading; the sender thread's next failed write is what ends the
        // session, and one dropped line is not worth blocking a touch frame.
        if (!sendQueue.offer(line)) {
            Log.w(TAG, "send queue full, dropped: $line")
        }
    }

    /**
     * Dedicated thread that drains [sendQueue] and writes to the TCP socket.
     *
     * Replaces the old stdout sender thread. The TCP socket is local
     * (127.0.0.1) and does not go through adbd, so it cannot be killed by
     * WiFi instability or adbd timeout.
     *
     * The writer is a [BufferedWriter], not a [java.io.PrintStream], and that
     * is load-bearing: `PrintStream` never throws. It swallows the IOException
     * and sets an internal error flag, so the old `catch (t: Throwable)` in
     * this loop could never fire — write failures were invisible, and the
     * daemon went on pushing into a socket whose peer had been frozen for
     * minutes. A writer that throws is the whole point: the first failed write
     * is the moment we learn the client is gone.
     *
     * [mySession] keeps a thread from a finished session out of the next one.
     * Without it, a thread still parked in `write()` when its socket was closed
     * would wake up, throw against the *new* session's check, and drop a
     * client that had only just connected.
     */
    private fun startTcpSenderThread(tcpOut: BufferedWriter, mySession: Int) {
        val t = Thread({
            Log.i(TAG, "TCP sender thread started (session $mySession)")
            while (running && mySession == sessionId && !sessionDead) {
                val line = try {
                    sendQueue.poll(2, java.util.concurrent.TimeUnit.SECONDS)
                } catch (_: InterruptedException) {
                    break
                } ?: continue
                try {
                    synchronized(tcpOut) {
                        tcpOut.write(line)
                        tcpOut.write('\n'.code)
                        tcpOut.flush()
                    }
                } catch (t2: Throwable) {
                    if (mySession != sessionId || sessionDead) break
                    Log.w(TAG, "TCP write failed (${t2.javaClass.simpleName}: ${t2.message})")
                    killSession("write failed")
                    break
                }
            }
            Log.i(TAG, "TCP sender thread stopped (session $mySession)")
        }, "aimbot-tcp-sender")
        t.isDaemon = true
        senderThread = t
        t.start()
    }

    private fun replyOk() = send("OK")
    private fun replyOkValue(value: String) = send("OK:$value")
    private fun replyErr(message: String) = send("ERR:$message")

    private fun shutdown() {
        stopGeometryWatcher()
        stopCaptureSupervisor()
        stopRegionPump()
        runCatching { ShellLayerHost.stop() }
        stopReaderThread()
        runCatching {
            ShellNative.readerUngrab()
            ShellNative.readerClose()
            ShellNative.uinputClose()
        }.onFailure { Log.w(TAG, "cleanup failed", it) }
    }

    // ── Relay scratch cleanup ──────────────────────────────────────────────
    //
    // The relay used to ferry file-picker / IME asks via /data/local/tmp files.
    // The pump is gone, but the scratch directory (aimbot_req.json,
    // aimbot_resp/<id>) still lives there across daemon restarts. Wipe it once
    // at boot — after that, no further writes are produced, so the cleanup
    // never has to run again.

    private fun clearRelayScratch() {
        runCatching {
            RequestRelay.clearStaleResponses()
            RequestRelay.clearStaleRequests()
        }.onFailure { Log.w(TAG, "relay scratch cleanup failed", it) }
    }
}
