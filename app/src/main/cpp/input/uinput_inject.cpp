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

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include <android/log.h>

#define LOG_TAG "AimbotInput"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kMaxFingers = 10;
constexpr int kMaxEvents = 512;

// A rejected uinput write is retried this many times, kWriteSpinUs apart, before
// the frame is declared lost. The input core drains independently of us, so a
// short wait is normally all an EAGAIN needs.
constexpr int kMaxWriteSpins = 20;
constexpr int kWriteSpinUs   = 250;

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float px, float py) : x(px), y(py) {}
};

/// One finger slot on the virtual device.
struct TouchObj {
    Vec2 pos{};
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

// "Touch fusion" takeover: while >= 0, uinput_mirror_physical() leaves this
// mirror slot alone (neither re-updates nor lifts it) so the aim loop can drive
// the real finger that lives there directly. Set by uinput_takeover_physical_id()
// (aim finds the physical finger in the touch area and reserves its slot) and
// cleared by uinput_release_takeover(). Keeping the slot down while aim owns it
// avoids the down/up/down flicker that two writers on one slot would cause.
int g_takeoverSlot = -1;

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

// ── Frame writer ─────────────────────────────────────────────────────────────

/// Emits one complete multitouch frame describing every finger slot.
///
/// The frame leaves as one `write`, and that write is the only thing keeping the
/// kernel's copy of the touch state in step with g_fingers / g_uploaded. Two
/// ways it used to lose that step, both with the same consequence:
///
///   * the fd is O_NONBLOCK, so a full input-core buffer answers EAGAIN and the
///     entire frame was dropped — including the TRACKING_ID events that begin
///     and end pointers;
///   * a short write left the frame unterminated, so its tail (BTN_TOUCH, the
///     TOOL bits and the terminating SYN_REPORT) never landed.
///
/// Either one desyncs the two sides permanently, because g_uploaded[] was
/// updated whether or not the bytes went out: the next frame then believes the
/// kernel already knows a finger's tracking id and does not re-send it, so every
/// frame from then on is a delta against a state the device never had. The panel
/// is grabbed by this process, so "the device stopped understanding touch" means
/// the whole phone stops responding until the daemon restarts — which is what a
/// sustained swipe, the highest event rate this path ever sees, could trigger.
///
/// So the write is retried through EAGAIN and continued through short writes,
/// and a frame that ultimately fails raises g_forceRestate so that the next one
/// re-states everything rather than building on a frame that never arrived.
void upload() {
    if (g_outputFd < 0) return;

    int count = 0;
    int active = 0;
    bool anyDown = false;

    // A restate frame ignores what the kernel is believed to know: every slot
    // gets its identity again, up or down, which is what resynchronises the two
    // sides after a lost write.
    const bool restate = g_forceRestate;

    for (int fi = 0; fi < kMaxFingers; ++fi) {
        const TouchObj& finger = g_fingers[fi];
        // What the kernel was last told about this slot. A restate frame
        // deliberately reports "nothing known", so every slot is described
        // again from scratch.
        const bool kernelHolds = g_uploaded[fi] && !restate;

        if (finger.isDown) {
            anyDown = true;
            ++active;
            count = pushEvent(count, EV_ABS, ABS_MT_SLOT, fi);
            if (!kernelHolds) count = pushEvent(count, EV_ABS, ABS_MT_TRACKING_ID, finger.id);
            count = pushEvent(count, EV_ABS, ABS_MT_POSITION_X, static_cast<int>(finger.pos.x));
            count = pushEvent(count, EV_ABS, ABS_MT_POSITION_Y, static_cast<int>(finger.pos.y));
            // Real fingers report pressure / contact area every frame, within a
            // small slice of the panel's reported max.
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
        } else if (g_uploaded[fi] || restate) {
            // ↑ `g_uploaded[fi] || restate`, NOT `!kernelHolds`.
            //
            // The kernel is still holding this slot down (or we are restating
            // blind, having lost a frame and learned nothing from it), so the
            // pointer has to be told to end. Testing the *negation* here did the
            // exact opposite — it ended pointers the kernel never had and left
            // every real lift unsent, so one pointer per touch stayed stuck in
            // the kernel forever. The physical panel is grabbed, so a stuck
            // pointer is the whole phone: nothing else can deliver touch until
            // the daemon restarts.
            count = pushEvent(count, EV_ABS, ABS_MT_SLOT, fi);
            count = pushEvent(count, EV_ABS, ABS_MT_TRACKING_ID, -1);
            g_uploaded[fi] = false;
        }
    }

    count = pushEvent(count, EV_KEY, BTN_TOUCH, anyDown ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_FINGER, active == 1 ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_DOUBLETAP, active == 2 ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_TRIPLETAP, active == 3 ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_QUADTAP, active == 4 ? 1 : 0);
    count = pushEvent(count, EV_KEY, BTN_TOOL_QUINTTAP, active >= 5 ? 1 : 0);
    count = pushEvent(count, EV_SYN, SYN_REPORT, 0);

    const size_t total = sizeof(input_event) * static_cast<size_t>(count);
    const auto*  bytes = reinterpret_cast<const uint8_t*>(g_buffer.events);

    size_t written = 0;
    int    spins   = 0;
    while (written < total) {
        const ssize_t n = write(g_outputFd, bytes + written, total - written);
        if (n > 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++spins > kMaxWriteSpins) break;
            usleep(kWriteSpinUs);
            continue;
        }
        break;  // EPIPE / ENODEV / anything else — the device is gone
    }

