package io.github.xiangsu1145.aimbotnextgen.shell

import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.File

/**
 * One-shot probe for the uinput side of touch — and its regression test.
 *
 * It drives the injector through a single down → up cycle, then reads the events
 * back off the device it created, so the lift is *observed* rather than assumed.
 * That matters more here than anywhere else in the daemon: `upload()` is the
 * only thing keeping the kernel's copy of the touch state in step with ours, the
 * physical panel is grabbed while the daemon is open, and a lift that never
 * leaves means a pointer stuck down — which is the whole phone, not one app.
 *
 * It never grabs the panel, and it destroys the virtual device on the way out
 * (`UI_DEV_DESTROY`), which is what releases any pointer that did get stuck. So
 * a regression here cannot outlive the probe.
 *
 * ```
 * adb shell "CLASSPATH=<apk> app_process /system/bin \
 *     io.github.xiangsu1145.aimbotnextgen.shell.TouchProbe <nativeLibDir>"
 * ```
 *
 * Passing looks like `TRACKING_IDs = [1000, -1]` followed by `PROBE PASS`: the
 * `1000` is the down announcing [ShellNative.INJECT_ID], the `-1` is the lift.
 */
object TouchProbe {

    private const val TAG = "aimbot_touchprobe"

    @JvmStatic
    fun main(args: Array<String>) {
        val libDir = args.getOrNull(0)
        if (libDir.isNullOrBlank()) {
            System.err.println("usage: TouchProbe <nativeLibDir>")
            return
        }
        System.load("$libDir/libaimbotng.so")

        // Doubles as a check on SysDisplay, and gives the injector something to
        // scale against.
        val geom = SysDisplay.defaultGeometry()
        val w = geom?.first ?: 0
        val h = geom?.second ?: 0
        val rot = geom?.third ?: 0
        println("PROBE geometry ${w}x$h rot=$rot")
        if (w <= 0 || h <= 0) {
            println("PROBE FAIL no geometry")
            return
        }

        // Read-only: readerInit opens the panels but does NOT grab them, so the
        // phone keeps working normally for the whole probe.
        if (!ShellNative.readerInit(w, h, rot)) {
            println("PROBE FAIL readerInit")
            return
        }
        ShellNative.uinputSetSourcePanel(ShellNative.readerGetPanelPath())

        val before = eventNodes()
        if (!ShellNative.uinputInit(w, h)) {
            ShellNative.readerClose()
            println("PROBE FAIL uinputInit")
            return
        }
        ShellNative.uinputSetScreenParams(w, h, w > h)

        val node = waitForNewNode(before)
        println("PROBE node = ${node ?: "(not found)"}")
        val fd = node?.let {
            try {
                Os.open(it, OsConstants.O_RDONLY or OsConstants.O_NONBLOCK, 0)
            } catch (e: ErrnoException) {
                Log.e(TAG, "open $it failed", e)
                null
            }
        }

        val seen = ArrayList<Int>()
        val buf = ByteArray(1 shl 16)
        fun drain() {
            val f = fd ?: return
            while (true) {
                val n = try {
                    Os.read(f, buf, 0, buf.size)
                } catch (e: ErrnoException) {
                    break  // EAGAIN — nothing more queued right now
                }
                if (n <= 0) break
                scanTrackingIds(buf, n, seen)
            }
        }

        ShellNative.uinputDown(ShellNative.INJECT_SLOT, ShellNative.INJECT_ID, w / 2, h / 2)
        Thread.sleep(200)
        drain()
        ShellNative.uinputUp(ShellNative.INJECT_SLOT)
        Thread.sleep(200)
        drain()

        fd?.let { runCatching { Os.close(it) } }
        ShellNative.uinputClose()
        ShellNative.readerClose()

        println("PROBE TRACKING_IDs = $seen")
        val down = seen.contains(ShellNative.INJECT_ID)
        val lift = seen.contains(-1)
        println(
            if (down && lift) "PROBE PASS (down ${ShellNative.INJECT_ID} and lift -1 both seen)"
            else "PROBE FAIL (down=$down lift=$lift)"
        )
    }

    /** Event nodes present right now, so a freshly created one stands out. */
    private fun eventNodes(): Set<String> =
        (File("/dev/input").list() ?: emptyArray())
            .filter { it.startsWith("event") }
            .map { "/dev/input/$it" }
            .toSet()

    private fun waitForNewNode(before: Set<String>): String? {
        repeat(20) {
            val fresh = eventNodes() - before
            if (fresh.isNotEmpty()) return fresh.sorted().first()
            Thread.sleep(50)
        }
        return null
    }

    /**
     * Pulls every `ABS_MT_TRACKING_ID` value out of a chunk of `input_event`s.
     *
     * Scanned as bytes rather than unmarshalled: `struct input_event` is
     * `timeval + u16 + u16 + s32`, and `timeval` is 16 bytes on arm64 but 8 on
     * armv7, so a fixed record stride would be right on one ABI and wrong on the
     * other. The `type`/`code` pair is four bytes whose values are fixed, and the
     * s32 that follows it is the payload, so anchoring there works on both.
     */
    private fun scanTrackingIds(buf: ByteArray, len: Int, out: MutableList<Int>) {
        var i = 0
        while (i + 8 <= len) {
            // EV_ABS (3), ABS_MT_TRACKING_ID (0x39), little-endian u16 pair.
            if (buf[i] == 3.toByte() && buf[i + 1] == 0.toByte() &&
                buf[i + 2] == 0x39.toByte() && buf[i + 3] == 0.toByte()
            ) {
                val v = (buf[i + 4].toInt() and 0xff) or
                        ((buf[i + 5].toInt() and 0xff) shl 8) or
                        ((buf[i + 6].toInt() and 0xff) shl 16) or
                        ((buf[i + 7].toInt() and 0xff) shl 24)
                out.add(v)
                i += 8
            } else {
                ++i
            }
        }
    }
}
