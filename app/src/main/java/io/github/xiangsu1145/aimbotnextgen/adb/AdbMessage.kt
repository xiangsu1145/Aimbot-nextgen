package io.github.xiangsu1145.aimbotnextgen.adb

import java.nio.ByteBuffer
import java.nio.ByteOrder

class AdbMessage(
    val command: Int,
    val arg0: Int,
    val arg1: Int,
    val dataLength: Int,
    val checksum: Int,
    val magic: Int,
    val data: ByteArray?
) {
    fun toByteArray(): ByteArray {
        val buffer = ByteBuffer.allocate(AdbProtocol.ADB_HEADER_LENGTH + (data?.size ?: 0))
            .order(ByteOrder.LITTLE_ENDIAN)
        buffer.putInt(command)
        buffer.putInt(arg0)
        buffer.putInt(arg1)
        buffer.putInt(data?.size ?: 0)
        buffer.putInt(if (data != null) calculateChecksum(data) else 0)
        buffer.putInt(command xor 0xFFFFFFFF.toInt())
        data?.let { buffer.put(it) }
        return buffer.array()
    }

    fun validate() {
        if (data != null) {
            val computed = calculateChecksum(data)
            if (computed != checksum) {
                throw AdbException("Checksum mismatch: expected=$checksum, computed=$computed")
            }
        }
        val expectedMagic = command xor 0xFFFFFFFF.toInt()
        if (magic != expectedMagic) {
            throw AdbException("Magic mismatch: expected=$expectedMagic, got=$magic")
        }
    }

    fun toStringShort(): String {
        return "AdbMessage(cmd=${String(ByteBuffer.allocate(4).putInt(command).array())}, arg0=$arg0, arg1=$arg1, len=${data?.size ?: 0})"
    }

    companion object {
        fun readFrom(input: java.io.DataInputStream): AdbMessage {
            val header = ByteArray(AdbProtocol.ADB_HEADER_LENGTH)
            input.readFully(header)
            val buf = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            val command = buf.int
            val arg0 = buf.int
            val arg1 = buf.int
            val dataLength = buf.int
            val checksum = buf.int
            val magic = buf.int
            val data = if (dataLength > 0) {
                ByteArray(dataLength).also { input.readFully(it) }
            } else null
            return AdbMessage(command, arg0, arg1, dataLength, checksum, magic, data).also { it.validate() }
        }

        fun create(command: Int, arg0: Int, arg1: Int, data: ByteArray? = null): AdbMessage {
            return AdbMessage(
                command, arg0, arg1,
                data?.size ?: 0,
                if (data != null) calculateChecksum(data) else 0,
                command xor 0xFFFFFFFF.toInt(),
                data
            )
        }

        fun create(command: Int, arg0: Int, arg1: Int, data: String): AdbMessage {
            return create(command, arg0, arg1, data.toByteArray(Charsets.UTF_8))
        }

        private fun calculateChecksum(data: ByteArray): Int {
            var sum = 0
            for (b in data) {
                sum += b.toInt() and 0xFF
            }
            return sum
        }
    }
}
