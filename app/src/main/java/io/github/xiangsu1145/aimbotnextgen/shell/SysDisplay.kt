package io.github.xiangsu1145.aimbotnextgen.shell

import android.content.Context
import android.content.ContextWrapper
import android.content.res.Resources
import android.graphics.Point
import android.hardware.display.DisplayManager
import android.os.IBinder
import android.os.Looper
import android.util.Log

/**
 * The panel's size and rotation, read by the daemon for itself.
 *
 * ── Why the daemon reads this, and not the app ───────────────────────────────
 *
 * The app used to be the only one who could: it has a Context, and the daemon is
 * a bare `app_process`. So the app pushed `SET_RESOLUTION` on connect and on
 * every rotation, and the daemon trusted it. That trust is misplaced, because an
 * app is not always in a position to answer:
 *
 *   * a process with no visible window is not guaranteed to have its
 *     configuration updated when the panel rotates — and once Android freezes a
 *     cached process, nothing in it runs at all, queued callbacks included;
 *   * `DisplayManagerGlobal` answers `getDisplay` from a per-process cache, so a
 *     process that missed the event keeps reporting the geometry it last saw.
 *
 * The symptom is a menu built for the *previous* orientation and then never
 * corrected, which reads as "the floating panel is off-centre, and opening the
 * app fixes it". The daemon has no such problem: it is a long-lived shell
 * process, never frozen, and it is the one that has to map coordinates and size
 * the layer anyway.
 *
 * ── Why the display service, and not just `DisplayManager` ───────────────────
 *
 * `DisplayManager` is the obvious API and it is the *fallback*, but it resolves
 * through the same per-process cache described above, which is invalidated only
 * when the display-event callback is delivered — and delivery needs binder
 * threads this process may never have started. A daemon polling that can be
 * handed the geometry it first saw, forever.
 *
 * So the primary path asks the display service directly. That is one IPC per
 * read with no cache in between, which is exactly the guarantee a value that
 * must follow a rotation needs. It is a hidden API, reached reflectively; if a
 * future build moves it, [fromDisplayManager] takes over and the log says so.
 */
internal object SysDisplay {

    private const val TAG = "aimbot_shell"

    /** Building a `DisplayManager` is all reflection; do it once. */
    @Volatile private var cachedDm: DisplayManager? = null

    // ── Reflection cache ───────────────────────────────────────────────────
    //
    // Every `getMethod` / `getField` hands back a freshly allocated copy of the
    // class's whole method or field table, and every `invoke` allocates the
    // boxed argument array. That is tolerable once and wasteful at poll rate:
    // this read runs from the geometry watcher (2 Hz) and the capture
    // supervisor (2.5 Hz) for the daemon's entire life, so on a long session
    // the lookup alone was feeding the GC tens of thousands of throwaway
    // arrays. Resolved once, then reused.

    private class DisplayServiceHandle(
        val service: Any,
        val getDisplayInfo: java.lang.reflect.Method,
        val logicalWidth: java.lang.reflect.Field,
        val logicalHeight: java.lang.reflect.Field,
        val rotation: java.lang.reflect.Field
    )

    @Volatile private var cachedService: DisplayServiceHandle? = null

    /** Default display as `(width, height, rotation)`, or null if unreadable. */
    fun defaultGeometry(): Triple<Int, Int, Int>? =
        fromDisplayService() ?: fromDisplayManager()

    fun displayManager(): DisplayManager? {
        cachedDm?.let { return it }
        return makeDisplayManager()?.also { cachedDm = it }
    }

    /** System density, for anything that has to turn pixels into dp. */
    fun density(): Int = try {
        Resources.getSystem().displayMetrics.densityDpi
    } catch (t: Throwable) {
        160
    }

    // ── Primary: one IPC, no cache ───────────────────────────────────────────

