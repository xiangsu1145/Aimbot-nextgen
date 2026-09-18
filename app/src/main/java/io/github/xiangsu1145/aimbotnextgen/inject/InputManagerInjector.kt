package io.github.xiangsu1145.aimbotnextgen.inject

import android.content.Context
import android.os.Build
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.InputDevice
import android.view.InputEvent
import android.view.MotionEvent
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Method
import kotlin.math.sin

/**
 * Touch injection through `InputManager.injectInputEvent()`.
 *
 * The other backend ([io.github.xiangsu1145.aimbotnextgen.shell.ShellNative]'s
 * uinput half) puts touches in as frames on a virtual touchscreen, which means
 * they travel through the input *device* layer — and on some devices they are
 * accepted there and then never delivered. This one hands a finished
 * [MotionEvent] straight to InputDispatcher: no EventHub, no InputReader, no
 * device to be misclassified.
 *
 * ## Two routes to the service, AIDL first
 *
 * `android.hardware.input.InputManager` is the public-ish wrapper, and going
 * through it works — but it is one more @hide class on the path and its
 * two-argument `injectInputEvent` is documented as compatibility-only. So the
 * binder is taken directly instead, the way the App this was modelled on does
 * it: `ServiceManager.getService("input")` → `IInputManager$Stub.asInterface`.
 * The wrapper is still tried afterwards, because a build that has the binder
 * but not the interface (or vice versa) is not worth losing the backend over.
 *
 * ## What goes into a MotionEvent
 *
 * The event has to be more than well-formed to survive on OEM builds:
 *
 *  * `deviceId` is the **real touchscreen's** id, found by scanning
 *    [InputDevice.getDeviceIds] for one whose sources include
 *    [InputDevice.SOURCE_TOUCHSCREEN]. Passing `0` produces an event that is
 *    valid but anonymous, and anonymous injects are the ones OEM input stacks
 *    are free to filter.
 *  * `pressure` is not a constant. It is a slow, smooth wander in the
 *    0.55..0.92 range — a real capacitive panel never reports a flat 1.0, and
 *    a stream of exactly-1.0 pressures is the single most machine-looking
 *    feature a synthesized touch can have.
 *  * `touchMajor` / `touchMinor` / `size` are derived from that pressure, so
 *    the contact patch has a plausible area instead of 0.
 *  * On Android 16+ (`SDK_INT >= 36`) an `ACTION_CANCEL` is injected before
 *    the first `ACTION_DOWN` of a gesture, to clear any stale gesture state
 *    InputDispatcher is still holding. Without it the DOWN can be refused.
 *
 * ## This class is the only writer
 *
 * Nothing else builds a MotionEvent, and nothing else decides what changed. The
 * native side publishes an *absolute* description of the world — "these fingers
 * should be down, here" — and this class diffs it against what it last told
 * Android, turning each change into exactly one action. That split exists
 * because the two halves need different knowledge: native knows which slots
 * exist (mirror slots, the aim's slot, the trigger's slot) and where they are;
 * this class knows what Android has been told, and therefore whether the change
 * is DOWN or POINTER_DOWN, which pointer index a POINTER_UP must name, and when
 * the gesture's downTime started.
 *
 * Getting that wrong is exactly how the previous implementation produced two
 * pointers that jumped between two positions: it handed out ids randomly from a
 * three-element set, so a finger that was already down could be given the same
 * id as a new one, and Android — which can only have one pointer per id —
 * merged them. Hence the rules below, which are not style choices:
 *
 *  * a pointer's `id` is a local slot number, assigned when it goes down and
 *    never reused while it is down;
 *  * every event carries **all** pointers currently down, in insertion order;
 *  * one commit produces one action, derived from the difference;
 *  * `downTime` is set by the first ACTION_DOWN and stays put for the whole
 *    gesture, across pointers coming and going;
 *  * `eventTime` is *now* — never a future timestamp;
 *  * the object returned by `obtain()` is always `recycle()`d.
 *
 * ## Why it logs so much
 *
 * A refused injection leaves **no** trace on this side: `injectInputEvent`
 * returns false, and the reason is written by InputDispatcher into the *system*
 * log, which a daemon under the shell UID usually cannot read. So everything
 * that could possibly explain a failure is written here instead — which route
 * was used, which method was resolved, what `deviceId` was found, the running
 * success/failure counts, and the last refusal. Without this the only field
 * report is "it does not work", which is where the previous version left us.
 */
object InputManagerInjector {

    private const val TAG = "AimbotInject"

    /** Android's own ceiling on pointers in one gesture. */
    private const val MAX_POINTERS = 10

