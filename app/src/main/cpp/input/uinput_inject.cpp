// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng input — uinput touch injection (implementation)
//
//  Ported from G:\ai\Aimbot-ai\android-client\app\src\main\cpp\src\injection\
//  touch_core.cpp. Kept: physical-panel discovery via `getevent -p`, identity /
//  ABS / KEY capability cloning, and the multitouch frame writer. Removed: the
//  license gate, the detection zones and the evdev reader threads (aimbot
//  logic, none of which belongs to the touch-injection primitive).
//
//  NOTE on EVIOCGRAB: this module never grabs. The grab is taken by touch_reader
//  (reader_grab) once the mirror here is ready to stand in for the physical
//  panel, and that ordering is the whole safety story: from the moment the panel
//  is grabbed, the virtual device below is the only source of touch the phone
//  has. A frame lost here is therefore not a dropped input event, it is the
//  screen going dead — which is why upload() no longer tolerates a partial or
//  rejected write.
// ─────────────────────────────────────────────────────────────────────────────
#include "uinput_inject.h"
#include "inject_backend.h"
#include "panel_filter.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include <time.h>

#include <android/log.h>

#define LOG_TAG "AimbotInput"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kMaxFingers = 10;
/// First tracking id the mirror hands out. 1.2.1 uses
/// `(devIdx * 2 + 1) * maxF + slot` with maxF = 10, so device 0 starts at 10.
constexpr int kMirrorIdBase = 10;

/// A slot's panel id while it is carrying nobody. Not 0: a panel id of 0 is a
/// perfectly ordinary tracking id, and reading "empty" as "finger 0" is how a
/// slot gets handed the identity of a finger that is not there.
constexpr int kNoPanelId = -1;

/// Finger identities arrive from the reader out of two sources that share one
/// numeric range: a protocol-B panel's own tracking id (0..65535) and the id the
/// reader invents for protocol-A frames, which live in 60000..63999 (see
/// kSyntheticBase / kSyntheticMax in touch_reader.cpp). They are mapped to
/// disjoint bands here, because on the InputManager backend the external id is
/// the *entire* identity of a pointer — two fingers sharing one is a frozen
/// pointer with no trace on this side.
constexpr int kPanelSyntheticBase = 60000;
constexpr int kPanelSyntheticMax  = 63999;
constexpr int kMirrorExtIdBase    = 100000;   // protocol B: base + tracking id
constexpr int kSyntheticExtIdBase = 200000;   // protocol A: base + (id - 60000)

/// The identity a mirrored finger is known by on the InputManager backend.
/// The aim's and the trigger's fixed ids are 1000 / 2000, clear of both bands.
int mirrorExtId(int panelId) {
    if (panelId >= kPanelSyntheticBase && panelId <= kPanelSyntheticMax)
        return kSyntheticExtIdBase + (panelId - kPanelSyntheticBase);
    return kMirrorExtIdBase + panelId;
}

constexpr int kMaxEvents = 512;

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float px, float py) : x(px), y(py) {}
};

/// One finger slot on the virtual device.
struct TouchObj {
    /// Panel coordinates — what the uinput frame carries. A mirror slot is
    /// filled with the exact numbers the panel reported; an aim slot with the
    /// conversion of screenToTouch().
    Vec2 pos{};
    /// The same finger in screen pixels, which is what a MotionEvent needs.
    /// Both are kept because the two backends want different things and
    /// converting at the last moment would mean either a round trip through
    /// truncating integer pixels (uinput) or a rotation-dependent inverse
    /// (InputManager), and this module used to get that wrong precisely because
    /// the value had to be reconstructed rather than remembered.
    Vec2 sPos{};
    /// The identity this finger is known by **outside** this file — what the
    /// InputManager backend keys its pointers on.
    ///
    /// Deliberately not [id]. [id] is the *wire* identity of a slot (`10 + k`,
    /// assigned by the position a finger happens to occupy), which is exactly
    /// right for uinput, where a slot is a lane the kernel tracks, and exactly
    /// wrong for a MotionEvent, where an id identifies the finger itself. Two
    /// physical fingers keep their panel tracking ids for as long as they are
    /// down, so those are what a pointer is keyed on here; the aim's and the
    /// trigger's ids go in as they are, in a range the panel cannot produce.
    int extId = 0;
    int id = 0;
    bool isDown = false;
};

struct InputBuffer {
    input_event events[kMaxEvents]{};
};

// ── State ────────────────────────────────────────────────────────────────────

std::mutex g_mutex;

int  g_outputFd = -1;        // /dev/uinput handle
bool g_initialized = false;

// ── Which way the frame leaves ───────────────────────────────────────────────
//
// Only the last hop differs between the two backends; everything above (slots,
// ids, the mirror mapping, the aim's takeover) is one implementation. See
// input/inject_backend.h for why both exist and why switching is destructive.
int  g_backend = INJECT_BACKEND_UINPUT;
/// Whether the InputManager path is usable. Only meaningful while g_backend is
/// INJECT_BACKEND_INPUT_MANAGER, and set from the Kotlin side's own answer —
/// never assumed, because "the reflection call returned" is not "the platform
/// accepted the event".
bool g_imReady = false;

/// Can the selected backend deliver a frame right now?
bool backendReadyLocked() {
    return g_backend == INJECT_BACKEND_INPUT_MANAGER ? g_imReady : g_initialized;
}

// Panel the caller wants us to clone (set by uinput_set_source_panel). When
// empty we fall back to discovering it with `getevent -p`.
char g_sourcePanel[256] = {};

// /dev/input/eventN of the virtual device we just created, or "" when none
// is alive. Used by touch_reader's enumeration to flag the entry as our own
// uinput device so the user can see it without being able to pick it.
char g_ourVirtualPath[128] = {};

// sysfs name (e.g. "input46") from UI_GET_SYSNAME — this is the basename
// of /sys/devices/virtual/input/<name>/, and the only stable handle on a
// uinput device that survives both the "name is the cloned panel" and the
// "name is a per-launch random string" paths. touch_reader matches each
// candidate eventN by readlink()-ing /sys/class/input/eventN and comparing
// the basename.
char g_ourVirtualSysname[64] = {};

TouchObj g_fingers[kMaxFingers]{};
bool     g_uploaded[kMaxFingers]{};   // what the kernel currently believes

/// The PANEL's tracking id currently sitting in each mirror slot, or
/// kNoPanelId. This is what makes a slot belong to a *finger* rather than to a
/// position.
///
/// The wire carries a constant synthetic id per slot (kMirrorIdBase + slot),
/// which is what aimbot 1.2.1 sends and what this file was aligned to; keeping
/// the panel id here is what lets the slot follow the finger instead.
///
/// That distinction is the whole point. The reader hands us a compacted list —
/// the gaps where lifted fingers used to be are gone — so the index a finger
/// appears at changes as its neighbours come and go. Assigning slots by that
/// index (which 1.2.1 can afford to do, because it indexes by the panel's own
/// slot number and never compacts) means the finger in slot 1 moves to slot 0
/// the moment the finger in slot 0 lifts; since a slot's wire identity is fixed,
/// the app is then told that pointer 10 walked from one finger's position to the
/// other's. That is the "two touch points jumping between positions" fault, and
/// this array is the fix: a slot keeps its owner until that owner is gone.
///
/// It is also the only way to answer "which slot is carrying the finger the
/// player is holding right now" for touch fusion — the wire id cannot, since it
/// is a constant per slot.
int g_panelIds[kMaxFingers] = {kNoPanelId, kNoPanelId, kNoPanelId, kNoPanelId, kNoPanelId,
                               kNoPanelId, kNoPanelId, kNoPanelId, kNoPanelId, kNoPanelId};

