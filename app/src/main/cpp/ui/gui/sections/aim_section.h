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

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ui/gui/widgets.h"
#include "tracking/pid_controller.h"

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

// ── Aim controller — two per-step position controllers, one per axis ────────
//
// The output of PPID::update() is the FINGER DISPLACEMENT for one control step,
// in px — not a velocity, and nothing divides by a nominal frame rate. The step
// time is real and is passed in as `dt`; the loop genuinely runs at 120 Hz
// while aiming and drops to 60 Hz in the detector tier.
//
// Deadzone and per-class Y band are applied AFTER the control step, so the
// controller always sees the raw error. The output ceiling and the derivative
// filter are constants inside PPID (see tracking/pid_controller.h), not sliders.
struct AimController {
    tracking::PPID pidX;
    tracking::PPID pidY;

    // Drag safety: lift+repress if finger travels further than this from press point.
    float maxDragDist = 400.0f;  // px

    /// Push slider values into both axes. PURE ASSIGNMENT — it must never reset
    /// anything. It is called once per render frame, so a version that cleared
    /// state (this used to be `init()`, which ended in reset()) would wipe the
    /// integrator and the derivative history 120 times a second. A genuinely
    /// new engagement calls reset() instead.
    ///
    /// There is NO sensitivity compensation here any more and no scaling of the
    /// gains or of the ceilings: kp, ki, kd and kf go in as they are.
    ///
    /// `kf` is the feed-forward STRENGTH, 0…0.20 — it is NOT the plant gain any
    /// more. The reconstruction constant is derived on-line (see `alphaEst`) and
    /// pushed into both axes; round 10's demand that the user calibrate kf
    /// against their game's sensitivity is what made a small kf actively worse
    /// than zero. See the header of tracking/pid_controller.h.
    void setGains(float kp, float ki, float kd, float outSmooth, float kf) {
        this->ffGain = std::max(0.0f, kf);
        pidX.setGains(kp, ki, kd, outSmooth, kf);
        pidY.setGains(kp, ki, kd, outSmooth, kf);
    }

    /// The engaged target's box width in screen px. Feeds the GATE — the
    /// reference PID's trick of expressing "far off" in target widths rather than
    /// pixels, so the same number means the same thing at every range. Call once
    /// per frame before step(); the controller holds it until it changes.
    void setTargetBoxPx(float px) {
        pidX.setTargetBoxPx(px);
        pidY.setTargetBoxPx(px);
    }

    /// Drive both axes, return the total displacement in px for THIS control
    /// step. The caller only has to add the player's own fusion assist on top.
    ///
    /// `dt` is the real step time in seconds and it is used, not decorative: the
    /// integral and derivative terms are both scaled by it, and the control loop
    /// genuinely changes rate (120 Hz while aiming, 60 Hz in the detector tier).
    ///
    /// `freezeX` / `freezeY` are the caller's per-axis deadzone decisions, and
    /// the caller must NOT zero the returned value afterwards. Inside the band
    /// the controller itself suppresses the proportional and derivative terms
    /// and holds the integrator while still adding what it has already learned,
    /// so a slowly drifting target is still followed rather than being left
    /// behind and then snapped after — the limit cycle the per-axis freeze was
    /// added for.
    ///
    /// `targetChanged` says the caller picked a DIFFERENT track this step. The
    /// caller knows (it has the track id); the controller cannot tell a switch
    /// from fast motion by looking at the error alone, and guessing wrong cost
    /// the integral that damps a large swing. Both axes are resynced together —
    /// a switch changes both.
    void step(float errX, float errY, float dt, bool freezeX, bool freezeY,
              bool targetChanged, float& outX, float& outY) {
        // The delay-aligned command must be read BEFORE update() shifts the ring:
        // it is the u(k−L) that produced the error we are about to act on, and it
        // is the regressor's increment in the plant-gain fit.
        const float uDelayed = pidX.delayedCommand();

        outX = pidX.update(errX, dt, freezeX, targetChanged);
        outY = pidY.update(errY, dt, freezeY, targetChanged);

        // The plant gain is a property of the game, not of an axis, so one fit on
        // X serves both. It is off the critical path: the whole kf range stays
        // stable with alpha_hat wrong by 2x in either direction, and `value()`
        // never returns anything unset.
        alphaEst.push(errX, uDelayed);
        if (alphaEst.valid()) {
            const float a = alphaEst.value();
            pidX.setAlphaHat(a);
            pidY.setAlphaHat(a);
        }
    }

