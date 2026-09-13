package io.github.xiangsu1145.aimbotnextgen.shell

import android.util.Log

/**
 * One-shot bring-up probe for the capture chain.
 *
 * The chain has a lot of places to fail quietly — the borrowed system Context,
 * the `CAPTURE_VIDEO_OUTPUT` permission, the display density, the ImageReader —
 * and none of them are reachable from a unit test, because they need the
 * platform and the shell UID. This runs the whole thing once, on its own, so a
 * failure can be attributed without involving the menu:
 *
 * ```
 * adb shell "CLASSPATH=<apk> app_process /system/bin \
 *     io.github.xiangsu1145.aimbotnextgen.shell.CaptureProbe <nativeLibDir> [size]"
 * ```
 *
 * Success shows up in logcat as `AimbotCapture: frame #1 ...`. A failure shows
 * which step refused.
 */
object CaptureProbe {

    private const val TAG = "aimbot_probe"

    @JvmStatic
    fun main(args: Array<String>) {
        val libDir = args.getOrNull(0)
        if (libDir.isNullOrBlank()) {
            System.err.println("usage: CaptureProbe <nativeLibDir> [size]")
            return
        }
        val size = args.getOrNull(1)?.toIntOrNull() ?: 320

        System.load("$libDir/libaimbotng.so")
        Log.i(TAG, "so loaded; starting capture at $size")
        println("PROBE starting $size")

        val started = ScreenCapture.start(size)
        Log.i(TAG, "ScreenCapture.start($size) = $started")
        println("PROBE start = $started")

        // Long enough for the pump to have pulled a good number of frames.
        try {
            Thread.sleep(3000)
        } catch (ignored: InterruptedException) {
            // Fall through to the stop.
        }

        ScreenCapture.stop()
        Log.i(TAG, "probe finished")
        println("PROBE done")
    }
}
