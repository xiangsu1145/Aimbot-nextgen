// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng input — uinput touch injection
//
//  Creates a virtual touchscreen through /dev/uinput and clones the identity,
//  ABS capabilities and KEY capabilities of the real panel, then injects
//  multitouch events. Ported from the previous project's touch_core
//  (G:\ai\Aimbot-ai\android-client), keeping the injection core and dropping
//  the parts that belonged to the aimbot logic (license gate, detection zones).
//
//  Requires a process that may open /dev/uinput — i.e. root or, on many ROMs,
//  the shell UID. It therefore belongs in the privileged shell daemon, not in
//  the app process.
//
//  Slot layout on the virtual device (a real panel exposes ~10 slots):
//    [0 .. kMirrorSlots)  mirror of the physical fingers (pass-through)
//    UINPUT_SLOT_PRIMARY  programmatic injection (menu/aim finger)
//    UINPUT_SLOT_TRIGGER  programmatic injection (second finger)
//
//  Coordinates passed to ui_* are SCREEN coordinates (the rotation is handled
//  internally), matching what a MotionEvent would report.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Dedicated slots on the virtual device, so injected fingers never collide with
// the mirrored physical ones.
#define UINPUT_SLOT_PRIMARY 8
#define UINPUT_ID_PRIMARY   1000
#define UINPUT_SLOT_TRIGGER 9
#define UINPUT_ID_TRIGGER   2000

// Slots available to uinput_mirror_physical() for physical fingers. A finger is
// given one when it goes down and keeps it until it lifts, so the slice of slot
// numbers a hand consumes is not necessarily contiguous, and slot i does not
// mean "the i-th finger" — it means "whichever finger went down onto it".
#define UINPUT_MIRROR_SLOTS 8

/// Tells uinput which panel to clone its identity / capabilities from. Call
/// this before uinput_init(). When never set, uinput discovers the panel by
/// itself with `getevent -p` — which is wrong once we have already created a
/// virtual device (it would find that one), so the shell daemon always passes
/// the path the reader opened.
void uinput_set_source_panel(const char* path);

/// Opens /dev/uinput, clones the real panel and registers the virtual device.
/// `screenW`/`screenH` are the current display size in pixels (used to map
/// screen coordinates onto the panel's ABS range). Returns false on failure.
bool uinput_init(int screenW, int screenH);

/// Destroys the virtual device and releases every fd. Safe to call repeatedly.
void uinput_close(void);

bool uinput_is_ready(void);

/// fd of the virtual device, or -1 when not initialised.
int uinput_get_fd(void);

/// /dev/input/eventN of the virtual device this daemon owns, or "" when
/// there is none. Stable for the lifetime of the virtual device — used by
/// touch_reader to flag the entry as "this is us" in the panel list, so the
/// user can see it without being able to pick it.
const char* uinput_get_our_path(void);

/// sysfs name (e.g. "input46") of the virtual device this daemon owns, or
/// "" when none. This is the basename of /sys/devices/virtual/input/<name>/,
/// and the only stable handle on a uinput device that survives cloning the
/// panel name (which makes the visible name match the real panel). touch_reader
/// readlink()s /sys/class/input/eventN and compares the basename to this.
const char* uinput_get_our_sysname(void);

/// Updates the screen size / orientation used by the coordinate mapping.
void uinput_set_screen_params(int w, int h, bool landscape);

/// Places a finger down at (screenX, screenY). `id` is the tracking id reported
/// to Android and must stay stable while the finger is down.
void uinput_down(int slot, int id, int screenX, int screenY);

/// Moves a finger that is already down.
void uinput_move(int slot, int screenX, int screenY);

/// Lifts a finger.
void uinput_up(int slot);

/// Mirrors the currently-down physical fingers onto slots [0 .. n) in ONE
/// uinput frame. Fingers beyond `n` — and any mirror slot not listed — are
/// lifted. Used to re-inject what we grabbed off the physical panel, so the
/// app underneath keeps receiving touches while the overlay stays transparent.
///
/// `ids` are the reader's pointer ids — the PANEL's tracking ids. They are not
/// what goes on the wire (every slot carries a constant synthetic id, as in
/// aimbot 1.2.1); they are kept per slot so that touch fusion can answer "which
/// mirror slot is carrying this finger right now", and so that the slot aim has
/// taken over is excluded by finger rather than by position. `rawXs`/`rawYs` are
/// **panel raw coordinates**, not screen pixels: they are written to the device
/// as-is. That is the whole point — this device declares the panel's own
/// coordinate range, so the exact numbers the panel produced are exactly what it
/// wants back, and routing them through screen pixels and out again only adds a
/// truncation and a dependency on two separately-maintained copies of the
/// rotation/scale state. `n` is clamped to UINPUT_MIRROR_SLOTS.
///
/// (The aim's synthetic touches are the other direction and take screen pixels:
/// see uinput_down/uinput_move.)
///
/// `screenXs`/`screenYs` carry the SAME fingers in display pixels, and exist for
/// the InputManager backend: a MotionEvent is in display coordinates and cannot
/// be handed panel raw. The caller already has both arrays — the reader computes
/// them in the same pass — so neither backend ever sees the other's space and
/// nothing is converted at the last moment.
void uinput_mirror_physical(const int* ids, const int* rawXs, const int* rawYs,
                            const int* screenXs, const int* screenYs, int n);

/// Slot-direct mirror for Protocol B panels (old touch_core.cpp behaviour).
///
/// `slotIds[k] >= 0` means panel slot k is down; otherwise it is up.
/// This preserves the panel's own slot number on the virtual device (virtual
/// slot k = panel slot k), so there is no compact-list allocation and no
/// same-frame lift+reuse of one wire id — the old project's 1:1 model that
/// never exhibited the teleport. Only slots [0 .. UINPUT_MIRROR_SLOTS) are
/// mirrored; slots beyond are ignored. `slotIds` may be larger than
/// UINPUT_MIRROR_SLOTS, excess entries are ignored.
void uinput_mirror_slots(const int* slotIds, const int* rawXs, const int* rawYs,
                         const int* screenXs, const int* screenYs, int maxSlots);

/// Lifts every mirrored finger (call when the sink is switched off so the
/// app underneath does not keep a phantom finger pressed).
void uinput_mirror_clear(void);

/// Aim "touch fusion": reserve the mirror slot currently carrying the physical
/// finger `id` so the aim loop can drive that real finger directly (instead of
/// pressing a separate synthetic finger). Returns the slot to drive, or -1 when
/// `id` is not currently mirrored. While a slot is reserved,
/// uinput_mirror_physical() stops updating/lifting it, so there is exactly one
/// writer on that slot — no double-touch flicker. The finger is already down in
/// the physical world, so the caller must MOVE it only (never uinput_down) and
/// must call uinput_release_takeover() (not uinput_up) when done, so the mirror
/// resumes reporting it seamlessly.
int uinput_takeover_physical_id(int id);

/// Release any reserved mirror slot. Call when aim stops driving a real finger;
/// the mirror then resumes reporting that finger without a down/up flicker.
void uinput_release_takeover(void);

/// How many frames in a row failed to reach the kernel. Resets to 0 on the
/// first frame that lands.
///
/// The physical panel is grabbed while this device is the only source of touch,
/// so a device that has stopped accepting writes is not a dropped input, it is
/// a phone that no longer responds to anything. That is worth reporting loudly
/// and worth NOT hiding: the obvious recovery — releasing the grab — would
/// restore the touchscreen and with it the appearance that nothing happened,
/// so the reader reports this and leaves the grab alone.
int uinput_write_failures(void);
#ifdef __cplusplus
}
#endif
