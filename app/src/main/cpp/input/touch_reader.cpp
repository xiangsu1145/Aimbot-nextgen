// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng input — physical touchscreen reader (implementation)
//
//  Ported from G:\ai\Aimbot-ai\android-client\app\src\main\cpp\src\injection\
//  inputmgr_core.cpp. Kept: device discovery, EVIOCGRAB, Protocol A / Protocol
//  B decoding, rotation-aware coordinate mapping, finger mirroring. Dropped:
//  the license gate, the detection zones and the Shizuku-friendly getevent
//  enumeration path for the common case (we run in the shell domain, which can
//  read /dev/input directly — getevent remains as a fallback).
// ─────────────────────────────────────────────────────────────────────────────
#include "touch_reader.h"

#include "uinput_inject.h"

#include <dirent.h>
#include <unistd.h>

#include "inject_backend.h"
#include "panel_filter.h"
#include "uinput_inject.h"
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <android/log.h>

#define LOG_TAG "AimbotReader"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kMaxDevices   = 4;
constexpr int kMaxSlots     = READER_MAX_POINTERS;

// Protocol A panels frequently send no ABS_MT_TRACKING_ID at all, so a finger
// has no name of its own and we have to invent one. Invented ids live in a
// high, dedicated range: it keeps them clear of the small ids real drivers
// pick, and clear of UINPUT_ID_TRIGGER (2000).
constexpr int kSyntheticBase = 60000;
constexpr int kSyntheticMax  = 63999;

// Consecutive rejected uinput frames after which the mirror is considered dead
// and says so, loudly and repeatedly. It deliberately does NOT try to recover:
// the only recovery available here is releasing the grab, which would restore
// the touchscreen and with it the illusion that nothing is wrong.
constexpr int kAlarmFailures = 150;

// The ONE place where this reader still differs from aimbot 1.2.1, and the only
// one that is switchable at runtime.
//
// 1.2.1 grabs the panel and then mirrors every physical finger 1:1, always —
// it has no notion of a gesture that belongs to the overlay, because its
// overlay is a real WindowManager window that consumes the touch on its own.
// Ours is a SurfaceFlinger layer, invisible to the input system, so if a tap on
// the menu were also mirrored the app underneath would receive it too (in an
// FPS, tapping a slider would also fire). Swallowing the gestures that BEGAN
// inside a published menu rectangle is what replaces the window.
//
// It changes nothing for any gesture that starts outside the menu — those are
// mirrored raw, exactly as 1.2.1 does, whatever this is set to.
//
// Exposed in the menu as Settings → 允许触摸穿透: turning it on gives a
// byte-identical 1.2.1 mirror (every touch reaches the system, menu taps
// included), which is the setting to test with when the question is whether the
// swallow is what a given device is objecting to.
constexpr bool kSwallowMenuGesturesDefault = true;

// ── State ────────────────────────────────────────────────────────────────────

std::mutex g_mutex;

std::vector<int>  g_fds;          // open + grabbed panel fds
std::string       g_panelPath;    // first panel path (uinput clones from it)
bool              g_ready    = false;
bool              g_grabbed  = false;
bool              g_sink     = true;   // mirror physical fingers to uinput
/// Mirror gestures that began on the menu as well (true) or keep letting the
/// menu own them (false). Runtime-switchable: Settings → 允许触摸穿透.
bool              g_passThrough = !kSwallowMenuGesturesDefault;

/// The panel is always taken exclusively with EVIOCGRAB (the design). The old
/// runtime toggle ("独占触摸面板") was removed: the daemon is the phone's only
/// touch source and re-injects what it wants. There is no compatibility mode.

int g_displayW = 0, g_displayH = 0;    // current orientation
int g_portraitW = 0, g_portraitH = 0;  // min/max of the two
int g_rotation = 0;                    // 0..3

int g_maxX = 0, g_maxY = 0;            // panel ABS range (reference panel)
bool g_refHasSlot = false;             // reference panel protocol (diagnostics)

// Protocol of every open panel. Decoding is per device, because one phone can
// in principle expose both a type-A and a type-B node and each has to be parsed
// with its own rules. In practice all panels of a device share one protocol.
std::vector<bool> g_fdHasSlot;

// Static backing for the names/paths returned by enumeratePanelsDetailed.
// Keeps the const char* in [ReaderPanel] valid until the next enumeration,
// which the Touch page only triggers when it is opened, not every frame.
std::vector<std::string>  g_panelPaths;
std::vector<std::string>  g_panelNames;
std::vector<bool>         g_panelOwnVirtual;

// ── Protocol B (slot addressing) ─────────────────────────────────────────────
int g_slotId[kMaxSlots];
int g_slotX[kMaxSlots];
int g_slotY[kMaxSlots];
int g_curSlot = 0;

// ── Protocol A (position stream) ─────────────────────────────────────────────
//
// No ABS_MT_SLOT: each finger is terminated by SYN_MT_REPORT and the whole
// frame by SYN_REPORT, so every frame re-states the complete set of live
// fingers and "lifted" simply means "absent from the frame".
//
// That forces a two-buffer design. `build` collects the frame currently
// arriving; only when SYN_REPORT closes it is it promoted to `snap`. Everyone
// else reads `snap`, because the hit-test and the uinput mirror both run after
// decodeBatch() has returned — i.e. after the frame is already complete.
struct ProtoAPending {
    int  tid = -1;
    int  x = 0, y = 0;
    bool hasPos = false;
} g_pending;

struct ProtoAFrame {
    int  count = 0;
    int  id[kMaxSlots];
    int  x[kMaxSlots];
    int  y[kMaxSlots];
    bool explicitId[kMaxSlots];  // true when the device supplied the id itself
};

