package io.github.xiangsu1145.aimbotnextgen.adb

object AdbProtocol {
    const val A_VERSION = 0x01000000
    const val A_MAXDATA = 4096
    const val ADB_HEADER_LENGTH = 24

    const val CNXN = 0x4e584e43
    const val OPEN = 0x4e45504f
    const val OKAY = 0x59414b4f
    const val CLSE = 0x45534c43
    const val WRTE = 0x45545257
    const val AUTH = 0x48545541
    const val STLS = 0x534c5453

    const val AUTH_TYPE_TOKEN = 1
    const val AUTH_TYPE_SIGNATURE = 2
    const val AUTH_TYPE_RSA_PUBLIC = 3
}