// "Touch fusion" takeover: while >= 0, uinput_mirror_physical() leaves this
// mirror slot alone (neither re-updates nor lifts it) so the aim loop can drive
// the real finger that lives there directly. Set by uinput_takeover_physical_id()
// (aim finds the physical finger in the touch area and reserves its slot) and
// cleared by uinput_release_takeover(). Keeping the slot down while aim owns it
// avoids the down/up/down flicker that two writers on one slot would cause.
int g_takeoverSlot = -1;
/// The PANEL tracking id aim reserved — i.e. the argument it passed to
/// uinput_takeover_physical_id().
///
/// The slot index alone is not sufficient. Slots are claimed by finger, so a
/// given finger does keep the same index — but the *exclusion* has to survive
/// the finger changing places too (a slot released and re-claimed differently
/// after a hand-off), and matching on the panel id is the form that stays true
/// under all of it. Excluding by index would exclude whichever finger happens to
/// sit there now, and the finger aim is driving could end up in a slot nobody is
/// excluding — the two-writers-on-one-slot situation all over again.
int g_takeoverPanelId = -1;

/// Whether the finger aim is driving showed up in the most recent mirror frame.
///
/// uinput_release_takeover() lifts the slot it hands back, because a finger that
/// is already physically gone will never be re-reported and the slot would stay
/// down as a phantom point forever. But if the finger is *still* down, lifting
/// it is pure noise: the mirror presses it again one frame later and the app
/// sees a pointer blink out and return. That flag tells the two apart.
bool g_takeoverSeen = false;

// Raised when a frame did not reach the kernel in full. The next frame then
// re-states every slot from scratch instead of sending deltas against a state
// the device never had — see upload().
bool g_forceRestate = false;

// Counts failed frames, so a sustained failure is visible without a log line
// per frame.
int g_writeFailures = 0;

InputBuffer g_buffer{};

float g_scaleX = 1.0f;       // screen px -> panel units
float g_scaleY = 1.0f;
int  g_screenW = 0;
int  g_screenH = 0;
bool g_landscape = true;

// Axes present on the real panel; 0 means "not advertised", in which case we
// must not report a value on that axis (a value on an unadvertised axis is
// itself a giveaway).
int g_pressureMax = 0;
int g_touchMajorMax = 0;
int g_widthMajorMax = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────

void genRandomString(char* str, int len) {
    srand(static_cast<unsigned>(time(nullptr)) + len);
    for (int i = 0; i < len - 1; ++i) {
        int flag = rand() % 3;
        if (flag == 0)      str[i] = static_cast<char>('A' + rand() % 26);
        else if (flag == 1) str[i] = static_cast<char>('a' + rand() % 26);
        else                str[i] = static_cast<char>('0' + rand() % 10);
    }
    str[len - 1] = '\0';
}

int randInRange(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + rand() % (hi - lo + 1);
}

int pushEvent(int& count, unsigned short type, unsigned short code, int value) {
    if (count >= kMaxEvents) return count;
    g_buffer.events[count].type = type;
    g_buffer.events[count].code = code;
    g_buffer.events[count].value = value;
    return count + 1;
}

/// Screen coordinates -> panel coordinates (portrait space), applying rotation.
void screenToTouch(int sx, int sy, float& tx, float& ty) {
    float px = g_landscape ? static_cast<float>(g_screenH - sy) : static_cast<float>(sx);
    float py = g_landscape ? static_cast<float>(sx) : static_cast<float>(sy);
    tx = px * g_scaleX;
    ty = py * g_scaleY;
}

// ── Injection health line ────────────────────────────────────────────────────
//
// Written for the case where we are NOT looking at the device. A remote report
// arrives as "his phone has no touch", and the one thing nobody can tell us is
// where along 面板 → grab → mirror → write → InputReader → App the signal died.
// Everything from write() rightwards is invisible here, so the best we can do
// is make the left half unambiguous — and it has to speak even when nobody is
// touching anything, because "nothing happened" and "nothing happened *yet*"
// look identical in a log that only records taps.
//
// So once every kHealthPeriodMs the whole injection state is summarised in one
// line, always:
//   +0 frames over a whole minute  → nothing ever called the mirror, i.e. the
//                                    fault is upstream of us (reader / grab);
//   frames climbing, failures=0    → frames reached write() and the kernel took
//                                    them, so the fault is downstream (InputReader,
//                                    the OEM stack) and no amount of staring at
//                                    this module will find it;
//   failures climbing              → the frames are being refused right here.
constexpr long long kHealthPeriodMs = 10'000;

long      g_uploadFrames    = 0;    // every frame that reached write()
long      g_restateFrames   = 0;    // frames that were full re-states (resyncs)
long long g_lastHealthMs    = 0;
long      g_lastHealthFrames = 0;

long long monotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

/// Prints the periodic summary described above. Called from upload() so the
/// line doubles as proof that the mirror is still being driven.
void maybeLogHealth() {
    const long long now = monotonicMs();
    if (g_lastHealthMs == 0) {
        g_lastHealthMs = now;
        g_lastHealthFrames = g_uploadFrames;
        return;
    }
    if (now - g_lastHealthMs < kHealthPeriodMs) return;

    // Which fingers are down, and where each of them came from. "Nothing gets
    // injected" has two causes that look identical from the outside — the
    // mirror never sees a physical finger, or aim never asks for one — and a
    // single `down=1` cannot tell them apart. Splitting the count by origin
    // does, from one line, without a second collection round trip.
    //
    // A slot with a panel id is one the reader is mirroring (or one aim has
    // taken over); a slot without one was asked for by program logic.
    int down = 0, mirrorDown = 0, aimDown = 0;
    for (int i = 0; i < kMaxFingers; ++i) {
        if (!g_fingers[i].isDown) continue;
        ++down;
        if (g_panelIds[i] != kNoPanelId) ++mirrorDown; else ++aimDown;
    }

    LOGI("health %lldms: frames=%+ld/%ld restates=%ld failures=%d force_restate=%d "
         "down=%d mirror=%d aim=%d backend=%s fd=%d ready=%d take=%d our=%s",
         now - g_lastHealthMs,
         g_uploadFrames - g_lastHealthFrames, g_uploadFrames, g_restateFrames,
         g_writeFailures, g_forceRestate ? 1 : 0, down, mirrorDown, aimDown,
         g_backend == INJECT_BACKEND_INPUT_MANAGER ? "inputmgr" : "uinput",
         g_outputFd,
         g_initialized ? 1 : 0, g_takeoverSlot,
         g_ourVirtualPath[0] != '\0' ? g_ourVirtualPath : "(none)");

    g_lastHealthMs = now;
    g_lastHealthFrames = g_uploadFrames;
}

// ── Frame writer ─────────────────────────────────────────────────────────────

/// The ABS codes this device actually declared, in declaration order.
///
/// Printed in the `uinput ready` line. Without it a log cannot answer two
/// questions that came up the hard way: which build of this file is running
/// (does the set contain ABS_X/ABS_Y?), and whether the declared axes are the
/// ones we think they are. Two sessions of remote debugging went into guessing
/// at exactly that, so it is now stated rather than assumed.
char g_declaredAbs[192] = {};

void recordDeclaredAbs(int code) {
    const size_t used = strlen(g_declaredAbs);
    if (used >= sizeof(g_declaredAbs) - 8) return;
    snprintf(g_declaredAbs + used, sizeof(g_declaredAbs) - used, "%s%d",
             used != 0 ? "," : "", code);
}

