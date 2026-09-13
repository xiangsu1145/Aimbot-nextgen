package io.github.xiangsu1145.aimbotnextgen.adb

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.database.ContentObserver
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.RemoteInput
import io.github.xiangsu1145.aimbotnextgen.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelChildren
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import java.net.ConnectException
import java.net.SocketTimeoutException

/**
 * Foreground service driving the wireless-debugging pairing flow.
 *
 * Notification policy (this is where the "one 正在配对 + one 配对成功" bug came
 * from): the whole flow uses a *single* notification id. While searching we own
 * it as the foreground notification; on the terminal result we explicitly
 * cancel it, detach from the foreground state (DETACH keeps any notification we
 * post afterwards alive, unlike REMOVE), then post the result on that same id.
 * There is therefore never more than one notification, whatever happens.
 */
class AdbPairingService : Service() {

    companion object {
        const val CHANNEL_ID = "adb_pairing"

        private const val TAG = "AdbPairingService"

        private const val NOTIFICATION_ID = 1001

        const val ACTION_START = "start"
        const val ACTION_STOP = "stop"
        const val ACTION_REPLY = "reply"
        /** Automation / CLI entry: `-a pair --ei port P --es pairing_code C` */
        const val ACTION_PAIR = "pair"

        const val EXTRA_PORT = "port"
        const val EXTRA_PAIRING_CODE = "pairing_code"

        private const val REMOTE_INPUT_KEY = "pairing_code"
        private const val ADB_WIFI_ENABLED = "adb_wifi_enabled"

        private const val PAIR_TIMEOUT_MS = 30_000L
        private const val RESCAN_INTERVAL_MS = 12_000L

        private const val REQ_CONTENT = 10
        private const val REQ_REPLY = 11
        private const val REQ_STOP = 12
        private const val REQ_RETRY = 13

        fun intent(context: Context, action: String): Intent =
            Intent(context, AdbPairingService::class.java).setAction(action)
    }

    private enum class Stage { Idle, Searching, Found, Working, Finished }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    private var adbMdns: AdbMdns? = null
    private var stage = Stage.Idle
    private var discoveredPort = -1
    private var wifiObserver: ContentObserver? = null
    private var rescanJob: Job? = null
    private var wirelessDebuggingOn = false

    // ── Lifecycle ─────────────────────────────────────────────────────────

