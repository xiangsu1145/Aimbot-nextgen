// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::aim — implementation. See aim_section.h for layout.
// ─────────────────────────────────────────────────────────────────────────────
#include "aim_section.h"

#include "ui/gui/sections/backflash_section.h"
#include "ui/gui/hud.h"
#include "ui/gui/theme.h"

#ifdef HAS_IMGUI

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <android/log.h>

#include "imgui.h"

#include "inference/model_runtime.h"
#include "inference/model_store.h"
#include "input/inject_backend.h"
#include "input/uinput_inject.h"
#include "input/touch_reader.h"
#include "tracking/kalman_tracker.h"

namespace aimbotng {
namespace ui {
namespace sections {

// Same tag as the rest of the HUD (AimbotNg). Deliberately sparse: only state
// TRANSITIONS are logged, never per-frame numbers. Fusion used to fail in total
// silence — a finger in the box, a delta of zero, and not one line to say so —
// and that is the single most expensive bug this page has had.
#ifndef LOGI
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AimbotNg", __VA_ARGS__)
#endif

PageAim g_pageAim;

namespace {

using namespace theme;

// ── Touch-area geometry constants (in surface pixels) ──────────────────────
//
// Edge-grip thickness is the inward "resize" strip along each edge — a touch
// landing here resizes one axis. Corner-grip is the half-extent of the
// square centred on each corner that takes priority over the edge strip and
// resizes both axes. The body (everything else inside the rectangle) moves.
constexpr float kTouchEdgeGrip    = 24.0f;
constexpr float kTouchCornerGrip  = 36.0f;
constexpr float kTouchCornerDraw  = 18.0f;   // drawn corner-handle size
constexpr float kTouchMinSize     = 200.0f;  // resize cannot go below this
constexpr float kTouchDashLen     = 16.0f;   // dashed-border dash length
constexpr float kTouchDashGap     = 8.0f;
constexpr float kTouchThickness   = 2.5f;

// ── Trigger-circle geometry constants (in surface pixels) ───────────────────
//
// Handles are small filled discs on the four cardinal points; the interior is
// the Move region (the whole disc moves as one). Edge of the circle itself
// catches no drag — see hitTestTrigger() for why.
constexpr float kTrigHandleHitR   = 36.0f;   // hit-test radius around each cardinal handle
constexpr float kTrigHandleDraw   = 9.0f;    // drawn size
constexpr float kTrigThickness    = 2.5f;
constexpr float kTrigMinR         = 60.0f;
constexpr int   kTrigSegments     = 64;
constexpr float kOverlayLabelSize = 38.0f;   // centre-label font size, surface px

// ── Aim-loop constants ─────────────────────────────────────────────────────
//
// kAimPressJitterPx: ± this many pixels of random offset on the press
// position, per the user's spec ("偏移上下左右随机10px").
constexpr float kAimPressJitterPx = 10.0f;

// kLeadVelTauSec: the low-pass the LEAD's copy of the tracker velocity goes
// through, in seconds. The tracker's velocity has a 1.4-render-frame time
// constant, i.e. it is a differentiator of a noisy measurement whose signal also
// contains the aim's own output; lead multiplies it by a frame count and folds
// the product back into the point the loop converges to, which is a phase lead
// in a delay loop that no gain row can bound (see the ROUND 19c note at the lead
// itself). 65 ms is chosen to sit between the two bands: above the detector's
// frame-to-frame ring, and far below the hundreds of milliseconds over which a
// real target changes its own velocity. Lower it if a genuinely fast direction
// change needs to be anticipated; raise it if the crosshair still sways with the
// lead up.
constexpr float kLeadVelTauSec = 0.065f;

// Palette — same border/handle/fill triplet for both overlays so they read as
// the same family. Accent-tinted fill + near-white border + white handles.
constexpr ImU32 kOverlayBorder = IM_COL32(238, 240, 246, 215);
constexpr ImU32 kOverlayHandle = IM_COL32(255, 255, 255, 235);
constexpr ImU32 kOverlayFill   = IM_COL32( 61, 123, 255,  28);
constexpr ImU32 kOverlayLabel  = IM_COL32(238, 240, 246, 235);

// ── Small helpers ───────────────────────────────────────────────────────────
inline float dist2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

inline widgets::Rect wRectOf(float wx, float wy, float ww, float wh,
                             float s, const Xf& xf) {
    const ImVec2 p = xf.pt(wx, wy);
    return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
}

/**
 * Paints a dashed rectangle outline. ImGui's AddRect cannot draw dashes, so
 * each edge is drawn as a sequence of short AddLine calls with gaps between
 * them. Cap-by-length, not by-segment-count, so a partially-clipped edge
 * still ends on a clean dash rather than a half-segment.
 */
void drawDashedRect(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
                    ImU32 col, float thickness,
                    float dashLen, float gapLen) {
    auto dashEdge = [&](ImVec2 a, ImVec2 b) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;
        const float ux = dx / len, uy = dy / len;
        float pos = 0.0f;
        while (pos < len) {
            const float endPos = (pos + dashLen > len) ? len : (pos + dashLen);
            dl->AddLine(ImVec2(a.x + ux * pos, a.y + uy * pos),
                        ImVec2(a.x + ux * endPos, a.y + uy * endPos),
                        col, thickness);
            pos = endPos + gapLen;
        }
    };
    dashEdge(ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y));   // top
    dashEdge(ImVec2(mx.x, mn.y), ImVec2(mx.x, mx.y));   // right
    dashEdge(ImVec2(mx.x, mx.y), ImVec2(mn.x, mx.y));   // bottom
    dashEdge(ImVec2(mn.x, mx.y), ImVec2(mn.x, mn.y));   // left
}

/** Four small filled squares at the rectangle's corners — the resize handles. */
void drawCornerHandles(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
                       float handleSize, ImU32 col) {
    const float h = handleSize * 0.5f;
    const float r = 2.0f;
    auto drawCorner = [&](float cx, float cy) {
        dl->AddRectFilled(ImVec2(cx - h, cy - h), ImVec2(cx + h, cy + h), col, r);
    };
    drawCorner(mn.x, mn.y);
    drawCorner(mx.x, mn.y);
    drawCorner(mn.x, mx.y);
    drawCorner(mx.x, mx.y);
}

/** Draws `text` horizontally and vertically centred on `center`, in surface px. */
void drawCentredLabel(ImDrawList* dl, const ImVec2& center, float size,
                      ImU32 col, const char* text) {
    ImFont* font = ImGui::GetFont();
    size = tsize(size);  // readability boost — measure and draw on the same size
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size,
                ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                col, text);
}

// ── LCG for the press-position random offset ────────────────────────────────
//
// Per-press seed from a simple Linear Congruential Generator. Cheap, no
// allocations, deterministic across runs of the daemon so the same session
// looks the same to anyone debugging it. Adequate for the ±10px jitter; this
// is not crypto.
inline int nextJitter() {
    static uint32_t s = 0xC0FFEEu;
    s = s * 1103515245u + 12345u;
    const int v = static_cast<int>((s >> 16) % 21u);   // 0..20
    return v - 10;                                      // -10..+10
}

// ── Area placement + real-finger trigger test ───────────────────────────────
//
// The touch-area and trigger-area overlays are *settings* — their toggles only
// decide whether the dashed box / circle is drawn, never whether aiming is
// allowed. So the geometry must exist (and be centred by default) regardless of
// the toggle state: the aim loop reads the touch-area centre + size, and the
// hold-to-aim test reads the trigger-area circle, even when both overlays are
// hidden.
void ensureAimAreasPlaced(float screenW, float screenH) {
    PageAim& p = g_pageAim;
    if (!p.touchArea.placed) {
        p.touchArea.x = (screenW - p.touchArea.w) * 0.5f;
        p.touchArea.y = (screenH - p.touchArea.h) * 0.5f;
        p.touchArea.placed = true;
    }
    if (!p.triggerArea.placed) {
        p.triggerArea.cx = screenW * 0.5f;
        p.triggerArea.cy = screenH * 0.5f;
        p.triggerArea.placed = true;
    }
}

// True when a real (physical) finger is currently held inside the trigger-area
// circle. Drives the "hold to aim" mode: with continuous-trigger OFF, aiming is
// permitted only while the player keeps a finger in this zone — exactly the old
// project's fire-zone semantics, which is what makes the trigger area meaningful
// rather than a dead setting. Uses the reader's live pointer set (screen coords).
bool realFingerInTriggerArea() {
    PageAim& p = g_pageAim;
    if (!reader_is_ready() || !p.triggerArea.placed) return false;
    const TriggerAreaOverlay& a = p.triggerArea;
    ReaderPointer pts[READER_MAX_POINTERS];
    const int n = reader_read_pointers(pts, READER_MAX_POINTERS);
    const float r2 = a.r * a.r;
    for (int i = 0; i < n; ++i) {
        const float dx = pts[i].x - a.cx;
        const float dy = pts[i].y - a.cy;
        if (dx * dx + dy * dy <= r2) return true;
    }
    return false;
}

// True when a real (physical) finger is inside the touch-area rectangle. Picks the
// one nearest the touch-area centre so the choice is stable when several qualify.
// Feeds "触摸融合" (touch fusion): when on, aim drives that real finger's mirror
// slot instead of pressing a separate synthetic finger.
bool realFingerInTouchArea(int& outId, float& outX, float& outY) {
    PageAim& p = g_pageAim;
    if (!reader_is_ready() || !p.touchArea.placed) return false;
    const TouchAreaOverlay& a = p.touchArea;
    ReaderPointer pts[READER_MAX_POINTERS];
    const int n = reader_read_pointers(pts, READER_MAX_POINTERS);
    const float cx = a.x + a.w * 0.5f;
    const float cy = a.y + a.h * 0.5f;
    float bestD2 = 1e18f;
    bool found = false;
    for (int i = 0; i < n; ++i) {
        if (pts[i].x < a.x || pts[i].x > a.x + a.w ||
            pts[i].y < a.y || pts[i].y > a.y + a.h) continue;
        const float dx = pts[i].x - cx, dy = pts[i].y - cy;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) {
            bestD2 = d2;
            outId = pts[i].id; outX = pts[i].x; outY = pts[i].y;
            found = true;
        }
    }
    return found;
}

