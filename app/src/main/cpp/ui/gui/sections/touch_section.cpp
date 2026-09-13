// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::touch — implementation. See touch_section.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "touch_section.h"

#include <android/log.h>

#include <cstring>
#include <string>
#include <vector>

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
                      float /*bottomY*/, float s, float es, const Xf& xf) {
    const float gap   = 12.0f * s;
    const float rowDd = widgets::kDropdownRowH * s;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        const ImVec2 p = xf.pt(wx, wy);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    ensureLoadedLocked();

    // The widget takes a `const char* const*`; g_items is rebuilt only when
    // the panel list changes, so its pointers stay valid for the frame.
    widgets::dropdown(dl, wRect(x, y, w, rowDd), g_pageTouch.device,
                      "触摸设备",
                      g_items.empty() ? nullptr : g_items.data(),
                      static_cast<int>(g_items.size()), es,
                      /*drawListInline=*/true,
                      g_disabled.empty() ? nullptr
                                         : reinterpret_cast<const bool*>(g_disabled.data()));
    y += rowDd + gap;
}

void syncTouchPage() {
    // Only one switch on this page, and only the dropdown needs to reach into
    // the daemon. Mirrors syncCapturePage() / syncModelPage() — one frame
    // per state change.
    static int lastChosen = -1;

    const int chosen = g_pageTouch.device.value;
    if (chosen == lastChosen) return;
    lastChosen = chosen;

    if (chosen < 0 || chosen >= static_cast<int>(g_panels.size())) {
        LOGW("touch dropdown chose out-of-range index %d", chosen);
        return;
    }
    // Last line of defence: the widget already refuses to set a disabled
    // value on a tap, but if anything else ever drives g_pageTouch.device
    // directly (a preset, a future feature) this still gates the call.
    if (g_panels[chosen].ownVirtual) {
        LOGW("touch dropdown tried to commit own-virtual device; ignored");
        lastChosen = chosen;   // keep state coherent so we don't loop-log
        return;
    }

    const char* path = g_panels[chosen].path;
    if (!reader_select_panel(path)) {
        // Selection refused (open / grab failed) — leave the dropdown where
        // the user put it so they can retry, but log loudly. The reader is
        // already idle at this point per reader_select_panel's contract, so
        // any subsequent touch on the panel goes to the system InputReader.
        LOGW("reader_select_panel(%s) failed; reader is idle until relaunch", path);
    }
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
