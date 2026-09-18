// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::backflash — the Back-Flash page (背闪).
//
//  When the model spots a "flash" target (the player's screen is being
//  whited-out by an enemy grenade), the player wants to turn AWAY from the
//  flash so their character model is not lit up when the screen comes back.
//  This page drives that gesture: a synthetic finger drops inside the touch
//  area, slides sideways (opposite the flash's screen position), lifts, waits,
//  then slides back to where the player was.
//
//  Layout, top-to-bottom on the right pane:
//    1. 背闪延迟  (ms)         — wait this long after spotting the flash
//    2. 复位延迟  (ms)         — wait this long between the swipe and the reset
//    3. 滑屏距离(px) [1..2000] — total px the finger travels during a swipe
//    4. 每帧最大移动 [1..400]   — per-frame movement cap (Bezier velocity peak)
//    5. 触摸区域               — dashed-box overlay (drag/resize, same shape
//                                as the Aim page's touch area)
//    6. 闪光类别               — multi-select over the model's class names
//
//  Touch lifecycle per detected flash (one full cycle = one swipe + one
//  reset; "press / slide / lift" repeats):
//
//    WaitFlashDelay            nothing on screen
//      ↓ wait flashDelay
//    Swiping (finger DOWN)     press at a random point inside the touch area,
//                              slide (Bezier progress curve, per-frame
//                              movement clamped to maxPerFramePx) to the
//                              computed END — END = press ± slideDistancePx
//                              on the axis pointing AWAY from the flash,
//                              plus ±4 px random on each axis (the END
//                              jitter is one-shot — only the target is
//                              randomised, not every frame's move).
//                              Lift at END.
//      ↓
//    WaitResetDelay            nothing on screen
//      ↓ wait resetDelay
//    Resetting (finger DOWN)   press at END (the post-swipe position), slide
//                              back to (press point + ±4 px random) using
//                              the same Bezier curve. Lift at END.
//      ↓
//    Idle                      wait for the next detection
//
//  During the whole cycle (WaitFlashDelay through Resetting) the page sets
//  blockingAim=true; syncAimPage() reads that flag through
//  backflashIsBlockingAim() and refuses to drive its own synthetic finger
//  on UINPUT_SLOT_PRIMARY while it is set, so back-flash and aim never
//  share the slot. The flag flips off when the cycle completes (back to
//  Idle).
//
//  syncBackFlashPage() is called BEFORE syncAimPage() so the flag is set
//  before aim checks it — a one-frame race the other way would have aim
//  press a finger on the slot back-flash is about to claim.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/gui/widgets.h"

namespace aimbotng {
namespace ui {
namespace sections {

// ── Back-flash touch-area overlay ──────────────────────────────────────────
// The swipe area: the press lands at a random point inside this rectangle;
// the swipe ends outside it. Drag/resize interaction mirrors the Aim page's
// touch area, so users only learn one set of gestures across both overlays.
struct BackFlashTouchArea {
    widgets::SwitchState toggle;
    float x = -1.0f, y = -1.0f;
    float w = 600.0f, h = 400.0f;
    bool  placed = false;
    /// Snapshot of the geometry captured on the press-down frame, restored
    /// every frame the drag is in flight (same pattern as the Aim page).
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

// ── Back-flash category filter ─────────────────────────────────────────────
// Same shape as the Aim page's filter: the widget mask selects which model
// classes qualify as a "flash" (typically just one entry — the model's flash
// class). First sync with a non-empty class list selects everything.
struct BackFlashCategoryState {
    widgets::MultiSelectState sel;
    std::vector<std::string> items;
    std::vector<const char*> itemPtrs;
    bool synced = false;

    void syncFromModel(const std::vector<std::string>& modelClasses);

    bool isClassSelected(int cls) const {
        if (cls < 0 || cls >= 32) return false;
        return (sel.mask & (1u << cls)) != 0;
    }

    bool hasAnySelected() const { return sel.mask != 0; }
};

// ── Page state machine ──────────────────────────────────────────────────────
struct PageBackFlash {
    /// Page master switch — on/off the user toggles from the page header.
    widgets::SwitchState enabled{false};

    /// 背闪延迟 (ms) — how long after the qualifying event the swipe fires.
    widgets::SliderState flashDelay{100.0f, 0.0f, 500.0f, 1.0f};

    /// 复位延迟 (ms) — how long the cycle waits between the swipe and the
    /// reset slide. Both sliders share the 0..500 ms range; integer-stepped
    /// so the readout lands on a whole millisecond.
    widgets::SliderState resetDelay{100.0f, 0.0f, 500.0f, 1.0f};