// Advances the finger on `slot` toward `target` (screen-centre reference) one
// frame-rate-independent step and uploads the new position. Shared by both the
// synthetic and the real-finger (fusion) paths so the aim math is identical.
//
// `dzXPx` / `dzYPx` are the global PER-AXIS stop radii — X freezes when
// |raw.x - centre.x| <= dzXPx, Y when |raw.y - centre.y| <= dzYPx (decoupled per
// axis, not one radial circle), each one a fraction of the target's OWN half
// width / half height, with 1.0 landing exactly on the box edge. `yDzPx` is the
// per-class Y-only band, a wider or narrower one: once the crosshair is
// vertically within `yDzPx` of the box centre, Y freezes and X keeps tracking.
//
// ROUND 17: all three are measured against `rawTarget` — the box centre the HUD
// draws — and NOT against `target`, which is the same point pushed along the
// tracker's velocity by the 延迟补偿 lead. The distinction is the whole fix for
// "准心一直锁在框旁边": with any lead the aim's OWN equilibrium sits v×lead off
// the box, so a band centred on the lead point happily declares "on target" while
// the crosshair is one box away from the enemy — and it stays there, because an
// equilibrium is not a transient. The controller still gets the lead-shifted
// error (`target`), so the lead still helps the approach; it just cannot decide
// where the aim comes to rest any more.
//
// `physDeltaX/Y`: the player's physical finger movement this frame, in screen
// px. Only the component that points the SAME WAY as the aim is added — the
// finger can push the aim along, it can never pull it back. See the projection
// below for why that is a projection and not a sign test.
//
// The feed-forward is gone — there is nothing for this function to hand over
// beyond the error and the flags. What used to be here (a feed-forward summed
// outside the controller, then a self-calibrating estimator inside it) is
// documented in the header of tracking/pid_controller.h: it was a bare
// integrator in a delay loop and it oscillated; its gate could not work either,
// because the tracker reports the box's SCREEN velocity, which the aim itself
// nulls. The controller now returns the complete displacement for the step.
static void driveAimToTarget(TouchAimState& st, int slot,
                             const ImVec2& target, const ImVec2& rawTarget,
                             const ImVec2& screenCenter,
                             const ImGuiIO& io, float dzXPx, float dzYPx,
                             float yDzPx,
                             float physDeltaX, float physDeltaY,
                             bool targetChanged, bool measurementCorrected) {
    // The control step, in seconds. This used to be discarded ((void)io) on the
    // grounds that the controller was "frame-rate independent" — it was not, it
    // was frame-rate DEPENDENT: the loop runs at 120 Hz while the panel is up or
    // a finger is down and at 60 Hz in the detector tier, so every dt-scaled
    // term silently retuned itself by 2x whenever that switch happened. The
    // integral and the derivative both need it; the proportional term must not
    // have it (its stability bound is a per-sample one).
    const float dt = std::clamp(io.DeltaTime, 1.0f / 2000.0f, 0.2f);

    // The target's own size, as the gate's unit of measure. `lastTargetBox` is
    // 0.5*max(w,h) (the same yardstick the lock-quality score uses), so the box's
    // longer side is twice it. Passing the LIVE box rather than a pixel constant
    // is the point: "far off" has to mean the same thing on a 20 px box at range
    // and a 300 px one at contact range, and only a width-relative threshold does
    // that. 0 disables the gate (the controller then runs open-loop on both the
    // feed-forward and the integrator), which is the right behaviour if the
    // tracker has no size yet.
    st.aim.setTargetBoxPx(2.0f * st.lastTargetBox);
    // Deadzone — decoupled per-axis stop radius.  A single radial circle was the
    // old code's bug: a target oscillating along X keeps popping OUTSIDE the
    // circle even when Y is at rest, dragging X onto the target centre.  Per-axis
    // freezing holds whichever axis is already inside its band.  The reference is
    // the SCREEN CENTRE (the crosshair), not the live finger.
    //
    // ROUND 17: the error that DRIVES the controller and the error that DECIDES
    // the freeze are now deliberately two different numbers. `dex/dey` are the
    // lead-shifted error (what the loop is asked to null, and so what `e` on the
    // log line means); `rdx/rdy` are measured from the RAW box centre — the box
    // the HUD draws. Only the second one can answer "is the crosshair on the
    // target", because with a lead the first is null exactly when the crosshair
    // is v×lead OUTSIDE the box. See the round-17 note on the signature above.
    const float dex = target.x - screenCenter.x;
    const float dey = target.y - screenCenter.y;
    const float rdx = rawTarget.x - screenCenter.x;
    const float rdy = rawTarget.y - screenCenter.y;
    const bool xInDz  = dzXPx > 0.0f && rdx * rdx <= dzXPx * dzXPx;
    const bool yInDz  = dzYPx > 0.0f && rdy * rdy <= dzYPx * dzYPx;
    const bool yInYFollow = yDzPx > 0.0f && rdy * rdy <= yDzPx * yDzPx;

    float pidX = 0.0f, pidY = 0.0f;
    // Controller takes the raw error (target - crosshair) and returns the finger
    // displacement for this step, in px. The two deadzone flags freeze that axis:
    // P and D are zeroed, and the trim stops ACCUMULATING (its leak and its
    // same-sign zeroing keep running). Everything else about the axis is left
    // alone, and in particular the PREDICTION TERM IS NOT GATED — the band exists
    // to stop the loop chasing the detector's box jitter, and the prediction term
    // is not chasing anything, it is the command a moving target requires.
    // ★ R23 removed the last piece of the old freeze: the trim no longer needs to
    // be bled toward a measured carrier, because the carrier now lives in the
    // prediction term, so "pause the accumulation" is the whole job.
    // Round 15 dropped the trim's CONTRIBUTION instead of its rate, which turned
    // the band into a bang-bang in a per-step DISPLACEMENT and read on the device
    // as 乱甩乱晃; see the round-16 section of tracking/pid_controller.h. The raw
    // error still reaches the derivative term so its state stays continuous;
    // otherwise it spikes every time the target crosses the stop band.
    //
    // `targetChanged` is passed straight through: the selection is "nearest
    // confirmed track", so it can land on a different enemy in one step, and
    // the controller has to be told rather than left to guess from the size of
    // the error jump. See TouchAimState::lastTargetId.
    // `measurementCorrected` goes down with `targetChanged` and for the same
    // reason: the caller knows something about the INPUT that the controller
    // cannot recover from it. On a corrected step Δe is the tracker's model
    // velocity plus the correction kick, and the kick grows with how far we
    // rotated the view — so without this flag the reconstruction reads the
    // loop's own handwriting as target motion, at gain `lookahead` through the
    // prediction term. (R23: that reconstruction now feeds ONLY the prediction
    // term. The trim used to read it as well, at gain 1, which is why this flag
    // used to matter twice over.)
    st.aim.step(dex, dey, dt, xInDz, (yInDz || yInYFollow), targetChanged,
                measurementCorrected, pidX, pidY);

    // ── Aim telemetry ──────────────────────────────────────────────────────
    //
    // ⚠ ROUND 22 — THE FIELD LIST BELOW IS THE OLD LINE'S and is kept as the
    // record of how the old loop was read. The line now printed is:
    //
    //   mean e  : unchanged — the error the controller saw (SCREEN px). It should
    //             hover within a pixel of zero on a moving target (the prediction
    //             term carries the command and the integral cleans up the rest)
    //             and go to zero outright when the target stops.
    //   tot     : unchanged — the total command this step, finger px.
    //   ff      : the prediction term's contribution, finger px. THE number to
    //             watch: it should settle at 前馈·u_ss and HOLD. On a 600 px/s
    //             strafe at 前馈 0.80 that is 0.8·(600/120)/0.10 = 40 finger px.
    //   v       : the reconstructed target velocity, SCREEN px/frame — the input
    //             the prediction term is built from, and the number this whole
    //             design exists to get. It should read the target's own speed,
    //             change sign cleanly at a reversal, and read 0 at rest. A `v`
    //             that keeps flipping sign while the crosshair sways is the
    //             prediction term chasing its own echo.
    //   tr      : TRUST in that velocity, 0…1. The controller's own detector for
    //             the fault above: it collapses when `v` is unstable, and it
    //             gates the prediction term down to 25 % when it does. Read it
    //             WITH `v` and `ff` — `tr` low and `ff` low means the loop has
    //             withdrawn its own excitation, which is the mechanism working,
    //             not a bug.
    //   og      : the output gain in force, 0.35…1.0. 1.0 at a steady lock; soft
    //             during a re-lock, which is what stops a target switch flinging.
    //   vok     : 1 once the velocity has been published at all (it needs six
    //             samples and two agreeing on a direction). 0 for the first few
    //             frames of an engagement is by design.
    //   trim/p/d: unchanged in kind — the residual cleaner, the proportional and
    //             the damping contributions, finger px.
    //
    // The rest is the old text.
    //
    // Deliberately at the aim's own rate rather than per frame: the numbers a
    // diagnosis needs are the ERROR and what the controller did about it, and
    // reading them at 120 Hz from logcat is neither possible nor useful. Four
    // seconds' worth of control at 120 Hz.
    //
    // Read the fields in this order — it is the tuning procedure, and each step
    // has a yes/no answer:
    //
    //   mean e  : the error the controller saw, averaged over the window.
    //             SIGN-CHANGING every window with a STATIONARY target means the
    //             loop is ringing → lower kp. ONE SIGN that stops growing while
    //             the target strafes means it is holding a steady offset → read
    //             `ff` next: if the feed-forward is idle, that is a 前馈增益
    //             problem, not a kp one.
    //   tot     : the controller's total output this step, in finger px. This is
    //             the only field that needs the plant gain to interpret, and the
    //             plant gain is not knowable here — so read it RELATIVELY. At a
    //             steady lock the rest of the line is pinned to it: `ff` should be
    //             about kf × tot, and `v` about tot (the reconstruction returns
    //             kSensFrozen × our own command, and kSensFrozen is 1.0).
    //             Those two ratios locate the fault without ever needing alpha.
    //             `tot` near `lim` means the finger cannot travel further in one
    //             step — the game's sensitivity is below what that ceiling can
    //             serve, so raise the ceiling constant, not the gains.
    //   ff      : the prediction term's contribution, in finger px. THE number to
    //             watch on a moving target. At a steady lock it should settle at
    //             about kf × tot (further scaled by `tr` and `og`) and HOLD there:
    //             this is the term that removes the lag without winding up. If the
    //             aim still trails while `ff` sits far BELOW kf × tot, the term is
    //             being THROTTLED rather than mis-tuned — read `tr` and `vok`
    //             before touching kf. At kf = 0 it is zero BY DESIGN.
    //   v       : the reconstructed target speed, in SCREEN px/step. Read it
    //             against `tot`, never against the target's real speed: at a
    //             steady lock the reconstruction is kSensFrozen × our own command,
    //             so `v ≈ tot` is the EXPECTED reading there (kSensFrozen is 1.0;
    //             it was 0.10 in the first build of this controller, which is one
    //             of the reasons `v` is worth watching). `v` far below
    //             that while `tot` is large means the reconstruction is not seeing
    //             our own output — check `vok` first, and only then the tracker.
    //             `v` collapsing to ~0 within a few frames of the target stopping
    //             IS the stop mechanism working (this replaced `cr`).
    //   d       : the damping term's contribution, in px. Pinned near 0 with
    //             kd > 0 means the damping term is dead; comparable to the
    //             feedback means kd is carrying the loop and kp has room.
    //   trim    : the integrator — the RESIDUAL cleaner, not the DC carrier (the
    //             prediction term does that). It should therefore stay SMALL. A
    //             large `trim` together with a small `ff` on a moving target means
    //             kf is wrong, by roughly the ratio between them. Pinned at its
    //             ceiling means the requirement exceeds what the loop may deliver
    //             even with the feed-forward: the aim is at its physical limit.
    //             See kAimTrimLimitPx.
    //   p       : the prediction term's raw contribution, in finger px — `ff`
    //             BEFORE the trust gate and the lookahead scaling. A healthy `p`
    //             with a suppressed `ff` points at the gate, not at the estimate.
    //   kf / lim
    //           : 前馈增益 in force (0.00–0.80 — the prediction term's share of the
    //             carrier, and the < 1 is the stability margin, not a taste) and
    //             the output ceiling it works against, in finger px/step. Read
    //             them together with `ff`: a strafing target whose `ff` stays near
    //             0 while `mean e` holds one sign is the whole "跟不上" failure,
    //             and the answer is a larger kf, not a larger kp. `yard` is the
    //             target's size (half its longer side, the "how far away is it"
    //             yardstick the selection pass already keeps) — the same px/s is a
    //             different physical motion on a distant box and one at contact
    //             range, so "it keeps up" is only meaningful at a stated box size.
    //   tr      : TRUST in the velocity estimate, 0..1 per axis — the gate on the
    //             prediction term. On an ordinary strafe it should read 0.85+.
    //             Persistently below ~0.5 while the target moves smoothly means the
    //             gate is mis-scaled (kTrustVelScale) and is taxing the carrier:
    //             the symptom is "能跟枪但总差一口气", never a shake. This replaced
    //             the old `stop`/`a` group — there is no stop detector to read now,
    //             because `v` going to 0 IS the stop.
    //   og      : the output gain, 0.35–1.0. It falls when the error is large, so a
    //             LOW `og` is expected just after a re-lock and during a jump. If it
    //             stays low while `mean e` is small, something is scaling the output
    //             that should not be.
    //   vok     : 1 when the reconstruction was refreshed this step, 0 when the
    //             step REUSED the previous estimate because the tracker had just
    //             re-anchored onto a detector result. A long run of 0 on a moving
    //             target means the estimate is starved and the prediction term has
    //             nothing to build from — a tracker-side problem, not a gain.
    //   id      : the track being engaged. A value that changes several times a
    //             second is a selection/identity problem, not a control problem,
    //             and no gain will fix it — read the tracker line's id list.
    {
        static int  sTick = 0;
        static float sSumX = 0.0f, sSumY = 0.0f;
        sSumX += dex; sSumY += dey;
        if (++sTick >= 480) {
            LOGI("aim: id=%d mean e=(%.1f,%.1f) tot=(%.2f,%.2f) "
                 "ff=(%.1f,%.1f) v=(%.1f,%.1f) d=(%.2f,%.2f) trim=(%.1f,%.1f) "
                 "p=(%.1f,%.1f) kf=%.2f tr=(%.2f,%.2f) og=(%.2f,%.2f) "
                 "vok=(%d,%d) lim=%.0f yard=%.0f dt=%.1fms",
                 st.lastTargetId,
                 sSumX / sTick, sSumY / sTick, pidX, pidY,
                 st.aim.ffX(), st.aim.ffY(),
                 st.aim.velX(), st.aim.velY(),
                 st.aim.derivX(), st.aim.derivY(),
                 st.aim.trimX(), st.aim.trimY(),
                 st.aim.propX(), st.aim.propY(),
                 // ROUND 22: 前馈 prints the slider's value unchanged (there is
                 // no guard) and the two characters after it are gone — they
                 // described the plant-gain estimator, which no longer exists.
                 // What replaced them is `tr`, trust in the velocity estimate,
                 // and `v`, the estimate itself.
                 st.aim.ffGainValue(),
                 st.aim.trustX(), st.aim.trustY(),
                 st.aim.outGainX(), st.aim.outGainY(),
                 st.aim.velReadyX() ? 1 : 0, st.aim.velReadyY() ? 1 : 0,
                 st.aim.outLimitPx(),
                 st.lastTargetBox, dt * 1000.0f);
            sTick = 0; sSumX = 0.0f; sSumY = 0.0f;
        }
    }

    // The per-axis freeze has ALREADY been applied inside the controller, and it
    // is not a plain "zero this axis": P and D are zeroed, the trim is HELD (not
    // accumulated and not leaked — see the three-state integral in
    // predictive_pid.h), and the prediction term is added as always. So the caller
    // must NOT zero the axis here — that would throw away the one term that is
    // legitimately still acting and turn the band into "the axis stops dead".
    // Round 15 dropped the integral's contribution inside the band instead, and
    // with kf = 0 that zeroed the axis completely: see the round-16 section of
    // tracking/pid_controller.h and PPID::update()'s `frozen` note.
    // ★ R23: the carrier is SPLIT between the prediction term (前馈's share) and the
    // trim (the rest), so "hold the trim" is what keeps the standing command alive
    // through the band. Freezing only the accumulation (the first R23 build) let
    // the trim's share leak away and read on the device as "准心到头上了就立马松手".
    float moveX = pidX;
    float moveY = pidY;

    // ── One-way fusion assist ───────────────────────────────────────────────
    // The player's finger may push the aim forwards, never backwards. Only the
    // component of the physical delta that lies ALONG the aim direction is
    // added; the opposing component is dropped. Projecting the delta (rather
    // than testing a dot-product sign and then adding the raw vector) is what
    // makes "helps, never hinders" true for every direction at once: a diagonal
    // drag that fights the aim still contributes its forward half, where the old
    // sign test threw the whole vector away.
    const float physLen2 = physDeltaX * physDeltaX + physDeltaY * physDeltaY;
    if (physLen2 > 0.25f) {  // > 0.5 px: below that it is panel jitter, not intent
        const float aimLen2 = moveX * moveX + moveY * moveY;
        if (aimLen2 > 0.25f) {
            const float invAim = 1.0f / std::sqrt(aimLen2);
            const float ux = moveX * invAim, uy = moveY * invAim;
            const float along = physDeltaX * ux + physDeltaY * uy;   // signed
            if (along > 0.0f) {
                moveX += ux * along;
                moveY += uy * along;
            }
            // along <= 0: the finger is pulling against the aim. Ignored — this
            // is the "不可以反向" half of the rule.
        } else {
            // Aim has no direction this frame (already converged) — let the
            // finger through so the player can still fine-tune by hand.
            moveX = physDeltaX;
            moveY = physDeltaY;
        }
    }

    st.position.x += moveX;
    st.position.y += moveY;
    uinput_move(slot, static_cast<int>(st.position.x), static_cast<int>(st.position.y));
}

