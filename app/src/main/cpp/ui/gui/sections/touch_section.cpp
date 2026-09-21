// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::touch — implementation. See touch_section.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "touch_section.h"

#include <android/log.h>

#include <cstring>
#include <string>
#include <vector>

#include "input/inject_backend.h"
#include "input/touch_reader.h"
#include "ui/gui/hud.h"
#include "ui/gui/theme.h"

namespace aimbotng {
namespace ui {
namespace sections {

PageTouch g_pageTouch;

#define TAG "AimbotTouch"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace {
using namespace theme;

/// Backing storage for the dropdown items. The widget takes const char*
/// pointers and the strings have to outlive the dropdown widget call, so we
/// keep them in a static pool and rebuild it whenever the panel list changes
/// (i.e. on first open and any time [syncTouchPage] detects a new path the
/// daemon has not seen before).
std::vector<ReaderPanel> g_panels;
std::vector<std::string> g_labels;
std::vector<const char*> g_items;
/// Parallel to g_items: own-virtual-device entries draw greyed-out and
/// ignore taps. Kept as `uint8_t` rather than `bool` because
/// `std::vector<bool>` is the bit-packed specialisation and has no `.data()`;
/// the dropdown widget reads one byte per entry, so this matches its layout.
std::vector<uint8_t>     g_disabled;

const char* kMid = " \xc2\xb7 ";   // U+00B7 middle dot, surrounded by spaces

/// The choices in the 触摸方式 dropdown. Order is the wire contract: the index
/// is what gets saved in config.json and handed to inject_set_backend(), and
/// the values are INJECT_BACKEND_* in input/inject_backend.h.
const char* const kBackendItems[] = {"uinput", "InputManager"};
constexpr int     kBackendCount   = 2;
static_assert(kBackendCount == 2, "kBackendItems and INJECT_BACKEND_* must agree");

/// Rebuilds the label / items arrays from the latest panel list, and returns
/// the index of the panel that matches [targetPath] (or -1 if missing).
int rebuildItems(const char* targetPath) {
    g_labels.clear();
    g_items.clear();
    g_disabled.clear();
    g_labels.reserve(g_panels.size());
    g_items.reserve(g_panels.size());
    g_disabled.reserve(g_panels.size());

    int match = -1;
    for (size_t i = 0; i < g_panels.size(); ++i) {
        // "event4" from "/dev/input/event4" — the user-facing leaf.
        std::string leaf;
        const char* slash = strrchr(g_panels[i].path, '/');
        leaf = (slash != nullptr) ? std::string(slash + 1)
                                   : std::string(g_panels[i].path);

        // Own uinput devices get a "(uinput)" tag so the user can spot why
        // they are greyed-out without having to guess. "uinput" is the kernel
        // term and reads as "this is software-injected" to anyone who has met
        // the term before — "本机" / "本设备" did not actually say that.
        std::string label = leaf + kMid + g_panels[i].name +
                            (g_panels[i].ownVirtual ? " (uinput)" : "");
        g_labels.push_back(std::move(label));
        g_items.push_back(g_labels.back().c_str());
        g_disabled.push_back(g_panels[i].ownVirtual ? 1 : 0);

        if (targetPath != nullptr && match < 0 &&
            !g_panels[i].ownVirtual &&
            strcmp(g_panels[i].path, targetPath) == 0) {
            match = static_cast<int>(i);
        }
    }
    return match;
}

/// First-time initialiser: pulls the full list, points the dropdown at the
/// panel the daemon actually grabbed on boot. Called once when the page has
/// never been populated.
void ensureLoadedLocked() {
    if (!g_panels.empty()) return;

    ReaderPanel buf[READER_MAX_PANELS];
    const int n = reader_list_panels(buf, READER_MAX_PANELS);
    g_panels.assign(buf, buf + n);

    const int match = rebuildItems(reader_get_panel_path());
    if (match >= 0) g_pageTouch.device.value = match;
    // No match is fine: the dropdown will show "(no panel selected)" by
    // showing the first entry but [syncTouchPage] will refuse to commit a
    // value the reader does not recognise.
}
}  // namespace

void drawTouchSection(ImDrawList* dl, float x, float& y, float w,
                      float /*bottomY*/, float s, float es, const Xf& xf,
                      Scroll& sc) {
    const float gap   = csize(12.0f) * s;
    const float rowDd = csize(widgets::kDropdownRowH) * s;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        const ImVec2 p = xf.pt(wx, wy);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    ensureLoadedLocked();

    // Capture the first row's on-screen rect BEFORE `y` moves on, and pass
    // `drawListInline=false` so its open list is painted later via
    // dropdownList(). The HUD shares one foreground draw list, so call order
    // is paint order — and without this the 触摸方式 row drawn next would
    // paint over 触摸设备's list, making it look like the list sits *below*
    // 触摸方式. This is the deferred-list pattern used by Aim's category
    // filter and by the model dialog.
    const float firstRowY = sc.screenY(y);
    widgets::Rect firstRowR = wRect(x, firstRowY, w, rowDd);

    // The widget takes a `const char* const*`; g_items is rebuilt only when
    // the panel list changes, so its pointers stay valid for the frame.
    widgets::dropdown(dl, firstRowR, g_pageTouch.device,
                      "触摸设备",
                      g_items.empty() ? nullptr : g_items.data(),
                      static_cast<int>(g_items.size()), es,
                      /*drawListInline=*/false,
                      g_disabled.empty() ? nullptr
                                         : reinterpret_cast<const bool*>(g_disabled.data()));
    y += rowDd + gap;

    // ── Injection backend ──────────────────────────────────────────────────
    // Which of the two ways a touch leaves this process. The label carries the
    // state as well as the name, because the failure this setting is here to
    // expose — "selected, and the platform will not let us inject" — has no
    // other symptom: the menu still works (it reads the panel directly) and the
    // phone simply ignores everything we send.
    const int chosenBackend = g_pageTouch.injectBackend.value;
    const bool backendOk = ::inject_is_ready() != 0;
    const bool backendName = chosenBackend == INJECT_BACKEND_INPUT_MANAGER;

    static std::string s_backendLabel;
    s_backendLabel = "触摸方式";
    if (!backendOk) {
        // inject_is_ready() answers for whichever backend is LIVE, so only
        // annotate when the selection and the live one agree — otherwise the
        // note would belong to the backend being switched away from.
        if (::inject_get_backend() == chosenBackend) {
            s_backendLabel += " (注入不可用)";
        }
    }

    const float secondRowY = sc.screenY(y);
    widgets::dropdown(dl, wRect(x, secondRowY, w, rowDd), g_pageTouch.injectBackend,
                      s_backendLabel.c_str(), kBackendItems, kBackendCount, es);
    y += rowDd + gap;

    // Deferred list for 触摸设备. Painting it here — AFTER 触摸方式's row — is
    // what makes it sit ABOVE 触摸方式 rather than below. No-op while the list
    // is closed (anim ≈ 0), so a closed dropdown costs nothing.
    widgets::dropdownList(dl, firstRowR, g_pageTouch.device,
                          g_items.empty() ? nullptr : g_items.data(),
                          static_cast<int>(g_items.size()), es,
                          g_disabled.empty() ? nullptr
                                             : reinterpret_cast<const bool*>(g_disabled.data()));
}

void syncTouchPage() {
    // Two rows here reach into the daemon: the panel dropdown picks which
    // /dev/input node the reader mirrors, and the backend dropdown picks how a
    // touch gets back out. Both follow the same rhythm as syncCapturePage() /
    // syncSettingsPage() — one call per actual change, never per frame.

    // ── Which panel ─────────────────────────────────────────────────────────
    static int lastChosen = -1;

    const int chosen = g_pageTouch.device.value;
    if (chosen != lastChosen) {
        lastChosen = chosen;

        if (chosen < 0 || chosen >= static_cast<int>(g_panels.size())) {
            LOGW("touch dropdown chose out-of-range index %d", chosen);
        } else if (g_panels[chosen].ownVirtual) {
            // Last line of defence: the widget already refuses to set a disabled
            // value on a tap, but if anything else ever drives g_pageTouch.device
            // directly (a preset, a future feature) this still gates the call.
            LOGW("touch dropdown tried to commit own-virtual device; ignored");
        } else {
            const char* path = g_panels[chosen].path;
            if (!reader_select_panel(path)) {
                // Selection refused (open / grab failed) — leave the dropdown
                // where the user put it so they can retry, but log loudly. The
                // reader is already idle at this point per reader_select_panel's
                // contract, so any subsequent touch on the panel goes to the
                // system InputReader.
                LOGW("reader_select_panel(%s) failed; reader is idle until relaunch", path);
            }
        }
    }

    // ── Which backend ───────────────────────────────────────────────────────
    //
    // `lastBackend` starts below zero rather than at the uinput default so the
    // first frame pushes whatever config.json says, even when that is uinput.
    // inject_set_backend() is idempotent, so that costs a compare and nothing
    // else — and it means the menu and the daemon can never disagree about the
    // choice, which is the one thing that would make this setting untestable.
    static int lastBackend = -1;
    const int nowBackend = g_pageTouch.injectBackend.value;
    if (nowBackend != lastBackend) {
        lastBackend = nowBackend;
        const int ready = ::inject_set_backend(nowBackend);
        if (ready) {
            __android_log_print(ANDROID_LOG_INFO, TAG, "inject backend -> %s (ready)",
                                nowBackend == INJECT_BACKEND_INPUT_MANAGER
                                    ? "InputManager" : "uinput");
        } else {
            // Not a rollback: silently going back to the other backend is how
            // this project has lost faults before. The choice stands, the row
            // says so, and the reason is in the log.
            const char* why = aimbotng::input::imBridgeLastError();
            __android_log_print(ANDROID_LOG_ERROR, TAG,
                                "inject backend -> %s NOT usable: %s",
                                nowBackend == INJECT_BACKEND_INPUT_MANAGER
                                    ? "InputManager" : "uinput",
                                (why && why[0]) ? why : "unknown");
        }
    }
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
