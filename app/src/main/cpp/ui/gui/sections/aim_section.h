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
// ROUND 22: the PIDF of rounds 1–21 is replaced by the predictive controller.
// The old header is still on disk (tracking/pid_controller.h) and is no longer
// included by anything, so reverting this one line reverts the whole round.
#include "tracking/predictive_pid.h"

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

// ── Aim controller — two predictive controllers, one per axis ───────────────
//
// ROUND 22 — WHAT THIS IS NOW. Two `tracking::PredictivePid` objects, and the
// page is a thin adapter over them. Everything the old AimController carried
// besides the two PIDs is gone, and each removal is worth naming because each
// one was a source of the faults the user reported:
//
//   * `alphaEst` / `slopeEst` / `alphaTrusted_` — the on-line plant-gain fits.
//     The plant gain no longer appears in any denominator anywhere in the loop
//     (see the header of tracking/predictive_pid.h), so there is nothing for a
//     fit to feed. Removing them removes the TIME-VARYING GAIN, which is a
//     jitter source that cannot be tuned away.
//   * `kfEff_` and the kf guard — the guard existed only because kf was a gain
//     on an echo whose size depended on the (untrusted) estimate. The
//     equivalent knob now has a provable margin of its own (lookahead < 1), so
//     it needs no guard and cannot be pushed into an unstable region.
//   * `carry` / the stop detector / the deadzone bleed — all three were
//     mechanisms for moving the INTEGRAL toward a measured target. The
//     integral is no longer the carrier (the prediction term is), so it does
//     not need to be steered; it needs to be droppable, and it is.
//
// The output of update() is still the FINGER DISPLACEMENT for one control step,
// in px, and the step time is still the real `dt`.
//
// Deadzone and per-class Y band are still applied by the caller, and still reach
// the controller as the per-axis `frozen` flags — where they suppress P and D
// and pause the integral's accumulation, and NEVER touch the prediction term.
struct AimController {
    tracking::PredictivePid pidX;
    tracking::PredictivePid pidY;

    // Drag safety: lift+repress if finger travels further than this from press point.
    float maxDragDist = 400.0f;  // px

    /// Push slider values into both axes. PURE ASSIGNMENT — it must never reset
    /// anything. It is called once per render frame, so a version that cleared
    /// state would wipe the integrator and the velocity memory 120 times a
    /// second. A genuinely new engagement calls reset() instead.
    ///
    /// kp / ki / kd keep the units the page has always used — px of output per
    /// px of error, per second, and per step — so a value arrived at by tuning
    /// is still that value here.
    ///
    /// The fifth argument is 前馈 (lookahead). Its TYPE of quantity changed in
    /// round 22 even though the row kept its name and its place: it was a
    /// dimensionless strength on a feed-forward whose other factor was
    /// 1/alpha_hat, and it is now the FRACTION OF THE CARRIER the prediction
    /// term supplies — a number with a hard meaning and a hard ceiling
    /// (tracking::kLookaheadMax, 0.80), because the feed-forward is a delayed
    /// copy of our own output with coefficient exactly `lookahead`, so
    /// `lookahead < 1` IS the stability margin. See the header of
    /// tracking/predictive_pid.h.
    ///
    /// No guard, no rate limit, no latch: with the ceiling at 0.80 every
    /// position of this slider is safe, so the knob goes straight through.
    void setGains(float kp, float ki, float kd, float outSmooth, float ff) {
        pidX.setParams(kp, ki, kd, outSmooth, ff);
        pidY.setParams(kp, ki, kd, outSmooth, ff);
    }

    /// The engaged target's box width in screen px. Feeds the jump thresholds,
    /// which are expressed as a fraction of a target rather than as a fixed
    /// number of pixels so a distant 20 px box and a contact-range 300 px box
    /// are judged the same way. Call once per frame before step().
    void setTargetBoxPx(float px) {
        pidX.setTargetBoxPx(px);
        pidY.setTargetBoxPx(px);
    }