// ── Touch-area hit testing & drag ───────────────────────────────────────────

/**
 * Where on the touch-area overlay did `mouse` land?
 *
 * Priority, top to bottom — corners first because a finger near a corner is
 * inherently near two edges, and the corner intent is the more specific one:
 *   1. any of the four corner squares
 *   2. any of the four edge strips (not overlapping the corner squares)
 *   3. the body (everything else inside the rectangle)
 *   4. None — the click is outside the overlay entirely
 */
TouchAreaOverlay::Drag hitTestTouch(const TouchAreaOverlay& area, ImVec2 mouse) {
    using Drag = TouchAreaOverlay::Drag;
    const float l = area.x, t = area.y;
    const float r = area.x + area.w, b = area.y + area.h;

    // Corners
    if (mouse.x >= l - kTouchCornerGrip && mouse.x <= l + kTouchCornerGrip &&
        mouse.y >= t - kTouchCornerGrip && mouse.y <= t + kTouchCornerGrip) return Drag::CornerTL;
    if (mouse.x >= r - kTouchCornerGrip && mouse.x <= r + kTouchCornerGrip &&
        mouse.y >= t - kTouchCornerGrip && mouse.y <= t + kTouchCornerGrip) return Drag::CornerTR;
    if (mouse.x >= l - kTouchCornerGrip && mouse.x <= l + kTouchCornerGrip &&
        mouse.y >= b - kTouchCornerGrip && mouse.y <= b + kTouchCornerGrip) return Drag::CornerBL;
    if (mouse.x >= r - kTouchCornerGrip && mouse.x <= r + kTouchCornerGrip &&
        mouse.y >= b - kTouchCornerGrip && mouse.y <= b + kTouchCornerGrip) return Drag::CornerBR;

    // Edge strips (inset so they don't overlap the corner squares above)
    if (mouse.y >= t + kTouchCornerGrip && mouse.y <= b - kTouchCornerGrip) {
        if (mouse.x >= l - kTouchEdgeGrip && mouse.x <= l + kTouchEdgeGrip) return Drag::EdgeL;
        if (mouse.x >= r - kTouchEdgeGrip && mouse.x <= r + kTouchEdgeGrip) return Drag::EdgeR;
    }
    if (mouse.x >= l + kTouchCornerGrip && mouse.x <= r - kTouchCornerGrip) {
        if (mouse.y >= t - kTouchEdgeGrip && mouse.y <= t + kTouchEdgeGrip) return Drag::EdgeT;
        if (mouse.y >= b - kTouchEdgeGrip && mouse.y <= b + kTouchEdgeGrip) return Drag::EdgeB;
    }

    // Body — inside the rectangle but outside every grip
    if (mouse.x >= l + kTouchEdgeGrip && mouse.x <= r - kTouchEdgeGrip &&
        mouse.y >= t + kTouchEdgeGrip && mouse.y <= b - kTouchEdgeGrip) return Drag::Move;

    return Drag::None;
}

/**
 * Drives the touch-area overlay one frame. Same shape as the slider's
 * press/drag/release: a click captures a drag mode + an anchor + a snapshot
 * of the geometry, every frame recomputes from `mouse - anchor` against the
 * snapshot, and `IsMouseDown == false` drops the drag.
 */
