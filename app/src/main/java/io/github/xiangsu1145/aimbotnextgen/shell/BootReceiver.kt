package io.github.xiangsu1145.aimbotnextgen.shell

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import io.github.xiangsu1145.aimbotnextgen.adb.AdbPairingState

/**
 * Re-arms the shell service after a reboot.
 *
 * The user enabled wireless ADB pairing once and pressed "启动" once, and they
 * expect both to keep working across reboots. We cannot restart the daemon
 * process on boot (that requires a connected USB or pre-authorised adb key,
 * neither of which we have on a freshly-booted device), so the best we can do
 * is bring the foreground service up; the service then checks `was_running`
 * and, if true and the user is still paired, calls `manager.startIfIdle()`,
 * which drives the auto-reconnect supervisor.
 *
 * On boot:
 *  - LOCKED_BOOT_COMPLETED arrives before the user unlocks the device — we
 *    can start the service then, but the user may not yet have unlocked, so
 *    the daemon-side reconnect (which needs the ADB key) will probably fail
 *    quietly and keep retrying.
 *  - BOOT_COMPLETED is the safer bet; it is delivered after first-unlock on
 *    modern Android. We listen for both because some OEMs only deliver one.
 *
 * The receiver is `exported=true` because the BOOT_COMPLETED broadcast comes
 * from the system. The IntentFilter restricts it to those two actions.
 */
class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        if (action != Intent.ACTION_BOOT_COMPLETED &&
            action != Intent.ACTION_LOCKED_BOOT_COMPLETED) {
            return
        }
        if (!AdbPairingState.isPaired(context)) {
            Log.i(TAG, "skipping: user is not paired")
            return
        }
        Log.i(TAG, "boot received ($action); resuming shell service")
        ShellDaemonService.startIfIdle(context)
    }

    companion object {
        private const val TAG = "BootReceiver"
    }
}
