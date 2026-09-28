// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::backflash — implementation. See backflash_section.h
//  for the touch lifecycle, the blockingAim contract, and the page layout.
//
//  Enhanced with:
//    * Multi-harmonic curve (多谐波不规则闭合曲线) for human-like trajectory
//    * Brightness fallback detection (亮度兜底检测) for full-screen whiteout
//    * Lossless accumulator (记账式无损累积器) for zero-residual return
//    * Waypoint recording & reverse playback (航点录制/倒序回放)
//    * Settle phase (停顿) to simulate a real player pausing
//    * Cooldown timer (冷却) to prevent rapid re-triggering
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

#include "capture/capture.h"
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

// Maximum waypoints recorded during one swipe. Well above what any realistic
// swipe produces (~200 at 120 Hz × 2 s) but cheap to bound.
constexpr size_t kMaxRecordedSteps = 2000;

// π constant
constexpr double kPi = 3.14159265358979323846;

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

/// Double-precision uniform in [0, 1).
inline double randPositiveD(PageBackFlash& p) {
    p.rng = p.rng * 1103515245u + 12345u;
    return static_cast<double>((p.rng >> 8) & 0xFFFFFFu) * (1.0 / 16777216.0);
}

/// Double-precision uniform in [-1, 1).
inline double randSignedD(PageBackFlash& p) {
    return randPositiveD(p) * 2.0 - 1.0;
}

/**
 * Evaluates the human-like swipe trajectory at normalized progress t ∈ [0, 1].
 *
 * At t = 0: outX = 0, outY = 0 (exact press point).
 * At t = 1: outX = dirSign * dist, outY = 0 (exact target point).
 *
 * Uses:
 *   - Smooth cubic ease-in-out progress along the main swipe axis.
 *   - Multi-harmonic perturbation (simulating human hand irregularity/tremor).
 *   - Natural thumb rotation arc modulation (pitchRatio).
 *   - Per-cycle drift (driftY).
 *
 * An envelope function sin(π·t) guarantees that all perturbations and vertical offsets
 * smoothly vanish at t = 0 and t = 1, ensuring zero error at the endpoints.
 */
