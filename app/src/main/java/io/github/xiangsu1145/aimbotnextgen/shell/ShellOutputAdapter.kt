package io.github.xiangsu1145.aimbotnextgen.shell

import android.graphics.Typeface
import android.view.Gravity
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

class ShellOutputAdapter : RecyclerView.Adapter<ShellOutputAdapter.ViewHolder>() {

    private val lines = mutableListOf<String>()

    class ViewHolder(val textView: TextView) : RecyclerView.ViewHolder(textView)

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val tv = TextView(parent.context).apply {
            layoutParams = RecyclerView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
            setPadding(12, 4, 12, 4)
            textSize = 11f
            typeface = Typeface.MONOSPACE
            setTextColor(AimbotColors.TERMINAL_TEXT)
            setBackgroundColor(AimbotColors.TERMINAL_BG)
            setHorizontallyScrolling(true)
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
            gravity = Gravity.CENTER_VERTICAL
        }
        return ViewHolder(tv)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        holder.textView.text = lines[position]
    }

    override fun getItemCount(): Int = lines.size

    fun addLine(line: String) {
        lines.add(line)
        if (lines.size > 10) {
            lines.removeAt(0)
            notifyItemRemoved(0)
        }
        notifyItemInserted(lines.size - 1)
    }

    fun setLines(newLines: List<String>) {
        lines.clear()
        lines.addAll(newLines.takeLast(10))
        notifyDataSetChanged()
    }

    fun clear() {
        val size = lines.size
        lines.clear()
        notifyItemRangeRemoved(0, size)
    }
}