ProtoAFrame g_protoABuild;   // frame being assembled
ProtoAFrame g_protoASnap;    // last complete frame — what everyone else reads

bool g_sawMtReport  = false;  // SYN_MT_REPORT seen in the CURRENT frame
bool g_frameHadAbs  = false;  // any EV_ABS seen in the CURRENT frame
bool g_btnUp        = false;  // BTN_TOUCH 0 seen in the CURRENT frame
bool g_usesMtReport = false;  // device ever emitted SYN_MT_REPORT (sticky)

int g_protoANextId = kSyntheticBase;  // allocator for invented finger ids

// Current output set.
std::vector<ReaderPointer> g_pointers;

// Menu rectangles published by the overlay ([x, y, w, h] per entry, screen px).
int  g_regionRects[READER_MAX_REGIONS * 4] = {};
int  g_regionCount = 0;

// When the rectangles were last published.
//
// They are only ever valid while the menu that owns them is on screen; a menu
// that went away without clearing them (renderer died, layer dropped) would
// leave the mirror swallowing touches inside a layout nobody can see. The
// publisher refreshes them at 30 Hz, so anything older than this is a menu that
// is no longer there — and pass-through is the safe answer to a stale claim,
// because a swallowed touch is invisible while a leaked one is merely unwanted.
long long g_regionStampUs = 0;
constexpr long long kRegionStaleUs = 1'000'000;   // 1 s ≈ 30 missed refreshes

// Gesture-level swallow latch. A gesture is decided once, when its first finger
// lands, and keeps that decision until every finger is lifted.
bool g_gestureActive   = false;
bool g_gestureConsumed = false;

// ── Helpers ──────────────────────────────────────────────────────────────────

long long nowUs() {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<long long>(tv.tv_sec) * 1000000LL + tv.tv_usec;
}

/// Panel coordinates -> screen coordinates in the current orientation.
///
/// The panel reports in its own (usually portrait) axes, so we first scale onto
/// the portrait rectangle and then rotate that onto the live display.
void panelToScreen(int rawX, int rawY, float* outX, float* outY) {
    float px = g_maxX > 0 ? static_cast<float>(rawX) * g_portraitW / g_maxX
                          : static_cast<float>(rawX);
    float py = g_maxY > 0 ? static_cast<float>(rawY) * g_portraitH / g_maxY
                          : static_cast<float>(rawY);

    switch (g_rotation) {
        case 1:  // ROTATION_90
            *outX = py;
            *outY = static_cast<float>(g_portraitW) - px;
            break;
        case 2:  // ROTATION_180
            *outX = static_cast<float>(g_portraitW) - px;
            *outY = static_cast<float>(g_portraitH) - py;
            break;
        case 3:  // ROTATION_270
            *outX = static_cast<float>(g_portraitH) - py;
            *outY = px;
            break;
        default:  // ROTATION_0
            *outX = px;
            *outY = py;
            break;
    }
}

void clearFingerState() {
    for (int i = 0; i < kMaxSlots; ++i) {
        g_slotId[i] = -1;
        g_slotX[i] = 0;
        g_slotY[i] = 0;
    }
    g_curSlot      = 0;
    g_pending      = ProtoAPending{};
    g_protoABuild  = ProtoAFrame{};
    g_protoASnap   = ProtoAFrame{};
    g_sawMtReport  = false;
    g_frameHadAbs  = false;
    g_btnUp        = false;
    g_usesMtReport = false;
    g_gestureActive   = false;
    g_gestureConsumed = false;
}

/// Allocates the next invented Protocol A finger id.
int nextSyntheticId() {
    const int id = g_protoANextId;
    g_protoANextId = (g_protoANextId >= kSyntheticMax) ? kSyntheticBase
                                                       : g_protoANextId + 1;
    return id;
}

/// Re-attaches finger identities to a freshly closed Protocol A frame.
///
/// Protocol A frames are purely positional: there is no slot and, on many
/// panels, no ABS_MT_TRACKING_ID either, so a finger carries no name of its
/// own. Matching only by index would make a partial lift look like the
/// surviving finger teleporting (index 1 becomes index 0), and the app
/// underneath would see the wrong pointer move.
///
/// So every unnamed finger is matched against the closest still-unclaimed
/// finger of the previous frame; one with no plausible predecessor is genuinely
/// new and gets a fresh id. The cap keeps a fast drag from claiming a finger
/// on the other side of the screen. It is a heuristic — a finger crossing more
/// than ~1/6 of the panel between two frames can be mis-matched — but it beats
/// index-order matching by a wide margin.
void reidentifyProtoA(ProtoAFrame& frame, const ProtoAFrame& prev) {
    bool claimed[kMaxSlots] = {};

    const float capX = g_maxX > 0 ? static_cast<float>(g_maxX) / 6.0f : 200.0f;
    const float capY = g_maxY > 0 ? static_cast<float>(g_maxY) / 6.0f : 200.0f;

    for (int i = 0; i < frame.count; ++i) {
        if (frame.explicitId[i]) {
            // The device named this finger; reserve its predecessor so that an
            // unnamed finger cannot steal the name.
            for (int p = 0; p < prev.count; ++p) {
                if (!claimed[p] && prev.id[p] == frame.id[i]) { claimed[p] = true; break; }
            }
            continue;
        }

        int    best     = -1;
        double bestDist = 0.0;
        for (int p = 0; p < prev.count; ++p) {
            if (claimed[p]) continue;
            const float dx = static_cast<float>(frame.x[i] - prev.x[p]);
            const float dy = static_cast<float>(frame.y[i] - prev.y[p]);
            if (std::fabs(dx) > capX || std::fabs(dy) > capY) continue;
            const double d = static_cast<double>(dx) * dx +
                             static_cast<double>(dy) * dy;
            if (best < 0 || d < bestDist) { best = p; bestDist = d; }
        }

        if (best >= 0) {
            claimed[best] = true;
            frame.id[i]   = prev.id[best];
        } else {
            frame.id[i] = nextSyntheticId();
        }
    }
}

