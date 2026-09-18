// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::capture — the Capture page (画面).
//
//  The only page that drives something outside the menu: its slider is the
//  size of the square the capture module pulls at the centre of the panel,
//  and its switch is the on/off for that pull. Every frame syncCapturePage()
//  publishes those two values into the capture module, so the page is the
//  single author of both — the menu can never disagree with what capture is
//  doing.
//
//  The live preview is drawn in the same pass: a square well on the right
//  pane filled with the latest frame texture, or a placeholder when no
//  frames have come back yet.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "ui/gui/widgets.h"

namespace aimbotng {
namespace ui {
namespace sections {

struct PageCapture {
    widgets::SwitchState enabled{false};
    widgets::SliderState size{640.0f, 192.0f, 640.0f, 32.0f};
};

extern PageCapture g_pageCapture;

/// Draw the Capture page: size slider on top, the preview well below.
void drawCaptureSection(ImDrawList* dl, float x, float& y, float w,
                        float bottomY, float s, float es, const Xf& xf,
                        Scroll& sc);

/// Mirror `g_pageCapture` into the capture module. Cheap and idempotent so
/// it runs every frame rather than being hooked to the individual edits —
/// there is no path by which the two can then drift apart.
void syncCapturePage();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
