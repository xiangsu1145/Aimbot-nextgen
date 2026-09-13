package io.github.xiangsu1145.aimbotnextgen.shell

import android.content.Context
import android.content.ContextWrapper
import android.content.res.Resources
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.graphics.Point
import android.graphics.Rect
import android.hardware.display.DisplayManager
import android.media.Image
import android.media.ImageReader
import android.os.IBinder
import android.os.Looper
import android.util.Log
import java.io.FileOutputStream
import java.util.concurrent.atomic.AtomicReference

/**
 * TEMP bring-up probe: what does a capture actually contain?
 *
 * The menu's preview only tells you "some pixels arrived", not whether the
 * virtual display *scaled* the screen into the well or *cropped* a region out
 * of it — both look like a picture. This dumps the raw frame to a PNG so the
 * question can be answered by looking.
 *
 * ```
 * adb shell "CLASSPATH=<apk> app_process /system/bin \
 *     io.github.xiangsu1145.aimbotnextgen.shell.CaptureDump <mode> <size>"
 * ```
 *   current <n>   the shipping setup: an n x n virtual display          -> dump_current_<n>.png
 *   native  <n>   a full-screen virtual display                          -> dump_native_full.png
 *                 + an n x n centre crop                                 -> dump_crop_<n>.png
 *   projection <n> n x n display, then setDisplayProjection to the centre  -> dump_proj_<n>.png
 *
 * Also logs which hidden SurfaceControl / VirtualDisplay members this build
 * exposes, because the answer decides whether a centre crop can be done with
 * small buffers or has to be done by cropping a full-resolution frame.
 */
object CaptureDump {

    private const val TAG = "aimbot_dump"
    private const val VIRTUAL_DISPLAY_FLAG_PUBLIC = 0x1

    @JvmStatic
    fun main(args: Array<String>) {
        val mode = args.getOrNull(0) ?: "current"
        val size = args.getOrNull(1)?.toIntOrNull() ?: 640

        probeApis()

        val dm = makeDisplayManager() ?: run {
            Log.e(TAG, "no DisplayManager")
            return
        }
        val def = dm.getDisplay(0)
        if (def != null) {
            val p = Point()
            @Suppress("DEPRECATION")
            def.getRealSize(p)
            Log.i(TAG, "default display real=${p.x}x${p.y} rot=${def.rotation} " +
                    "mode=${def.mode.physicalWidth}x${def.mode.physicalHeight} " +
                    "density=${def.mode.physicalWidth}")
        }

        when (mode) {
            "current" -> current(dm, size)
            "native" -> native(dm, size)
            "projection" -> projection(dm, size)
            else -> Log.e(TAG, "unknown mode: $mode")
        }
        Log.i(TAG, "done")
    }

    // ── The three experiments ────────────────────────────────────────────────

    /** Exactly what ships today: an n x n virtual display fed by the mirror. */
    private fun current(dm: DisplayManager, n: Int) {
        val reader = ImageReader.newInstance(n, n, PixelFormat.RGBA_8888, 2)
        val vd = dm.createVirtualDisplay(
            "aimbot-dump-cur", n, n, density(), reader.surface, VIRTUAL_DISPLAY_FLAG_PUBLIC
        )
        Thread.sleep(700)  // let SurfaceFlinger produce something to look at
        val img = acquire(reader, 6000)
        if (img == null) Log.e(TAG, "current: no frame")
        else {
            save(toBitmap(img), "/data/local/tmp/dump_current_$n.png")
            img.close()
        }
        vd.release()
        reader.close()
    }

