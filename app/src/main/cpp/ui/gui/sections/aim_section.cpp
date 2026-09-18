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
// `physDeltaX/Y`: physical touch delta in px from the last frame. When non-zero
// and the aim direction and physical direction align (dot > 0), the physical delta
// is ADDED to the aim output — so the player's fine adjustment is preserved.
// When directions conflict, only the aim output is used.
static void driveAimToTarget(TouchAimState& st, int slot,
                             const ImVec2& target, const ImVec2& screenCenter,
                             const ImGuiIO& io, float deadzonePx, float yDzPx,
                             float physDeltaX, float physDeltaY) {
    (void)io;  // dt kept for step() API compat; PPID is frame-rate-independent
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
    if (xInDz && (yInDz || yInYFollow)) return;

    float moveX = 0.0f, moveY = 0.0f;
    // PPID takes raw error (target - current) and returns displacement in px.
    st.aim.step(dex, dey, 0.0f, moveX, moveY);

    // Per-axis freeze: if an axis is inside its band, don't move it this frame.
    if (xInDz)          moveX = 0.0f;
    if (yInDz || yInYFollow) moveY = 0.0f;

    // Fusion: add physical touch delta when aim direction and physical direction align.
    const float physLen2 = physDeltaX * physDeltaX + physDeltaY * physDeltaY;
    if (physLen2 > 0.25f) {  // > 0.5 px threshold to ignore jitter
        const float aimLen2 = moveX * moveX + moveY * moveY;
        if (aimLen2 > 0.25f) {
            // Both aim and physical have non-trivial output — check direction alignment.
            const float aimLen   = std::sqrt(aimLen2);
            const float physLen  = std::sqrt(physLen2);
            const float dot = (moveX * physDeltaX + moveY * physDeltaY) / (aimLen * physLen);
            if (dot > 0.0f) {
                // Same direction: blend physical delta fully into aim output.
                moveX += physDeltaX;
                moveY += physDeltaY;
            }
            // dot <= 0: opposing or perpendicular — use pure aim, do nothing.
        } else {
            // Aim output near zero — use pure physical delta (aim converged, user nudges).
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
    aim.reset();
    justReleasedTakeover = false;
}

void TouchAimState::release() {
    phase = Phase::Idle;
    drivingReal = false;
    realId = -1;
    physX = physY = -1.0f;
    lastPhysX = lastPhysY = -1.0f;
    // NOTE: justReleasedTakeover is intentionally left as-is here — the takeover
    // release paths set it true just before calling release(), and the Idle
    // branch clears it after reading. Resetting it here would defeat the guard.
}

// ── drawAimSection ──────────────────────────────────────────────────────────

void drawAimSection(ImDrawList* dl, float x, float& y, float w,
                    float /*bottomY*/, float s, float es, const Xf& xf,
                    Scroll& sc) {
    const float gap   = 12.0f * s;
    const float rowSw = widgets::kSwitchRowH   * s;
    const float rowSl = widgets::kSliderRowH   * s;
    const float rowDd = widgets::kDropdownRowH * s;

    // Each row's rect is built at the scrolled screen y so paint coordinates
    // and hit-test rectangles stay in lock-step. `y` keeps advancing in natural
    // (un-scrolled) coordinates, which is what lets drawContent compute the
    // page's total content height for the scroll-range clamp.
    auto wRect = [&](float wx, float wy, float ww, float wh) {
        return wRectOf(wx, wy - sc.offset, ww, wh, s, xf);
    };

    // ── PPID controller parameters ─────────────────────────────────────────
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.kp,       "Kp",       1, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.kd,       "Kd",       1, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.predictX,  "预测X",    1, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.predictY,  "预测Y",    1, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.rate,     "自适应",   2, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageAim.smooth,   "平滑",     0, es);
    y += rowSl + gap;

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
    // When on, aim always uses a synthetic finger. If a physical finger is held
    // inside the touch area, its per-frame delta is blended with the aim output
    // when both directions align (dot > 0); pure aim is used when directions conflict.
    // The physical finger mirrors normally to the game. When off, aim takes over
    // the physical finger exclusively (old behaviour: no synthetic press).
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
    if (snap.active) {
        tracking::tracker().update(snap.detections);
    } else {
        tracking::tracker().reset();
    }

    // ── Copy slider values into the PPID controller on every press ───────────
    // init() is idempotent so calling every frame is safe; it only resets the
    // PID state when the aim goes Idle (aim.reset() in press()).  Slider tweaks
    // take effect on the very next step because PPID reads kp/kd/predict/rate/smooth
    // inside update(), not at init time.
    p.touchAim.aim.init(
        p.kp.value, p.kd.value,
        p.predictX.value, p.predictY.value,
        p.rate.value, p.smooth.value);

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
    float bestVelX = 0.0f, bestVelY = 0.0f;
    int   bestCls = -1;
    bool hasTarget = false;
    float bestDistSq = 1e18f;
    // Pick the confirmed track whose (smoothed) centre is nearest the crosshair.
    // Tracks come from the Kalman tracker, so the centre is jitter-smoothed and
    // stable frame-to-frame. Lead (delay-compensation) is NOT applied to the
    // target itself here — it goes into the controller's F term via
    // aim.targetVelX/Y + aim.delayFrames, which keeps the PID error honest and
    // avoids the "lead inside the error" double-counting that pulled the aim
    // past the target.
    const auto& tracks = tracking::tracker().tracks();
    if (p.aimCategory.hasAnySelected()) {
        for (const auto& t : tracks) {
            if (!t.confirmed) continue;
            if (!p.aimCategory.isClassSelected(t.cls)) continue;
            const float dx = t.cx - screenCenter.x;
            const float dy = t.cy - screenCenter.y;
            const float dSq = dx * dx + dy * dy;
            if (dSq < bestDistSq) {
                bestDistSq = dSq;
                target     = ImVec2(t.cx, t.cy);
                bestVelX   = t.vx;
                bestVelY   = t.vy;
                targetW    = t.w;
                targetH    = t.h;
                bestCls    = t.cls;
                hasTarget  = true;
            }
        }
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

    // ── Choose injection mode for this frame ──────────────────────────────────
    //   Fusion ON:  aim always drives its own synthetic finger.
    //               If a real finger is in the touch area, read its delta each frame
    //               and BLEND it with the aim output (directions aligned → add delta;
    //               opposing → pure aim). The physical finger mirrors normally.
    //   Fusion OFF: old behaviour — takeover the real finger if present (aim
    //               exclusively drives it, no synthetic press); otherwise synthetic.
    //
    // There is only ever ONE active touch point: a real finger being driven is
    // removed from the mirror (uinput_takeover_physical_id), so no second writer
    // competes on its slot. With fusion ON the real finger stays in the mirror.

    int  realId = -1;
    float realX = 0.0f, realY = 0.0f;
    const bool realFingerInArea = realFingerInTouchArea(realId, realX, realY);

    // Physical touch delta for this frame (fusion path).
    float physDeltaX = 0.0f, physDeltaY = 0.0f;
    if (p.fusion.value && realFingerInArea) {
        // Same physical finger as last frame — compute delta.
        if (p.touchAim.realId == realId && p.touchAim.lastPhysX >= 0.0f) {
            physDeltaX = realX - p.touchAim.lastPhysX;
            physDeltaY = realY - p.touchAim.lastPhysY;
        }
        p.touchAim.lastPhysX = realX;
        p.touchAim.lastPhysY = realY;
        p.touchAim.realId    = realId;
    } else {
        // No physical finger in area — reset tracking.
        p.touchAim.lastPhysX = p.touchAim.lastPhysY = -1.0f;
    }

    if (p.touchAim.phase == TouchAimState::Phase::Idle) {
        // ── Idle: start aiming when a selected target is on screen ────────────
        if (!hasTarget) return;
        if (!p.fusion.value && realFingerInArea) {
            // Fusion OFF + real finger present → takeover mode (old behaviour).
            const int slot = uinput_takeover_physical_id(realId);
            if (slot >= 0) {
                p.touchAim.position.x = realX;
                p.touchAim.position.y = realY;
                p.touchAim.start      = p.touchAim.position;
                p.touchAim.slot       = slot;
                p.touchAim.drivingReal = true;
                p.touchAim.phase  = TouchAimState::Phase::Pressed;
                p.touchAim.aim.reset();
                driveAimToTarget(p.touchAim, slot, target, screenCenter, io,
                                 deadzonePx, yDzPx, 0.0f, 0.0f);
                return;
            }
            // Takeover not ready yet (mirror race). Retry next frame.
        }
        // One-frame guard: if a fusion takeover was just released (real finger
        // lifted), skip spawning a synthetic finger this frame so we don't get a
        // phantom "extra touch point".
        if (p.touchAim.justReleasedTakeover) {
            p.touchAim.justReleasedTakeover = false;
            return;
        }
        // Synthetic auto-press. Fusion ON: always synthetic (aim + physical blend).
        // Fusion OFF: synthetic when no real finger present.
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
            // Takeover release (fusion OFF path).
            uinput_release_takeover();
            p.touchAim.justReleasedTakeover = true;
        } else {
            uinput_up(p.touchAim.slot);
        }
        p.touchAim.release();
        return;
    }

    if (p.touchAim.drivingReal) {
        // Fusion OFF: takeover mode — re-takeover if finger changed or left.
        if (!realFingerInArea || p.touchAim.realId != realId) {
            uinput_release_takeover();
            p.touchAim.justReleasedTakeover = true;
            p.touchAim.drivingReal = false;
            p.touchAim.realId      = -1;
            p.touchAim.phase = TouchAimState::Phase::Idle;
            return;
        }
        driveAimToTarget(p.touchAim, p.touchAim.slot, target, screenCenter, io,
                         deadzonePx, yDzPx, 0.0f, 0.0f);
        return;
    }

    // Fusion ON path: synthetic finger + physical delta blend.
    // Fusion OFF path (no real finger): pure synthetic.
    driveAimToTarget(p.touchAim, p.touchAim.slot, target, screenCenter, io,
                     deadzonePx, yDzPx, physDeltaX, physDeltaY);

    // Drag safety (synthetic only): if the finger has travelled too far from its
    // press point, lift it. The next frame re-presses at the touch area, so the
    // on-panel delta is re-anchored instead of the finger sticking at the edge.
    const float dxs = p.touchAim.position.x - p.touchAim.start.x;
    const float dys = p.touchAim.position.y - p.touchAim.start.y;
    if (dxs * dxs + dys * dys > p.touchAim.aim.maxDragDist * p.touchAim.aim.maxDragDist) {
        uinput_up(p.touchAim.slot);
        p.touchAim.release();
    }
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng

#endif  // HAS_IMGUI