    /** scrcpy's choice: hand the event over and return, do not block the caller. */
    private const val INJECT_INPUT_EVENT_MODE_ASYNC = 0

    /** `Process.INVALID_UID` — "do not restrict this to any one target window". */
    private const val INVALID_UID = -1

    /**
     * Android 16. From this release InputDispatcher validates the injected
     * gesture's state before accepting a DOWN, so a stale cancel has to go out
     * first. Measured on a real device by the App this is modelled on; it is
     * a no-op on everything older, which is why it is version-gated rather
     * than unconditional.
     */
    private const val SDK_ANDROID_16 = 36

    /**
     * How often [pushFrame] writes a status line.
     *
     * Deliberately short. `frameCount` only advances when a finger actually
     * moves, so a real test session can be far shorter than a wall-clock
     * estimate suggests — at 1000 frames a 20-second trial produced not one
     * line, which is how a diagnostic ends up saying nothing. ~2 s at 100 Hz.
     */
    private const val STATS_EVERY_FRAMES = 200

    /** One id fixed at press time; `localId` is what goes into the MotionEvent. */
    private class Finger(val externalId: Int, val localId: Int) {
        var x = 0f
        var y = 0f

        /**
         * Carried across frames so the value can wander smoothly instead of
         * jumping per event — see [humanizedPressure].
         */
        var pressure = 0.72f
    }

    private val lock = Any()

    /** The binder proxy (`IInputManager`) or the wrapper instance. */
    private var service: Any? = null
    private var injectMethod: Method? = null

    /** How the handle was obtained, for the log. */
    private var route = "none"

    /** `Class#getName` of whatever [service] turned out to be, for the log. */
    private var serviceClass = "none"

    /** Human-readable signature of [injectMethod], for the log. */
    private var methodSignature = "none"

    private var ready = false
    private var lastError = ""

    /**
     * The real touchscreen's Android device id, or 0 when it could not be
     * determined. Not re-scanned once found: devices do not change mid-session,
     * and the scan costs a binder round trip plus an [InputDevice] per id.
     */
    private var touchDeviceId = 0
    private var touchDeviceName = "unknown"

    /** Pointers Android currently believes are down, in the order they went down. */
    private val fingers = ArrayList<Finger>(MAX_POINTERS)
    private var downTime = 0L

    private var lastRefusalLogMs = 0L
    private var lastPermissionLogMs = 0L

    /** Running counts, reported in the log and by [stats]. */
    private var okCount = 0
    private var failCount = 0
    private var cancelOkCount = 0
    private var cancelFailCount = 0

    /**
     * Accepted events split by action, so the periodic line answers "is a
     * gesture actually flowing" without a second round trip: a DOWN with no
     * following MOVE is a finger that went down and never moved, which is a
     * different report from a stream that never started.
     */
    private var downOk = 0
    private var upOk = 0
    private var moveOk = 0

    /** Frames pushed, used only to pace the periodic status line. */
    private var frameCount = 0

    /**
     * The desired set for the frame being processed, in flat arrays. Reused
     * rather than rebuilt: at 100+ frames a second, one small list per frame is
     * one small list too many.
     */
    private val wantIds = IntArray(MAX_POINTERS)
    private val wantXs = FloatArray(MAX_POINTERS)
    private val wantYs = FloatArray(MAX_POINTERS)

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /**
     * Obtains the input service handle and resolves `injectInputEvent`.
     *
     * Reflective on purpose. The classes involved are @hide, but this process is
     * the daemon — `app_process` under the shell UID — and it already reaches
     * @hide APIs the same way for its SurfaceFlinger layer (ShellLayerHost),
     * which works. No hidden-API exemption is needed, and none is used.
     *
     * Idempotent. Returns false when the handle could not be obtained, which the
     * caller must report rather than paper over.
     */
    @JvmStatic
    fun init(): Boolean = synchronized(lock) {
        if (ready) return true

        return try {
            val handle = obtainService()
            if (handle == null) {
                fail("no input service — neither the IInputManager binder nor " +
                        "InputManager.getInstance() could be reached")
                return false
            }
            val cls = handle.javaClass
            val method = findInjectMethod(cls)
            if (method == null) {
                fail("${cls.name} exposes no usable inject method " +
                        "(${describeInjectCandidates(cls)})")
                return false
            }

            service = handle
            serviceClass = cls.name
            injectMethod = method
            methodSignature = method.name + describeParameters(method)

            detectTouchDevice()
            resetState()
            okCount = 0
            failCount = 0
            cancelOkCount = 0
            cancelFailCount = 0
            ready = true
            lastError = ""

            Log.i(TAG, "InputManager injector ready")
            Log.i(TAG, "  route    : $route")
            Log.i(TAG, "  service  : $serviceClass")
            Log.i(TAG, "  method   : $methodSignature")
            Log.i(TAG, "  deviceId : $touchDeviceId ($touchDeviceName)")
            Log.i(TAG, "  android  : SDK ${Build.VERSION.SDK_INT}" +
                    (if (Build.VERSION.SDK_INT >= SDK_ANDROID_16) " (pre-down cancel enabled)" else ""))
            true
        } catch (t: Throwable) {
            fail("reflect: ${t.javaClass.simpleName}: ${t.message}")
            false
        }
    }

