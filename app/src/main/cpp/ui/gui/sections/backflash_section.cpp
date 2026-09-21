// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::backflash — implementation. See backflash_section.h
//  for the touch lifecycle, the blockingAim contract, and the page layout.
// ─────────────────────────────────────────────────────────────────────────────
#include "backflash_section.h"

#include "ui/gui/hud.h"
#include "ui/gui/theme.h"

#ifdef HAS_IMGUI

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "imgui.h"

#include "inference/model_runtime.h"
#include "inference/model_store.h"
#include "input/inject_backend.h"
#include "input/touch_reader.h"
#include "input/uinput_inject.h"
#include "tracking/kalman_tracker.h"

namespace aimbotng {
namespace ui {
namespace sections {

PageBackFlash g_pageBackFlash;

namespace {

using namespace theme;

// ── Overlay geometry constants ──────────────────────────────────────────────
//
// Same set as the Aim page so the user's mental model ("drag a dashed box,
// resize from edges") is shared across both overlays. A future consolidation
// could pull these into a shared header; today two copies is cheaper than a
// new shared type the next reader has to learn.
constexpr float kTouchEdgeGrip   = 24.0f;
constexpr float kTouchCornerGrip = 36.0f;
constexpr float kTouchCornerDraw = 18.0f;
constexpr float kTouchMinSize    = 200.0f;
constexpr float kTouchDashLen    = 16.0f;
constexpr float kTouchDashGap    = 8.0f;
constexpr float kTouchThickness  = 2.5f;
constexpr float kOverlayLabelSize = 38.0f;

constexpr ImU32 kOverlayBorder = IM_COL32(238, 240, 246, 215);
constexpr ImU32 kOverlayHandle = IM_COL32(255, 255, 255, 235);
constexpr ImU32 kOverlayFill   = IM_COL32( 61, 123, 255,  28);
constexpr ImU32 kOverlayLabel  = IM_COL32(238, 240, 246, 235);

// Per-axis END jitter (±kEndJitterPx). Applied to the swipe END and the
// reset END — once each, at the start of the sub-action. Not applied to
// every frame's move (the per-frame Bezier step is what shapes the path).
constexpr float kEndJitterPx = 4.0f;

// ── Helpers ────────────────────────────────────────────────────────────────

inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline widgets::Rect wRectOf(float wx, float wy, float ww, float wh,
                             float s, const Xf& xf) {
    const ImVec2 p = xf.pt(wx, wy);
    return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
}

/// Uniform in [0, 1) — used to pick the press point inside the touch area.
inline float randPositive(PageBackFlash& p) {
    p.rng = p.rng * 1103515245u + 12345u;
    return static_cast<float>((p.rng >> 8) & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

/// Uniform in [-1, 1) — used for the ±kEndJitterPx END offsets (scaled by
/// the caller). Same cheap LCG; not crypto, just needs to not repeat every
/// cycle.
inline float randSigned(PageBackFlash& p) {
    p.rng = p.rng * 1103515245u + 12345u;
    return static_cast<float>((p.rng >> 8) & 0xFFFFFFu) * (1.0f / 16777216.0f) * 2.0f - 1.0f;
}

/**
 * 4th-order (quartic) Bezier curve, 5 control points.
 *   B(t) = (1-t)^4 P0 + 4(1-t)^3 t P1 + 6(1-t)^2 t^2 P2 + 4(1-t) t^3 P3 + t^4 P4
 *
 * Control points (0, 0.25, 0.75, 1, 1) yield an ease-in-out: B'(0) = 1
 * (smooth at the press — no jerk on the way out), B'(1) = 0 (lands at
 * the END with zero velocity — no overshoot). The peak velocity sits
 * around t ≈ 0.5 and is bounded by 1 (in normalized units), so the per-
 * frame clamp below is the safety net, not the regulator.
 */
inline float bezier4(float t,
                     float p0, float p1, float p2, float p3, float p4) {
    const float u = 1.0f - t;
    return u*u*u*u*p0 + 4.0f*u*u*u*t*p1 +
           6.0f*u*u*t*t*p2 + 4.0f*u*t*t*t*p3 + t*t*t*t*p4;
}

inline void drawDashedRect(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
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
    dashEdge(ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y));
    dashEdge(ImVec2(mx.x, mn.y), ImVec2(mx.x, mx.y));
    dashEdge(ImVec2(mx.x, mx.y), ImVec2(mn.x, mx.y));
    dashEdge(ImVec2(mn.x, mx.y), ImVec2(mn.x, mn.y));
}

inline void drawCornerHandles(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
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

inline void drawCentredLabel(ImDrawList* dl, const ImVec2& center, float size,
                             ImU32 col, const char* text) {
    ImFont* font = ImGui::GetFont();
    size = tsize(size);  // readability boost — measure and draw on the same size
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size,
                ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                col, text);
}

// ── Touch-area placement ───────────────────────────────────────────────────
//
// Geometry exists regardless of the toggle state (toggle only controls
// drawing). Centred by default so the first frame after enabling already
// has a usable area.
void ensureBackFlashAreasPlaced(float screenW, float screenH) {
    PageBackFlash& p = g_pageBackFlash;
    if (!p.touchArea.placed) {
        p.touchArea.x = (screenW - p.touchArea.w) * 0.5f;
        p.touchArea.y = (screenH - p.touchArea.h) * 0.5f;
        p.touchArea.placed = true;
    }
}

// ── Touch-box hit testing & drag ────────────────────────────────────────────

BackFlashTouchArea::Drag hitTestBackFlash(const BackFlashTouchArea& area, ImVec2 mouse) {
    using Drag = BackFlashTouchArea::Drag;
    const float l = area.x, t = area.y;
    const float r = area.x + area.w, b = area.y + area.h;

    if (mouse.x >= l - kTouchCornerGrip && mouse.x <= l + kTouchCornerGrip &&
        mouse.y >= t - kTouchCornerGrip && mouse.y <= t + kTouchCornerGrip) return Drag::CornerTL;
    if (mouse.x >= r - kTouchCornerGrip && mouse.x <= r + kTouchCornerGrip &&
        mouse.y >= t - kTouchCornerGrip && mouse.y <= t + kTouchCornerGrip) return Drag::CornerTR;
    if (mouse.x >= l - kTouchCornerGrip && mouse.x <= l + kTouchCornerGrip &&
        mouse.y >= b - kTouchCornerGrip && mouse.y <= b + kTouchCornerGrip) return Drag::CornerBL;
    if (mouse.x >= r - kTouchCornerGrip && mouse.x <= r + kTouchCornerGrip &&
        mouse.y >= b - kTouchCornerGrip && mouse.y <= b + kTouchCornerGrip) return Drag::CornerBR;

    if (mouse.y >= t + kTouchCornerGrip && mouse.y <= b - kTouchCornerGrip) {
        if (mouse.x >= l - kTouchEdgeGrip && mouse.x <= l + kTouchEdgeGrip) return Drag::EdgeL;
        if (mouse.x >= r - kTouchEdgeGrip && mouse.x <= r + kTouchEdgeGrip) return Drag::EdgeR;
    }
    if (mouse.x >= l + kTouchCornerGrip && mouse.x <= r - kTouchCornerGrip) {
        if (mouse.y >= t - kTouchEdgeGrip && mouse.y <= t + kTouchEdgeGrip) return Drag::EdgeT;
        if (mouse.y >= b - kTouchEdgeGrip && mouse.y <= b + kTouchEdgeGrip) return Drag::EdgeB;
    }

    if (mouse.x >= l + kTouchEdgeGrip && mouse.x <= r - kTouchEdgeGrip &&
        mouse.y >= t + kTouchEdgeGrip && mouse.y <= b - kTouchEdgeGrip) return Drag::Move;

    return Drag::None;
}

void driveBackFlashTouch(BackFlashTouchArea& area, ImVec2 mouse,
                         float screenW, float screenH) {
    using Drag = BackFlashTouchArea::Drag;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (area.drag == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Drag hit = hitTestBackFlash(area, mouse);
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

    if (area.x < 0.0f) area.x = 0.0f;
    if (area.y < 0.0f) area.y = 0.0f;
    if (area.x + area.w > screenW) area.x = screenW - area.w;
    if (area.y + area.h > screenH) area.y = screenH - area.h;
}

// ── State-machine helpers ──────────────────────────────────────────────────

// Lifts any in-flight finger. Called from every early-out path so a gate
// closing mid-cycle never leaves a phantom finger pressed on PRIMARY.
void cancelFinger() {
    PageBackFlash& p = g_pageBackFlash;
    if (p.fingerDown) {
        uinput_up(UINPUT_SLOT_PRIMARY);
        p.fingerDown = false;
    }
}

// Picks a random press point INSIDE the touch area (uniform on the rectangle).
void pickPressPoint(PageBackFlash& p, float& outX, float& outY) {
    const BackFlashTouchArea& a = p.touchArea;
    outX = a.x + randPositive(p) * a.w;
    outY = a.y + randPositive(p) * a.h;
}

// Direction the swipe goes, derived from the flash target vs. screen centre.
//
//   flash.cx < screen.cx → swipe RIGHT  (+1 in x) — turning away to the right
//   flash.cx > screen.cx → swipe LEFT   (-1 in x) — turning away to the left
//
// Y stays at press.y (the synthetic finger slides horizontally, matching
// how a real player turns their view: a horizontal swipe on the edge of
// the screen rotates the camera around the vertical axis).
int swipeDirection(float flashCx, float screenCx) {
    return (flashCx < screenCx) ? +1 : -1;
}

/**
 * Advances the current sub-action one frame. Both Swiping and Resetting
 * share this step — the only difference between them is which (start, end)
 * pair is in `p.curStart*` / `p.curEnd*`.
 *
 * The Bezier progress curve maps 0..1 onto the segment; the per-frame
 * clamp (`maxPerFramePx`) is a safety on the rare frame the curve's
 * local slope would ask for more than the cap. Termination is "close
 * enough to the END" (|last - end| < 1 px) OR a hard 2× subTotalFrames
 * ceiling — the latter is the guard against a clamp that prevents the
 * curve from ever landing (shouldn't happen with the chosen control
 * points, but cheap to bound).
 */
void advanceSubFrame(PageBackFlash& p) {
    ++p.subFrame;
    const float tF = static_cast<float>(p.subFrame) /
                     static_cast<float>(p.subTotalFrames);
    const float t = (tF > 1.0f) ? 1.0f : tF;
    const float u = bezier4(t, 0.0f, 0.25f, 0.75f, 1.0f, 1.0f);

    float nx = p.curStartX + u * (p.curEndX - p.curStartX);
    float ny = p.curStartY + u * (p.curEndY - p.curStartY);

    const float cap = std::max(1.0f, p.maxPerFramePx.value);
    const float dx = nx - p.lastX, dy = ny - p.lastY;
    const float d  = std::sqrt(dx * dx + dy * dy);
    if (d > cap && d > 0.0f) {
        const float s = cap / d;
        nx = p.lastX + dx * s;
        ny = p.lastY + dy * s;
    }
    p.lastX = nx;
    p.lastY = ny;
    uinput_move(UINPUT_SLOT_PRIMARY,
                static_cast<int>(nx), static_cast<int>(ny));
}

// True when the synthetic finger has effectively reached the END of the
// current sub-action. Two termination criteria, see advanceSubFrame.
bool subActionDone(const PageBackFlash& p) {
    const float dx = p.curEndX - p.lastX, dy = p.curEndY - p.lastY;
    return (dx*dx + dy*dy < 1.0f) ||
           (p.subFrame >= p.subTotalFrames * 2);
}

// Compute the (start, end, totalFrames) for one sub-action. The endpoints
// are set by the caller; this just derives the frame budget.
void armSubAction(PageBackFlash& p, float totalDistance) {
    const float cap = std::max(1.0f, p.maxPerFramePx.value);
    p.subTotalFrames = std::max(2,
        static_cast<int>(std::ceil(std::max(1.0f, totalDistance) / cap)));
    p.subFrame = 0;
}

}  // namespace

// ── BackFlashCategoryState ──────────────────────────────────────────────────

void BackFlashCategoryState::syncFromModel(
    const std::vector<std::string>& modelClasses) {
    // Always rebuild. Same reasoning as the Aim page's filter: a same-size
    // list could be a stale cache, and empty-vs-empty is exactly the case
    // where the widget needs to reset to "(无)".
    items = modelClasses;
    if (items.size() > 32) items.resize(32);
    itemPtrs.clear();
    itemPtrs.reserve(items.size());
    for (const auto& s : items) itemPtrs.push_back(s.c_str());

    if (!synced && !items.empty()) {
        uint32_t mask = 0;
        for (size_t i = 0; i < items.size(); ++i) mask |= (1u << i);
        sel.mask = mask;
        synced = true;
    }
}

// ── drawBackFlashSection ────────────────────────────────────────────────────

void drawBackFlashSection(ImDrawList* dl, float x, float& y, float w,
                          float /*bottomY*/, float s, float es, const Xf& xf,
                          Scroll& sc) {
    const float gap   = csize(12.0f) * s;
    const float rowSw = csize(widgets::kSwitchRowH)   * s;
    const float rowSl = csize(widgets::kSliderRowH)   * s;
    const float rowDd = csize(widgets::kDropdownRowH) * s;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        // Apply the page's scroll offset so paint and hit-test agree. `y`
        // keeps advancing in natural coordinates, which is what lets the
        // caller compute `sc.maxOffset` from the final `*y` value.
        const ImVec2 p = xf.pt(wx, wy - sc.offset);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    // ── Category list off the model (same fallback as the Aim page) ───────
    // Cheap — `snapshot()` copies a small struct and a short vector. The
    // widget reads `itemPtrs` later in this frame, so the sync happens
    // BEFORE the rows below it are drawn.
    const infer::runtime::Snapshot snap = infer::runtime::snapshot();
    std::vector<std::string> classLabels = snap.classes;
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
    g_pageBackFlash.category.syncFromModel(classLabels);

    // ── Row order (see header): timing → tuning → overlay → filter ──────────

    // 1. 背闪延迟 (ms)
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.flashDelay,
                         "背闪延迟", 0, es);
    y += rowSl + gap;

    // 2. 复位延迟 (ms)
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.resetDelay,
                         "复位延迟", 0, es);
    y += rowSl + gap;