/// Emits one complete multitouch frame describing every finger slot.
///
/// aimbot 1.2.1 verbatim (touch_core.cpp -> upload()): one `write` per frame,
/// no retry, no restate. Per active finger: SLOT, TRACKING_ID (only when the
/// kernel is not already believed to hold it), POSITION_X/Y, then PRESSURE /
/// TOUCH_MAJOR / WIDTH_MAJOR with a random value inside a small slice of the
/// panel's own maximum — but only for the axes the cloned panel advertises
/// (`g_*Max > 0`), because reporting a value on an axis the panel does not have
/// is itself a tell. The tail is BTN_TOUCH, the five BTN_TOOL_* bits and the
/// terminating SYN_REPORT.
///
/// Absolute coordinates go out raw: the reader stores what the panel reported
/// and this writes it back unchanged, which is what 1.2.1 does (its s2tx/s2ty
/// are 1.0 by construction). No ABS_X/ABS_Y are sent, because the device does
/// not declare them — 1.2.1 does not either.
/// InputManager half of upload(): hands the complete desired pointer set to the
/// Kotlin state machine and lets it work out what changed.
///
/// The division of labour is the point. This side knows which slots exist and
/// what should be under each of them *now*; the other side knows what it told
/// Android last time, and therefore what the correct action for this change is
/// (DOWN vs POINTER_DOWN, which pointer index a POINTER_UP refers to, when the
/// gesture's downTime started). Neither half can be split any further without
/// somebody reconstructing the other's state, which is exactly how the previous
/// implementation ended up with two pointers sharing one id.
///
/// Screen pixels, not panel raw: a MotionEvent is in display coordinates.
/// `sPos` is filled by whichever call put the finger down — the mirror from the
/// reader's own screen conversion, aim from the coordinates it was given — so
/// nothing is recomputed here.
void uploadViaInputManager() {
    int ids[kMaxFingers];
    int xs[kMaxFingers];
    int ys[kMaxFingers];
    int n = 0;

    for (int fi = 0; fi < kMaxFingers; ++fi) {
        if (!g_fingers[fi].isDown) continue;
        ids[n] = g_fingers[fi].extId;
        xs[n] = static_cast<int>(g_fingers[fi].sPos.x);
        ys[n] = static_cast<int>(g_fingers[fi].sPos.y);
        ++n;
    }
    aimbotng::input::imBridgePushFrame(ids, xs, ys, n);
}

void upload() {
    ++g_uploadFrames;
    maybeLogHealth();

    if (g_backend == INJECT_BACKEND_INPUT_MANAGER) {
        if (g_imReady) uploadViaInputManager();
        return;
    }

    if (g_outputFd < 0) return;

    int count = 0;
    int activeFingerCount = 0;
    bool hasActiveFinger = false;

    for (int fi = 0; fi < kMaxFingers; ++fi) {
        const TouchObj& finger = g_fingers[fi];
        const bool wasUploaded = g_uploaded[fi];

        if (finger.isDown) {
            hasActiveFinger = true;
            ++activeFingerCount;
            count = pushEvent(count, EV_ABS, ABS_MT_SLOT, fi);
            if (!wasUploaded)
                count = pushEvent(count, EV_ABS, ABS_MT_TRACKING_ID, finger.id);
            count = pushEvent(count, EV_ABS, ABS_MT_POSITION_X, static_cast<int>(finger.pos.x));
            count = pushEvent(count, EV_ABS, ABS_MT_POSITION_Y, static_cast<int>(finger.pos.y));
            // Only emit axes the cloned panel advertises.
            if (g_pressureMax > 0)
                count = pushEvent(count, EV_ABS, ABS_MT_PRESSURE,
                                  randInRange(g_pressureMax / 333, g_pressureMax / 40));
            if (g_touchMajorMax > 0)
                count = pushEvent(count, EV_ABS, ABS_MT_TOUCH_MAJOR,
                                  randInRange(g_touchMajorMax / 12, g_touchMajorMax / 4));
            if (g_widthMajorMax > 0)
                count = pushEvent(count, EV_ABS, ABS_MT_WIDTH_MAJOR,
                                  randInRange(g_widthMajorMax / 12, g_widthMajorMax / 4));
            g_uploaded[fi] = true;
        } else if (wasUploaded) {
            count = pushEvent(count, EV_ABS, ABS_MT_SLOT, fi);
            count = pushEvent(count, EV_ABS, ABS_MT_TRACKING_ID, -1);
            g_uploaded[fi] = false;
        }
    }

    count = pushEvent(count, EV_KEY, BTN_TOUCH, hasActiveFinger ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_FINGER, activeFingerCount == 1 ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_DOUBLETAP, activeFingerCount == 2 ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_TRIPLETAP, activeFingerCount == 3 ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_QUADTAP, activeFingerCount == 4 ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_QUINTTAP, activeFingerCount >= 5 ? 1 : 0);
    count = pushEvent(count, EV_SYN, SYN_REPORT, 0);

    const size_t total = sizeof(input_event) * static_cast<size_t>(count);
    const ssize_t n = write(g_outputFd, g_buffer.events, total);
    if (n < 0) {
        ++g_writeFailures;
        if (g_writeFailures == 1 || (g_writeFailures % 120) == 0) {
            LOGE("upload: write failed errno=%d (failures=%d)", errno, g_writeFailures);
        }
    }
}

