// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::aim — the Aim page (自瞄).
//
//  One section, one file pair: the page's state lives in `g_pageAim`, and
//  drawAimSection() is the only function the rest of the HUD calls. The master
//  switch lives on the section state so drawContent() can pick it up without
//  knowing what kind of switch it is.
//
//  Touch-area (dashed box) and trigger-area (circle) are *overlays* drawn in
//  screen coordinates — they describe where the aim/trigger operate on the
//  game below, not the menu above. drawAimOverlays() runs from drawHud()
//  before the board is painted, so the menu still sits on top of them, and
//  the overlays stay visible even after the menu is dismissed (so the user
//  can see what region they've set).
//
//  syncAimPage() is where the page drives system state: the PIDF controller
//  and touch-state machine live there, run every frame regardless of menu
//  visibility, and inject synthetic touches via uinput when there is a target.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ui/gui/widgets.h"

namespace aimbotng {
namespace ui {
namespace sections {

/// The Aim page's touch-area overlay: a dashed rectangle the user can drag
/// (move) and resize (edges resize one axis, corners resize both). Drawn in
/// screen pixels, not menu pixels, so the geometry survives the board being
/// hidden.
struct TouchAreaOverlay {
    widgets::SwitchState toggle;
    /// Top-left in screen pixels. -1 means "not placed yet" — the first
    /// overlay frame centres it on the screen.
    float x = -1.0f, y = -1.0f;
    float w = 600.0f, h = 400.0f;
    bool  placed = false;
    /// What the finger-down on the overlay started manipulating. The same
    /// geometry-snapshot pattern as the slider: the drag is delta-on-anchor,
    /// restored every frame from the *snapshot* fields, not accumulated.
    enum class Drag {
        None,
        Move,
        EdgeL, EdgeR, EdgeT, EdgeB,
        CornerTL, CornerTR, CornerBL, CornerBR,
    };
    Drag  drag    = Drag::None;
    float anchorX = 0.0f, anchorY = 0.0f;
    float startX  = 0.0f, startY  = 0.0f;
    float startW  = 0.0f, startH  = 0.0f;
};

/// The Aim page's trigger-area overlay: a perfect circle the user can move
/// (drag the centre) and scale (drag the N/E/S/W cardinal points). The edge
/// itself does nothing — it is the boundary the area *covers*, not a handle.
///
/// The shape must stay a perfect circle, so resize is "new radius = distance
/// from centre to finger". The centre stays put while a resize drag is in
/// flight; only Move drags it.
struct TriggerAreaOverlay {
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

// ── Aim-category filter ───────────────────────────────────────────────────
//
// The classes the model emits (e.g. "head", "chest") plus a 32-bit selection
// mask. `mask` is the widget's authoritative state; the items list is rebuilt
// from the model's class list whenever the model changes, and on the first
// sync the mask defaults to "everything selected" so the user does not have
// to tick a dozen boxes to start using the bot.
struct AimCategoryState {
    widgets::MultiSelectState sel;        // widget state (mask + open + anim)
    std::vector<std::string> items;       // class names, mirrors model output
    std::vector<const char*> itemPtrs;    // stable pointers into items
    bool synced = false;                  // has the model ever reported classes?

    /// Pulls the latest class list off the model. Rebuilds `items` /
    /// `itemPtrs` only when the size changed (cheap equality on count), and
    /// primes the mask to all-ones the very first time so the user's first
    /// aim run targets everything the model knows about.
    void syncFromModel(const std::vector<std::string>& modelClasses);

    /// True when the given class index is selected in the mask.
    bool isClassSelected(int cls) const {
        if (cls < 0 || cls >= 32) return false;
        return (sel.mask & (1u << cls)) != 0;
    }