    // 3. 滑屏距离(px) — total px the finger travels in one sub-action.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.slideDistancePx,
                         "滑屏距离(px)", 0, es);
    y += rowSl + gap;

    // 4. 每帧最大移动 — per-frame step cap, paired with the Bezier curve.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.maxPerFramePx,
                         "每帧最大移动", 0, es);
    y += rowSl + gap;

    // 5. 触摸区域 toggle — shows/hides the dashed box on screen.
    widgets::switchButton(dl, wRect(x, y, w, rowSw),
                          g_pageBackFlash.touchArea.toggle,
                          "触摸区域", es);
    y += rowSw + gap;

    // 6. 闪光类别 multi-select — deferred list (multiSelectList below): same
    // one-draw-list reason as Aim/Trigger — keeps the open list above any
    // rows drawn after it (none here, but the pattern stays the same).
    const widgets::Rect catRow = wRect(x, y, w, rowDd);
    widgets::multiSelect(dl, catRow, g_pageBackFlash.category.sel,
                         "闪光类别",
                         g_pageBackFlash.category.itemPtrs.empty()
                            ? nullptr
                            : g_pageBackFlash.category.itemPtrs.data(),
                         static_cast<int>(g_pageBackFlash.category.itemPtrs.size()),
                         es, /*drawListInline=*/false);
    y += rowDd + gap;