/// Finds the real touchscreen by parsing `getevent -p`. Doing it through
/// getevent avoids needing a direct /dev/input fd just to enumerate devices.
/// Fills out the path and the panel's coordinate range.
///
/// Accepts Protocol B (ABS_MT_SLOT + MT X/Y) unconditionally; Protocol A (no
/// slot) only with the full direct-touch signature — MT X/Y + BTN_TOUCH +
/// INPUT_PROP_DIRECT — same gate as touch_reader's live probe. The protocol-A
/// path exists for Huawei/Honor, whose only touch node ("input_mt_wrapper")
/// has no ABS_MT_SLOT; the BTN_TOUCH+DIRECT requirements are what keep vivo's
/// fingerprint-helper nodes (vivo_ts_fp / vivo_fp, MT axes at panel
/// resolution but no BTN_TOUCH, no DIRECT) out.
///
/// Ranks candidates the same way touch_reader's live enumeration does
/// (INPUT_PROP_DIRECT + extra MT axes, blacklist by name), because the first
/// passing device in getevent's output order is not necessarily the real
/// panel — a Bluetooth band mimicking a touchscreen sorts earlier. getevent
/// prints no bus type, so DIRECT + axis count is all the text fallback has.
bool detectTouchDeviceViaGetevent(char* outPath, size_t pathSize, int& outMaxX, int& outMaxY) {
    FILE* fp = popen("/system/bin/getevent -p 2>&1", "r");
    if (!fp) {
        LOGE("detectTouchDevice: popen getevent failed");
        return false;
    }

    struct Candidate {
        std::string path;
        int maxX = 0, maxY = 0;
        int score = 0;
    };
    std::vector<Candidate> candidates;

    char line[512];
    std::string currentPath;
    bool inDevice = false, skipDevice = false, inKeyBlock = false;
    bool hasSlot = false, hasX = false, hasY = false, hasDirect = false;
    bool hasBtnTouch = false;
    bool hasTracking = false, hasTouchMajor = false, hasPressure = false;
    int maxX = 0, maxY = 0;

    auto commitCurrent = [&]() {
        if (inDevice && !skipDevice && hasX && hasY &&
            (hasSlot || (hasBtnTouch && hasDirect))) {
            int score = hasDirect ? 1 : 0;
            if (hasTracking)   ++score;
            if (hasTouchMajor) ++score;
            if (hasPressure)   ++score;
            candidates.push_back({currentPath, maxX, maxY, score});
            LOGI("getevent candidate %s max=%d,%d slot=%d direct=%d btnTouch=%d score=%d",
                 currentPath.c_str(), maxX, maxY, hasSlot ? 1 : 0,
                 hasDirect ? 1 : 0, hasBtnTouch ? 1 : 0, score);
        }
    };

    while (fgets(line, sizeof(line), fp)) {
        // New device section: "add device N: /dev/input/eventX"
        if (strstr(line, "add device") && strstr(line, "/dev/input/event")) {
            commitCurrent();

            char* p = strstr(line, "/dev/input/event");
            if (p) {
                char* end = p;
                while (*end && *end != '\n' && *end != '\r' && *end != ' ') end++;
                currentPath.assign(p, static_cast<size_t>(end - p));
            }
            inDevice = true;
            skipDevice = false;
            inKeyBlock = false;
            hasSlot = hasX = hasY = hasDirect = false;
            hasBtnTouch = false;
            hasTracking = hasTouchMajor = hasPressure = false;
            maxX = maxY = 0;
            continue;
        }
        if (!inDevice) continue;

        // "name:" comes before the events/props sections, so a device we must
        // skip (our own earlier uinput device, or a blacklisted accessory) is
        // flagged here and its later axis lines are ignored entirely.
        if (strstr(line, "name:")) {
            skipDevice = strstr(line, "Aimbot") != nullptr ||
                         isBlacklistedPanelName(line);
            if (skipDevice) LOGI("getevent: %s blacklisted — skipped", currentPath.c_str());
            continue;
        }
        if (skipDevice) continue;

        if (strstr(line, "INPUT_PROP_DIRECT")) hasDirect = true;

        // KEY 块内找 BTN_TOUCH (014a)。getevent 的续行是纯十六进制码，不带
        // 所属类型头，所以靠"最近一次出现的事件类型头"判断当前行属于哪个块；
        // 只在 KEY 块里匹配 014a，避免把它误当成其他字段里的数字。
        if (strstr(line, "KEY (")) {
            inKeyBlock = true;
        } else if (strstr(line, "ABS (") || strstr(line, "SW (") ||
                   strstr(line, "FF (") || strstr(line, "MSC (") ||
                   strstr(line, "LED (") || strstr(line, "SND (") ||
                   strstr(line, "REP (") || strstr(line, "EV (")) {
            inKeyBlock = false;
        }
        if (inKeyBlock &&
            (strstr(line, "014a") || strstr(line, "BTN_TOUCH"))) {
            hasBtnTouch = true;
        }

        // 002f=ABS_MT_SLOT(47), 0035=ABS_MT_POSITION_X(53), 0036=ABS_MT_POSITION_Y(54),
        // 0039=ABS_MT_TRACKING_ID, 0030=ABS_MT_TOUCH_MAJOR, 003a=ABS_MT_PRESSURE.
        // Some Android builds print symbolic names instead — accept both.
        if (strstr(line, "002f") || strstr(line, "ABS_MT_SLOT")) hasSlot = true;
        if (strstr(line, "0035") || strstr(line, "ABS_MT_POSITION_X")) {
            hasX = true;
            int val;
            if (sscanf(line, "%*x%*[^m]min %*d, max %d", &val) == 1 && val > 0) maxX = val;
        }
        if (strstr(line, "0036") || strstr(line, "ABS_MT_POSITION_Y")) {
            hasY = true;
            int val;
            if (sscanf(line, "%*x%*[^m]min %*d, max %d", &val) == 1 && val > 0) maxY = val;
        }
        if (strstr(line, "0039") || strstr(line, "ABS_MT_TRACKING_ID")) hasTracking = true;
        if (strstr(line, "0030") || strstr(line, "ABS_MT_TOUCH_MAJOR")) hasTouchMajor = true;
        if (strstr(line, "003a") || strstr(line, "ABS_MT_PRESSURE")) hasPressure = true;
    }
    commitCurrent();

    pclose(fp);
    if (candidates.empty()) return false;

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         return a.score > b.score;
                     });
    const Candidate& best = candidates.front();
    strncpy(outPath, best.path.c_str(), pathSize - 1);
    outPath[pathSize - 1] = '\0';
    outMaxX = best.maxX > 0 ? best.maxX : 0;
    outMaxY = best.maxY > 0 ? best.maxY : 0;
    LOGD("detected touch device: %s abs=%dx%d", outPath, outMaxX, outMaxY);
    return true;
}

// ── Virtual device creation ──────────────────────────────────────────────────

