package io.github.xiangsu1145.aimbotnextgen.ui.modelfactory

import android.app.Activity
import android.app.Dialog
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.animation.DecelerateInterpolator
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import io.github.xiangsu1145.aimbotnextgen.ui.dp
import io.github.xiangsu1145.aimbotnextgen.ui.matchParentWrapContent
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

/**
 * 模型过滤器弹窗：格式 / 量化 / 分辨率 三组多选 chip，选项之间留有间距。
 *
 * Selections are edited on temporary copies while the dialog is open; only
 * tapping 确定 commits them through [onConfirm]. 重置 clears the temporary
 * selections (visible immediately on the chips); dismissing the dialog
 * (back / outside tap) discards changes.
 */
class FilterDialog(
    private val activity: Activity,
    private val formatOptions: List<String>,
    private val quantizeOptions: List<String>,
    private val resolutionOptions: List<String>,
    currentFormats: Set<String>,
    currentQuantize: Set<String>,
    currentResolutions: Set<String>,
    private val onConfirm: (Set<String>, Set<String>, Set<String>) -> Unit,
) {

    private val selFormats = LinkedHashSet(currentFormats)
    private val selQuantize = LinkedHashSet(currentQuantize)
    private val selResolutions = LinkedHashSet(currentResolutions)

    private val chipStrips = mutableListOf<LinearLayout>()
    private var dialog: Dialog? = null

    fun show() {
        val ctx = activity
        val card = MaterialCardView(ctx).apply {
            radius = ctx.dp(28).toFloat()
            cardElevation = 0f
            strokeWidth = 0
            setCardBackgroundColor(AimbotColors.SURFACE)
        }

        val body = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(ctx.dp(24), ctx.dp(24), ctx.dp(24), ctx.dp(16))
        }

        body.addView(TextView(ctx).apply {
            text = "过滤"
            textSize = 20f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(AimbotColors.ON_SURFACE)
        })
        body.addView(View(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(1, ctx.dp(16))
        })

        // Chip groups, each row scrollable, options spaced 6dp apart.
        val groups = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        groups.addView(chipGroup(ctx, "格式", formatOptions, selFormats))
        groups.addView(chipGroup(ctx, "量化", quantizeOptions, selQuantize))
        groups.addView(chipGroup(ctx, "分辨率", resolutionOptions, selResolutions))

        val scroll = ScrollView(ctx).apply { addView(groups, matchParentWrapContent()) }
        body.addView(scroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
        ))
        body.addView(View(ctx).apply { layoutParams = LinearLayout.LayoutParams(1, ctx.dp(16)) })

        // ── Buttons: 重置 (text) | 确定 (filled) ──
        val btnRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END or Gravity.CENTER_VERTICAL
        }
        btnRow.addView(MaterialButton(ctx).apply {
            text = "重置"
            textSize = 14f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            isAllCaps = false
            cornerRadius = ctx.dp(20)
            minimumHeight = ctx.dp(40)
            minWidth = ctx.dp(72)
            setTextColor(AimbotColors.PRIMARY)
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
            setOnClickListener { resetSelections() }
        })
        btnRow.addView(View(ctx).apply { layoutParams = LinearLayout.LayoutParams(ctx.dp(8), 1) })
        btnRow.addView(MaterialButton(ctx).apply {
            text = "确定"
            textSize = 14f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            isAllCaps = false
            cornerRadius = ctx.dp(20)
            minimumHeight = ctx.dp(40)
            minWidth = ctx.dp(96)
            setTextColor(AimbotColors.ON_PRIMARY)
            setBackgroundColor(AimbotColors.PRIMARY)
            setOnClickListener {
                onConfirm(selFormats, selQuantize, selResolutions)
                dialog?.dismiss()
            }
        })
        body.addView(btnRow)

        card.addView(body)

        val frame = android.widget.FrameLayout(ctx).apply {
            setPadding(ctx.dp(20), ctx.dp(20), ctx.dp(20), ctx.dp(20))
        }
        frame.addView(card)

        val dlg = Dialog(ctx)
        dlg.setContentView(frame)
        dlg.setCanceledOnTouchOutside(true)
        dlg.window?.apply {
            setBackgroundDrawable(ColorDrawable(android.graphics.Color.TRANSPARENT))
            setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        dialog = dlg

        // MD3 emphasized enter: scale up from 92% with a fade.
        card.scaleX = 0.92f
        card.scaleY = 0.92f
        card.alpha = 0f
        dlg.show()
        card.animate()
            .scaleX(1f).scaleY(1f).alpha(1f)
            .setDuration(220)
            .setInterpolator(DecelerateInterpolator(1.6f))
            .start()
    }

    /** One labeled chip group; chips are separated by 6dp gaps. */
    private fun chipGroup(
        ctx: Activity,
        label: String,
        options: List<String>,
        selected: LinkedHashSet<String>,
    ): LinearLayout {
        val strip = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
        }
        options.forEachIndexed { i, option ->
            if (i > 0) {
                strip.addView(View(ctx).apply {
                    layoutParams = LinearLayout.LayoutParams(ctx.dp(6), 1)
                })
            }
            strip.addView(newFilterChip(ctx, option, selected) {})
        }
        chipStrips.add(strip)
        return LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, ctx.dp(8), 0, ctx.dp(8))
            addView(TextView(ctx).apply {
                text = label
                textSize = 12f
                setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            })
            strip.layoutParams = matchParentWrapContent()
            addView(strip)
        }
    }

    /** Clears every temporary selection and unchecks all chips at once. */
    private fun resetSelections() {
        selFormats.clear()
        selQuantize.clear()
        selResolutions.clear()
        for (strip in chipStrips) {
            for (j in 0 until strip.childCount) {
                (strip.getChildAt(j) as? com.google.android.material.chip.Chip)
                    ?.isChecked = false
            }
        }
    }
}