    /// 滑屏距离(px) [1..2000] — total px the finger travels during a swipe
    /// (and the matching reset). The Bezier progress curve maps 0..1 onto
    /// this distance; the per-frame clamp (see below) bounds how much of
    /// it is realised in a single frame.
    widgets::SliderState slideDistancePx{300.0f, 1.0f, 2000.0f, 1.0f};

    /// 每帧最大移动 [1..400] (px) — hard cap on the per-frame step. The
    /// Bezier curve naturally smooths the velocity around its endpoints,
    /// but a long swipe + tiny cap could still ask for a > cap step in
    /// the middle; this clamp rejects the overshoot and lets the swipe
    /// finish on the next frame instead.
    widgets::SliderState maxPerFramePx{120.0f, 1.0f, 400.0f, 1.0f};

    /// Touch-area overlay (the dashed box on screen).
    BackFlashTouchArea touchArea;

    /// Flash-category filter over the model's classes.
    BackFlashCategoryState category;

    // ── State machine ─────────────────────────────────────────────────────
    enum class Phase {
        Idle,           // waiting for a target
        WaitFlashDelay, // target seen, waiting flashDelay before swiping
        Swiping,        // finger DOWN, sliding to the swipe end
        WaitResetDelay, // swipe done, finger UP, waiting resetDelay
        Resetting,      // finger DOWN, sliding back to the press point
    };
    Phase    phase        = Phase::Idle;
    int64_t  phaseEnterMs = 0;

    // Press position (random point inside the touch area, kept across the
    // whole cycle so the reset knows where to return to).
    float pressX = 0.0f, pressY = 0.0f;

    // Post-swipe position (== reset's start position).
    float swipeEndX = 0.0f, swipeEndY = 0.0f;

    // Current sub-action endpoints (set on entry to Swiping / Resetting,
    // advanced by the per-frame Bezier step). The Bezier progress curve
    // maps onto the segment (curStart..curEnd).
    float curStartX = 0.0f, curStartY = 0.0f;
    float curEndX   = 0.0f, curEndY   = 0.0f;

    // Last uploaded position — used by the per-frame clamp to compute the
    // actual step (the Bezier asks for an offset; the clamp may shrink it).
    float lastX = 0.0f, lastY = 0.0f;

    // Frame budget for the current sub-action. Computed at entry as
    // ceil(distance / maxPerFramePx) — the Bezier curve is slower at
    // the endpoints so this is a slight under-budget; the per-frame clamp
    // covers the rare overshoot.
    int subFrame      = 0;
    int subTotalFrames = 0;

    // Finger currently held down on UINPUT_SLOT_PRIMARY. Every entry to
    // Swiping/Resetting sets it true with a uinput_down; every exit to
    // Idle (through WaitResetDelay or via a gate failure) clears it with
    // a uinput_up. cancelFinger() centralises the "lift if down" path so
    // a gates-closed mid-cycle doesn't leave a phantom finger pressed.
    bool fingerDown = false;

    /// True for the entire cycle (WaitFlashDelay..Resetting). syncAimPage()
    /// reads this through backflashIsBlockingAim() and lifts its own finger
    /// when it is true, so back-flash and aim never share UINPUT_SLOT_PRIMARY.
    /// Cleared when the cycle returns to Idle.
    bool blockingAim = false;

    // LCG for the press-point random and the ±4 px END jitter. Same cheap
    // deterministic generator the Aim page uses — adequate for randomness
    // that just needs to not look identical every cycle.
    uint32_t rng = 0xC0FFEEu;
};

extern PageBackFlash g_pageBackFlash;

/// Draw the Back-Flash page starting at `(x, *y)`, advancing `*y` past every
/// row. `sc` carries the per-page scroll offset.
void drawBackFlashSection(ImDrawList* dl, float x, float& y, float w,
                          float bottomY, float s, float es, const Xf& xf,
                          Scroll& sc);

/// Draw the Back-Flash page's screen-space overlays (touch-area dashed box),
/// including drag/resize. Called from drawHud() every frame, regardless of
/// menu visibility — same rule as the Aim/Trigger overlays.
void drawBackFlashOverlays();

/// Per-frame state-publishing hook. Syncs the flash-category list, runs the
/// phase machine (Idle / WaitFlashDelay / Swiping / WaitResetDelay /
/// Resetting), and writes uinput_down / uinput_move / uinput_up on
/// UINPUT_SLOT_PRIMARY. Sets `blockingAim=true` for the entire cycle so
/// syncAimPage() yields the slot. Called from drawHud() BEFORE syncAimPage()
/// so the blocking flag is set before aim checks it.
void syncBackFlashPage();

/// True while the back-flash state machine is mid-cycle (any phase other
/// than Idle). Read by syncAimPage() — when true, aim lifts its finger and
/// refuses to drive a new one. Backed by `g_pageBackFlash.blockingAim`.
bool backflashIsBlockingAim();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