/// Registers the virtual touchscreen, cloning as much of the real panel's
/// identity and capability set as we can read from `sourceFd` (may be -1).
bool createUinputDevice(int screenX, int screenY, int sourceFd) {
    uinput_user_dev dev{};
    g_declaredAbs[0] = '\0';   // fresh declaration list per device
    g_outputFd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_outputFd < 0) {
        LOGE("open /dev/uinput failed errno=%d", errno);
        return false;
    }

    // ══════════════════════════════════════════════════════════════════════
    //  The rest of this function is aimbot 1.2.1 VERBATIM.
    //
    //  Source: G:\ai\Aimbot-ai\android-client @ 1.2.1
    //          app/src/main/cpp/src/injection/touch_core.cpp
    //          -> createUinputDevice()
    //
    //  Everything below this banner is a line-for-line copy, and the same is
    //  true of upload() and uinput_mirror_physical() further down. The point is
    //  not tidiness: 1.2.1 is the build that was measured working on the device
    //  we cannot reproduce, with uinput, and every "improvement" this fork had
    //  layered on top of it (random device name, a reduced ABS set, added
    //  ABS_X/Y, per-gesture mirror gating) moved it further from that build.
    //  So the injection path is now that build, and nothing else.
    // ══════════════════════════════════════════════════════════════════════

    // ── Device identity: clone from the real panel when we have its fd ──
    // 1.2.1 deliberately clones the panel's own identity: a cloned
    // bustype/vendor/product/name makes /proc/bus/input/devices and
    // InputDevice enumeration match a genuine touchscreen. Without a source fd
    // it falls back to plausible values (BUS_I2C is the common touch bus)
    // rather than the illegal bustype=0 a bare uinput device would expose.
    input_id realId{};
    bool haveRealId = (sourceFd >= 0 && ioctl(sourceFd, EVIOCGID, &realId) == 0);

    char realName[UINPUT_MAX_NAME_SIZE]{};
    bool haveRealName = (sourceFd >= 0 &&
                         ioctl(sourceFd, EVIOCGNAME(sizeof(realName) - 1), realName) > 0 &&
                         realName[0] != '\0');

    if (haveRealName) {
        strncpy(dev.name, realName, UINPUT_MAX_NAME_SIZE - 1);
    } else {
        char randomName[16]{};
        genRandomString(randomName, sizeof(randomName));
        strncpy(dev.name, randomName, UINPUT_MAX_NAME_SIZE - 1);
    }

    if (haveRealId) {
        dev.id = realId;
    } else {
        dev.id.bustype = BUS_I2C;
        dev.id.vendor = rand() % 10 + 5;
        dev.id.product = rand() % 10 + 5;
        dev.id.version = rand() % 10 + 5;
    }

    // Clone input device properties (INPUT_PROP_DIRECT, INPUT_PROP_POINTER, ...)
    // from the real panel. Different vendors set different prop combinations —
    // a OnePlus panel is INPUT_PROP_DIRECT alone, some Samsung panels also set
    // INPUT_PROP_POINTER, etc. Falling back to INPUT_PROP_DIRECT if the clone
    // fails keeps the device at least minimally registerable.
    bool clonedProps = false;
    if (sourceFd >= 0) {
        uint8_t propBits[64]{};
        ssize_t propRes = ioctl(sourceFd, EVIOCGPROP(sizeof(propBits)), propBits);
        if (propRes > 0) {
            int propCount = static_cast<int>(propRes) < static_cast<int>(sizeof(propBits))
                                ? static_cast<int>(propRes)
                                : static_cast<int>(sizeof(propBits));
            for (int j = 0; j < propCount; ++j) {
                for (int k = 0; k < 8; ++k) {
                    int code = j * 8 + k;
                    if (propBits[j] & (1 << k)) {
                        ioctl(g_outputFd, UI_SET_PROPBIT, code);
                    }
                }
            }
            clonedProps = true;
        }
    }
    if (!clonedProps) {
        ioctl(g_outputFd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    }

    ioctl(g_outputFd, UI_SET_EVBIT, EV_ABS);
    ioctl(g_outputFd, UI_SET_EVBIT, EV_SYN);
    ioctl(g_outputFd, UI_SET_EVBIT, EV_KEY);

    // Mandatory axes for our multitouch injection. Note: we intentionally do NOT
    // register ABS_X/ABS_Y — real panels (including this device's "touchpanel")
    // report coordinates via ABS_MT_POSITION_X/Y only. A ghost ABS_X/Y axis is
    // a strong uinput tell. SLOT/POSITION ranges are overridden to the panel's
    // actual coordinate extent below.
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    recordDeclaredAbs(ABS_MT_SLOT);
    recordDeclaredAbs(ABS_MT_POSITION_X);
    recordDeclaredAbs(ABS_MT_POSITION_Y);
    recordDeclaredAbs(ABS_MT_TRACKING_ID);

    char randomPhys[16]{};
    genRandomString(randomPhys, sizeof(randomPhys));
    ioctl(g_outputFd, UI_SET_PHYS, randomPhys);

    // ── Clone every ABS axis (incl. pressure / touch major / width) from the
    // real panel, preserving each axis's min/max/fuzz/flat. This adds the
    // pressure and contact-area capability bits a genuine touchscreen has. ──
    g_pressureMax = g_touchMajorMax = g_widthMajorMax = 0;
    if (sourceFd >= 0) {
        uint8_t* absBits = nullptr;
        ssize_t absSize = 0;
        int absRes = 0;
        while (true) {
            absRes = ioctl(sourceFd, EVIOCGBIT(EV_ABS, absSize), absBits);
            if (absRes < absSize) break;
            absSize = absRes + 16;
            absBits = static_cast<uint8_t*>(realloc(absBits, absSize * 2));
        }
        for (int j = 0; j < absRes; ++j) {
            for (int k = 0; k < 8; ++k) {
                int code = j * 8 + k;
                if (!(absBits[j] & (1 << k))) continue;
                input_absinfo ai{};
                if (ioctl(sourceFd, EVIOCGABS(code), &ai) != 0) continue;
                ioctl(g_outputFd, UI_SET_ABSBIT, code);
                recordDeclaredAbs(code);
                dev.absmin[code] = ai.minimum;
                dev.absmax[code] = ai.maximum;
                dev.absfuzz[code] = ai.fuzz;
                dev.absflat[code] = ai.flat;
                if (code == ABS_MT_PRESSURE)    g_pressureMax = ai.maximum;
                if (code == ABS_MT_TOUCH_MAJOR) g_touchMajorMax = ai.maximum;
                if (code == ABS_MT_WIDTH_MAJOR) g_widthMajorMax = ai.maximum;
            }
        }
        free(absBits);
    }

    // Clone the real panel's KEY capabilities (button set) verbatim. We do NOT
    // hardcode any BTN_TOOL_* bits — doing so would diverge from the real panel
    // (which is a strong uinput tell). Fall back to the minimum required
    // multitouch set (BTN_TOUCH + BTN_TOOL_FINGER) if no source fd is available.
    uint8_t* bits = nullptr;
    ssize_t bitsSize = 0;
    int res = 0;
    bool clonedKeys = false;
    if (sourceFd >= 0) {
        while (true) {
            res = ioctl(sourceFd, EVIOCGBIT(EV_KEY, bitsSize), bits);
            if (res < bitsSize) break;
            bitsSize = res + 16;
            bits = static_cast<uint8_t*>(realloc(bits, bitsSize * 2));
        }
        for (int j = 0; j < res; ++j) {
            for (int k = 0; k < 8; ++k) {
                int code = j * 8 + k;
                if (bits[j] & (1 << k)) {
                    ioctl(g_outputFd, UI_SET_KEYBIT, code);
                }
            }
        }
        clonedKeys = res > 0;
    }
    free(bits);
    if (!clonedKeys) {
        ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOUCH);
        ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    }

    // Override ranges for axes we drive. The ABS-bit loop above already copied
    // min/max from the real panel for every axis (including SLOT, POSITION_X/Y,
    // PRESSURE, TOUCH_MAJOR). We only re-pin:
    //   - POSITION_X/Y to the panel's actual coordinate extent
    //   - TRACKING_ID to 65535 so we never run out of IDs internally.
    // ABS_MT_SLOT is left as the real panel reported it (Pixel/older panels
    // declare 5 slots, OnePlus/Samsung declare 10). Fall back to 9 only when no
    // real-panel data was available.
    if (dev.absmax[ABS_MT_SLOT] == 0) {
        dev.absmin[ABS_MT_SLOT] = 0;
        dev.absmax[ABS_MT_SLOT] = kMaxFingers - 1;
    }
    dev.absmin[ABS_MT_POSITION_X] = 0;
    dev.absmax[ABS_MT_POSITION_X] = screenX;
    dev.absmin[ABS_MT_POSITION_Y] = 0;
    dev.absmax[ABS_MT_POSITION_Y] = screenY;
    dev.absmin[ABS_MT_TRACKING_ID] = 0;
    dev.absmax[ABS_MT_TRACKING_ID] = 65535;
    if (write(g_outputFd, &dev, sizeof(dev)) < 0) {
        LOGE("write(uinput_user_dev) failed errno=%d", errno);
        close(g_outputFd);
        g_outputFd = -1;
        return false;
    }

    if (ioctl(g_outputFd, UI_DEV_CREATE) != 0) {
        LOGE("UI_DEV_CREATE failed errno=%d", errno);
        close(g_outputFd);
        g_outputFd = -1;
        return false;
    }

    // Record our own uinput device identity. UI_GET_SYSNAME returns the
    // sysfs name (e.g. "input46"), NOT the devtmpfs eventN — these two
    // numbering schemes are independent (the sysfs name matches the input
    // device count, the devtmpfs name matches devtmpfs's allocation order).
    // touch_reader matches by readlink-ing /sys/class/input/eventN and
    // comparing the basename against this string.
    char sysname[64] = {};
    if (ioctl(g_outputFd, UI_GET_SYSNAME(sizeof(sysname) - 1), sysname) > 0) {
        strncpy(g_ourVirtualSysname, sysname, sizeof(g_ourVirtualSysname) - 1);
        g_ourVirtualSysname[sizeof(g_ourVirtualSysname) - 1] = '\0';
        snprintf(g_ourVirtualPath, sizeof(g_ourVirtualPath), "/sys/class/input/%s", sysname);
        LOGD("uinput created: sysname=%s classPath=%s", g_ourVirtualSysname, g_ourVirtualPath);
    } else {
        LOGE("UI_GET_SYSNAME failed errno=%d — virtual device path not tracked", errno);
    }

    LOGD("uinput created: name='%s' bus=0x%x vid=0x%x pid=0x%x pressure=%d major=%d",
         dev.name, dev.id.bustype, dev.id.vendor, dev.id.product, g_pressureMax, g_touchMajorMax);
    return true;
}

/// Lifts everything, one frame. Caller must hold g_mutex.
///
/// Used before the virtual device is destroyed and before the backend is
/// swapped. A device that disappears while a slot is down leaves that pointer
/// pressed on the far side for as long as the app believes it exists, and on
/// the InputManager path there is no device teardown that would end the gesture
/// implicitly — the MotionEvent stream has to say so itself.
///
/// The call is unconditional rather than "only if something was down": the
/// other side's idea of what is held is its own, and the one time it disagrees
/// is exactly the one time this needs to speak.
void liftAllLocked() {
    for (int i = 0; i < kMaxFingers; ++i) g_fingers[i].isDown = false;
    g_takeoverSlot    = -1;
    g_takeoverPanelId = -1;
    g_takeoverSeen    = false;
    upload();
}