    /**
     * The input service, by the cheapest route that works.
     *
     * The AIDL binder first — it is what the App this is modelled on uses, and
     * it skips both the `InputManager` wrapper class and any need for a Context.
     * The wrapper is the fallback: `getInstance()` is a plain static that needs
     * no Context either, and `getSystemService("input")` is the last resort
     * (it is the one scrcpy uses, and it is last here because it means standing
     * up ActivityThread's main-thread machinery to get a Context nobody else
     * needs).
     */
    private fun obtainService(): Any? {
        val byBinder = try {
            val binder = Class.forName("android.os.ServiceManager")
                .getMethod("getService", String::class.java)
                .invoke(null, "input") as? IBinder
            if (binder == null) {
                Log.w(TAG, "ServiceManager.getService(\"input\") returned null")
                null
            } else {
                binder.isBinderAlive
                val proxy = Class.forName("android.hardware.input.IInputManager\$Stub")
                    .getMethod("asInterface", IBinder::class.java)
                    .invoke(null, binder)
                if (proxy == null) Log.w(TAG, "IInputManager.Stub.asInterface returned null")
                proxy
            }
        } catch (t: Throwable) {
            Log.w(TAG, "IInputManager binder route unavailable: " +
                    "${t.javaClass.simpleName}: ${t.message}")
            null
        }
        if (byBinder != null) {
            route = "IInputManager binder (ServiceManager.getService)"
            return byBinder
        }

        val byStatic = try {
            Class.forName("android.hardware.input.InputManager")
                .getMethod("getInstance")
                .invoke(null)
        } catch (t: Throwable) {
            Log.w(TAG, "InputManager.getInstance() unavailable: ${t.javaClass.simpleName}")
            null
        }
        if (byStatic != null) {
            route = "InputManager.getInstance() wrapper"
            return byStatic
        }

        val byContext = systemContext()?.getSystemService("input")
        if (byContext != null) route = "Context.getSystemService(\"input\") wrapper"
        return byContext
    }

    /**
     * Resolves the inject method, preferring the two-argument form.
     *
     * The preferred signature is `injectInputEvent(InputEvent, int)`. It is not
     * the only one that exists, and it is not guaranteed to keep existing —
     * AOSP's own AIDL annotates it "exists only for compatibility purposes and
     * may be removed in a future release" — so when it is missing the whole
     * method table is scanned instead. A build that ships only the three-argument
     * `injectInputEventToTarget(event, mode, targetUid)` is otherwise perfectly
     * usable, and losing the backend over a signature change would be silly.
     *
     * The scan's acceptance rule is deliberately structural rather than
     * name-based beyond `inject`: first parameter an [InputEvent], every
     * remaining parameter an int/boolean/long (their boxed forms included).
     * Anything else is not something we know how to fill in, so it is skipped
     * rather than called with made-up arguments.
     */
    private fun findInjectMethod(cls: Class<*>): Method? {
        val preferred = try {
            cls.getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
        } catch (t: Throwable) {
            Log.i(TAG, "no two-argument injectInputEvent on ${cls.name} " +
                    "(${t.javaClass.simpleName}) — scanning the method table")
            null
        }
        if (preferred != null) {
            preferred.isAccessible = true
            return preferred
        }

        for (m in cls.methods) {
            if (!m.name.contains("inject", ignoreCase = true)) continue
            if (!supportsInjectMethod(m)) continue
            m.isAccessible = true
            Log.i(TAG, "falling back to ${m.name}${describeParameters(m)} on ${cls.name}")
            return m
        }
        return null
    }

    /** The acceptance rule for the fallback scan. See [findInjectMethod]. */
    private fun supportsInjectMethod(method: Method): Boolean {
        val p = method.parameterTypes
        if (p.isEmpty()) return false
        if (!InputEvent::class.java.isAssignableFrom(p[0])) return false
        for (i in 1 until p.size) {
            if (!isSupportedInjectParameterType(p[i])) return false
        }
        return true
    }

