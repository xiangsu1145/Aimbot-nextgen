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
    /// `kf` is the feed-forward STRENGTH, 0…0.50 (round 15) — it is NOT the plant
    /// gain. The reconstruction constant is derived on-line (see `alphaEst`) and
    /// pushed into both axes. Round 11 capped it at 0.20 and that cap — not the
    /// user's tuning — is why the aim could not keep up then; round 15 fixed the
    /// other end, because the user's own tuning (kf 0, kp 0.05, ki 0.20, kd 0.26)
    /// tracks perfectly with the integral as the carrier, which makes the
    /// feed-forward a trim rather than a carrier. See the round-15 section of
    /// tracking/pid_controller.h.
    void setGains(float kp, float ki, float kd, float outSmooth, float kf) {
        // The SLIDER's value, kept exactly as asked. This is what the config
        // stores and what the user sees on the row — the knob is his.
        this->ffGain = std::max(0.0f, kf);

        // ROUND 19: what actually reaches the axes is GUARDED. kf is a gain on
        // the echo of our own delayed command (s = kf·(1 − alpha/alpha_hat)), so
        // a large kf is only safe while alpha_hat is RIGHT — and the seed is a
        // guess. See kFfGuardStrengthMax for the measurements behind that.
        //
        // The guard is a CAP and not a working point. It applies only while no
        // fit has corroborated alpha_hat and lifts to whatever the slider says
        // once two independent fits agree, so 激进/保守 stays the user's own
        // knob; the guard only makes the first second safe instead of audible.
        const float want = alphaTrusted()
                               ? this->ffGain
                               : std::min(this->ffGain, tracking::kFfGuardStrengthMax);

        // Moved as a RATE, never as a step. A jump in kf is a jump in the DC
        // carrier's split between the two channels, i.e. a velocity step — the
        // same class of fault as round 15's "乱甩乱晃" — so it obeys the same
        // rule as every other gate in the controller. This runs once per render
        // frame, so the rate below is per-frame by construction.
        kfEff_ += (want - kfEff_) * tracking::kFfGuardRate;
        if (!std::isfinite(kfEff_)) kfEff_ = want;

        pidX.setGains(kp, ki, kd, outSmooth, kfEff_);
        pidY.setGains(kp, ki, kd, outSmooth, kfEff_);
    }

    /// True once the plant gain has been CORROBORATED — both fits have an answer
    /// and the two agree. This is the gate the kf cap opens on, and it is the
    /// whole reason there are two estimators.
    ///
    /// Neither fit is trusted alone. One works on the cumulative command and the
    /// other on its differences, over different windows, sharing no arithmetic;
    /// landing on the same alpha is therefore evidence rather than a restatement.
    /// A single fit can satisfy its own normal equations while describing
    /// nothing, and the price of believing such a fit is a full-strength kf on a
    /// wrong alpha_hat, which is a ring on the device.
    ///
    /// The band is wide on purpose (0.6…1.7). Both values carry kAlphaBias, so a
    /// correct pair sits near 1.0, and the width is there to absorb the ±30 %
    /// each fit is known to carry — not to make the test easy.
    ///
    /// It latches: once corroborated, the cap stays open for the session. alpha
    /// belongs to the GAME, not to the engagement, so lifting and re-pressing the
    /// finger must not re-arm the guard. Nothing on the target-changed path
    /// touches it for the same reason.
    bool alphaTrusted() {
        if (alphaTrusted_) return true;
        if (!alphaEst.valid() || !slopeEst.valid()) return false;
        const float a = alphaEst.value(), s = slopeEst.value();
        if (!(a > 0.0f) || !(s > 0.0f)) return false;
        const float r = s / a;
        if (r > 0.6f && r < 1.7f) alphaTrusted_ = true;
        return alphaTrusted_;
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
    /// the caller must NOT zero the returned value afterwards. The band gates the
    /// integrator's RATE, never its CONTRIBUTION: inside it P and D stop driving
    /// and the trim stops accumulating, being bled toward the carrier the
    /// reconstruction measures the target to need (carry = ffVel/alpha_hat, i.e.
    /// zero at rest and the standing carrier on a strafe) — while still being
    /// ADDED to the output. Round 15 dropped it from the output as well, and with
    /// kf = 0 that zeroed the axis outright and read as 乱甩乱晃; see the round-16
    /// section of tracking/pid_controller.h.
    ///
    /// `targetChanged` says the caller picked a DIFFERENT track this step. The
    /// caller knows (it has the track id); the controller cannot tell a switch
    /// from fast motion by looking at the error alone, and guessing wrong cost
    /// the integral that damps a large swing. Both axes are resynced together —
    /// a switch changes both.
    /// `measurementCorrected` is the caller's knowledge that the TRACKER
    /// re-associated this step — it pulled the published centre back onto a
    /// detector result one render frame old instead of letting the model advance
    /// it by its own velocity. The controller must be told, and cannot infer it:
    /// the artifact it has to reject is proportional to our own output, so from
    /// the error alone it is indistinguishable from a genuinely fast target. See
    /// the parameter of the same name on AimController::update().
    void step(float errX, float errY, float dt, bool freezeX, bool freezeY,
              bool targetChanged, bool measurementCorrected,
              float& outX, float& outY) {
        // The delay-aligned command must be read BEFORE update() shifts the ring:
        // it is the u(k−L) that produced the error we are about to act on, and it
        // is the regressor's increment in the plant-gain fit.
        const float uDelayed = pidX.delayedCommand();

        outX = pidX.update(errX, dt, freezeX, targetChanged, measurementCorrected);
        outY = pidY.update(errY, dt, freezeY, targetChanged, measurementCorrected);

        // The plant gain is a property of the game, not of an axis, so one fit on
        // X serves both. Pushed UNCONDITIONALLY, not only while valid(): the
        // estimator's value() starts at kAlphaHatSeed (0.10, the SAFE side) and
        // is never unset, and pushing it only on `valid()` is what left the
        // controller on its 1.0 default until the first successful fit — an
        // OVER-estimate of a touch game's ~0.1 plant gain, i.e. +0.9·kf of
        // positive feedback and the crosshair leaving the screen. `valid()` is
        // now for the log's `?` marker only.
        alphaEst.push(errX, uDelayed);
        // ROUND 19: the SECOND, independent fit — differences against the delayed
        // command instead of cumulative positions against it. Same inputs, an
        // entirely different piece of arithmetic, which is what makes agreement
        // between the two mean something (see alphaTrusted()).
        slopeEst.push(errX, uDelayed);

        // Where both have an answer they are AVERAGED rather than chosen between:
        // each carries ±30 % of its own and the errors are of different kinds (one
        // detrends quadratically over a longer window, the other linearly over a
        // short one), so the mean is better than either. Where only one has an
        // answer, that one is used — a surviving fit is still a measurement, and
        // the seed is a guess.
        const bool  pv = alphaEst.valid(), sv = slopeEst.valid();
        const float a  = (pv && sv)
                             ? 0.5f * (alphaEst.value() + slopeEst.value())
                             : (pv ? alphaEst.value()
                                   : (sv ? slopeEst.value() : alphaEst.value()));
        pidX.setAlphaHat(a);
        pidY.setAlphaHat(a);
    }

    /// The estimated plant gain in force (alpha_hat), for the log. Compare it
    /// against the box size and the reachable view speed to sanity-check the
    /// gains: the P-loop's useful ceiling is kp·alpha < 0.285 at this delay.
    float alphaHatValue() const { return pidX.alphaHatValue(); }
    /// True once the plant-gain fit has ever converged. For the log.
    bool  alphaValid() const { return alphaEst.valid(); }

    /// Trim of each axis, for the diagnostic log. Read it AGAINST carryX/Y, not
    /// against a fixed idea of "small": the trim is the DC carrier whenever kf is
    /// small (the user's own tuning runs kf = 0), so |trim| ≈ |carry| is the
    /// healthy state on a moving target, and trim pinned at its leash means the
    /// requirement is beyond what the loop may deliver. At rest both should be 0.
    float trimX() const { return pidX.integralValue(); }
    float trimY() const { return pidY.integralValue(); }

    /// The feed-forward's contribution to each axis, in px. THE number to watch
    /// on a moving target: it should settle at kf·ΔT/alpha and hold there. Near
    /// zero while the error trails the target means the feed-forward is not
    /// running (kf = 0, or every frame reports targetChanged) — and at kf = 0 it
    /// is zero BY DESIGN, which is why `carry` below exists.
    float ffX() const { return pidX.ffValue(); }
    float ffY() const { return pidY.ffValue(); }

    /// The carrier each axis is measured to NEED, in finger px/step — the trim's
    /// own units, and the value the deadzone bleeds the trim toward. Unlike `ff`
    /// this is NOT scaled by kf, so it is the number that stays meaningful at
    /// kf = 0. On a strafe read it against `trim`: the two should converge. At
    /// rest it should read 0, and trim should follow it there.
    float carryX() const { return pidX.carryPx(); }
    float carryY() const { return pidY.carryPx(); }

    /// True while the STOP DETECTOR (round 17) has that axis at rest — the
    /// measured carrier has read ~0 for kStopHoldSteps in a row — and is
    /// therefore bleeding the trim toward it. This is the mechanism that removes
    /// the shake a large ki leaves behind when a moving target STOPS, and it is
    /// the one case the deadzone's own trigger cannot reach, because the
    /// oscillation itself holds |e| outside the band. Read it with trimX/Y and
    /// carryX/Y; see PPID::stoppedNow() for what each combination means.
    bool stoppedX() const { return pidX.stoppedNow(); }
    bool stoppedY() const { return pidY.stoppedNow(); }

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

    /// The feed-forward strength ACTUALLY in force, for the log — the slider's
    /// value once the guard is open, the guarded value before that. Print THIS
    /// one: `ffGainSlider()` below is the knob, and the two differ exactly when
    /// the round-19 guard is doing something.
    float ffGainValue() const { return kfEff_; }
    /// The kf slider's own value, unguarded. The knob, not the working point.
    float ffGainSlider() const { return ffGain; }
    /// True once both fits have agreed on the plant gain. For the log: while it
    /// is false the kf row is CAPPED, which is the intended state for the first
    /// fraction of a second of a session and not a fault.
    bool alphaCorroborated() const { return alphaTrusted_; }
    /// The output ceiling in force (finger px/step). A constant.
    float outLimitPx() const { return pidX.outLimitPx(); }

private:
    /// Mirrors the kf row. 0.05 is the round-15 default: the integral carries
    /// the DC command and the feed-forward trims it. ROUND 19 keeps that default
    /// and widens the row to kFfStrengthMax (0.80) — the working point is still
    /// the user's to pick, and 0.05 remains the shipped one.
    float ffGain = 0.05f;
    /// The strength in force after the guard, rate-limited toward `ffGain`. It
    /// starts at ZERO rather than at the slider's value: on the first frame
    /// nobody has corroborated alpha_hat, and starting with the guard already
    /// satisfied is the one thing that would make the guard useless. Rising from
    /// zero costs nothing — the integral carries the DC command regardless of kf
    /// — and, being a rate, it cannot step.
    float kfEff_ = 0.0f;
    /// One plant-gain fit, shared by both axes (alpha is the game's sensitivity,
    /// not an axis property).
    tracking::AlphaEstimator alphaEst;
    /// ROUND 19: a SECOND, independent fit on the same plant gain — a straight
    /// line through (Δe, u(k−L)) instead of a quadratic through (e, U(k)). Its
    /// whole purpose is to be something the first fit can be checked against;
    /// see alphaTrusted().
    tracking::SlopeAlphaEstimator slopeEst;
    /// Latched by alphaTrusted(): true once both fits have agreed at least once.
    bool alphaTrusted_ = false;
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
    /// the panel — so it is the constant tracking::kOutLimitPx (180 px/step,
    /// tanh) and the page draws no row for it. The integral's leash is the
    /// constant tracking::kTrimLimitPx (150). Neither is scaled by anything:
    /// kf is the only place the plant gain enters, and it lives in its own row.
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
    /// The leash is now 150, and the proximity schedule no longer throttles ki.
    ///
    /// The other half of the same bug is DISCHARGE RATE. The integral used to
    /// come back at exactly the rate it went out — ki·e·dt — so a 50 px trim
    /// built up against a strafe takes 50/(ki·|e|·dt) frames to give back. At
    /// ki = 0.1, e = −20 px, 120 Hz that is 3000 frames: twenty-five seconds.
    /// "ki 会导致过冲并且很慢的描回来" is that arithmetic, exactly. It now unwinds
    /// kIntReleaseGain (6x) faster whenever the error is already pulling it back
    /// toward zero (see PPID::update).
    ///
    /// Range 0–4, step 0.01, shown to TWO decimals. Step alone was never the
    /// problem — with a one-decimal readout a 0.01 move is invisible, which is
    /// what made it look like the step was still 0.1.
    ///
    /// How much ki you need depends on how much carrier kf is not supplying.
    /// Round 15's reading was "with the shipped kf (0.05) the INTEGRAL is the
    /// carrier, and the user's own working pair is ki 0.20 with kf = 0". Round 18
    /// ships ki 0.12 with kf 0.05 and kp 0.04 — the same rule read at a different
    /// point: kf takes the fast part of the carrier, so the integral comes down.
    /// With no feed-forward at all (kf = 0) the integral carries the whole DC
    /// command and 0.2–1.0 is the useful band.
    /// ROUND 18 — THE SHIPPED SET IS THE USER'S CONVERGED ONE, and it is a
    /// DIFFERENT BALANCE from round 15's, not a nudge: kp 0.04 / ki 0.12 /
    /// kd 0.15 / kf 0.05, where round 15 shipped 0.10 / 0.5 / 0.20 with kf 0.05.
    /// kf is no longer ~0, and that is what moved the other three: the
    /// feed-forward takes the high-frequency load off the integral, so ki can
    /// come down to a fifth, and a smaller kp with a smaller kd then holds the
    /// phase margin. The two sets are not comparable number-for-number — read
    /// the Kp/Ki note above for why "how much ki you need" is a function of "how
    /// much carrier kf is not supplying".
    ///
    /// ⚠ NOTE ON HOW THESE REACH A DEVICE. kp/ki/kd/outSmooth/delay are read from
    /// config.json UNCONDITIONALLY (see apply() in config_manager.cpp), so an
    /// existing install keeps the tuning it already stored — a new default is
    /// only observable on a fresh config file. That is deliberate (a stored
    /// tuning is never silently overwritten), and it also means "I changed the
    /// default" is not something you can verify without deleting config.json.
    widgets::SliderState kp{0.04f, 0.0f, 0.6f, 0.01f};
    widgets::SliderState ki{0.12f, 0.0f, 4.0f, 0.01f};
    widgets::SliderState kd{0.15f, 0.0f, 2.0f, 0.01f};
    /// 输出平滑 — EMA on the controller's output, 0..1, 1.0 = OFF. Shipped at
    /// 0.85: it trades a small, bounded amount of phase lag for a real cut in the
    /// finger-command noise. See the row's note in drawAimSection().
    widgets::SliderState outSmooth{0.85f, 0.0f, 1.0f, 0.05f};

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
    widgets::SliderState aimDelayFrames{0.20f, 0.0f, 5.0f, 0.05f};

    // 丢框预测帧数 (trackPredictHoldFrames) MOVED TO THE SETTINGS PAGE in round
    // 18 — see settings_section.h, which now owns the slider and the note. It is
    // tracker behaviour and it belongs next to 丢失帧, not next to the gains.

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
    widgets::SliderState ffGain{0.05f, 0.0f, tracking::kFfStrengthMax, 0.01f};


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
