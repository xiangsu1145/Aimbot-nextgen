package io.github.xiangsu1145.aimbotnextgen.adb

import android.util.Log
import java.io.Closeable
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.net.ssl.SSLSocket

private const val TAG = "AdbPairClient"

/** Never let a dead/blocked peer freeze the UI: every step is bounded. */
private const val CONNECT_TIMEOUT_MS = 5_000
private const val READ_TIMEOUT_MS = 10_000

private const val kCurrentKeyHeaderVersion = 1.toByte()
private const val kMinSupportedKeyHeaderVersion = 1.toByte()
private const val kMaxSupportedKeyHeaderVersion = 1.toByte()
private const val kMaxPeerInfoSize = 8192
private const val kMaxPayloadSize = kMaxPeerInfoSize * 2

private const val kExportedKeyLabel = "adb-label\u0000"
private const val kExportedKeySize = 64

private const val kPairingPacketHeaderSize = 6

private class PeerInfo(
    val type: Byte,
    data: ByteArray
) {
    val data = ByteArray(kMaxPeerInfoSize - 1)

    init {
        data.copyInto(this.data, 0, 0, data.size.coerceAtMost(kMaxPeerInfoSize - 1))
    }

    enum class Type(val value: Byte) {
        ADB_RSA_PUB_KEY(0.toByte()),
        ADB_DEVICE_GUID(0.toByte()),
    }

    fun writeTo(buffer: ByteBuffer) {
        buffer.run {
            put(type)
            put(data)
        }
        Log.d(TAG, "write PeerInfo ${toStringShort()}")
    }

    override fun toString(): String = "PeerInfo(${toStringShort()})"

    fun toStringShort(): String = "type=$type, data=${data.contentToString()}"

    companion object {
        fun readFrom(buffer: ByteBuffer): PeerInfo {
            val type = buffer.get()
            val data = ByteArray(kMaxPeerInfoSize - 1)
            buffer.get(data)
            return PeerInfo(type, data)
        }
    }
}

private class PairingPacketHeader(
    val version: Byte,
    val type: Byte,
    val payload: Int
) {
    enum class Type(val value: Byte) {
        SPAKE2_MSG(0.toByte()),
        PEER_INFO(1.toByte())
    }

    fun writeTo(buffer: ByteBuffer) {
        buffer.run {
            put(version)
            put(type)
            putInt(payload)
        }
        Log.d(TAG, "write PairingPacketHeader ${toStringShort()}")
    }

    override fun toString(): String = "PairingPacketHeader(${toStringShort()})"

    fun toStringShort(): String = "version=${version.toInt()}, type=${type.toInt()}, payload=$payload"

    companion object {
        fun readFrom(buffer: ByteBuffer): PairingPacketHeader? {
            val version = buffer.get()
            val type = buffer.get()
            val payload = buffer.int

            if (version < kMinSupportedKeyHeaderVersion || version > kMaxSupportedKeyHeaderVersion) {
                Log.e(TAG, "PairingPacketHeader version mismatch (us=$kCurrentKeyHeaderVersion them=${version})")
                return null
            }
            if (type != Type.SPAKE2_MSG.value && type != Type.PEER_INFO.value) {
                Log.e(TAG, "Unknown PairingPacket type=$type")
                return null
            }
            if (payload <= 0 || payload > kMaxPayloadSize) {
                Log.e(TAG, "header payload not within a safe payload size (size=${payload})")
                return null
            }

            val header = PairingPacketHeader(version, type, payload)
            Log.d(TAG, "read PairingPacketHeader ${header.toStringShort()}")
            return header
        }
    }
}

private class PairingContext private constructor(private val nativePtr: Long) {

    val msg: ByteArray

    init {
        msg = nativeMsg(nativePtr)
    }

    fun initCipher(theirMsg: ByteArray) = nativeInitCipher(nativePtr, theirMsg)

    fun encrypt(`in`: ByteArray) = nativeEncrypt(nativePtr, `in`)

    fun decrypt(`in`: ByteArray) = nativeDecrypt(nativePtr, `in`)

    fun destroy() = nativeDestroy(nativePtr)

    private external fun nativeMsg(nativePtr: Long): ByteArray
    private external fun nativeInitCipher(nativePtr: Long, theirMsg: ByteArray): Boolean
    private external fun nativeEncrypt(nativePtr: Long, inbuf: ByteArray): ByteArray?
    private external fun nativeDecrypt(nativePtr: Long, inbuf: ByteArray): ByteArray?
    private external fun nativeDestroy(nativePtr: Long)