    if (written == total) {
        // The two sides agree, so the next frame may go back to being a delta.
        g_forceRestate = false;
        if (g_writeFailures > 0) {
            LOGD("upload: resynced after %d failed frame(s)", g_writeFailures);
            g_writeFailures = 0;
        }
        return;
    }

    // The frame did not land, and how much of it did is not knowable — the
    // kernel may hold a prefix of it. Nothing may be assumed from here on, so
    // the next frame restates every slot rather than compounding the damage.
    g_forceRestate = true;
    if (++g_writeFailures == 1 || (g_writeFailures % 120) == 0) {
        LOGE("upload: short write (%zu/%zu bytes) errno=%d — restating next frame "
             "(failures=%d)", written, total, errno, g_writeFailures);
    }
}

// ── Physical panel discovery ─────────────────────────────────────────────────

/// Finds the real touchscreen by parsing `getevent -p`. Doing it through
/// getevent avoids needing a direct /dev/input fd just to enumerate devices.
/// Fills out the path and the panel's coordinate range.
bool detectTouchDeviceViaGetevent(char* outPath, size_t pathSize, int& outMaxX, int& outMaxY) {
    FILE* fp = popen("/system/bin/getevent -p 2>&1", "r");
    if (!fp) {
        LOGE("detectTouchDevice: popen getevent failed");
        return false;
    }

    char line[512];
    char currentPath[256] = "";
    bool hasSlot = false, hasX = false, hasY = false;
    int maxX = 0, maxY = 0;
    int deviceCount = 0;
    bool found = false;

    auto tryCommit = [&]() -> bool {
        if (deviceCount > 0 && hasSlot && hasX && hasY) {
            strncpy(outPath, currentPath, pathSize - 1);
            outPath[pathSize - 1] = '\0';
            outMaxX = maxX > 0 ? maxX : 0;
            outMaxY = maxY > 0 ? maxY : 0;
            LOGD("detected touch device: %s abs=%dx%d", outPath, outMaxX, outMaxY);
            return true;
        }
        return false;
    };

    while (fgets(line, sizeof(line), fp)) {
        // New device section: "add device N: /dev/input/eventX"
        if (strstr(line, "add device") && strstr(line, "/dev/input/event")) {
            if (tryCommit()) { found = true; break; }

            char* p = strstr(line, "/dev/input/event");
            if (p) {
                char* end = p;
                while (*end && *end != '\n' && *end != '\r' && *end != ' ') end++;
                size_t len = static_cast<size_t>(end - p);
                if (len >= sizeof(currentPath)) len = sizeof(currentPath) - 1;
                memcpy(currentPath, p, len);
                currentPath[len] = '\0';
            }
            deviceCount++;
            hasSlot = hasX = hasY = false;
            maxX = maxY = 0;
        }
        // Skip a virtual device we may have created earlier.
        if (strstr(line, "name:") && strstr(line, "Aimbot")) {
            hasSlot = hasX = hasY = false;
            maxX = maxY = 0;
        }
        // 002f=ABS_MT_SLOT(47), 0035=ABS_MT_POSITION_X(53), 0036=ABS_MT_POSITION_Y(54).
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
    }
    if (!found) found = tryCommit();

    pclose(fp);
    return found;
}

// ── Virtual device creation ──────────────────────────────────────────────────