/// True when (x, y) lands inside one of the rectangles the overlay published.
/// Caller must hold g_mutex.
bool pointInRegionsLocked(float x, float y) {
    if (g_regionCount > 0 && nowUs() - g_regionStampUs > kRegionStaleUs) {
        LOGE("regions went stale (%lld ms old) — dropping them, pass-through",
             (nowUs() - g_regionStampUs) / 1000);
        g_regionCount = 0;
    }
    for (int i = 0; i < g_regionCount; ++i) {
        const int* r = &g_regionRects[i * 4];
        if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) return true;
    }
    return false;
}

/// Rebuilds g_pointers from the decoded state. Caller must hold g_mutex.
/// Returns true when at least one pointer is down.
bool rebuildPointersLocked(int* ids, int* xs, int* ys, int* rawXs, int* rawYs,
                           int* outCount) {
    g_pointers.clear();
    int n = 0;

    // `sx, sy` are screen pixels (what the menu and the swallow-test speak);
    // `rx, ry` are the panel's own raw coordinates, untouched.
    //
    // The raw pair exists for the mirror. It used to be reconstituted from the
    // screen pixels by inverting this same mapping, which is identity on paper
    // and is not identity in practice: the conversion to pixels is truncated to
    // an int, and the inverse it is multiplied through depends on two pieces of
    // state (this file's rotation, uinput's landscape flag and scale) that are
    // set from different places and are known to be able to disagree after a
    // geometry change. A mirror built that way can be a pixel out on a good day
    // and systematically wrong on a rotated one, and there is no reason to
    // round-trip at all: the panel already gave us the exact value the cloned
    // device wants, and the reference implementations just write it back.
    auto emit = [&](int id, float sx, float sy, int rx, int ry) {
        g_pointers.push_back(ReaderPointer{id, sx, sy});
        if (n < kMaxSlots) {
            ids[n]   = id;
            xs[n]    = static_cast<int>(sx);
            ys[n]    = static_cast<int>(sy);
            rawXs[n] = rx;
            rawYs[n] = ry;
            ++n;
        }
    };

    // Protocol B panels leave their fingerprints in the slot array. A B panel
    // only ever touches a slot once it has a tracking id, so an untouched array
    // (all -1) simply produces nothing — which is why this can be unconditional
    // instead of branching on the protocol. Panels of both kinds can be open at
    // the same time and are merged here.
    for (int i = 0; i < kMaxSlots; ++i) {
        if (g_slotId[i] < 0) continue;
        float sx, sy;
        panelToScreen(g_slotX[i], g_slotY[i], &sx, &sy);
        emit(g_slotId[i], sx, sy, g_slotX[i], g_slotY[i]);
    }

    // Protocol A panels: the last frame that SYN_REPORT closed.
    for (int i = 0; i < g_protoASnap.count; ++i) {
        float sx, sy;
        panelToScreen(g_protoASnap.x[i], g_protoASnap.y[i], &sx, &sy);
        emit(g_protoASnap.id[i], sx, sy, g_protoASnap.x[i], g_protoASnap.y[i]);
    }

    *outCount = n;
    return n > 0;
}

// ── Device discovery ─────────────────────────────────────────────────────────

/// What [probeTouchDevice] learned about one event node.
struct TouchProbeInfo {
    bool ok = false;         // accepted panel: B = slot+MT X/Y, A = MT X/Y+BTN_TOUCH+DIRECT
    bool hasSlot = false;
    bool hasDirect = false;  // INPUT_PROP_DIRECT (direct touch, not a pointer pad)
    bool hasBtnTouch = false;// BTN_TOUCH in the KEY bitmap
    bool hasMtPos = false;   // ABS_MT_POSITION_X and _Y both advertised
    int  mtAxisCount = 0;    // extra MT axes: TRACKING_ID / TOUCH_MAJOR / PRESSURE
    int  maxX = 0, maxY = 0;
};

/// True when the device advertises the multitouch axes we need.
TouchProbeInfo probeTouchDevice(int fd) {
    TouchProbeInfo info;
    bool hasX = false, hasY = false;

    uint8_t* bits = nullptr;
    ssize_t size = 0;
    int res = 0;
    while (true) {
        res = ioctl(fd, EVIOCGBIT(EV_ABS, size), bits);
        if (res < size) break;
        size = res + 16;
        bits = static_cast<uint8_t*>(realloc(bits, size * 2));
    }
    for (int j = 0; j < res; ++j) {
        for (int k = 0; k < 8; ++k) {
            if (!(bits[j] & (1 << k))) continue;
            int code = j * 8 + k;
            if (code == ABS_MT_SLOT)           info.hasSlot = true;
            else if (code == ABS_MT_POSITION_X) hasX = true;
            else if (code == ABS_MT_POSITION_Y) hasY = true;
            else if (code == ABS_MT_TRACKING_ID ||
                     code == ABS_MT_TOUCH_MAJOR ||
                     code == ABS_MT_PRESSURE) {
                ++info.mtAxisCount;
            }
        }
    }
    free(bits);

    info.hasMtPos = hasX && hasY;
    if (!hasX || !hasY) return info;

    // BTN_TOUCH 是真触摸面板的标志。vivo 把"屏下指纹辅助触摸"做成带
    // ABS_MT_POSITION_X/Y 的节点（vivo_ts_fp 的轴范围甚至和真屏一致），但那类
    // 节点既没有 BTN_TOUCH 也没有 INPUT_PROP_DIRECT —— 仅凭 X/Y 校验会把它们
    // 误识别成主屏，reader 一 grab，真触摸屏就被独占，系统触摸整体死亡。
    uint8_t keyBits[(KEY_MAX + 7) / 8] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) > 0) {
        info.hasBtnTouch = (keyBits[BTN_TOUCH / 8] >> (BTN_TOUCH % 8)) & 1;
    }

    input_absinfo infoX{}, infoY{};
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &infoX) != 0) return info;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &infoY) != 0) return info;
    info.maxX = infoX.maximum;
    info.maxY = infoY.maximum;

    uint8_t props[(INPUT_PROP_MAX + 7) / 8] = {};
    if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) > 0) {
        info.hasDirect = (props[INPUT_PROP_DIRECT / 8] >> (INPUT_PROP_DIRECT % 8)) & 1;
    }

    // Protocol B（带 ABS_MT_SLOT）照旧无条件接受。协议 A（无 SLOT）只在完整
    // 直触签名齐备时开放：MT 位置轴 + BTN_TOUCH + INPUT_PROP_DIRECT。这条
    // 路径是为华为/荣耀的 "input_mt_wrapper" 设的 —— 那类机器唯一的触摸节点
    // 没有 ABS_MT_SLOT，但 BTN_TOUCH/DIRECT/TRACKING_ID 一样不缺；而指纹辅助
    // 节点（vivo_ts_fp / vivo_fp）最多只凑齐 MT 位置轴。
    if (info.hasSlot) {
        info.ok = true;
    } else {
        info.ok = info.hasBtnTouch && info.hasDirect;
    }
    return info;
}

