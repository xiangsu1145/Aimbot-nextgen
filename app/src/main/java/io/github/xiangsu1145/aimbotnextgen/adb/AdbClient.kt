package io.github.xiangsu1145.aimbotnextgen.adb

import android.util.Log
import java.io.Closeable
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager
import java.security.cert.X509Certificate

private const val TAG = "AdbClient"

class AdbClient(
    private val host: String,
    private val port: Int,
    private val key: AdbKey
) : Closeable {

    private lateinit var socket: Socket
    private lateinit var plainInput: DataInputStream
    private lateinit var plainOutput: DataOutputStream

    private var useTls = false
    private lateinit var tlsSocket: SSLSocket
    private lateinit var tlsInput: DataInputStream
    private lateinit var tlsOutput: DataOutputStream

    private val input get() = if (useTls) tlsInput else plainInput
    private val output get() = if (useTls) tlsOutput else plainOutput

    /** Serialises writes: a live shell session writes from a different thread
     *  than the one driving the session. */
    private val writeLock = Any()

    /** Local stream ids, one per shell command or session. Never reused: a
     *  number recycled too early would let a late message from the previous
     *  stream be mistaken for one of the new stream's. */
    private var nextSessionId = 1

    private var sessionReaderThread: Thread? = null

    fun connect() {
        socket = Socket(host, port)
        socket.tcpNoDelay = true
        plainInput = DataInputStream(socket.getInputStream())
        plainOutput = DataOutputStream(socket.getOutputStream())

        write(AdbMessage.create(AdbProtocol.CNXN, AdbProtocol.A_VERSION, AdbProtocol.A_MAXDATA, "host::"))

        var msg = read()
        when (msg.command) {
            AdbProtocol.STLS -> {
                write(AdbMessage.create(AdbProtocol.STLS, 0x01000000, 0))
                tlsSocket = key.defaultSslContext.socketFactory.createSocket(socket, host, port, true) as SSLSocket
                tlsSocket.startHandshake()
                tlsInput = DataInputStream(tlsSocket.inputStream)
                tlsOutput = DataOutputStream(tlsSocket.outputStream)
                useTls = true
                msg = read()
            }
        }
        // Handle AUTH loop (may happen multiple times for unpaired devices)
        var authAttempts = 0
        while (msg.command == AdbProtocol.AUTH && authAttempts < 3) {
            handleAuth(msg)
            msg = read()
            authAttempts++
        }
        if (msg.command != AdbProtocol.CNXN) {
            throw AdbException("Expected CNXN, got ${msg.toStringShort()}")
        }
        Log.d(TAG, "Connected to ADB on $host:$port")
    }

    private fun handleAuth(msg: AdbMessage) {
        if (msg.arg0 == AdbProtocol.AUTH_TYPE_TOKEN) {
            write(AdbMessage.create(AdbProtocol.AUTH, AdbProtocol.AUTH_TYPE_SIGNATURE, 0, key.sign(msg.data)))
            val reply = read()
            if (reply.command == AdbProtocol.AUTH) {
                write(AdbMessage.create(AdbProtocol.AUTH, AdbProtocol.AUTH_TYPE_RSA_PUBLIC, 0, key.adbPublicKey))
            }
        }
    }

    fun shellCommand(command: String, listener: ((ByteArray) -> Unit)? = null) {
        val localId = nextSessionId++
        write(AdbMessage.create(AdbProtocol.OPEN, localId, 0, "shell:$command"))
        var msg = read()
        when (msg.command) {
            AdbProtocol.OKAY -> {
                while (true) {
                    msg = read()
                    val remoteId = msg.arg0
                    when (msg.command) {
                        AdbProtocol.WRTE -> {
                            if (msg.data != null && msg.data.isNotEmpty()) {
                                listener?.invoke(msg.data)
                            }
                            write(AdbMessage.create(AdbProtocol.OKAY, localId, remoteId))
                        }
                        // Flow control for one of our own writes — adbd acks
                        // every WRTE we push, and an OKAY can therefore land
                        // anywhere in the stream. Normal traffic, not an error.
                        //
                        // Throwing here (as this used to) left that message
                        // unconsumed on the socket, so the NEXT command read it
                        // as the reply to its own OPEN and every command after
                        // that was one message out of phase — permanently. The
                        // symptom was a launch that worked perfectly while the
                        // app reported "daemon never published a port", with
                        // AdbException: Unexpected command during shell:
                        // AdbMessage(cmd=YAKO...) on every single command after
                        // the first hiccup. readSessionLoop() had always ignored
                        // these; shellCommand() simply never learned to.
                        AdbProtocol.OKAY -> Unit
                        AdbProtocol.CLSE -> {
                            write(AdbMessage.create(AdbProtocol.CLSE, localId, remoteId))
                            return
                        }
                        else -> throw AdbException("Unexpected command during shell: ${msg.toStringShort()}")
                    }
                }
            }
            AdbProtocol.CLSE -> {
                write(AdbMessage.create(AdbProtocol.CLSE, localId, msg.arg0))
            }
            else -> throw AdbException("Expected OKAY or CLSE, got ${msg.toStringShort()}")
        }
    }

    private fun write(msg: AdbMessage) {
        synchronized(writeLock) {
            output.write(msg.toByteArray())
            output.flush()
        }
    }

    private fun read(): AdbMessage {
        return AdbMessage.readFrom(input)
    }

    /**
     * Opens a persistent, bidirectional `shell:` stream.
     *
     * Unlike [shellCommand], which sends one command and drains its output, the
     * returned [ShellSession] stays open: the caller can keep writing lines to
     * the remote process' stdin and receives its stdout through [onData]. That
     * is what lets us run the privileged input daemon in the foreground and talk
     * to it with a line protocol.
     *
     * Output is delivered on a dedicated reader thread — after this call returns,
     * this client must not be used for [shellCommand] until the session closes,
     * because there is only one stream to read from.
     */
    fun openShellSession(
        command: String,
        onData: (ByteArray) -> Unit,
        onClosed: () -> Unit
    ): ShellSession {
        val localId = nextSessionId++
        write(AdbMessage.create(AdbProtocol.OPEN, localId, 0, "shell:$command"))

        val msg = read()
        if (msg.command != AdbProtocol.OKAY) {
            if (msg.command == AdbProtocol.CLSE) {
                write(AdbMessage.create(AdbProtocol.CLSE, localId, msg.arg0))
            }
            throw AdbException("shell session refused: ${msg.toStringShort()}")
        }

        val session = ShellSession(this, localId, msg.arg0, onData, onClosed)
        sessionReaderThread = Thread({ readSessionLoop(session) }, "adb-shell-reader").also {
            it.isDaemon = true
            it.start()
        }
        return session
    }

    private fun readSessionLoop(session: ShellSession) {
        try {
            while (!session.isClosed) {
                val msg = read()
                when (msg.command) {
                    AdbProtocol.WRTE -> {
                        msg.data?.let { if (it.isNotEmpty()) session.deliver(it) }
                        write(AdbMessage.create(AdbProtocol.OKAY, session.localId, session.remoteId))
                    }
                    // Acknowledgement of one of OUR writes: adbd acks every WRTE
                    // we push into the daemon's stdin. This is normal traffic, NOT
                    // the remote closing — treating it as unexpected used to tear
                    // the session down right after the first command we sent,
                    // which surfaced as "Shell 守护进程已断开" while the daemon was
                    // still alive and running.
                    AdbProtocol.OKAY -> Unit
                    AdbProtocol.CLSE -> {
                        write(AdbMessage.create(AdbProtocol.CLSE, session.localId, session.remoteId))
                        session.markClosedFromRemote()
                        return
                    }
                    else -> {
                        Log.w(TAG, "shell session: unexpected ${msg.toStringShort()}")
                        session.markClosedFromRemote()
                        return
                    }
                }
            }
        } catch (t: Throwable) {
            Log.w(TAG, "shell session read loop ended: ${t.message}")
            session.markClosedFromRemote()
        }
    }

    /** Sends one chunk to a session's remote stdin. Used by [ShellSession]. */
    internal fun sendToSession(session: ShellSession, data: ByteArray) {
        write(AdbMessage.create(AdbProtocol.WRTE, session.localId, session.remoteId, data))
    }

    /** Closes a session's remote endpoint. Used by [ShellSession]. */
    internal fun closeSession(session: ShellSession) {
        runCatching { write(AdbMessage.create(AdbProtocol.CLSE, session.localId, session.remoteId)) }
    }

    /**
     * A live `shell:` stream. Write to it with [writeLine]; its stdout arrives on
     * the callback passed to [AdbClient.openShellSession].
     */
    class ShellSession internal constructor(
        private val client: AdbClient,
        internal val localId: Int,
        internal val remoteId: Int,
        private val onData: (ByteArray) -> Unit,
        private val onClosed: () -> Unit
    ) {
        @Volatile internal var isClosed: Boolean = false
            private set

        internal fun deliver(data: ByteArray) = onData(data)

        /** Called by the reader thread when the remote end goes away. */
        internal fun markClosedFromRemote() {
            if (isClosed) return
            isClosed = true
            onClosed()
        }

        /** Sends one line to the remote process' stdin. */
        fun writeLine(line: String) {
            if (isClosed) return
            client.sendToSession(this, (line + "\n").toByteArray(Charsets.UTF_8))
        }

        /** Closes this session (idempotent). */
        fun close() {
            if (isClosed) return
            isClosed = true
            client.closeSession(this)
            onClosed()
        }
    }

    override fun close() {
        runCatching { plainInput.close() }
        runCatching { plainOutput.close() }
        runCatching { socket.close() }
        if (useTls) {
            runCatching { tlsInput.close() }
            runCatching { tlsOutput.close() }
            runCatching { tlsSocket.close() }
        }
    }

    private object TrustAllTrustManager : X509TrustManager {
        override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
        override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
        override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
    }
}