    /// Drive both axes, return the total displacement in px for THIS control
    /// step. The caller only has to add the player's own fusion assist on top.
    ///
    /// `dt` is the real step time in seconds and it is used, not decorative: the
    /// loop genuinely changes rate (120 Hz while aiming, 60 Hz in the detector
    /// tier) and the derivative, the reconstruction and the integral are all
    /// normalised by it.
    ///
    /// `freezeX` / `freezeY` are the caller's per-axis deadzone decisions, and
    /// the caller must NOT zero the returned value afterwards: inside the band
    /// the prediction term is STILL ADDED, which is the one term that follows a
    /// moving target. Round 15 suppressed the integral inside the band instead
    /// and with kf = 0 that zeroed the axis outright, which read on the device
    /// as 乱甩乱晃.
    ///
    /// `targetChanged` says the caller picked a DIFFERENT track this step. The
    /// caller knows (it has the track id); the controller cannot tell a switch
    /// from fast motion by looking at the error alone.
    ///
    /// `measurementCorrected` says the TRACKER re-associated this step — it
    /// pulled the published centre back onto a detector result one render frame
    /// old instead of letting the model advance it by its own velocity. The
    /// controller must be told and cannot infer it: the artifact is
    /// proportional to our own output, so from the error alone it is
    /// indistinguishable from a genuinely fast target. The controller responds
    /// by HOLDING its velocity estimate for one frame rather than updating it
    /// from a difference that is not a velocity.
    void step(float errX, float errY, float dt, bool freezeX, bool freezeY,
              bool targetChanged, bool measurementCorrected,
              float& outX, float& outY) {
        outX = pidX.update(errX, dt, freezeX, targetChanged, measurementCorrected);
        outY = pidY.update(errY, dt, freezeY, targetChanged, measurementCorrected);
    }

    /// The reconstructed target velocity each axis is working from, in SCREEN
    /// px per frame. THE number to watch: it should settle at the target's own
    /// speed and hold there, track sign changes cleanly, and read ~0 when the
    /// target is standing. A value that keeps flipping sign while the crosshair
    /// sways means the prediction term is chasing its own echo — read it with
    /// `trust` (below) and lower 前馈.
    float velX() const { return pidX.velValue(); }
    float velY() const { return pidY.velValue(); }

    /// The prediction term's contribution to each axis's output, in finger px.
    /// It should settle at `lookahead × u_ss` on a moving target and hold, which
    /// at the shipped 0.80 is most of the command. Near zero on a strafing
    /// target while the velocity above is large means the trust gate has closed
    /// (see below) or 前馈 is too low.
    float ffX() const { return pidX.ffValue(); }
    float ffY() const { return pidY.ffValue(); }

    /// The residual cleaner's contribution, finger px — the term that USED to be
    /// the carrier and is now a small mop-up value. Large and growing on a
    /// moving target means 前馈 is not supplying the carrier; a value that
    /// survives the target stopping means the error never read 0, so check `e`
    /// before touching ki.
    float trimX() const { return pidX.integralValue(); }
    float trimY() const { return pidY.integralValue(); }

    /// The P and D contributions, finger px.
    float propX() const { return pidX.pValue(); }
    float propY() const { return pidY.pValue(); }
    float derivX() const { return pidX.dValue(); }
    float derivY() const { return pidY.dValue(); }

    /// Trust in the velocity estimate, 0…1. Collapsing toward 0 means the
    /// estimate is ringing; it BOOSTS the restoring gains (kpScale/kdScale) and
    /// the prediction scale, and it ATTENUATES the integral's authority. ★ It does
    /// NOT gate the prediction term — an earlier version cut that term to a floor
    /// of 0.25 whenever trust dipped, and trust dips hardest exactly when the
    /// aim arrives (|Δv̂| is large on arrival), so the gate removed the command at
    /// the worst possible moment. See the note inside predictive_pid.h.
    float trustX() const { return pidX.trustValue(); }
    float trustY() const { return pidY.trustValue(); }

    /// The output gain in force, 0.35…1.0. A re-lock starts soft (0.45–0.70)
    /// and tightens as the error closes; it is 1.0 at a steady lock, so it costs
    /// nothing when the aim is already on target.
    float outGainX() const { return pidX.outGain(); }
    float outGainY() const { return pidY.outGain(); }

    /// True once enough consecutive velocity samples have agreed on a direction
    /// for a velocity to be published at all. While false the prediction term is
    /// zero — the first few frames of an engagement, by design.
    bool  velReadyX() const { return pidX.velocityReady(); }
    bool  velReadyY() const { return pidY.velocityReady(); }

    void reset() {
        pidX.reset();
        pidY.reset();
    }