    override fun onCreate() {
        super.onCreate()
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "ADB 配对", NotificationManager.IMPORTANCE_HIGH).apply {
                description = "用于输入 ADB 配对码"
                setSound(null, null)
                setShowBadge(false)
            }
        )
        wirelessDebuggingOn = isWirelessDebuggingEnabled()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val action = intent?.action
        Log.i(TAG, "onStartCommand action=$action stage=$stage")
        when (action) {
            ACTION_STOP -> finish()
            ACTION_REPLY -> {
                val port = intent.getIntExtra(EXTRA_PORT, discoveredPort)
                val code = RemoteInput.getResultsFromIntent(intent)
                    ?.getCharSequence(REMOTE_INPUT_KEY)
                    ?.toString()
                    ?.trim()
                    .orEmpty()
                Log.i(TAG, "Reply received, port=$port codeLength=${code.length}")
                if (code.isNotEmpty() && port > 0) {
                    doPairing(code, port)
                } else {
                    Log.w(TAG, "Reply rejected: code empty=${code.isEmpty()}, port=$port")
                    if (stage == Stage.Found && port > 0) notifyInput(port)
                }
            }
            ACTION_PAIR -> {
                val port = intent.getIntExtra(EXTRA_PORT, discoveredPort)
                val code = intent.getStringExtra(EXTRA_PAIRING_CODE)?.trim().orEmpty()
                if (code.isNotEmpty() && port > 0) doPairing(code, port)
                else Log.w(TAG, "Pair rejected: code empty=${code.isEmpty()}, port=$port")
            }
            else -> {
                // Anything else -- including ACTION_START and the very first
                // intent -- (re)starts the search. Repeated delivery is safe.
                if (stage == Stage.Finished) finish()
                else startSearch()
            }
        }
        // NOT_STICKY: a stale intent must never restart the service back into
        // an old "正在配对" state after the process was killed and recreated.
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        Log.i(TAG, "onDestroy stage=$stage")
        stopBackgroundWork()
        super.onDestroy()
    }

    // ── Searching ─────────────────────────────────────────────────────────

    private fun startSearch() {
        Log.i(TAG, "startSearch() from stage=$stage")
        if (stage == Stage.Working || stage == Stage.Finished) return

        discoveredPort = -1
        stage = Stage.Searching
        wirelessDebuggingOn = isWirelessDebuggingEnabled()

        val mdns = adbMdns ?: AdbMdns(
            this,
            AdbMdns.TLS_PAIRING,
            onPortFound = { port -> onPairingServiceFound(port) },
            onServiceLost = { onPairingServiceLost() }
        ).also { adbMdns = it }

        notifySearching(enterForeground = true)
        mdns.restart()

        observeWirelessDebugging(true)
        startRescanLoop()
    }

    private fun startRescanLoop() {
        rescanJob?.cancel()
        rescanJob = scope.launch {
            // Keep rescanning while we are actively looking OR have a cached
            // service. adbd changes its port on every wireless-debugging toggle,
            // and NsdManager does not reliably deliver onServiceLost, so a
            // periodic restart is what actually catches the new port.
            while (stage == Stage.Searching || stage == Stage.Found) {
                delay(RESCAN_INTERVAL_MS)
                if (stage != Stage.Searching && stage != Stage.Found) break
                Log.i(TAG, "Rescan tick, restarting discovery")
                adbMdns?.restart()
            }
        }
    }

    private fun onPairingServiceFound(port: Int) {
        Log.i(TAG, "Pairing service found on port $port (stage=$stage)")
        mainHandler.post {
            if (stage != Stage.Searching && stage != Stage.Found) {
                Log.d(TAG, "Ignoring port $port, stage=$stage")
                return@post
            }
            if (stage == Stage.Found && port == discoveredPort) {
                // Same port we already show; the rescan loop is what keeps this
                // fresh, so don't re-post the notification (avoids flicker).
                Log.d(TAG, "Port $port unchanged, keeping current notification")
                return@post
            }
            discoveredPort = port
            stage = Stage.Found
            notifyInput(port)
        }
    }

    private fun onPairingServiceLost() {
        Log.i(TAG, "Pairing service lost (stage=$stage)")
        mainHandler.post {
            if (stage != Stage.Found && stage != Stage.Searching) return@post
            discoveredPort = -1
            stage = Stage.Searching
            wirelessDebuggingOn = isWirelessDebuggingEnabled()
            notifySearching(enterForeground = false)
            adbMdns?.restart()
            startRescanLoop()
        }
    }

    private fun observeWirelessDebugging(register: Boolean) {
        if (register) {
            if (wifiObserver != null) return
            val observer = object : ContentObserver(mainHandler) {
                override fun onChange(selfChange: Boolean, uri: Uri?) = handleWirelessDebuggingChange()
            }
            runCatching {
                contentResolver.registerContentObserver(
                    Settings.Global.getUriFor(ADB_WIFI_ENABLED), false, observer
                )
                wifiObserver = observer
            }.onFailure { Log.w(TAG, "registerContentObserver failed", it) }
        } else {
            wifiObserver?.let { runCatching { contentResolver.unregisterContentObserver(it) } }
            wifiObserver = null
        }
    }

    private fun handleWirelessDebuggingChange() {
        val enabled = isWirelessDebuggingEnabled()
        Log.i(TAG, "$ADB_WIFI_ENABLED -> $enabled (stage=$stage)")
        val changed = enabled != wirelessDebuggingOn
        wirelessDebuggingOn = enabled
        if (!changed) return
        // Wireless debugging was toggled. The adbd service set changed and the
        // port we cached (or already "found") is now stale: adbd picks a new
        // random port every time. Drop it unconditionally and re-search so a
        // fresh resolve reports the current port. This is also the fix for the
        // "turned wireless debugging off but it still shows the pairing service"
        // bug -- previous code only reacted while in the Searching stage, so a
        // cached Found state was never cleared.
        Log.i(TAG, "Wireless debugging toggled, resetting stale discovery and re-searching")
        discoveredPort = -1
        stage = Stage.Searching
        notifySearching(enterForeground = false)
        adbMdns?.restart()
        startRescanLoop()
    }

    private fun isWirelessDebuggingEnabled(): Boolean = try {
        Settings.Global.getInt(contentResolver, ADB_WIFI_ENABLED, 0) != 0
    } catch (e: Exception) {
        Log.w(TAG, "reading $ADB_WIFI_ENABLED failed", e)
        false
    }

    // ── Pairing ───────────────────────────────────────────────────────────

    private fun doPairing(code: String, port: Int) {
        if (stage == Stage.Working) {
            Log.w(TAG, "Pairing already in progress, ignoring duplicate")
            return
        }
        stage = Stage.Working
        discoveredPort = port
        rescanJob?.cancel()
        observeWirelessDebugging(false)
        adbMdns?.stop()
        notifyWorking(port)

        scope.launch {
            var client: AdbPairingClient? = null
            val outcome: Pair<Boolean, Throwable?> = try {
                withTimeout(PAIR_TIMEOUT_MS) {
                    Log.i(TAG, "Loading AdbKey")
                    val key = AdbKey(this@AdbPairingService)
                    Log.i(TAG, "Pairing against 127.0.0.1:$port")
                    client = AdbPairingClient("127.0.0.1", port, code, key)
                    client.start() to null
                }
            } catch (e: Throwable) {
                Log.e(TAG, "Pairing failed", e)
                false to e
            } finally {
                runCatching { client?.close() }
                    .onFailure { Log.w(TAG, "closing AdbPairingClient failed", it) }
            }
            Log.i(TAG, "Pairing finished success=${outcome.first}")
            withContext(Dispatchers.Main) {
                showResult(outcome.first, port, outcome.second)
            }
        }
    }

    private fun showResult(success: Boolean, port: Int, error: Throwable?) {
        val text: String
        if (success) {
            Log.i(TAG, "Pairing succeeded")
            text = "点击前往启动 Shell 服务"
        } else {
            text = when (error) {
                is ConnectException -> "无法连接到端口 $port，请重新打开「使用配对码配对设备」"
                is AdbInvalidPairingCodeException -> "配对码错误或已过期，请重新获取配对码"
                is AdbKeyException -> "密钥存储错误: ${error.message}"
                is SocketTimeoutException -> "超时：adbd 未响应"
                is kotlinx.coroutines.TimeoutCancellationException -> "超时：adbd 未在 ${PAIR_TIMEOUT_MS / 1000}s 内响应"
                null -> "adbd 拒绝了本次配对"
                else -> error.message ?: "未知错误"
            }
        }

        val detail = buildString {
            append(text)
            if (!success && error != null) {
                append("\n")
                append(error.javaClass.simpleName)
                error.message?.let { append(": $it") }
            }
        }

        AdbPairingState.setPaired(this, success)

        // Shizuku parity: STOP_FOREGROUND_REMOVE tears down the in-progress
        // "正在配对" foreground notification, then we re-post on the SAME id.
        // The shade therefore never holds both a "正在配对" and a "配对成功"
        // entry -- there is exactly one.
        val nm = getSystemService(NotificationManager::class.java)
        stopForegroundRemoving()
        nm.notify(NOTIFICATION_ID, buildResultNotification(success, text, detail))
        stopBackgroundWork()

        sendBroadcast(
            Intent(AdbPairingState.ACTION_PAIRING_RESULT)
                .setPackage(packageName)
                .putExtra(AdbPairingState.EXTRA_SUCCESS, success)
                .putExtra(AdbPairingState.EXTRA_MESSAGE, text)
        )

        stage = Stage.Finished
        stopSelf()
    }

    /** Called when the user cancels; leaves nothing behind. */
    private fun finish() {
        Log.i(TAG, "finish()")
        stopForegroundRemoving()
        getSystemService(NotificationManager::class.java).cancel(NOTIFICATION_ID)
        stopBackgroundWork()
        stage = Stage.Finished
        stopSelf()
    }

    private fun stopForegroundRemoving() {
        runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                stopForeground(STOP_FOREGROUND_REMOVE)
            } else {
                @Suppress("DEPRECATION")
                stopForeground(true)
            }
        }.onFailure { Log.w(TAG, "stopForeground failed", it) }
    }

    private fun stopBackgroundWork() {
        rescanJob?.cancel()
        rescanJob = null
        mainHandler.removeCallbacksAndMessages(null)
        observeWirelessDebugging(false)
        runCatching { adbMdns?.stop() }
        adbMdns = null
        scope.coroutineContext.cancelChildren()
    }

    // ── Notifications ─────────────────────────────────────────────────────

    private fun baseBuilder(): NotificationCompat.Builder =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setOnlyAlertOnce(true)

    private fun notifySearching(enterForeground: Boolean) {
        // Shizuku parity: the searching notification carries a single, fixed
        // message. Whether wireless debugging is on is taught in
        // AdbPairingTutorialActivity, not here. Branching on that state here is
        // exactly what produced the stale "请打开无线调试" line after the user
        // enabled it -- so we no longer do it.
        val text = "正在搜索「无线调试」配对服务…"
        val builder = baseBuilder()
            .setContentTitle("正在搜索配对服务…")
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setContentIntent(tutorialPendingIntent())
            .addAction(0, "重新搜索", actionPendingIntent(ACTION_START, REQ_RETRY))
            .addAction(0, "停止", actionPendingIntent(ACTION_STOP, REQ_STOP))
            .setOngoing(true)
        publish(builder.build(), enterForeground)
    }

    private fun notifyInput(port: Int) {
        val remoteInput = RemoteInput.Builder(REMOTE_INPUT_KEY)
            .setLabel("输入配对码")
            .build()
        val replyIntent = Intent(this, AdbPairingService::class.java)
            .setAction(ACTION_REPLY)
            .putExtra(EXTRA_PORT, port)
        val replyPending = PendingIntent.getForegroundService(
            this, REQ_REPLY, replyIntent,
            PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val builder = baseBuilder()
            .setContentTitle("发现配对服务（端口 $port）")
            .setContentText("点击「输入配对码」填写 6 位配对码")
            .setContentIntent(tutorialPendingIntent())
            .addAction(
                NotificationCompat.Action.Builder(0, "输入配对码", replyPending)
                    .addRemoteInput(remoteInput)
                    .build()
            )
            .addAction(0, "停止", actionPendingIntent(ACTION_STOP, REQ_STOP))
            .setOngoing(true)
        publish(builder.build(), false)
    }

    private fun notifyWorking(port: Int) {
        val builder = baseBuilder()
            .setContentTitle("正在配对...")
            .setContentText("127.0.0.1:$port")
            .setContentIntent(tutorialPendingIntent())
            .addAction(0, "停止", actionPendingIntent(ACTION_STOP, REQ_STOP))
            .setOngoing(true)
        publish(builder.build(), false)
    }

    private fun buildResultNotification(success: Boolean, text: String, detail: String): Notification {
        val builder = baseBuilder()
            .setContentTitle(if (success) "配对成功" else "配对失败")
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(detail))
            .setOngoing(false)
            .setAutoCancel(true)
            .setContentIntent(
                PendingIntent.getActivity(
                    this, REQ_CONTENT,
                    Intent(this, MainActivity::class.java).apply {
                        flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                            Intent.FLAG_ACTIVITY_CLEAR_TOP or
                            Intent.FLAG_ACTIVITY_SINGLE_TOP
                        putExtra(MainActivity.EXTRA_START_SHELL, success)
                    },
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                )
            )
        if (!success) {
            builder.addAction(0, "重新搜索", actionPendingIntent(ACTION_START, REQ_RETRY))
        }
        return builder.build()
    }

    private fun publish(notification: Notification, enterForeground: Boolean) {
        val nm = getSystemService(NotificationManager::class.java)
        if (enterForeground) {
            runCatching {
                startForeground(
                    NOTIFICATION_ID,
                    notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST
                )
            }.onFailure {
                Log.e(TAG, "startForeground failed, falling back to plain notify", it)
                nm.notify(NOTIFICATION_ID, notification)
            }
        } else {
            nm.notify(NOTIFICATION_ID, notification)
        }
    }

    private fun actionPendingIntent(action: String, requestCode: Int): PendingIntent =
        PendingIntent.getForegroundService(
            this, requestCode,
            intent(this, action),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

    private fun tutorialPendingIntent(): PendingIntent =
        PendingIntent.getActivity(
            this, REQ_CONTENT,
            Intent(this, io.github.xiangsu1145.aimbotnextgen.adb.AdbPairingTutorialActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                    Intent.FLAG_ACTIVITY_CLEAR_TOP or
                    Intent.FLAG_ACTIVITY_SINGLE_TOP
            },
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
}
