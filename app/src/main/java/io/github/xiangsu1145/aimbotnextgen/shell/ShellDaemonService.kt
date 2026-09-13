package io.github.xiangsu1145.aimbotnextgen.shell

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import io.github.xiangsu1145.aimbotnextgen.MainActivity
import io.github.xiangsu1145.aimbotnextgen.R
import io.github.xiangsu1145.aimbotnextgen.adb.AdbPairingState

/**
 * Foreground service that owns the [ShellManager] for as long as the user
 * wants the shell up.
 *
 * Why a service:
 *  - The manager keeps an ADB TCP socket open from a coroutine in this process.
 *    Without a foreground service the OS freezes the app into a deep standby
 *    bucket in 30-60s, the socket stalls, and the user sees "Shell 守护进程
 *    已断开" within minutes of backgrounding.
 *  - With a service we sit in the active bucket the whole time the
 *    notification is up. The persistent notification is the contract for
 *    keeping the socket warm.
 *
 * Why this matters specifically here:
 *  - The shell process itself (the `app_process` we launch via adb) is fine —
 *    it lives in shell UID and runs until its stdin closes. What dies is the
 *    *bridge* the app owns, the TCP session that talks to it.
 *  - This service owns the bridge. The shell process keeps running as long as
 *    this service holds the ADB session open.
 *
 * Lifecycle:
 *  - Started by [io.github.xiangsu1145.aimbotnextgen.MainActivity] via
 *    [startIfIdle] or [start]. Returns START_STICKY so a low-memory kill gets
 *    the system to bring us back; on revival we re-arm the supervisor via
 *    [onStartCommand] and try to reconnect.
 *  - The boot receiver [BootReceiver] calls [startIfIdle] when the device
 *    comes back from a reboot and the user had previously started the shell.
 *
 * Activity binding:
 *  - The Activity binds to read state and forward UI commands. Binding is
 *    optional — the service keeps running with no bound clients. We do not
 *    call stopSelf when the last client unbinds; the user's "启动" tap is the
 *    intent and their next "停止" tap is the exit.
 */
class ShellDaemonService : Service() {

    companion object {
        private const val TAG = "ShellDaemonService"

        const val CHANNEL_ID = "shell_daemon"

        private const val NOTIFICATION_ID = 2001

        const val ACTION_START_IF_IDLE = "io.github.xiangsu1145.aimbotnextgen.action.START_IF_IDLE"
        const val ACTION_STOP = "io.github.xiangsu1145.aimbotnextgen.action.STOP"

        /**
         * Brings the service up. Safe to call repeatedly; the OS de-duplicates
         * services that are already running.
         *
         * Must be `startForegroundService` (not `startService`) on API 26+ —
         * the service declares itself as foreground in onStartCommand and will
         * raise ForegroundServiceStartNotAllowedException if started from a
         * backgrounded app context (rare on this app's flow since we always
         * start it from a visible Activity).
         */
        fun start(context: Context) {
            val i = Intent(context, ShellDaemonService::class.java)
            ContextCompat.startForegroundService(context, i)
        }

        /**
         * Brings the service up and, once it is alive, kicks the manager into
         * auto-reconnect if the user had previously been running it (read from
         * SharedPreferences). On a fresh install this is a no-op for the
         * manager itself.
         */
        fun startIfIdle(context: Context) {
            val i = Intent(context, ShellDaemonService::class.java)
                .setAction(ACTION_START_IF_IDLE)
            ContextCompat.startForegroundService(context, i)
        }

        /** Asks the service to tear the daemon down. */
        fun stop(context: Context) {
            val i = Intent(context, ShellDaemonService::class.java)
                .setAction(ACTION_STOP)
            ContextCompat.startForegroundService(context, i)
        }
    }

    private lateinit var manager: ShellManager

    private val listeners = java.util.concurrent.CopyOnWriteArrayList<(ShellManager) -> Unit>()

    inner class LocalBinder : Binder() {
        fun getService(): ShellDaemonService = this@ShellDaemonService
        fun getManager(): ShellManager = manager
    }

    private val binder = LocalBinder()

    override fun onCreate() {
        super.onCreate()
        createChannel()
        manager = ShellManager(this)
        startInForeground()
        // Order matters above: the manager must be initialised before we read
        // wasRunning() / paired, otherwise we crash with UninitializedProperty.
        Log.i(TAG, "onCreate (was_running=${manager.wasRunning()} paired=${AdbPairingState.isPaired(this)})")
        // Print the OS-level background-freedom status so a logcat scrape
        // answers "is battery optimisation killing us?" without the user
        // having to navigate Settings.
        val pm = getSystemService(android.content.Context.POWER_SERVICE) as? android.os.PowerManager
        val ignoring = pm?.isIgnoringBatteryOptimizations(packageName)
        Log.i(TAG, "battery_optimisation ignoring=$ignoring (false == OS will freeze us in background)")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Always (re)raise to foreground — START_STICKY replays this with a
        // null intent, and the service must re-arm its foreground state or the
        // system kills it within 5s.
        startInForeground()

        when (intent?.action) {
            ACTION_STOP -> {
                Log.i(TAG, "ACTION_STOP")
                manager.stop()
            }
            ACTION_START_IF_IDLE -> {
                // Explicit user / boot-receiver ask. The wasRunning gate is
                // what makes the boot receiver a no-op on a fresh install: the
                // user has to actually press 启动 once before we are willing to
                // resume on boot.
                Log.i(TAG, "ACTION_START_IF_IDLE (wasRunning=${manager.wasRunning()})")
                if (manager.wasRunning() && AdbPairingState.isPaired(this)) {
                    manager.startIfIdle()
                }
            }
            // No `else` branch on purpose. START_STICKY replay with a null intent
            // used to fall through here and call startIfIdle() if wasRunning was
            // true — which, on a device whose OEM kills the daemon process
            // minutes into a session (OPPO ColorOS), turned into an infinite
            // "disconnect → auto-relaunch → disconnect → ..." loop. The user's
            // instruction is "show me a clear error and stop; let me retry by
            // hand", so a stale service replay does nothing — the daemon is
            // dead and stays dead until the next manual press.
        }

        // NOT_STICKY: combine with the no-op replay above to fully drop the
        // "system restarts us → we silently try the daemon again" path. If the
        // OS kills us under memory pressure, the user has to relaunch the app
        // (or press 启动 when it is in front), which is exactly what they asked
        // for. The BootReceiver still starts us on boot via ACTION_START_IF_IDLE.
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        Log.i(TAG, "onDestroy")
        super.onDestroy()
    }

    // ── Foreground notification ────────────────────────────────────────────

    private fun createChannel() {
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, getString(R.string.shell_channel_name),
                NotificationManager.IMPORTANCE_LOW).apply {
                description = getString(R.string.shell_channel_desc)
                setShowBadge(false)
                setSound(null, null)
            }
        )
    }

    private fun startInForeground() {
        val tap = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val n = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_shell)
            .setContentTitle(getString(R.string.shell_notif_title_running))
            .setContentText(getString(R.string.shell_notif_text_idle))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setContentIntent(tap)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID, n,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            )
        } else {
            startForeground(NOTIFICATION_ID, n)
        }
    }
}