/// Destroys the virtual device. Caller must hold g_mutex.
void closeLocked() {
    if (!g_initialized && g_outputFd < 0) return;

    // Logged every time, including when it is invoked implicitly by another
    // call (uinput_init calls this first). A device silently losing its state
    // mid-session used to be invisible: nothing said when, or why, and the
    // symptom was just "touch stopped somewhere further down".
    LOGI("uinput teardown: initialized=%d fd=%d frames=%ld restates=%ld failures=%d "
         "path='%s'",
         g_initialized ? 1 : 0, g_outputFd, g_uploadFrames, g_restateFrames,
         g_writeFailures,
         g_ourVirtualPath[0] != '\0' ? g_ourVirtualPath : "(none)");

    if (g_outputFd >= 0) {
        ioctl(g_outputFd, UI_DEV_DESTROY);
        close(g_outputFd);
        g_outputFd = -1;
    }
    g_ourVirtualPath[0] = '\0';
    g_ourVirtualSysname[0] = '\0';
    memset(g_buffer.events, 0, sizeof(g_buffer.events));
    memset(g_fingers, 0, sizeof(g_fingers));
    memset(g_uploaded, 0, sizeof(g_uploaded));
    // Not memset: the empty value is kNoPanelId, not 0. Zeroing would make every
    // slot claim to be carrying the finger whose tracking id is 0.
    for (int& panelId : g_panelIds) panelId = kNoPanelId;
    // The takeover has to die with the device: its id would otherwise survive
    // into the next device's life and make the mirror silently skip whichever
    // fresh finger happened to be handed that same panel tracking id.
    g_takeoverSlot    = -1;
    g_takeoverPanelId = -1;
    g_takeoverSeen    = false;
    g_initialized = false;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

extern "C" void uinput_set_source_panel(const char* path) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!path) {
        g_sourcePanel[0] = '\0';
        return;
    }
    strncpy(g_sourcePanel, path, sizeof(g_sourcePanel) - 1);
    g_sourcePanel[sizeof(g_sourcePanel) - 1] = '\0';
}

extern "C" bool uinput_init(int screenW, int screenH) {
    if (screenW <= 0 || screenH <= 0) {
        LOGE("uinput_init: invalid screen size %dx%d", screenW, screenH);
        return false;
    }

    std::lock_guard<std::mutex> guard(g_mutex);
    closeLocked();

    g_screenW = screenW;
    g_screenH = screenH;

    // Discover the panel so we can clone it and know its coordinate range.
    // Prefer the panel the caller already opened (the reader holds it grabbed,
    // so its path is authoritative); fall back to `getevent -p`.
    int panelMaxX = screenW;
    int panelMaxY = screenH;
    char panelPath[256] = "";
    if (g_sourcePanel[0] != '\0') {
        strncpy(panelPath, g_sourcePanel, sizeof(panelPath) - 1);
        int probe = open(panelPath, O_RDONLY);
        if (probe >= 0) {
            input_absinfo ix{}, iy{};
            if (ioctl(probe, EVIOCGABS(ABS_MT_POSITION_X), &ix) == 0 && ix.maximum > 0)
                panelMaxX = ix.maximum;
            if (ioctl(probe, EVIOCGABS(ABS_MT_POSITION_Y), &iy) == 0 && iy.maximum > 0)
                panelMaxY = iy.maximum;
            close(probe);
            LOGD("source panel %s ABS %dx%d", panelPath, panelMaxX, panelMaxY);
        } else {
            LOGE("source panel %s not readable errno=%d", panelPath, errno);
            panelPath[0] = '\0';
        }
    }
    if (panelPath[0] == '\0') {
        if (detectTouchDeviceViaGetevent(panelPath, sizeof(panelPath), panelMaxX, panelMaxY)) {
            LOGD("using touch device ABS %dx%d", panelMaxX, panelMaxY);
        } else {
            LOGD("no touch device detected, using screen size as ABS %dx%d", panelMaxX, panelMaxY);
        }
    }

    // Open the panel read-only purely to clone its identity and capability set.
    int cloneFd = -1;
    if (panelPath[0] != '\0') cloneFd = open(panelPath, O_RDONLY);
    if (cloneFd < 0) LOGD("panel %s not readable, cloning skipped", panelPath);

    bool created = createUinputDevice(panelMaxX, panelMaxY, cloneFd);
    if (cloneFd >= 0) close(cloneFd);
    if (!created) {
        closeLocked();
        return false;
    }

    // The panel's ABS range maps onto the *portrait* dimension of the display,
    // so the mapping survives rotation without touching the device itself.
    int portraitW = screenW < screenH ? screenW : screenH;
    int portraitH = screenW < screenH ? screenH : screenW;
    g_landscape = screenW > screenH;
    g_scaleX = static_cast<float>(panelMaxX) / static_cast<float>(portraitW > 0 ? portraitW : 1);
    g_scaleY = static_cast<float>(panelMaxY) / static_cast<float>(portraitH > 0 ? portraitH : 1);

    g_initialized = true;
    // Everything a remote report needs to reconstruct this device, at INFO so
    // it survives a `*：I` filter: compare it against the tester's
    // `getevent -p` and we learn both whether our clone came up and what it
    // cloned. Panels whose ABS range is a multiple of the pixel size (MTK ships
    // 100x) are the ones where a wrong scale shows up as a badly offset touch.
    LOGI("uinput ready: screen=%dx%d landscape=%d panel=%dx%d scale=%.3f,%.3f "
         "slots=%d clone='%s' sysname='%s' fd=%d abs=[%s]",
         screenW, screenH, g_landscape ? 1 : 0, panelMaxX, panelMaxY,
         g_scaleX, g_scaleY, kMaxFingers,
         panelPath[0] != '\0' ? panelPath : "(none)",
         g_ourVirtualSysname[0] != '\0' ? g_ourVirtualSysname : "(none)",
         g_outputFd, g_declaredAbs);
    return true;
}

extern "C" void uinput_close(void) {
    LOGI("uinput_close requested (was initialized=%d fd=%d)", g_initialized ? 1 : 0, g_outputFd);
    std::lock_guard<std::mutex> guard(g_mutex);
    // Lift first, destroy second. The other order would tear the device out
    // from under whatever it was holding, and every pointer still down at that
    // moment stays down as far as the app underneath is concerned.
    liftAllLocked();
    closeLocked();
}

// ── Touch-fusion takeover ─────────────────────────────────────────────────────
//
// Aim "touch fusion" lets the aim loop drive a REAL finger (the one the player is
// holding inside the touch area) instead of spawning a second, separate synthetic
// finger. To do that without two writers fighting over one slot, aim reserves the
// mirror slot that currently carries the chosen physical finger here, then drives
// it directly. uinput_mirror_physical() stops touching that slot for as long as
// it is reserved, so there is exactly one writer — no double-touch flicker.

extern "C" int uinput_takeover_physical_id(int id) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!backendReadyLocked()) return -1;
    // Matched against g_panelIds, NOT g_fingers[].id: the frame carries a
    // synthetic id per slot (see the note on g_panelIds), so the panel tracking
    // id the caller has in hand only ever appears there.
    for (int i = 0; i < UINPUT_MIRROR_SLOTS; ++i) {
        if (g_fingers[i].isDown && g_panelIds[i] == id) {
            g_takeoverSlot    = i;
            g_takeoverPanelId = id;
            g_takeoverSeen    = true;   // it is down right now by definition
            return i;
        }
    }
    g_takeoverSlot    = -1;
    g_takeoverPanelId = -1;
    return -1;
}