    /// The estimated plant gain in force (alpha_hat), for the log. Compare it
    /// against the box size and the reachable view speed to sanity-check the
    /// gains: the P-loop's useful ceiling is kp·alpha < 0.285 at this delay.
    float alphaHatValue() const { return pidX.alphaHatValue(); }
    /// True once the plant-gain fit has ever converged. For the log.
    bool  alphaValid() const { return alphaEst.valid(); }

    /// Trim of each axis, for the diagnostic log. See PPID::integralValue() —
    /// it is the RESIDUAL cleaner while the feed-forward runs, so it should stay
    /// small; a large trim together with a small `ff` on a moving target means
    /// kf is off, and roughly by their ratio.
    float trimX() const { return pidX.integralValue(); }
    float trimY() const { return pidY.integralValue(); }

    /// The feed-forward's contribution to each axis, in px. THE number to watch
    /// on a moving target: it should settle at ΔT/alpha and hold there. Near
    /// zero while the error trails the target means the feed-forward is not
    /// running (kf = 0, or every frame reports targetChanged).
    float ffX() const { return pidX.ffValue(); }
    float ffY() const { return pidY.ffValue(); }

    /// The damping term's contribution to each axis's output, in px, for the
    /// diagnostic log. A D contribution that is always ~0 while the target
    /// strafes means kd is not doing anything; one that is comparable to the
    /// trim means it is carrying the loop and kp can be raised.
    float derivX() const { return pidX.derivPx(); }
    float derivY() const { return pidY.derivPx(); }

    void reset() {
        pidX.reset();
        pidY.reset();
        // NOT alphaEst: the plant gain belongs to the game, not to the engagement.
        // Wiping it here would throw away a converged fit every time the finger is
        // lifted, and the FF would spend the next 0.75 s back at the 1.0 default.
    }

    /// The feed-forward gain in force (== kf == FF strength, 0…0.20), for the log.
    float ffGainValue() const { return ffGain; }
    /// The output ceiling in force (finger px/step). A constant.
    float outLimitPx() const { return pidX.outLimitPx(); }

private:
    float ffGain = 0.20f;
    /// One plant-gain fit, shared by both axes (alpha is the game's sensitivity,
    /// not an axis property).
    tracking::AlphaEstimator alphaEst;
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

    /// Last physical finger position seen, in screen px. Drives the one-way
    /// fusion assist: (current - lastPhysical) is the player's movement this
    /// frame, of which only the component along the aim direction is kept.
    /// -1 means "no physical finger was tracked last frame" — the delta is
    /// meaningless across that boundary and must not be computed.
    float lastPhysX = -1.0f, lastPhysY = -1.0f;

    /// Set for ONE frame after a fusion takeover is released (real finger lifted /
    /// swapped). Lets the Idle branch skip spawning a synthetic finger that frame,
    /// so we don't get a phantom "extra touch point" the instant a real finger
    /// leaves the touch area. Cleared when read, and on press()/release().
    bool justReleasedTakeover = false;

    /// Set by the drag-safety lift so the re-press that follows does NOT reset
    /// the controller.
    ///
    /// Drag safety lifts the synthetic finger once it strays more than
    /// `maxDragDist` (400 px) from its press point, and the next frame presses a
    /// new one at the touch-area centre. That is a re-anchor of WHERE THE FINGER
    /// SITS, not a new engagement: the target, its velocity and the game's
    /// sensitivity are all unchanged, so the controller's trim is still valid and
    /// must survive. It did not — press() called reset() unconditionally, which
    /// on a 400 px budget at 720 px/s means the integrator was wiped roughly
    /// twice a second, so it could never hold the trim that removes the trailing
    /// error. Cleared by press() when consumed, and by release() so that a
    /// drag-safety lift immediately followed by a target loss cannot leave the
    /// flag set for the NEXT engagement.
    bool carryTrim = false;

