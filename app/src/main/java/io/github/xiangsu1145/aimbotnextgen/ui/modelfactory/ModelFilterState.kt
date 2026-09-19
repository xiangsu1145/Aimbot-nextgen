package io.github.xiangsu1145.aimbotnextgen.ui.modelfactory

/**
 * Process-wide filter selection state (user requirement: keep filter state in
 * variables that survive screen/dialog recreation). The filter dialog edits
 * temporary copies and commits back into these sets on 确定.
 */
object ModelFilterState {
    val formats = LinkedHashSet<String>()
    val quantize = LinkedHashSet<String>()
    val resolutions = LinkedHashSet<String>()

    val hasSelection: Boolean
        get() = formats.isNotEmpty() || quantize.isNotEmpty() || resolutions.isNotEmpty()

    fun clearAll() {
        formats.clear()
        quantize.clear()
        resolutions.clear()
    }
}