void driveTouchArea(TouchAreaOverlay& area, ImVec2 mouse, float screenW, float screenH) {
    using Drag = TouchAreaOverlay::Drag;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (area.drag == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Drag hit = hitTestTouch(area, mouse);
            if (hit != Drag::None) {
                area.drag    = hit;
                area.anchorX = mouse.x;
                area.anchorY = mouse.y;
                area.startX  = area.x;
                area.startY  = area.y;
                area.startW  = area.w;
                area.startH  = area.h;
            }
        }
        return;
    }
    if (!down) {
        area.drag = Drag::None;
        return;
    }

    const float dx = mouse.x - area.anchorX;
    const float dy = mouse.y - area.anchorY;

    switch (area.drag) {
        case Drag::Move: {
            area.x = area.startX + dx;
            area.y = area.startY + dy;
            break;
        }
        case Drag::EdgeL: {
            float nw = area.startW - dx;
            if (nw < kTouchMinSize) nw = kTouchMinSize;
            area.w = nw;
            area.x = area.startX + (area.startW - nw);
            break;
        }
        case Drag::EdgeR: {
            float nw = area.startW + dx;
            if (nw < kTouchMinSize) nw = kTouchMinSize;
            area.w = nw;
            break;
        }
        case Drag::EdgeT: {
            float nh = area.startH - dy;
            if (nh < kTouchMinSize) nh = kTouchMinSize;
            area.h = nh;
            area.y = area.startY + (area.startH - nh);
            break;
        }
        case Drag::EdgeB: {
            float nh = area.startH + dy;
            if (nh < kTouchMinSize) nh = kTouchMinSize;
            area.h = nh;
            break;
        }
        case Drag::CornerTL: {
            float nw = area.startW - dx;
            float nh = area.startH - dy;
            if (nw < kTouchMinSize) nw = kTouchMinSize;
            if (nh < kTouchMinSize) nh = kTouchMinSize;
            area.w = nw;
            area.h = nh;
            area.x = area.startX + (area.startW - nw);
            area.y = area.startY + (area.startH - nh);
            break;
        }
        case Drag::CornerTR: {
            float nw = area.startW + dx;
            float nh = area.startH - dy;
            if (nw < kTouchMinSize) nw = kTouchMinSize;
            if (nh < kTouchMinSize) nh = kTouchMinSize;
            area.w = nw;
            area.h = nh;
            area.y = area.startY + (area.startH - nh);
            break;
        }
        case Drag::CornerBL: {
            float nw = area.startW - dx;
            float nh = area.startH + dy;
            if (nw < kTouchMinSize) nw = kTouchMinSize;
            if (nh < kTouchMinSize) nh = kTouchMinSize;
            area.w = nw;
            area.h = nh;
            area.x = area.startX + (area.startW - nw);
            break;
        }
        case Drag::CornerBR: {
            float nw = area.startW + dx;
            float nh = area.startH + dy;
            if (nw < kTouchMinSize) nw = kTouchMinSize;
            if (nh < kTouchMinSize) nh = kTouchMinSize;
            area.w = nw;
            area.h = nh;
            break;
        }
        default: break;
    }

    // Final clamp to the screen — a drag past the edge is allowed (the
    // rectangle's own edge grip clamps itself), but the body can't end up
    // half off-screen.
    if (area.x < 0.0f) area.x = 0.0f;
    if (area.y < 0.0f) area.y = 0.0f;
    if (area.x + area.w > screenW) area.x = screenW - area.w;
    if (area.y + area.h > screenH) area.y = screenH - area.h;
}

// ── Trigger-circle hit testing & drag ───────────────────────────────────────

/**
 * Where on the trigger-area overlay did `mouse` land?
 *
 * Priority: the four cardinal handles (N/E/S/W) → the interior of the circle
 * → None. The interior (which now includes what used to be the explicit
 * centre handle) is the Move region — same pattern as the touch area's body,
 * so users only learn one drag rule across both overlays.
 *
 * The edge itself (anywhere on the circumference that is NOT inside a handle's
 * hit disc) returns None deliberately — the user said the edge cannot change
 * size, and silently letting it act as a Move handle would be ambiguous about
 * which behaviour they wanted.
 */
TriggerAreaOverlay::Drag hitTestTrigger(const TriggerAreaOverlay& area, ImVec2 mouse) {
    using Drag = TriggerAreaOverlay::Drag;
    const float hr = kTrigHandleHitR;

    if (dist2(mouse.x, mouse.y, area.cx,        area.cy - area.r) <= hr * hr) return Drag::ResizeN;
    if (dist2(mouse.x, mouse.y, area.cx + area.r, area.cy       ) <= hr * hr) return Drag::ResizeE;
    if (dist2(mouse.x, mouse.y, area.cx,        area.cy + area.r) <= hr * hr) return Drag::ResizeS;
    if (dist2(mouse.x, mouse.y, area.cx - area.r, area.cy       ) <= hr * hr) return Drag::ResizeW;

    // Anywhere inside the disc is Move — including the centre, which used to
    // be its own dedicated handle. Users had to aim at a small dot to drag;
    // now any touch inside the circle moves it, matching the touch area.
    if (dist2(mouse.x, mouse.y, area.cx, area.cy) <= area.r * area.r) return Drag::Move;

    return Drag::None;
}

/**
 * Drives the trigger-area overlay one frame. Same capture / snapshot pattern
 * as the touch-area drive, with one twist: the four cardinal resizes all use
 * the same math ("new radius = distance from centre to finger"), so they
 * share a switch arm. The shape stays a perfect circle by construction —
 * the centre is held fixed during any resize drag.
 */
void driveTriggerArea(TriggerAreaOverlay& area, ImVec2 mouse, float screenW, float screenH) {
    using Drag = TriggerAreaOverlay::Drag;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (area.drag == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Drag hit = hitTestTrigger(area, mouse);
            if (hit != Drag::None) {
                area.drag    = hit;
                area.anchorX = mouse.x;
                area.anchorY = mouse.y;
                area.startCX = area.cx;
                area.startCY = area.cy;
                area.startR  = area.r;
            }
        }
        return;
    }
    if (!down) {
        area.drag = Drag::None;
        return;
    }

    switch (area.drag) {
        case Drag::Move: {
            area.cx = area.startCX + (mouse.x - area.anchorX);
            area.cy = area.startCY + (mouse.y - area.anchorY);
            break;
        }
        case Drag::ResizeN:
        case Drag::ResizeE:
        case Drag::ResizeS:
        case Drag::ResizeW: {
            const float ndx = mouse.x - area.cx;
            const float ndy = mouse.y - area.cy;
            float nr = sqrtf(ndx * ndx + ndy * ndy);
            if (nr < kTrigMinR) nr = kTrigMinR;
            area.r = nr;
            break;
        }
        default: break;
    }

    // Final clamp — keep the whole circle on-screen.
    if (area.cx - area.r < 0.0f)         area.cx = area.r;
    if (area.cy - area.r < 0.0f)         area.cy = area.r;
    if (area.cx + area.r > screenW)     area.cx = screenW - area.r;
    if (area.cy + area.r > screenH)     area.cy = screenH - area.r;
}

}  // namespace

// ── Public struct implementations (out of line to keep the header tidy) ─────

void AimCategoryState::syncFromModel(const std::vector<std::string>& modelClasses) {
    // Always rebuild. The previous "skip if same size" optimisation was
    // wrong: a same-size list could be a stale cache from a different
    // model (or the previous app run), and empty-vs-empty is *exactly*
    // the case where the widget needs to reset to "(无)" — model unloaded.
    items = modelClasses;
    // The bitmask is 32 bits wide; anything beyond 32 classes has no bit to
    // live in and isClassSelected() would refuse it. Truncate here so the
    // widget count matches the mask width.
    if (items.size() > 32) items.resize(32);
    itemPtrs.clear();
    itemPtrs.reserve(items.size());
    for (const auto& s : items) itemPtrs.push_back(s.c_str());

    // First-ever sync with a non-empty list: select everything so the user
    // does not have to manually tick every class to start aiming. After this
    // the mask is owned by the user — a later unload+reload with the same
    // model keeps whatever they last picked (intentional: deselecting all
    // and reloading should NOT silently re-enable everything).
    if (!synced && !items.empty()) {
        uint32_t mask = 0;
        for (size_t i = 0; i < items.size(); ++i) mask |= (1u << i);
        sel.mask = mask;
        synced = true;
    }
}
// ── TouchAimState ──────────────────────────────────────────────────────────

void TouchAimState::press(const TouchAreaOverlay& area) {
    // Seed at the touch-area centre ± jitter. The touch area is the synthetic
    // finger's home (where the virtual thumb rests), not an aim filter and not
    // the PID error reference. The aim DIRECTION is independent of this point
    // because the PID error reference is the SCREEN CENTRE (see syncAimPage),
    // exactly as the old project's AimController — that is what stops the aim
    // from drifting toward a corner when the touch area is moved off-centre.
    position.x = area.x + area.w * 0.5f + nextJitter();
    position.y = area.y + area.h * 0.5f + nextJitter();
    start      = position;   // press point, for drag safety
    phase  = Phase::Pressed;
    // A press is normally a NEW engagement and starts from a clean controller.
    // It is not one when drag safety just lifted the same finger off the same
    // target: that is a re-anchor of the finger's position, and the trim (which
    // encodes the target's velocity and the game's sensitivity) is still valid.
    // Without this the integrator was wiped about twice a second at 720 px/s,
    // which is why the trailing error never went away.
    if (carryTrim) carryTrim = false;
    else           aim.reset();
    justReleasedTakeover = false;
}

void TouchAimState::release() {
    phase = Phase::Idle;
    drivingReal = false;
    realId = -1;
    // -1 means "no physical finger was tracked", so the next frame cannot
    // mistake the gap for a large finger movement.
    lastPhysX = lastPhysY = -1.0f;
    // A release normally ends the engagement, so any pending "this press is just
    // a re-anchor" marker is stale by the time the next press happens. The one
    // caller that wants the trim carried over sets the flag AFTER calling this —
    // see the drag-safety path in syncAimPage().
    carryTrim = false;
    // Same reasoning for the tracked id: a released aim is not engaging anything,
    // and leaving the old id here would make the next engagement's first frame
    // look like a switch (and needlessly clear the trim it just inherited).
    lastTargetId = -1;
    // And for the position it was last seen at — the switch test confirms an id
    // change against movement, so a stale position from a previous engagement
    // would make a fresh lock look like a jump to another enemy.
    lastTargetCx = lastTargetCy = -1.0f;
    lastTargetBox = 0.0f;
    // NOTE: justReleasedTakeover is intentionally left as-is here — the takeover
    // release paths set it true just before calling release(), and the Idle
    // branch clears it after reading. Resetting it here would defeat the guard.
}

// ── drawAimSection ──────────────────────────────────────────────────────────

