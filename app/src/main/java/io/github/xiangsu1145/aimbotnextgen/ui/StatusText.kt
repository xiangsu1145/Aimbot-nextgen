package io.github.xiangsu1145.aimbotnextgen.ui

import io.github.xiangsu1145.aimbotnextgen.shell.ShellManager

/** Live text for the status card's "Shell" row. */
fun shellStatusLabel(paired: Boolean, state: ShellManager.ShellState): String = when {
    !paired -> "Unpaired"
    state == ShellManager.ShellState.RUNNING -> "Ready"
    else -> "Not started"
}

/**
 * Live text for the status card's "Menu" row.
 *
 * There is no permission to report any more: the menu layer is built by the shell
 * daemon on SurfaceFlinger, so all the app can say is whether it asked for it.
 */
fun menuStatus(running: Boolean): String = if (running) "On" else "Off"
