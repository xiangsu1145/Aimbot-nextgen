package io.github.xiangsu1145.aimbotnextgen.ui.theme

import android.graphics.Color

object AimbotColors {
    val PRIMARY = Color.parseColor("#6750A4")
    val ON_PRIMARY = Color.WHITE
    val PRIMARY_CONTAINER = Color.parseColor("#EADDFF")
    val ON_PRIMARY_CONTAINER = Color.parseColor("#21005D")
    val SURFACE = Color.parseColor("#FFFBFE")
    val ON_SURFACE = Color.parseColor("#1C1B1F")
    val SURFACE_VARIANT = Color.parseColor("#E7E0EC")
    val ON_SURFACE_VARIANT = Color.parseColor("#49454F")
    val OUTLINE = Color.parseColor("#79747E")
    val SURFACE_CONTAINER = Color.parseColor("#F3EDF7")
    val START_BG = Color.parseColor("#DAE1FF")
    val STOP_BG = Color.parseColor("#FFCDD2")
    val ERROR = Color.parseColor("#B3261E")
    val TERMINAL_BG = Color.parseColor("#1E1E1E")
    val TERMINAL_TEXT = Color.parseColor("#D4D4D4")
    val TERMINAL_HEADER = Color.parseColor("#569CD6")
    val SHELL_CARD_BG = Color.parseColor("#F3EDF7")
    val SHELL_STATUS_RUNNING = Color.parseColor("#388E3C")
    val SHELL_STATUS_IDLE = Color.parseColor("#9E9E9E")
    val SHELL_STATUS_ERROR = Color.parseColor("#D32F2F")
    val SHELL_BTN_START = Color.parseColor("#E8DEF8")
    val SHELL_BTN_PAIR = Color.parseColor("#FFECB3")
    val SHELL_BTN_STOP = Color.parseColor("#FFCDD2")

    // Battery-optimization warning card. Sits under the Shell card and disappears
    // once the user whitelists the app from Settings → Battery → Unrestricted.
    val WARNING_BG = Color.parseColor("#FFF4E1")
    val WARNING_BORDER = Color.parseColor("#F57C00")
    val ON_WARNING = Color.parseColor("#5D4037")
    val WARNING_BTN_BG = Color.parseColor("#F57C00")
    val ON_WARNING_BTN = Color.parseColor("#FFFFFFFF")
}
