package io.github.xiangsu1145.aimbotnextgen.ui.modelfactory

import android.content.Context
import android.graphics.Typeface
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.progressindicator.LinearProgressIndicator
import io.github.xiangsu1145.aimbotnextgen.R
import io.github.xiangsu1145.aimbotnextgen.ui.dp
import io.github.xiangsu1145.aimbotnextgen.ui.gap
import io.github.xiangsu1145.aimbotnextgen.download.ModelDownloadManager
import io.github.xiangsu1145.aimbotnextgen.ui.matchParentWrapContent
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

/**
 * 下载任务 sub page: search bar + live task list. Filtering of models lives
 * on the factory page's search-bar filter dialog.
 */
class DownloadTasksScreen(
    context: Context,
    private val onBack: () -> Unit,
) : FrameLayout(context), ModelDownloadManager.Listener {

    private var searchQuery: String = ""
    private lateinit var adapter: TaskAdapter
    private lateinit var taskList: RecyclerView
    private lateinit var emptyText: TextView

    init {
        build()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        ModelDownloadManager.addListener(this)
    }

    override fun onDetachedFromWindow() {
        ModelDownloadManager.removeListener(this)
        super.onDetachedFromWindow()
    }

    override fun onTasksChanged() {
        post { refilter() }
    }

    // ── View construction ──────────────────────────────────────────────────

    private fun build() {
        setBackgroundColor(AimbotColors.SURFACE)

        val column = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(context.dp(16), context.dp(8), context.dp(16), context.dp(12))
        }
        addView(column)

        // Navigation-bar inset padding so the list is never cut off by the
        // gesture bar (same treatment as the main screen's scroll content).
        ViewCompat.setOnApplyWindowInsetsListener(this) { v, insets ->
            v.updatePadding(
                bottom = context.dp(12) +
                    insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom
            )
            insets
        }

        // ── Header: back button + title ──
        val header = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        header.addView(ImageButton(context).apply {
            setImageResource(R.drawable.ic_back)
            contentDescription = "返回"
            setColorFilter(AimbotColors.ON_SURFACE)
            layoutParams = ViewGroup.LayoutParams(context.dp(44), context.dp(44))
            setOnClickListener { onBack() }
        })
        header.addView(TextView(context).apply {
            text = "下载任务"
            textSize = 18f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(AimbotColors.ON_SURFACE)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { marginStart = context.dp(4) }
        })
        header.addView(View(context), LinearLayout.LayoutParams(0, 1, 1f))
        header.addView(MaterialButton(context).apply {
            text = "清除已完成"
            textSize = 12f
            isAllCaps = false
            setTextColor(AimbotColors.PRIMARY)
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
            cornerRadius = context.dp(18)
            minimumHeight = context.dp(36)
            setOnClickListener { ModelDownloadManager.clearFinished() }
        })
        column.addView(header)

        // ── Search bar ──
        column.addView(context.gap(12))
        val searchBar = buildSearchBar(context, "搜索下载任务")
        searchBar.edit.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable) {
                searchQuery = s.toString().trim()
                refilter()
            }
        })
        column.addView(searchBar.card)

        // ── Task list / empty state ──
        adapter = TaskAdapter()
        taskList = RecyclerView(context).apply {
            layoutManager = LinearLayoutManager(context)
            adapter = this@DownloadTasksScreen.adapter
            overScrollMode = RecyclerView.OVER_SCROLL_NEVER
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f,
            ).apply { topMargin = context.dp(4) }
        }
        column.addView(taskList)

        emptyText = TextView(context).apply {
            text = "暂无下载任务"
            textSize = 14f
            setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            gravity = Gravity.CENTER
            visibility = View.VISIBLE
        }
        column.addView(emptyText, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f,
        ))

        refilter()
    }

    // ── Filtering ──────────────────────────────────────────────────────────

    private fun refilter() {
        if (!::adapter.isInitialized) return
        adapter.submit(ModelDownloadManager.tasksSnapshot().filter { task ->
            val m = task.model
            searchQuery.isEmpty() ||
                m.modelName.contains(searchQuery, ignoreCase = true) ||
                m.modelId.contains(searchQuery, ignoreCase = true)
        })
        val empty = adapter.itemCount == 0
        emptyText.visibility = if (empty) View.VISIBLE else View.GONE
        taskList.visibility = if (empty) View.GONE else View.VISIBLE
    }

    // ── Task cards ─────────────────────────────────────────────────────────

    private class TaskAdapter : RecyclerView.Adapter<Holder>() {
        private var items: List<ModelDownloadManager.Task> = emptyList()

        fun submit(list: List<ModelDownloadManager.Task>) {
            items = list
            notifyDataSetChanged()
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder =
            Holder(buildTaskCard(parent.context))

        override fun getItemCount(): Int = items.size

        override fun onBindViewHolder(holder: Holder, position: Int) =
            holder.bind(items[position])
    }

    private class Holder(val card: MaterialCardView) : RecyclerView.ViewHolder(card) {
        fun bind(task: ModelDownloadManager.Task) = card.bindTask(task)
    }

    companion object {
        private data class TaskChildren(
            val title: TextView,
            val status: TextView,
            val bar: LinearProgressIndicator,
        )

        fun buildTaskCard(context: Context): MaterialCardView {
            val card = MaterialCardView(context).apply {
                layoutParams = matchParentWrapContent().apply { topMargin = context.dp(8) }
                radius = context.dp(16).toFloat()
                cardElevation = 0f
                strokeWidth = 0
            }
            val col = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(context.dp(16), context.dp(14), context.dp(16), context.dp(14))
            }
            val title = TextView(context).apply {
                textSize = 15f
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                setTextColor(AimbotColors.ON_SURFACE)
            }
            val status = TextView(context).apply {
                textSize = 12f
                setTextColor(AimbotColors.ON_SURFACE_VARIANT)
                setPadding(0, context.dp(3), 0, 0)
            }
            val bar = LinearProgressIndicator(context).apply {
                trackCornerRadius = context.dp(3)
                trackThickness = context.dp(6)
                setIndicatorColor(AimbotColors.PRIMARY)
                setTrackColor(AimbotColors.SURFACE_VARIANT)
                visibility = View.GONE
                layoutParams = matchParentWrapContent().apply { topMargin = context.dp(8) }
            }
            col.addView(title)
            col.addView(status)
            col.addView(bar)
            card.addView(col)
            card.tag = TaskChildren(title, status, bar)
            return card
        }

        fun MaterialCardView.bindTask(task: ModelDownloadManager.Task) {
            val (title, status, bar) = tag as TaskChildren
            title.text = task.model.modelName
            contentDescription = task.model.modelName
            when (task.state) {
                ModelDownloadManager.State.QUEUED -> {
                    status.text = "排队中"
                    status.setTextColor(AimbotColors.ON_SURFACE_VARIANT)
                    bar.visibility = View.VISIBLE
                }
                ModelDownloadManager.State.RUNNING -> {
                    status.text = if (task.totalBytes > 0) {
                        "下载中 ${task.percent}% · ${fmt(task.downloadedBytes)} / ${fmt(task.totalBytes)}"
                    } else {
                        "下载中 · 已接收 ${fmt(task.downloadedBytes)}"
                    }
                    status.setTextColor(AimbotColors.ON_SURFACE_VARIANT)
                    bar.visibility = View.VISIBLE
                    if (task.totalBytes > 0) bar.setProgressCompat(task.percent, true)
                }
                ModelDownloadManager.State.COMPLETED -> {
                    status.text = "已完成 · ${fmt(task.totalBytes)}"
                    status.setTextColor(AimbotColors.SHELL_STATUS_RUNNING)
                    bar.visibility = View.GONE
                }
                ModelDownloadManager.State.FAILED -> {
                    status.text = "下载失败：${task.error ?: "网络错误"}"
                    status.setTextColor(AimbotColors.ERROR)
                    bar.visibility = View.GONE
                }
                ModelDownloadManager.State.CANCELLED -> {
                    status.text = "已取消"
                    status.setTextColor(AimbotColors.ON_SURFACE_VARIANT)
                    bar.visibility = View.GONE
                }
            }
        }

        private fun fmt(bytes: Long): String = when {
            bytes >= 1 shl 20 -> "%.1f MB".format(bytes / 1048576f)
            bytes >= 1 shl 10 -> "%.1f KB".format(bytes / 1024f)
            else -> "$bytes B"
        }
    }
}