    /// The output ceiling in force (finger px/step). A constant.
    float outLimitPx() const { return pidX.outLimitPx(); }
    /// 前馈 in force — the slider's value, unchanged (there is no guard any more).
    float ffGainValue() const { return pidX.lookaheadValue(); }

private:
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
    /// ⚠ ROUND 22 — READ THIS BEFORE THE LONG NOTES BELOW.
    ///
    /// The controller was replaced wholesale (see the header of
    /// tracking/predictive_pid.h). The rows kept their names and their units —
    /// kp/ki/kd are still px of output per px of error, per second and per step
    /// — but the BALANCE they sit in changed completely, so the prose under
    /// each row below describes the OLD loop and is kept only as the record of
    /// how these numbers were arrived at. Where it disagrees with this banner,
    /// this banner wins.
    ///
    /// What changed, in one line each:
    ///   * 前馈 is now the FRACTION OF THE CARRIER the prediction term supplies
    ///     (the same idea as before, but with a real ceiling of 0.80 and no
    ///     1/alpha_hat under it). It carries 80 % of a moving target's command.
    ///   * The INTEGRAL is no longer the carrier. It is a residual cleaner that
    ///     is dropped outright whenever the error reads zero — which is what
    ///     removes "the target stopped and the aim sways".
    ///   * kp can therefore be larger than the old ultra-conservative 0.04: the
    ///     contractive loop gain is still kp·alpha, and with the frozen plant
    ///     model at 0.10 the bound kp·alpha < 0.30 (at L ≈ 6) puts the shipped
    ///     0.40 about 7× inside it.
    ///   * nothing is estimated on-line any more, so none of these values is
    ///     fighting a moving target of its own.
    ///
    /// The gain block is re-seeded once by the schema change (kCtlSchema 13) —
    /// the only revision where that was done for kp/ki/kd as well as for 前馈,
    /// because the loop they were tuned against no longer exists.
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
    /// The shipped 0.04 sits about 7x inside that bound at alpha = 1 and L = 6
    /// (round 18; round 15 shipped 0.10, which was 3x). The margin is deliberately
    /// large because the bound is worst-case in BOTH unknowns at once, and alpha
    /// is estimated on-line rather than known.
    ///
    /// Ki (0–4, px of output per px-of-error-second): the term that erases the
    /// standing error a moving target would otherwise need. It is the DC carrier
    /// when the feed-forward is off, and a residual cleaner when it is on — so
    /// "the right ki" is a function of what kf is supplying, not a constant.
    /// Round 15 measured 0.5 as the optimum with kp 0.10 and kf ≈ 0, and called
    /// that "not a small mop-up value"; round 18 ships 0.12 with kp 0.04 and
    /// kf 0.05, where the feed-forward carries the fast part and the integral only
    /// has to hold the standing carrier. Both are correct readings of the same
    /// rule, and neither number means anything without the others.
    /// Its row steps in 0.01 and shows two decimals: it stepped in 0.1 before,
    /// which made the whole useful range 0.0…0.2 three clicks wide.
    ///
    /// The scheduled gain multiplier applies to I as well as to P (see the
    /// round-13 section of tracking/pid_controller.h): a large error accumulates
    /// at reduced authority, which is what stops the integral from charging up
    /// during a transient and spending the charge when the target stops.
    ///
    /// Kd (0–2, PER STEP, dimensionless): the filtered weight on the error's
    /// one-step change. Damping, and the only currency a delay-limited loop has
    /// for buying phase margin. Measured optimum is kd ≈ 2·kp; going past that
    /// is how a high-alpha game starts to ring, for the same reason kp is.
    ///
    /// 输出平滑 (outSmooth): a REAL output EMA, 1.0 = off. Round 18 ships 0.85 —
    /// NOT off, and that reversal is worth stating plainly rather than leaving to
    /// be found. It adds phase lag, and phase lag is the one thing a delay-limited
    /// loop cannot afford, so this row is a genuine trade and not free
    /// smoothness: 0.85 cuts the step-to-step noise in the finger command (D
    /// differences a noisy signal, and F passes the tracker's noise through) for a
    /// bounded amount of lag. Read it AGAINST the gains, never on its own — if you
    /// raise kd to buy damping and then raise this to hide the jitter, you have
    /// spent the same damping twice.
    ///
    /// Go to 1.0 if the aim feels sluggish or overshoots; lower it if the crosshair
    /// visibly trembles.
    ///
    /// 输出限幅 used to live here as a slider. It was never a tuning parameter —
    /// it is the ceiling that stops a re-lock from flinging the finger across
    /// the panel — so it is the constant tracking::kAimOutLimitPx (180 px/step,
    /// tanh) and the page draws no row for it. The integral's leash is the
    /// constant tracking::kAimTrimLimitPx (150). Neither is scaled by anything:
    /// kf is the only place the plant gain enters, and it lives in its own row.
    ///
    /// ⚠⚠ ROUND 14/15/18 NOTES BELOW ARE HISTORY — kept only for the
    /// discharge-rate arithmetic (the last of the three old paragraphs), which
    /// still describes the code. From round 23 the integral is NOT the carrier
    /// (the prediction term is), "kf" is 前馈, and kp/ki/kd are no longer read
    /// unconditionally from a file of another schema. Read them as history.
    ///
    /// ── ROUND 14: Ki IS A CARRIER, AND THE LEASH WAS STRANGLING IT ───────────
    ///
    /// The command a loop must HOLD on a target moving w view px per step is
    /// u_ss = w/alpha — which at alpha = 0.1 is ten times w, not a tenth of the
    /// output ceiling. So the old leash of 90 finger px/step was never a safety
    /// rail; it was a hard ceiling on how fast a target the integral could hold
    /// at all: 90 x 0.1 = 9 view px/step, i.e. 1080 px/s. Past that the integral
    /// pins against the clamp and P has to make up the difference out of the
    /// remaining error — hundreds of px of it. That, not the gain values, is why
    /// a low-sensitivity game could not keep up once the feed-forward was small.
    /// The leash is now 150 (tracking::kAimTrimLimitPx), and the proximity
    /// schedule no longer throttles ki.
    ///
    /// The other half of the same bug is DISCHARGE RATE. The integral used to
    /// come back at exactly the rate it went out — ki·e·dt — so a 50 px trim
    /// built up against a strafe takes 50/(ki·|e|·dt) frames to give back. At
    /// ki = 0.1, e = −20 px, 120 Hz that is 3000 frames: twenty-five seconds.
    /// "ki 会导致过冲并且很慢的描回来" is that arithmetic, exactly. It now unwinds
    /// kIntReleaseGain (6x) faster whenever the error is already pulling it back
    /// toward zero (see PPID::update).
    ///
    /// ── ROUND 23: WHAT THIS ROW IS NOW, AND WHY THE RANGE IS 0.04 ───────────
    ///
    /// The carrier is the PREDICTION TERM's job (ff = 前馈 · v̂ · kfEff). The
    /// integral supplies the residual (1 − 前馈) of it, and a leaky integrator
    /// can only supply a carrier by standing on an error, because its charge IS
    /// ki·e/(1 − leak). So this slider sets the loop's remaining steady-state
    /// lag; 前馈 sets most of it, and ki buys back what is left.
    ///
    /// ★ THE RANGE IS SMALL BECAUSE THE USEFUL BAND IS NARROW, and the reason is
    /// the leak: tracking::kIntegralLeakPerFrame is 0.95 (a 19-frame time
    /// constant) now, deliberately close to the loop's own ~8-frame dead time,
    /// because a much slower integrator stops trimming the loop and starts
    /// resonating with it — that was the sawtooth the user reported as "一会能跟
    /// 上，一会突然慢一点". With the leak where it belongs the integrator's DC
    /// gain is 1/(1 − leak) = 20 per unit error, and the useful band is
    /// ki ≈ 0.01 … 0.03:
    ///
    ///     0.010  trails ~3.9 px on a 600 px/s strafe, smooth — the shipped value
    ///     0.015  ~0.8 px
    ///     0.020  ~ −0.8 px (a hair ahead), still clean if α is near the assumed 1
    ///     0.030  clean at α ≈ 0.5 only
    ///     0.050+ limit-cycles at every α
    ///
    /// The swing ceiling moves with α (view px per finger px), which is NOT
    /// measurable on the device without reverse-engineering: roughly 0.03 at
    /// α 0.5, 0.02 at α 1.0, 0.01 at α 2.0. A 0–1.00 range would be 30x past the
    /// end of the band, and 0.01 steps would leave three usable positions.
    ///
    /// ★ READ IT AS A LADDER: 0.010 → 0.015 → 0.020 → 0.025, stopping as soon as
    /// the trailing is gone. Too far is unmistakable in the telemetry — the
    /// `mean e=` column starts changing sign every few seconds instead of
    /// settling on one value — and it is not damageable, so one step back is the
    /// whole recovery. Step 0.001 and THREE decimals (two was unusable: the
    /// entire band is 0.01…0.03, which at two decimals is three stops).
    ///
    /// ⚠ NOTE ON HOW THESE REACH A DEVICE. kp/ki/kd/outSmooth/delay are read from
    /// config.json ONLY from a file whose `ctl` equals kCtlSchema (round 22/v14
    /// — see apply() in config_manager.cpp); across a schema change the whole
    /// gain block is re-seeded, because the loop those numbers were tuned
    /// against no longer exists. Within one schema a stored tuning is never
    /// overwritten — deliberate — so "I changed the default" is not observable
    /// on an existing install without deleting config.json. The leak itself is a
    /// COMPILE-TIME constant, so it needs no config migration at all.
    widgets::SliderState kp{0.05f, 0.0f, 1.00f, 0.01f};
    widgets::SliderState ki{0.01f, 0.0f, 0.04f, 0.001f};
    widgets::SliderState kd{0.15f, 0.0f, 1.00f, 0.01f};
    /// 输出平滑 — EMA on the controller's output, 0..1, 1.0 = OFF. Shipped at
    /// 0.85: it trades a small, bounded amount of phase lag for a real cut in the
    /// finger-command noise. See the row's note in drawAimSection().
    widgets::SliderState outSmooth{1.0f, 0.0f, 1.0f, 0.05f};

