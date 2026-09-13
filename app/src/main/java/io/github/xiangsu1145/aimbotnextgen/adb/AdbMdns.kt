package io.github.xiangsu1145.aimbotnextgen.adb

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.net.InetAddress
import java.net.NetworkInterface

private const val TAG = "AdbMdns"

/**
 * Discovers adbd's mDNS service (pairing or connect) and reports the port.
 *
 * Two problems are solved here that previously made pairing unusable:
 *
 * 1. **Identity check must not depend on a port probe.** Shizuku additionally
 *    requires `bind(127.0.0.1, port)` to fail. adbd does not always bind the
 *    pairing port on loopback, so the probe succeeds and the *valid* port is
 *    dropped. We decide "is this our own device" purely from the advertised
 *    host address.
 *
 * 2. **A stale discovery never sees services advertised later.** When the user
 *    turns wireless debugging on *after* this app started scanning, an already
 *    established NsdManager discovery often stays silent. Hence [restart],
 *    driven by AdbPairingService's rescan timer and the `adb_wifi_enabled`
 *    observer, plus a multicast lock (several OEM roms drop mDNS otherwise).
 *
 * Lifecycle is a single source of truth: [wantRunning] is the requested state,
 * [phase] is what NsdManager has confirmed. Every callback reconciles the two,
 * so overlapping start()/stop()/restart() can no longer leak listeners.
 */
