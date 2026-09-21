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
import android.widget.Toast
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
import io.github.xiangsu1145.aimbotnextgen.shell.ShellController
import java.io.File

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
    private var importButton: com.google.android.material.button.MaterialButton? = null
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
        // A task for this model may already exist from a previous dialog. Only
        // a live or just-failed task re-opens in PROGRESS mode. A COMPLETED task
        // never leaves the manager's map, so rendering it here would pin every
        // later visit to the full-bar-and-"完成" card until process death, even
        // though the file has long been on disk — renderInfo() (already run by
        // buildContent) is the truthful state then.
        ModelDownloadManager.taskFor(model.modelId)?.let { task ->
            if (task.state != ModelDownloadManager.State.COMPLETED) renderTask(task)
        }
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
            // Material 3 paints a small "stop indicator" dot in the indicator
            // colour at the far right end of the track by default; this design
            // has no place for it, so shrink it away entirely.
            setTrackStopIndicatorSize(0)
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
        // 导入：把已下载的模型文件登记进 daemon 的模型列表（按 C++ 默认参数）。
        val import = button("导入", filled = false).apply {
            setTextColor(AimbotColors.ON_PRIMARY_CONTAINER)
            setBackgroundColor(AimbotColors.PRIMARY_CONTAINER)
            setOnClickListener { importModel() }
        }
        importButton = import
        btnRow.addView(cancel)
        btnRow.addView(View(ctx).apply { layoutParams = LinearLayout.LayoutParams(ctx.dp(8), 1) })
        btnRow.addView(import)
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
            // Already on disk: 导入 placeholder + red 删除 instead of 下载.
            importButton?.visibility = View.VISIBLE
            action.text = "删除"
            action.setTextColor(AimbotColors.ON_PRIMARY)
            setBackgroundColor(action, AimbotColors.ERROR)
            action.alpha = 1f
            action.setOnClickListener {
                ModelDownloadManager.delete(activity, model)
                dismiss()
            }
        } else {
            importButton?.visibility = View.GONE
            action.text = "下载"
            action.setTextColor(AimbotColors.ON_PRIMARY)
            setBackgroundColor(action, AimbotColors.PRIMARY)
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

    /**
     * 导入：把已下载的模型文件登记进 shell daemon 的模型列表。
     *
     * The file already sits in the shared models dir
     * (/sdcard/Android/data/<pkg>/models/…), which the shell-uid daemon reads
     * directly, and the model store itself lives in the same dir now — so the
     * whole import is one `IMPORT <path>` protocol command. The daemon's
     * native store decides every parameter (default engine for the kind,
     * confidence 0.5, threads 1, HTP perf 1, probed input size / class count /
     * tensor type) and answers `OK:<id>` / `ERR:<why>`; the App never writes
     * the store itself, because the daemon keeps the list in memory and
     * rewrites the file on every menu mutation.
     */
    private fun importModel() {
        val controller = ShellController.get(activity)
        if (!controller.isRunning) {
            Toast.makeText(activity, "Shell 未运行，无法导入", Toast.LENGTH_SHORT).show()
            return
        }
        val file = File(ModelRepository.targetDir(activity, model.format), model.fileName)
        if (!file.isFile) {
            Toast.makeText(activity, "模型文件不存在", Toast.LENGTH_SHORT).show()
            return
        }
        importButton?.isEnabled = false
        Toast.makeText(activity, "正在导入…", Toast.LENGTH_SHORT).show()
        controller.request("IMPORT ${file.absolutePath}") { reply ->
            importButton?.isEnabled = true
            val msg = when {
                reply == null -> "导入失败：Shell 无响应"
                reply.startsWith("OK:") -> "导入成功，可在悬浮窗的模型列表查看"
                else -> "导入失败：${reply.removePrefix("ERR:").removePrefix("IMPORT: ")}"
            }
            Toast.makeText(activity, msg, Toast.LENGTH_SHORT).show()
        }
    }

    /** Renders whatever the given task currently looks like. */
    private fun renderTask(task: ModelDownloadManager.Task) {
        val cancel = cancelButton ?: return
        val action = actionButton ?: return
        importButton?.visibility = View.GONE
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
        activity.runOnUiThread {
            if (dialog?.isShowing != true) return@runOnUiThread
            // A COMPLETED task is only worth showing while the progress section
            // is already up — i.e. the download finished inside this very
            // dialog. Anything else (a re-opened dialog, a change triggered by
            // some other model's task) must not drag the dialog back into the
            // progress card; the info state (导入/删除) is the truthful one.
            if (task.state == ModelDownloadManager.State.COMPLETED &&
                progressHost?.visibility != View.VISIBLE
            ) {
                return@runOnUiThread
            }
            renderTask(task)
        }
    }

    // ── Helpers ────────────────────────────────────────────────────────────

    /** MaterialButton keeps its ripple when the tint is set via backgroundTint. */
    private fun setBackgroundColor(button: com.google.android.material.button.MaterialButton, color: Int) {
        button.backgroundTintList = android.content.res.ColorStateList.valueOf(color)
    }

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
