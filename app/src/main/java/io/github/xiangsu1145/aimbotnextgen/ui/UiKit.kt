package io.github.xiangsu1145.aimbotnextgen.ui

import android.content.Context
import android.graphics.Typeface
import android.util.TypedValue
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

/**
 * Small helpers shared by the hand-built views (the UI is constructed in code,
 * not XML, so these keep the builders readable).
 */

/** Converts dp to px using the receiver's display metrics. */
fun Context.dp(value: Int): Int =
    (value * resources.displayMetrics.density + 0.5f).toInt()

/** A vertical gap of [height] dp. */
fun Context.gap(height: Int): View = View(this).apply {
    layoutParams = LinearLayout.LayoutParams(1, dp(height))
}

/**
 * Press feedback for icon-only buttons built in code: replaces the default
 * grey ImageButton background with a borderless ripple (image + tap animation).
 */
fun View.borderlessRipple() {
    val tv = TypedValue()
    context.theme.resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, tv, true)
    setBackgroundResource(tv.resourceId)
}

/** A horizontal gap of [width] dp. */
fun Context.spacer(width: Int): View = View(this).apply {
    layoutParams = LinearLayout.LayoutParams(dp(width), 1)
}

/** A small section header label. */
fun Context.sectionLabel(text: String): TextView = TextView(this).apply {
    this.text = text
    textSize = 14f
    setTextColor(AimbotColors.PRIMARY)
    typeface = Typeface.DEFAULT_BOLD
    letterSpacing = 0.02f
}

// ── Layout params ────────────────────────────────────────────────────────

fun matchParent(): Int = ViewGroup.LayoutParams.MATCH_PARENT

fun wrapContent(): Int = ViewGroup.LayoutParams.WRAP_CONTENT

fun matchParentWrapContent(): LinearLayout.LayoutParams =
    LinearLayout.LayoutParams(matchParent(), wrapContent())