extern "C" void uinput_release_takeover(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    // Hand the slot back to the mirror by lifting it immediately.
    //
    // This is unconditional — we lift even when the finger is still physically
    // down. The alternative (keep the slot down and let the next mirror frame
    // snap it to the real finger) makes the pointer slide continuously from the
    // aim target to the player's thumb, i.e. a visible screen jump. Lifting first
    // makes the pointer blink out and re-appear at the real position one frame
    // later — a 1-frame flicker, but no teleport. That is the behaviour the
    // pre-regression build had, and it is what we want.
    //
    // The reservation (`g_panelIds`) survives the lift, so the finger is
    // re-pressed in the same slot, under the same wire id the app already knows.
    if (g_takeoverSlot >= 0 && g_takeoverSlot < kMaxFingers &&
        g_fingers[g_takeoverSlot].isDown) {
        g_fingers[g_takeoverSlot].isDown = false;
        upload();
    }
    g_takeoverSlot    = -1;
    g_takeoverPanelId = -1;
    g_takeoverSeen    = false;
}

extern "C" bool uinput_is_ready(void) { return g_initialized; }

extern "C" const char* uinput_get_our_path(void) {
    // Read without locking: a half-updated string is "old path" or "empty",
    // both harmless — and we never write into the middle of the buffer.
    return g_ourVirtualPath;
}

extern "C" const char* uinput_get_our_sysname(void) {
    // The sysfs name (e.g. "input46") that touch_reader uses to recognise
    // our virtual device — see uinput_get_our_path() for the threading note.
    return g_ourVirtualSysname;
}

// ── Backend selection ────────────────────────────────────────────────────────
//
// See input/inject_backend.h for the two backends and why switching between
// them has to be destructive. What is worth repeating here is the order of the
// three steps, because two of them are irreversible and one of them can fail:
//
//   1. lift everything the CURRENT backend is holding, while it is still alive
//      enough to carry the lift (a uinput frame with every TRACKING_ID back to
//      -1, or an empty pointer set for InputManager);
//   2. tear the current backend down — UI_DEV_DESTROY and close for uinput,
//      nothing at all for InputManager, whose whole existence is a Java object;
//   3. bring the new one up, and report whether it came up.
//
// The reporting is not decoration. If InputManager is selected and the platform
// refuses to let us inject, the correct outcome is a loud one-line failure and
// a phone whose menu still works but whose touches do not arrive — NOT a silent
// slide back to uinput, because that would hide the fault behind a symptom that
// disappears, which is the exact failure this project keeps running into.

extern "C" int inject_set_backend(int backend) {
    if (backend != INJECT_BACKEND_UINPUT && backend != INJECT_BACKEND_INPUT_MANAGER) {
        LOGE("inject: refusing unknown backend %d", backend);
        return 0;
    }

    int screenW = 0;
    int screenH = 0;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (g_backend == backend) {
            if (backendReadyLocked()) return 1;
            // Selected already, but not usable. Retry rather than keep reporting
            // the same failure: the reason it failed can change without the
            // selection changing — the usual one is `INJECT_EVENTS`, which is a
            // settings toggle the user may have just flipped.
            if (backend == INJECT_BACKEND_INPUT_MANAGER) {
                g_imReady = aimbotng::input::imBridgeInit();
                return g_imReady ? 1 : 0;
            }
            return 0;
        }

        screenW = g_screenW;
        screenH = g_screenH;

        // Step 1 + 2, for the backend we are leaving.
        liftAllLocked();
        if (g_backend == INJECT_BACKEND_UINPUT) {
            closeLocked();          // UI_DEV_DESTROY + close(fd), logged
        } else {
            g_imReady = false;
        }

        g_backend = backend;
        g_imReady = false;
    }

    if (backend == INJECT_BACKEND_INPUT_MANAGER) {
        // Step 3. Off the lock: this one touches the JVM, and the first call
        // into it may have to attach the calling thread.
        g_imReady = aimbotng::input::imBridgeInit();
        const char* err = aimbotng::input::imBridgeLastError();
        if (g_imReady) {
            LOGI("inject: backend = InputManager (uinput device destroyed)");
        } else {
            LOGE("inject: backend = InputManager but it is NOT usable: %s "
                 "— the virtual touchscreen has already been destroyed, so "
                 "nothing will be injected until this is fixed",
                 (err && err[0]) ? err : "unknown reason");
        }
        return g_imReady ? 1 : 0;
    }

    // Back to uinput: rebuild the device. It has to be rebuilt rather than
    // resurrected — the kernel destroyed it step 2 — and the screen size is the
    // one that was in force, so a device brought back after a rotation still
    // maps the same pixels.
    if (screenW <= 0 || screenH <= 0) {
        LOGE("inject: cannot rebuild the uinput device, screen size unknown");
        return 0;
    }
    if (!uinput_init(screenW, screenH)) {
        LOGE("inject: uinput device could not be rebuilt (%dx%d)", screenW, screenH);
        return 0;
    }
    LOGI("inject: backend = uinput (device rebuilt %dx%d)", screenW, screenH);
    return uinput_is_ready() ? 1 : 0;
}

extern "C" int inject_get_backend(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_backend;
}

extern "C" int inject_is_ready(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    return backendReadyLocked() ? 1 : 0;
}

extern "C" void inject_release_all(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    liftAllLocked();
}

extern "C" int uinput_write_failures(void) {    std::lock_guard<std::mutex> guard(g_mutex);
    return g_writeFailures;
}

extern "C" int uinput_get_fd(void) { return g_outputFd; }

extern "C" void uinput_set_screen_params(int w, int h, bool landscape) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (w > 0) g_screenW = w;
    if (h > 0) g_screenH = h;
    g_landscape = landscape;
}

extern "C" void uinput_down(int slot, int id, int screenX, int screenY) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!backendReadyLocked() || slot < 0 || slot >= kMaxFingers) return;

    float tx, ty;
    screenToTouch(screenX, screenY, tx, ty);
    g_fingers[slot].id = id;
    g_fingers[slot].extId = id;   // the caller's id already identifies the finger
    g_fingers[slot].pos = Vec2(tx, ty);
    g_fingers[slot].sPos = Vec2(static_cast<float>(screenX), static_cast<float>(screenY));
    g_fingers[slot].isDown = true;
    upload();
}

extern "C" void uinput_move(int slot, int screenX, int screenY) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!backendReadyLocked() || slot < 0 || slot >= kMaxFingers) return;
    if (!g_fingers[slot].isDown) return;

    float tx, ty;
    screenToTouch(screenX, screenY, tx, ty);
    g_fingers[slot].pos = Vec2(tx, ty);
    g_fingers[slot].sPos = Vec2(static_cast<float>(screenX), static_cast<float>(screenY));
    upload();
}

extern "C" void uinput_up(int slot) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!backendReadyLocked() || slot < 0 || slot >= kMaxFingers) return;
    if (!g_fingers[slot].isDown) return;

    g_fingers[slot].isDown = false;
    upload();
}

