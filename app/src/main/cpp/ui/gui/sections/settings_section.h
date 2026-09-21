// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::settings — the Settings page (设置).
//
//  Page-level toggles for HUD behaviour (FPS overlay, screenshot block) and
//  personalisation knobs (menu scale, language). Mirrors the shape of the
//  other section pages so adding a row is the same in every file.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "ui/gui/widgets.h"
#include <cstdint>

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

    /// Mirrors the gestures that begin on this menu out to the system as well,
    /// instead of letting the menu own them.
    ///
    /// The menu is not a window: the input system has no idea it is there, so a
    /// tap on it is either swallowed by the reader or delivered to whatever is
    /// underneath — there is no third option. Off is the useful default (tapping
    /// a slider must not also fire the game). On reproduces aimbot 1.2.1
    /// exactly, which is what to reach for when a device is suspected of
    /// objecting to the swallowed gestures. Written into the reader by
    /// syncSettingsPage(); persisted as "touchPass" in config.json.
    widgets::SwitchState   touchPassthrough{false};

    /// Draws the Kalman tracker's smoothed tracks as blue boxes with their
    /// track id (no score, no class) — for watching identity stability and
    /// smoothing, distinct from the raw red detection overlay. Off by default.
    widgets::SwitchState   showTracking{false};

    /// Multi-target Kalman tracker tuning (see tracking/kalman_tracker.h).
    /// Written into tracking::trackerConfig() by syncSettingsPage().
    ///
    /// trackIou is the minimum association score. It used to be a bare IoU
    /// threshold at 0.5, which a fast target can never clear: a box 100 px wide
    /// crossing at 60 px per detector frame overlaps its previous position with
    /// IoU ≈ 0.2, so the track was dropped and recreated every frame — id churn,
    /// a jittering box, and a velocity pinned at 0 because a new track starts
    /// from rest. The score now also carries a centre-distance term (see
    /// matchAndUpdate), so 0.15 keeps fast targets attached to their own track.
    /// Raise it only if two different enemies get merged into one track.
    widgets::SliderState trackIou{0.15f, 0.05f, 0.6f, 0.05f};         // min association score
    widgets::SliderState trackConfirm{1.0f, 1.0f, 10.0f, 1.0f};      // frames to confirm a track
    widgets::SliderState trackTerminate{5.0f, 1.0f, 10.0f, 1.0f};    // missed frames before drop

    // ── Inference trigger ─────────────────────────────────────────────────
    //
    /// When on, inference runs continuously. When off, inference only runs while
    /// a physical finger is held inside the inference-area circle — saving the
    /// cost of the detector when no aiming is needed.
    widgets::SwitchState continuousInference{true};

    /// Inference-area overlay: a circle on screen. Visible and adjustable only
    /// when continuousInference is OFF. Inference runs only when a physical
    /// finger is held inside this circle.
    struct InferenceAreaOverlay {
        widgets::SwitchState toggle;
        float cx = -1.0f, cy = -1.0f;
        float r  = 200.0f;
        bool  placed = false;
        enum class Drag {
            None,
            Move,
            ResizeN, ResizeE, ResizeS, ResizeW,
        };
        Drag  drag    = Drag::None;
        float anchorX = 0.0f, anchorY = 0.0f;
        float startCX = 0.0f, startCY = 0.0f;
        float startR  = 0.0f;
    };
    InferenceAreaOverlay inferenceArea;
};

extern PageSettings g_pageSettings;

void drawSettingsSection(ImDrawList* dl, float x, float& y, float w,
                         float bottomY, float s, float es, const Xf& xf,
                         Scroll& sc);

/// Publishes this page's system-level switch (currently the "Anti Screenshot"
/// toggle) into the layer it actually controls. Cheap: does work only on a
/// transition, and [syncCapturePage] / [syncModelPage] set the rhythm.
///
/// Called once per frame by drawHud() before the board is drawn so a flip
/// made while the board is hidden still takes effect — see the long note in
/// drawHud().
void syncSettingsPage();

/// Draw the inference-area circle overlay (foreground draw list, every frame).
/// The area is placed (centred) regardless of the continuousInference switch;
/// only the toggle controls whether it is drawn.
void drawInferenceAreaOverlay();

/// Whether a physical finger is currently held inside the inference-area circle
/// — the hold-to-infer intent, independent of whether the engine is ready.
///
/// Exported because the render loop needs it for frame pacing: while the finger
/// is down the detector is (or is about to be) waiting on frames, and the loop
/// must not fall through to the dormant tier. `infer::runtime::wantsFrames()`
/// cannot answer that question on its own — it only turns true once arm() has
/// taken effect, which is *after* the compile on a cold start, so the loop would
/// otherwise coast at 2 fps for the whole compile and then hand the detector
/// frames at 2 fps for as long as the finger stayed down.
bool holdToInferHeld();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