/// Enumerates /dev/input/event* and returns the paths of real panels.
/// Devices whose name contains "Aimbot" (our own virtual device) are skipped.
void enumeratePanelsDetailed(std::vector<ReaderPanel>& out, bool skipOurVirtualDevice);

int enumeratePanels(std::vector<std::string>& out, bool skipOurVirtualDevice) {
    std::vector<ReaderPanel> detailed;
    enumeratePanelsDetailed(detailed, skipOurVirtualDevice);
    out.reserve(detailed.size());
    for (const ReaderPanel& p : detailed) out.emplace_back(p.path);
    return static_cast<int>(detailed.size());
}

/// Same walk as [enumeratePanels], but fills a path+name list — the form the
/// UI uses to render "event4 • synaptics,s2338". Storage for the buffers is
/// kept in a static pool and survives the next call, so callers that only
/// hold on to `ReaderPanel::path` / `name` pointers can use them straight
/// away. Copy into a std::string if the result must outlive the next call.
void enumeratePanelsDetailed(std::vector<ReaderPanel>& out,
                              bool skipOurVirtualDevice) {
    out.clear();

    DIR* dir = opendir("/dev/input");
    if (!dir) {
        LOGE("open /dev/input failed errno=%d", errno);
        return;
    }

    std::vector<std::string> candidates;
    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        candidates.emplace_back(std::string("/dev/input/") + entry->d_name);
    }
    closedir(dir);
    std::sort(candidates.begin(), candidates.end());

    // Reused across calls so the const char* the UI holds onto stays valid
    // until the next enumeration (which only happens on UI open, not every
    // frame). Each entry is owned by `g_panelNames` / `g_panelPaths`.
    g_panelPaths.clear();
    g_panelNames.clear();
    g_panelOwnVirtual.clear();

    // The daemon's own uinput device (which uinput_inject creates) lives under
    // /dev/input/eventN too. The only stable handle on it is the sysfs name
    // ("inputN" — the basename of /sys/devices/virtual/input/<name>/), because
    // cloning the panel name makes the EVIOCGNAME match the real panel, and
    // the devtmpfs eventN numbering has nothing to do with the sysfs inputN
    // numbering. touch_reader matches each candidate by readlink-ing
    // /sys/class/input/eventN and comparing the link's basename to the
    // sysfs name uinput_inject captured right after UI_DEV_CREATE.
    const char* ourSysname = uinput_get_our_sysname();
    const bool haveOurVirtual = (ourSysname != nullptr && ourSysname[0] != '\0');

    // Collected first, ranked afterwards: the "first in sorted path order"
    // default was wrong — "/dev/input/event12" sorts before "/dev/input/event7"
    // and a Bluetooth band mimicking a touchscreen would then be grabbed
    // instead of the real panel.
    struct FoundPanel {
        std::string path;
        std::string name;
        bool ownVirtual;
        int  score;
        TouchProbeInfo probe;
    };
    std::vector<FoundPanel> found;

    for (const std::string& path : candidates) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;

        TouchProbeInfo probe = probeTouchDevice(fd);
        bool ok = probe.ok;

        char name[128] = {};
        if (ok && skipOurVirtualDevice) {
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0 &&
                strstr(name, "Aimbot") != nullptr) {
                ok = false;
            }
        }
        // Always grab a name for the UI label, even on devices the probe
        // rejected: it is the only way the user can tell two event nodes apart.
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) <= 0) {
            snprintf(name, sizeof(name), "(unnamed)");
        }

        // 带 MT 位置轴却被拒的节点值得记一笔：要么是被护栏正确挡下的指纹
        // 辅助节点，要么是用户唯一的触摸屏、而我们的门槛对它太紧。
        if (!ok && probe.hasMtPos) {
            LOGI("panel %s '%s' has MT position axes but was rejected "
                 "(slot=%d btnTouch=%d direct=%d)",
                 path.c_str(), name, probe.hasSlot ? 1 : 0,
                 probe.hasBtnTouch ? 1 : 0, probe.hasDirect ? 1 : 0);
        }

        // capability-wise a Bluetooth band can pass the probe; drop it by name.
        if (ok && isBlacklistedPanelName(name)) {
            LOGI("panel %s '%s' matches blacklist — skipped", path.c_str(), name);
            ok = false;
        }

        // Own uinput? Walk /sys/class/input/<eventN>, readlink the symlink,
        // and look for "/<sysfs-name>/event" inside the target. The symlink
        // for a uinput device looks like
        //   ../../devices/virtual/input/<sysfs-name>/<eventN>
        // and the sysfs-name (e.g. "input47") is the second-to-last segment.
        // The basename alone ("<eventN>") is the same as our leaf and would
        // match every panel, so it cannot be the discriminator.
        bool ownVirtual = false;
        if (haveOurVirtual) {
            // path = "/dev/input/eventN" → strip "/dev/input/" → "eventN"
            const char* slash = strrchr(path.c_str(), '/');
            const char* leaf = (slash != nullptr) ? slash + 1 : path.c_str();
            char linkPath[256];
            snprintf(linkPath, sizeof(linkPath), "/sys/class/input/%s", leaf);
            char target[256] = {};
            const ssize_t n = readlink(linkPath, target, sizeof(target) - 1);
            if (n > 0) {
                target[n] = '\0';
                // Pattern: "/<sysname>/event<...>" — matches uinput devices
                // (under /sys/devices/virtual/input/) but not platform panels
                // (whose paths look like ../../devices/platform/.../inputX/
                // and have nothing after the sysname).
                char needle[80];
                snprintf(needle, sizeof(needle), "/%s/event", ourSysname);
                if (strstr(target, needle) != nullptr) ownVirtual = true;
            }
        }

        if (ok) {
            // Real panels declare INPUT_PROP_DIRECT and carry the extra MT axes
            // (TOUCH_MAJOR/PRESSURE/TRACKING_ID); accessories that merely mimic
            // a touchscreen usually declare only the minimum set. Scored, not
            // hard-filtered, so a panel that skips DIRECT still stays in the
            // race. Ties keep the lexical path order (the old default).
            int score = (probe.hasDirect ? 1 : 0) + probe.mtAxisCount;
            LOGI("panel %s '%s' max=%d,%d slot=%d direct=%d mtAxes=%d score=%d own=%d",
                 path.c_str(), name, probe.maxX, probe.maxY,
                 probe.hasSlot ? 1 : 0, probe.hasDirect ? 1 : 0,
                 probe.mtAxisCount, score, ownVirtual ? 1 : 0);
            found.push_back({path, name, ownVirtual, score, probe});
        }
        close(fd);
    }

    std::stable_sort(found.begin(), found.end(),
                     [](const FoundPanel& a, const FoundPanel& b) {
                         return a.score > b.score;
                     });
    if (found.size() > static_cast<size_t>(kMaxDevices)) {
        found.resize(static_cast<size_t>(kMaxDevices));
    }

    // Fill the static pools in ranked order so the const char* handed out below
    // stays valid until the next enumeration.
    for (const FoundPanel& f : found) {
        g_panelPaths.push_back(f.path);
        g_panelNames.push_back(f.name);
        g_panelOwnVirtual.push_back(f.ownVirtual);
        out.push_back({g_panelPaths.back().c_str(),
                       g_panelNames.back().c_str(),
                       f.ownVirtual});
    }
}

