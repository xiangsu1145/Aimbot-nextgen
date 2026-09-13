package io.github.xiangsu1145.aimbotnextgen.shell

import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.util.Log

/**
 * The frame producer.
 *
 * A virtual display mirrors the physical screen into an [ImageReader], and a
 * thread hands each frame's RGBA plane to native through
 * [ShellNative.capturePushFrame]. Nothing here needs root or MediaProjection:
 * the daemon is `app_process` under shell UID 2000, which is inside the
 * platform-signed permission domain, so SurfaceFlinger lets it build a display
 * and composites into it.
 *
 * Why the framework's [DisplayManager] rather than the native
 * `SurfaceComposerClient` the rest of the daemon uses:
 *
 *  * the daemon has no Context, and `DisplayManager` needs one — [SysDisplay]
 *    borrows the framework's own;
 *  * the native route needs the consumer side of a graphic buffer queue, whose
 *    types (`BufferItem`, `CpuConsumer::LockedBuffer`) and virtual tables live
 *    in libgui's *private* headers. Reaching them would mean vendoring a slice
 *    of the AOSP tree and pinning it to one platform release.
 *
 * `DisplayManager.createVirtualDisplay` reaches the very same SurfaceFlinger
 * entry point the native call would, so this is the same mechanism wearing a
 * friendlier API.
 *
 * ── Size is a crop, not a scale ───────────────────────────────────────────────
 *
 * The menu's slider picks how much of the screen to keep, and the answer is
 * taken from the **centre** at native resolution. Getting that out of the
 * framework is the one non-obvious thing in this file:
 *
 *  * SurfaceFlinger decides what a mirrored display contains through
 *    `Transaction.setDisplayProjection`, which would let a small surface sample
 *    a sub-rectangle of the screen — the efficient way to do this. But the
 *    token it needs is not reachable from Java: `VirtualDisplay` only exposes
 *    its `IVirtualDisplayCallback` (a callback, not the display token), so the
 *    projection can neither be read nor set from here.
 *  * A virtual display built smaller than the screen therefore makes the
 *    framework **scale** the whole mirror into the surface. At 2K that is a
 *    ~5x downscale: a blurred thumbnail, useless for a detector.
 *  * So the display is built at the screen's native size, which makes the
 *    mirror 1:1 and pixel-exact, and the crop is taken on the native side as
 *    the frame crosses (see capture::pushFrame). Only the square that survives
 *    is copied; the rest of the screen is discarded where it already is.
 *
 * The pleasant side effect: the crop side is not baked into the virtual
 * display, so dragging the slider only moves a rectangle — [setCrop] — instead
 * of rebuilding a display and reallocating a full-resolution buffer. A rebuild
 * is only needed when the screen itself changes shape, which is what
 * [needsRestart] polls for.
 */
object ScreenCapture {

    private const val TAG = "aimbot_capture"

    /** `DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC` — the display is world-visible. */
    private const val VIRTUAL_DISPLAY_FLAG_PUBLIC = 0x1

    /**
     * Frames the producer may hold at once.
     *
     * Two is the minimum that works — one with the consumer, one with the
     * producer. Three keeps one in hand to absorb one slow consumer without
     * losing frames, but costs ~25 MB extra at 2K. On memory-constrained
     * devices (shell process, OEM OOM killers) the trade favours two: a
     * dropped frame at 120 Hz is invisible, but 25 MB can be the difference
     * between surviving and being killed.
     */
    private const val kMaxImages = 3

    /** Screen shape a producer was built against, so a rotation can be noticed. */
    private data class Geometry(val width: Int, val height: Int, val rotation: Int)

    /** The square being cut out of the screen, in screen pixels. */
    private data class Crop(val x: Int, val y: Int, val side: Int)

    @Volatile private var virtualDisplay: VirtualDisplay? = null
    @Volatile private var reader: ImageReader? = null
    @Volatile private var pump: Thread? = null
    @Volatile private var running = false

    @Volatile private var crop = Crop(0, 0, 0)
    @Volatile private var geometry: Geometry? = null