inline void evalMultiHarmonic(const PageBackFlash& p, double t,
                              double& outX, double& outY) {
    const double dist = std::max(1.0, static_cast<double>(p.slideDistancePx.value));
    const double wob  = static_cast<double>(p.wobble.value);
    const double pitch = static_cast<double>(p.pitchRatio.value);

    t = std::clamp(t, 0.0, 1.0);

    // Cubic smoothstep for ease-in and ease-out
    const double u = t * t * (3.0 - 2.0 * t);

    // Multi-harmonic modulation (fundamental + 2nd harmonic + 3rd harmonic)
    const double harm = 0.55 * std::sin(2.0 * kPi * 1.0 * t + p.harm1Phase)
                      + 0.30 * std::sin(2.0 * kPi * 2.0 * t + p.harm2Phase)
                      + 0.15 * std::sin(2.0 * kPi * 3.0 * t + p.harm3Phase);

    // Boundary envelope: vanishes at t = 0 and t = 1
    const double envelope = std::sin(kPi * t);

    // Horizontal: main swipe travel towards dirSign * dist
    const double baseX = p.dirSign * dist * u;
    const double devX  = p.dirSign * (wob * dist * 0.05 * harm) * envelope;
    outX = baseX + devX;

    // Vertical: thumb rotation arc + wobble + drift
    const double arcY   = pitch * dist * envelope;
    const double devY   = (wob * dist * 0.08 * harm) * envelope;
    const double driftY = p.driftY * envelope;
    outY = arcY + devY + driftY;
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

// Picks a random press point INSIDE the touch area oriented towards the swipe direction
// to maximize available travel distance across the screen.
void pickPressPoint(PageBackFlash& p, int dir, float screenW, float screenH,
                    float& outX, float& outY) {
    const BackFlashTouchArea& a = p.touchArea;
    if (dir > 0) {
        // Swiping right (+X): start near left edge of touch area for maximum stroke runway
        outX = a.x + randPositive(p) * std::min(a.w * 0.25f, 60.0f);
    } else {
        // Swiping left (-X): start near right edge of touch area
        outX = a.x + a.w - randPositive(p) * std::min(a.w * 0.25f, 60.0f);
    }
    outY = a.y + a.h * 0.25f + randPositive(p) * (a.h * 0.5f);

    // Keep within valid display bounds
    outX = std::clamp(outX, 10.0f, screenW - 10.0f);
    outY = std::clamp(outY, 10.0f, screenH - 10.0f);
}

// Resets all per-cycle state to prepare for a new swipe cycle.
void resetCycleState(PageBackFlash& p) {
    p.intentX = 0.0;
    p.intentY = 0.0;
    p.sentX   = 0;
    p.sentY   = 0;
    p.forwardPos  = 0.0;
    p.totalForward = 0.0;
    p.waypointDx.clear();
    p.waypointDy.clear();
    p.backIdx   = 0;
    p.backShift = 0.0;
    p.backDone  = false;
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

// ── sampleFrameBrightness ──────────────────────────────────────────────────
//
// Samples the latest capture frame to compute:
//   outBrightFraction — ratio of sampled pixels whose luma >= brightPixelLevel
//   outMeanLuma       — average luma of all sampled pixels, normalised to [0,1]
//
// Uses step-16 interval sampling over the RGBA capture buffer. At 640×640
// this yields ~2560 samples — enough for stable statistics at negligible CPU
// cost. The capture module stores frames as tightly-packed RGBA.

void sampleFrameBrightness(float& outBrightFraction, float& outMeanLuma) {
    outBrightFraction = 0.0f;
    outMeanLuma       = 0.0f;

    int w = 0, h = 0;
    if (!capture::peekSize(w, h) || w <= 0 || h <= 0) return;

    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    // Use a thread-local scratch buffer to avoid allocation every frame.
    static thread_local std::vector<uint8_t> buf;
    if (buf.size() != bytes) buf.resize(bytes);

    uint64_t lastId = 0;
    int gotW = 0, gotH = 0;
    if (!capture::copyLatestInto(buf.data(), buf.size(), gotW, gotH, lastId)) return;
    if (gotW <= 0 || gotH <= 0) return;

    const size_t total = static_cast<size_t>(gotW) * static_cast<size_t>(gotH);
    const int level = static_cast<int>(
        g_pageBackFlash.brightPixelLevel.value * 255.0f);

    uint64_t bright = 0;
    uint64_t sum    = 0;
    uint64_t n      = 0;

    // Step-16 sampling: every 16th pixel. RGBA layout, 4 bytes per pixel.
    for (size_t i = 0; i < total; i += 16) {
        const uint8_t* px = buf.data() + i * 4;
        // RGBA → luma as (R + G + B) / 3
        const int luma = (static_cast<int>(px[0]) + px[1] + px[2]) / 3;
        sum += static_cast<uint64_t>(luma);
        if (luma >= level) ++bright;
        ++n;
    }

    if (n > 0) {
        outBrightFraction = static_cast<float>(
            static_cast<double>(bright) / static_cast<double>(n));
        outMeanLuma = static_cast<float>(
            static_cast<double>(sum) / static_cast<double>(n) / 255.0);
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

    // ── Row order: brightness → timing → motion → overlay → filter ──────

    // 1. 亮度兜底检测 switch
    widgets::switchButton(dl, wRect(x, y, w, rowSw),
                          g_pageBackFlash.brightDetect,
                          "亮度兜底检测", es);
    y += rowSw + gap;

    // 2. 背闪延迟 (ms)
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.flashDelay,
                         "背闪延迟", 0, es);
    y += rowSl + gap;

    // 3. 复位延迟 (ms)
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.resetDelay,
                         "复位延迟", 0, es);
    y += rowSl + gap;

    // 4. 停顿时间 (ms) — pause at the end of swipe, simulating human hesitation
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.settleMs,
                         "停顿时间", 0, es);
    y += rowSl + gap;

    // 5. 冷却时间 (ms) — minimum gap between two trigger cycles
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.cooldownMs,
                         "冷却时间", 0, es);
    y += rowSl + gap;

    // 6. 滑屏距离(px) — total px the finger travels in one sub-action.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.slideDistancePx,
                         "滑屏距离(px)", 0, es);
    y += rowSl + gap;

    // 7. 滑屏速度上限 — max speed the finger moves along the curve
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.swipeSpeed,
                         "滑屏速度上限", 0, es);
    y += rowSl + gap;

    // 8. 每帧最大移动 — per-frame step cap, safety net for the curve
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.maxPerFramePx,
                         "每帧最大移动", 0, es);
    y += rowSl + gap;

    // 9. 轨迹不规则度 — multi-harmonic wobble factor (0=straight, 1=wavy)
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.wobble,
                         "轨迹不规则度", 2, es);
    y += rowSl + gap;

    // 10. 垂直分量 — vertical pitch ratio of the swipe
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.pitchRatio,
                         "垂直分量", 2, es);
    y += rowSl + gap;

    // 11. 随机抖动(px) — per-frame random noise on the finger position
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.jitterPx,
                         "随机抖动(px)", 1, es);
    y += rowSl + gap;

    // 12. 亮度阈值 — whiteout mean-brightness trigger level
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.brightThreshold,
                         "亮度阈值", 2, es);
    y += rowSl + gap;

    // 13. 高亮像素阈值 — pixel-level brightness threshold
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.brightPixelLevel,
                         "高亮像素阈值", 2, es);
    y += rowSl + gap;

    // 14. 高亮占比阈值 — bright pixel ratio to trigger
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl),
                         g_pageBackFlash.brightBlobRatio,
                         "高亮占比阈值", 2, es);
    y += rowSl + gap;

    // 15. 触摸区域 toggle — shows/hides the dashed box on screen.
    widgets::switchButton(dl, wRect(x, y, w, rowSw),
                          g_pageBackFlash.touchArea.toggle,
                          "触摸区域", es);
    y += rowSw + gap;

    // 16. 闪光类别 multi-select
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

