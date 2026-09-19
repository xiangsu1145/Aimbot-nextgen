package io.github.xiangsu1145.aimbotnextgen.model

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Model manifest source + private-directory layout.
 *
 * Storage layout (app-private external dir, no permission needed):
 *   <external>/Android/data/<pkg>/models/
 *     ├── models.json          cloud manifest cache (same schema as the repo one)
 *     ├── tflite/              downloaded .tflite files
 *     └── onnx/                downloaded .onnx files
 *
 * GitHub access always goes through a free acceleration proxy prefix — direct
 * raw.githubusercontent.com / github.com connections are never attempted.
 */
object ModelRepository {

    /** Cloud manifest, published in this very repository. */
    const val CLOUD_MANIFEST_URL: String =
        "https://raw.githubusercontent.com/xiangsu1145/Aimbot-nextgen/main/models.json"

    /**
     * Free GitHub acceleration proxies (prefix style: proxy + original URL).
     * Tried top-down for both manifest fetches and model downloads; the list
     * order is the only priority — no direct connection fallback, by design.
     */
    val PROXY_PREFIXES: List<String> = listOf(
        "https://gh-proxy.com/",
        "https://ghproxy.net/",
        "https://ghfast.top/",
    )

    private const val HTTP_CONNECT_TIMEOUT_MS = 10_000
    private const val HTTP_READ_TIMEOUT_MS = 20_000

    // ── Directory layout ───────────────────────────────────────────────────

    /**
     * /Android/data/<pkg>/models/. getExternalFilesDir(null) ends in
     * .../Android/data/<pkg>/files, so the shared parent is one level up.
     * Falls back to internal storage when external storage is unavailable.
     */
    fun modelsRootDir(context: Context): File {
        val external = context.getExternalFilesDir(null)?.parentFile
        return if (external != null && external.isDirectory) {
            File(external, "models")
        } else {
            File(context.filesDir, "models")
        }
    }

    fun manifestFile(context: Context): File = File(modelsRootDir(context), "models.json")

    fun tfliteDir(context: Context): File =
        File(modelsRootDir(context), ModelFormat.TFLITE.idPrefix).apply { mkdirs() }

    fun onnxDir(context: Context): File =
        File(modelsRootDir(context), ModelFormat.ONNX.idPrefix).apply { mkdirs() }

    fun targetDir(context: Context, format: ModelFormat): File =
        if (format == ModelFormat.ONNX) onnxDir(context) else tfliteDir(context)

    fun isDownloaded(context: Context, model: ModelInfo): Boolean =
        File(targetDir(context, model.format), model.fileName).isFile

    // ── Manifest access ────────────────────────────────────────────────────

    /**
     * Reads the manifest cache written by the last successful cloud fetch.
     * Returns null when the cache does not exist or no longer parses — the UI
     * then shows an empty state (no built-in sample data, by design).
     */
    fun loadCachedManifest(context: Context): List<ModelInfo>? {
        val f = manifestFile(context)
        if (!f.isFile) return null
        return runCatching { ModelInfo.parseList(f.readText()) }.getOrNull()
    }

    /**
     * Fetches the cloud manifest through the proxy list (top-down, first
     * success wins) and rewrites the local cache on success. Must be called
     * off the main thread. Throws [IOException] when every proxy fails.
     */
    fun fetchCloudManifest(context: Context): List<ModelInfo> {
        var lastError: IOException? = null
        for (prefix in PROXY_PREFIXES) {
            try {
                val text = httpGetText(prefix + CLOUD_MANIFEST_URL)
                val models = ModelInfo.parseList(text)
                if (models.isNotEmpty()) {
                    writeManifestCache(context, models)
                    return models
                }
                lastError = IOException("empty model list via $prefix")
            } catch (e: IOException) {
                lastError = e
            }
        }
        throw lastError ?: IOException("all GitHub proxies failed")
    }

    /** Candidate download URLs for a model file, one per proxy, in priority order. */
    fun candidateDownloadUrls(model: ModelInfo): List<String> =
        PROXY_PREFIXES.map { it + model.modelUrl }

    /** Writes the manifest cache atomically enough for a tiny JSON document. */
    private fun writeManifestCache(context: Context, models: List<ModelInfo>) {
        val root = modelsRootDir(context)
        if (!root.isDirectory) root.mkdirs()
        val doc = JSONObject()
            .put("version", 1)
            .put("updatedAt", System.currentTimeMillis() / 1000)
            .put("models", JSONArray().apply { models.forEach { put(it.toJsonObject()) } })
        val out = manifestFile(context)
        val tmp = File(root, "models.json.tmp")
        tmp.writeText(doc.toString(2))
        if (!tmp.renameTo(out)) {
            out.writeText(doc.toString(2))
            tmp.delete()
        }
    }

    private fun httpGetText(url: String): String {
        val conn = URL(url).openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = HTTP_CONNECT_TIMEOUT_MS
            conn.readTimeout = HTTP_READ_TIMEOUT_MS
            conn.instanceFollowRedirects = true
            conn.setRequestProperty("User-Agent", "AimbotNextgen/1.0")
            if (conn.responseCode !in 200..299) {
                throw IOException("HTTP ${conn.responseCode} from $url")
            }
            return conn.inputStream.bufferedReader().use { it.readText() }
        } finally {
            conn.disconnect()
        }
    }
}
