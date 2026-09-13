// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::settings — the Settings page (设置).
//
//  Page-level toggles for HUD behaviour (FPS overlay, screenshot block) and
//  personalisation knobs (menu scale, language). Mirrors the shape of the
//  other section pages so adding a row is the same in every file.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "ui/gui/widgets.h"

namespace aimbotng {
namespace ui {
namespace sections {

struct PageSettings {
    widgets::SwitchState   fpsOverlay{true};
    widgets::SwitchState   antiScreenshot{true};
    /// Draws the boxes the detector found, on top of everything. Off by default:
    /// it is a debugging view, and a screen full of rectangles is not what
    /// anyone wants while actually aiming.
    widgets::SwitchState   showDetections{false};

    /// Draws the Kalman tracker's smoothed tracks as blue boxes with their
    /// track id (no score, no class) — for watching identity stability and
    /// smoothing, distinct from the raw red detection overlay. Off by default.
    widgets::SwitchState   showTracking{false};

    /// Multi-target Kalman tracker tuning (see tracking/kalman_tracker.h).
    /// Written into tracking::trackerConfig() by syncSettingsPage().
    widgets::SliderState trackIou{0.5f, 0.1f, 0.9f, 0.05f};        // min IoU to associate
    widgets::SliderState trackConfirm{1.0f, 1.0f, 10.0f, 1.0f};    // frames to confirm a track
    widgets::SliderState trackTerminate{5.0f, 1.0f, 30.0f, 1.0f};  // missed frames before drop
};

extern PageSettings g_pageSettings;

void drawSettingsSection(ImDrawList* dl, float x, float& y, float w,
                         float bottomY, float s, float es, const Xf& xf);

/// Publishes this page's system-level switch (currently the "Anti Screenshot"
/// toggle) into the layer it actually controls. Cheap: does work only on a
/// transition, and [syncCapturePage] / [syncModelPage] set the rhythm.
///
/// Called once per frame by drawHud() before the board is drawn so a flip
/// made while the board is hidden still takes effect — see the long note in
/// drawHud().
void syncSettingsPage();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