    private fun isSupportedInjectParameterType(type: Class<*>): Boolean =
        type == Int::class.javaPrimitiveType || type == Int::class.javaObjectType ||
                type == Boolean::class.javaPrimitiveType || type == Boolean::class.javaObjectType ||
                type == Long::class.javaPrimitiveType || type == Long::class.javaObjectType

    /**
     * Fills in every argument after the event.
     *
     * `mode = 0` is `INJECT_INPUT_EVENT_MODE_ASYNC`: hand the event over and
     * return, do not wait for the app to consume it. That is what a 100 Hz
     * stream wants — a synchronous inject would make our frame rate a function
     * of the target app's main thread.
     *
     * The third position of a `...ToTarget` method is `targetUid`, and `-1`
     * (`Process.INVALID_UID`) means "no target restriction", which is what is
     * wanted here: the touches are meant for whatever is on screen, not for one
     * particular app. A real uid would make InputDispatcher *fail* the inject
     * when the window under the pointer belongs to somebody else.
     */
    private fun buildInjectArguments(method: Method, event: InputEvent): Array<Any?> {
        val types = method.parameterTypes
        val args = arrayOfNulls<Any?>(types.size)
        args[0] = event
        for (i in 1 until types.size) {
            args[i] = defaultArgumentFor(method, i, types[i])
        }
        return args
    }

    private fun defaultArgumentFor(method: Method, index: Int, type: Class<*>): Any? = when {
        type == Int::class.javaPrimitiveType || type == Int::class.javaObjectType -> {
            if (index == 2 && method.name.contains("ToTarget", ignoreCase = true)) INVALID_UID
            else INJECT_INPUT_EVENT_MODE_ASYNC
        }
        type == Boolean::class.javaPrimitiveType || type == Boolean::class.javaObjectType -> false
        type == Long::class.javaPrimitiveType || type == Long::class.javaObjectType -> 0L
        else -> null
    }

    @JvmStatic
    fun isReady(): Boolean = synchronized(lock) { ready }

    /** Last failure, for the log and the menu. Empty when there has been none. */
    @JvmStatic
    fun lastError(): String = synchronized(lock) { lastError }

    /** One-line summary of how injection is doing. Written to the daemon log. */
    @JvmStatic
    fun stats(): String = synchronized(lock) { statsLocked() }

    private fun statsLocked(): String =
        "im[$route | $methodSignature | dev=$touchDeviceId | " +
                "frames=$frameCount ok=$okCount fail=$failCount | " +
                "down=$downOk up=$upOk move=$moveOk | " +
                "cancel_ok=$cancelOkCount cancel_fail=$cancelFailCount]"

    /** Lifts everything and forgets the gesture. */
    @JvmStatic
    fun releaseAll() {
        synchronized(lock) {
            if (!ready) return
            // An empty desired set is how "nothing should be down" is spelled,
            // and applyDesired() turns it into the ACTION_UP(s) that say so.
            applyDesired(0)
        }
    }

    // ── The frame entry point (called from native) ───────────────────────────

    /**
     * Publishes the complete set of fingers that should be down.
     *
     * `ids` are the native side's finger identities — for a mirrored finger the
     * *panel's* tracking id, not the slot it happens to occupy, so the identity
     * follows the finger when the slots around it change; for the aim and the
     * trigger, their fixed ids. `xs`/`ys` are **screen pixels**; the two
     * backends deliberately keep different coordinate spaces, and this one
     * cannot use panel raw.
     */
    @JvmStatic
    fun pushFrame(ids: IntArray, xs: IntArray, ys: IntArray, n: Int) {
        synchronized(lock) {
            if (!ready) return

            // A status line every few hundred frames, so a device that "does not
            // work" arrives with numbers attached instead of an adjective. At
            // ~100 Hz this is roughly every 10 s.
            if (++frameCount % STATS_EVERY_FRAMES == 0) Log.i(TAG, statsLocked())

            val count = n.coerceIn(0, MAX_POINTERS)
            for (i in 0 until count) {
                wantIds[i] = ids[i]
                wantXs[i] = xs[i].toFloat()
                wantYs[i] = ys[i].toFloat()
            }
            applyDesired(count)
        }
    }

    // ── The state machine ────────────────────────────────────────────────────