void closeLocked() {
    for (int fd : g_fds) {
        if (fd >= 0) {
            if (g_grabbed) ioctl(fd, EVIOCGRAB, 0);
            close(fd);
        }
    }
    g_fds.clear();
    g_fdHasSlot.clear();
    g_grabbed = false;
    g_ready = false;
    g_panelPath.clear();
    g_pointers.clear();
    clearFingerState();
}

// ── Event decoding ───────────────────────────────────────────────────────────

/// Feeds one evdev batch through the Protocol B (slot) or Protocol A (position
/// stream) decoder, depending on what the source device speaks.
/// Returns true when a SYN_REPORT closed a frame.
bool decodeBatch(const struct input_event* batch, size_t count, bool useSlots) {
    bool frameEnd = false;

    for (size_t j = 0; j < count; ++j) {
        const input_event& ie = batch[j];

        if (useSlots) {
            if (ie.type == EV_ABS) {
                switch (ie.code) {
                    case ABS_MT_SLOT:
                        g_curSlot = (ie.value < 0) ? 0
                                  : (ie.value >= kMaxSlots ? kMaxSlots - 1 : ie.value);
                        break;
                    case ABS_MT_TRACKING_ID:
                        g_slotId[g_curSlot] = ie.value;
                        break;
                    case ABS_MT_POSITION_X:
                        g_slotX[g_curSlot] = ie.value;
                        // Some panels never send TRACKING_ID; treat a position
                        // report as the finger coming down.
                        if (g_slotId[g_curSlot] < 0) g_slotId[g_curSlot] = g_curSlot + 1;
                        break;
                    case ABS_MT_POSITION_Y:
                        g_slotY[g_curSlot] = ie.value;
                        if (g_slotId[g_curSlot] < 0) g_slotId[g_curSlot] = g_curSlot + 1;
                        break;
                    default:
                        break;
                }
            } else if (ie.type == EV_SYN && ie.code == SYN_REPORT) {
                frameEnd = true;
            }
        } else {
            // ── Protocol A: position stream ──────────────────────────────────
            // No ABS_MT_SLOT. Each finger ends with SYN_MT_REPORT, the frame
            // ends with SYN_REPORT, and every frame carries the complete set of
            // live fingers — so a finger is lifted simply by not being in it.
            if (ie.type == EV_ABS) {
                g_frameHadAbs = true;
                switch (ie.code) {
                    case ABS_MT_POSITION_X:  g_pending.x = ie.value; g_pending.hasPos = true; break;
                    case ABS_MT_POSITION_Y:  g_pending.y = ie.value; g_pending.hasPos = true; break;
                    case ABS_MT_TRACKING_ID: g_pending.tid = ie.value; break;
                    default: break;
                }
            } else if (ie.type == EV_SYN && ie.code == SYN_MT_REPORT) {
                // End of one finger's data. A report with no position is a
                // release marker (some drivers close a contact with
                // TRACKING_ID -1) and is deliberately not appended.
                if (g_pending.hasPos && g_protoABuild.count < kMaxSlots) {
                    const int i = g_protoABuild.count;
                    g_protoABuild.explicitId[i] = (g_pending.tid >= 0);
                    g_protoABuild.id[i] = g_pending.tid;
                    g_protoABuild.x[i]  = g_pending.x;
                    g_protoABuild.y[i]  = g_pending.y;
                    ++g_protoABuild.count;
                }
                g_sawMtReport  = true;
                g_usesMtReport = true;
                g_pending = ProtoAPending{};
            } else if (ie.type == EV_SYN && ie.code == SYN_REPORT) {
                // Frame closed — decide what it means, then promote it.
                if (g_btnUp) {
                    g_protoASnap.count = 0;                 // BTN_TOUCH 0 == all up
                } else if (!g_sawMtReport && g_pending.hasPos) {
                    // A device that never emits SYN_MT_REPORT is single-touch.
                    g_protoABuild.count = 1;
                    g_protoABuild.explicitId[0] = (g_pending.tid >= 0);
                    g_protoABuild.id[0] = g_pending.tid;
                    g_protoABuild.x[0]  = g_pending.x;
                    g_protoABuild.y[0]  = g_pending.y;
                    reidentifyProtoA(g_protoABuild, g_protoASnap);
                    g_protoASnap = g_protoABuild;
                } else if (g_protoABuild.count > 0) {
                    reidentifyProtoA(g_protoABuild, g_protoASnap);
                    g_protoASnap = g_protoABuild;
                } else if (g_usesMtReport && !g_frameHadAbs) {
                    g_protoASnap.count = 0;                 // bare frame == all up
                }
                // Any other bare frame leaves the snapshot untouched, so a
                // stray SYN_REPORT cannot invent a lift.

                g_protoABuild = ProtoAFrame{};
                g_pending     = ProtoAPending{};
                g_sawMtReport = false;
                g_frameHadAbs = false;
                g_btnUp       = false;
                frameEnd = true;
            } else if (ie.type == EV_KEY && ie.code == BTN_TOUCH && ie.value == 0) {
                // Deferred to SYN_REPORT so the decision is made with the whole
                // frame in hand: some drivers still report the last position of
                // the finger being released in the same frame.
                g_btnUp = true;
            }
        }
    }
    return frameEnd;
}