    /** Minimum interval between full rebuilds to avoid rapid stop/start cycles. */
    private const val kMinRestartIntervalMs = 5_000L
    @Volatile private var lastStartTime = 0L

    /** The crop side of the live producer, or 0 when idle. */
    fun activeSize(): Int = if (running) crop.side else 0

    /** Whether the virtual display and reader are still alive. */
    fun isAlive(): Boolean = running && virtualDisplay != null && reader != null

    /**
     * Whether the live producer is stale — either nothing is running, or the
     * screen changed shape under it (the user rotated the phone) and the mirror
     * is no longer 1:1. A changed crop side is *not* a reason to rebuild; that
     * is handled by [setCrop] without touching the display.
     */
    fun needsRestart(): Boolean {
        if (!running) return true
        val now = liveGeometry() ?: return false  // unreadable: keep what works
        // Only rebuild on size change (width/height). Rotation does not affect
        // the virtual display — the crop handles orientation, and the display
        // is built at the screen's native resolution either way.
        return now.width != geometry?.width || now.height != geometry?.height
    }

    /**
     * Builds a producer that mirrors the whole screen. Replaces any existing
     * one. Returns false — with the reason in logcat — when the platform
     * refuses. [side] is the initial crop; see [setCrop].
     */
    @Synchronized
    fun start(side: Int): Boolean {
        // If already running with matching dimensions, just update the crop
        // instead of tearing down the whole display + ImageReader chain.
        if (running && reader != null && virtualDisplay != null) {
            val now = liveGeometry()
            if (now != null && now.width == geometry?.width && now.height == geometry?.height) {
                val nextCrop = centredCrop(side, now.width, now.height)
                if (nextCrop != crop) {
                    crop = nextCrop
                    Log.i(TAG, "crop updated to ${crop.side} at (${crop.x},${crop.y}) (no rebuild)")
                }
                return true
            }
            // Throttle: don't rebuild more than once every kMinRestartIntervalMs
            val now2 = System.currentTimeMillis()
            if (now2 - lastStartTime < kMinRestartIntervalMs) {
                Log.d(TAG, "start throttled (${now2 - lastStartTime}ms since last start, " +
                        "need ${now?.width}x${now?.height} ≠ ${geometry?.width}x${geometry?.height})")
                return true  // "ok" to avoid supervisor retry loop
            }
        }

        stop()
        lastStartTime = System.currentTimeMillis()

        val dm = SysDisplay.displayManager()
        if (dm == null) {
            Log.e(TAG, "no DisplayManager")
            return false
        }

        val geom = liveGeometry()
        if (geom == null) {
            Log.e(TAG, "display size unavailable — cannot build a 1:1 mirror")
            return false
        }

        val reader = try {
            ImageReader.newInstance(geom.width, geom.height, PixelFormat.RGBA_8888, kMaxImages)
        } catch (t: Throwable) {
            Log.e(TAG, "ImageReader.newInstance(${geom.width}x${geom.height}) failed", t)
            return false
        }

        this.reader = reader
        geometry = geom
        running = true
        crop = centredCrop(side, geom.width, geom.height)
        ShellNative.captureSetRunning(true)

        val t = Thread({ pump(reader) }, "aimbot-capture")
        t.isDaemon = true
        pump = t
        t.start()

        val display = try {
            dm.createVirtualDisplay(
                "aimbot-capture", geom.width, geom.height, SysDisplay.density(),
                reader.surface, VIRTUAL_DISPLAY_FLAG_PUBLIC
            )
        } catch (t: Throwable) {
            Log.e(TAG, "createVirtualDisplay(${geom.width}x${geom.height}) failed", t)
            running = false
            ShellNative.captureSetRunning(false)
            reader.close()
            return false
        }

        virtualDisplay = display

        Log.i(TAG, "mirror up ${geom.width}x${geom.height}@${geom.rotation}, " +
                "crop ${crop.side} at (${crop.x},${crop.y})")
        return true
    }

