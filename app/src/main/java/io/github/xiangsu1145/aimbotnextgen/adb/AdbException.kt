package io.github.xiangsu1145.aimbotnextgen.adb

open class AdbException : Exception {
    constructor(message: String) : super(message)
    constructor(message: String, cause: Throwable) : super(message, cause)
    constructor(cause: Throwable) : super(cause)
}

class AdbInvalidPairingCodeException : AdbException("配对码错误")
class AdbKeyException(cause: Throwable) : AdbException("密钥错误", cause)