    /**
     * Diffs the desired set against what Android was last told, and emits one
     * event per difference.
     *
     * Additions are applied before removals so a hand-off (one finger lifting as
     * another lands) never passes through "no pointers down" in the middle, and
     * an addition is refused — not silently dropped into the same event — when
     * the device is already at MAX_POINTERS.
     */
    private fun applyDesired(n: Int) {
        var guard = 0
        while (guard++ <= MAX_POINTERS * 2 + 4) {
            var addIndex = -1
            for (i in 0 until n) {
                if (indexOfExternal(wantIds[i]) < 0) {
                    addIndex = i
                    break
                }
            }

            if (addIndex >= 0 && fingers.size < MAX_POINTERS) {
                val localId = nextLocalId()
                if (localId < 0) break   // impossible while size < MAX_POINTERS
                val f = Finger(wantIds[addIndex], localId)
                f.x = wantXs[addIndex]
                f.y = wantYs[addIndex]
                fingers.add(f)
                val index = fingers.size - 1
                val action = if (fingers.size == 1) MotionEvent.ACTION_DOWN
                else MotionEvent.ACTION_POINTER_DOWN
                if (!emit(action, index)) {
                    // Refused, so Android never saw this pointer. Take it back
                    // out and let the next frame try again: believing otherwise
                    // is how a stream desynchronises once and never recovers —
                    // the pointer is never announced again, and every later
                    // event that mentions it is rejected as inconsistent.
                    fingers.removeAt(index)
                    break
                }
                continue
            }

            var removeIndex = -1
            for (i in fingers.indices) {
                if (!containsExternal(n, fingers[i].externalId)) {
                    removeIndex = i
                    break
                }
            }

            if (removeIndex >= 0) {
                // The action's pointer index refers to a position in THIS
                // event's pointer array, which still contains the finger being
                // lifted — so the event is built first and the finger removed
                // after. Reversed, POINTER_UP would name the wrong finger
                // whenever it was not the last one.
                if (fingers.size == 1) {
                    if (!emit(MotionEvent.ACTION_UP, 0)) break
                    fingers.removeAt(0)
                    downTime = 0L
                } else {
                    if (!emit(MotionEvent.ACTION_POINTER_UP, removeIndex)) break
                    fingers.removeAt(removeIndex)
                }
                continue
            }
            break
        }

        var moved = false
        for (i in 0 until n) {
            val fi = indexOfExternal(wantIds[i])
            if (fi < 0) continue
            val f = fingers[fi]
            if (f.x != wantXs[i] || f.y != wantYs[i]) {
                f.x = wantXs[i]
                f.y = wantYs[i]
                moved = true
            }
        }
        // A refused MOVE needs no rollback — the coordinates are already where
        // they should be, so the next frame simply re-sends them.
        //
        // MOVEs are emitted only for coordinate changes, not for pressure drift:
        // a stationary finger's pressure is re-randomised in the next event it
        // appears in, and emitting an event per pressure tick would double the
        // stream's event rate for something no consumer reads.
        if (moved && fingers.isNotEmpty()) emit(MotionEvent.ACTION_MOVE, 0)
    }

    /**
     * Builds and injects one event for the current [fingers], then recycles it.
     *
     * Returns whether the event landed. A `false` is information, not a hiccup:
     * the caller un-does whatever it was about to believe, so a refused event
     * costs one frame instead of desynchronising the stream for good.
     */
    private fun emit(action: Int, pointerIndex: Int): Boolean {
        val count = fingers.size
        if (count == 0) return true

        // Two pointers may never share an id — InputDispatcher validates this
        // and rejects the whole event ("duplicate pointer id"), silently, if
        // they do. The ids come from a live-list search, so this cannot happen;
        // it is checked anyway because the failure mode is a frozen pointer
        // that leaves no trace anywhere.
        for (i in 0 until count) {
            for (j in i + 1 until count) {
                if (fingers[i].localId == fingers[j].localId) {
                    Log.e(TAG, "refusing to inject: two pointers share local id " +
                            "${fingers[i].localId} — this is a bug in this class")
                    return false
                }
            }
        }

        val now = SystemClock.uptimeMillis()
        if (action == MotionEvent.ACTION_DOWN) downTime = now
        if (downTime == 0L) downTime = now

        val props = Array(count) { MotionEvent.PointerProperties() }
        val coords = Array(count) { MotionEvent.PointerCoords() }
        for (i in 0 until count) {
            val f = fingers[i]
            props[i].id = f.localId
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER
            applyTouchShape(coords[i], f.x, f.y, humanizedPressure(f))
        }

        val fullAction = when (action) {
            MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP ->
                action or (pointerIndex shl MotionEvent.ACTION_POINTER_INDEX_SHIFT)
            else -> action
        }

        // Android 16 refuses to start a gesture while it still believes one is
        // in progress, so a bare DOWN after an interrupted stream is dropped.
        // Clearing with a CANCEL first costs one extra event per gesture.
        if (fullAction == MotionEvent.ACTION_DOWN && !cancelBeforeDown(props, coords, count)) {
            // Deliberately not aborting: the CANCEL is a best-effort cleanup,
            // and on builds that do not need it a refusal is meaningless. The
            // DOWN is tried either way; if it is refused the caller rolls back.
        }

        val event = MotionEvent.obtain(
            downTime,
            now,
            fullAction,
            count,
            props,
            coords,
            0,                      // metaState
            0,                      // buttonState
            1f,                     // xPrecision
            1f,                     // yPrecision
            touchDeviceId,          // deviceId — the real panel, never 0 if it is known
            0,                      // edgeFlags
            InputDevice.SOURCE_TOUCHSCREEN,
            0                       // flags
        )
        // The source is already set by obtain(); setting it again is what the
        // reference implementation does, and it is free.
        event.source = InputDevice.SOURCE_TOUCHSCREEN
        return try {
            invokeInject(event)
        } catch (t: Throwable) {
            // MotionEvent.obtain() throws IllegalArgumentException for a
            // malformed event. Nothing has been sent, so the caller must not
            // commit the change this event was describing.
            Log.e(TAG, "could not build a MotionEvent (action=$fullAction, " +
                    "pointers=$count): ${t.javaClass.simpleName}: ${t.message}")
            lastError = "obtain: ${t.message}"
            false
        } finally {
            // obtain() hands out pooled objects; not recycling leaks native
            // memory once per frame.
            event.recycle()
        }
    }

