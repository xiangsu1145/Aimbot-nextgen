package io.github.xiangsu1145.aimbotnextgen.shell

import java.io.File

/**
 * Stale-file cleanup for the (retired) request relay.
 *
 * The relay used to ferry file-picker / IME asks between the daemon and the
 * App process via two files under shell's writable temp dir:
 *   * `aimbot_req.json`   — what the menu wanted
 *   * `aimbot_resp/<id>`  — the App's reply
 *
 * The pump and the activities it launched are gone, but the scratch dir
 * itself survives across app reinstalls (it is *shell's* tmp, not the app's
 * private storage). The two helpers below wipe it on boot, so any residual
 * state from a previous version can never replay. After the first call,
 * nothing else ever writes there, so there is no ongoing overhead.
 */
internal object RequestRelay {

    private const val TAG = "aimbot_relay"

    /** Directory both sides agreed on; lives in shell's writable scratch. */
    internal const val REQ_PATH = "/data/local/tmp/aimbot_req.json"
    internal const val RESP_DIR = "/data/local/tmp/aimbot_resp"

    init {
        // Best-effort: the dir may not exist (first run, or already wiped).
        File(RESP_DIR).mkdirs()
    }

    /** Drains every leftover response file — call on startup. */
    fun clearStaleResponses() {
        val d = File(RESP_DIR)
        d.listFiles()?.forEach { it.delete() }
    }

    /**
     * Deletes a leftover request file — call on startup.
     *
     * The request file lives in `/data/local/tmp`, which survives an app
     * reinstall. Without this, a stale request written by a previous session
     * would sit on disk forever — harmless today (consume / launch paths are
     * gone) but a footgun if anyone ever re-introduces them.
     */
    fun clearStaleRequests() {
        File(REQ_PATH).delete()
    }
}