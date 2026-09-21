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
// `deadzonePx` is the global PER-AXIS stop radius: X freezes when |target.x -
// centre.x| <= deadzonePx and Y freezes when |target.y - centre.y| <= deadzonePx
// (decoupled, not a single radial circle — see the deadzone note below). `yDzPx`
// is the per-class Y-only stop band, a wider/narrower band than the global one:
// once the crosshair is vertically within `yDzPx` of the target centre, Y freezes
// and X keeps tracking on its own.
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
                             const ImVec2& target, const ImVec2& screenCenter,
                             const ImGuiIO& io, float deadzonePx, float yDzPx,
                             float physDeltaX, float physDeltaY,
                             bool targetChanged) {
    // The control step, in seconds. This used to be discarded ((void)io) on the
    // grounds that the controller was "frame-rate independent" — it was not, it
    // was frame-rate DEPENDENT: the loop runs at 120 Hz while the panel is up or
    // a finger is down and at 60 Hz in the detector tier, so every dt-scaled
    // term silently retuned itself by 2x whenever that switch happened. The
    // integral and the derivative both need it; the proportional term must not
    // have it (its stability bound is a per-sample one).
    const float dt = std::clamp(io.DeltaTime, 1.0f / 2000.0f, 0.2f);
    // Deadzone — decoupled per-axis stop radius.  A single radial circle was the
    // old code's bug: a target oscillating along X keeps popping OUTSIDE the
    // circle even when Y is at rest, dragging X onto the target centre.  Per-axis
    // freezing holds whichever axis is already inside its band.  The reference is
    // the SCREEN CENTRE (the crosshair), not the live finger.
    const float dex = target.x - screenCenter.x;
    const float dey = target.y - screenCenter.y;
    const bool xInDz  = deadzonePx > 0.0f && dex * dex <= deadzonePx * deadzonePx;
    const bool yInDz  = deadzonePx > 0.0f && dey * dey <= deadzonePx * deadzonePx;
    const bool yInYFollow = yDzPx > 0.0f && dey * dey <= yDzPx * yDzPx;

    float pidX = 0.0f, pidY = 0.0f;
    // Controller takes the raw error (target - crosshair) and returns the finger
    // displacement for this step, in px. The two deadzone flags only HOLD the
    // integrator of a frozen axis; the raw error still reaches the proportional
    // and derivative terms so the derivative's state stays continuous (otherwise
    // it spikes every time the target crosses the stop band). The output itself
    // is still zeroed below, so the visible behaviour is unchanged — what changes
    // is that the trim no longer accumulates behind a discarded output.
    //
    // `targetChanged` is passed straight through: the selection is "nearest
    // confirmed track", so it can land on a different enemy in one step, and
    // the controller has to be told rather than left to guess from the size of
    // the error jump. See TouchAimState::lastTargetId.
    st.aim.step(dex, dey, dt, xInDz, (yInDz || yInYFollow), targetChanged,
                pidX, pidY);

    // ── Aim telemetry ──────────────────────────────────────────────────────
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
    //   tot     : the controller's total output this step, in finger px. On a
    //             strafing target this should settle at about ΔT/alpha, i.e.
    //             (v/120)·(1/alpha): 5 for 600 px/s at alpha = 1.0, 50 at
    //             alpha = 0.1. A `tot` much smaller than that IS the "跟不上"
    //             failure. Two different causes, and `ff`/`trim`/`lim` tell them
    //             apart: `ff` near 0 with `trim` pinned means the feed-forward is
    //             not running (raise 前馈增益); `tot` near `lim` means the finger
    //             cannot travel further in one step — the game's sensitivity is
    //             below what this ceiling can serve.
    //   ff      : the velocity feed-forward's contribution, in px. THE number to
    //             watch on a moving target — it should settle at about ΔT/alpha
    //             and HOLD there. It is the term that removes the lag without
    //             winding up, so if the aim still trails while `ff` sits near
    //             zero, that is the whole problem and nothing about kp will fix
    //             it: raise 前馈增益.
    //   d       : the damping term's contribution, in px. Pinned near 0 with
    //             kd > 0 means the damping term is dead; comparable to the
    //             feedback means kd is carrying the loop and kp has room.
    //   trim    : the integrator — now the RESIDUAL cleaner, not the DC carrier
    //             (the feed-forward does that). It should therefore stay SMALL.
    //             A large `trim` together with a small `ff` on a moving target
    //             means kf is wrong, by roughly the ratio between them.
    //             Pinned at its ceiling means the requirement exceeds what the
    //             loop may deliver even with the feed-forward: the aim is at its
    //             physical limit. See kTrimLimitPx.
    //   kf / lim
    //           : 前馈增益 in force, and the output ceiling it works against
    //             (finger px/step, a constant 180). Read them together with `ff`:
    //             a strafing target whose `ff` stays near 0 while `mean e` holds
    //             one sign is the whole "跟不上" failure, and the answer is a
    //             larger kf, not a larger kp. `yard` is the target's size (half
    //             its longer side, the "how far away is it" yardstick the
    //             selection pass already keeps) — the same px/s is a different
    //             physical motion on a distant box and one at contact range, so
    //             "it keeps up" is only meaningful at a stated box size.
    //   id      : the track being engaged. A value that changes several times a
    //             second is a selection/identity problem, not a control problem,
    //             and no gain will fix it — read the tracker line's id list.
    {
        static int  sTick = 0;
        static float sSumX = 0.0f, sSumY = 0.0f;
        sSumX += dex; sSumY += dey;
        if (++sTick >= 480) {
            LOGI("aim: id=%d mean e=(%.1f,%.1f) tot=(%.2f,%.2f) "
                 "ff=(%.1f,%.1f) d=(%.2f,%.2f) trim=(%.1f,%.1f) "
                 "kf=%.2f lim=%.0f yard=%.0f dt=%.1fms",
                 st.lastTargetId,
                 sSumX / sTick, sSumY / sTick, pidX, pidY,
                 st.aim.ffX(), st.aim.ffY(),
                 st.aim.derivX(), st.aim.derivY(),
                 st.aim.trimX(), st.aim.trimY(),
                 st.aim.ffGainValue(), st.aim.outLimitPx(),
                 st.lastTargetBox, dt * 1000.0f);
            sTick = 0; sSumX = 0.0f; sSumY = 0.0f;
        }
    }

    // The per-axis freeze has ALREADY been applied inside the controller: it
    // suppressed the proportional and derivative terms and held the integrator,
    // while still adding what the loop has already learned. So the caller must
    // NOT zero the axis here — doing that would leave whatever the loop had
    // trimmed behind, and bring back the "target leaves the band, the crosshair
    // snaps after it and overshoots" limit cycle around the deadzone edge — the
    // very thing the per-axis freeze was introduced for. See
    // AimController::step() and PPID::update()'s `frozen` note.
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
    // All three gains are per control step now and share one unit system — see
    // the class note in tracking/pid_controller.h for what each one does and the
    // measured grid behind its default.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.kp,        "Kp",       2, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.ki,        "Ki",       1, es);
    y += rowSl + gap;
    // Kd is PER STEP and dimensionless now (it used to be per-second, which made
    // its real weight kd*120 and put the useful part of the slider in its first
    // few pixels). Two decimals match the 0.05 step.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.kd,        "Kd",       2, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.outSmooth, "输出平滑", 2, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.aimDelayFrames,        "延迟补偿",  2, es);
    y += rowSl + gap;
    // The feed-forward gain, and the only row in this page that is a
    // CALIBRATION rather than a taste: kf = 1/alpha, the reciprocal of the game's
    // sensitivity in view px per finger px.
    //
    // Its row was 速度前馈 once, feeding an estimator that was structurally a
    // bare integrator (it measured its own output with no restoring term, so it
    // oscillated — 1453 px peak-to-peak at alpha = 2.5), and it was briefly two
    // rows (this one plus 灵敏度补偿). Both are gone. The single knob now carries
    // the whole plant gain because the reconstruction uses its own reciprocal as
    // the assumed 1/alpha, so the same number appears twice and the residual
    // self-term is exactly (1 − kf·alpha).
    //
    // Two things about tuning it that the measurements insist on:
    //   * the criterion is the SIGN of the error on a steadily strafing target —
    //     trailing means raise, leading means lower;
    //   * the band near kf ~ 1/alpha·0.2 is WORSE than 0, so a small non-zero kf
    //     is not a safe starting point. Jump to 3 or 4 rather than creeping.
    // scripts/aim_pidf_bench.py 表14 is the map.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.ffGain,              "前馈增益",  2, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.trackPredictHoldFrames,"预测帧数",  0, es);
    y += rowSl + gap;
    // 输出限幅 used to be a row here, and it never was a tuning parameter: it is
    // the ceiling that stops a re-lock from flinging the finger across the panel.
    // It is the constant tracking::kOutLimitPx (180 finger px/step, tanh), with a
    // separate, smaller leash on the integral (kTrimLimitPx, 90). Neither is
    // scaled by anything — the plant gain enters the loop through 前馈增益 alone.

    // ── Aim deadzone ────────────────────────────────────────────────────────
    // 0.0 = move onto the target centre; 1.0 = stop at the target edge. The
    // stop radius is a fraction of the target box's half-size, so 0.1 still
    // lands on the target and 1.0 stops at its nearest edge.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.deadzone, "死区", 1, es);
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

    // Publish the predictHoldFrames slider (Aim page) into the global
    // tracker config. syncSettingsPage() publishes trackIou/Confirm/Terminate;
    // this is the Aim-page half of the tracker tuning. Cheap scalar write.
    tracking::trackerConfig().predictHoldFrames =
        static_cast<int>(p.trackPredictHoldFrames.value + 0.5f);

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
    if (snap.active) {
        const bool freshDetections = (snap.detSeq != aimLastDetSeq);
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
    // cause note at the top of tracking/pid_controller.h. Applying gains and
    // clearing state are now separate operations, and only a new engagement (or
    // a re-anchor-free press) calls reset().
    //
    // Four gains and the output EMA: that is the whole parameter set. There is no
    // sensitivity compensation argument any more and nothing is scaled — the
    // fifth argument is 前馈增益, which is both the feed-forward gain and the
    // reciprocal of the reconstruction constant, so it carries the plant gain
    // inside itself (kf = 1/alpha). The output ceiling and the derivative's
    // filter constant are loop constants: tracking::kOutLimitPx / kTrimLimitPx /
    // kDerivTauSec. See AimController::setGains().
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
    if (hasTarget && p.aimDelayFrames.value > 0.0f) {
        target.x += bestVelX * p.aimDelayFrames.value;
        target.y += bestVelY * p.aimDelayFrames.value;
    }

    // ── Aim deadzone: 0.0 = move onto the target CENTRE; 1.0 = stop at the
    // target EDGE. The stop radius is a fraction of the target box's half-size
    // (min(w,h)/2), so it is resolution- and distance-independent: 0.1 still
    // nudges well onto the target, 1.0 stops as soon as the target's nearest
    // edge reaches the crosshair. 0.0 = converge to the exact centre.
    const float targetHalf = std::min(targetW, targetH) > 0.0f
        ? 0.5f * std::min(targetW, targetH)
        : 0.02f * std::min(io.DisplaySize.x, io.DisplaySize.y);
    const float deadzonePx = (hasTarget && p.deadzone.value > 0.0f)
        ? p.deadzone.value * targetHalf
        : 0.0f;

    // ── Per-class Y deadzone: stop band is v * box half-height ─────────────
    // v = 0 → band 0, Y chases the middle precisely (today's behaviour).
    // v = 1 → band = half the box height, i.e. Y holds still whenever the
    // crosshair is vertically inside the box. Missing entry = 0.0.
    float yFollowV = 0.0f;
    const auto yfIt = p.yFollow.find(bestCls);
    if (yfIt != p.yFollow.end()) yFollowV = std::clamp(yfIt->second.value, 0.0f, 1.0f);
    const float yDzPx = (hasTarget && yFollowV > 0.0f)
        ? yFollowV * (targetH > 0.0f ? targetH * 0.5f : targetHalf)
        : 0.0f;

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
                driveAimToTarget(p.touchAim, slot, target, screenCenter, io,
                                 deadzonePx, yDzPx, 0.0f, 0.0f,
                                 targetChanged);
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
        driveAimToTarget(p.touchAim, p.touchAim.slot, target, screenCenter, io,
                         deadzonePx, yDzPx, physDeltaX, physDeltaY,
                         targetChanged);
        return;
    }

    // Synthetic finger path (fusion OFF, or fusion ON with no finger in the box).
    driveAimToTarget(p.touchAim, p.touchAim.slot, target, screenCenter, io,
                     deadzonePx, yDzPx, physDeltaX, physDeltaY,
                     targetChanged);

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
