// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::touch — the Touch page (触摸).
//
//  Picks which physical touch panel the daemon mirrors. The page is one
//  dropdown: nothing else lives here any more — sensitivity, smoothing and
//  the finger/mouse input switch were either never wired up or never useful
//  for the way the menu is actually used today, and the dropdown is the only
//  thing the page has to offer.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "ui/gui/widgets.h"

namespace aimbotng {
namespace ui {
namespace sections {

struct PageTouch {
    /// Index into the panel list the page most recently rendered. Persisted
    /// across frames so the choice survives a redraw; the sync layer pushes
    /// any change down into the daemon's reader.
    widgets::DropdownState device{0};

    /// Which way a touch we produce leaves the process: `uinput` (0) builds a
    /// virtual touchscreen and writes evdev frames into it, `InputManager` (1)
    /// hands a MotionEvent to InputDispatcher and never touches the device
    /// layer at all.
    ///
    /// Two options rather than a switch because neither is "the advanced one":
    /// uinput is the shape aimbot 1.2.1 has and it works on the phones we can
    /// test, while InputManager is the one that still works on a phone whose
    /// platform accepts a virtual touchscreen and then drops everything it
    /// sends. Persisted as "inject" under "touch" in config.json, and applied
    /// at daemon start (before the first OPEN, so no virtual device is built
    /// when InputManager is the saved choice).
    widgets::DropdownState injectBackend{0};
};

extern PageTouch g_pageTouch;

void drawTouchSection(ImDrawList* dl, float x, float& y, float w,
                      float bottomY, float s, float es, const Xf& xf,
                      Scroll& sc);

/// Called once per frame, the same place syncCapturePage / syncModelPage
/// already publish system state. Watching just the dropdown keeps this
/// cheap (one compare per frame) and matches the existing rhythm.
void syncTouchPage();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