    /// Track id the aim was engaging last control step, or -1 for "none".
    ///
    /// This is how the controller is told a switch happened. It replaces a
    /// magnitude heuristic that lived inside the controller ("the error jumped
    /// more than 220 px"), and that heuristic was wrong in both directions: it
    /// fired on any large legitimate swing — which is precisely what a limit
    /// cycle looks like, so it wiped the integral that would have damped the
    /// swing, twice per period — and it missed the common case of a switch
    /// between two adjacent enemies, whose centres are only ~100 px apart.
    /// The caller has the track id; the controller never did. Cleared by
    /// release() so a new engagement never inherits a stale id.
    int lastTargetId = -1;

    /// Where the last engaged target WAS, and how big it was, in screen px.
    ///
    /// Needed because the track id alone is no longer a trustworthy test for
    /// "did the aim switch enemies". Identity is genuinely unstable in two
    /// situations that are NOT switches — a head box and a body box of the same
    /// enemy are two tracks (NMS is per class, so one enemy legitimately yields
    /// two boxes), and a track that has to be re-acquired can come back under a
    /// new id. Treating either as a switch clears the integrator, and doing that
    /// several times a second is the "永远滞后" half of the reported symptom:
    /// the trim can never build the steady-state velocity that removes a
    /// trailing error. So the id change is confirmed against POSITION — a real
    /// enemy switch moves the aim point by a whole box or more, an id flicker
    /// does not.
    float lastTargetCx  = -1.0f;
    float lastTargetCy  = -1.0f;
    float lastTargetBox = 0.0f;   // 0.5*(w+h), the yardstick for "that far away"

    /// Drops a finger at the TOUCH-AREA centre ± jitter (the finger's home,
    /// where the virtual thumb rests — matching the old project's
    /// AimController, which seeds at the aim area). The aim direction is
    /// independent of this point because the PID error reference is the SCREEN
    /// CENTRE, never the live finger (that mismatch was the "screen-centre"
    /// bug). Records `start` so drag safety can later lift if the finger
    /// travels too far. Caller is responsible for the uinput_down() that
    /// immediately follows.
    ///
    /// Resets the controller UNLESS `carryTrim` is set — see that flag above.
    void press(const TouchAreaOverlay& area);

    /// Releases the synthetic finger. Caller is responsible for the matching
    /// uinput_up() — same separation as press().
    void release();
};

/// True while the aim is actually driving a finger. The render loop uses it to
/// decide whether the overlay needs the full 120 Hz rate: syncing at 60 Hz while
/// a 120 fps game is being tracked makes the finger advance in 12 px steps at
/// 720 px/s (visible as stepping) and doubles the loop's delay in seconds. The
/// smoothness the user is asking for is bought exactly here, and it costs
/// nothing when nobody is aiming because the tier falls back on its own.
bool aimIsDriving();

// ── The Aim page itself ──────────────────────────────────────────────────
//
// One struct holds everything drawAimSection() needs to render and
// syncAimPage() needs to drive the PIDF loop. No globals beyond `g_pageAim`.
struct PageAim {
    widgets::SwitchState enabled{true};