    /// Lead, in detector frames: how far ahead of its tracked centre the aim
    /// places the target, to pay for the delay between the screenshot the
    /// detector saw and the touch the game will receive. Applied as
    /// (target.x, target.y) += (vx, vy) * this, using the tracker's filtered
    /// velocity.
    ///
    /// ⚠ ROUND 18: THE DEFAULT IS 0.20, and what this row is FOR matters as much
    /// as what it costs — round 17 only learned the second half.
    ///
    /// FOR: the screenshot the detector saw is already a few frames old by the
    /// time a touch lands, so a target crossing at v px/frame is v·(that delay) px
    /// further along than the box says. This term pays that bill by aiming at
    /// (box centre + v·this), using the tracker's filtered velocity. It is the one
    /// knob in the loop that makes it LEAD rather than lag, so it directly buys
    /// reduced tracking lag on a moving target. That is its whole job, and it is
    /// why it is not zero.
    ///
    /// COST (round 17 — the reason this row is now safe). It enters the output
    /// through the proportional gain, i.e. the SAME channel as Kf, so the two add;
    /// at the old default of 1.0 the surplus showed up as overshoot on every
    /// moving target. Worse, it MOVES THE POINT THE LOOP CONSIDERS "ARRIVED": a
    /// lead is a steady-state offset, so while the target keeps moving v is
    /// non-zero and the aim can sit satisfied at box centre + v·this — which is
    /// outside the box. That is precisely the round-17 "准心一直锁在框旁边"
    /// report. The fix was NOT to remove the lead but to stop it from voting: the
    /// deadzone and the error are now measured against the RAW box centre
    /// (rawTarget in syncAimPage), while this term only shifts what the loop aims
    /// AT. "移动一下屏幕就正常了" was the fingerprint of the old behaviour — a
    /// re-association zeroes the track's velocity and the offset collapses.
    ///
    /// Range 0–5, step 0.05. Raise it if the crosshair trails a steadily moving
    /// target; lower it (or zero it) if the crosshair overshoots one that changes
    /// direction — a lead is a bet that the target keeps going.
    ///
    /// ★ DEFAULT 0.20 → 0.00 (round 23). The lead is a THIRD self-referential
    /// path — `setpoint += boxVelocity · τ`, and the box's screen velocity
    /// contains our own output — and the reference controller has no lead at all:
    /// the lookahead that a moving target needs is the prediction term's job now,
    /// and doing it twice is how the round-17 "sits beside the box" fault and the
    /// round-19c "only shakes while actually driving" fault were produced. It
    /// also shifts what the loop calls "arrived", which is the last thing that
    /// should be in play while a "the aim stops driving when it gets there"
    /// report is being chased. Off by default; the slider is still there for
    /// anyone who measures a benefit.
    widgets::SliderState aimDelayFrames{0.0f, 0.0f, 5.0f, 0.05f};

