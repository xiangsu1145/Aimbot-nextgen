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
import android.widget.TextView
import com.google.android.material.card.MaterialCardView
import com.google.android.material.progressindicator.LinearProgressIndicator
import io.github.xiangsu1145.aimbotnextgen.R
import io.github.xiangsu1145.aimbotnextgen.ui.dp
import io.github.xiangsu1145.aimbotnextgen.download.ModelDownloadManager
import io.github.xiangsu1145.aimbotnextgen.ui.matchParentWrapContent
import io.github.xiangsu1145.aimbotnextgen.model.ModelInfo
import io.github.xiangsu1145.aimbotnextgen.model.ModelFormat
import io.github.xiangsu1145.aimbotnextgen.model.ModelRepository
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

/**
 * Model detail dialog.
 *
 * Two visual states in one dialog:
 *  - INFO: description + full parameter list, buttons [取消] / [下载]
 *    (disabled `已下载` when the file is already on disk).
 *  - PROGRESS: in-dialog realtime progress bar, buttons [取消下载] / [后台下载].
 *    Dismissing the dialog at any point leaves the task running in background.
 *
 * The card scales/fades in on show (MD3 emphasized easing) and listens to the
 * download manager only while visible.
 */
class ModelDetailDialog(
    private val activity: Activity,
    private val model: ModelInfo,
) : ModelDownloadManager.Listener {

    private var dialog: Dialog? = null
    private var progressHost: LinearLayout? = null
    private var progressBar: LinearProgressIndicator? = null
    private var progressText: TextView? = null
    private var cancelButton: com.google.android.material.button.MaterialButton? = null
    private var actionButton: com.google.android.material.button.MaterialButton? = null
    private var dialogCard: MaterialCardView? = null

    /** Starts in INFO mode; [show] opens the dialog. */
    fun show() {
        val card = buildContent()
        dialogCard = card

        // The window itself is MATCH_PARENT-wide; the 20dp frame padding
        // provides the side margins so the card never touches the screen edge.
        val frame = android.widget.FrameLayout(activity).apply {
            setPadding(activity.dp(20), activity.dp(20), activity.dp(20), activity.dp(20))
        }
        frame.addView(card)

        val dlg = Dialog(activity)
        dlg.setContentView(frame)
        dlg.setCanceledOnTouchOutside(true)
        dlg.window?.apply {
            setBackgroundDrawable(ColorDrawable(android.graphics.Color.TRANSPARENT))
            setLayout(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
        }
        dlg.setOnDismissListener { ModelDownloadManager.removeListener(this) }
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

        ModelDownloadManager.addListener(this)
        // A task for this model may already be running from a previous dialog.
        ModelDownloadManager.taskFor(model.modelId)?.let { renderTask(it) }
    }

    fun dismiss() {
        dialog?.dismiss()
    }

    // ── Content ────────────────────────────────────────────────────────────

    private fun buildContent(): MaterialCardView {
        val ctx = activity
        val card = MaterialCardView(ctx).apply {
            radius = ctx.dp(28).toFloat()
            cardElevation = 0f
            strokeWidth = 0
            setCardBackgroundColor(AimbotColors.SURFACE)
            layoutParams = matchParentWrapContent().apply {
                setMargins(ctx.dp(20), ctx.dp(20), ctx.dp(20), ctx.dp(20))
            }
        }

        val body = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(ctx.dp(24), ctx.dp(24), ctx.dp(24), ctx.dp(16))
        }

        body.addView(TextView(ctx).apply {
            text = model.modelName
            textSize = 20f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(AimbotColors.ON_SURFACE)
        })

        body.addView(ctx.dp(10).let { View(ctx).apply { layoutParams = LinearLayout.LayoutParams(1, it) } })

        body.addView(TextView(ctx).apply {
            text = model.description.ifBlank { "（该模型暂无介绍）" }
            textSize = 13f
            setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            setLineSpacing(ctx.dp(3).toFloat(), 1f)
        })

        body.addView(gap(ctx, 16))
        body.addView(paramRow(ctx, "模型格式", model.format.label))
        body.addView(paramRow(ctx, "量化精度", model.quantize))
        body.addView(paramRow(ctx, "推理分辨率", model.resolution))
        body.addView(paramRow(ctx, "类别数量", "${model.classCount} 类"))
        body.addView(paramRow(ctx, "模型 ID", model.modelId))

        // ── Progress section (hidden until 下载 is tapped) ──
        val host = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }
        val bar = LinearProgressIndicator(ctx).apply {
            trackCornerRadius = ctx.dp(3)
            trackThickness = ctx.dp(6)
            setIndicatorColor(AimbotColors.PRIMARY)
            setTrackColor(AimbotColors.SURFACE_VARIANT)
            layoutParams = matchParentWrapContent().apply { topMargin = ctx.dp(6) }
        }
        val text = TextView(ctx).apply {
            textSize = 12f
            setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            gravity = Gravity.CENTER
            setPadding(0, ctx.dp(8), 0, 0)
        }
        host.addView(bar)
        host.addView(text)
        body.addView(host)
        progressHost = host
        progressBar = bar
        progressText = text

        body.addView(gap(ctx, 16))

        // ── Buttons row ──
        val btnRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END or Gravity.CENTER_VERTICAL
        }
        fun button(text: String, filled: Boolean): com.google.android.material.button.MaterialButton {
            return com.google.android.material.button.MaterialButton(ctx).apply {
                this.text = text
                textSize = 14f
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                isAllCaps = false
                cornerRadius = ctx.dp(20)
                minimumHeight = ctx.dp(40)
                minWidth = ctx.dp(72)
                if (filled) {
                    setTextColor(AimbotColors.ON_PRIMARY)
                    setBackgroundColor(AimbotColors.PRIMARY)
                } else {
                    setTextColor(AimbotColors.PRIMARY)
                    setBackgroundColor(android.graphics.Color.TRANSPARENT)
                }
            }
        }
        val cancel = button("取消", filled = false)
        val action = button("下载", filled = true)
        btnRow.addView(cancel)
        btnRow.addView(View(ctx).apply { layoutParams = LinearLayout.LayoutParams(ctx.dp(8), 1) })
        btnRow.addView(action)
        body.addView(btnRow)

        card.addView(body)
        cancelButton = cancel
        actionButton = action
        renderInfo()
        return card
    }

    /** INFO mode: parameter table + 取消/下载 buttons. */
    private fun renderInfo() {
        progressHost?.visibility = View.GONE
        val cancel = cancelButton ?: return
        val action = actionButton ?: return

        cancel.text = "取消"
        cancel.setOnClickListener { dismiss() }

        if (ModelRepository.isDownloaded(activity, model)) {
            action.text = "已下载"
            action.alpha = 0.5f
            action.setOnClickListener(null)
        } else {
            action.text = "下载"
            action.alpha = 1f
            action.setOnClickListener { startDownload() }
        }
    }

    /** Switches the dialog into PROGRESS mode and enqueues the background task. */
    private fun startDownload() {
        val task = ModelDownloadManager.enqueue(activity, model) ?: run {
            renderInfo()
            return
        }
        progressHost?.visibility = View.VISIBLE
        renderTask(task)
    }

    /** Renders whatever the given task currently looks like. */
    private fun renderTask(task: ModelDownloadManager.Task) {
        val cancel = cancelButton ?: return
        val action = actionButton ?: return
        progressHost?.visibility = View.VISIBLE
        val text = progressText ?: return
        val bar = progressBar ?: return

        when (task.state) {
            ModelDownloadManager.State.QUEUED -> {
                text.text = "排队中…"
                cancel.text = "取消下载"
                cancel.setOnClickListener {
                    ModelDownloadManager.cancel(model.modelId); dismiss()
                }
                action.text = "后台下载"
                action.setOnClickListener { dismiss() }
            }
            ModelDownloadManager.State.RUNNING -> {
                if (task.totalBytes > 0) bar.setProgressCompat(task.percent, true)
                text.text = if (task.totalBytes > 0) {
                    "下载中 ${task.percent}% · ${fmt(task.downloadedBytes)} / ${fmt(task.totalBytes)}"
                } else {
                    "下载中 · 已接收 ${fmt(task.downloadedBytes)}"
                }
                cancel.text = "取消下载"
                cancel.setOnClickListener {
                    ModelDownloadManager.cancel(model.modelId); dismiss()
                }
                action.text = "后台下载"
                action.setOnClickListener { dismiss() }
            }
            ModelDownloadManager.State.COMPLETED -> {
                bar.setProgressCompat(100, true)
                text.text = "下载完成 · ${fmt(task.totalBytes)}"
                text.setTextColor(AimbotColors.SHELL_STATUS_RUNNING)
                cancel.visibility = View.GONE
                action.text = "完成"
                action.setOnClickListener { dismiss() }
            }
            ModelDownloadManager.State.FAILED -> {
                text.text = "下载失败：${task.error ?: "网络错误"}"
                text.setTextColor(AimbotColors.ERROR)
                cancel.text = "关闭"
                cancel.setOnClickListener { dismiss() }
                action.text = "重试"
                action.setOnClickListener {
                    ModelDownloadManager.cancel(model.modelId)
                    startDownload()
                }
            }
            ModelDownloadManager.State.CANCELLED -> renderInfo()
        }
    }

    // ── ModelDownloadManager.Listener ─────────────────────────────────────

    override fun onTasksChanged() {
        val task = ModelDownloadManager.taskFor(model.modelId) ?: return
        activity.runOnUiThread { if (dialog?.isShowing == true) renderTask(task) }
    }

    // ── Helpers ────────────────────────────────────────────────────────────

    private fun paramRow(ctx: android.content.Context, label: String, value: String): LinearLayout {
        return LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, ctx.dp(5), 0, ctx.dp(5))
            addView(TextView(ctx).apply {
                text = label
                textSize = 13f
                setTextColor(AimbotColors.ON_SURFACE_VARIANT)
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            })
            addView(TextView(ctx).apply {
                text = value
                textSize = 13f
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                setTextColor(AimbotColors.ON_SURFACE)
            })
        }
    }

    private fun gap(ctx: android.content.Context, h: Int): View =
        View(ctx).apply { layoutParams = LinearLayout.LayoutParams(1, ctx.dp(h)) }

    private fun fmt(bytes: Long): String = when {
        bytes >= 1 shl 20 -> "%.1f MB".format(bytes / 1048576f)
        bytes >= 1 shl 10 -> "%.1f KB".format(bytes / 1024f)
        else -> "$bytes B"
    }
}
