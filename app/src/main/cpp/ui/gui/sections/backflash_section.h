// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::backflash — the Back-Flash page (背闪).
//
//  When the model spots a "flash" target (the player's screen is being
//  whited-out by an enemy grenade) or when a full-screen whiteout is detected,
//  the player wants to turn AWAY from the flash so their character model is not
//  lit up when the screen comes back.
//  This page drives that gesture using a multi-harmonic curve for human-like
//  movement, brightness fallback detection, settle phases, and cooldowns.
//
//  Layout, top-to-bottom on the right pane:
//    1. 亮度兜底检测     — switch to enable fallback brightness detection
//    2. 背闪延迟  (ms)   — wait this long after spotting the flash
//    3. 复位延迟  (ms)   — wait this long between the swipe and the reset
//    4. 滑屏距离(px)     — total px the finger travels during a swipe
//    5. 每帧最大移动     — per-frame movement cap
//    6. 滑动速度(px/s)   — controls how fast the swipe moves
//    7. 轨迹不规则度     — how irregular the path is (wobble)
//    8. 垂直分量         — vertical component ratio during swipe
//    9. 随机抖动(px)     — per-frame random jitter
//   10. 停顿时间(ms)     — pause after reaching the end before resetting
//   11. 冷却时间(ms)     — minimum time between two triggers
//   12. 亮度阈值         — average screen brightness threshold
//   13. 高亮像素阈值     — single pixel brightness level to count as bright
//   14. 高亮占比阈值     — ratio of bright pixels needed to trigger
//   15. 触摸区域         — dashed-box overlay (drag/resize, same shape
//                          as the Aim page's touch area)
//   16. 闪光类别         — multi-select over the model's class names
//
//  Touch lifecycle per detected flash (one full cycle = one swipe + one
//  reset; "press / slide / lift" repeats):
//
//    WaitFlashDelay            nothing on screen
//      ↓ wait flashDelay
//    Swiping (finger DOWN)     press at a random point inside the touch area,
//                              slide (multi-harmonic progress curve, per-frame
//                              movement clamped) to the computed END on the
//                              axis pointing AWAY from the flash, recording
//                              waypoints along the way.
//                              Lift at END.
//      ↓
//    Settling (finger UP)      wait settleMs and resetDelay
//      ↓
//    Resetting (finger DOWN)   press at END (the post-swipe position), slide
//                              back consuming recorded waypoints in reverse
//                              to ensure exact inverse path. Lift at start.
//      ↓
//    Cooldown                  wait cooldownMs
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

    /// 亮度兜底检测 — enable brightness-based fallback detection.
    widgets::SwitchState brightDetect{true};

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

    /// 每帧最大移动 [1..1500] (px) — hard cap on the per-frame step.
    widgets::SliderState maxPerFramePx{300.0f, 1.0f, 1500.0f, 10.0f};

    /// 滑屏速度上限(px/s) [500..60000] — controls how fast the swipe moves.
    widgets::SliderState swipeSpeed{12000.0f, 500.0f, 60000.0f, 500.0f};

    /// 轨迹不规则度 — how irregular the path is (0=straight, 1=very wavy).
    widgets::SliderState wobble{0.40f, 0.0f, 1.0f, 0.05f};

    /// 垂直分量 — vertical component ratio during swipe.
    widgets::SliderState pitchRatio{0.12f, 0.0f, 0.5f, 0.01f};

    /// 随机抖动(px) — per-frame random jitter.
    widgets::SliderState jitterPx{1.5f, 0.0f, 10.0f, 0.5f};

    /// 停顿时间(ms) — pause after reaching the end before resetting.
    widgets::SliderState settleMs{80.0f, 0.0f, 500.0f, 10.0f};

    /// 冷却时间(ms) — minimum time between two triggers.
    widgets::SliderState cooldownMs{2500.0f, 500.0f, 10000.0f, 100.0f};

    /// 亮度阈值 — average screen brightness threshold for whiteout detection.
    widgets::SliderState brightThreshold{0.86f, 0.5f, 1.0f, 0.01f};

    /// 高亮像素阈值 — single pixel brightness level to count as "very bright".
    widgets::SliderState brightPixelLevel{0.90f, 0.5f, 1.0f, 0.01f};

    /// 高亮占比阈值 — ratio of bright pixels needed to trigger.
    widgets::SliderState brightBlobRatio{0.22f, 0.01f, 0.5f, 0.01f};

    /// Touch-area overlay (the dashed box on screen).
    BackFlashTouchArea touchArea;

    /// Flash-category filter over the model's classes.
    BackFlashCategoryState category;

    // ── State machine ─────────────────────────────────────────────────────
    enum class Phase {
        Idle,           // waiting for a target
        WaitFlashDelay, // target seen, waiting flashDelay before swiping
        Swiping,        // finger DOWN, sliding along multi-harmonic curve
        Settling,       // NEW: finger UP, waiting settleMs (模拟真人停顿)
        Resetting,      // finger DOWN, sliding back using recorded waypoints
        Cooldown,       // NEW: cycle complete, waiting cooldownMs
    };
    Phase    phase        = Phase::Idle;
    int64_t  phaseEnterMs = 0;

    // Press position (random point inside the touch area, kept across the
    // whole cycle so the reset knows where to return to).
    float pressX = 0.0f, pressY = 0.0f;

    // Post-swipe position (== reset's start position).
    float swipeEndX = 0.0f, swipeEndY = 0.0f;

    // Last uploaded position — used by the per-frame clamp to compute the
    // actual step (the Bezier asks for an offset; the clamp may shrink it).
    float lastX = 0.0f, lastY = 0.0f;

    // ── Multi-harmonic curve state ──────────────────────────────────────
    // Random phases for the 3 harmonics, generated at cycle start.
    double harm1Phase = 0.0, harm2Phase = 0.0, harm3Phase = 0.0;
    // Vertical drift amplitude for this cycle (±3% of slideDistancePx).
    double driftY = 0.0;
    // Total intended travel and current progress along the curve.
    double totalForward = 0.0;
    double forwardPos   = 0.0;
    // Direction sign: +1 = swipe right (flash on left), -1 = swipe left.
    double dirSign = 1.0;

    // ── Lossless accumulator (记账式无损累积器) ──────────────────────────
    // Tracks the exact intended position vs. what was actually sent to
    // uinput, ensuring the reset phase returns the finger to EXACTLY the
    // press point with zero pixel residual. The fractional remainder from
    // integer truncation is preserved across frames.
    double intentX = 0.0, intentY = 0.0;  // cumulative intended offset from press
    int    sentX   = 0,    sentY   = 0;    // cumulative integer px actually sent

    // ── Waypoint recording (航点录制) ───────────────────────────────────
    // During Swiping, each frame's geometric step (dx, dy) is recorded.
    // During Resetting, waypoints are consumed in reverse (LIFO) to
    // ensure the finger returns along the exact inverse path.
    std::vector<float> waypointDx;
    std::vector<float> waypointDy;
    size_t backIdx    = 0;     // current reverse-playback index
    double backShift  = 0.0;   // partial consumption within one waypoint
    bool   backDone   = false;  // all waypoints consumed

    // ── Brightness detection (亮度检测) ─────────────────────────────────
    // Results from the last frame's brightness sampling, used by the state
    // machine as a fallback trigger when YOLO doesn't detect a flash.
    float lastBrightFraction = 0.0f;  // ratio of "very bright" pixels
    float lastMeanLuma       = 0.0f;  // average screen brightness [0,1]

    // ── Cooldown ────────────────────────────────────────────────────────
    int64_t lastTriggerMs  = 0;  // timestamp of last trigger completion
    int64_t cycleStartMs   = 0;  // when the current cycle started

    // ── Time tracking ───────────────────────────────────────────────────
    int64_t lastFrameMs = 0;  // timestamp of last frame for dt computation

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
/// phase machine (Idle / WaitFlashDelay / Swiping / Settling /
/// Resetting / Cooldown), and writes uinput_down / uinput_move / uinput_up on
/// UINPUT_SLOT_PRIMARY. Sets `blockingAim=true` for the entire cycle so
/// syncAimPage() yields the slot. Called from drawHud() BEFORE syncAimPage()
/// so the blocking flag is set before aim checks it.
void syncBackFlashPage();

/// True while the back-flash state machine is mid-cycle (any phase other
/// than Idle). Read by syncAimPage() — when true, aim lifts its finger and
/// refuses to drive a new one. Backed by `g_pageBackFlash.blockingAim`.
bool backflashIsBlockingAim();

/// Samples the latest capture frame's brightness. Called from syncBackFlashPage()
/// to provide fallback flash detection when the model fails to identify a flash
/// (e.g. full-screen whiteout). Uses step-16 interval sampling for minimal CPU cost.
void sampleFrameBrightness(float& outBrightFraction, float& outMeanLuma);

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