    // 丢框预测帧数 (trackPredictHoldFrames) MOVED TO THE SETTINGS PAGE in round
    // 18 — see settings_section.h, which now owns the slider and the note. It is
    // tracker behaviour and it belongs next to 丢失帧, not next to the gains.

    /// 前馈 — the prediction term's share of the carrier. ROUND 22: same row,
    /// same place, same "how much of the standing command does the feed-forward
    /// supply" reading as the kf row always had, but the quantity under it is
    /// now a plain fraction with a PROVABLE ceiling instead of a strength on a
    /// 1/alpha_hat:
    ///
    ///     ff [finger px] = 前馈 · v̂ [screen px/frame] / tracking::kSensFrozen
    ///
    /// and at a steady lock v̂ = kSensFrozen·u, so the term settles at
    /// `前馈 · u_ss` — exactly the carrier share, whatever the game's real
    /// sensitivity turns out to be. The reconstruction subtracts our own delayed
    /// command to find v̂, so the term is a delayed copy of our own output with
    /// coefficient exactly `前馈`, and **前馈 < 1 is the whole stability
    /// margin**. That is why the row stops at tracking::kLookaheadMax (0.80) and
    /// why there is no guard and no rate limit on it any more: every position of
    /// this slider is safe, so the knob goes straight through to the controller.
    ///
    /// SET IT HIGH. 0.80 is the shipped value and the useful band is 0.60…0.80.
    /// Below about 0.5 the integral has to make up the difference, which is the
    /// old arrangement and brings the old behaviour with it. At 0 the term is
    /// off exactly and the integral carries everything — legal, and it is what
    /// the old default did, but it is no longer the recommended working point.
    ///
    /// ⚠ WHAT THE OLD NOTE BELOW DESCRIBES. Everything from here to the slider
    /// line is the history of the kf row under the PIDF — the 1/alpha_hat
    /// formula, the guard, the self-copy stability window. It is kept because it
    /// is how the current understanding was reached, and it is NOT a description
    /// of what this row does now.
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
    /// kf is only a strength, 0…0.50:
    ///
    ///     ff = (kf/alpha_hat)·LPF( Δe + alpha_hat·u(k−L) )
    ///
    /// Residual self-term = kf·(1 − alpha/alpha_hat)·u(k−L), so the error in the
    /// constant is asymmetric: under-estimate = extra damping (safe), over-
    /// estimate = the z = 1 trap returns (bad). "A small kf is safe" is therefore
    /// true — and useless, which is the round-11 mistake this one fixes.
    ///
    /// THE RANGE WAS THE BUG. Expanding the formula at DC gives the feed-forward
    /// a gain on TARGET VELOCITY of kf/alpha_hat and a share of the DC carrier of
    ///
    ///     kf·alpha/alpha_hat  ≈ 0.8·kf
    ///
    /// — independent of the game. A ceiling of 0.20 could therefore never supply
    /// more than 16 % of the command the loop needs to hold a moving target, and
    /// the user's 0.08 supplied 6 %: that is why "kf 跟不上枪" was literally
    /// true, and why raising ki to compensate overshot instead (the integral is
    /// the wrong carrier — it has to LEARN the velocity, and wind-up is the
    /// price). Measured at his gains on a 600 px/s target, alpha = 0.1:
    /// kf 0.08 -> lag 663 px, 0.20 -> 645, 0.40 -> 126, 0.80 -> 36, 1.00 -> −12.
    ///
    /// HOW TO SET IT — it is a CONSTANT, not a calibration. Full carrier needs
    /// kf = alpha_hat/alpha, the reciprocal of the estimator's own 0.8 bias, so
    /// ~1.0 on every game and every device; the usable band is 0.8…1.5. Watch the
    /// error while the target strafes steadily: one persistent sign = trailing =
    /// raise it; an error that leads (crosshair ahead) = lower it. Sway on a STILL
    /// target is the OTHER knob — that is the feed-forward amplifying the
    /// tracker's noise by kf/alpha_hat (12.5× at alpha 0.1), so lower kf or raise
    /// kFfTauSec. And if the aim shakes at rest even with kf = 0, the cause is not
    /// this row at all: check kp × alpha_hat (in the log) against 0.285.
    ///
    /// 0 turns the feed-forward off exactly and leaves the integral as the sole
    /// DC carrier. Safe everywhere, but it trails a strafe at low sensitivity.
    ///
    /// Range 0.00–0.50, step 0.01, default 0.05. See tracking/pid_controller.h.
    ///
    /// ROUND 13 — DO NOT PUSH THIS TO 1.00 BEFORE READING THE SEED CHANGE. The
    /// crosshair flew off the screen at kf = 1.00 because alpha_hat was still on
    /// its old 1.0 DEFAULT: with a true alpha near 0.1 the residual self-term
    /// kf·(1 − alpha/alpha_hat) is then +0.9·kf, i.e. 0.9 of positive feedback.
    /// The controller now seeds alpha_hat at 0.10 and pushes it from the first
    /// step rather than only after a successful fit, and the feed-forward is
    /// bounded four ways, so 1.00 is reachable — but if the crosshair ever runs
    /// away again, the diagnostic order is: read `a=` in the log (it should be
    /// near 0.1, NOT 1.0), then check `|Δe|` per frame against the jump
    /// detector, then lower kf. See the round-13 section of
    /// tracking/pid_controller.h.
    ///
    /// ROUND 14 — WHY 0.80 BECAME THE DEFAULT, AND WHY THE CEILING CAME DOWN TO
    /// 1.20. ⚠ SUPERSEDED BY ROUND 15 (0.50 / 0.05) — kept because the stability
    /// argument below is exactly what shows the new ceiling is safe.
    /// The residual self-term is a self-COPY of our own delayed command, and a
    /// copy with coefficient s is stable only while |s| < 1: the loop's
    /// characteristic equation is z^L = s, so |z| = |s|^(1/L). With the
    /// estimator's 0.7 bias plus its +/-30 %, r = alpha/alpha_hat lands in
    /// roughly 1.0…2.0, and then
    ///
    ///     kf = 1.00  →  s = −1.00…0.00    a pole ON the unit circle   fragile
    ///     kf = 0.80  →  s = −0.80…0.00    strictly inside it          safe
    ///
    /// while the carrier share kf·r/(1 − kf + kf·r) is 0.79…0.88 either way. So
    /// 0.80 buys the entire stability margin for the ~15 % of the carrier the
    /// integral is already cleaning up — which is the trade the "跟枪稳定不抖"
    /// requirement actually asks for. 1.20 stays reachable for a game whose
    /// alpha the fit cannot reach; past that the self-copy is a limit cycle, not
    /// a trim, and no gain underneath it will hide that.
    /// ROUND 15 — THE USER'S RANGE. 0.00–0.50, default 0.05.
    ///
    /// Round 14 set 0.80 as the working value on the strength of the self-copy
    /// stability window, and that argument still holds — but it argued about
    /// where the FEED-FORWARD may safely stop supplying the carrier, not about
    /// what the user needs. He tuned the loop without it (kf 0, kp 0.05, ki 0.20,
    /// kd 0.26) and the integral carries the DC command fine now that its leash
    /// is 150 and it unwinds at 6×; with that working, a small kf is a trim on
    /// top and 1.20 was only ever a way to get the crosshair off the screen.
    ///
    /// The ceiling is 0.50 because the residual self-copy coefficient
    /// s = kf·(1 − alpha/alpha_hat) cannot reach the unit circle there even if
    /// the estimator is off by 2× — so every position of the slider is usable and
    /// nothing on it runs away. 0 still switches F off exactly.
    ///
    /// The DEADZONE and kf are no longer complementary, and this is the round-16
    /// correction. Round 15 suppressed the integral inside the band, which made
    /// the feed-forward the only term that could follow motion through it — and
    /// at the user's kf = 0 that left NOTHING, so the band zeroed the axis and the
    /// aim flung. The band now leaves the trim in the output and only bleeds it
    /// toward the carrier the reconstruction measures the target to need, so a
    /// strafe is followed through the band at ANY kf including 0. Set the band by
    /// the target box's jitter alone; it no longer has to be traded against kf.
    // ROUND 19: the top of the row is the controller's OWN constant, not a
    // duplicate of it — the two had drifted apart once already (the slider
    // stopped at 0.50 while the header's own log line described a working point
    // of 1.0), and a range that disagrees with the derivation is worse than a
    // narrow one. 0.80 is the last value the self-copy window calls safe.
    widgets::SliderState ffGain{tracking::kLookaheadDefault, 0.0f, tracking::kLookaheadMax, 0.01f};


