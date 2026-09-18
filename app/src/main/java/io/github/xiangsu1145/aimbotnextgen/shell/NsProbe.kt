package io.github.xiangsu1145.aimbotnextgen.shell

import android.os.Process
import android.util.Log

/**
 * Throwaway probe: loads /data/local/tmp/libnsprobe.so with plain
 * `System.load()` from an app_process JVM, so the probe's JNI_OnLoad runs in
 * exactly the classloader namespace libaimbotng.so lands in. Run it with:
 *
 *   CLASSPATH=$(pm path ...) app_process64 /data/local/tmp \
 *       io.github.xiangsu1145.aimbotnextgen.shell.NsProbe
 *
 * and compare an app_process64 launched from /system/bin against one launched
 * from /data/local/tmp — /linkerconfig/ld.config.txt picks the section by the
 * executable's directory, and [unrestricted] leaves the default namespace
 * non-isolated with /vendor/${LIB} on its search path.
 */
object NsProbe {
    private const val TAG = "NsProbe"

    @JvmStatic
    fun main(args: Array<String>) {
        Log.i(TAG, "start pid=${Process.myPid()} uid=${Process.myUid()}")
        try {
            System.load("/data/local/tmp/libnsprobe.so")
            Log.i(TAG, "System.load(libnsprobe.so) returned")
        } catch (t: Throwable) {
            Log.i(TAG, "System.load(libnsprobe.so) FAILED: ${t.javaClass.simpleName}: ${t.message}")
        }
    }
}
