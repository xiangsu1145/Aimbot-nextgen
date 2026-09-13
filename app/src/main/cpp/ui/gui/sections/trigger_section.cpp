// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::trigger — implementation. See trigger_section.h
//  for the porting notes (old-project sources + what changed).
// ─────────────────────────────────────────────────────────────────────────────
#include "trigger_section.h"

#include "ui/gui/hud.h"
#include "ui/gui/theme.h"

#ifdef HAS_IMGUI

#include <algorithm>
#include <chrono>
#include <cmath>

#include "imgui.h"

#include "inference/model_runtime.h"
#include "inference/model_store.h"
#include "input/touch_reader.h"
#include "input/uinput_inject.h"
#include "tracking/kalman_tracker.h"

namespace aimbotng {
namespace ui {
namespace sections {

PageTrigger g_pageTrigger;

namespace {

// ── Overlay geometry constants (same interaction as the Aim page) ─────────
constexpr float kTouchEdgeGrip   = 24.0f;
constexpr float kTouchCornerGrip = 36.0f;
constexpr float kTouchCornerDraw = 18.0f;
constexpr float kTouchMinSize    = 200.0f;
constexpr float kTouchDashLen    = 16.0f;
constexpr float kTouchDashGap    = 8.0f;
constexpr float kTouchThickness  = 2.5f;

constexpr float kTrigHandleHitR = 36.0f;
constexpr float kTrigHandleDraw = 9.0f;
constexpr float kTrigThickness  = 2.5f;
constexpr float kTrigMinR       = 60.0f;
constexpr int   kTrigSegments   = 64;
constexpr float kOverlayLabelSize = 38.0f;

constexpr ImU32 kOverlayBorder = IM_COL32(238, 240, 246, 215);
constexpr ImU32 kOverlayHandle = IM_COL32(255, 255, 255, 235);
constexpr ImU32 kOverlayFill   = IM_COL32( 61, 123, 255,  28);
constexpr ImU32 kOverlayLabel  = IM_COL32(238, 240, 246, 235);

inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline float dist2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

inline widgets::Rect wRectOf(float wx, float wy, float ww, float wh,
                              float s, const Xf& xf) {
    const ImVec2 p = xf.pt(wx, wy);
    return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
}

/// LCG uniform in [0,1). Same cheap deterministic generator as the Aim page.
inline float rand01(PageTrigger& p) {
    p.rng = p.rng * 1103515245u + 12345u;
    return static_cast<float>((p.rng >> 8) & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

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
    dashEdge(ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y));
    dashEdge(ImVec2(mx.x, mn.y), ImVec2(mx.x, mx.y));
    dashEdge(ImVec2(mx.x, mx.y), ImVec2(mn.x, mx.y));
    dashEdge(ImVec2(mn.x, mx.y), ImVec2(mn.x, mn.y));
}

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

void drawCentredLabel(ImDrawList* dl, const ImVec2& center, float size,
                      ImU32 col, const char* text) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size,
                ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                col, text);
}

// ── Area placement + real-finger tests ────────────────────────────────────
// Same rule as the Aim page: geometry exists regardless of the toggle state
// (toggles only control drawing), centred by default.

void ensureTriggerAreasPlaced(float screenW, float screenH) {
    PageTrigger& p = g_pageTrigger;
    if (!p.touchArea.placed) {
        p.touchArea.x = (screenW - p.touchArea.w) * 0.5f;
        p.touchArea.y = (screenH - p.touchArea.h) * 0.5f;
        p.touchArea.placed = true;
    }
    if (!p.holdArea.placed) {
        p.holdArea.cx = screenW * 0.5f;
        p.holdArea.cy = screenH * 0.5f;
        p.holdArea.placed = true;
    }
}

/// Real finger inside the hold circle — the "hold to fire" gate.
/// (Old project: hold-to-aim fire-zone semantics.)
bool realFingerInHoldArea() {
    PageTrigger& p = g_pageTrigger;
    if (!reader_is_ready() || !p.holdArea.placed) return false;
    const TriggerHoldArea& a = p.holdArea;
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

/// Real finger inside the touch (fire) box — manual-fire suppression.
/// (Old project: `isFingerInFireZone()` early-return in processTrigger —
/// while the player is tapping fire themselves, the auto trigger stays out.)
bool realFingerInTouchArea() {
    PageTrigger& p = g_pageTrigger;
    if (!reader_is_ready() || !p.touchArea.placed) return false;
    const TriggerTouchArea& a = p.touchArea;
    ReaderPointer pts[READER_MAX_POINTERS];
    const int n = reader_read_pointers(pts, READER_MAX_POINTERS);
    for (int i = 0; i < n; ++i) {
        if (pts[i].x >= a.x && pts[i].x <= a.x + a.w &&
            pts[i].y >= a.y && pts[i].y <= a.y + a.h) return true;
    }
    return false;
}

// ── Touch-box hit testing & drag (same as the Aim page) ───────────────────

TriggerTouchArea::Drag hitTestTouchBox(const TriggerTouchArea& area, ImVec2 mouse) {
    using Drag = TriggerTouchArea::Drag;
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

void driveTouchBox(TriggerTouchArea& area, ImVec2 mouse, float screenW, float screenH) {
    using Drag = TriggerTouchArea::Drag;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (area.drag == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Drag hit = hitTestTouchBox(area, mouse);
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

// ── Hold-circle hit testing & drag (same as the Aim page) ─────────────────

TriggerHoldArea::Drag hitTestHold(const TriggerHoldArea& area, ImVec2 mouse) {
    using Drag = TriggerHoldArea::Drag;
    const float hr = kTrigHandleHitR;

    if (dist2(mouse.x, mouse.y, area.cx,         area.cy - area.r) <= hr * hr) return Drag::ResizeN;
    if (dist2(mouse.x, mouse.y, area.cx + area.r, area.cy        ) <= hr * hr) return Drag::ResizeE;
    if (dist2(mouse.x, mouse.y, area.cx,         area.cy + area.r) <= hr * hr) return Drag::ResizeS;
    if (dist2(mouse.x, mouse.y, area.cx - area.r, area.cy        ) <= hr * hr) return Drag::ResizeW;

    if (dist2(mouse.x, mouse.y, area.cx, area.cy) <= area.r * area.r) return Drag::Move;

    return Drag::None;
}

void driveHoldArea(TriggerHoldArea& area, ImVec2 mouse, float screenW, float screenH) {
    using Drag = TriggerHoldArea::Drag;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (area.drag == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Drag hit = hitTestHold(area, mouse);
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

    if (area.cx - area.r < 0.0f)     area.cx = area.r;
    if (area.cy - area.r < 0.0f)     area.cy = area.r;
    if (area.cx + area.r > screenW) area.cx = screenW - area.r;
    if (area.cy + area.r > screenH) area.cy = screenH - area.r;
}

}  // namespace

// ── TriggerCategoryState ──────────────────────────────────────────────────

void TriggerCategoryState::syncFromModel(const std::vector<std::string>& modelClasses) {
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

// ── drawTriggerSection ────────────────────────────────────────────────────

void drawTriggerSection(ImDrawList* dl, float x, float& y, float w,
                        float /*bottomY*/, float s, float es, const Xf& xf) {
    const float gap   = 12.0f * s;
    const float rowSw = widgets::kSwitchRowH   * s;
    const float rowSl = widgets::kSliderRowH   * s;
    const float rowDd = widgets::kDropdownRowH * s;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        return wRectOf(wx, wy, ww, wh, s, xf);
    };

    // Timing + tap sliders — ranges/defaults from the old project.
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageTrigger.reaction, "反应速度", 0, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageTrigger.cooldown, "冷却时间", 0, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageTrigger.upFluct, "向上波动", 0, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageTrigger.downFluct, "向下波动", 0, es);
    y += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageTrigger.touchDuration, "触摸时间", 0, es);
    y += rowSl + gap;

    // ── Touch-area toggle ────────────────────────────────────────────────
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageTrigger.touchArea.toggle,
                          "触摸区域", es);
    y += rowSw + gap;

    // ── Continuous-trigger toggle ────────────────────────────────────────
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageTrigger.continuousTrigger,
                          "持续触发", es);
    y += rowSw + gap;

    // ── Hold-area toggle (hidden when Continuous Trigger is on) ──────────
    if (!g_pageTrigger.continuousTrigger.value) {
        widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageTrigger.holdArea.toggle,
                              "触发区域", es);
        y += rowSw + gap;
    }

