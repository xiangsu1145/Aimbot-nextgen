package io.github.xiangsu1145.aimbotnextgen.ui.modelfactory

import android.app.Activity
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.animation.AccelerateInterpolator
import android.view.animation.DecelerateInterpolator
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout
import com.google.android.material.card.MaterialCardView
import io.github.xiangsu1145.aimbotnextgen.R
import io.github.xiangsu1145.aimbotnextgen.ui.dp
import io.github.xiangsu1145.aimbotnextgen.download.ModelDownloadManager
import io.github.xiangsu1145.aimbotnextgen.model.ModelInfo
import io.github.xiangsu1145.aimbotnextgen.model.ModelRepository
import io.github.xiangsu1145.aimbotnextgen.ui.borderlessRipple
import io.github.xiangsu1145.aimbotnextgen.ui.gap
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

/**
 * 模型工厂 page (replaces the old empty 设置 tab).
 *
 * Top-level layout:
 *   [ search bar | filter ]  search filters the list; the trailing funnel
 *                            button opens the multi-select filter dialog
 *   [ card list ]            vertical model cards; downloaded models carry a
 *                            green 已下载 badge — there is no separate
 *                            downloaded tab
 *
 * The list comes ONLY from the GitHub manifest (or its local cache) — when
 * neither is available the page shows an empty state, never sample data.
 * Every entry into this page triggers a cloud manifest refresh.
 *
 * The right-hand sub page (下载任务) slides in over this page when the
 * toolbar's download-tasks button is tapped; [handleBack] reverses it.
 */