    companion object {
        fun create(password: ByteArray): PairingContext? {
            val nativePtr = nativeConstructor(true, password)
            return if (nativePtr != 0L) PairingContext(nativePtr) else null
        }

        @JvmStatic
        private external fun nativeConstructor(isClient: Boolean, password: ByteArray): Long
    }
}

class AdbPairingClient(
    private val host: String,
    private val port: Int,
    private val pairCode: String,
    private val key: AdbKey
) : Closeable {

    private enum class State {
        Ready,
        ExchangingMsgs,
        ExchangingPeerInfo,
        Stopped
    }

    private lateinit var socket: Socket
    private lateinit var inputStream: DataInputStream
    private lateinit var outputStream: DataOutputStream
    private var sslSocket: SSLSocket? = null

    private val peerInfo: PeerInfo = PeerInfo(PeerInfo.Type.ADB_RSA_PUB_KEY.value, key.adbPublicKey)
    private lateinit var pairingContext: PairingContext
    private var state: State = State.Ready

    fun start(): Boolean {
        Log.i(TAG, "Pairing $host:$port, key material label len=${kExportedKeyLabel.length}")
        setupTlsConnection()

        state = State.ExchangingMsgs

        if (!doExchangeMsgs()) {
            state = State.Stopped
            return false
        }

        state = State.ExchangingPeerInfo

        if (!doExchangePeerInfo()) {
            state = State.Stopped
            return false
        }

        state = State.Stopped
        return true
    }

    /**
     * TLS handshake + keying-material export.
     *
     * The socket creator and the exporter must belong to the same Conscrypt
     * (see [TlsStrategy]); otherwise the internal cast fails. Each strategy
     * owns both steps, and we only fall through to the next one before any
     * protocol byte has been written to the socket.
     */
    private fun setupTlsConnection() {
        if (AdbConscrypt.strategies.isEmpty()) {
            throw AdbException("设备上没有可用的 Conscrypt, 无法导出 TLS 密钥材料")
        }

        val errors = LinkedHashMap<String, String>()
        for (strategy in AdbConscrypt.strategies) {
            runCatching { tryStrategy(strategy) }
                .onSuccess { return }
                .onFailure { e ->
                    Log.e(TAG, "Strategy ${strategy.name} failed", e)
                    errors[strategy.name] = e.logLine()
                    resetConnection()
                }
        }

        throw IllegalStateException(
            "TLS 握手/密钥导出失败 -> " + errors.entries.joinToString(" | ") { "${it.key}: ${it.value}" }
        )
    }

    private fun tryStrategy(strategy: TlsStrategy) {
        Log.i(TAG, "[${strategy.name}] connecting $host:$port (connect=${CONNECT_TIMEOUT_MS}ms, read=${READ_TIMEOUT_MS}ms)")

        val raw = Socket()
        raw.tcpNoDelay = true
        raw.soTimeout = READ_TIMEOUT_MS
        raw.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
        socket = raw
        Log.d(TAG, "[${strategy.name}] TCP connected on port $port")

        val negotiated = key.sslContext(strategy).socketFactory
            .createSocket(raw, host, port, true) as SSLSocket
        negotiated.soTimeout = READ_TIMEOUT_MS
        sslSocket = negotiated
        Log.d(TAG, "[${strategy.name}] enabled protocols=${negotiated.enabledProtocols.contentToString()}")

        negotiated.startHandshake()
        Log.i(
            TAG,
            "[${strategy.name}] handshake OK: ${negotiated.session.protocol}/${negotiated.session.cipherSuite} " +
                "on ${negotiated.javaClass.name}"
        )

        inputStream = DataInputStream(negotiated.inputStream)
        outputStream = DataOutputStream(negotiated.outputStream)

        val pairCodeBytes = pairCode.toByteArray()
        val keyMaterial = strategy.exportKeyingMaterial(negotiated, kExportedKeyLabel, kExportedKeySize)
        Log.i(TAG, "[${strategy.name}] exported ${keyMaterial.size} bytes of keying material")

        val passwordBytes = ByteArray(pairCodeBytes.size + keyMaterial.size)
        pairCodeBytes.copyInto(passwordBytes)
        keyMaterial.copyInto(passwordBytes, pairCodeBytes.size)

        val context = PairingContext.create(passwordBytes)
        checkNotNull(context) { "Unable to create PairingContext." }
        pairingContext = context
        Log.d(TAG, "PairingContext ready, our SPAKE2 msg=${context.msg.size} bytes")
    }

    private fun Throwable.logLine(): String = "${javaClass.simpleName}: ${message ?: "no message"}"

    private fun resetConnection() {
        runCatching { sslSocket?.close() }
        sslSocket = null
        runCatching { socket.close() }
    }

    private fun createHeader(type: PairingPacketHeader.Type, payloadSize: Int): PairingPacketHeader {
        return PairingPacketHeader(kCurrentKeyHeaderVersion, type.value, payloadSize)
    }

    private fun readHeader(): PairingPacketHeader? {
        val bytes = ByteArray(kPairingPacketHeaderSize)
        try {
            inputStream.readFully(bytes)
        } catch (e: SocketTimeoutException) {
            Log.e(TAG, "Timed out (${READ_TIMEOUT_MS}ms) waiting for a pairing packet header")
            throw e
        } catch (e: java.io.EOFException) {
            Log.e(TAG, "adbd closed the connection before replying")
            throw e
        }
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        return PairingPacketHeader.readFrom(buffer)
    }

    private fun writeHeader(header: PairingPacketHeader, payload: ByteArray) {
        val buffer = ByteBuffer.allocate(kPairingPacketHeaderSize).order(ByteOrder.BIG_ENDIAN)
        header.writeTo(buffer)

        outputStream.write(buffer.array())
        outputStream.write(payload)
        outputStream.flush()
        Log.d(TAG, "write payload, size=${payload.size}")
    }

    private fun doExchangeMsgs(): Boolean {
        val msg = pairingContext.msg
        val size = msg.size

        Log.d(TAG, "Sending SPAKE2 msg (${size} bytes)")
        val ourHeader = createHeader(PairingPacketHeader.Type.SPAKE2_MSG, size)
        writeHeader(ourHeader, msg)

        val theirHeader = readHeader() ?: run {
            Log.e(TAG, "Bad header while expecting their SPAKE2 msg")
            return false
        }
        if (theirHeader.type != PairingPacketHeader.Type.SPAKE2_MSG.value) {
            Log.e(TAG, "Expected SPAKE2_MSG, got type=${theirHeader.type}")
            return false
        }

        val theirMessage = ByteArray(theirHeader.payload)
        inputStream.readFully(theirMessage)
        Log.d(TAG, "Received their SPAKE2 msg (${theirMessage.size} bytes)")

        if (!pairingContext.initCipher(theirMessage)) {
            Log.e(TAG, "initCipher failed -- most likely the pairing code is wrong")
            return false
        }
        Log.d(TAG, "SPAKE2 exchange done, cipher initialized")
        return true
    }

    private fun doExchangePeerInfo(): Boolean {
        val buf = ByteBuffer.allocate(kMaxPeerInfoSize).order(ByteOrder.BIG_ENDIAN)
        peerInfo.writeTo(buf)

        val outbuf = pairingContext.encrypt(buf.array()) ?: run {
            Log.e(TAG, "encrypt(peerInfo) failed")
            return false
        }

        Log.d(TAG, "Sending our PeerInfo (${outbuf.size} bytes ciphertext)")
        val ourHeader = createHeader(PairingPacketHeader.Type.PEER_INFO, outbuf.size)
        writeHeader(ourHeader, outbuf)

        val theirHeader = readHeader() ?: run {
            Log.e(TAG, "Bad header while expecting their PeerInfo")
            return false
        }
        if (theirHeader.type != PairingPacketHeader.Type.PEER_INFO.value) {
            Log.e(TAG, "Expected PEER_INFO, got type=${theirHeader.type}")
            return false
        }

        val theirMessage = ByteArray(theirHeader.payload)
        inputStream.readFully(theirMessage)
        Log.d(TAG, "Received their PeerInfo (${theirMessage.size} bytes ciphertext)")

        val decrypted = pairingContext.decrypt(theirMessage) ?: throw AdbInvalidPairingCodeException()
        if (decrypted.size != kMaxPeerInfoSize) {
            Log.e(TAG, "Got size=${decrypted.size} PeerInfo.size=$kMaxPeerInfoSize")
            return false
        }
        val theirPeerInfo = PeerInfo.readFrom(ByteBuffer.wrap(decrypted))
        Log.d(TAG, "Received PeerInfo type=${theirPeerInfo.type}")
        Log.i(TAG, "Pairing handshake completed successfully")
        return true
    }

    override fun close() {
        Log.d(TAG, "Closing pairing client")
        try { inputStream.close() } catch (_: Throwable) {}
        try { outputStream.close() } catch (_: Throwable) {}
        try { sslSocket?.close() } catch (_: Throwable) {}
        try { socket.close() } catch (_: Throwable) {}
        sslSocket = null

        if (state != State.Ready && ::pairingContext.isInitialized) {
            try { pairingContext.destroy() } catch (_: Throwable) {}
        }
        state = State.Stopped
    }

    companion object {
        init {
            System.loadLibrary("adb")
        }

        @JvmStatic
        external fun available(): Boolean
    }
}
