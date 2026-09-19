package io.github.xiangsu1145.aimbotnextgen.ui.modelfactory

import android.content.Context
import android.content.res.ColorStateList
import android.view.ViewGroup
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import com.google.android.material.chip.Chip
import io.github.xiangsu1145.aimbotnextgen.ui.dp
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

/**
 * Shared multi-select filter chip row used by both the model factory page
 * (search-bar filter) and the download-tasks page.
 *
 * Semantics: rows AND together; within a row any selected chip matches; a row
 * with no selection matches everything.
 */

/** One filter row: fixed-width label + horizontally scrollable chip strip. */
internal class FilterRow(val root: LinearLayout, val chipStrip: LinearLayout)

internal fun buildFilterRow(
    context: Context,
    label: String,
    options: List<String>,
    selected: MutableSet<String>,
    onChange: () -> Unit,
): FilterRow {
    val strip = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
    }
    options.forEach { strip.addView(newFilterChip(context, it, selected, onChange)) }
    val row = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = android.view.Gravity.CENTER_VERTICAL
        setPadding(0, context.dp(4), 0, context.dp(4))
        addView(TextView(context).apply {
            text = label
            textSize = 12f
            setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            minWidth = context.dp(52)
        })
        addView(HorizontalScrollView(context).apply {
            isHorizontalScrollBarEnabled = false
            clipToPadding = false
            layoutParams =
                LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            addView(strip)
        })
    }
    return FilterRow(row, strip)
}

/** MD3 filter chip bound to a selection set; toggling re-runs [onChange]. */
internal fun newFilterChip(
    context: Context,
    label: String,
    selected: MutableSet<String>,
    onChange: () -> Unit,
    initiallyChecked: Boolean = false,
): Chip {
    return Chip(context).apply {
        text = label
        isCheckable = true
        isCheckedIconVisible = false
        chipBackgroundColor = ColorStateList(
            arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
            intArrayOf(AimbotColors.PRIMARY_CONTAINER, AimbotColors.SURFACE_CONTAINER),
        )
        setTextColor(ColorStateList(
            arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
            intArrayOf(AimbotColors.ON_PRIMARY_CONTAINER, AimbotColors.ON_SURFACE_VARIANT),
        ))
        chipStrokeWidth = 1f
        setChipStrokeColor(ColorStateList(
            arrayOf(intArrayOf(-android.R.attr.state_checked), intArrayOf()),
            intArrayOf(AimbotColors.OUTLINE, AimbotColors.OUTLINE),
        ))
        setOnCheckedChangeListener { _, checked ->
            if (checked) selected.add(label) else selected.remove(label)
            onChange()
        }
        // Reflect the stored selection on creation (listener above treats this
        // as a no-op add/remove for labels already in their target state).
        isChecked = initiallyChecked
    }
}