    /**
     * Injects the `ACTION_CANCEL` that has to precede a gesture on Android 16+.
     *
     * It carries the pointers about to go down, at their real coordinates, with
     * the same `deviceId` — an "anonymous" cancel with no pointer would itself
     * be the kind of event the platform is entitled to ignore.
     *
     * @return whether it was accepted. A `false` is logged and does not stop
     *   the DOWN: this is cleanup, not a precondition we can verify.
     */
    private fun cancelBeforeDown(
        props: Array<MotionEvent.PointerProperties>,
        coords: Array<MotionEvent.PointerCoords>,
        count: Int
    ): Boolean {
        if (Build.VERSION.SDK_INT < SDK_ANDROID_16) return true
        if (count <= 0) return true

        val now = SystemClock.uptimeMillis()
        val cancel = try {
            MotionEvent.obtain(
                now, now,
                MotionEvent.ACTION_CANCEL,
                count, props, coords,
                0, 0, 1f, 1f,
                touchDeviceId,
                0, InputDevice.SOURCE_TOUCHSCREEN, 0
            )
        } catch (t: Throwable) {
            Log.w(TAG, "could not build the pre-down CANCEL: " +
                    "${t.javaClass.simpleName}: ${t.message}")
            return false
        }
        cancel.source = InputDevice.SOURCE_TOUCHSCREEN
        return try {
            val ok = invokeInject(cancel)
            if (ok) {
                ++cancelOkCount
            } else {
                ++cancelFailCount
                Log.w(TAG, "pre-down CANCEL refused (pointers=$count device=$touchDeviceId) — " +
                        "a stale gesture may keep the following DOWN from being accepted")
            }
            ok
        } finally {
            cancel.recycle()
        }
    }