class ModelFactoryScreen(
    private val activity: Activity,
) : FrameLayout(activity), ModelDownloadManager.Listener {

    private val cloudAdapter = ModelCardAdapter(
        isDownloaded = { ModelRepository.isDownloaded(context, it) },
        onClick = { ModelDetailDialog(activity, it).show() },
    )

    private var models: List<ModelInfo> = emptyList()
    private var cloudQuery = ""
    private var tasksShown = false
    private var refreshing = false

    private val knownResolutions = LinkedHashSet<String>()

    private lateinit var cloudPage: SwipeRefreshLayout
    private lateinit var cloudStatus: TextView
    private lateinit var tasksPage: DownloadTasksScreen
    private lateinit var filterButton: ImageButton

    /** Notified when the 下载任务 sub page opens/closes (hides the toolbar entry). */
    var onTasksPageVisibilityChanged: ((Boolean) -> Unit)? = null

    init {
        build()
        loadData()
    }

    // ── View construction ──────────────────────────────────────────────────

    private fun build() {
        setBackgroundColor(AimbotColors.SURFACE)

        val column = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(context.dp(16), context.dp(8), context.dp(16), 0)
        }
        addView(column)

        ViewCompat.setOnApplyWindowInsetsListener(this) { v, insets ->
            v.updatePadding(
                bottom = insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom
            )
            insets
        }

        // ── Search bar with trailing filter button ──
        val searchBar = buildSearchBar(context, "搜索云端模型", onFilterClick = { openFilterDialog() })
        filterButton = searchBar.filterButton!!
        searchBar.edit.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable) {
                cloudQuery = s.toString().trim()
                applyFilters()
            }
        })
        column.addView(searchBar.card)
        column.addView(context.gap(8))

        // ── Model list (pull-to-refresh) ──
        cloudPage = SwipeRefreshLayout(context).apply {
            setColorSchemeColors(AimbotColors.PRIMARY)
            setOnRefreshListener { refreshCloud() }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f,
            )
            addView(RecyclerView(context).apply {
                layoutManager = LinearLayoutManager(context)
                adapter = cloudAdapter
                overScrollMode = RecyclerView.OVER_SCROLL_NEVER
                clipToPadding = false
                setPadding(0, context.dp(8), 0, context.dp(12))
            })
        }
        column.addView(cloudPage)

        cloudStatus = TextView(context).apply {
            textSize = 14f
            setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            gravity = Gravity.CENTER
            setPadding(0, context.dp(32), 0, 0)
            visibility = View.GONE
        }
        addView(cloudStatus, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.CENTER_VERTICAL,
        ))

        // ── 下载任务 sub page (slides in, hidden by default) ──
        tasksPage = DownloadTasksScreen(context) { showCardsPage() }.apply {
            visibility = View.GONE
        }
        addView(tasksPage, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
        ))
    }

    // ── Data ───────────────────────────────────────────────────────────────

    private fun loadData() {
        // List = last successful cloud fetch (local cache) only. No cache and
        // no network means an honest empty state — never sample data.
        models = ModelRepository.loadCachedManifest(context) ?: emptyList()
        applyFilters()
        refreshCloud()
    }

    /** Pull-to-refresh + entry sync: fetch the manifest via the proxy list. */
    private fun refreshCloud() {
        if (refreshing) return
        refreshing = true
        cloudPage.isRefreshing = true
        if (models.isEmpty()) {
            cloudStatus.text = "正在同步云端清单…"
            cloudStatus.visibility = View.VISIBLE
        }
        Thread {
            val result = runCatching {
                ModelRepository.fetchCloudManifest(activity.applicationContext)
            }
            activity.runOnUiThread {
                refreshing = false
                cloudPage.isRefreshing = false
                result.onSuccess { fetched ->
                    if (fetched.isNotEmpty()) models = fetched
                }.onFailure {
                    Toast.makeText(
                        activity, "云端清单同步失败", Toast.LENGTH_SHORT,
                    ).show()
                }
                applyFilters()
            }
        }.start()
    }

    // ── Filtering ──────────────────────────────────────────────────────────

    /** Re-runs the list through search + multi-select filters. */
    private fun applyFilters() {
        models.forEach { knownResolutions.add(it.resolution) }
        if (::filterButton.isInitialized) {
            filterButton.setColorFilter(
                if (ModelFilterState.hasSelection) AimbotColors.PRIMARY
                else AimbotColors.ON_SURFACE_VARIANT
            )
        }
        cloudAdapter.submit(models)
        cloudAdapter.filter(
            cloudQuery, ModelFilterState.formats, ModelFilterState.quantize,
            ModelFilterState.resolutions,
        )
        applyCloudState()
    }

    private fun applyCloudState() {
        val filteredEmpty = models.isNotEmpty() && cloudAdapter.itemCount == 0
        cloudStatus.text = when {
            refreshing && models.isEmpty() -> "正在同步云端清单…"
            models.isEmpty() -> "云端清单加载失败\n检查网络后下拉重试"
            filteredEmpty -> "没有符合过滤条件的模型"
            else -> ""
        }
        cloudStatus.visibility =
            if (models.isEmpty() || filteredEmpty) View.VISIBLE else View.GONE
    }

    // ── ModelDownloadManager.Listener ─────────────────────────────────────

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        // Registration is what makes 已下载 badges react to a download that
        // finishes while this page is showing. The screen used to implement the
        // interface without ever registering, so the badge only appeared after
        // an app restart rebuilt the screen from scratch.
        ModelDownloadManager.addListener(this)
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        ModelDownloadManager.removeListener(this)
    }

    override fun onTasksChanged() {
        // Download complete / deleted: refresh list + 已下载 badges.
        activity.runOnUiThread { applyFilters() }
    }

    // ── Filter dialog ──────────────────────────────────────────────────────

    /** Opens the multi-select filter dialog; selections commit on 确定. */
    private fun openFilterDialog() {
        FilterDialog(
            activity,
            listOf("TFLite", "ONNX"),
            listOf("INT8", "FP16", "FP32", "混合精度"),
            knownResolutions.toList(),
            ModelFilterState.formats, ModelFilterState.quantize, ModelFilterState.resolutions,
        ) { formats, quantizes, resolutions ->
            ModelFilterState.formats.clear(); ModelFilterState.formats.addAll(formats)
            ModelFilterState.quantize.clear(); ModelFilterState.quantize.addAll(quantizes)
            ModelFilterState.resolutions.clear(); ModelFilterState.resolutions.addAll(resolutions)
            applyFilters()
        }.show()
    }

    // ── Sub-page navigation ────────────────────────────────────────────────

    /** Opens the 下载任务 sub page with a slide-in-from-right animation. */
    fun showTasksPage() {
        if (tasksShown) return
        tasksShown = true
        tasksPage.visibility = View.VISIBLE
        tasksPage.translationX = activity.dp(64).toFloat()
        tasksPage.alpha = 0f
        tasksPage.animate()
            .alpha(1f).translationX(0f)
            .setDuration(260)
            .setInterpolator(DecelerateInterpolator(1.5f))
            .start()
        onTasksPageVisibilityChanged?.invoke(true)
    }

    /** Slides the 下载任务 sub page back out to the right. */
    fun showCardsPage() {
        if (!tasksShown) return
        tasksShown = false
        tasksPage.animate()
            .alpha(0f).translationX(activity.dp(64).toFloat())
            .setDuration(200)
            .setInterpolator(AccelerateInterpolator())
            .withEndAction {
                tasksPage.visibility = View.GONE
                tasksPage.translationX = 0f
                tasksPage.alpha = 1f
            }
            .start()
        onTasksPageVisibilityChanged?.invoke(false)
    }

    /** Returns true when this call closed the tasks page (consumed the back key). */
    fun handleBack(): Boolean {
        if (!tasksShown) return false
        showCardsPage()
        return true
    }
}

