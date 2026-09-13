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
};

extern PageTouch g_pageTouch;

void drawTouchSection(ImDrawList* dl, float x, float& y, float w,
                      float bottomY, float s, float es, const Xf& xf);

/// Called once per frame, the same place syncCapturePage / syncModelPage
/// already publish system state. Watching just the dropdown keeps this
/// cheap (one compare per frame) and matches the existing rhythm.
void syncTouchPage();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