    // ── Trigger-category multi-select ────────────────────────────────────
    // Deferred list (multiSelectList below): same one-draw-list reason as the
    // Aim page — keeps the open list above any rows drawn after it.
    const widgets::Rect catRow = wRect(x, y, w, rowDd);
    widgets::multiSelect(dl, catRow, g_pageTrigger.category.sel,
                         "触发类别",
                         g_pageTrigger.category.itemPtrs.empty()
                            ? nullptr
                            : g_pageTrigger.category.itemPtrs.data(),
                         static_cast<int>(g_pageTrigger.category.itemPtrs.size()),
                         es, /*drawListInline=*/false);
    y += rowDd + gap;

    widgets::multiSelectList(dl, catRow, g_pageTrigger.category.sel,
                             g_pageTrigger.category.itemPtrs.empty()
                                ? nullptr
                                : g_pageTrigger.category.itemPtrs.data(),
                             static_cast<int>(g_pageTrigger.category.itemPtrs.size()),
                             es);
}

// ── drawTriggerOverlays ───────────────────────────────────────────────────

void drawTriggerOverlays() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    if (screenW <= 1.0f || screenH <= 1.0f) return;

    ensureTriggerAreasPlaced(screenW, screenH);

    // ── Touch (fire) box ─────────────────────────────────────────────────
    if (g_pageTrigger.touchArea.toggle.value) {
        TriggerTouchArea& area = g_pageTrigger.touchArea;
        driveTouchBox(area, mouse, screenW, screenH);

        const ImVec2 mn(area.x, area.y);
        const ImVec2 mx(area.x + area.w, area.y + area.h);
        const ImVec2 centre(area.x + area.w * 0.5f, area.y + area.h * 0.5f);

        dl->AddRectFilled(mn, mx, kOverlayFill, 4.0f);
        drawDashedRect(dl, mn, mx, kOverlayBorder,
                       kTouchThickness, kTouchDashLen, kTouchDashGap);
        drawCornerHandles(dl, mn, mx, kTouchCornerDraw, kOverlayHandle);
        drawCentredLabel(dl, centre, kOverlayLabelSize, kOverlayLabel, "开火区");
    }