    /**
     * Moves the crop without rebuilding anything — the virtual display is at
     * native size either way, so this is just a rectangle the frame thread reads
     * on its next pass.
     */
    fun setCrop(side: Int) {
        val geom = geometry ?: return
        val next = centredCrop(side, geom.width, geom.height)
        if (next == crop) return
        crop = next
        Log.i(TAG, "crop set to ${next.side} at (${next.x},${next.y})")
    }

    /** Tears the producer down. Idempotent. */
    @Synchronized
    fun stop() {
        if (!running && reader == null && virtualDisplay == null) return

        running = false
        crop = Crop(0, 0, 0)
        geometry = null
        ShellNative.captureSetRunning(false)

        val t = pump
        pump = null
        t?.interrupt()

        runCatching { virtualDisplay?.release() }
            .onFailure { Log.w(TAG, "release failed", it) }
        virtualDisplay = null

        // Closing the reader is what unblocks a thread parked in
        // acquireLatestImage(); it comes back as IllegalStateException, which
        // the pump treats as "shutting down" rather than an error.
        runCatching { reader?.close() }
            .onFailure { Log.w(TAG, "reader close failed", it) }
        reader = null

        Log.i(TAG, "capture stopped")
    }

    /** The square of `side` pixels centred on a `w x h` screen, clamped to fit. */
    private fun centredCrop(side: Int, w: Int, h: Int): Crop {
        val s = side.coerceIn(1, minOf(w, h))
        return Crop((w - s) / 2, (h - s) / 2, s)
    }

    // ── Frame pump ───────────────────────────────────────────────────────────

    /**
     * Pulls the newest frame and hands its centre crop to native.
     *
     * `acquireLatestImage` blocks until a frame is ready and discards any that
     * piled up behind it, which is the behaviour wanted here: a stale frame is
     * worth less than a fresh one, and the menu is showing the capture, not
     * recording it.
     *
     * The crop is re-read every pass so a slider drag takes effect on the next
     * frame without a rebuild.
     */
    private fun pump(reader: ImageReader) {
        Log.i(TAG, "frame pump running")
        while (running && !Thread.currentThread().isInterrupted) {
            val image = try {
                reader.acquireLatestImage()
            } catch (t: Throwable) {
                if (running) Log.e(TAG, "acquireLatestImage failed", t)
                break
            }

            if (image == null) {
                // acquireLatestImage does not block — an empty queue comes
                // straight back as null. Going round again immediately turned
                // this into a busy spin: tens of thousands of acquire attempts
                // a second, each allocating a throwaway Image on the way in,
                // which is what had the pump pinned at ~80% of a core and the
                // GC clearing a few hundred MB every couple of seconds while
                // the menu was showing nothing at all.
                //
                // 4 ms is a 250 Hz poll: well above the display's frame rate,
                // so no frame is late, and effectively free when idle.
                try {
                    Thread.sleep(4)
                } catch (_: InterruptedException) {
                    break
                }
                continue
            }

            try {
                val c = crop
                if (c.side > 0) {
                    val plane = image.planes[0]
                    ShellNative.capturePushFrame(
                        plane.buffer, image.width, image.height, plane.rowStride,
                        c.x, c.y, c.side
                    )
                }
            } catch (t: Throwable) {
                Log.e(TAG, "frame push failed", t)
            } finally {
                runCatching { image.close() }
            }
        }
        Log.i(TAG, "frame pump exited")
    }

    // ── Display geometry ─────────────────────────────────────────────────────

    /**
     * The default display's current size and rotation, or null if it cannot be
     * read. [SysDisplay] explains why the daemon reads this for itself and why
     * the panel's size is the one that is wanted.
     */
    private fun liveGeometry(): Geometry? =
        SysDisplay.defaultGeometry()?.let { Geometry(it.first, it.second, it.third) }

    // ── Context plumbing ─────────────────────────────────────────────────────

    /** [SysDisplay] owns this; the mirror only needs the manager to build one. */
    private fun displayManager(): DisplayManager? = SysDisplay.displayManager()

}
