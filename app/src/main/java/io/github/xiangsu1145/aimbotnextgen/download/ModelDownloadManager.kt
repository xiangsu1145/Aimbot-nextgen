package io.github.xiangsu1145.aimbotnextgen.download

import android.content.Context
import android.os.Handler
import android.os.Looper
import io.github.xiangsu1145.aimbotnextgen.model.ModelInfo
import io.github.xiangsu1145.aimbotnextgen.model.ModelRepository
import java.io.File
import java.io.FileOutputStream
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
 * - Downloads go through [ModelRepository.candidateUrls] (top-down, first
 *   success wins) and RESUME from whatever a previous attempt already wrote,
 *   so a flaky connection costs seconds of work rather than the whole file.
 * - Files land in the format-specific private directory via a `.part` temp
 *   file that is renamed on completion, so a half-written file is never
 *   mistaken for a usable model.
 * - A download is only accepted when its byte count matches the length the
 *   server declared. A resume that silently stitches two different bodies
 *   together would otherwise look like a success.
 *
 * The manager is process-wide: the UI re-attaches listeners on every screen
 * rebuild and re-reads state from [tasks] / [taskFor], so Activity recreation
 * never loses progress.
 */
object ModelDownloadManager {

    enum class State { QUEUED, RUNNING, COMPLETED, FAILED, CANCELLED }

    private const val CONNECT_TIMEOUT_MS = 10_000

    /**
     * Per-read timeout, not per-download. On a bad line the gaps BETWEEN
     * packets are what grow, so this has to be generous: too small and every
     * attempt dies mid-file, which is exactly the case resume exists for.
     */
    private const val READ_TIMEOUT_MS = 60_000

    /**
     * How many times one source is retried before moving to the next. Retrying
     * the SAME source is what makes resume pay off — a different source may
     * answer with a different byte range, while the same one picks up where it
     * stopped.
     */
    private const val ATTEMPTS_PER_SOURCE = 3

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
     * Queues a model for download. Returns the active task when one is already
     * running, null when the file is already on disk (nothing to do).
     *
     * A finished-but-unsuccessful task is RESTARTED rather than handed back.
     * On a bad connection "tap 下载 again" is the natural retry, and returning
     * the dead task would leave the user looking at an old error with no way
     * forward except clearing the list first. The restart resumes from the
     * `.part` file, so it costs only the bytes actually missing.
     */
    fun enqueue(context: Context, model: ModelInfo): Task? {
        if (ModelRepository.isDownloaded(context, model)) return null
        synchronized(tasks) {
            appContext = context.applicationContext
            val existing = tasks[model.modelId]
            if (existing != null) {
                if (existing.isActive) return existing
                existing.error = null
                existing.downloadedBytes = 0
                existing.totalBytes = -1
                existing.state = State.QUEUED
                notifyChanged()
                executor.execute { runTask(existing) }
                return existing
            }
            val task = Task(model)
            tasks[model.modelId] = task
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
        val dir = ModelRepository.targetDir(context, model.format)
        val file = File(dir, model.fileName)
        // The .part file goes with it. Leaving it behind would make the next
        // download silently resume into a prefix of the very file the user
        // just deleted — a corrupt model that reports success.
        File(dir, model.fileName + ".part").delete()
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
            repeat(ATTEMPTS_PER_SOURCE) {
                if (task.state == State.CANCELLED) {
                    // Keep the .part file: a cancel means "not now", not "throw
                    // away the bytes already paid for".
                    return
                }
                try {
                    val expected = downloadToFile(url, partFile, task)
                    if (expected > 0 && partFile.length() != expected) {
                        throw IOException(
                            "short download: ${partFile.length()} of $expected bytes")
                    }
                    if (!partFile.renameTo(outFile)) {
                        // Same-volume rename of a fresh temp file should never
                        // fail; fall back to a plain copy before giving up.
                        partFile.copyTo(outFile, overwrite = true)
                        partFile.delete()
                    }
                    task.totalBytes = if (expected > 0) expected else partFile.length()
                    task.downloadedBytes = task.totalBytes
                    task.state = State.COMPLETED
                    notifyChanged()
                    return
                } catch (e: IOException) {
                    // Deliberately KEEP the .part file — the next attempt (same
                    // source, or the next one) resumes from it. Losing it here
                    // is what turned a brief stall into "downloaded 9 MB for
                    // nothing, start again", which on a bad line never ends.
                    //
                    // A source that answered with rubbish (an error page, a
                    // truncated body) does not slip through: the length check
                    // above rejects it, and a wrong-sized partial makes the
                    // next server answer 416, which drops it.
                    lastError = e
                }
            }
        }
        task.error = lastError?.message ?: "download failed"
        task.state = State.FAILED
        notifyChanged()
    }