    /// True when at least one bit is set — when the mask is empty the aim
    /// loop has no candidates and stays idle.
    bool hasAnySelected() const { return sel.mask != 0; }
};

// ── Aim controller (ported from Aimbot-ai old project's AimController) ─────
//
// A proper position-PID with the anti-oscillation machinery the old project
// spent years tuning — the bare `kp*e + ki*∫ + kd*Δ` that was here before is
// exactly what made the aim twitch, because the raw derivative amplifies
// per-frame box jitter and an unfiltered integrator winds up on large sweeps.
//
// What this adds over a bare PID:
//   * integral separation — only integrate while |error| is small; reset the
//     accumulator on a sign change (anti-windup, kills the vertical bounce).
//   * derivative EMA — smooths dE/dt so box jitter does not spike the D term.
//   * per-axis gain — Y gets reduced Kp/Kd (kpYRatio / kdYRatio) because the
//     touch pipeline and target Y motion are noisier than X.
//   * velocity damping (rate feedback) — subtracts a fraction of last frame's
//     applied move, braking the approach and removing overshoot bounce.
//   * per-frame clamp — bounds how far one step may travel.
//   * feedforward (F term) — leads moving targets using target velocity.
//
// The output is a *per-reference-frame* displacement in px (reference = 60 Hz,
// matching the old project). The caller scales it by `nstep = dt*60` so the
// behaviour is identical at any render rate — see syncAimPage().
struct AimController {
    // Gains, copied from the sliders each frame.
    float kp = 0.5f, ki = 0.0f, kd = 0.0f, kf = 0.0f;

    // Internal tuning — the old project's defaults, do not expose as sliders.
    float kpYRatio = 0.6f;   // Y-axis Kp attenuation (anti vertical oscillation)
    float kdYRatio = 0.85f;  // Y-axis Kd attenuation
    float kfYRatio = 0.7f;   // Y-axis feedforward attenuation
    float kfGain   = 2.5f;   // internal amplifier on the F term
    float derivFilterAlpha = 0.4f;  // D-term EMA: 1.0 = raw, 0.0 = frozen
    float integralSeparationThresh = 200.0f;  // px: don't integrate above this
    float integralLimit = 100.0f;  // anti-windup clamp on the accumulator
    float velocityDamping = 0.35f; // rate-feedback brake (0=none,0.5=firm)
    float maxPerFrame = 600.0f;    // px cap on one step's displacement
    float maxDragDist = 400.0f;    // px: lift+repress if finger exceeds this from start

    // ── F-term input (Kalman velocity path — UNUSED as of 2026-09-13) ──────
    // The Kalman tracker (tracking/kalman_tracker.h) emits per-frame target
    // velocity in screen pixels per render frame (vx, vy). These fields hold
    // that velocity when syncAimPage writes it; the F-term code in step()
    // currently does NOT read them — it uses the old-project `smoothVel`
    // (EMA of position deltas). The fields are kept so a future change can
    // re-enable the Kalman-velocity F path without touching the header again.
    float targetVelX = 0.0f, targetVelY = 0.0f;  // from tracker, px/frame
    float delayFrames = 2.0f;                     // ≈33 ms @ 60 Hz; slider sets

    // Per-axis state.
    float integX = 0.0f,  integY = 0.0f;   // accumulator (with separation)
    float derivFiltX = 0.0f, derivFiltY = 0.0f;  // EMA-smoothed derivative
    float prevErrX = 0.0f, prevErrY = 0.0f;       // last frame's error
    float prevAppliedX = 0.0f, prevAppliedY = 0.0f; // last frame's move (for damping)
    float prevTargetX = 0.0f, prevTargetY = 0.0f; // last frame's target (FF)
    float smoothVelX = 0.0f, smoothVelY = 0.0f;   // EMA target velocity (FF)
    bool  hasPrevTarget = false;

    void reset() {
        integX = integY = 0.0f;
        derivFiltX = derivFiltY = 0.0f;
        prevErrX = prevErrY = 0.0f;
        prevAppliedX = prevAppliedY = 0.0f;
        targetVelX = targetVelY = 0.0f;
        prevTargetX = prevTargetY = 0.0f;
        smoothVelX = smoothVelY = 0.0f;
        hasPrevTarget = false;
    }

