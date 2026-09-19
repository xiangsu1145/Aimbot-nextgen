package io.github.xiangsu1145.aimbotnextgen.model

import org.json.JSONArray
import org.json.JSONObject

/**
 * Model runtime format. Derived from the [ModelInfo.modelId] prefix
 * ("tflite-xxx" / "onnx-xxx"), which also decides the target sub-directory
 * under the app's private models folder.
 */
enum class ModelFormat(val idPrefix: String, val fileExt: String, val label: String) {
    TFLITE("tflite", ".tflite", "TFLite"),
    ONNX("onnx", ".onnx", "ONNX");

    companion object {
        fun fromId(modelId: String): ModelFormat? =
            entries.firstOrNull { modelId.startsWith(it.idPrefix + "-") }
    }
}

/**
 * One entry of the model manifest (cloud `models.json` and its local cache).
 *
 * Field names follow the manifest schema exactly — see /models.json at the
 * repository root for the template.
 */
data class ModelInfo(
    val modelId: String,
    val modelName: String,
    val modelUrl: String,
    val description: String,
    val resolution: String,
    val quantize: String,
    val classCount: Int,
) {
    val format: ModelFormat
        get() = ModelFormat.fromId(modelId) ?: ModelFormat.TFLITE

    /** File name inside models/tflite/ or models/onnx/. */
    val fileName: String
        get() = modelId + format.fileExt

    /** One-line meta string shown under the card title: 量化 · 分辨率 · 类别数. */
    val metaLine: String
        get() = "$quantize · $resolution · ${classCount}类"

    fun toJsonObject(): JSONObject = JSONObject().apply {
        put("modelId", modelId)
        put("modelName", modelName)
        put("modelUrl", modelUrl)
        put("description", description)
        put("resolution", resolution)
        put("quantize", quantize)
        put("classCount", classCount)
    }

    companion object {
        fun fromJson(o: JSONObject): ModelInfo {
            val modelId = o.optString("modelId")
            require(modelId.isNotBlank()) { "manifest entry missing modelId" }
            return ModelInfo(
                modelId = modelId,
                modelName = o.optString("modelName", modelId),
                modelUrl = o.optString("modelUrl"),
                description = o.optString("description"),
                resolution = o.optString("resolution", "640x640"),
                quantize = o.optString("quantize", "FP32"),
                classCount = o.optInt("classCount", 0),
            )
        }

        /**
         * Accepts either the full document `{"models":[...]}` (with an optional
         * "version"/"updatedAt" envelope ignored here) or a bare array for
         * forward compatibility.
         */
        fun parseList(text: String): List<ModelInfo> {
            val arr: JSONArray = when {
                text.trimStart().startsWith("[") -> JSONArray(text)
                else -> JSONObject(text).optJSONArray("models") ?: JSONArray()
            }
            val out = ArrayList<ModelInfo>(arr.length())
            for (i in 0 until arr.length()) {
                runCatching { out.add(fromJson(arr.getJSONObject(i))) }
            }
            return out
        }
    }
}