extern "C" void uinput_mirror_physical(const int* ids, const int* rawXs, const int* rawYs,
                                      const int* screenXs, const int* screenYs, int n) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!backendReadyLocked()) return;
    if (n < 0) n = 0;
    if (n > UINPUT_MIRROR_SLOTS) n = UINPUT_MIRROR_SLOTS;

    const int takeoverId = g_takeoverPanelId;   // -1 when aim owns no real finger
    g_takeoverSeen = false;   // set below when the driven finger is still there

    // Slot assignment: a finger keeps the slot it already owns.
    //
    // aimbot 1.2.1 gets this for free — it indexes its finger array with the
    // panel's own slot number, so a finger's slot is a property of the finger and
    // cannot change while it is down. That is exactly why 1.2.1 is safe here and
    // why copying only its frame format was not enough: we are handed a
    // COMPACTED list, so the same property has to be reconstructed. `g_panelIds`
    // records who owns each slot, and a finger that is still down is always put
    // back where it was; a slot changes hands only when its owner is gone.
    //
    // Position-based assignment is the fault this replaces. With two fingers
    // down, the moment the first one lifts the second finger's list index
    // becomes 0 — so its position is written into slot 0, whose constant wire
    // id is still live, and slot 1 (a different wire id, the one the app was
    // tracking for that finger) is lifted. The app is told pointer 10 slid from
    // the lifted finger's position to the surviving one's and pointer 11
    // vanished: two pointers, one finger, swapping places. Slot mixing.
    //
    // EXCEPT for the finger aim has taken over. There is only ever ONE writer
    // per slot: while `takeoverId` is set, this function must neither move that
    // finger nor lift it — the aim loop owns its position, and its `isDown` was
    // already reported by us. Two writers on one slot is the same fault by
    // another route (the slot alternating between the finger's position and the
    // aim target's), which is why the takeover exists at all.
    bool claimed[UINPUT_MIRROR_SLOTS] = {};

    // Ownership, not occupancy: a slot is its owner's for as long as the owner is
    // down, and a slot whose owner has been momentarily lifted (see
    // uinput_release_takeover) is still reserved. Pressing that finger again
    // therefore lands it back in the slot it had, under the wire id it had.
    auto ownerOf = [&](int panelId) -> int {
        if (panelId == kNoPanelId) return -1;
        for (int k = 0; k < UINPUT_MIRROR_SLOTS; ++k) {
            if (g_panelIds[k] == panelId) return k;
        }
        return -1;
    };

    // Pass 1 — a finger that still has a slot keeps it; only its position is
    // refreshed. Nothing about its identity changes, so no TRACKING_ID is
    // re-sent and the app sees a pointer that moved rather than one that was
    // replaced.
    for (int i = 0; i < n; ++i) {
        const int k = ownerOf(ids[i]);
        if (k < 0 || claimed[k]) continue;   // claimed: the same id listed twice
        claimed[k] = true;
        // Aim owns this finger's position for as long as it holds it.
        if (takeoverId >= 0 && ids[i] == takeoverId) {
            g_takeoverSeen = true;
            continue;
        }

        if (!g_fingers[k].isDown) {
            // Reserved, but not currently pressed — the hand-off back from aim.
            // Re-press it under the id it already had.
            g_fingers[k].isDown = true;
            g_uploaded[k]       = false;   // it was lifted, so TRACKING_ID again
        }
        g_fingers[k].pos  = Vec2(static_cast<float>(rawXs[i]),
                                 static_cast<float>(rawYs[i]));
        g_fingers[k].sPos = Vec2(static_cast<float>(screenXs[i]),
                                 static_cast<float>(screenYs[i]));
    }

    // Pass 2 — a slot whose finger is gone is lifted and returned to the pool.
    // Never the slot aim is driving: its release belongs to
    // uinput_release_takeover(). The aim and trigger slots live outside
    // UINPUT_MIRROR_SLOTS, so they are never touched here either.
    //
    // This runs before allocation on purpose. "Which slots are free" is
    // completely determined by the incoming list, so reaping first means a slot
    // freed this frame is immediately available to a finger arriving in the same
    // frame — and, more importantly, that a slot which is merely reserved (its
    // owner momentarily lifted by a takeover hand-off) is not stolen by the new
    // finger, which would cost the returning finger its wire id.
    for (int k = 0; k < UINPUT_MIRROR_SLOTS; ++k) {
        if (claimed[k]) continue;
        if (takeoverId >= 0 && g_panelIds[k] == takeoverId) continue;
        g_fingers[k].isDown = false;
        g_panelIds[k]       = kNoPanelId;
    }

    // Pass 3 — a finger that was not down before takes the lowest free slot.
    for (int i = 0; i < n; ++i) {
        if (ownerOf(ids[i]) >= 0) continue;
        // A takeover only ever names a finger that is already down, so this is
        // a cannot-happen; leaving the slot untouched is still the right answer.
        if (takeoverId >= 0 && ids[i] == takeoverId) continue;

        int k = -1;
        for (int c = 0; c < UINPUT_MIRROR_SLOTS; ++c) {
            if (g_fingers[c].isDown) continue;   // reserved or live
            k = c;
            break;
        }
        if (k < 0) continue;   // more physical fingers than there are mirror slots

        g_panelIds[k]        = ids[i];
        g_fingers[k].id      = kMirrorIdBase + k;
        g_fingers[k].extId   = mirrorExtId(ids[i]);
        g_fingers[k].pos     = Vec2(static_cast<float>(rawXs[i]),
                                    static_cast<float>(rawYs[i]));
        g_fingers[k].sPos    = Vec2(static_cast<float>(screenXs[i]),
                                    static_cast<float>(screenYs[i]));
        g_fingers[k].isDown  = true;
        g_uploaded[k]        = false;   // force TRACKING_ID on the next frame
    }

    upload();
}

extern "C" void uinput_mirror_slots(const int* slotIds, const int* rawXs, const int* rawYs,
                                    const int* screenXs, const int* screenYs, int maxSlots) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!backendReadyLocked()) return;
    if (!slotIds || !rawXs || !rawYs || !screenXs || !screenYs) return;
    if (maxSlots < 0) maxSlots = 0;
    if (maxSlots > kMaxFingers) maxSlots = kMaxFingers;
    const int limit = (maxSlots < UINPUT_MIRROR_SLOTS) ? maxSlots : UINPUT_MIRROR_SLOTS;
    const int takeoverId = g_takeoverPanelId;
    g_takeoverSeen = false;
    // Direct 1:1 mapping: panel slot k -> virtual slot k (old touch_core.cpp model).
    // No compact allocation, so lift and down never share one wire id in one SYN_REPORT.
    for (int k = 0; k < UINPUT_MIRROR_SLOTS; ++k) {
        const bool hasFinger = (k < limit && slotIds[k] != kNoPanelId && slotIds[k] >= 0);
        if (hasFinger && takeoverId >= 0 && slotIds[k] == takeoverId) {
            g_takeoverSeen = true;
            continue;
        }
        if (takeoverId >= 0 && g_panelIds[k] == takeoverId) continue;
        if (hasFinger) {
            const int panelId = slotIds[k];
            if (!g_fingers[k].isDown) g_uploaded[k] = false;
            // Keep g_panelIds in sync for takeover lookup.
            g_panelIds[k] = panelId;
            g_fingers[k].id = kMirrorIdBase + k;
            g_fingers[k].extId = mirrorExtId(panelId);
            g_fingers[k].pos = Vec2(static_cast<float>(rawXs[k]), static_cast<float>(rawYs[k]));
            g_fingers[k].sPos = Vec2(static_cast<float>(screenXs[k]), static_cast<float>(screenYs[k]));
            g_fingers[k].isDown = true;
        } else {
            g_fingers[k].isDown = false;
            g_panelIds[k] = kNoPanelId;
        }
    }
    // Clear any excess mirror slots beyond limit (should already be handled).
    upload();
}

extern "C" void uinput_mirror_clear(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!backendReadyLocked()) return;

    // A swallowed gesture clears the mirror, but it must not clear the finger
    // aim is driving: that pointer belongs to aim for the whole time it is
    // around, and lifting it here would make the aim point blink out and be
    // re-pressed — the same flicker, by a different route. aim's own
    // uinput_release_takeover() is what ends it.
    const int takeoverId = g_takeoverPanelId;

    bool any = false;
    for (int i = 0; i < UINPUT_MIRROR_SLOTS; ++i) {
        if (takeoverId >= 0 && g_panelIds[i] == takeoverId) continue;
        if (g_fingers[i].isDown) {
            g_fingers[i].isDown = false;
            any = true;
        }
        // Hand the slot back even when it was already up: a cleared gesture
        // ends every claim, and a stale owner would make the next finger with
        // that id look like it was still here — the mirror would then move it
        // instead of pressing it, so it would never be announced at all.
        g_panelIds[i] = kNoPanelId;
    }
    if (any) upload();
}
