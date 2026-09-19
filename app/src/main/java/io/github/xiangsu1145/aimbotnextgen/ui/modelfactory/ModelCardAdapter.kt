package io.github.xiangsu1145.aimbotnextgen.ui.modelfactory

import android.content.Context
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.card.MaterialCardView
import io.github.xiangsu1145.aimbotnextgen.R
import io.github.xiangsu1145.aimbotnextgen.ui.dp
import io.github.xiangsu1145.aimbotnextgen.ui.matchParentWrapContent
import io.github.xiangsu1145.aimbotnextgen.model.ModelFormat
import io.github.xiangsu1145.aimbotnextgen.model.ModelInfo
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

/** Per-format inline SVG (VectorDrawable) type icon. */
fun modelTypeIcon(format: ModelFormat): Int = when (format) {
    ModelFormat.TFLITE -> R.drawable.ic_model_tflite
    ModelFormat.ONNX -> R.drawable.ic_model_onnx
}

/**
 * Adapter for the model-card list.
 *
 * Card layout — type SVG icon vertically centered on the left, model name as
 * the title, a `量化 · 分辨率 · 类别数` meta line underneath, and a green
 * 已下载 badge on the trailing edge for models already stored locally.
 */
class ModelCardAdapter(
    private val isDownloaded: (ModelInfo) -> Boolean,
    private val onClick: (ModelInfo) -> Unit,
) : RecyclerView.Adapter<ModelCardAdapter.CardHolder>() {

    private var all: List<ModelInfo> = emptyList()
    private var shown: List<ModelInfo> = emptyList()
    private var query: String = ""
    private var formats: Set<String> = emptySet()
    private var quantizes: Set<String> = emptySet()
    private var resolutions: Set<String> = emptySet()

    /** Replaces the backing list and re-applies the current filters. */
    fun submit(models: List<ModelInfo>) {
        all = models
        refilter()
    }

    /**
     * Combined filter: name/id search + multi-select sets (a set with no
     * selection matches all; rows AND together).
     */
    fun filter(
        query: String,
        formats: Set<String> = emptySet(),
        quantizes: Set<String> = emptySet(),
        resolutions: Set<String> = emptySet(),
    ) {
        this.query = query.trim()
        this.formats = formats
        this.quantizes = quantizes
        this.resolutions = resolutions
        refilter()
    }

    private fun refilter() {
        shown = all.filter { m ->
            (query.isEmpty() ||
                m.modelName.contains(query, ignoreCase = true) ||
                m.modelId.contains(query, ignoreCase = true)) &&
                (formats.isEmpty() || m.format.label in formats) &&
                (quantizes.isEmpty() || m.quantize in quantizes) &&
                (resolutions.isEmpty() || m.resolution in resolutions)
        }
            // Downloaded models float to the top; each group keeps manifest order.
            .sortedByDescending { isDownloaded(it) }
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): CardHolder =
        CardHolder(buildModelCard(parent.context), isDownloaded)


    override fun getItemCount(): Int = shown.size

    override fun onBindViewHolder(holder: CardHolder, position: Int) {
        val model = shown[position]
        holder.bind(model)
        holder.card.setOnClickListener { onClick(model) }
    }

    class CardHolder(
        val card: MaterialCardView,
        private val isDownloaded: (ModelInfo) -> Boolean,
    ) : RecyclerView.ViewHolder(card) {
        fun bind(model: ModelInfo) {
            val (icon, title, meta, badge) = card.tag as CardChildren
            icon.setImageResource(modelTypeIcon(model.format))
            title.text = model.modelName
            meta.text = model.metaLine
            badge.visibility = if (isDownloaded(model)) View.VISIBLE else View.GONE
            card.contentDescription = model.modelName
        }
    }

    companion object {
        /**
         * Builds the card skeleton once per holder; [CardHolder.bind] swaps the
         * content when the list scrolls or refilters.
         */
        fun buildModelCard(context: Context): MaterialCardView {
            val card = MaterialCardView(context).apply {
                layoutParams = matchParentWrapContent().apply { topMargin = context.dp(8) }
                radius = context.dp(16).toFloat()
                cardElevation = 0f
                strokeWidth = 0
                isClickable = true
                isFocusable = true
            }

            val row = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(context.dp(16), context.dp(14), context.dp(16), context.dp(14))
                gravity = Gravity.CENTER_VERTICAL
            }

            val icon = ImageView(context).apply {
                layoutParams = LinearLayout.LayoutParams(context.dp(32), context.dp(32))
                setColorFilter(AimbotColors.PRIMARY)
            }
            row.addView(icon)

            row.addView(View(context).apply {
                layoutParams = LinearLayout.LayoutParams(context.dp(14), 1)
            })

            val textCol = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams =
                    LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            }
            val title = TextView(context).apply {
                textSize = 16f
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                setTextColor(AimbotColors.ON_SURFACE)
            }
            val meta = TextView(context).apply {
                textSize = 12f
                setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            }
            textCol.addView(title)
            textCol.addView(meta)
            row.addView(textCol)

            // Trailing 已下载 badge (green check icon + label), shown only for
            // models whose file already exists in the private models directory.
            val badge = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                visibility = View.GONE
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                ).apply { marginStart = context.dp(8) }
            }
            badge.addView(ImageView(context).apply {
                setImageResource(R.drawable.ic_downloaded)
                setColorFilter(AimbotColors.SHELL_STATUS_RUNNING)
                layoutParams = LinearLayout.LayoutParams(context.dp(16), context.dp(16))
            })
            badge.addView(TextView(context).apply {
                text = "已下载"
                textSize = 11f
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                setTextColor(AimbotColors.SHELL_STATUS_RUNNING)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                ).apply { marginStart = context.dp(3) }
            })
            row.addView(badge)

            card.addView(row)
            card.tag = CardChildren(icon, title, meta, badge)
            return card
        }

        private data class CardChildren(
            val icon: ImageView,
            val title: TextView,
            val meta: TextView,
            val badge: LinearLayout,
        )
    }
}