/// Registers the virtual touchscreen, cloning as much of the real panel's
/// identity and capability set as we can read from `sourceFd` (may be -1).
bool createUinputDevice(int screenX, int screenY, int sourceFd) {
    uinput_user_dev dev{};
    g_outputFd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_outputFd < 0) {
        LOGE("open /dev/uinput failed errno=%d", errno);
        return false;
    }

    // ── Identity: clone the real panel so we appear as a genuine touchscreen ──
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
        dev.id.bustype = BUS_I2C;  // the usual bus for touch controllers
        dev.id.vendor = rand() % 10 + 5;
        dev.id.product = rand() % 10 + 5;
        dev.id.version = rand() % 10 + 5;
    }

    // ── Properties (INPUT_PROP_DIRECT / POINTER / ...) ──
    bool clonedProps = false;
    if (sourceFd >= 0) {
        uint8_t propBits[64]{};
        ssize_t propRes = ioctl(sourceFd, EVIOCGPROP(sizeof(propBits)), propBits);
        if (propRes > 0) {
            int n = static_cast<int>(propRes) < static_cast<int>(sizeof(propBits))
                        ? static_cast<int>(propRes)
                        : static_cast<int>(sizeof(propBits));
            for (int j = 0; j < n; ++j)
                for (int k = 0; k < 8; ++k)
                    if (propBits[j] & (1 << k)) ioctl(g_outputFd, UI_SET_PROPBIT, j * 8 + k);
            clonedProps = true;
        }
    }
    if (!clonedProps) ioctl(g_outputFd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    ioctl(g_outputFd, UI_SET_EVBIT, EV_ABS);
    ioctl(g_outputFd, UI_SET_EVBIT, EV_SYN);
    ioctl(g_outputFd, UI_SET_EVBIT, EV_KEY);

    // Mandatory axes. ABS_X / ABS_Y are deliberately NOT registered: real panels
    // here report coordinates via ABS_MT_POSITION_X/Y only, and a ghost ABS_X/Y
    // axis is a strong uinput tell.
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(g_outputFd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);

    char randomPhys[16]{};
    genRandomString(randomPhys, sizeof(randomPhys));
    ioctl(g_outputFd, UI_SET_PHYS, randomPhys);

    // ── Clone every ABS axis (incl. pressure / touch major / width) preserving
    // min/max/fuzz/flat, so we advertise the same capability set as the panel. ──
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

    // ── Clone the panel's KEY capabilities verbatim (no hardcoded BTN_TOOL_*),
    // falling back to the minimum multitouch set when there is no source. ──
    uint8_t* keyBits = nullptr;
    ssize_t keySize = 0;
    int keyRes = 0;
    bool clonedKeys = false;
    if (sourceFd >= 0) {
        while (true) {
            keyRes = ioctl(sourceFd, EVIOCGBIT(EV_KEY, keySize), keyBits);
            if (keyRes < keySize) break;
            keySize = keyRes + 16;
            keyBits = static_cast<uint8_t*>(realloc(keyBits, keySize * 2));
        }
        for (int j = 0; j < keyRes; ++j)
            for (int k = 0; k < 8; ++k)
                if (keyBits[j] & (1 << k)) ioctl(g_outputFd, UI_SET_KEYBIT, j * 8 + k);
        clonedKeys = keyRes > 0;
    }
    free(keyBits);
    if (!clonedKeys) {
        ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOUCH);
        ioctl(g_outputFd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    }

    // Pin the axes we drive. SLOT keeps whatever the panel reported; the
    // remaining ranges are set explicitly as a safety net.
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

/// Destroys the virtual device. Caller must hold g_mutex.
void closeLocked() {
    if (!g_initialized && g_outputFd < 0) return;

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
    LOGD("uinput ready scale=%.3f,%.3f", g_scaleX, g_scaleY);
    return true;
}

extern "C" void uinput_close(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_takeoverSlot = -1;
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
    if (!g_initialized) return -1;
    for (int i = 0; i < UINPUT_MIRROR_SLOTS; ++i) {
        if (g_fingers[i].isDown && g_fingers[i].id == id) {
            g_takeoverSlot = i;
            return i;
        }
    }
    g_takeoverSlot = -1;
    return -1;
}

extern "C" void uinput_release_takeover(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    // Lift the taken-over slot immediately. When the real finger is already
    // physically lifted the mirror will never re-report it, so without this the
    // kernel keeps that slot stuck down as a phantom touch point — and the next
    // frame's synthetic press then produces a second, "extra" point. Sending the
    // up here guarantees the hand-off is always clean. If the finger is still
    // physically down the mirror re-presses it next frame; at most a one-frame
    // flicker, which is far better than a stuck ghost.
    if (g_takeoverSlot >= 0 && g_takeoverSlot < kMaxFingers) {
        if (g_fingers[g_takeoverSlot].isDown) {
            g_fingers[g_takeoverSlot].isDown = false;
            upload();
        }
    }
    g_takeoverSlot = -1;
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

extern "C" int uinput_write_failures(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
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
    if (!g_initialized || slot < 0 || slot >= kMaxFingers) return;

    float tx, ty;
    screenToTouch(screenX, screenY, tx, ty);
    g_fingers[slot].id = id;
    g_fingers[slot].pos = Vec2(tx, ty);
    g_fingers[slot].isDown = true;
    upload();
}

extern "C" void uinput_move(int slot, int screenX, int screenY) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized || slot < 0 || slot >= kMaxFingers) return;
    if (!g_fingers[slot].isDown) return;

    float tx, ty;
    screenToTouch(screenX, screenY, tx, ty);
    g_fingers[slot].pos = Vec2(tx, ty);
    upload();
}