    widgets::multiSelectList(dl, catRow, g_pageBackFlash.category.sel,
                             g_pageBackFlash.category.itemPtrs.empty()
                                ? nullptr
                                : g_pageBackFlash.category.itemPtrs.data(),
                             static_cast<int>(g_pageBackFlash.category.itemPtrs.size()),
                             es);
}

// ── drawBackFlashOverlays ──────────────────────────────────────────────────

void drawBackFlashOverlays() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    if (screenW <= 1.0f || screenH <= 1.0f) return;

    ensureBackFlashAreasPlaced(screenW, screenH);

    if (g_pageBackFlash.touchArea.toggle.value) {
        BackFlashTouchArea& area = g_pageBackFlash.touchArea;
        driveBackFlashTouch(area, mouse, screenW, screenH);

        const ImVec2 mn(area.x, area.y);
        const ImVec2 mx(area.x + area.w, area.y + area.h);
        const ImVec2 centre(area.x + area.w * 0.5f, area.y + area.h * 0.5f);

        dl->AddRectFilled(mn, mx, kOverlayFill, 4.0f);
        drawDashedRect(dl, mn, mx, kOverlayBorder,
                       kTouchThickness, kTouchDashLen, kTouchDashGap);
        drawCornerHandles(dl, mn, mx, kTouchCornerDraw, kOverlayHandle);
        drawCentredLabel(dl, centre, kOverlayLabelSize, kOverlayLabel, "触摸区域");
    }
}