// ── syncBackFlashPage — the enhanced swipe state machine ───────────────────

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

    // ── Brightness sampling (every frame, cheap) ────────────────────────
    if (p.brightDetect.value) {
        sampleFrameBrightness(p.lastBrightFraction, p.lastMeanLuma);
    }

    // ── Pick the flash target ──────────────────────────────────────────────
    // Nearest confirmed track in the flash-category set; falls back to the
    // raw detection box when no track is confirmed yet.
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

    // ── Brightness fallback trigger ─────────────────────────────────────
    // If YOLO didn't find a flash but the screen is bright enough, treat it
    // as a flash trigger. Direction defaults to a random side since we can't
    // determine the source from brightness alone.
    bool brightnessTrigger = false;
    if (!hasTarget && p.brightDetect.value) {
        const bool whiteout = (p.lastMeanLuma >= p.brightThreshold.value);
        const bool brightBlob = (p.lastBrightFraction >= p.brightBlobRatio.value);
        if (whiteout || brightBlob) {
            brightnessTrigger = true;
            // Use random direction when we can't determine flash source
            bestCx = scx + (randSigned(p) > 0.0f ? -200.0f : 200.0f);
            bestCy = scy;
        }
    }

    // ── State machine ─────────────────────────────────────────────────────
    switch (p.phase) {
        case PageBackFlash::Phase::Idle: {
            p.blockingAim = false;
            if (!hasTarget && !brightnessTrigger) return;

            // ── Cooldown check ─────────────────────────────────────────────
            if (p.lastTriggerMs > 0) {
                const int64_t elapsed = now - p.lastTriggerMs;
                if (elapsed < static_cast<int64_t>(p.cooldownMs.value)) return;
            }

            // Determine and lock swipe direction at trigger detection
            const int initialDir = swipeDirection(
                hasTarget ? bestCx : (scx + (randSigned(p) > 0.0f ? -200.0f : 200.0f)),
                scx);
            p.dirSign = static_cast<double>(initialDir);

            // ── Begin a cycle ─────────────────────────────────────────────
            p.phase        = PageBackFlash::Phase::WaitFlashDelay;
            p.phaseEnterMs = now;
            p.cycleStartMs = now;
            p.blockingAim  = true;   // hold the slot from this frame forward
            p.lastFrameMs  = now;    // init dt tracking
            return;
        }

        case PageBackFlash::Phase::WaitFlashDelay: {
            if (now - p.phaseEnterMs < static_cast<int64_t>(p.flashDelay.value)) return;

            // If we have an active target during delay, update direction; otherwise retain locked direction
            if (hasTarget) {
                p.dirSign = static_cast<double>(swipeDirection(bestCx, scx));
            }
            const int dir = (p.dirSign >= 0.0) ? 1 : -1;

            // ── Enter Swiping: pick a press point, init multi-harmonic ────
            pickPressPoint(p, dir, screenW, screenH, p.pressX, p.pressY);

            // Reset accumulator and waypoints
            resetCycleState(p);

            // Generate random phases for multi-harmonic curve
            p.harm1Phase = randPositiveD(p) * 2.0 * kPi;
            p.harm2Phase = randPositiveD(p) * 2.0 * kPi;
            p.harm3Phase = randPositiveD(p) * 2.0 * kPi;

            // Vertical drift: ±3% of slide distance
            const double dist = std::max(1.0,
                static_cast<double>(p.slideDistancePx.value));
            p.driftY = randSignedD(p) * dist * 0.03;
            p.dirSign = static_cast<double>(dir);
            p.totalForward = dist;
            p.forwardPos = 0.0;

            // Press finger at the chosen point
            uinput_down(UINPUT_SLOT_PRIMARY, UINPUT_ID_PRIMARY,
                        static_cast<int>(p.pressX),
                        static_cast<int>(p.pressY));
            p.fingerDown = true;
            p.lastX = p.pressX;
            p.lastY = p.pressY;
            p.lastFrameMs = now;

            // Reserve waypoint storage
            p.waypointDx.reserve(512);
            p.waypointDy.reserve(512);

            p.phase        = PageBackFlash::Phase::Swiping;
            p.phaseEnterMs = now;
            return;
        }

        case PageBackFlash::Phase::Swiping: {
            // dt-based speed control: advance along the curve proportionally
            // to real elapsed time, capped at 100ms to prevent jumps after hitches.
            const double dtMs = static_cast<double>(
                std::min(now - p.lastFrameMs, static_cast<int64_t>(100)));
            const double dt = dtMs / 1000.0;
            p.lastFrameMs = now;

            const double speed = std::max(100.0,
                static_cast<double>(p.swipeSpeed.value));
            const double cap = static_cast<double>(std::max(1.0f, p.maxPerFramePx.value));

            // Bound per-frame progress by maxPerFramePx so high swipeSpeed
            // cannot prematurely finish the swipe before the finger has physically traveled the distance.
            const double step = std::min(speed * dt, cap);
            p.forwardPos = std::min(p.forwardPos + step, p.totalForward);

            const double t = (p.totalForward > 0.0)
                ? std::min(p.forwardPos / p.totalForward, 1.0)
                : 1.0;

            // Evaluate multi-harmonic curve at progress t
            double cx, cy;
            evalMultiHarmonic(p, t, cx, cy);

            // Target position = press point + curve offset
            double targetX = static_cast<double>(p.pressX) + cx;
            double targetY = static_cast<double>(p.pressY) + cy;

            // Add per-frame jitter (only while still in motion, not at end)
            double jitX = 0.0, jitY = 0.0;
            if (p.forwardPos < p.totalForward && p.jitterPx.value > 0.0f) {
                jitX = randSignedD(p) * static_cast<double>(p.jitterPx.value);
                jitY = randSignedD(p) * static_cast<double>(p.jitterPx.value);
            }

            // Compute candidate position with jitter
            float nx = static_cast<float>(targetX + jitX);
            float ny = static_cast<float>(targetY + jitY);

            // Screen boundary clamping
            nx = std::clamp(nx, 5.0f, screenW - 5.0f);
            ny = std::clamp(ny, 5.0f, screenH - 5.0f);

            // Per-frame movement cap check against last position
            const float capF = static_cast<float>(cap);
            const float dx = nx - p.lastX, dy = ny - p.lastY;
            const float d  = std::sqrt(dx * dx + dy * dy);
            if (d > capF && d > 0.0f) {
                const float s = capF / d;
                nx = p.lastX + dx * s;
                ny = p.lastY + dy * s;
            }

            // Record the ACTUAL step that was injected into uinput
            const float actualDx = nx - p.lastX;
            const float actualDy = ny - p.lastY;
            if (p.waypointDx.size() < kMaxRecordedSteps) {
                p.waypointDx.push_back(actualDx);
                p.waypointDy.push_back(actualDy);
            }

            p.lastX = nx;
            p.lastY = ny;
            p.sentX = static_cast<int>(nx) - static_cast<int>(p.pressX);
            p.sentY = static_cast<int>(ny) - static_cast<int>(p.pressY);

            uinput_move(UINPUT_SLOT_PRIMARY,
                        static_cast<int>(nx), static_cast<int>(ny));

            // Check if swipe is complete:
            // forwardPos reached totalForward AND finger reached within cap of target (or timeout guard)
            const float clampedTargetX = std::clamp(static_cast<float>(targetX), 5.0f, screenW - 5.0f);
            const float clampedTargetY = std::clamp(static_cast<float>(targetY), 5.0f, screenH - 5.0f);
            const float remDx = clampedTargetX - nx;
            const float remDy = clampedTargetY - ny;
            const float remD  = std::sqrt(remDx * remDx + remDy * remDy);

            const bool reachedTarget = (p.forwardPos >= p.totalForward - 1e-9) && (remD <= capF);
            const bool timedOut = (now - p.phaseEnterMs > 3000);

            if (reachedTarget || timedOut) {
                // Swipe done — lift finger, enter settling
                p.swipeEndX = p.lastX;
                p.swipeEndY = p.lastY;
                uinput_up(UINPUT_SLOT_PRIMARY);
                p.fingerDown = false;
                p.phase        = PageBackFlash::Phase::Settling;
                p.phaseEnterMs = now;
            }
            return;
        }

        case PageBackFlash::Phase::Settling: {
            // Wait settleMs + resetDelay before starting reset.
            // This simulates a real player pausing momentarily after turning.
            const int64_t totalWait = static_cast<int64_t>(p.settleMs.value)
                                    + static_cast<int64_t>(p.resetDelay.value);
            if (now - p.phaseEnterMs < totalWait) return;

            // ── Enter Resetting: press finger at post-swipe position ──────
            uinput_down(UINPUT_SLOT_PRIMARY, UINPUT_ID_PRIMARY,
                        static_cast<int>(p.swipeEndX),
                        static_cast<int>(p.swipeEndY));
            p.fingerDown = true;
            p.lastX = p.swipeEndX;
            p.lastY = p.swipeEndY;
            p.lastFrameMs = now;

            // Reset playback state
            p.backIdx   = 0;
            p.backShift = 0.0;
            p.backDone  = false;

            p.phase        = PageBackFlash::Phase::Resetting;
            p.phaseEnterMs = now;
            return;
        }

        case PageBackFlash::Phase::Resetting: {
            // dt-based reverse playback: consume recorded waypoints in reverse
            // order, moving the finger back toward the press point.
            const double dtMs = static_cast<double>(
                std::min(now - p.lastFrameMs, static_cast<int64_t>(100)));
            const double dt = dtMs / 1000.0;
            p.lastFrameMs = now;

            const size_t n = p.waypointDx.size();

            // Timeout guard: if resetting takes > 3 seconds, force completion
            if (now - p.phaseEnterMs > 3000) {
                p.backDone = true;
            }

            if (p.backDone || p.backIdx >= n) {
                // ── Cycle done — lift, drop the slot hold ────
                // Move finger precisely back to press point before lifting
                uinput_move(UINPUT_SLOT_PRIMARY,
                            static_cast<int>(p.pressX),
                            static_cast<int>(p.pressY));
                uinput_up(UINPUT_SLOT_PRIMARY);
                p.fingerDown    = false;
                p.lastTriggerMs = now;
                p.phase         = PageBackFlash::Phase::Cooldown;
                p.phaseEnterMs  = now;
                p.blockingAim   = false;
                return;
            }

            // Reverse-playback speed is 15% faster than the swipe speed
            const double speed = std::max(100.0,
                static_cast<double>(p.swipeSpeed.value)) * 1.15;
            const double cap = static_cast<double>(std::max(1.0f, p.maxPerFramePx.value));
            double budget = std::min(speed * dt, cap);
            if (budget <= 0.0) budget = 1.0;

            float frameMoveX = 0.0f;
            float frameMoveY = 0.0f;

            // Consume waypoints in reverse order (LIFO), supporting
            // cross-tick splitting when a single waypoint is larger than
            // this frame's budget.
            while (budget > 1e-9 && p.backIdx < n) {
                const size_t ri = n - 1 - p.backIdx;
                const double sx = static_cast<double>(p.waypointDx[ri]);
                const double sy = static_cast<double>(p.waypointDy[ri]);
                const double mag = std::sqrt(sx * sx + sy * sy);

                if (mag < 1e-9) {
                    ++p.backIdx;
                    p.backShift = 0.0;
                    continue;
                }

                const double left = mag - p.backShift;
                if (left <= 0.0) {
                    ++p.backIdx;
                    p.backShift = 0.0;
                    continue;
                }

                // Consume as much of this waypoint as the budget allows
                const double consume = std::min(left, budget);
                const double frac = consume / mag;

                // Move in the REVERSE direction
                frameMoveX += static_cast<float>(-sx * frac);
                frameMoveY += static_cast<float>(-sy * frac);

                p.backShift += consume;
                budget -= consume;

                if (p.backShift >= mag - 1e-9) {
                    ++p.backIdx;
                    p.backShift = 0.0;
                }
            }

            p.lastX += frameMoveX;
            p.lastY += frameMoveY;

            // Clamp to screen boundaries
            p.lastX = std::clamp(p.lastX, 5.0f, screenW - 5.0f);
            p.lastY = std::clamp(p.lastY, 5.0f, screenH - 5.0f);

            // Single uinput_move per frame
            uinput_move(UINPUT_SLOT_PRIMARY,
                        static_cast<int>(p.lastX),
                        static_cast<int>(p.lastY));

            if (p.backIdx >= n) {
                p.backDone = true;
            }
            return;
        }

        case PageBackFlash::Phase::Cooldown: {
            // Cooldown is handled by the Idle → trigger check, so just
            // transition back immediately. The cooldown timer is enforced
            // at the Idle gate.
            p.phase        = PageBackFlash::Phase::Idle;
            p.phaseEnterMs = 0;
            p.blockingAim  = false;
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