class AdbMdns(
    private val context: Context,
    private val serviceType: String,
    private val onPortFound: (Int) -> Unit,
    private val onServiceLost: () -> Unit = {}
) {

    private enum class Phase { Idle, Starting, Running, Stopping }

    private val nsdManager: NsdManager = context.getSystemService(NsdManager::class.java)
    private val mainHandler = Handler(Looper.getMainLooper())
    
    private var phase = Phase.Idle
    private var wantRunning = false

    private var adbdServiceName: String? = null
    private var lastReportedPort = -1

    private var multicastLock: WifiManager.MulticastLock? = null

    private val discoveryListener = DiscoveryListenerImpl()
    private val resolveListener = ResolveListenerImpl()

    fun start() {
        mainHandler.post { doStart() }
    }

    fun stop() {
        mainHandler.post { doStop() }
    }

    /**
     * Drop the current discovery and begin a fresh one. Needed whenever the
     * device may have started advertising the service after we began scanning.
     */
    fun restart() {
        mainHandler.post { doRestart() }
    }

    private fun doStart() {
        Log.i(TAG, "start() phase=$phase")
        wantRunning = true
        if (phase == Phase.Idle) beginDiscovery()
    }

    private fun doStop() {
        Log.i(TAG, "stop() phase=$phase")
        wantRunning = false
        when (phase) {
            Phase.Running -> requestSystemStop()
            Phase.Starting -> phase = Phase.Stopping // confirmed handled in onDiscoveryStarted
            Phase.Idle, Phase.Stopping -> Unit
        }
    }

    private fun doRestart() {
        Log.i(TAG, "restart() phase=$phase")
        wantRunning = true
        when (phase) {
            Phase.Idle -> beginDiscovery()
            Phase.Running -> requestSystemStop() // will be restarted from onDiscoveryStopped
            Phase.Starting, Phase.Stopping -> Unit // already in flight, the callbacks will settle it
        }
    }

    private fun beginDiscovery() {
        acquireMulticastLock()
        // Forget any previously resolved port/name. adbd picks a fresh random
        // port every time wireless debugging is toggled, so a stale cached port
        // would make pairing target the wrong endpoint. Clearing here guarantees
        // the next resolve re-reports whatever adbd advertises now.
        lastReportedPort = -1
        adbdServiceName = null
        runCatching {
            Log.i(TAG, "discoverServices($serviceType)")
            nsdManager.discoverServices(serviceType, NsdManager.PROTOCOL_DNS_SD, discoveryListener)
            phase = Phase.Starting
        }.onFailure {
            Log.e(TAG, "discoverServices($serviceType) threw", it)
            phase = Phase.Idle
            releaseMulticastLock()
        }
    }

    private fun requestSystemStop() {
        phase = Phase.Stopping
        runCatching {
            nsdManager.stopServiceDiscovery(discoveryListener)
            Log.i(TAG, "stopServiceDiscovery($serviceType) requested")
        }.onFailure {
            Log.e(TAG, "stopServiceDiscovery threw", it)
            phase = Phase.Idle
            releaseMulticastLock()
        }
    }

    private fun acquireMulticastLock() {
        runCatching {
            val wifi = context.applicationContext.getSystemService(WifiManager::class.java) ?: return
            val lock = multicastLock ?: wifi.createMulticastLock("adb-mdns").apply {
                setReferenceCounted(true)
            }
            multicastLock = lock
            if (!lock.isHeld) {
                lock.acquire()
                Log.i(TAG, "MulticastLock acquired")
            }
        }.onFailure { Log.w(TAG, "MulticastLock unavailable", it) }
    }

    private fun releaseMulticastLock() {
        runCatching {
            multicastLock?.takeIf { it.isHeld }?.release()
        }.onFailure { Log.w(TAG, "releasing MulticastLock failed", it) }
    }

    private fun onServiceResolved(info: NsdServiceInfo) {
        if (!wantRunning) {
            Log.d(TAG, "Ignoring ${info.serviceName}: discovery no longer wanted")
            return
        }
        val host = info.host
        val port = info.port
        if (host == null || port <= 0) {
            Log.w(TAG, "Resolved ${info.serviceName} without usable host/port (host=$host port=$port)")
            return
        }
        if (!isThisDevice(host)) {
            Log.i(TAG, "Ignoring ${info.serviceName} at ${host.hostAddress}: another device")
            return
        }

        adbdServiceName = info.serviceName
        if (port == lastReportedPort) {
            Log.d(TAG, "Port $port already reported, skipping duplicate")
            return
        }
        lastReportedPort = port
        Log.i(TAG, "Reporting $serviceType port $port (${info.serviceName})")
        mainHandler.post { onPortFound(port) }
    }

    private fun isThisDevice(host: InetAddress): Boolean {
        if (runCatching { NetworkInterface.getByInetAddress(host) != null }.getOrDefault(false)) {
            return true
        }
        // Fallback for roms that fail reverse lookups; compare plain strings and
        // ignore IPv6 scope ids.
        val wanted = host.hostAddress?.substringBefore('%') ?: return false
        return runCatching {
            NetworkInterface.getNetworkInterfaces().asSequence()
                .flatMap { it.inetAddresses.asSequence() }
                .any { it.hostAddress?.substringBefore('%') == wanted }
        }.getOrDefault(false)
    }

    private inner class DiscoveryListenerImpl : NsdManager.DiscoveryListener {

        override fun onDiscoveryStarted(type: String) {
            Log.i(TAG, "onDiscoveryStarted($type) phase=$phase wantRunning=$wantRunning")
            if (!wantRunning || phase == Phase.Stopping) {
                requestSystemStop()
                return
            }
            phase = Phase.Running
        }

        override fun onDiscoveryStopped(type: String) {
            Log.i(TAG, "onDiscoveryStopped($type) wantRunning=$wantRunning")
            phase = Phase.Idle
            releaseMulticastLock()
            if (wantRunning) beginDiscovery()
        }

        override fun onStartDiscoveryFailed(type: String, errorCode: Int) {
            Log.e(TAG, "onStartDiscoveryFailed($type) error=$errorCode")
            phase = Phase.Idle
            releaseMulticastLock()
            if (wantRunning) {
                mainHandler.postDelayed(retryRunnable, RETRY_DELAY_MS)
            }
        }

        override fun onStopDiscoveryFailed(type: String, errorCode: Int) {
            Log.e(TAG, "onStopDiscoveryFailed($type) error=$errorCode")
            phase = Phase.Idle
            releaseMulticastLock()
        }

        override fun onServiceFound(info: NsdServiceInfo) {
            Log.i(TAG, "onServiceFound: ${info.serviceName}")
            if (!wantRunning) return
            runCatching { nsdManager.resolveService(info, resolveListener) }
                .onFailure { Log.e(TAG, "resolveService threw", it) }
        }

        override fun onServiceLost(info: NsdServiceInfo) {
            Log.i(TAG, "onServiceLost: ${info.serviceName}")
            if (info.serviceName == adbdServiceName) {
                adbdServiceName = null
                lastReportedPort = -1
                mainHandler.post { onServiceLost() }
            }
        }
    }

    private val retryRunnable = Runnable {
        if (wantRunning && phase == Phase.Idle) beginDiscovery()
    }

    private inner class ResolveListenerImpl : NsdManager.ResolveListener {
        override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {
            Log.e(TAG, "onResolveFailed: ${info.serviceName}, error=$errorCode")
        }

        override fun onServiceResolved(info: NsdServiceInfo) {
            this@AdbMdns.onServiceResolved(info)
        }
    }

    companion object {
        private const val RETRY_DELAY_MS = 1_000L

        const val TLS_CONNECT = "_adb-tls-connect._tcp"
        const val TLS_PAIRING = "_adb-tls-pairing._tcp"
    }
}