/// Reads whatever is pending on `fd` and decodes it with the protocol that
/// device actually speaks. Returns true on a frame.
bool processFd(int fd, bool useSlots) {
    struct input_event batch[64];
    ssize_t n = read(fd, batch, sizeof(batch));
    if (n <= 0) return false;
    if (n % static_cast<ssize_t>(sizeof(input_event)) != 0) {
        LOGE("partial evdev read (%zd bytes)", n);
        return false;
    }
    return decodeBatch(batch, static_cast<size_t>(n) / sizeof(input_event), useSlots);
}

/// Publishes the decoded pointers and, when the sink is on, mirrors them out.
///
/// The mirror decision is taken per GESTURE, not per frame: it is made from the
/// first finger's landing point the moment the panel goes from idle to touched,
/// and held until every finger lifts. Two reasons:
///
///   * a tap on the ImGui menu is decided correctly even though ImGui itself
///     only learns about the press on its next frame (per-frame gating would
///     always let the first down-through),
///   * a finger that starts on the game keeps working while it is dragged
///     across the menu, and a finger that starts on the menu keeps belonging to
///     it while it is dragged off — no mid-drag ownership flip-flop.
void publishPointers(bool mirror) {
    int ids[kMaxSlots];
    int xs[kMaxSlots];
    int ys[kMaxSlots];
    int rawXs[kMaxSlots];
    int rawYs[kMaxSlots];
    int n = 0;
    // Slot-direct buffers for Protocol B (old touch_core.cpp 1:1 model)
    int slotIds[kMaxSlots];
    int slotRawXs[kMaxSlots];
    int slotRawYs[kMaxSlots];
    int slotXs[kMaxSlots];
    int slotYs[kMaxSlots];
    bool useSlotsDirect = false;
    bool swallow = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        rebuildPointersLocked(ids, xs, ys, rawXs, rawYs, &n);
        // Build slot-direct view for Protocol B: panel slot k -> virtual slot k
        useSlotsDirect = g_refHasSlot;
        if (useSlotsDirect) {
            for (int k = 0; k < kMaxSlots; ++k) {
                if (g_slotId[k] >= 0) {
                    slotIds[k] = g_slotId[k];
                    slotRawXs[k] = g_slotX[k];
                    slotRawYs[k] = g_slotY[k];
                    float sx, sy;
                    panelToScreen(g_slotX[k], g_slotY[k], &sx, &sy);
                    slotXs[k] = static_cast<int>(sx);
                    slotYs[k] = static_cast<int>(sy);
                } else {
                    slotIds[k] = -1;
                    slotRawXs[k] = 0;
                    slotRawYs[k] = 0;
                    slotXs[k] = 0;
                    slotYs[k] = 0;
                }
            }
            // Protocol A fingers (g_protoASnap) have no slot number; if any
            // are present on a supposedly-B panel treat as fallback to compact.
            if (g_protoASnap.count > 0) useSlotsDirect = false;
        }
        if (n > 0) {
            if (!g_gestureActive) {
                g_gestureConsumed = pointInRegionsLocked(xs[0], ys[0]);
                LOGD("gesture at %d,%d -> %s", xs[0], ys[0],
                     g_gestureConsumed ? "menu (swallowed)" : "passthrough");
            }
            g_gestureActive = true;
        } else {
            g_gestureActive = false;
        }
        swallow = g_gestureConsumed;
    }

    if (!mirror) return;
    if (swallow && !g_passThrough) {
        uinput_mirror_clear();
    } else if (useSlotsDirect) {
        // Zero-delay 1:1 mirror — identical to G:\ai\Aimbot-ai touch_core.cpp
        uinput_mirror_slots(slotIds, slotRawXs, slotRawYs, slotXs, slotYs, kMaxSlots);
    } else {
        uinput_mirror_physical(ids, rawXs, rawYs, xs, ys, n);
    }
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