    /// ── Controller parameters ─────────────────────────────────────────────
    ///
    /// All three gains are in ONE consistent unit system — per control step —
    /// and the defaults below were chosen by sweeping the loop over the two
    /// quantities nobody knows: alpha (the game's sensitivity, view px per
    /// finger px) and L (the loop's delay, in control steps). See
    /// scripts/aim_screenvel_check.py and the header of
    /// tracking/pid_controller.h.
    ///
    /// Kp (0–0.6, per STEP — "the fraction of the gap to cover this step"):
    /// a discrete proportional loop with L steps of delay is stable while
    /// kp*alpha < 2·sin(π/(2(2L+1))) — 0.45 at L=4, 0.30 at L=6, where alpha is
    /// the game's own sensitivity in view px per finger px. Nothing compensates
    /// for that any more, so a HIGH-sensitivity game is expected to lower kp —
    /// and at alpha above ~1.5 it must, or the loop rings no matter what kf is.
    /// The shipped 0.10 is 4x inside the bound at alpha = 1.
    ///
    /// Ki (0–4, px of output per px-of-error-second): the term that erases the
    /// standing error a moving target would otherwise need. It is the DC carrier
    /// when the feed-forward is off, and a residual cleaner when it is on — so
    /// 0.5 is the measured optimum with kp = 0.10 and is not a small mop-up value.
    ///
    /// Kd (0–2, PER STEP, dimensionless): the filtered weight on the error's
    /// one-step change. Damping, and the only currency a delay-limited loop has
    /// for buying phase margin. Measured optimum is kd ≈ 2·kp; going past that
    /// is how a high-alpha game starts to ring, for the same reason kp is.
    ///
    /// 输出平滑 (outSmooth): a REAL output EMA, 1.0 = off. Off by default: it
    /// adds phase lag, and phase lag is the one thing a delay-limited loop
    /// cannot afford. Lower it for a gentler-looking motion and accept the
    /// extra latency. Measured, 0.8 against 1.0 differs in the decimal places.
    ///
    /// 输出限幅 used to live here as a slider. It was never a tuning parameter —
    /// it is the ceiling that stops a re-lock from flinging the finger across
    /// the panel — so it is the constant tracking::kOutLimitPx (180 px/step,
    /// tanh) and the page draws no row for it. The integral's leash is the
    /// constant tracking::kTrimLimitPx (90). Neither is scaled by anything:
    /// kf is the only place the plant gain enters, and it lives in its own row.
    widgets::SliderState kp{0.10f, 0.0f, 0.6f, 0.01f};
    widgets::SliderState ki{0.5f, 0.0f, 4.0f, 0.1f};
    widgets::SliderState kd{0.20f, 0.0f, 2.0f, 0.05f};
    widgets::SliderState outSmooth{1.0f, 0.0f, 1.0f, 0.05f};

    /// Lead, in detector frames: how far ahead of its tracked centre the aim
    /// places the target, to pay for the delay between the screenshot the
    /// detector saw and the touch the game will receive. Applied as
    /// (target.x, target.y) += (vx, vy) * this, using the tracker's filtered
    /// velocity.
    ///
    /// ⚠ DEFAULT IS NOW 0.0, and the reason matters: this term enters the output
    /// through the proportional gain, so in steady state it adds Kp*v*this to
    /// the finger's motion — which is the SAME channel as 速度前馈. With both at
    /// their old defaults (1.0 here, 1.0 there) the loop was adding ≈1.5x the
    /// target's velocity when 1.0x is what makes the crosshair travel with it,
    /// and the surplus showed up as overshoot on every moving target. Two knobs
    /// pushing on one term is exactly the "I tuned one and the other fought it"
    /// trap; 速度前馈 is now the single velocity knob and this one is the
    /// optional extra. See the Kp/Ki note above — with a working integrator the
    /// lag is already handled, so starting at 0 loses nothing.
    ///
    /// Range 0–5, step 0.05. Raise it if the crosshair trails a target that is
    /// steadily moving; lower it (or zero it) if the crosshair overshoots one
    /// that changes direction — a lead is a bet that the target keeps going.
    widgets::SliderState aimDelayFrames{0.0f, 0.0f, 5.0f, 0.05f};

    /// Kalman forward-prediction window: how many consecutive missed frames
    /// the tracker continues to push the predicted position along (vx, vy)
    /// before freezing at last_valid. 0 freezes instantly (the pre-2026
    /// behaviour), 3 ≈ 50 ms covers a brief occlusion, 5 ≈ 83 ms covers
    /// ducking behind cover. The track's lifetime is automatically extended to
    /// cover this window (see TrackerConfig::predictHoldFrames), so the value
    /// you set here is the value you get — it is no longer silently capped by
    /// 丢失帧. Range 0–30, step 1 (30 ≈ 0.5 s of dead reckoning).
    widgets::SliderState trackPredictHoldFrames{3.0f, 0.0f, 30.0f, 1.0f};