void drawAimSection(ImDrawList* dl, float x, float& y, float w,
                    float /*bottomY*/, float s, float es, const Xf& xf,
                    Scroll& sc) {
    const float gap   = csize(12.0f) * s;
    const float rowSw = csize(widgets::kSwitchRowH)   * s;
    const float rowSl = csize(widgets::kSliderRowH)   * s;
    const float rowDd = csize(widgets::kDropdownRowH) * s;

    // Each row's rect is built at the scrolled screen y so paint coordinates
    // and hit-test rectangles stay in lock-step. `y` keeps advancing in natural
    // (un-scrolled) coordinates, which is what lets drawContent compute the
    // page's total content height for the scroll-range clamp.
    auto wRect = [&](float wx, float wy, float ww, float wh) {
        return wRectOf(wx, wy - sc.offset, ww, wh, s, xf);
    };

    // ── Controller parameters ───────────────────────────────────────────────
    // All FOUR gains are per control step and share one unit system — see the
    // class note in tracking/predictive_pid.h for what each one does now, and the
    // note on PageAim for why the shipped set is what it is (Kp 0.40 / Ki 0.20 /
    // Kd 0.60 / Kf 0.80).
    //
    // ROUND 22: the labels did not change and the ORDER did not change, but two
    // of the four changed what they mean. Kf is now 前馈 — the fraction of the
    // standing command the prediction term supplies, capped at 0.80 — and Ki is
    // no longer the carrier. Both changes are the point of the round: the
    // integral could not be cleared while it was the carrier, and not being able
    // to clear it is what left the aim swaying after a target stopped.
    //
    // The order is still the order they act in: Kp (proportional), Ki (the
    // residual cleaner), Kd (damping), Kf (the carrier), with Kf directly under
    // Kd as of round 18.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.kp,        "Kp",       2, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.ki,        "Ki",       2, es);
    y += rowSl + gap;
    // Kd is PER STEP and dimensionless now (it used to be per-second, which made
    // its real weight kd*120 and put the useful part of the slider in its first
    // few pixels). Two decimals match the 0.01 step.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.kd,        "Kd",       2, es);
    y += rowSl + gap;
    // Kf — 前馈, the prediction term's share of the carrier. 0.00–0.80, step 0.01,
    // default 0.80, and the ceiling is a hard stability limit rather than a
    // preference: the term is a delayed copy of our own output with coefficient
    // exactly this value, so `Kf < 1` is the margin and 0.80 is 20 % inside it.
    //
    // SET IT HIGH. This is now the term that follows a moving target; Ki only
    // cleans up what is left over. Below about 0.5 the integral has to make up
    // the difference — which is the old arrangement, and brings the old
    // behaviour (a charge that cannot be dropped at rest) with it.
    //
    // ⚠ The long note this row used to carry described kf as a strength on a
    // feed-forward scaled by 1/alpha_hat, with an on-line estimator under it and
    // a stability window derived from the estimator's error. None of that is
    // true any more — there is no estimator in the loop at all. The history is
    // kept in the PageAim note above the row's own declaration; what matters at
    // the row is the sentence before this one.
    // ⚠ LABEL RENAMED "Kf" → "前馈". The row keeps its place and its 0.00–0.80
    // range, but "Kf" was actively misleading: it is the name of a knob the user
    // has years of muscle memory for, in a loop where that name meant something
    // else, and the first thing that happened with this controller was exactly the
    // predictable thing — 前馈 was left at 0.16, a perfectly reasonable OLD kf, at
    // which the integral has to supply 84 % of the carrier. That number is not a
    // small mistake in the new architecture, it is most of the command, so the row
    // announces what it now is.
    //
    // NOTE the comment above this line still refers to it as Kf in places; the
    // name change is the labels' business, not the notes'.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.ffGain,    "前馈",     2, es);
    y += rowSl + gap;
    // 输出平滑 — EMA on the controller's OWN output. 0..1, where 1.0 is OFF.
    //
    // WHAT IT IS FOR: the finger command is a position delta per step, and every
    // term in it carries the tracker's frame-to-frame noise — D most of all,
    // because it differences a noisy signal, and F with it. This row is the last
    // filter before that noise reaches the game as visible crosshair jitter. It
    // does not change WHERE the loop settles, only how rough the ride there is.
    //
    // WHAT IT COSTS: an EMA is pure phase lag, and phase lag is the one thing a
    // delay-limited loop cannot afford — it eats the phase margin Kd just bought.
    // That is why it shipped at 1.0 (off) for years. 0.85 keeps a real cut of the
    // step-to-step noise for a small, bounded lag: raise it toward 1.0 if the aim
    // feels sluggish or overshoots, lower it if the crosshair visibly trembles.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.outSmooth, "输出平滑", 2, es);
    y += rowSl + gap;
    // 延迟补偿 — the loop's ONLY lead term, in detector frames:
    //     target += trackerVelocity × this
    //
    // WHAT IT IS FOR: the screenshot the detector saw is already a few frames old
    // by the time a touch lands, so a target moving at v px/frame is v·(that
    // delay) px further along than the box says. This term aims that far ahead, so
    // the loop stops lagging a moving target. That is its whole job.
    //
    // WHAT IT COSTS: it enters the output through Kp — the same channel as Kf —
    // and, more subtly, it used to MOVE THE POINT THE LOOP CONSIDERS "ARRIVED".
    // A lead is a steady-state offset: while the target keeps moving, v is
    // non-zero, so the aim could sit satisfied at box centre + v·this, which is
    // outside the box. That is exactly the round-17 "准心一直锁在框旁边" report,
    // and "移动一下屏幕就正常了" was its fingerprint (a re-association zeroes the
    // track's velocity, so the offset collapses). Round 17's fix is that the
    // deadzone and the error are measured against the RAW box centre — see
    // rawTarget in syncAimPage — so this term shifts what the loop aims AT but
    // never what counts as on-target.
    //
    // Range 0–5, step 0.05. Raise it if the crosshair trails a steadily moving
    // target; lower it (or zero it) if it overshoots one that changes direction —
    // a lead is a bet that the target keeps going.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.aimDelayFrames, "延迟补偿", 2, es);
    y += rowSl + gap;
    // 输出限幅 used to be a row here, and it never was a tuning parameter: it is
    // the ceiling that stops a re-lock from flinging the finger across the panel.
    // It is the constant tracking::kOutLimitPx (180 finger px/step, tanh), with a
    // separate, smaller leash on the integral (kTrimLimitPx, 150 since round 14 —
    // at 90 the leash was a hard ceiling on how fast a target the integral could
    // hold at all, not a safety rail). Neither is scaled by anything.

    // ── Aim deadzone ────────────────────────────────────────────────────────
    // 0.0 = move onto the target CENTRE; 1.0 = stop at the target EDGE. The stop
    // radius is a fraction of the target box's half-size, so 0.05 still lands on
    // the target and 1.0 stops at its nearest edge. Per-axis, not a circle, and
    // 0.0 disables it.
    //
    // WHAT IT ACTUALLY DOES (round 16 — the honest version). Inside the band P
    // and D stop driving, which is what this row is for: those two are what
    // amplify the detector's jitter. The TRIM is NOT switched off — it stops
    // accumulating and is pulled toward what the reconstruction measures the
    // target to need, so at rest it goes to zero on its own (the 静止时一直晃 is
    // fixed for real) while a strafe keeps the carrier the loop taught it. Round
    // 15 additionally removed the trim from the output, and that is what made the
    // aim fling: a per-step displacement switched off and on at the band's own
    // rate is a velocity step as large as the trim itself, at every crossing.
    //
    // SO: this row is for detector JITTER only, and it does not need to be big.
    // 0.1…0.3 quiets a box that twitches; 1.0 also blocks the target's real
    // motion, and what you feel then is the aim refusing to move. Two decimals
    // and a 0.05 step on purpose: finding the smallest value that quiets the
    // jitter is the whole exercise.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.deadzone, "死区", 2, es);
    y += rowSl + gap;

    // ── Touch-area toggle ──────────────────────────────────────────────────
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageAim.touchArea.toggle,
                          "触摸区域", es);
    y += rowSw + gap;

    // ── Touch-fusion toggle ───────────────────────────────────────────────
    // ON: aim takes over the real finger held inside the touch area and moves
    // it — no second touch point appears. The finger's own movement is added
    // when it points the same way as the aim and dropped when it points the
    // other way (helps, never fights).
    // OFF: aim presses its own synthetic finger; the player's finger is left
    // alone.
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageAim.fusion,
                          "触摸融合", es);
    y += rowSw + gap;

    // ── Continuous-trigger toggle ──────────────────────────────────────────
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageAim.continuousTrigger,
                          "持续触发", es);
    y += rowSw + gap;

    // ── Trigger-area toggle (hidden when Continuous Trigger is on) ──────────
    if (!g_pageAim.continuousTrigger.value) {
        widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageAim.triggerArea.toggle,
                              "触发区域", es);
        y += rowSw + gap;
    }

    // ── Aim-category multi-select ──────────────────────────────────────────
    // Always shown — the mask defaults to "all classes" so an unconfigured
    // page still aims at everything the model knows about. The widget reads
    // g_pageAim.aimCategory.itemPtrs, which syncAimPage() rebuilds from the
    // model snapshot at the start of every frame.
    //
    // The open list is painted DEFERRED (multiSelectList at the end of this
    // function): the board shares one draw list, so an inline list would be
    // covered by the Y死区 rows drawn below it.
    const widgets::Rect catRow = wRect(x, y, w, rowDd);
    widgets::multiSelect(dl, catRow, g_pageAim.aimCategory.sel,
                         "瞄准类别",
                         g_pageAim.aimCategory.itemPtrs.empty()
                            ? nullptr
                            : g_pageAim.aimCategory.itemPtrs.data(),
                         static_cast<int>(g_pageAim.aimCategory.itemPtrs.size()),
                         es, /*drawListInline=*/false);
    y += rowDd + gap;

    // ── Per-class Y follow ───────────────────────────────────────────────
    // One slider per *selected* class only — unselected classes show nothing.
    // Values for unselected / temporarily-gone classes are kept, so unticking
    // does not wipe tuning. Stale indices (model switched to fewer classes)
    // are pruned, but only while a class list exists — an unloaded model must
    // not wipe the map.
    const size_t nCls = g_pageAim.aimCategory.itemPtrs.size();
    if (nCls > 0) {
        for (auto it = g_pageAim.yFollow.begin(); it != g_pageAim.yFollow.end();) {
            if (it->first < 0 || static_cast<size_t>(it->first) >= nCls)
                it = g_pageAim.yFollow.erase(it);
            else
                ++it;
        }
        for (size_t i = 0; i < nCls && i < 32; ++i) {
            if (!g_pageAim.aimCategory.isClassSelected(static_cast<int>(i))) continue;
            auto it = g_pageAim.yFollow.find(static_cast<int>(i));
            if (it == g_pageAim.yFollow.end()) {
                it = g_pageAim.yFollow
                         .emplace(static_cast<int>(i),
                                  widgets::SliderState{0.0f, 0.0f, 1.0f, 0.1f})
                         .first;
            }
            char label[128];
            snprintf(label, sizeof(label), "Y死区·%s",
                     g_pageAim.aimCategory.items[i].c_str());
            widgets::sliderFloat(dl, wRect(x, y, w, rowSl), it->second, label, 1, es);
            y += rowSl + gap;
        }
    }

    // ── Deferred category list ───────────────────────────────────────────
    // Painted after every row so the open list floats above the Y死区
    // sliders (same pattern as the Add-Model dialog's dropdownList calls).
    widgets::multiSelectList(dl, catRow, g_pageAim.aimCategory.sel,
                             g_pageAim.aimCategory.itemPtrs.empty()
                                ? nullptr
                                : g_pageAim.aimCategory.itemPtrs.data(),
                             static_cast<int>(g_pageAim.aimCategory.itemPtrs.size()),
                             es);
}

// ── drawAimOverlays ─────────────────────────────────────────────────────────