    /** One n x n surface, but the whole screen mirrored into it 1:1. */
    private fun native(dm: DisplayManager, n: Int) {
        val def = dm.getDisplay(0)
        val p = Point()
        @Suppress("DEPRECATION")
        def.getRealSize(p)
        val w = p.x
        val h = p.y
        Log.i(TAG, "native: building ${w}x$h virtual display")

        val reader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2)
        val vd = dm.createVirtualDisplay(
            "aimbot-dump-nat", w, h, density(), reader.surface, VIRTUAL_DISPLAY_FLAG_PUBLIC
        )
        Thread.sleep(700)
        val img = acquire(reader, 6000)
        if (img == null) {
            Log.e(TAG, "native: no frame")
        } else {
            val full = toBitmap(img)
            Log.i(TAG, "native frame ${full.width}x${full.height}")
            img.close()
            saveShrunk(full, "/data/local/tmp/dump_native_full.png", 900)

            val cx = (full.width - n) / 2
            val cy = (full.height - n) / 2
            if (cx >= 0 && cy >= 0) {
                val crop = Bitmap.createBitmap(full, cx, cy, n, n)
                save(crop, "/data/local/tmp/dump_crop_$n.png")
            } else {
                Log.w(TAG, "native: screen smaller than crop")
            }
        }
        vd.release()
        reader.close()
    }

    /**
     * The efficient route: keep the surface n x n, but aim the display's
     * projection at the centre of the source so it samples instead of scales.
     */
    private fun projection(dm: DisplayManager, n: Int) {
        val def = dm.getDisplay(0)
        val p = Point()
        @Suppress("DEPRECATION")
        def.getRealSize(p)
        val srcW = p.x
        val srcH = p.y
        val cx = (srcW - n) / 2
        val cy = (srcH - n) / 2
        Log.i(TAG, "projection: src=${srcW}x$srcH centre=($cx,$cy) side=$n")

        val reader = ImageReader.newInstance(n, n, PixelFormat.RGBA_8888, 2)
        val vd = dm.createVirtualDisplay(
            "aimbot-dump-prj", n, n, density(), reader.surface, VIRTUAL_DISPLAY_FLAG_PUBLIC
        )

        val token = readDisplayToken(vd)
        Log.i(TAG, "projection: token=$token")
        if (token != null) {
            try {
                val txnCls = Class.forName("android.view.SurfaceControl\$Transaction")
                val txn = txnCls.getDeclaredConstructor().newInstance()
                val m = txnCls.getMethod(
                    "setDisplayProjection", IBinder::class.java, Int::class.javaPrimitiveType,
                    Rect::class.java, Rect::class.java
                )
                m.invoke(txn, token, 0,
                    Rect(cx, cy, cx + n, cy + n), Rect(0, 0, n, n))
                txnCls.getMethod("apply").invoke(txn)
                Log.i(TAG, "projection: setDisplayProjection OK")
            } catch (t: Throwable) {
                Log.w(TAG, "projection: setDisplayProjection failed", t)
            }
        }

        Thread.sleep(700)
        val img = acquire(reader, 6000)
        if (img == null) Log.e(TAG, "projection: no frame")
        else {
            save(toBitmap(img), "/data/local/tmp/dump_proj_$n.png")
            img.close()
        }
        vd.release()
        reader.close()
    }

    // ── Reflection probes ────────────────────────────────────────────────────

    private fun probeApis() {
        try {
            val c = Class.forName("android.view.SurfaceControl\$Transaction")
            c.methods.filter {
                val n = it.name.lowercase()
                n.contains("projection") || n.contains("layerstack") || n.contains("displaysurface")
            }.forEach { Log.i(TAG, "txn: ${it.name}(${it.parameterTypes.joinToString { p -> p.simpleName }})") }
        } catch (t: Throwable) {
            Log.w(TAG, "txn probe failed", t)
        }
        try {
            val c = Class.forName("android.hardware.display.VirtualDisplay")
            c.declaredMethods.filter { it.name.contains("Token", true) }
                .forEach { Log.i(TAG, "vd.method: ${it.name} -> ${it.returnType.simpleName}") }
            c.declaredFields.filter { it.name.contains("oken", true) }
                .forEach { Log.i(TAG, "vd.field: ${it.name} : ${it.type.simpleName}") }
        } catch (t: Throwable) {
            Log.w(TAG, "vd probe failed", t)
        }
    }

    private fun readDisplayToken(vd: Any): IBinder? {
        try {
            val m = vd.javaClass.getMethod("getToken")
            m.isAccessible = true
            (m.invoke(vd) as? IBinder)?.let { return it }
        } catch (ignored: Throwable) {
        }
        for (name in listOf("mToken", "mDisplayToken")) {
            try {
                val f = vd.javaClass.getDeclaredField(name)
                f.isAccessible = true
                (f.get(vd) as? IBinder)?.let { return it }
            } catch (ignored: Throwable) {
            }
        }
        return null
    }

    // ── Frame plumbing ───────────────────────────────────────────────────────

    private fun acquire(reader: ImageReader, timeoutMs: Long): Image? {
        val ref = AtomicReference<Image?>()
        val t = Thread {
            try {
                ref.set(reader.acquireLatestImage())
            } catch (e: Throwable) {
                Log.e(TAG, "acquireLatestImage failed", e)
            }
        }
        t.isDaemon = true
        t.start()
        t.join(timeoutMs)
        if (t.isAlive) {
            Log.e(TAG, "acquire timed out after ${timeoutMs}ms")
            return null
        }
        return ref.get()
    }

    /** RGBA_8888 image -> ARGB Bitmap, unpacking a padded row stride. */
    private fun toBitmap(image: Image): Bitmap {
        val w = image.width
        val h = image.height
        val plane = image.planes[0]
        val buf = plane.buffer
        val rowStride = plane.rowStride
        val rowBytes = w * 4
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        if (rowStride == rowBytes) {
            buf.rewind()
            bmp.copyPixelsFromBuffer(buf)
            return bmp
        }
        val pixels = IntArray(w * h)
        val row = ByteArray(rowStride)
        buf.rewind()
        for (y in 0 until h) {
            val n = minOf(rowStride, buf.remaining())
            if (n <= 0) break
            buf.get(row, 0, n)
            val base = y * w
            for (x in 0 until w) {
                val o = x * 4
                pixels[base + x] =
                    ((row[o + 3].toInt() and 0xff) shl 24) or
                    ((row[o].toInt() and 0xff) shl 16) or
                    ((row[o + 1].toInt() and 0xff) shl 8) or
                    (row[o + 2].toInt() and 0xff)
            }
        }
        bmp.setPixels(pixels, 0, w, 0, 0, w, h)
        return bmp
    }

    private fun save(bmp: Bitmap, path: String) {
        FileOutputStream(path).use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
        Log.i(TAG, "saved $path (${bmp.width}x${bmp.height})")
    }

    private fun saveShrunk(bmp: Bitmap, path: String, maxSide: Int) {
        val scale = maxSide.toFloat() / maxOf(bmp.width, bmp.height)
        val out = if (scale < 1f) {
            Bitmap.createScaledBitmap(
                bmp, (bmp.width * scale).toInt(), (bmp.height * scale).toInt(), true
            )
        } else bmp
        save(out, path)
    }

    // ── Context plumbing (mirrors ScreenCapture) ─────────────────────────────

    private fun systemContext(): Context? = try {
        val at = Class.forName("android.app.ActivityThread")
        val thread = (try {
            at.getMethod("currentActivityThread").invoke(null)
        } catch (t: Throwable) {
            null
        }) ?: run {
            try {
                Looper.prepareMainLooper()
            } catch (ignored: Throwable) {
            }
            at.getMethod("systemMain").invoke(null)
        }
        at.getMethod("getSystemContext").invoke(thread) as? Context
    } catch (t: Throwable) {
        Log.w(TAG, "no system context", t)
        null
    }

    private fun makeDisplayManager(): DisplayManager? {
        val ctx = object : ContextWrapper(systemContext()) {
            override fun getPackageName(): String = "com.android.shell"
            override fun getOpPackageName(): String = "com.android.shell"
            override fun getApplicationContext(): Context = this
            override fun getResources(): Resources = Resources.getSystem()
        }
        return try {
            val ctor = DisplayManager::class.java.getDeclaredConstructor(Context::class.java)
            ctor.isAccessible = true
            ctor.newInstance(ctx)
        } catch (t: Throwable) {
            Log.e(TAG, "DisplayManager unavailable", t)
            null
        }
    }

    private fun density(): Int = try {
        Resources.getSystem().displayMetrics.densityDpi
    } catch (t: Throwable) {
        160
    }
}