/** Rounded MD3 search bar and its optional trailing filter button. */
internal class SearchBar(
    val card: MaterialCardView,
    val edit: EditText,
    val filterButton: ImageButton?,
)

/**
 * MD3 rounded search bar (pill card + magnifier SVG + borderless text field).
 * When [onFilterClick] is given, a funnel button is placed at the far right
 * of the bar. The EditText carries the hint as contentDescription so
 * uiautomator dump exposes the field even while empty.
 */
internal fun buildSearchBar(
    context: android.content.Context,
    hint: String,
    onFilterClick: (() -> Unit)? = null,
): SearchBar {
    val card = MaterialCardView(context).apply {
        radius = context.dp(28).toFloat()
        cardElevation = 0f
        strokeWidth = 0
        setCardBackgroundColor(AimbotColors.SURFACE_CONTAINER)
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, context.dp(48),
        )
    }
    val row = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(context.dp(16), 0, context.dp(8), 0)
    }
    val icon = ImageView(context).apply {
        setImageResource(R.drawable.ic_search)
        setColorFilter(AimbotColors.ON_SURFACE_VARIANT)
        layoutParams = LinearLayout.LayoutParams(context.dp(20), context.dp(20))
    }
    val edit = EditText(context).apply {
        this.hint = hint
        contentDescription = hint
        background = null
        textSize = 14f
        setTextColor(AimbotColors.ON_SURFACE)
        setHintTextColor(AimbotColors.ON_SURFACE_VARIANT)
        setSingleLine(true)
        imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH
        layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
    }
    row.addView(icon)
    row.addView(View(context).apply {
        layoutParams = LinearLayout.LayoutParams(context.dp(10), 1)
    })
    row.addView(edit)

    var filterButton: ImageButton? = null
    if (onFilterClick != null) {
        filterButton = ImageButton(context).apply {
            setImageResource(R.drawable.ic_filter)
            contentDescription = "过滤"
            setColorFilter(AimbotColors.ON_SURFACE_VARIANT)
            borderlessRipple()
            layoutParams = LinearLayout.LayoutParams(context.dp(40), context.dp(40))
            setOnClickListener { onFilterClick() }
        }
        row.addView(filterButton)
    }
    card.addView(row)
    return SearchBar(card, edit, filterButton)
}