void drawAimOverlays() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    if (screenW <= 1.0f || screenH <= 1.0f) return;

    // Geometry must exist for the aim loop even when the overlays are hidden
    // (their toggles only control *drawing*). Centres everything by default.
    ensureAimAreasPlaced(screenW, screenH);

    // ── Touch-area overlay ──────────────────────────────────────────────────
    if (g_pageAim.touchArea.toggle.value) {
        TouchAreaOverlay& area = g_pageAim.touchArea;
        if (!area.placed) {
            area.x = (screenW - area.w) * 0.5f;
            area.y = (screenH - area.h) * 0.5f;
            area.placed = true;
        }
        driveTouchArea(area, mouse, screenW, screenH);

        const ImVec2 mn(area.x, area.y);
        const ImVec2 mx(area.x + area.w, area.y + area.h);
        const ImVec2 centre(area.x + area.w * 0.5f, area.y + area.h * 0.5f);

        dl->AddRectFilled(mn, mx, kOverlayFill, 4.0f);
        drawDashedRect(dl, mn, mx, kOverlayBorder,
                       kTouchThickness, kTouchDashLen, kTouchDashGap);
        drawCornerHandles(dl, mn, mx, kTouchCornerDraw, kOverlayHandle);
        drawCentredLabel(dl, centre, kOverlayLabelSize, kOverlayLabel, "触摸区域");
    }

    // ── Trigger-area overlay ────────────────────────────────────────────────
    if (g_pageAim.triggerArea.toggle.value) {
        TriggerAreaOverlay& area = g_pageAim.triggerArea;
        if (!area.placed) {
            area.cx = screenW * 0.5f;
            area.cy = screenH * 0.5f;
            area.placed = true;
        }
        driveTriggerArea(area, mouse, screenW, screenH);

        const ImVec2 c(area.cx, area.cy);
        dl->AddCircleFilled(c, area.r, kOverlayFill, kTrigSegments);
        dl->AddCircle      (c, area.r, kOverlayBorder, kTrigSegments, kTrigThickness);

        dl->AddCircleFilled(ImVec2(area.cx,        area.cy - area.r), kTrigHandleDraw, kOverlayHandle);
        dl->AddCircleFilled(ImVec2(area.cx + area.r, area.cy       ), kTrigHandleDraw, kOverlayHandle);
        dl->AddCircleFilled(ImVec2(area.cx,        area.cy + area.r), kTrigHandleDraw, kOverlayHandle);
        dl->AddCircleFilled(ImVec2(area.cx - area.r, area.cy       ), kTrigHandleDraw, kOverlayHandle);
        drawCentredLabel(dl, c, kOverlayLabelSize, kOverlayLabel, "触发区域");
    }
}

// ── syncAimPage — the PIDF + uinput loop ────────────────────────────────────

