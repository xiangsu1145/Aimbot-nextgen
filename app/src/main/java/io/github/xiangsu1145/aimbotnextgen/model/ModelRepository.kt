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
 * GitHub access prefers a free acceleration proxy prefix and keeps a direct
 * connection as the LAST resort — see [candidateUrls].
 */
object ModelRepository {

    /** Cloud manifest, published in this very repository (models/models.json). */
    const val CLOUD_MANIFEST_URL: String =
        "https://raw.githubusercontent.com/xiangsu1145/Aimbot-nextgen/main/models/models.json"

    /**
     * Free GitHub acceleration proxies (prefix style: proxy + original URL),
     * tried top-down for both manifest fetches and model downloads.
     *
     * These are volunteer-run and churn fast: a host that works today can be
     * parked or gone next month, and a dead one usually still resolves in DNS
     * while serving nothing, so it fails by timeout rather than by name. Treat
     * this list as a starting order, not as the strategy — [candidateUrls]
     * appends a direct connection after it, and ModelDownloadManager resumes
     * partial files instead of restarting them.
     */
    val PROXY_PREFIXES: List<String> = listOf(
        "https://gh-proxy.com/",
        "https://ghproxy.net/",
        "https://ghfast.top/",
        "https://gh-proxy.net/",
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
     * Fetches the cloud manifest through [candidateUrls] (top-down, first
     * success wins) and rewrites the local cache on success. Must be called
     * off the main thread. Throws [IOException] when every source fails.
     */
    fun fetchCloudManifest(context: Context): List<ModelInfo> {
        var lastError: IOException? = null
        for (url in candidateUrls(CLOUD_MANIFEST_URL)) {
            try {
                val text = httpGetText(url)
                val models = ModelInfo.parseList(text)
                if (models.isNotEmpty()) {
                    writeManifestCache(context, models)
                    return models
                }
                lastError = IOException("empty model list via $url")
            } catch (e: IOException) {
                lastError = e
            }
        }
        throw lastError ?: IOException("every source failed")
    }

    /**
     * Every URL worth trying for one original GitHub URL, in priority order:
     * all proxies first, then the direct URL as a last resort.
     *
     * A direct attempt looks like dead weight, because raw.githubusercontent is
     * blocked often enough that this app historically never tried it. But
     * "often" is not "always", and the failure is not uniform: DNS is usually
     * clean (real 185.199.x addresses) and the TCP handshake frequently
     * succeeds, with only the TLS layer interfered with — and it comes and goes
     * by line and by hour. As the final fallback it costs one extra attempt.
     */
    fun candidateUrls(originalUrl: String): List<String> =
        PROXY_PREFIXES.map { it + originalUrl } + originalUrl

    /** Candidate download URLs for a model file, in priority order. */
    fun candidateDownloadUrls(model: ModelInfo): List<String> =
        candidateUrls(model.modelUrl)

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