// ── syncBackFlashPage — the swipe state machine ────────────────────────────

void syncBackFlashPage() {
    PageBackFlash& p = g_pageBackFlash;
    const ImGuiIO& io = ImGui::GetIO();
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    const int64_t now = nowMs();

    // Geometry must exist regardless of the toggle state — the swipe picks
    // a press point inside the area even when the dashed box is hidden.
    if (screenW > 1.0f && screenH > 1.0f) {
        ensureBackFlashAreasPlaced(screenW, screenH);
    }

    // ── Gates ───────────────────────────────────────────────────────────
    // Master switch on, injection backend ready, model loaded + worker
    // running, at least one flash class selected, touch area placed. No
    // "hold" gate — back-flash is a continuous feature; the synthetic
    // finger is owned by this state machine for the entire cycle.
    //
    // The Kalman tracker is warmed by syncAimPage() running in the SAME
    // frame (which sees `blockingAim=true` set here first — see the file
    // header). One frame of staleness in the target pick is acceptable
    // for a flash that lasts hundreds of milliseconds.
    const infer::runtime::Snapshot snap = infer::runtime::snapshot();
    const bool gatesOpen =
        p.enabled.value &&
        inject_is_ready() &&
        snap.active &&
        p.category.hasAnySelected() &&
        p.touchArea.placed;

    if (!gatesOpen) {
        cancelFinger();
        p.phase        = PageBackFlash::Phase::Idle;
        p.phaseEnterMs = 0;
        p.blockingAim  = false;
        return;
    }

    const float scx = screenW * 0.5f;
    const float scy = screenH * 0.5f;

    // ── Pick the flash target ──────────────────────────────────────────────
    // Nearest confirmed track in the flash-category set; falls back to the
    // raw detection box when no track is confirmed yet (the track is the
    // smoothed centre — preferred when available, raw as a safety net so a
    // first-frame flash is still actionable).
    float bestCx = 0.0f, bestCy = 0.0f;
    bool  hasTarget = false;
    float bestDistSq = 1e18f;

    const auto& tracks = tracking::tracker().tracks();
    for (const auto& t : tracks) {
        if (!t.confirmed) continue;
        if (!p.category.isClassSelected(t.cls)) continue;
        const float dx = t.cx - scx, dy = t.cy - scy;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestDistSq) {
            bestDistSq = d2;
            bestCx = t.cx;
            bestCy = t.cy;
            hasTarget = true;
        }
    }
    if (!hasTarget) {
        for (const auto& b : snap.detections) {
            if (!p.category.isClassSelected(b.cls)) continue;
            const float dx = b.cx() - scx, dy = b.cy() - scy;
            const float d2 = dx * dx + dy * dy;
            if (d2 < bestDistSq) {
                bestDistSq = d2;
                bestCx = b.cx();
                bestCy = b.cy();
                hasTarget = true;
            }
        }
    }

    // ── State machine ─────────────────────────────────────────────────────
    //
    // Once a cycle starts (entering WaitFlashDelay) it runs to completion
    // even if the flash target disappears mid-cycle — cancelling part-way
    // would leave the synthetic finger mid-gesture, which is the worst of
    // both worlds. Each phase is purely time-based once entered.
    switch (p.phase) {
        case PageBackFlash::Phase::Idle: {
            p.blockingAim = false;
            if (!hasTarget) return;
            // ── Begin a cycle ─────────────────────────────────────────────
            p.phase        = PageBackFlash::Phase::WaitFlashDelay;
            p.phaseEnterMs = now;
            p.blockingAim  = true;   // hold the slot from this frame forward
            return;
        }

        case PageBackFlash::Phase::WaitFlashDelay: {
            if (now - p.phaseEnterMs < static_cast<int64_t>(p.flashDelay.value)) return;

            // ── Enter Swiping: pick a press point + END, finger DOWN ──────
            pickPressPoint(p, p.pressX, p.pressY);
            const int dir = swipeDirection(bestCx, scx);
            const float dist = std::max(1.0f, p.slideDistancePx.value);

            // END = press ± dist on the swipe axis (Y stays at press.y, so
            // the slide is horizontal — matches how a player turns a view
            // with a thumb swipe), plus ±kEndJitterPx on each axis. The
            // jitter is one-shot: the END is randomised, but every frame
            // along the Bezier path is deterministic from there.
            p.swipeEndX = p.pressX + dir * dist + randSigned(p) * kEndJitterPx;
            p.swipeEndY = p.pressY                     + randSigned(p) * kEndJitterPx;

            p.curStartX = p.pressX;
            p.curStartY = p.pressY;
            p.curEndX   = p.swipeEndX;
            p.curEndY   = p.swipeEndY;

            uinput_down(UINPUT_SLOT_PRIMARY, UINPUT_ID_PRIMARY,
                        static_cast<int>(p.pressX),
                        static_cast<int>(p.pressY));
            p.fingerDown = true;
            p.lastX = p.pressX;
            p.lastY = p.pressY;

            armSubAction(p, dist);
            p.phase = PageBackFlash::Phase::Swiping;
            return;
        }

        case PageBackFlash::Phase::Swiping: {
            advanceSubFrame(p);
            if (subActionDone(p)) {
                // ── Swipe done — lift, wait for reset ──────────────────────
                uinput_up(UINPUT_SLOT_PRIMARY);
                p.fingerDown = false;
                p.phase        = PageBackFlash::Phase::WaitResetDelay;
                p.phaseEnterMs = now;
            }
            return;
        }

        case PageBackFlash::Phase::WaitResetDelay: {
            if (now - p.phaseEnterMs < static_cast<int64_t>(p.resetDelay.value)) return;

            // ── Enter Resetting: from the post-swipe position back to the
            // press point, with ±kEndJitterPx random on the END (the press
            // point itself is the deterministic anchor). Finger DOWN at the
            // current (post-swipe) position. ────────────────────────────────
            p.curStartX = p.lastX;
            p.curStartY = p.lastY;
            p.curEndX   = p.pressX + randSigned(p) * kEndJitterPx;
            p.curEndY   = p.pressY + randSigned(p) * kEndJitterPx;

            uinput_down(UINPUT_SLOT_PRIMARY, UINPUT_ID_PRIMARY,
                        static_cast<int>(p.curStartX),
                        static_cast<int>(p.curStartY));
            p.fingerDown = true;

            const float dist = std::max(1.0f, p.slideDistancePx.value);
            armSubAction(p, dist);
            p.phase = PageBackFlash::Phase::Resetting;
            return;
        }

        case PageBackFlash::Phase::Resetting: {
            advanceSubFrame(p);
            if (subActionDone(p)) {
                // ── Cycle done — lift, drop the slot hold, back to Idle ────
                uinput_up(UINPUT_SLOT_PRIMARY);
                p.fingerDown = false;
                p.phase        = PageBackFlash::Phase::Idle;
                p.phaseEnterMs = 0;
                p.blockingAim  = false;   // aim may press again next frame
            }
            return;
        }
    }
}

bool backflashIsBlockingAim() {
    // Cheap inline read — syncAimPage calls this every frame on the gate
    // path; the alternative of routing it through a getter on the page
    // struct costs nothing functional but adds a layer for no benefit.
    return g_pageBackFlash.blockingAim;
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng

#endif  // HAS_IMGUI