    /// Aim deadzone (0.0–1.0, step 0.05, two decimals): ports the old project's
    /// `convergeThresh`. Once the TARGET is within `deadzone` of the screen
    /// centre (crosshair) P and D stop driving that axis — they are what amplify
    /// the box's jitter — and the trim stops accumulating, being bled toward the
    /// carrier the reconstruction measures (0 at rest, so a standing charge no
    /// longer walks the view off a still target); the trim is still ADDED to the
    /// output, which is what round 15 got wrong. 0.0 = no
    /// deadzone (must converge to the pixel); 1.0 = stop as soon as the
    /// crosshair is inside the box at all.
    ///
    /// ⚠ ROUND 17: THE RADIUS IS PER-AXIS, AND IT IS MEASURED FROM THE RAW BOX
    /// CENTRE. Both halves of that sentence were bugs until this round and the
    /// user reported both as one symptom — "准心一直锁在框旁边，框在左边".
    ///
    ///   (a) The radius was `value × 0.5·min(w,h)` for BOTH axes, so on any box
    ///       that is not square the X band and the Y band were different
    ///       fractions of it (a tall box got an X band a third of its own half
    ///       width). It is now X ⇒ `value × w/2`, Y ⇒ `value × h/2`, i.e. the
    ///       "框内百分比" the slider says it is.
    ///   (b) The band was measured from the LEAD-SHIFTED aim point (the target
    ///       pushed along the tracker's velocity by 延迟补偿), NOT from the box
    ///       the HUD draws. So with any lead at all the loop's own equilibrium —
    ///       "the aim point is on the crosshair" — sits `v × lead` away from the
    ///       box centre, and because that IS an equilibrium (e ≈ 0 there) the aim
    ///       parked there and STAYED, one whole box off the target, for as long
    ///       as the target kept moving. The band is now measured against the RAW
    ///       centre — the same point the box is drawn at — while the controller
    ///       still receives the lead-shifted error, so lead keeps helping the
    ///       APPROACH and can no longer decide where the aim comes to rest.
    ///       (This is also why "移动一下屏幕就正常了": a re-association zeroes
    ///       the track's velocity, the v × lead offset collapses, and the aim
    ///       falls back onto the box.)
    ///
    /// And the band's trigger now has a second, independent mechanism behind it:
    /// see kStopHoldSteps in tracking/pid_controller.h for why a bleed gated on
    /// |e| <= band could never fire on the shake it was written for.
    ///
    /// ⚠ THIS ROW WAS CONNECTED IN ROUND 15, and until then it did not do what
    /// it says on two counts: (a) it only suppressed P and D, so the integral —
    /// the one term that actually walks a resting aim off a target — kept
    /// driving the finger inside the band; and (b) it therefore never stopped the
    /// static shake, which is what the user was trying to kill with it. It also
    /// only affected the GLOBAL band; see yFollow for the Y-only one.
    ///
    /// SET IT SMALL, and only for noise. The band is the loop's "on target"
    /// declaration, and inside it the loop is deliberately open — the integral
    /// is suppressed, so nothing is left to hold a MOVING target but the
    /// feed-forward. A large deadzone with kf = 0 therefore moves in bursts by
    /// construction. 0.1…0.3 kills detector jitter; 1.0 stops at the target's
    /// edge and is a setting for a stationary target only.
    widgets::SliderState deadzone{0.0f, 0.0f, 1.0f, 0.05f};

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
    ///
    /// Either way the screen carries exactly ONE touch point while the aim holds
    /// it, and that is enforced in the READER rather than here — see
    /// reader_set_point_guard(). A finger that lands inside the touch area while
    /// the aim owns its contact is not mirrored at all, so pressing during an
    /// engagement cannot turn into a second look contact (the "两个触摸点" bug).
    /// First come, first served: a finger already down when the aim engages
    /// keeps the point and the takeover drives it; a finger pressed afterwards
    /// is refused — and stays refused until IT lifts, not until the aim lets go.
    /// Releasing it earlier would drop it on the app at its current position
    /// while the app's one look contact is wherever the aim left it, i.e. a
    /// camera jump. Fingers outside the touch area — the fire button, the
    /// trigger finger — are mirrored exactly as before, which the trigger in
    /// particular requires: swallowing it would release the aim's own hold.
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

