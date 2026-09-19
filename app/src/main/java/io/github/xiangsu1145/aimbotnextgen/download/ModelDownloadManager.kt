package io.github.xiangsu1145.aimbotnextgen.download

import android.content.Context
import android.os.Handler
import android.os.Looper
import io.github.xiangsu1145.aimbotnextgen.model.ModelInfo
import io.github.xiangsu1145.aimbotnextgen.model.ModelRepository
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.LinkedHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors

/**
 * Background model download queue.
 *
 * - One worker thread: tasks run sequentially, in enqueue order, and keep
 *   running after the detail dialog is dismissed (the dialog only listens).
 * - Downloads always go through [ModelRepository.PROXY_PREFIXES] (top-down,
 *   first success wins); direct GitHub connections are never attempted.
 * - Files land in the format-specific private directory via a `.part` temp
 *   file that is renamed on completion, so a half-written file is never
 *   mistaken for a usable model.
 *
 * The manager is process-wide: the UI re-attaches listeners on every screen
 * rebuild and re-reads state from [tasks] / [taskFor], so Activity recreation
 * never loses progress.
 */
object ModelDownloadManager {

    enum class State { QUEUED, RUNNING, COMPLETED, FAILED, CANCELLED }

    interface Listener {
        /** Called on the main thread after any task state/progress change. */
        fun onTasksChanged()
    }

    class Task internal constructor(val model: ModelInfo) {
        @Volatile var state: State = State.QUEUED
            internal set
        @Volatile var downloadedBytes: Long = 0
            internal set
        @Volatile var totalBytes: Long = -1L
            internal set
        @Volatile var error: String? = null
            internal set

        val isActive: Boolean
            get() = state == State.QUEUED || state == State.RUNNING

        /** 0..100, or -1 when the total size is still unknown. */
        val percent: Int
            get() = if (totalBytes > 0) ((downloadedBytes * 100) / totalBytes).toInt() else -1
    }

    private val executor = Executors.newSingleThreadExecutor { r -> Thread(r, "model-dl") }
    private val tasks = LinkedHashMap<String, Task>()
    private val listeners = CopyOnWriteArrayList<Listener>()
    private val main = Handler(Looper.getMainLooper())

    @Volatile private var appContext: Context? = null

    fun addListener(l: Listener) { listeners.add(l); l.onTasksChanged() }
    fun removeListener(l: Listener) { listeners.remove(l) }

    fun tasksSnapshot(): List<Task> = synchronized(tasks) { ArrayList(tasks.values) }

    fun taskFor(modelId: String): Task? = synchronized(tasks) { tasks[modelId] }

    /**
     * Queues a model for download. Returns the existing task when this model
     * is already queued/running/done, null when the file is already on disk
     * (nothing to download).
     */
    fun enqueue(context: Context, model: ModelInfo): Task? {
        if (ModelRepository.isDownloaded(context, model)) return null
        synchronized(tasks) {
            tasks[model.modelId]?.let { return it }
            val task = Task(model)
            tasks[model.modelId] = task
            appContext = context.applicationContext
            notifyChanged()
            executor.execute { runTask(task) }
            return task
        }
    }

    /** Cancels an active task; completed files are left untouched. */
    fun cancel(modelId: String) {
        synchronized(tasks) { tasks[modelId] }?.let { task ->
            if (task.isActive) {
                task.state = State.CANCELLED
                notifyChanged()
            }
        }
    }

    /**
     * Deletes a downloaded model file from the private models directory and
     * notifies listeners so the 已下载 list refreshes.
     */
    fun delete(context: Context, model: ModelInfo): Boolean {
        val file = File(ModelRepository.targetDir(context, model.format), model.fileName)
        val ok = !file.exists() || file.delete()
        if (ok) notifyChanged()
        return ok
    }

    /** Removes finished (non-active) tasks from the list view. */
    fun clearFinished() {
        synchronized(tasks) {
            tasks.entries.removeAll { !it.value.isActive }
        }
        notifyChanged()
    }

    // ── Worker ─────────────────────────────────────────────────────────────

    private fun runTask(task: Task) {
        val context = appContext ?: run { task.state = State.FAILED; return }
        task.state = State.RUNNING
        notifyChanged()

        val dir = ModelRepository.targetDir(context, task.model.format)
        if (!dir.isDirectory) dir.mkdirs()
        val outFile = File(dir, task.model.fileName)
        val partFile = File(dir, task.model.fileName + ".part")

        var lastError: IOException? = null
        for (url in ModelRepository.candidateDownloadUrls(task.model)) {
            if (task.state == State.CANCELLED) return
            try {
                val size = downloadToFile(url, partFile, task)
                if (task.state == State.CANCELLED) {
                    partFile.delete()
                    return
                }
                if (!partFile.renameTo(outFile)) {
                    // Same-volume rename of a fresh temp file should never fail;
                    // fall back to a plain copy before giving up.
                    partFile.copyTo(outFile, overwrite = true)
                    partFile.delete()
                }
                task.totalBytes = size
                task.state = State.COMPLETED
                notifyChanged()
                return
            } catch (e: IOException) {
                lastError = e
                partFile.delete()
            }
        }
        task.error = lastError?.message ?: "download failed"
        task.state = State.FAILED
        notifyChanged()
    }

    /** Streams [url] into [out], reporting throttled progress on the way. */
    private fun downloadToFile(url: String, out: File, task: Task): Long {
        val conn = URL(url).openConnection() as HttpURLConnection
        var copied = 0L
        try {
            conn.connectTimeout = 10_000
            conn.readTimeout = 30_000
            conn.instanceFollowRedirects = true
            conn.setRequestProperty("User-Agent", "AimbotNextgen/1.0")
            if (conn.responseCode !in 200..299) {
                throw IOException("HTTP ${conn.responseCode}")
            }
            task.totalBytes = conn.contentLengthLong.takeIf { it > 0 } ?: -1L
            notifyChanged()

            conn.inputStream.use { input ->
                out.outputStream().use { output ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        if (task.state == State.CANCELLED) {
                            throw IOException("cancelled")
                        }
                        val n = input.read(buf)
                        if (n < 0) break
                        output.write(buf, 0, n)
                        copied += n
                        maybeNotifyProgress(task, copied)
                    }
                }
            }
        } finally {
            conn.disconnect()
        }
        return copied
    }

    // ── Notification ───────────────────────────────────────────────────────

    private var lastNotifyAt = 0L

    /** Progress callbacks are throttled to ~100 ms so the UI thread stays idle. */
    private fun maybeNotifyProgress(task: Task, copied: Long) {
        task.downloadedBytes = copied
        val now = System.currentTimeMillis()
        if (now - lastNotifyAt >= 100) {
            lastNotifyAt = now
            notifyChanged()
        }
    }

    private fun notifyChanged() {
        main.post {
            for (l in listeners) l.onTasksChanged()
        }
    }
}