    /// kf — the velocity feed-forward STRENGTH. NOT the plant gain any more.
    ///
    /// ROUND 10 WAS WRONG, and the way it was wrong is worth keeping. It asked
    /// for kf = 1/alpha, where alpha is the game's sensitivity in view px per
    /// finger px. That is algebraically right and practically a trap, because the
    /// number then carries two jobs at once — the STRENGTH of F and the
    /// RECIPROCAL of the reconstruction constant — and expanding the formula
    /// shows what that costs:
    ///
    ///     ff = kf·LPF(Δe) + LPF(u(k−L))            ← second term has gain 1
    ///
    /// The controller was adding its own delayed command back into its input.
    /// That is a pole at z = 1: an accelerator with no brake. It "tracked"
    /// (a DC pole integrates a steady target's motion to zero error) and it never
    /// settled (the same pole is marginally stable, so noise keeps it moving),
    /// and it OVERSHOT when the target stopped — with ki = 0. Those three are one
    /// mechanism, which is why every attempt to tune them apart failed.
    ///
    /// NOW. The reconstruction constant is derived on-line (AlphaEstimator) and
    /// kf is only a strength, 0…0.20:
    ///
    ///     ff = (kf/alpha_hat)·LPF( Δe + alpha_hat·u(k−L) )
    ///
    /// Residual self-term = kf·(1 − alpha/alpha_hat)·u(k−L), so a small kf is now
    /// genuinely the safe choice (the opposite of round 10). Measured across
    /// alpha 0.5…2.5 with alpha_hat off by 0.4×…2.0×: at kf = 0.20 the lag is
    /// −5 px, the sway 5–11 px and the post-stop overshoot 3–6 px in every cell
    /// (scripts/aim_selftrap_bench.py 表8). That robustness is why the range
    /// stops at 0.20.
    ///
    /// HOW TO SET IT. Watch the error while the target strafes at a constant
    /// speed: one sign that persists = the crosshair trails = raise kf. Sway on a
    /// STILL target at any kf means the problem is not kf at all — check
    /// kp × alpha_hat (shown in the log) against 0.285, because that is the
    /// delay's hard ceiling and no feed-forward setting will fix it.
    ///
    /// 0 turns the feed-forward off exactly and leaves the integral as the sole
    /// DC carrier. Safe everywhere; it trails a fast strafe.
    ///
    /// Range 0.00–0.20, step 0.01, default 0.20 (full strength). The range is
    /// sized to the robust band on purpose — see tracking/pid_controller.h.
    widgets::SliderState ffGain{0.20f, 0.0f, 0.20f, 0.01f};


    /// Aim deadzone (0.0–1.0, one decimal): ports the old project's
    /// `convergeThresh`. Once the TARGET is within `deadzone` of the screen
    /// centre (crosshair) the finger stops nudging and holds steady, instead of
    /// twitching around trying to land exactly on a jittering box. 0.0 = no
    /// deadzone (must converge to the pixel), 1.0 = stop as soon as the target
    /// is within ~8% of the shorter screen edge of the crosshair.
    widgets::SliderState deadzone{0.0f, 0.0f, 1.0f, 0.1f};

    /// Touch-area overlay (the dashed box on screen).
    TouchAreaOverlay touchArea;

    /// Touch fusion — ON: aim drives a REAL finger, and no new touch point is
    /// created. If a physical finger is held inside the touch area, aim takes
    /// over that finger's mirror slot and moves it directly: the aim increment
    /// is added straight onto the finger's own coordinates. The player keeps
    /// control of the FORWARD direction only — finger movement pointing the
    /// same way as the aim is added to it (the finger helps), movement pointing
    /// the other way is discarded (it can never fight the aim).
    ///
    /// OFF: aim presses a separate synthetic finger at the touch-area centre;
    /// the player's own finger is mirrored to the game untouched. No takeover.
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
/// `sc` carries the per-page scroll offset: paint coordinates and hit-test
/// rectangles go through `sc.screenY(y)` so they sit at the right visual
/// position, but `*y` continues to advance in *natural* (un-scrolled)
/// coordinates so the caller can compute `sc.maxOffset` from how far `*y`
/// ran. The natural-Y convention is what makes the Aim overlay drags (the
/// touch-area dashed box and trigger-area circle) keep working unchanged —
/// they live in screen pixels, not menu pixels, so scroll never touches them.
void drawAimSection(ImDrawList* dl, float x, float& y, float w,
                    float bottomY, float s, float es, const Xf& xf,
                    Scroll& sc);

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