    /// Writes the per-reference-frame move (px) needed to chase `target` from
    /// `current` into `outMove`. `dt` only feeds the feedforward velocity term.
    void step(ImVec2 target, ImVec2 current, float dt, ImVec2& outMove);
};

// ── Aim-touch state machine ───────────────────────────────────────────────
//
// Two phases: Idle (no finger down) and Pressed (a synthetic finger is held
// on the panel). The transitions:
//
    //   Idle → Pressed   on a frame where the aim is permitted (master switch on,
    //                    uinput ready, and either continuous-trigger OR a real
    //                    finger held in the trigger area) AND a selected target is
    //                    on screen. The synthetic finger drops at the TOUCH-AREA
    //                    centre ± jitter — the touch area is the finger's home
    //                    (where the virtual thumb rests), exactly as the old
    //                    project's AimController seeds at the aim area. The aim
    //                    DIRECTION is independent of where the touch area sits
    //                    because the PID error reference is the SCREEN CENTRE, not
    //                    the live finger (this is the "screen-centre coordinate"
    //                    bug the old project hit and fixed — using the finger /
    //                    touch-area position as the reference made the aim drift
    //                    toward a corner whenever the overlay was moved).
//   Pressed → Idle   only when the target is LOST (no selected detection) or the
//                    synthetic finger has travelled further than maxDragDist from
//                    its press point (drag safety). The finger is NOT released at
//                    the touch-area edge. Convergence does NOT release: the finger
//                    stays locked on the target (hold-on-lock, like the old
//                    project) so the aim does not thrash press/release.
struct TouchAimState {
    enum class Phase { Idle, Pressed };
    Phase  phase   = Phase::Idle;
    ImVec2 position = ImVec2(0, 0);   // current synthetic-finger position (screen px)
    ImVec2 start    = ImVec2(0, 0);   // press point; drag safety compares against this
    AimController aim;

    /// Stable tracking-id used in uinput_down() so the kernel sees the same
    /// finger across a lift/re-press cycle. The game does not care about the
    /// exact value, only that it does not change mid-gesture.
    int trackingId = 1;

    /// When "touch fusion" is on, aim drives a REAL finger that the player holds
    /// inside the touch area. In that case `drivingReal` is true, `realId` is the
    /// physical finger's id, and `slot` is the mirror slot aim has reserved via
    /// uinput_takeover_physical_id() — aim MOVES it only (never presses), and the
    /// mirror ignores that slot. When false, `slot` is UINPUT_SLOT_PRIMARY and aim
    /// presses a separate synthetic finger at the touch-area centre as usual.
    int  slot = 0;
    bool drivingReal = false;
    int  realId = -1;

    /// Set for ONE frame after a fusion takeover is released (real finger lifted /
    /// swapped). Lets the Idle branch skip spawning a synthetic finger that frame,
    /// so we don't get a phantom "extra touch point" the instant a real finger
    /// leaves the touch area. Cleared when read, and on press()/release().
    bool justReleasedTakeover = false;

    /// Drops a finger at the TOUCH-AREA centre ± jitter (the finger's home,
    /// where the virtual thumb rests — matching the old project's
    /// AimController, which seeds at the aim area). The aim direction is
    /// independent of this point because the PID error reference is the SCREEN
    /// CENTRE, never the live finger (that mismatch was the "screen-centre"
    /// bug). Records `start` so drag safety can later lift if the finger
    /// travels too far. Caller is responsible for the uinput_down() that
    /// immediately follows.
    void press(const TouchAreaOverlay& area);

    /// Releases the synthetic finger. Caller is responsible for the matching
    /// uinput_up() — same separation as press().
    void release();
};

// ── The Aim page itself ──────────────────────────────────────────────────
//
// One struct holds everything drawAimSection() needs to render and
// syncAimPage() needs to drive the PIDF loop. No globals beyond `g_pageAim`.
struct PageAim {
    widgets::SwitchState enabled{true};

    /// PIDF controller parameters (sliders write these; the controller reads).
    /// Defaults are the values the project used before the AimController port;
    /// the screen-centre error reference (see syncAimPage) is what keeps the aim
    /// direction independent of where the touch area sits, not these gains.
    widgets::SliderState kp{0.5f, 0.01f, 2.0f,  0.01f};
    widgets::SliderState ki{0.0f, 0.0f,  1.0f,  0.001f};
    widgets::SliderState kd{0.0f, 0.0f,  1.0f,  0.01f};
    widgets::SliderState kf{0.0f, 0.0f,  1.0f,  0.001f};