    /**
     * Calls the resolved method with the right arguments and decides whether the
     * platform took the event.
     *
     * The return value is read leniently on purpose. Through the `InputManager`
     * wrapper it is always a boolean, but through an AIDL proxy it is whatever
     * the other side wrote back, and a build that answers with `null` or a
     * non-boolean has not necessarily refused anything. Only an explicit
     * `false` / `0` counts as a refusal.
     */
    private fun invokeInject(event: MotionEvent): Boolean {
        val target = service ?: return false
        val method = injectMethod ?: return false
        return try {
            val args = buildInjectArguments(method, event)
            val result = method.invoke(target, *args)
            val accepted = when (result) {
                is Boolean -> result
                is Number -> result.toInt() != 0
                else -> true
            }
            if (accepted) {
                ++okCount
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                        ++downOk
                        if (downOk == 1) {
                            // The coordinates are printed because "accepted but
                            // nothing moves" and "accepted, but the touch lands
                            // at the wrong place" are the same symptom from the
                            // outside and different bugs inside. Screen pixels
                            // for a 1280x2772 panel are three-digit numbers; a
                            // five-digit pair here means panel-raw leaked in.
                            Log.i(TAG, "first DOWN accepted: ${event.pointerCount} pointer(s) " +
                                    "at (${event.getX(0).toInt()}, ${event.getY(0).toInt()}) " +
                                    "device=$touchDeviceId — if the touch lands elsewhere on " +
                                    "screen, these coordinates are the wrong space")
                        }
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> ++upOk
                    MotionEvent.ACTION_MOVE -> ++moveOk
                    else -> Unit
                }
                if (okCount == 1) {
                    Log.i(TAG, "first injection accepted (action=${event.actionMasked}, " +
                            "pointers=${event.pointerCount}, device=$touchDeviceId) " +
                            "via $methodSignature")
                }
                true
            } else {
                ++failCount
                reportRefusal("refused action=${event.actionMasked} " +
                        "pointers=${event.pointerCount} device=$touchDeviceId")
                false
            }
        } catch (t: Throwable) {
            ++failCount
            val cause = if (t is InvocationTargetException) t.cause ?: t else t
            if (cause is SecurityException && cause.message?.contains("INJECT_EVENTS") == true) {
                reportPermissionProblem(cause.message ?: "")
            } else {
                Log.e(TAG, "injectInputEvent threw (total failures=$failCount) " +
                        "action=${event.actionMasked} backends=$methodSignature", cause)
                lastError = "${cause.javaClass.simpleName}: ${cause.message}"
            }
            false
        }
    }

    // ── The event's "shape" ──────────────────────────────────────────────────

    /**
     * Fills a pointer's coordinates and contact-patch fields.
     *
     * A real panel reports an area that grows with pressure, and a `size` that
     * is small but never zero. Leaving them at 0 (which is what the previous
     * version did) produces a contact with no area at all — valid to
     * InputDispatcher, but not to anything downstream that looks at the shape
     * of a touch.
     *
     * The constants are the reference implementation's, and the ratio between
     * `touchMajor` and `touchMinor` is what makes the patch elliptical the way
     * a finger tip is.
     *
     * Written as one function over a fresh `PointerCoords` on purpose: the
     * reference clears the struct first, which would wipe the coordinates if
     * they were set by the caller beforehand.
     */
    private fun applyTouchShape(
        coords: MotionEvent.PointerCoords,
        x: Float,
        y: Float,
        pressure: Float
    ) {
        coords.x = x
        coords.y = y
        coords.pressure = pressure
        val c = pressure.coerceIn(0.1f, 1f)
        val major = 35f * c + 20f
        coords.touchMajor = major
        coords.touchMinor = (0.13f * c + 0.72f) * major
        coords.size = c * 0.33f + 0.05f
    }

    /**
     * A pressure that wanders instead of sitting still.
     *
     * Two slow sinusoids — one keyed to where the finger is, one to when it is —
     * give a value in 0.60..0.90 that never repeats exactly. That range matters:
     * a real finger's reported pressure is neither at the sensor's ceiling nor
     * constant, and a stream of exactly-1.0 pressures is the easiest possible
     * tell that a touch was synthesized.
     *
     * Smoothed towards its target rather than assigned directly, because the
     * target moves slower than the frame rate and a per-frame jump would be its
     * own artefact.
     *
     * The caller must hold [lock] — [Finger.pressure] is both read and written.
     */
    private fun humanizedPressure(f: Finger): Float {
        val now = SystemClock.uptimeMillis().toFloat()
        val phase = f.localId * 0.731f
        val spatial = sin(0.0017f * f.x + 0.0011f * f.y + phase)
        val temporal = sin(0.0009f * now + phase * 1.37f)
        val wave = ((spatial * 0.6f + temporal * 0.4f) + 1f) * 0.5f   // 0..1
        val target = (0.60f + wave * 0.30f).coerceIn(0.55f, 0.92f)
        f.pressure += (target - f.pressure) * 0.25f
        return f.pressure.coerceIn(0.55f, 0.92f)
    }

    /**
     * Finds the real touchscreen's Android device id.
     *
     * `MotionEvent`'s `deviceId` is what ties an event to a device in
     * InputDispatcher's view of the world. `0` is accepted — it is what scrcpy
     * sends — but it names *no device*, and an event that comes from nothing is
     * exactly what an OEM's injected-input filter is looking for. Reporting the
     * panel that is actually on the phone makes the event indistinguishable from
     * a real one at that layer.
     *
     * Best effort: on a build where the device list is not reachable from this
     * process the answer stays 0, which is the old behaviour and still works
     * everywhere the old behaviour worked.
     */
    private fun detectTouchDevice() {
        try {
            val ids = InputDevice.getDeviceIds()
            for (id in ids) {
                val device = InputDevice.getDevice(id) ?: continue
                if (device.sources and InputDevice.SOURCE_TOUCHSCREEN ==
                    InputDevice.SOURCE_TOUCHSCREEN
                ) {
                    touchDeviceId = device.id
                    touchDeviceName = device.name ?: "unnamed"
                    return
                }
            }
            Log.w(TAG, "no device in ${ids.size} reports SOURCE_TOUCHSCREEN — " +
                    "injecting with deviceId=0")
            touchDeviceId = 0
            touchDeviceName = "none found"
        } catch (t: Throwable) {
            Log.w(TAG, "could not enumerate input devices " +
                    "(${t.javaClass.simpleName}: ${t.message}) — injecting with deviceId=0")
            touchDeviceId = 0
            touchDeviceName = "enumeration failed"
        }
    }

    // ── Bookkeeping ──────────────────────────────────────────────────────────

    /// One line, at most every 3 s, for an event the platform would not take.
    /// Rate-limited because a refused stream is refused every frame, and this is
    /// the only trace such a failure leaves: InputDispatcher logs the reason on
    /// ITS side (logcat of the system), while nothing at all comes back here.
    private fun reportRefusal(what: String) {
        lastError = what
        val now = SystemClock.uptimeMillis()
        if (now - lastRefusalLogMs < 3000) return
        lastRefusalLogMs = now
        Log.e(TAG, "$what (total failures=$failCount, successes=$okCount) — " +
                "check the system log for \"Injection failed: invalid event\" " +
                "or \"Inconsistent event\"")
    }

    /**
     * `injectInputEvent` needs `android.permission.INJECT_EVENTS`. AOSP grants it
     * to the shell UID; MIUI/HyperOS hides it behind "USB debugging (Security
     * settings)", which has to be switched on and the phone rebooted. scrcpy
     * carries the same hint for the same reason.
     *
     * Rate-limited and never fatal to this object: if the user turns the setting
     * on, the very next frame starts working, so there is nothing to recover.
     */
    private fun reportPermissionProblem(message: String) {
        val now = SystemClock.uptimeMillis()
        lastError = message
        if (now - lastPermissionLogMs < 3000) return
        lastPermissionLogMs = now
        Log.e(TAG, "InputManager refused the event: $message")
        Log.e(TAG, "Enable \"USB debugging (Security settings)\" and reboot the phone " +
                "— without it this device will not let the shell UID inject touches.")
    }

    private fun indexOfExternal(externalId: Int): Int {
        for (i in fingers.indices) if (fingers[i].externalId == externalId) return i
        return -1
    }

    private fun containsExternal(n: Int, externalId: Int): Boolean {
        for (i in 0 until n) if (wantIds[i] == externalId) return true
        return false
    }

    /**
     * The lowest local id nobody is holding right now, or -1 when there is none.
     *
     * Derived from the live list rather than kept in a side table. That is not
     * just tidier — it is the fix for a real fault: a side table has to be
     * released on every path a pointer can leave by, and the one path that
     * forgot to (a lone finger's ACTION_UP) leaked an id per gesture. After ten
     * taps every id was marked taken, so the next two-finger touch handed out
     * an id that was already in use, InputDispatcher rejected every event of
     * that stream as a duplicate pointer id, and the injected pointers froze
     * with no trace on this side. Deriving the answer from the list makes that
     * class of bug impossible instead of merely fixed.
     *
     * Returning -1 rather than a fallback id is deliberate: a duplicate id is
     * far worse than one skipped frame, because the platform throws the whole
     * event away.
     */
    private fun nextLocalId(): Int {
        for (localId in 0 until MAX_POINTERS) {
            var taken = false
            for (f in fingers) {
                if (f.localId == localId) {
                    taken = true
                    break
                }
            }
            if (!taken) return localId
        }
        return -1
    }

    private fun resetState() {
        fingers.clear()
        downTime = 0L
    }

    private fun fail(message: String) {
        ready = false
        lastError = message
        route = "none"
        Log.e(TAG, "InputManager injector unavailable: $message")
    }

    /** `injectInputEvent(InputEvent, int)` — for the log. */
    private fun describeParameters(method: Method): String =
        method.parameterTypes.joinToString(", ", "(", ")") { it.simpleName }

    /** Every `inject*` method the class exposes — for the log when none is usable. */
    private fun describeInjectCandidates(cls: Class<*>): String =
        cls.methods
            .filter { it.name.contains("inject", ignoreCase = true) }
            .joinToString("; ") { it.name + describeParameters(it) }
            .ifEmpty { "no inject* method at all" }

    // ── Context ──────────────────────────────────────────────────────────────

    /**
     * A Context to ask for a system service, in a process that has no Activity.
     *
     * Only reached when both binder routes failed, which on any build that
     * exposes the input service at all should not happen. Same route the capture
     * class already takes: the real system context via `ActivityThread`,
     * starting the main thread's machinery first if this process has not had one
     * yet.
     */
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
        Log.e(TAG, "no system context for InputManager", t)
        null
    }
}