    /**
     * Asks `IDisplayManager.getDisplayInfo(0)` straight.
     *
     * `logicalWidth`/`logicalHeight` rather than the `app*` pair: the former is
     * the whole panel in the current rotation, which is the space the layer and
     * the touch mapping both work in. The `app*` pair excludes persistent decor
     * (this device's navigation bar is 42 px), which would leave the menu a
     * little short of centred.
     */
    private fun fromDisplayService(): Triple<Int, Int, Int>? {
        val h = cachedService ?: resolveDisplayService()?.also { cachedService = it }
        if (h == null) {
            Log.w(TAG, "display service unavailable")
            return null
        }
        return try {
            val info = h.getDisplayInfo.invoke(h.service, 0) ?: return null
            val w = h.logicalWidth.getInt(info)
            val ht = h.logicalHeight.getInt(info)
            val rot = h.rotation.getInt(info)
            if (w > 0 && ht > 0) Triple(w, ht, rot) else null
        } catch (t: Throwable) {
            // A dead binder or a moved field: drop the cache so the next poll
            // resolves from scratch rather than failing forever.
            Log.w(TAG, "display service read failed (${t.javaClass.simpleName}: ${t.message})")
            cachedService = null
            null
        }
    }

    /** Resolves the display service and its accessors once. */
    private fun resolveDisplayService(): DisplayServiceHandle? {
        return try {
            val sm = Class.forName("android.os.ServiceManager")
            val binder = sm.getMethod("getService", String::class.java)
                .invoke(null, "display") as? IBinder ?: return null

            val stub = Class.forName("android.hardware.display.IDisplayManager\$Stub")
            val service = stub.getMethod("asInterface", IBinder::class.java)
                .invoke(null, binder) ?: return null

            // Resolved against the concrete proxy class: `getDisplayInfo` lives
            // there, and looking it up on the Stub would miss it.
            val cls = service.javaClass
            val getDisplayInfo = cls.getMethod("getDisplayInfo", Int::class.javaPrimitiveType)

            // The info object's own class, so the fields are read from the type
            // that actually declares them.
            val probe = getDisplayInfo.invoke(service, 0) ?: return null
            val infoCls = probe.javaClass
            DisplayServiceHandle(
                service, getDisplayInfo,
                infoCls.getField("logicalWidth"),
                infoCls.getField("logicalHeight"),
                infoCls.getField("rotation")
            )
        } catch (t: Throwable) {
            Log.w(TAG, "display service resolve failed (${t.javaClass.simpleName}: ${t.message})")
            null
        }
    }

    // ── Fallback: the public API ─────────────────────────────────────────────

    private fun fromDisplayManager(): Triple<Int, Int, Int>? {
        return try {
            val dm = displayManager() ?: return null
            val display = dm.getDisplay(0) ?: return null
            val p = Point()
            @Suppress("DEPRECATION")
            display.getRealSize(p)
            if (p.x > 0 && p.y > 0) Triple(p.x, p.y, display.rotation) else null
        } catch (t: Throwable) {
            Log.w(TAG, "DisplayManager geometry read failed", t)
            null
        }
    }

    // ── Context plumbing ─────────────────────────────────────────────────────

    /**
     * The framework's own system context.
     *
     * `app_process` never went through the normal ActivityThread attach, so there
     * is no Context to be had here — but ActivityThread keeps one for the system,
     * and it is reachable. Tried without `systemMain()` first: if a thread
     * already exists that is both cheaper and what the platform expects.
     */
    private fun systemContext(): Context? {
        return try {
            val at = Class.forName("android.app.ActivityThread")
            val thread = (try {
                at.getMethod("currentActivityThread").invoke(null)
            } catch (t: Throwable) {
                null
            }) ?: run {
                // systemMain() attaches a thread, and attach() wants a looper.
                try {
                    Looper.prepareMainLooper()
                } catch (ignored: Throwable) {
                    // Already prepared — fine.
                }
                at.getMethod("systemMain").invoke(null)
            }
            at.getMethod("getSystemContext").invoke(thread) as? Context
        } catch (t: Throwable) {
            Log.w(TAG, "no system context (${t.javaClass.simpleName}: ${t.message})")
            null
        }
    }

    /**
     * A Context that [DisplayManager] will accept.
     *
     * It stores the context and hands it back when a size or density has to be
     * resolved, so all this needs to survive is `getResources()` — which is why
     * that one is overridden rather than left to a base that may be null.
     */
    private fun makeDisplayManager(): DisplayManager? {
        val base = systemContext()
        val ctx = object : ContextWrapper(base) {
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
}