    /// Aim deadzone (0.0–1.0, one decimal): ports the old project's
    /// `convergeThresh`. Once the TARGET is within `deadzone` of the screen
    /// centre (crosshair) the finger stops nudging and holds steady, instead of
    /// twitching around trying to land exactly on a jittering box. 0.0 = no
    /// deadzone (must converge to the pixel), 1.0 = stop as soon as the target
    /// is within ~8% of the shorter screen edge of the crosshair.
    widgets::SliderState deadzone{0.0f, 0.0f, 1.0f, 0.1f};

    /// Aim lead / 提前量 (0–10, one-decimal "frames" of delay to project).
    /// Routes into the PIDF controller's F term:
    ///     F = targetVel × lead
    /// The target velocity is the Kalman tracker's per-frame velocity (vx,
    /// vy). lead=0 disables the F term entirely; lead=1 projects ~16 ms; lead=5
    /// projects ~80 ms (covers detection+inject latency, common sweet spot).
    /// The slider is integer-stepped because we want "number of frames of
    /// look-ahead", not a fractional multiplier.
    widgets::SliderState lead{2.0f, 0.0f, 10.0f, 1.0f};

    /// Touch-area overlay (the dashed box on screen).
    TouchAreaOverlay touchArea;

    /// Touch fusion — when on, the aim loop drives a REAL finger the player holds
    /// inside the touch area (via the mirror slot it reserves) instead of pressing
    /// a separate synthetic finger. The real finger must already be down to be
    /// taken over; if the touch area is empty, aim falls back to pressing a
    /// synthetic finger as usual. Prevents the two-touch-point flicker that two
    /// writers on one slot would cause.
    widgets::SwitchState fusion{false};

    /// Continuous trigger — when on, the trigger-area switch is hidden on the
    /// page and the trigger fires for as long as a target is on screen. The
    /// trigger-area overlay's `toggle.value` is left untouched so the user's
    /// configured region comes back unchanged when they turn this off again.
    widgets::SwitchState continuousTrigger{false};

    /// Trigger-area overlay (the circle on screen).
    TriggerAreaOverlay triggerArea;

    /// Multi-select filter over the model's classes. Drives which detections
    /// the aim loop is allowed to target.
    AimCategoryState aimCategory;

    /// Per-class Y deadzone (Y死区·<类别>, 0.0–1.0, default 0.0): how much
    /// vertical slop the Y axis tolerates before chasing the target box.
    /// 0.0 = no deadzone, Y aims at the box middle precisely; 1.0 = deadzone
    /// spans half the box height, i.e. Y stops as soon as the crosshair is
    /// vertically inside the box. The stop band is `v * boxHalfHeight`, so it
    /// scales with each target's own size. X is unaffected — it stays on the
    /// global radial deadzone.
    /// Only classes currently *selected* in aimCategory get a slider row;
    /// values for unselected classes are kept. Keyed by class index.
    std::map<int, widgets::SliderState> yFollow;

    /// The synthetic touch state machine — see TouchAimState for the rules.
    TouchAimState touchAim;
};

/// The single PageAim instance. The rail reaches in to read the master
/// switch; drawAimSection() drives every control on the page; syncAimPage()
/// drives the PIDF loop and the uinput injection.
extern PageAim g_pageAim;

/// Draw the Aim page starting at `(x, *y)`, advancing `*y` past every row.
void drawAimSection(ImDrawList* dl, float x, float& y, float w,
                    float bottomY, float s, float es, const Xf& xf);

/// Draw the Aim page's screen-space overlays (touch-area dashed box and
/// trigger-area circle), including all drag/interaction handling. Called
/// from drawHud() every frame the render loop ticks, regardless of whether
/// the menu board is showing.
void drawAimOverlays();

/// Per-frame state-publishing hook. Syncs the aim-category item list off the
/// model, copies slider values into the controller, and runs the press /
/// release / re-press / uinput-injection loop described on TouchAimState.
/// Called from drawHud() before any board draws — runs even when the menu is
/// hidden, so a configured aim keeps firing after the user dismisses the menu.
void syncAimPage();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