    /**
     * Streams [url] into [out], resuming when [out] already holds a prefix of
     * the file, and returns the file's full length (-1 when the server will
     * not say).
     *
     * Resume is negotiated, never assumed:
     *   * `Range: bytes=<have>-` is sent whenever there is something to resume
     *     from;
     *   * **206** means the server honoured it and the body is a suffix ⇒ append;
     *   * **200** means it ignored the header and is sending the WHOLE file ⇒
     *     the partial must be overwritten, not appended to;
     *   * **416** means our offset is past the end (stale or oversized partial)
     *     ⇒ drop the partial and let the caller start over.
     *
     * Appending a 200 body onto an existing partial is the one mistake that
     * yields a corrupt file that looks like a success, so the two cases are
     * kept explicitly apart rather than sharing one write path.
     */
    private fun downloadToFile(url: String, out: File, task: Task): Long {
        val have = if (out.isFile) out.length() else 0L
        val conn = URL(url).openConnection() as HttpURLConnection
        var written = have
        var expected = -1L
        try {
            conn.connectTimeout = CONNECT_TIMEOUT_MS
            conn.readTimeout = READ_TIMEOUT_MS
            conn.instanceFollowRedirects = true
            conn.setRequestProperty("User-Agent", "AimbotNextgen/1.0")
            if (have > 0) conn.setRequestProperty("Range", "bytes=$have-")

            val code = conn.responseCode
            when (code) {
                200 -> written = 0L            // whole body: start from scratch
                206 -> Unit                    // suffix: append after `have`
                416 -> {
                    out.delete()
                    throw IOException("range at $have bytes rejected")
                }
                else -> throw IOException("HTTP $code")
            }

            // A 206 states the FULL length in Content-Range ("bytes a-b/total")
            // while its Content-Length covers only the suffix — so the two
            // cases need different arithmetic, not one shared formula.
            expected = if (code == 206) {
                totalFromContentRange(conn).takeIf { it > 0 }
                    ?: conn.contentLengthLong.takeIf { it > 0 }?.let { have + it }
                    ?: -1L
            } else {
                conn.contentLengthLong.takeIf { it > 0 } ?: -1L
            }
            task.totalBytes = expected
            task.downloadedBytes = written
            notifyChanged()

            conn.inputStream.use { input ->
                FileOutputStream(out, code == 206).use { output ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        if (task.state == State.CANCELLED) {
                            throw IOException("cancelled")
                        }
                        val n = input.read(buf)
                        if (n < 0) break
                        output.write(buf, 0, n)
                        written += n
                        maybeNotifyProgress(task, written)
                    }
                }
            }
        } finally {
            conn.disconnect()
        }
        return expected
    }

    /** Full length from a 206 header `Content-Range: bytes first-last/total`. */
    private fun totalFromContentRange(conn: HttpURLConnection): Long {
        val header = conn.getHeaderField("Content-Range") ?: return -1L
        return header.substringAfterLast('/', "").trim().toLongOrNull() ?: -1L
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
