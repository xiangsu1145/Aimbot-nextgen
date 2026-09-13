package io.github.xiangsu1145.aimbotnextgen.adb

import android.util.Log
import java.security.SecureRandom
import javax.net.ssl.KeyManager
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.TrustManager

private const val TAG = "AdbConscrypt"

/**
 * One TLS "strategy" = one Conscrypt implementation, used for BOTH creating the
 * SSLSocket and exporting the RFC 5705 keying material.
 *
 * The rule is absolute: the export call must come from the exact same Conscrypt
 * that created the socket. Mixing them (bundled org.conscrypt.Conscrypt.exportKeyingMaterial
 * called on a platform conscrypt socket, or vice versa) fails at best with a
 * ClassCastException, at worst silently produces wrong key material.
 */
interface TlsStrategy {

    val name: String

    fun createSslContext(
        keyManagers: Array<KeyManager>?,
        trustManagers: Array<TrustManager>?,
        random: SecureRandom?
    ): SSLContext

    fun exportKeyingMaterial(socket: SSLSocket, label: String, size: Int): ByteArray
}

/**
 * Shizuku uses the platform Conscrypt (com.android.org.conscrypt) directly, which
 * is also what `SSLContext.getInstance("TLSv1.3")` returns on device. This strategy
 * reproduces that pairing exactly: default platform SSLContext + platform
 * Conscrypt export (reached by reflection, since the platform class is not part
 * of the SDK).
 */
private object PlatformConscrypt : TlsStrategy {

    override val name: String = "platform-conscrypt"

    private val conscryptClass: Class<*>? by lazy {
        runCatching { Class.forName("com.android.org.conscrypt.Conscrypt") }
            .onFailure { Log.w(TAG, "Platform Conscrypt class not visible", it) }
            .getOrNull()
    }

    private val exportMethod by lazy {
        conscryptClass?.let { clazz ->
            runCatching {
                clazz.getDeclaredMethod(
                    "exportKeyingMaterial",
                    SSLSocket::class.java,
                    String::class.java,
                    ByteArray::class.java,
                    Int::class.javaPrimitiveType
                ).apply { isAccessible = true }
            }.onFailure { Log.w(TAG, "Platform Conscrypt exportKeyingMaterial not visible", it) }
                .getOrNull()
        }
    }

    override fun createSslContext(
        keyManagers: Array<KeyManager>?,
        trustManagers: Array<TrustManager>?,
        random: SecureRandom?
    ): SSLContext {
        return SSLContext.getInstance("TLSv1.3").apply { init(keyManagers, trustManagers, random) }
    }

    override fun exportKeyingMaterial(socket: SSLSocket, label: String, size: Int): ByteArray {
        val method = exportMethod ?: error("platform Conscrypt unavailable")
        val result = method.invoke(null, socket, label, null, size) as? ByteArray
            ?: error("platform Conscrypt returned no data")
        require(result.size == size) { "platform Conscrypt returned ${result.size} bytes, expected $size" }
        return result
    }

    val available: Boolean get() = conscryptClass != null && exportMethod != null
}

/**
 * Fallback for the (rare) case where the platform Conscrypt cannot be reached:
 * bundle our own Conscrypt and use it consistently for both steps.
 */
private object BundledConscrypt : TlsStrategy {

    override val name: String = "bundled-conscrypt"

    private val provider: java.security.Provider? by lazy {
        runCatching { org.conscrypt.Conscrypt.newProvider() }
            .onFailure { Log.w(TAG, "Bundled Conscrypt provider unavailable", it) }
            .getOrNull()
    }

    override fun createSslContext(
        keyManagers: Array<KeyManager>?,
        trustManagers: Array<TrustManager>?,
        random: SecureRandom?
    ): SSLContext {
        val p = provider ?: error("bundled Conscrypt unavailable")
        return SSLContext.getInstance("TLSv1.3", p).apply { init(keyManagers, trustManagers, random) }
    }

    override fun exportKeyingMaterial(socket: SSLSocket, label: String, size: Int): ByteArray {
        if (provider == null) error("bundled Conscrypt unavailable")
        val result = org.conscrypt.Conscrypt.exportKeyingMaterial(socket, label, null, size)
        require(result.size == size) { "bundled Conscrypt returned ${result.size} bytes, expected $size" }
        return result
    }

    val available: Boolean get() = provider != null
}

object AdbConscrypt {

    /**
     * Ordered preference. Platform first because that is what Shizuku uses and
     * what adbd's own BoringSSL server was tested against.
     */
    val strategies: List<TlsStrategy> by lazy {
        val list = ArrayList<TlsStrategy>(2)
        if (PlatformConscrypt.available) {
            Log.i(TAG, "Strategy available: ${PlatformConscrypt.name}")
            list.add(PlatformConscrypt)
        } else {
            Log.w(TAG, "Platform Conscrypt strategy unavailable")
        }
        if (BundledConscrypt.available) {
            Log.i(TAG, "Strategy available: ${BundledConscrypt.name}")
            list.add(BundledConscrypt)
        }
        if (list.isEmpty()) Log.e(TAG, "No TLS strategy available -- pairing will fail")
        list
    }
}