extern "C" bool reader_init(int screenW, int screenH, int rotation) {
    if (screenW <= 0 || screenH <= 0) {
        LOGE("reader_init: bad screen size %dx%d", screenW, screenH);
        return false;
    }

    // Enumerate first, pick the same one the legacy path did (first in sorted
    // order), then delegate to reader_init_with_path. The two paths share
    // everything else.
    std::vector<ReaderPanel> detailed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        enumeratePanelsDetailed(detailed, /*skipOurVirtualDevice=*/true);
    }
    if (detailed.empty()) {
        LOGE("no touch panel found under /dev/input");
        return false;
    }
    return reader_init_with_path(detailed.front().path, screenW, screenH, rotation);
}

extern "C" bool reader_init_with_path(const char* path, int screenW, int screenH, int rotation) {
    if (screenW <= 0 || screenH <= 0) {
        LOGE("reader_init_with_path: bad screen size %dx%d", screenW, screenH);
        return false;
    }
    if (path == nullptr || path[0] == '\0') {
        LOGE("reader_init_with_path: empty path");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    closeLocked();

    g_displayW = screenW;
    g_displayH = screenH;
    g_portraitW = std::min(screenW, screenH);
    g_portraitH = std::max(screenW, screenH);
    g_rotation = ((rotation % 4) + 4) % 4;
    clearFingerState();

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOGE("open %s failed errno=%d", path, errno);
        return false;
    }
    bool hasSlot = false;
    int maxX = 0, maxY = 0;
    TouchProbeInfo probe = probeTouchDevice(fd);
    if (!probe.ok) {
        LOGE("probeTouchDevice rejected %s", path);
        close(fd);
        return false;
    }
    hasSlot = probe.hasSlot;
    maxX = probe.maxX;
    maxY = probe.maxY;
    g_maxX = maxX;
    g_maxY = maxY;
    g_refHasSlot = hasSlot;
    g_panelPath = path;
    g_fds.push_back(fd);
    g_fdHasSlot.push_back(hasSlot);

    g_ready = true;
    // INFO, not DEBUG: this is the line that tells a remote report whether we
    // found the right panel at all, and whether it speaks protocol A or B.
    // A protocol-A-only stack shows up as touch that works for one finger and
    // goes wrong on the second, which reads exactly like "his overlay works,
    // the game does not".
    LOGI("reader ready: %s max=%d,%d protocol=%s portrait=%dx%d rotation=%d",
         path, maxX, maxY, hasSlot ? "B(slot)" : "A(stream)",
         g_portraitW, g_portraitH, g_rotation);
    return true;
}

extern "C" void reader_close(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    closeLocked();
}

extern "C" int reader_list_panels(ReaderPanel* out, int max) {
    if (out == nullptr || max <= 0) return 0;
    std::vector<ReaderPanel> detailed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        enumeratePanelsDetailed(detailed, /*skipOurVirtualDevice=*/true);
    }
    const int n = std::min<int>(static_cast<int>(detailed.size()), max);
    for (int i = 0; i < n; ++i) out[i] = detailed[i];
    return n;
}

extern "C" bool reader_select_panel(const char* path) {
    if (path == nullptr || path[0] == '\0') return false;

    // Cache the current geometry under the lock — reader_init_with_path needs
    // it, but it clobbers g_display* itself, so reading it after would race.
    int w = 0, h = 0, rot = 0;
    bool wasReady = false;
    bool wasGrabbed = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        w      = g_displayW;
        h      = g_displayH;
        rot    = g_rotation;
        wasReady   = g_ready;
        wasGrabbed = g_grabbed;
        if (g_panelPath == path) {
            // Already on this one. Cheap idempotent path; no churn.
            return true;
        }
    }

    if (!wasReady) {
        LOGE("reader_select_panel: reader not initialised");
        return false;
    }

    // Drop the grab FIRST, then close, then re-open the new panel and re-grab.
    // If the new open fails we deliberately do not re-grab the old panel: the
    // uinput module is still cloning the old one and a finger that lands now
    // would briefly read as a real system touch on the new (ungrabbed) panel,
    // which is the symptom the grab exists to prevent. The caller will see
    // the false return and can decide.
    reader_ungrab();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        closeLocked();
    }
    if (!reader_init_with_path(path, w, h, rot)) {
        LOGE("reader_select_panel: open %s failed, reader is now idle", path);
        return false;
    }
    if (!reader_grab()) {
        LOGE("reader_select_panel: grab on %s failed, reader is open but ungrabbed", path);
        return false;
    }
    LOGD("reader_select_panel: %s -> %s (wasGrabbed=%d)", "old", path, wasGrabbed ? 1 : 0);
    return true;
}

extern "C" bool reader_is_ready(void) { return g_ready; }

extern "C" void reader_set_screen_params(int screenW, int screenH, int rotation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (screenW > 0 && screenH > 0) {
        g_displayW = screenW;
        g_displayH = screenH;
        g_portraitW = std::min(screenW, screenH);
        g_portraitH = std::max(screenW, screenH);
    }
    g_rotation = ((rotation % 4) + 4) % 4;
}