void syncAimPage() {
    PageAim& p = g_pageAim;

    // 丢框预测帧数 (predictHoldFrames) used to be published here. It moved to the
    // Settings page in round 18 — it is tracker behaviour, not a gain, and it
    // belongs next to 丢失帧; syncSettingsPage() writes it now.

    // ── Pull the latest class list off the model every frame ──────────────
    // Cheap — `snapshot()` copies a small struct and a short vector. The
    // widget reads `itemPtrs` later in this frame, so we sync BEFORE
    // drawAimSection() runs. `snap.classes` is populated as soon as `load()`
    // puts the entry's classes on the snapshot, so a model whose load
    // succeeded but whose worker has not yet flipped `active` is NOT skipped.
    const infer::runtime::Snapshot snap = infer::runtime::snapshot();
    std::vector<std::string> classLabels = snap.classes;

    // Placeholder fallback. Models whose TSV row has no class names (older
    // entries, or models that ship without an embedded label file) end up
    // with an empty `snap.classes` even when `classCount > 0` — the snapshot
    // only carries the *names*, not the count. Pull the loaded entry out of
    // model_store and synthesise "0", "1", …, "N-1" so the widget has
    // something to show and the aim loop can still target each class.
    // Gated on `classLabels.empty()` only — NOT on `snap.active`: the load
    // event, not the start of inference, is what makes the category list
    // valid, and gating on `active` made the widget stay "(无)" until the
    // user tapped 推理.
    if (classLabels.empty()) {
        const auto entries = model::all();
        for (const auto& e : entries) {
            if (e.loaded && e.classCount > 0) {
                classLabels.reserve(static_cast<size_t>(e.classCount));
                for (int i = 0; i < e.classCount; ++i) {
                    classLabels.push_back(std::to_string(i));
                }
                break;
            }
        }
    }
    p.aimCategory.syncFromModel(classLabels);

    // ── Ensure aim-area geometry exists (toggles only control drawing) ──────
    // The touch-area centre/size and the trigger-area circle are read by the
    // loop below even when their overlays are hidden, so they must be placed
    // (centred by default) regardless of the toggle state.
    const ImGuiIO& io = ImGui::GetIO();
    ensureAimAreasPlaced(io.DisplaySize.x, io.DisplaySize.y);

    // ── Feed raw detections into the multi-target Kalman tracker ────────────
    // Runs every frame the model is active (regardless of the aim master
    // switch) so tracks stay warm and the aim loop consumes smoothed, stable
    // tracks with a velocity estimate instead of the raw jittering boxes.
    //
    // This is called from drawHud(), i.e. the 120 Hz render loop, while the
    // detector completes at ~60 Hz. Half of these calls therefore hold the very
    // same list the tracker already consumed, and the tracker is told which is
    // which. Re-associating a repeat does not "refresh" anything: it corrects
    // the filter back toward a measurement that is already a frame old, which
    // drags the published position backwards (the box visibly lags) and makes
    // (vx, vy) ring instead of converge — and lead/feed-forward both multiply
    // those, so the ripple comes out as finger shake. `detSeq` is bumped under
    // the same lock that swaps the list, so comparing it is exact.
    static uint64_t aimLastDetSeq = 0;
    // Hoisted out of the `snap.active` block (round 20) because the aim loop
    // below needs it too. It is the difference between the two laws the tracker's
    // published centre follows: on a step with NO new detector result, predictAll
    // advanced it by exactly its own (vx, vy); on a step WITH one, matchAndUpdate
    // pulled it back onto a measurement that is already a render frame old. Only
    // the first is a velocity, and the controller has to be told which step it is
    // looking at — see the `measurementCorrected` parameter of
    // AimController::update(). Default is the permissive one, so a frame with no
    // tracker running can never gate anything off.
    bool freshDetections = true;
    if (snap.active) {
        freshDetections = (snap.detSeq != aimLastDetSeq);
        aimLastDetSeq = snap.detSeq;
        // No timestep is passed. The tracker's motion model is one RENDER frame
        // per call and its velocity is px per render frame, so the rate is
        // implicit; the only consumer that needed wall-clock time was the
        // cruise-velocity filter, which is gone.
        tracking::tracker().update(snap.detections, freshDetections);
    } else {
        aimLastDetSeq = 0;
        tracking::tracker().reset();
    }

    // ── Copy slider values into the controller on every frame ────────────────
    // This must be a PURE assignment. It used to run through init(), which ended
    // in reset(), so the integrator and the derivative history were wiped once
    // per render frame — see AimController::setGains() in the header and the root
    // cause note at the top of tracking/predictive_pid.h. Applying gains and
    // clearing state are now separate operations, and only a new engagement (or
    // a re-anchor-free press) calls reset().
    //
    // Four gains and the output EMA, as before; the fifth argument is 前馈, whose
    // MEANING changed in round 22 while its row kept its name and its place. It
    // was a strength on a feed-forward whose other factor was 1/alpha_hat (an
    // on-line estimate, and the source of most of rounds 1–21); it is now the
    // FRACTION OF THE CARRIER the prediction term supplies, with a hard ceiling
    // of tracking::kLookaheadMax because the term is a delayed copy of our own
    // output and `前馈 < 1` is the stability margin. Everything else the old code
    // needed here — the estimator, the guard, the rate limit on kf — is gone
    // with the controller. The remaining loop constants are
    // tracking::kAimOutLimitPx / kAimTrimLimitPx / kFfLimitPx.
    p.touchAim.aim.setGains(
        p.kp.value, p.ki.value, p.kd.value, p.outSmooth.value,
        p.ffGain.value);

    // ── Decide whether the aim loop may fire ────────────────────────────────
    //   * master switch on
    //   * an injection backend is up — asked of the injector, NOT of uinput.
    //     This used to test uinput_is_ready(), which asks "is there a /dev/uinput
    //     device?" — true only while the uinput backend is the live one, so
    //     under the InputManager backend (where the virtual device is destroyed
    //     on purpose, see input/inject_backend.h) the aim silently did nothing
    //     at all. The question the aim actually has is "can a finger of mine
    //     reach the system?", which is what inject_is_ready() answers for
    //     whichever backend is selected.
    //   * a "hold" is active: continuous-trigger OR a real finger held inside
    //     the trigger area — that is what makes the trigger area meaningful
    //     (it is the fire zone). The touch-area / trigger-area *toggles* do
    //     NOT gate aiming; they only show/hide the overlays, exactly as the
    //     user asked.
    // When the hold drops mid-press, the synthetic finger is released so the
    // game never sees a phantom finger stuck down.
    const bool holdActive = p.continuousTrigger.value || realFingerInTriggerArea();
    // Back-flash blocks aim during its whole cycle (it owns UINPUT_SLOT_PRIMARY
    // — see backflash_section.h). The flag is set by syncBackFlashPage() which
    // runs BEFORE this function so it is current; we just read it.
    const bool blockedByBackFlash = backflashIsBlockingAim();
    const bool canAim = p.enabled.value && inject_is_ready() && holdActive && !blockedByBackFlash;
    if (!canAim) {
        if (p.touchAim.phase == TouchAimState::Phase::Pressed) {
            // Hand a taken-over real finger back to the mirror (no uinput_up, so it
            // does not flicker down/up); lift a synthetic finger normally.
            if (p.touchAim.drivingReal) uinput_release_takeover();
            else                        uinput_up(p.touchAim.slot);
            p.touchAim.release();
        }
        return;
    }

    // ── Find the best target detection ────────────────────────────────────
    // Select the detection closest to the SCREEN CENTRE (crosshair origin). The
    // touch area is NOT a filter — any selected-class target anywhere on screen
    // is eligible; the crosshair reference just keeps the pick stable
    // frame-to-frame. The PID error itself is also referenced to the screen
    // centre (see the step() call below) — that is the fix for the
    // "screen-centre coordinate" bug: using the touch-area / live-finger
    // position as the reference made the aim drift toward a corner whenever the
    // overlay was moved. The touch area is only the synthetic finger's HOME
    // (press/move point).
    const ImVec2 screenCenter(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    ImVec2 target(0, 0);
    float targetW = 0.0f, targetH = 0.0f;
    /// The tracker's velocity (px per RENDER frame). It has exactly ONE consumer
    /// left: the 延迟补偿 lead, which is off by default. It is NOT fed to the
    /// controller any more, and it cannot be: it is the box's SCREEN velocity, so
    /// it contains the aim's own output, and a well-tracked target reports ~0.
    /// See note 2 in the header of tracking/pid_controller.h.
    /// The tracker has exactly one velocity: the slow "cruise" estimate that used
    /// to sit here was measured to cost more phase lag than the jitter it removed
    /// — see the note on TrackedTarget in kalman_tracker.h.
    float bestVelX = 0.0f, bestVelY = 0.0f;
    int   bestCls = -1;
    int   bestId  = -1;
    bool hasTarget = false;
    float bestDistSq = 1e18f;

    /// Hysteresis on re-selection: the engaged track is kept unless a rival is
    /// more than this many times closer (in distance) to the crosshair.
    constexpr float kStickyReselect = 1.5f;

    // ── Pick the target ──────────────────────────────────────────────────────
    // Nearest confirmed track to the crosshair — with two rules whose only job
    // is to stop the ENGAGED ID changing while the enemy has not.
    //
    //   1. A `lost` track never outranks a live one. A track that has missed its
    //      measurements is still published for a few frames, so a one-frame
    //      dropout does not break the lock, and it is held at its last known
    //      position. But when a live track exists, the held one must not compete
    //      with it: the case that produces both at once is precisely a failed
    //      association — the held track plus the fresh one that replaces it —
    //      and the two then sit a few px apart, so "nearest" alternates between
    //      them and the id visibly flip-flops. Live tracks are therefore tried
    //      in a first pass and held ones only if that pass finds nothing, which
    //      is the brief-occlusion case they exist for.
    //
    //   2. Sticky re-selection. The engaged track is scored as if it were
    //      kStickyReselect times closer than it is, so a rival has to be
    //      substantially nearer to take over. This matters more here than in a
    //      general tracker because ONE ENEMY CAN LEGITIMATELY PRODUCE TWO BOXES:
    //      NMS upstream is per class, so a head/body model yields a head box and
    //      a body box, and their centres are tens of px apart rather than zero.
    //      Without hysteresis the pick flips between them as the target crosses
    //      the crosshair — one enemy, two ids, alternating.
    //
    // Neither rule changes WHERE the aim points for a given track; they only
    // decide which track is the target, so aim precision is untouched.
    const auto& tracks = tracking::tracker().tracks();
    if (p.aimCategory.hasAnySelected()) {
        for (int pass = 0; pass < 2 && !hasTarget; ++pass) {
            const bool wantLost = (pass == 1);
            for (const auto& t : tracks) {
                if (!t.confirmed) continue;
                if (t.lost != wantLost) continue;
                if (!p.aimCategory.isClassSelected(t.cls)) continue;
                const float dx = t.cx - screenCenter.x;
                const float dy = t.cy - screenCenter.y;
                const float dSq = dx * dx + dy * dy;
                // The sticky bonus, applied as a distance discount so a single
                // comparison covers both "is this the engaged track" and "is a
                // rival far enough ahead to take over".
                const float eff = (t.id == p.touchAim.lastTargetId)
                    ? dSq / (kStickyReselect * kStickyReselect)
                    : dSq;
                if (eff >= bestDistSq) continue;
                bestDistSq = eff;
                target     = ImVec2(t.cx, t.cy);
                bestVelX   = t.vx;
                bestVelY   = t.vy;
                targetW    = t.w;
                targetH    = t.h;
                bestCls    = t.cls;
                bestId     = t.id;
                hasTarget  = true;
            }
        }
    }

    // ── Lead (inference delay compensation) ──────────────────────────────────
    // Push the chosen target along the tracker's estimated velocity for the
    // configured number of frames. 0 disables lead (current position becomes
    // the target, like a non-predictive aim).
    //
    // This is now the ONLY consumer of the tracker's raw Kalman velocity, and
    // that is on purpose. The velocity's time constant at 120 Hz is 1.4 frames
    // (K_vel = 0.422 from the tracker's own noise ratio), i.e. it is a
    // differentiator rather than an estimate — which is exactly why the feed-
    // forward no longer touches it and why this slider defaults to 0. A lead is
    // an explicit bet that the target keeps going, so it is left to the user and
    // off by default; the feed-forward does the actual lag-killing, and it does
    // it without needing this signal at all.
    // ── ROUND 17: keep the RAW centre alongside the lead-shifted one ──────────
    // This pair is the fix for "准心一直锁在框旁边，框在左边". The lead point is
    // where the loop is TOLD to converge, so it is also a genuine EQUILIBRIUM:
    // with any lead at all the aim comes to rest at e ≈ 0 with the crosshair at
    // boxCentre + v×lead — up to a whole box off the target — and it stays there
    // for as long as the target keeps moving. The deadzone is therefore measured
    // against `rawTarget` (the box the HUD draws, i.e. the box the user sees)
    // while the controller still receives the lead-shifted error, so lead keeps
    // buying the approach and no longer gets a vote on where the aim parks.
    // That is also why "移动一下屏幕就正常了": a re-association zeroes the
    // track's velocity, the v×lead offset collapses to 0, and the aim falls back
    // onto the box. See the round-17 note on PageAim::deadzone.
    const ImVec2 rawTarget = target;   // the box centre, before any lead
    // ── ROUND 19c: the velocity the lead multiplies MUST be low-passed ────────
    //
    // This is the FOURTH copy of one mistake, and the only one still live. The
    // note above lists the other three: `bestVel * 1/alpha`, the self-calibrating
    // estimator, and the gate around it. All three were removed for the same
    // reason — the tracker's velocity is the box's SCREEN velocity and the aim's
    // own output is inside it, so anything that multiplies it feeds our own
    // command back into the loop through a delay. The one that oscillated did so
    // at 1453 px peak-to-peak at alpha = 2.5. The lead never got the same
    // treatment because it is multiplied by a slider that defaults to 0, so it
    // was assumed to be off; it is not off for a user who turns it up, and it
    // is the same plant.
    //
    // WHY IT IS NOT BOUNDED BY ANY GAIN ROW. The velocity's own time constant is
    // 1.4 render frames (K_vel = 0.422 — see the note above), i.e. it is a
    // DIFFERENTIATOR, not an estimate. Lead multiplies that by a frame count and
    // adds the product to the point the loop is told to converge to, so the loop
    // sees tau x (d/dt of box position) — and the box's position contains
    // alpha x u. A differentiator inside a delay loop is a phase LEAD, and the
    // gain around it is tau x alpha x (how hard the aim is driving): kp, ki and
    // kf only set how hard "how hard" is. That is why "kf 随便拉都震荡" was
    // literally true, why ki = 0.01 did not help, and why the swing only appears
    // while the crosshair is ON the target (that is when the loop is driving and
    // when the deadzone stops absorbing the wobble). The user's own measurement
    // is the cleanest evidence there is: at 延迟补偿 = 0.2 it "一直不停的抖根本
    // 不平衡", at 0 it went smooth, and it "又好了" once turned back on after the
    // loop had settled.
    //
    // THE FIX IS A SEPARATION OF BANDS, not a gain. The ring and the self-term
    // both live at the detector's rate (they are frame-to-frame differences of a
    // noisy measurement), while the thing the lead is trying to predict — where
    // the enemy will be 0-5 render frames from now — is a target that changes
    // direction over hundreds of milliseconds. One first-order low-pass on the
    // LEAD'S OWN COPY separates them: the ring is attenuated by ~10x, the target's
    // real motion passes nearly untouched, and nothing the controller consumes is
    // changed — the track keeps its raw velocity for everything else.
    //
    // The filter is re-seeded on a track CHANGE (a new enemy has a different
    // velocity and no history here is meaningful) and cleared when the lead is
    // off, so switching the slider on starts from the measurement rather than
    // from a stale value. This does NOT remove the self-term — under R23 its size
    // is set by lookahead x (1 - alpha_true/kSensFrozen), i.e. by a slider that is
    // bounded below 1 and a FROZEN constant, so it is bounded by design instead of
    // by an estimate that could be wrong in either direction. What this filter
    // removes is the part of it that lives above the target's own bandwidth, which
    // is where the oscillation is.
    static float leadVelX = 0.0f;
    static float leadVelY = 0.0f;
    static int   leadVelId = -1;
    if (hasTarget && p.aimDelayFrames.value > 0.0f) {
        const float dtl = std::clamp(io.DeltaTime, 1.0f / 2000.0f, 0.2f);
        const float av  = dtl / (dtl + kLeadVelTauSec);
        if (bestId != leadVelId) {          // new track: no history, take the sample
            leadVelX  = bestVelX;
            leadVelY  = bestVelY;
            leadVelId = bestId;
        } else {
            leadVelX += (bestVelX - leadVelX) * av;
            leadVelY += (bestVelY - leadVelY) * av;
        }
        target.x += leadVelX * p.aimDelayFrames.value;
        target.y += leadVelY * p.aimDelayFrames.value;
    } else {
        leadVelId = -1;                     // lead off: do not carry a stale velocity in
    }

    // ── Aim deadzone: 0.0 = move onto the target CENTRE; 1.0 = stop with the
    // crosshair anywhere inside the box. Distance- and resolution-independent,
    // because the radius is a fraction of the TARGET'S OWN box.
    //
    // ROUND 17 — the radius is now PER AXIS (X: w/2, Y: h/2). It used to be
    // `value * 0.5*min(w,h)` for BOTH axes, so on a tall box the X band was a
    // third of the X half-width it claimed to be; "框内的百分比" is per axis or it
    // is not a percentage of the box at all. The per-class Y band below was
    // already h-based, which is why the two disagreed on anything but a square.
    const float targetHalf = std::min(targetW, targetH) > 0.0f
        ? 0.5f * std::min(targetW, targetH)
        : 0.02f * std::min(io.DisplaySize.x, io.DisplaySize.y);
    const float boxHalfW = targetW > 0.0f ? 0.5f * targetW : targetHalf;
    const float boxHalfH = targetH > 0.0f ? 0.5f * targetH : targetHalf;
    const bool  dzOn     = hasTarget && p.deadzone.value > 0.0f;
    const float dzXPx    = dzOn ? p.deadzone.value * boxHalfW : 0.0f;
    const float dzYPx    = dzOn ? p.deadzone.value * boxHalfH : 0.0f;

    // ── Per-class Y deadzone: stop band is v * box half-height ─────────────
    // v = 0 → band 0, Y chases the middle precisely (today's behaviour).
    // v = 1 → band = half the box height, i.e. Y holds still whenever the
    // crosshair is vertically inside the box. Missing entry = 0.0.
    float yFollowV = 0.0f;
    const auto yfIt = p.yFollow.find(bestCls);
    if (yfIt != p.yFollow.end()) yFollowV = std::clamp(yfIt->second.value, 0.0f, 1.0f);
    const float yDzPx = (hasTarget && yFollowV > 0.0f) ? yFollowV * boxHalfH : 0.0f;

    // ── Did the selection land on a DIFFERENT ENEMY this frame? ──────────────
    // The controller must be told, because it cannot tell a switch from the
    // target moving fast by looking at the error. It used to try, with a "the
    // error jumped more than 220 px" heuristic, and that heuristic was wrong in
    // both directions: it fired on any large legitimate swing (which is exactly
    // what a limit cycle looks like, so it wiped the integral that would have
    // damped the swing, twice per period) and it missed a switch between two
    // adjacent enemies at ~100 px apart, which is the common case.
    //
    // The track id replaced that heuristic — but the id ALONE is not enough, and
    // using it alone is one of the reasons this loop trails. An id change does
    // not always mean a different enemy. Two common situations produce a new id
    // on the SAME enemy:
    //   * a head/body model emits two boxes for one enemy — NMS is per class —
    //     so the engaged id alternates between the head track and the body track
    //     as the crosshair moves;
    //   * a track that had to be re-acquired comes back carrying a new number.
    // Treating those as switches clears the integrator, and doing that several
    // times a second means the trim can never build the steady-state velocity
    // that removes a trailing error. So the id change is CONFIRMED against
    // POSITION: a real enemy switch moves the aim point by a whole box or more,
    // an id flicker does not.
    //
    // Only a valid-to-valid transition is considered at all — going from -1 (no
    // target) to an id is a new engagement and the controller is already reset
    // by press(); going to -1 releases the finger. Both are handled elsewhere.
    const bool idChanged =
        (bestId >= 0) && (p.touchAim.lastTargetId >= 0) &&
        (bestId != p.touchAim.lastTargetId);
    constexpr float kSwitchDistOfBox = 0.75f;
    bool movedToAnotherEnemy = false;
    if (hasTarget && p.touchAim.lastTargetCx >= 0.0f) {
        const float ddx = target.x - p.touchAim.lastTargetCx;
        const float ddy = target.y - p.touchAim.lastTargetCy;
        const float yard = p.touchAim.lastTargetBox > 1.0f
            ? p.touchAim.lastTargetBox : 1.0f;
        const float lim = kSwitchDistOfBox * yard;
        movedToAnotherEnemy = (ddx * ddx + ddy * ddy) > lim * lim;
    }
    const bool targetChanged = idChanged && movedToAnotherEnemy;
    p.touchAim.lastTargetId = bestId;
    if (hasTarget) {
        p.touchAim.lastTargetCx  = target.x;
        p.touchAim.lastTargetCy  = target.y;
        p.touchAim.lastTargetBox = 0.5f * std::max(targetW, targetH);
    } else {
        p.touchAim.lastTargetCx = -1.0f;   // stale position must not be reused
    }

    // ── Choose injection mode for this frame ──────────────────────────────────
    //   Fusion ON:  aim wants to drive a REAL finger. If one is held inside the
    //               touch area, take over its mirror slot and MOVE it — no
    //               synthetic finger is pressed, so the screen keeps exactly one
    //               touch point and it is the player's own. The aim increment is
    //               added straight onto that finger's coordinates, plus the
    //               one-way assist (see driveAimToTarget). With no finger in the
    //               box it falls back to a synthetic finger so aim still works.
    //   Fusion OFF: pure synthetic finger. The player's finger is mirrored to the
    //               game untouched and the aim never touches it.
    //
    // There is only ever ONE writer per slot. A taken-over finger is excluded
    // from the mirror (uinput_takeover_physical_id), so nothing competes with aim
    // for its position — that is the entire reason the takeover exists.

    int  realId = -1;
    float realX = 0.0f, realY = 0.0f;
    const bool realFingerInArea = realFingerInTouchArea(realId, realX, realY);

    // Physical touch delta for the fusion assist. Taken from `prevRealId`, read
    // BEFORE the block below writes the new id back: comparing the freshly
    // written id against itself would make the "a different finger is now under
    // the crosshair" test permanently false, so a takeover would happily follow
    // whatever finger happened to be nearest instead of the one it took.
    const int prevRealId = p.touchAim.realId;
    float physDeltaX = 0.0f, physDeltaY = 0.0f;
    if (p.fusion.value && realFingerInArea) {
        // Only meaningful while the SAME finger was tracked last frame — a
        // first sighting, or a switch between two fingers, is a gap across
        // which "delta" would be the distance between them.
        if (prevRealId == realId && p.touchAim.lastPhysX >= 0.0f) {
            physDeltaX = realX - p.touchAim.lastPhysX;
            physDeltaY = realY - p.touchAim.lastPhysY;
        }
        p.touchAim.lastPhysX = realX;
        p.touchAim.lastPhysY = realY;
        p.touchAim.realId    = realId;
    } else {
        // No physical finger in the area — the delta is undefined next frame.
        p.touchAim.lastPhysX = p.touchAim.lastPhysY = -1.0f;
    }

    // 前馈增益 (kf) is pushed in with the gains — see setGains() below.
    //
    // What used to be here was a 速度前馈 slider feeding a SELF-CALIBRATING
    // feed-forward estimator inside PPID, and before that `bestVel * 1/alpha` —
    // the tracker's Kalman velocity times a gain the user had to calibrate by
    // guessing. Both are gone. The estimator turned out to be a bare integrator
    // in a delay loop (it oscillated: 1453 px of peak-to-peak crosshair swing at
    // alpha = 2.5), and the gate that was supposed to keep it quiet could not
    // work, because the tracker's velocity is the box's SCREEN velocity — the
    // aim's own output is in it. See the header of tracking/pid_controller.h
    // for both proofs and scripts/aim_alpha_fix.py for the measurements.
    //
    // `bestVelX/bestVelY` are still read above because 延迟补偿 (the lead) uses
    // them; that one is an explicit, optional bet on the target continuing.
    // Nothing in the controller consumes the velocity any more.

    if (p.touchAim.phase == TouchAimState::Phase::Idle) {
        // ── Idle: start aiming when a selected target is on screen ────────────
        if (!hasTarget) return;

        // Fusion ON + a finger inside the touch box → drive that finger.
        if (p.fusion.value && realFingerInArea) {
            const int slot = uinput_takeover_physical_id(realId);
            if (slot >= 0) {
                p.touchAim.position.x  = realX;
                p.touchAim.position.y  = realY;
                p.touchAim.start       = p.touchAim.position;
                p.touchAim.slot        = slot;
                p.touchAim.drivingReal = true;
                p.touchAim.realId      = realId;
                p.touchAim.lastPhysX   = realX;   // anchor the assist delta here
                p.touchAim.lastPhysY   = realY;
                p.touchAim.phase       = TouchAimState::Phase::Pressed;
                p.touchAim.aim.reset();
                LOGI("fusion: took over finger id=%d slot=%d at %d,%d",
                     realId, slot, static_cast<int>(realX), static_cast<int>(realY));
                // No assist on the takeover frame: the finger has not moved since
                // the anchor was taken, and passing a delta here would double-count
                // the very movement that put it inside the box.
                driveAimToTarget(p.touchAim, slot, target, rawTarget,
                                 screenCenter, io, dzXPx, dzYPx, yDzPx,
                                 0.0f, 0.0f, targetChanged, freshDetections);
                return;
            }
            // Not available this frame: the finger is not mirrored yet (mirror
            // race), or the gesture was swallowed by the menu. Fall through to a
            // synthetic finger so aiming does not stall — the next frame retries
            // the takeover.
        }

        // One-frame guard: if a fusion takeover was just released (real finger
        // lifted / left the box), skip spawning a synthetic finger this frame so
        // we don't get a phantom "extra touch point" the instant it leaves.
        if (p.touchAim.justReleasedTakeover) {
            p.touchAim.justReleasedTakeover = false;
            return;
        }
        // Synthetic auto-press (fusion OFF, or fusion ON without a finger in the
        // box).
        p.touchAim.press(p.touchArea);
        p.touchAim.slot        = UINPUT_SLOT_PRIMARY;
        p.touchAim.realId      = -1;
        p.touchAim.drivingReal = false;
        uinput_down(UINPUT_SLOT_PRIMARY, UINPUT_ID_PRIMARY,
                    static_cast<int>(p.touchAim.position.x),
                    static_cast<int>(p.touchAim.position.y));
        return;
    }

    // ── Pressed: stay locked on; release only on target-loss ──────────────────
    if (!hasTarget) {
        if (p.touchAim.drivingReal) {
            // Hand the finger back to the mirror; no uinput_up, so the game does
            // not see it flicker up and down.
            uinput_release_takeover();
            p.touchAim.justReleasedTakeover = true;
            LOGI("fusion: handed back finger id=%d (target lost)", prevRealId);
        } else {
            uinput_up(p.touchAim.slot);
        }
        p.touchAim.release();
        return;
    }

    if (p.touchAim.drivingReal) {
        // Driving a real finger. Stop the moment it lifts, leaves the touch box,
        // a different finger becomes the tracked one, or fusion is switched off —
        // in every case the slot goes back to the mirror first, so the "one
        // writer per slot" invariant never breaks.
        if (!p.fusion.value || !realFingerInArea || prevRealId != realId) {
            uinput_release_takeover();
            p.touchAim.justReleasedTakeover = true;
            LOGI("fusion: handed back finger id=%d (left box / fusion off)", prevRealId);
            p.touchAim.release();
            return;
        }
        driveAimToTarget(p.touchAim, p.touchAim.slot, target, rawTarget,
                         screenCenter, io, dzXPx, dzYPx, yDzPx,
                         physDeltaX, physDeltaY, targetChanged, freshDetections);
        return;
    }

    // Synthetic finger path (fusion OFF, or fusion ON with no finger in the box).
    driveAimToTarget(p.touchAim, p.touchAim.slot, target, rawTarget,
                     screenCenter, io, dzXPx, dzYPx, yDzPx,
                     physDeltaX, physDeltaY, targetChanged, freshDetections);

    // Drag safety (synthetic only): if the finger has travelled too far from its
    // press point, lift it. The next frame re-presses at the touch area, so the
    // on-panel delta is re-anchored instead of the finger sticking at the edge.
    //
    // `carryTrim` is what makes that re-anchor cheap. It is a move of the
    // finger's HOLDING POSITION, not a new engagement: same target, same
    // velocity, same game sensitivity, so the controller's trim is still correct
    // and press() must not clear it. Without this flag the integrator was wiped
    // every 400 px of travel — about twice a second against a 720 px/s strafe —
    // and a trim that is always being reset is indistinguishable from no trim at
    // all, which is precisely the "永远跟不上" behaviour.
    const float dxs = p.touchAim.position.x - p.touchAim.start.x;
    const float dys = p.touchAim.position.y - p.touchAim.start.y;
    if (dxs * dxs + dys * dys > p.touchAim.aim.maxDragDist * p.touchAim.aim.maxDragDist) {
        uinput_up(p.touchAim.slot);
        p.touchAim.release();
        // Must come AFTER release(), which clears the flag.
        p.touchAim.carryTrim = true;
    }
}

// ── Is the aim actually driving a finger right now? ─────────────────────────
//
// The render loop asks this to decide whether the overlay needs the full rate.
// The detector tier paces the loop at 60 fps, which was harmless while the aim
// was a proportional loop that could not track anyway — but it is the wrong rate
// for a 120 fps game with a 120 fps capture and a 2 ms inference:
//
//   * the finger is moved once per render frame, so at 720 px/s it advances in
//     12 px steps at 60 fps and 6 px steps at 120 fps. The 12 px staircase is
//     what "不丝滑" looks like.
//   * the loop's delay, and therefore the smallest usable gain and the largest
//     achievable bandwidth, is counted in FRAMES. Halving the frame period
//     halves the delay in milliseconds and doubles the phase margin, which is
//     what makes a higher Kp — i.e. a tighter lock — affordable.
//   * the tracker is fed from this same function, so at 60 fps it was being
//     shown every other detection and half the detector's output was discarded.
//
// The cost is the overlay's own redraw, and it is only paid while a target is
// actually being engaged: the moment the aim lets go, the tier falls back to
// whatever it was doing before.
bool aimIsDriving() {
    return g_pageAim.touchAim.phase == TouchAimState::Phase::Pressed;
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng

#endif  // HAS_IMGUI