extern "C" void uinput_up(int slot) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized || slot < 0 || slot >= kMaxFingers) return;
    if (!g_fingers[slot].isDown) return;

    g_fingers[slot].isDown = false;
    upload();
}

extern "C" void uinput_mirror_physical(const int* ids, const int* xs, const int* ys, int n) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized) return;
    if (n < 0) n = 0;
    if (n > UINPUT_MIRROR_SLOTS) n = UINPUT_MIRROR_SLOTS;

    const int takeover = g_takeoverSlot;   // -1 when aim is not driving a real finger

    // Slots are assigned BY FINGER ID, not by array position.
    //
    // Position-based assignment only works while the reader hands the fingers
    // over in a stable order. It does not for Protocol A — that protocol has no
    // slot and no guaranteed ordering, so when one of several fingers lifts,
    // the survivors shift down an index. The finger that used to be index 1
    // becomes index 0, inherits slot 0 (whose tracking id is only re-sent when
    // the slot goes down, see upload()) and the app underneath sees the pointer
    // that was already there teleport. The same happens on Protocol B when a
    // finger other than the last one lifts, because rebuildPointersLocked()
    // compacts the active slots.
    //
    // Keying on the id instead makes a finger keep its slot for its whole life,
    // so each one reads as one continuous press.
    bool slotTaken[UINPUT_MIRROR_SLOTS] = {};

    // Pass 1 — reserve the slot that currently carries each id still present.
    // Skip the finger whose slot aim has taken over (it belongs to aim now).
    for (int k = 0; k < n; ++k) {
        if (takeover >= 0 && g_fingers[takeover].isDown && g_fingers[takeover].id == ids[k])
            continue;
        for (int i = 0; i < UINPUT_MIRROR_SLOTS; ++i) {
            if (slotTaken[i]) continue;
            if (g_fingers[i].isDown && g_fingers[i].id == ids[k]) {
                slotTaken[i] = true;
                break;
            }
        }
    }

    // Pass 2 — place every finger, reusing a reserved slot when it has one and
    // taking the lowest free slot otherwise. Again skip the taken-over finger.
    for (int k = 0; k < n; ++k) {
        if (takeover >= 0 && g_fingers[takeover].isDown && g_fingers[takeover].id == ids[k])
            continue;
        int slot = -1;
        for (int i = 0; i < UINPUT_MIRROR_SLOTS; ++i) {
            if (slotTaken[i] && g_fingers[i].isDown && g_fingers[i].id == ids[k]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            for (int i = 0; i < UINPUT_MIRROR_SLOTS; ++i) {
                if (!slotTaken[i]) { slot = i; break; }
            }
        }
        if (slot < 0) break;  // more fingers than slots: drop the extras

        slotTaken[slot] = true;

        float tx, ty;
        screenToTouch(xs[k], ys[k], tx, ty);

        if (!g_fingers[slot].isDown) {
            g_fingers[slot].id = ids[k];
            g_fingers[slot].isDown = true;
        } else if (g_fingers[slot].id != ids[k]) {
            // The slot changed identity. Force upload() to re-send TRACKING_ID
            // so the app ends the old pointer and starts the new one instead of
            // silently teleporting it.
            g_fingers[slot].id = ids[k];
            g_uploaded[slot] = false;
        }
        g_fingers[slot].pos = Vec2(tx, ty);
    }

    // Pass 3 — whatever no finger claimed has been lifted. The taken-over slot
    // is left exactly as aim last set it (kept down, position untouched), so the
    // real finger does not flicker while aim drives it.
    for (int i = 0; i < UINPUT_MIRROR_SLOTS; ++i) {
        if (i == takeover) continue;
        if (!slotTaken[i]) g_fingers[i].isDown = false;
    }

    upload();
}

extern "C" void uinput_mirror_clear(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized) return;

    bool any = false;
    for (int i = 0; i < UINPUT_MIRROR_SLOTS; ++i) {
        if (g_fingers[i].isDown) {
            g_fingers[i].isDown = false;
            any = true;
        }
    }
    if (any) upload();
}