    /// Per-class aim-height offset (Y偏移·<类别>, 0.0–1.0, default 0.5): WHERE
    /// along the Y deadzone band the crosshair comes to rest.
    ///
    /// 0.5 = the box middle, i.e. exactly what the aim did before this existed —
    /// which is why it ships at 0.5. Larger aims HIGHER: the band is divided
    /// top-down, so 1.0 is its top edge and 0.0 its bottom.
    ///
    /// ★ The BAND is the unit, not the box. The offset is
    ///     (0.5 - v) * 2 * bandHalfPx,   bandHalfPx = max(全局死区, Y死区) * 框半高
    /// — the Y stop radius actually in force. Two consequences, both deliberate:
    ///   * the rest position is guaranteed to satisfy the very deadzone that
    ///     decides whether Y is frozen, because the band IS the offset's range.
    ///     A larger range would put the aim point OUTSIDE the band, the deadzone
    ///     would then read "not on target" there and pull Y back to the middle,
    ///     and the slider would end up fighting the loop instead of choosing
    ///     where it rests;
    ///   * with both deadzones at 0 (Y死区 defaults to 0.0) the band is 0 wide
    ///     and the slider does nothing — it is a position WITHIN the stop band,
    ///     so there has to be a band. Set Y死区 first.
    ///
    /// Applied to the aim point every frame, and that is still "Y is not
    /// adjusted every frame": inside the band Y freezes exactly as before, so
    /// the offset shows up where it is wanted — on the first pull-in, and again
    /// whenever the target moves vertically out of the band. Keyed by class
    /// index, values kept for unselected classes, same as yFollow.
    std::map<int, widgets::SliderState> yOffset;

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
