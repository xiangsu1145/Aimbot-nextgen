package io.github.xiangsu1145.aimbotnextgen.adb

import android.content.Context

/**
 * Remembers whether this device has already authorized our adb key.
 *
 * Pairing and "connected" are two different states: a successful pairing only
 * adds our key to adbd's keystore, it is not lost when the process dies -- so it
 * must be persisted, otherwise the UI invites the user to pair again and again
 * even though the device list already shows this app.
 */
object AdbPairingState {

    const val ACTION_PAIRING_RESULT = "io.github.xiangsu1145.aimbotnextgen.action.ADB_PAIRING_RESULT"
    const val EXTRA_SUCCESS = "success"
    const val EXTRA_MESSAGE = "message"

    private const val PREF = "adb_pairing_state"
    private const val KEY_PAIRED = "paired"

    fun setPaired(context: Context, paired: Boolean) {
        context.getSharedPreferences(PREF, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_PAIRED, paired)
            .apply()
    }

    /** True only if pairing actually completed; adbd may still have since been reset. */
    fun isPaired(context: Context): Boolean {
        return context.getSharedPreferences(PREF, Context.MODE_PRIVATE)
            .getBoolean(KEY_PAIRED, false)
    }
}