extern "C" bool reader_grab(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready) return false;

    if (g_grabbed) return true;

    int grabbed = 0;
    for (size_t i = 0; i < g_fds.size(); ++i) {
        // Clear a stale grab left behind by a crashed daemon, then take ours.
        ioctl(g_fds[i], EVIOCGRAB, 0);
        if (ioctl(g_fds[i], EVIOCGRAB, 1) == 0) {
            ++grabbed;
        } else {
            LOGE("EVIOCGRAB failed on fd=%d errno=%d", g_fds[i], errno);
        }
    }
    g_grabbed = grabbed > 0;
    // INFO: from this instant the daemon is the device's only source of touch.
    // Everything the user reports as "the screen does not respond" starts here,
    // so the log has to be able to say whether we ever took the panel.
    LOGI("grab: %d/%zu device(s) — panel %s is now ours alone", grabbed, g_fds.size(),
         g_panelPath.c_str());
    return g_grabbed;
}

extern "C" void reader_ungrab(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_grabbed) return;
    for (int fd : g_fds) ioctl(fd, EVIOCGRAB, 0);
    g_grabbed = false;
    // INFO: the device has its own touch back from here, regardless of what the
    // injection side is doing. Pair with the uinput teardown line in
    // AimbotInput — together they are the whole "touch restored" story.
    LOGI("grab released");
}

extern "C" bool reader_is_grabbed(void) { return g_grabbed; }

extern "C" void reader_set_sink(bool enabled) {
    bool changed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        changed = (g_sink != enabled);
        g_sink = enabled;
    }
    if (changed && !enabled) uinput_mirror_clear();
}

extern "C" bool reader_get_sink(void) { return g_sink; }

/// Turns "let the menu own its gestures" on and off.
///
/// ON (passthrough) = mirror everything, exactly like aimbot 1.2.1. OFF = a
/// gesture that began inside a menu rectangle stays with the menu.
///
/// The order matters when it is turned back off mid-gesture: the fingers we had
/// already mirrored while passthrough was on are still held down on the virtual
/// device, and nothing will lift them, because from here on the gesture counts
/// as swallowed and a swallowed gesture is only ever cleared — so lift them
/// now, once, rather than leaving a stuck pointer behind. Same reasoning as
/// reader_set_sink() below.
extern "C" void reader_set_pass_through(bool enabled) {
    bool changed = false;
    bool consumed = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        changed = (g_passThrough != enabled);
        g_passThrough = enabled;
        consumed = g_gestureActive && g_gestureConsumed;
    }
    if (changed && !enabled && consumed) uinput_mirror_clear();
}

extern "C" bool reader_get_pass_through(void) { return g_passThrough; }

extern "C" void reader_set_regions(const int* rects, int count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!rects || count <= 0) {
        g_regionCount = 0;
        g_regionStampUs = nowUs();
        return;
    }
    g_regionStampUs = nowUs();
    if (count > READER_MAX_REGIONS) count = READER_MAX_REGIONS;
    for (int i = 0; i < count * 4; ++i) g_regionRects[i] = rects[i];
    g_regionCount = count;
}

extern "C" int reader_get_region_count(void) { return g_regionCount; }

extern "C" int reader_poll(int timeoutMs) {
    if (!g_ready || g_fds.empty()) return -1;

    std::vector<struct pollfd> pfds(g_fds.size());
    for (size_t i = 0; i < g_fds.size(); ++i) {
        pfds[i].fd = g_fds[i];
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    int ret = poll(pfds.data(), pfds.size(), timeoutMs);
    if (ret < 0) {
        if (errno != EINTR) LOGE("poll failed errno=%d", errno);
        return -1;
    }
    if (ret == 0) return 0;

    bool frame = false;
    for (size_t d = 0; d < g_fds.size(); ++d) {
        if (!(pfds[d].revents & POLLIN)) continue;
        const bool useSlots = (d < g_fdHasSlot.size()) ? g_fdHasSlot[d] : true;
        if (processFd(g_fds[d], useSlots)) frame = true;
    }
    if (!frame) return 0;

    // We hold the panel exclusively, so the system is NOT receiving the physical
    // finger natively — mirroring it back through uinput is what lets the app
    // underneath keep responding. The reader always decodes and publishes the
    // finger for the menu; the mirror only re-injects when the sink is on.
    publishPointers(g_sink);

    // ── Shout, do not paper over ────────────────────────────────────────────
    //
    // While we hold EVIOCGRAB this process is the only thing that can produce
    // touch, so a virtual device that stops accepting frames reads exactly like a
    // dead touchscreen. It is tempting to answer that by dropping the grab and
    // handing the panel back — and that is precisely the wrong move: the phone
    // starts working again, the symptom disappears, and the fault that caused it
    // never gets reported, so it comes back later with no trace. The grab is
    // held on purpose and stays held; what this does instead is make the failure
    // impossible to miss in the log.
    const int failures = uinput_write_failures();
    if (failures >= kAlarmFailures &&
        (failures == kAlarmFailures || failures % 120 == 0)) {
        LOGE("TOUCH DEAD: virtual device refused %d consecutive frames — "
             "grab deliberately still held, the panel will NOT work until "
             "this is investigated", failures);
    }
    return 1;
}

extern "C" int reader_read_pointers(ReaderPointer* out, int maxCount) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!out || maxCount <= 0) return 0;
    int n = 0;
    for (const ReaderPointer& p : g_pointers) {
        if (n >= maxCount) break;
        out[n++] = p;
    }
    return n;
}

extern "C" int reader_pointer_count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_pointers.size());
}

extern "C" int reader_get_max_x(void) { return g_maxX; }
extern "C" int reader_get_max_y(void) { return g_maxY; }

extern "C" const char* reader_get_panel_path(void) { return g_panelPath.c_str(); }