    // ── Hold (trigger) circle ────────────────────────────────────────────
    if (g_pageTrigger.holdArea.toggle.value) {
        TriggerHoldArea& area = g_pageTrigger.holdArea;
        driveHoldArea(area, mouse, screenW, screenH);

        const ImVec2 c(area.cx, area.cy);
        dl->AddCircleFilled(c, area.r, kOverlayFill, kTrigSegments);
        dl->AddCircle      (c, area.r, kOverlayBorder, kTrigSegments, kTrigThickness);

        dl->AddCircleFilled(ImVec2(area.cx,         area.cy - area.r), kTrigHandleDraw, kOverlayHandle);
        dl->AddCircleFilled(ImVec2(area.cx + area.r, area.cy        ), kTrigHandleDraw, kOverlayHandle);
        dl->AddCircleFilled(ImVec2(area.cx,         area.cy + area.r), kTrigHandleDraw, kOverlayHandle);
        dl->AddCircleFilled(ImVec2(area.cx - area.r, area.cy        ), kTrigHandleDraw, kOverlayHandle);
        drawCentredLabel(dl, c, kOverlayLabelSize, kOverlayLabel, "触发区");
    }
}

// ── syncTriggerPage — the trigger + tap loop ──────────────────────────────

void syncTriggerPage() {
    PageTrigger& p = g_pageTrigger;

    // ── Category list off the model (same fallback as the Aim page) ──────
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
    p.category.syncFromModel(classLabels);

    // ── Geometry must exist even when hidden ──────────────────────────────
    const ImGuiIO& io = ImGui::GetIO();
    ensureTriggerAreasPlaced(io.DisplaySize.x, io.DisplaySize.y);

    const int64_t now = nowMs();

    // ── Always complete a pending tap-up first ────────────────────────────
    // The up must land even when the page is disabled mid-tap, or the slot
    // stays stuck down in the kernel (a phantom finger).
    if (p.tapDown && now >= p.tapUpAtMs) {
        uinput_up(UINPUT_SLOT_TRIGGER);
        p.tapDown = false;
    }

    auto resetTiming = [&]() {
        p.enterMs = 0;
        p.fired = false;
    };

    // ── Gates: master switch, uinput, model ───────────────────────────────
    if (!p.enabled.value || !uinput_is_ready() || !snap.active) {
        resetTiming();
        return;
    }

    // ── Hold gate: continuous-trigger OR a real finger in the hold circle ─
    const bool holdActive = p.continuousTrigger.value || realFingerInHoldArea();
    if (!holdActive) {
        resetTiming();
        return;
    }

    // ── Manual-fire suppression: a real finger tapping fire itself ────────
    // (Old: `isFingerInFireZone()` early-return.) Timing is deliberately
    // preserved so the auto trigger resumes in rhythm when the finger lifts.
    if (realFingerInTouchArea()) return;

    if (!p.category.hasAnySelected()) {
        resetTiming();
        return;
    }

    // ── Hit test: screen centre inside a selected box ─────────────────────
    // Smoothed tracker tracks first, raw snapshot boxes as fallback so an
    // unconfirmed track never blocks fire.
    const float scx = io.DisplaySize.x * 0.5f;
    const float scy = io.DisplaySize.y * 0.5f;
    bool onTarget = false;

    const auto& tracks = tracking::tracker().tracks();
    for (const auto& t : tracks) {
        if (!t.confirmed || !p.category.isClassSelected(t.cls)) continue;
        const float x1 = t.cx - t.w * 0.5f, x2 = t.cx + t.w * 0.5f;
        const float y1 = t.cy - t.h * 0.5f, y2 = t.cy + t.h * 0.5f;
        if (scx >= x1 && scx <= x2 && scy >= y1 && scy <= y2) {
            onTarget = true;
            break;
        }
    }
    if (!onTarget) {
        for (const auto& b : snap.detections) {
            if (!p.category.isClassSelected(b.cls)) continue;
            if (scx >= b.x1 && scx <= b.x2 && scy >= b.y1 && scy <= b.y2) {
                onTarget = true;
                break;
            }
        }
    }

    if (!onTarget) {
        // Crosshair left the target — reset, exactly like the old project.
        resetTiming();
        return;
    }

    // ── Reaction / cooldown state machine with per-shot jitter ────────────
    auto rollWait = [&](float base) -> int64_t {
        const float up = p.upFluct.value;
        const float dn = p.downFluct.value;
        const float span = up + dn;
        const float jitter = (span > 0.0f) ? (rand01(p) * span - dn) : 0.0f;
        const float wait = base + jitter;
        return static_cast<int64_t>(wait < 10.0f ? 10.0f : wait);
    };

    if (!p.fired) {
        // First shot: wait `reaction` after the crosshair enters the target.
        if (p.enterMs == 0) {
            p.enterMs = now;
            p.currentWaitMs = rollWait(p.reaction.value);
        }
        if (now - p.enterMs < p.currentWaitMs) return;
        p.fired = true;
        p.enterMs = now;
        p.currentWaitMs = rollWait(p.cooldown.value);
    } else {
        if (now - p.enterMs < p.currentWaitMs) return;
        p.enterMs = now;
        p.currentWaitMs = rollWait(p.cooldown.value);
    }

    // ── Fire: random point in the touch box, non-blocking tap ─────────────
    // A tap still held from the previous shot blocks this one; the timestamp
    // was already advanced above, so it fires on the next frame after the up.
    if (p.tapDown) return;

    const TriggerTouchArea& a = p.touchArea;
    const float px = a.x + rand01(p) * a.w;
    const float py = a.y + rand01(p) * a.h;
    uinput_down(UINPUT_SLOT_TRIGGER, UINPUT_ID_TRIGGER,
                static_cast<int>(px), static_cast<int>(py));
    p.tapDown = true;
    const float dur = std::clamp(p.touchDuration.value, 1.0f, 50.0f);
    p.tapUpAtMs = now + static_cast<int64_t>(dur);
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng

#endif  // HAS_IMGUI
