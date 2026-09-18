// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::hud — implementation. See hud.h for the rationale.
//
//  What lives here vs. what moved out:
//
//    * Xf, lerpColor, textCentred — shared by every draw call inside the board,
//      so they live in hud.h and are defined once here.
//    * The rail, the board shell, the show/hide animation, the layout — they
//      are HUD-level concerns and stay in this file.
//    * Each page (Aim / Trigger / Model / Touch / Capture / Settings) now
//      owns its state and its draw function in ui/gui/sections/, so adding a
//      row to a page means touching one file, not two.
//
//  drawContent() is now a thin dispatcher: the right pane is laid out by the
//  active section. Model is the only page that draws an overlay on top, so
//  drawBoard() reaches into sections::drawModelOverlays() when its dialog is
//  up.
// ─────────────────────────────────────────────────────────────────────────────
#include "hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/gui/float_button.h"
#include "ui/gui/notify.h"
#include "config/config_manager.h"
#include "inference/model_runtime.h"
#include "tracking/kalman_tracker.h"
#include "ui/gui/sections/aim_section.h"
#include "ui/gui/sections/backflash_section.h"
#include "ui/gui/sections/capture_section.h"
#include "ui/gui/sections/model_section.h"
#include "ui/gui/sections/settings_section.h"
#include "ui/gui/sections/touch_section.h"
#include "ui/gui/sections/trigger_section.h"

namespace aimbotng {
namespace ui {

// HUD visibility animation state. g_hudVisibleTarget is what the user asked
// for (true = board should be shown); g_hudAnim is the eased 0..1 progress
// the board is currently rendering at. They diverge during the transition
// and converge at the ends. First frame starts at anim = 1 (board visible,
// no fly-in).
//
// Defined at namespace scope (not the anonymous namespace below) so the
// float_button.cpp — which needs to flip g_hudVisibleTarget — can link to it
// through the extern declaration in float_button.h.
bool  g_hudVisibleTarget = true;
float g_hudAlpha         = 1.0f;

}  // namespace ui
}  // namespace aimbotng

#ifdef HAS_IMGUI
// ImGui declares operator new/delete in the global namespace, so it MUST be
// included at file scope rather than from inside our `namespace aimbotng::ui`
// block — otherwise the overloads land in our namespace and break any code
// trying to `new` something unparented.
#include "imgui.h"
#include "theme.h"
#include "widgets.h"

namespace aimbotng {
namespace ui {

// ── lerpColor / textCentred ───────────────────────────────────────────────────
//
// Defined here (and declared in hud.h) so the section files share one
// implementation. They were inside an anonymous namespace before the split —
// every translation unit that needed them rebuilt its own copy.

ImU32 lerpColor(ImU32 a, ImU32 b, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float u = 1.0f - t;
    const int r = (int)(((a >> IM_COL32_R_SHIFT) & 0xFF) * u + ((b >> IM_COL32_R_SHIFT) & 0xFF) * t);
    const int g = (int)(((a >> IM_COL32_G_SHIFT) & 0xFF) * u + ((b >> IM_COL32_G_SHIFT) & 0xFF) * t);
    const int bl = (int)(((a >> IM_COL32_B_SHIFT) & 0xFF) * u + ((b >> IM_COL32_B_SHIFT) & 0xFF) * t);
    const int al = (int)(((a >> IM_COL32_A_SHIFT) & 0xFF) * u + ((b >> IM_COL32_A_SHIFT) & 0xFF) * t);
    return IM_COL32(r, g, bl, al);
}

void textCentred(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
                 float size, ImU32 col, const char* text) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(font, size,
                ImVec2(mn.x + (mx.x - mn.x - ts.x) * 0.5f,
                       mn.y + (mx.y - mn.y - ts.y) * 0.5f),
                col, text);
}

namespace {

// ── Design constants ─────────────────────────────────────────────────────────

// Everything on the board is expressed against a 1200 px tall design, then
// multiplied by hudScale(). Change these numbers and the look follows on every
// screen; nothing below is resolution-dependent.
//
// The width counterpart is the 4:3 board those 1200 px were designed in.
// hudScale() takes the smaller of the two ratios, so a board that is unusually
// tall cannot make the rail's fixed-size title spill out of it.
constexpr float kDesignHeight = 1200.0f;
constexpr float kDesignWidth  = kDesignHeight * 4.0f / 3.0f;  // 1600

// The board keeps 6% of the screen as a margin on each side. Enough that it
// reads as a floating panel rather than a full-bleed page, and it keeps clear
// of the camera cutout and the rounded display corners.
constexpr float kMarginFraction = 0.06f;

// Colours live in theme.h — the board and the controls sitting on it
// (widgets.cpp) both paint from there, so the accent cannot drift apart
// between them.
using namespace theme;

// ── Rail metrics ─────────────────────────────────────────────────────────────
// Design pixels against the 1200 px tall board, multiplied by hudScale().
constexpr float kRailFraction  = 0.20f;  // the rail owns the left 20%
constexpr float kRailPad       = 26.0f;
constexpr float kTitleTop      = 44.0f;  // "Aimbot"
constexpr float kTitleSize     = 46.0f;
constexpr float kSubSize       = 22.0f;  // "Nextgen", right-aligned under it
constexpr float kSubGap        = 10.0f;
constexpr float kDividerGap    = 26.0f;  // header block -> hairline
constexpr float kNavTopGap     = 28.0f;  // hairline -> first button
constexpr float kNavHeight     = 76.0f;
constexpr float kNavSpacing    = 14.0f;
constexpr float kNavRounding   = 16.0f;
constexpr float kNavTextSize   = 26.0f;
// Render FPS readout, parked in the rail's bottom-left corner.
constexpr float kFpsSize       = 21.0f;
// Ease-out rate for the sliding highlight, in 1/s. The step per frame is a
// fraction of the distance that is *left*, so the block keeps slowing down as
// it closes in — the "decelerating" feel, and frame-rate independent.
constexpr float kNavAnimRate   = 19.0f;

// ── Content (right pane) metrics ─────────────────────────────────────────────
constexpr float kContentPad     = 44.0f;  // pane edge -> content
constexpr float kContentTop     = 38.0f;  // pane top -> page title
constexpr float kPageTitleSize  = 36.0f;
constexpr float kTitleRuleGap   = 24.0f;  // title -> hairline
constexpr float kFirstRowGap    = 30.0f;  // hairline -> first control

// The rail's labels, in MenuSection order.
const char* kNavLabels[] = {"自瞄", "扳机", "背闪", "模型", "触摸", "截图", "设置"};
static_assert(sizeof(kNavLabels) / sizeof(kNavLabels[0]) ==
                  static_cast<size_t>(MenuSection::Count),
              "kNavLabels must have one entry per MenuSection");

constexpr float kCornerRadiusDesign = 20.0f;

// Appearance animation: scale + alpha the board eases between when the float
// button toggles it. The scale grows from 70 % of the layout size to the full
// 100 % as the board appears, and runs the same path backwards as it hides;
// alpha mirrors it 0 → 1 → 0. Both are driven from a single 0..1 progress
// value held in g_hudAnim and updated every frame from g_hudVisibleTarget —
// see drawHud().
//
// Why both knobs instead of just alpha: a pure alpha fade looks like the board
// is "appearing out of thin air", whereas a scale-from-70 % read as "the board
// pops out toward you". On a 120 Hz panel the difference is obvious.
constexpr float kHudScaleMin    = 0.70f;          // scale when anim == 0
constexpr float kHudEaseRate    = 18.0f;          // 1/s, dt-based ease-out

// ── State ────────────────────────────────────────────────────────────────────
// Written and read on the render thread only (drawHud → updateRegions), so no
// lock is needed.
HudRect g_rect;
float   g_scale = 1.0f;
MenuSection g_section = MenuSection::Aim;
// Top edge of the sliding highlight, in surface pixels. Negative until the
// first frame places it, so the very first paint does not fly in from y=0.
float   g_highlightY = -1.0f;

// Per-page vertical scroll offset (surface px) for the right-pane sections, so a
// page whose rows overflow the board can be dragged like a native scroll view.
// One slot per MenuSection; survives page switches (each page keeps its place).
float g_scroll[static_cast<int>(MenuSection::Count)] = {0};
bool  g_scrollDrag       = false;  // a scroll drag is in progress
float g_scrollDragStartY = 0.0f;  // mouse.y when the drag began
float g_scrollDragStart  = 0.0f;  // g_scroll value when the drag began
/// Pixels of movement before a press becomes a scroll drag. Anything shorter
/// is treated as a tap and stays available to whatever widget is under the
/// finger. Below this distance the offset does not move, so a tap on an empty
/// strip does not nudge the page.
constexpr float kScrollSlop = 8.0f;

// HUD visibility animation state. g_hudVisibleTarget is what the user asked
// for (true = board should be shown); g_hudAnim is the eased 0..1 progress
// the board is currently rendering at. They diverge during the transition
// and converge at the ends. First frame starts at anim = 1 (board visible,
// no fly-in).
//
// g_hudVisibleTarget lives at namespace scope (see top of file) so the float
// button can flip it without depending on the rest of this translation unit.
float g_hudAnim = 1.0f;

bool inside(const ImVec2& p, const ImVec2& mn, const ImVec2& mx) {
    return p.x >= mn.x && p.x < mx.x && p.y >= mn.y && p.y < mx.y;
}

/**
 * The left rail: header block, hairline, and the section buttons.
 *
 * Hit testing is done by hand against the rectangles we just computed rather
 * than through ImGui's widget system — the rail is draw-list geometry, not a
 * window, so there is no widget for ImGui to test for us. A tap switches the
 * section on press; the highlight then *slides* to the new row.
 */
void drawRail(ImDrawList* dl, const HudRect& r, float s, float railW, const Xf& xf) {
    const ImVec2 railMin(r.x, r.y);
    const ImVec2 railMax(r.x + railW, r.y + r.h);
    const float rounding = kCornerRadiusDesign * s;

    dl->AddRectFilled(xf.pt(railMin), xf.pt(railMax),
                      xf.col(PaneRail), xf.s(rounding), ImDrawFlags_RoundCornersLeft);

    // ── Header ───────────────────────────────────────────────────────────────
    ImFont* font = ImGui::GetFont();
    const float padX = kRailPad * s;

    const float titleY = r.y + kTitleTop * s;
    dl->AddText(font, kTitleSize * s,
                xf.pt(ImVec2(railMin.x + padX, titleY)),
                xf.col(TextPrimary), "Aimbot");

    // "Nextgen" sits under the title, flush with the rail's right padding.
    const float subY = titleY + kTitleSize * s + kSubGap * s;
    const ImVec2 subSize = font->CalcTextSizeA(kSubSize * s, FLT_MAX, 0.0f, "Nextgen");
    dl->AddText(font, kSubSize * s,
                xf.pt(ImVec2(railMax.x - padX - subSize.x, subY)),
                xf.col(TextMuted), "Nextgen");

    // ── Hairline under the header ────────────────────────────────────────────
    const float dividerY = subY + subSize.y + kDividerGap * s;
    dl->AddLine(xf.pt(ImVec2(railMin.x + padX, dividerY)),
                xf.pt(ImVec2(railMax.x - padX, dividerY)),
                xf.col(Divider), xf.s(1.0f * s));

    // ── Section buttons ──────────────────────────────────────────────────────
    // Hit testing stays in the unscaled coordinate space: r is the visual rect,
    // so a tap that lands inside a section button has coordinates that match
    // what ImGui's IO is reporting (mouse positions are also in surface pixels
    // and the cursor tracks the finger, which is unaffected by the board scale).
    const ImGuiIO& io = ImGui::GetIO();

    const float rowH = kNavHeight * s;
    const float rowStep = rowH + kNavSpacing * s;
    const float firstY = dividerY + kNavTopGap * s;
    const int   count = static_cast<int>(MenuSection::Count);

    // Selection first: a tap only decides *which* row is active. Everything
    // below then animates towards that decision.
    //
    // Click-activated like every other button (press began inside the row AND
    // release lands inside it), and dead while a Model dialog owns the board:
    // a tap outside the dialog card is the dialog's scrim-dismiss, never a
    // page switch.
    const bool dialogOpen = sections::anyDialogOpen();
    for (int i = 0; i < count; ++i) {
        const ImVec2 mn(railMin.x + padX, firstY + i * rowStep);
        const ImVec2 mx(railMax.x - padX, mn.y + rowH);
        if (!dialogOpen &&
            widgets::clicked(widgets::Rect{mn.x, mn.y, mx.x - mn.x, mx.y - mn.y})) {
            g_section = static_cast<MenuSection>(i);
        }
    }

    // The highlight glides rather than jumps. Each frame it covers a fixed
    // fraction of the distance still *remaining*, so it moves less and less as
    // it closes in — an exponential ease-out, and because the fraction is
    // scaled by the frame time it decelerates identically at any frame rate.
    //
    // A property of that curve is that it only ever approaches the target, so
    // the glide would otherwise creep on forever. Below a pixel we snap: the
    // difference is invisible, and it gives the animation a definite end.
    const float targetY = firstY + static_cast<int>(g_section) * rowStep;
    if (g_highlightY < 0.0f) {
        g_highlightY = targetY;  // first frame — place it, do not fly in
    } else {
        const float dy = targetY - g_highlightY;
        if (fabsf(dy) <= 1.0f) {
            g_highlightY = targetY;  // < 1 px: invisible, so just arrive
        } else {
            float dt = io.DeltaTime;
            if (!(dt > 0.0f)) dt = 1.0f / 60.0f;  // guard a stalled/paused frame
            g_highlightY += dy * (1.0f - expf(-kNavAnimRate * dt));
        }
    }

    const ImVec2 hlMin(railMin.x + padX, g_highlightY);
    const ImVec2 hlMax(railMax.x - padX, g_highlightY + rowH);
    dl->AddRectFilled(xf.pt(hlMin), xf.pt(hlMax),
                      xf.col(Accent), xf.s(kNavRounding * s));

    // Labels light up as the block arrives rather than snapping with it: each
    // one blends from muted to white by how close the highlight's centre is.
    // Rows the block has left fade back out on their own.
    const float hlCentre = g_highlightY + rowH * 0.5f;
    for (int i = 0; i < count; ++i) {
        const ImVec2 mn(railMin.x + padX, firstY + i * rowStep);
        const ImVec2 mx(railMax.x - padX, mn.y + rowH);
        const float rowCentre = mn.y + rowH * 0.5f;
        const float lit = 1.0f - fminf(1.0f, fabsf(hlCentre - rowCentre) / rowH);
        textCentred(dl, xf.pt(mn), xf.pt(mx), xf.s(kNavTextSize * s),
                    xf.col(lerpColor(TextMuted, TextOnAccent, lit)), kNavLabels[i]);
    }

    // ── Render FPS, bottom-left corner ───────────────────────────────────────
    // io.Framerate is ImGui's own smoothed 1/DeltaTime — i.e. the rate this
    // render loop is actually presenting at, not the panel's refresh rate.
    // Gated by the FPS 叠加 switch on the Settings page — the rail is the
    // only place it could possibly live (it's the only surface that is
    // guaranteed to be on screen when a debug reading matters).
    if (sections::g_pageSettings.fpsOverlay.value) {
        char fps[32];
        snprintf(fps, sizeof(fps), "%.0f FPS", io.Framerate);
        const float fpsSize = kFpsSize * s;
        const ImVec2 fpsExtent = font->CalcTextSizeA(fpsSize, FLT_MAX, 0.0f, fps);
        dl->AddText(font, fpsSize,
                    xf.pt(ImVec2(railMin.x + padX, railMax.y - kRailPad * s - fpsExtent.y)),
                    xf.col(TextMuted), fps);
    }
}

/**
 * The page's master on/off switch, or null for a page that has none.
 *
 * Aim, Trigger and Capture are whole features that are simply on or off.
 * Rather than spend the first row of the page on a switch labelled "Enabled",
 * their toggle rides on the header line next to the page title.
 */
widgets::SwitchState* masterSwitch(MenuSection section) {
    switch (section) {
        case MenuSection::Aim:       return &sections::g_pageAim.enabled;
        case MenuSection::Trigger:   return &sections::g_pageTrigger.enabled;
        case MenuSection::BackFlash: return &sections::g_pageBackFlash.enabled;
        case MenuSection::Capture:   return &sections::g_pageCapture.enabled;
        case MenuSection::Model:     return &sections::g_pageModel.enabled;
        default:                     return nullptr;
    }
}

/**
 * The right pane: the active section's title, then its controls.
 *
 * The actual control layout lives in each section file (drawXxxSection()). This
 * function only sets up the header, the master switch and the soft bottom,
 * then hands the cursor `y` to whatever section is active.
 *
 * Everything drawn here is clipped to the board, so a dropdown opening towards
 * the bottom edge is cut off by the panel rather than spilling onto the desktop
 * behind it.
 */
void drawContent(ImDrawList* dl, const HudRect& r, float s, float railW, const Xf& xf) {
    // The widget layer needs the *combined* scale (layout × appearance) so its
    // internals — switch tracks, slider grooves, dropdown rows — draw at the
    // same physical size the layout scale was supposed to produce. Without
    // this they would render at layout size and only the outer rect would
    // shrink, leaving mismatched-looking controls.
    const float es = xf.effective_s(s);
    const float pad = kContentPad * s;
    const float x   = r.x + railW + pad;
    const float w   = r.w - railW - pad * 2.0f;
    if (w <= 1.0f) return;

    dl->PushClipRect(xf.pt(ImVec2(r.x, r.y)), xf.pt(ImVec2(r.x + r.w, r.y + r.h)), true);

    // ── Header ───────────────────────────────────────────────────────────────
    ImFont* font = ImGui::GetFont();
    const float titleY    = r.y + kContentTop * s;
    const float titleSize = kPageTitleSize * s;

    // The switch geometry is worked out before the title is painted because the
    // title line's readout is laid out against the switch, and computing it
    // twice is how the two end up disagreeing by a padding constant.
    const float trackW    = widgets::kSwitchTrackW * s;
    const float trackH    = widgets::kSwitchTrackH * s;
    const float padTouch  = 14.0f * s;
    widgets::SwitchState* master    = masterSwitch(g_section);
    const float switchLeft = x + w - trackW - padTouch;

    dl->AddText(font, titleSize, xf.pt(ImVec2(x, titleY)), xf.col(TextPrimary),
                kNavLabels[static_cast<int>(g_section)]);

    // A page that is just on or off wears its master switch on the header line
    // instead of spending a row on it: right-aligned exactly where that row's
    // toggle used to sit, only raised onto the title's centreline. The touch
    // rectangle is padded a little past the groove, because a finger is bigger
    // than the control it is aiming at.
    if (master != nullptr) {
        // Positions go through pt(), sizes through s(). Passing an absolute
        // coordinate to s() scales it about the surface origin, which happens to
        // look right until the board animates — then the switch slides sideways
        // while everything around it scales about the centre.
        const ImVec2 swPos = xf.pt(ImVec2(
            switchLeft, titleY + (titleSize - trackH) * 0.5f - padTouch * 0.5f));

        // The Model page's master switch owns "is inference running at all",
        // and that is the Settings page's "持续推理" switch's decision when
        // continuous mode is on. In triggered mode (continuous off) the
        // inference-area circle is the one that arms/disarms, so letting the
        // user flip the Model switch here would silently fight the trigger:
        // the user would think they are turning it on, and the next finger-out
        // would yank it back. Greying the row and swallowing taps keeps the
        // surface honest — there is exactly one knob in each mode, and which
        // knob it is changes with the Settings switch.
        bool modelSwitchLive = !sections::anyDialogOpen();
        if (g_section == MenuSection::Model) {
            const bool continuous = sections::g_pageSettings.continuousInference.value;
            modelSwitchLive = modelSwitchLive && continuous;
            // Force the bit off if the user landed here with continuous off
            // (typical: opened the Model page first, then visited Settings and
            // flipped it). The previous state lingers in the SwitchState's
            // animation ease, but the logical value is what syncModelPage()
            // reads — leaving it on would have the runtime running while the
            // user cannot reach the off switch.
            if (!continuous) sections::g_pageModel.enabled.value = false;
        }

        widgets::switchToggle(dl,
            {swPos.x, swPos.y, xf.s(trackW + padTouch), xf.s(trackH + padTouch)},
            *master, es, modelSwitchLive);
    }

    // What that switch is costing, on the same line. It belongs here rather than
    // in the page body: it is only meaningful while the switch is on, it changes
    // every frame, and a row that changes every frame is a row nobody reads.
    if (g_section == MenuSection::Model) {
        const float roSize = kPageTitleSize * 0.55f * s;
        const float gap    = 22.0f * s;
        // Available width between the title and the switch, less a gutter so the
        // two never touch even at the largest menu scale.
        const float avail  = switchLeft - (x + kPageTitleSize * s * 3.2f) - gap;

        std::string readout = infer::runtime::timingTextDetailed();
        float roW = font->CalcTextSizeA(roSize, FLT_MAX, 0.0f, readout.c_str()).x;
        if (roW > avail) {
            // Falls back to the total, which is the part that answers the
            // question the line exists to answer.
            readout = infer::runtime::timingText();
            roW = font->CalcTextSizeA(roSize, FLT_MAX, 0.0f, readout.c_str()).x;
        }

        if (roW <= avail) {
            const float roX = switchLeft - gap - roW;
            const float roY = titleY + (titleSize - roSize) * 0.5f;
            dl->AddText(font, roSize, xf.pt(ImVec2(roX, roY)), xf.col(TextMuted),
                        readout.c_str());
        }
    }

    // The pages publish their switches in drawHud(), before the board is drawn:
    // see the note there for why they are not done next to their controls.

    const float ruleY = titleY + titleSize + kTitleRuleGap * s;
    dl->AddLine(xf.pt(ImVec2(x, ruleY)), xf.pt(ImVec2(x + w, ruleY)),
                xf.col(Divider), xf.s(1.0f * s));

    // ── Re-clip below the hairline ──────────────────────────────────────────
    // The outer clip is the full board, so the title + hairline above this
    // point stay visible. Sections, however, draw at `y - sc.offset`: a deep
    // scroll would push the top rows' screenY above the hairline and the outer
    // clip would let them paint right over the title. Pop the outer clip and
    // push a narrower one that starts at the hairline, so anything scrolled
    // above it is cut off instead of overpainting. The bottom still matches
    // the board so a dropdown opening downward is still cut off by the panel.
    // Applied to every page — the dispatch is the same path for all of them,
    // and Capture's offset is always 0 so it is unaffected.
    dl->PopClipRect();
    dl->PushClipRect(xf.pt(ImVec2(r.x, ruleY)),
                     xf.pt(ImVec2(r.x + r.w, r.y + r.h)),
                     true);

    // ── Dispatch to the active section ───────────────────────────────────────
    // The cursor `y` starts just under the hairline; each section's draw
    // function advances it past every row it owns. The capture section uses
    // `bottomY` to size the preview well against the panel's bottom edge.
    float y = ruleY + kFirstRowGap * s;
    const float bottomY = r.y + r.h - pad;

    // ── Per-page vertical scroll ─────────────────────────────────────────────
    // The active section draws into a virtual content track whose top-left is
    // shifted up by `sc.offset`. The clip is the board, so anything scrolled
    // past the top/bottom is just hidden — same look as a native scroll view.
    // We clamp the offset to the page's actual content height, which the
    // section reports back by leaving `y` in natural (un-scrolled) coordinates.
    const int secIdx = static_cast<int>(g_section);
    const float viewH    = bottomY - (ruleY + kFirstRowGap * s);
    const widgets::Rect scrollRect{x, ruleY + kFirstRowGap * s, w, viewH};

    // Scroll gesture detection runs BEFORE the section, so it can claim the
    // gesture on the same frame a press-down crosses into scroll-drag
    // territory — widgets then see a consumed gesture and stay inert.
    {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;
        // Arm the drag on press-down inside the scrollable area. Stays "armed
        // but dormant" until the finger travels past kScrollSlop — a small tap
        // stays available to whatever control is underneath. Skip if a widget
        // is already dragging the finger: the slider owns the gesture for as
        // long as it's tracking the thumb, and the page must stay still.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !widgets::gestureConsumed() &&
            !widgets::widgetDraggingActive() &&
            inside(mouse,
                   ImVec2(scrollRect.x, scrollRect.y),
                   ImVec2(scrollRect.x + scrollRect.w, scrollRect.y + scrollRect.h))) {
            g_scrollDrag = true;
            g_scrollDragStartY = mouse.y;
            g_scrollDragStart  = g_scroll[secIdx];
        }
        // Release ends the drag. Only consume the gesture if the finger
        // actually travelled past the slop — a short tap that armed the drag
        // but never moved must still reach the widget under the finger (a
        // tap on a closed multi-select row to open it, for example).
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && g_scrollDrag) {
            const float dy = mouse.y - g_scrollDragStartY;
            if (fabsf(dy) > kScrollSlop) widgets::consumeGesture();
            g_scrollDrag = false;
            // The page owned the finger for this drag — clear any widget-drag
            // latch that some widget might have set on the same press frame
            // (defensive: slider should not grab a drag whose press-down
            // started in empty space, but a stale flag is cheap to nuke).
            widgets::clearWidgetDragging();
        }
    }

    Scroll sc;
    sc.offset = g_scroll[secIdx];

    // While a scroll drag is in flight past the slop, claim the gesture BEFORE
    // the section runs — otherwise a slider/switch the finger is "passing over"
    // while dragging the page would still fire. The slop check matches the
    // one below, so a near-press that has not yet moved does NOT pre-empt
    // widgets; the user can still tap a control whose hit area sits in the
    // scrollable region.
    if (g_scrollDrag) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float dy = mouse.y - g_scrollDragStartY;
        if (fabsf(dy) > kScrollSlop) widgets::consumeGesture();
    }

    switch (g_section) {
        case MenuSection::Aim:
            sections::drawAimSection(dl, x, y, w, bottomY, s, es, xf, sc);
            break;
        case MenuSection::Trigger:
            sections::drawTriggerSection(dl, x, y, w, bottomY, s, es, xf, sc);
            break;
        case MenuSection::BackFlash:
            sections::drawBackFlashSection(dl, x, y, w, bottomY, s, es, xf, sc);
            break;
        case MenuSection::Model:
            sections::drawModelSection(dl, x, y, w, bottomY, s, es, xf, sc);
            break;
        case MenuSection::Touch:
            sections::drawTouchSection(dl, x, y, w, bottomY, s, es, xf, sc);
            break;
        case MenuSection::Capture:
            sections::drawCaptureSection(dl, x, y, w, bottomY, s, es, xf, sc);
            break;
        case MenuSection::Settings:
            sections::drawSettingsSection(dl, x, y, w, bottomY, s, es, xf, sc);
            break;
        default:
            break;
    }

    // ── Scroll clamp + commit the drag offset ────────────────────────────────
    // `y` is now the page's natural bottom (header offset + every row's
    // height + every gap). The scrollable range is the excess over the
    // viewport. Once a drag is in flight we move the offset to follow the
    // finger until slop is exceeded, then consume the gesture.
    const float contentH = y - (ruleY + kFirstRowGap * s);
    sc.maxOffset = (contentH > viewH) ? (contentH - viewH) : 0.0f;

    if (sectionIsScrollable(g_section) && sc.active()) {
        if (sc.offset > sc.maxOffset) sc.offset = sc.maxOffset;
        if (sc.offset < 0.0f) sc.offset = 0.0f;

        if (g_scrollDrag) {
            const ImGuiIO& io = ImGui::GetIO();
            const ImVec2 mouse = io.MousePos;
            const float dy = mouse.y - g_scrollDragStartY;
            if (fabsf(dy) > kScrollSlop) {
                sc.offset = g_scrollDragStart - dy;
                if (sc.offset < 0.0f) sc.offset = 0.0f;
                if (sc.offset > sc.maxOffset) sc.offset = sc.maxOffset;
                widgets::consumeGesture();
            }
        }
        g_scroll[secIdx] = sc.offset;
    } else {
        // Page does not (currently) need scrolling; park the offset so a
        // future overflow does not start mid-scroll, and make sure no in-flight
        // drag from a different page bleeds across.
        g_scroll[secIdx] = 0.0f;
        g_scrollDrag = false;
    }

    // Drops the narrow "below the hairline" clip pushed after the header.
    // The outer full-board clip was already popped just before the section
    // dispatch — this one matches that inner push.
    dl->PopClipRect();
}

void drawBoard(ImDrawList* dl, const HudRect& r, float s, const Xf& xf) {
    const ImVec2 min(r.x, r.y);
    const ImVec2 max(r.x + r.w, r.y + r.h);
    const float  rounding = kCornerRadiusDesign * s;
    const float  railW = r.w * kRailFraction;

    dl->AddRectFilled(xf.pt(min), xf.pt(max), xf.col(PaneRight), xf.s(rounding));
    drawRail(dl, r, s, railW, xf);
    drawContent(dl, r, s, railW, xf);

    // The Model page is the only one that draws on top of the right pane —
    // the Add-Model dialog (and the file browser sitting on top of it).
    // reach for it from here so the rail does not have to know it exists.
    sections::drawModelOverlays(dl, s, xf.effective_s(s), xf);
}

}  // namespace
}  // namespace ui
}  // namespace aimbotng
#endif

namespace aimbotng {
namespace ui {

namespace {

using namespace theme;

/**
 * Paints the detector's boxes straight onto the surface.
 *
 * No Xf and no board coordinates. These rectangles describe the screen, not the
 * menu: the board's scale-and-fade animation must not touch them, and they must
 * survive the board being hidden — which is the moment they are most useful,
 * because a person who is aiming is not looking at a menu.
 *
 * A label rides on every box. A bare rectangle cannot answer the one question
 * anyone looking at it has, which is what the model thought it found and how
 * sure it was; and a box with a low score and a box with a high one look
 * identical otherwise.
 */
void drawDetectionOverlay() {
    if (!sections::g_pageSettings.showDetections.value) return;

    const infer::runtime::Snapshot snap = infer::runtime::snapshot();
    if (!snap.active) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    constexpr float kLabelSize = 21.0f;
    constexpr float kRound     = 3.0f;
    constexpr float kPadX      = 6.0f;
    constexpr float kPadY      = 3.0f;

    for (const infer::Box& b : snap.detections) {
        const ImVec2 mn(b.x1, b.y1);
        const ImVec2 mx(b.x2, b.y2);
        dl->AddRect(mn, mx, DetectBox, kRound, 0, 2.0f);

        char label[96];
        const char* name = (b.cls >= 0 &&
                            static_cast<size_t>(b.cls) < snap.classes.size())
                               ? snap.classes[static_cast<size_t>(b.cls)].c_str()
                               : nullptr;
        if (name != nullptr) {
            snprintf(label, sizeof(label), "%s %.2f", name, b.score);
        } else {
            snprintf(label, sizeof(label), "#%d %.2f", b.cls, b.score);
        }

        const ImVec2 ts = font->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, label);
        // Above the box by preference, flipped inside it when the box is at the
        // top of the screen and there is nothing above to sit on.
        ImVec2 lmn(mn.x, mn.y - ts.y - kPadY * 2.0f);
        if (lmn.y < 0.0f) lmn.y = mn.y + 1.0f;
        const ImVec2 lmx(lmn.x + ts.x + kPadX * 2.0f, lmn.y + ts.y + kPadY * 2.0f);

        dl->AddRectFilled(lmn, lmx, DetectPlate, kRound);
        dl->AddText(font, kLabelSize, ImVec2(lmn.x + kPadX, lmn.y + kPadY),
                    DetectText, label);
    }
}

// ── drawTrackingOverlay ────────────────────────────────────────────────────
//
// Blue boxes around the Kalman tracker's smoothed tracks. Distinct from the
// red raw detection overlay (drawDetectionOverlay above) so the two can be
// toggled independently:
//   * showDetections off, showTracking on  -> only the smoothed track.
//   * showDetections on,  showTracking off -> only the raw detector output.
//   * both on                              -> both, useful for checking
//                                             "does the box actually settle?"
//
// The label is just the track id. No score, no class — the point of this view
// is to watch identity (does the id stay on one enemy?) and smoothing, not the
// model's per-box confidence.
void drawTrackingOverlay() {
    if (!sections::g_pageSettings.showTracking.value) return;

    const auto& tracks = tracking::tracker().tracks();
    if (tracks.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    constexpr float kLabelSize = 21.0f;
    constexpr float kRound     = 3.0f;
    constexpr float kPadX      = 6.0f;
    constexpr float kPadY      = 3.0f;

    for (const tracking::TrackedTarget& t : tracks) {
        if (!t.confirmed) continue;       // only show "real" tracks
        if (t.w <= 0.0f || t.h <= 0.0f) continue;

        const float halfW = t.w * 0.5f;
        const float halfH = t.h * 0.5f;
        const ImVec2 mn(t.cx - halfW, t.cy - halfH);
        const ImVec2 mx(t.cx + halfW, t.cy + halfH);
        dl->AddRect(mn, mx, TrackBox, kRound, 0, 2.0f);

        char label[32];
        snprintf(label, sizeof(label), "#%d", t.id);

        const ImVec2 ts = font->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, label);
        ImVec2 lmn(mn.x, mn.y - ts.y - kPadY * 2.0f);
        if (lmn.y < 0.0f) lmn.y = mn.y + 1.0f;
        const ImVec2 lmx(lmn.x + ts.x + kPadX * 2.0f, lmn.y + ts.y + kPadY * 2.0f);

        dl->AddRectFilled(lmn, lmx, TrackPlate, kRound);
        dl->AddText(font, kLabelSize, ImVec2(lmn.x + kPadX, lmn.y + kPadY),
                    TrackText, label);
    }
}

}  // namespace

HudRect hudRect() { return g_rect; }
float   hudScale() { return g_scale; }
MenuSection currentSection() { return g_section; }

bool hudSettled() {
    const float target = g_hudVisibleTarget ? 1.0f : 0.0f;
    return fabsf(target - g_hudAnim) < 0.002f;
}

bool hudHidden() { return g_hudAnim <= 0.001f && !g_hudVisibleTarget; }

void drawHud() {
#ifdef HAS_IMGUI
    ImGuiIO& io = ImGui::GetIO();
    const float dw = io.DisplaySize.x;
    const float dh = io.DisplaySize.y;
    if (dw <= 1.0f || dh <= 1.0f) return;  // no surface yet

    // Reset the per-frame "gesture consumed" flag before any widget is drawn,
    // so a tap cannot leak through to a control underneath it this frame.
    widgets::beginFrame();

    // Detections first, so the board paints over them: a box belongs to the
    // screen behind the menu, not on top of it. Drawn before the hidden-case
    // early return below, which is the whole point — the menu being out of the
    // way is when someone is most likely to be looking at what the model found.
    drawDetectionOverlay();
    drawTrackingOverlay();

    // Aim overlays (touch-area dashed box + trigger-area circle). Same layer
    // and same reasoning as detection overlay: drawn over the game, under the
    // board, and survives the board being hidden so a configured region stays
    // visible after the menu is dismissed.
    sections::drawAimOverlays();
    // Trigger overlays (fire box + hold circle). Independent geometry from the
    // Aim page — same layer, same lifetime rules.
    sections::drawTriggerOverlays();
    // Inference-area circle (from Settings page). Same layer, shown only when
    // continuous inference is OFF and the area overlay toggle is on.
    sections::drawInferenceAreaOverlay();
    // Back-flash overlay (touch-area dashed box). Same pattern as the Aim and
    // Trigger overlays: independent geometry, same layer rules. Drawn here
    // (before the board) so the menu still sits above the dashed box when
    // the menu is open, and so the user can see where the swipe operates
    // after the menu is dismissed.
    sections::drawBackFlashOverlays();

    // The pages' switches are published here rather than from inside the board,
    // because they describe *system* state and not a rendering concern. While
    // these lived in drawContent() they were skipped on every frame the board
    // was hidden — so a capture switch left on kept running by accident, and a
    // model marked as loaded while the menu was out of the way would not take
    // effect until it came back. Both are idempotent and act only on a change,
    // which is what keeps a model load off the per-frame path.
    //
    // syncBackFlashPage() runs FIRST so its `blockingAim=true` is in effect
    // before syncAimPage() reads it on the gate path. Aim and back-flash share
    // UINPUT_SLOT_PRIMARY, so the order matters: if aim ran first it would
    // press a finger this frame and back-flash would clobber it the next.
    // Running back-flash first lets aim see the flag and skip its press.
    sections::syncBackFlashPage();
    sections::syncAimPage();
    // After syncAimPage (which keeps the Kalman tracker warm) so the trigger
    // reads smoothed tracks from this very frame.
    sections::syncTriggerPage();
    sections::syncCapturePage();
    sections::syncModelPage();
    sections::syncSettingsPage();
    sections::syncTouchPage();

    // Persist page parameters (throttled + only-on-change inside).
    config::pollAutosave();

    // ── Advance the show/hide animation ────────────────────────────────────
    // g_hudVisibleTarget is the user's intent (true = board should be shown);
    // g_hudAnim is the eased 0..1 progress the board is currently rendered
    // at. dt-based ease-out (the same curve the rail highlight and the slider
    // thumb use) decelerates into the endpoint instead of snapping.
    const float target = g_hudVisibleTarget ? 1.0f : 0.0f;
    const float dy = target - g_hudAnim;
    if (fabsf(dy) > 0.0005f) {
        float dt = io.DeltaTime;
        if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
        g_hudAnim += dy * (1.0f - expf(-kHudEaseRate * dt));
    } else {
        g_hudAnim = target;
    }

    // Fully hidden (and not on the way back up): publish no menu rect and
    // skip the board draw entirely. The float button still draws below.
    if (g_hudAnim <= 0.001f && !g_hudVisibleTarget) {
        g_rect = HudRect{};
        g_hudAlpha = 0.0f;
        drawFloatButton();
        // Toasts are drawn on the hidden path too, and deliberately: the moment
        // a person most needs to be told "the graph is compiling" or "there is
        // no frame source" is when they are holding a finger on the inference
        // area with the board out of the way. They are non-modal and take no
        // touch, so showing them here costs nothing.
        notify::draw();
        return;
    }

    // ── Layout ──────────────────────────────────────────────────────────────
    // The board takes the whole margin box, but its shape is bounded: at most
    // 4:3 wide, at most 3:4 tall.
    //
    // A landscape panel is wider than 4:3, so the bound bites and the board
    // stays the floating panel it has always been. A portrait panel is taller
    // than 3:4, and gets a portrait board to match — it used to get the 4:3
    // landscape slab centred in the middle of it, which reads as "a landscape
    // menu squashed onto an upright screen".
    constexpr float kMaxAspect = 4.0f / 3.0f;  // widest the board may be
    constexpr float kMinAspect = 3.0f / 4.0f;  // tallest the board may be

    const float availW = dw * (1.0f - 2.0f * kMarginFraction);
    const float availH = dh * (1.0f - 2.0f * kMarginFraction);

    float w = availW;
    float h = availH;
    const float aspect = w / h;
    if (aspect > kMaxAspect) {
        w = h * kMaxAspect;
    } else if (aspect < kMinAspect) {
        h = w / kMinAspect;
    }

    g_rect.x = (dw - w) * 0.5f;  // centred
    g_rect.y = (dh - h) * 0.5f;
    g_rect.w = w;
    g_rect.h = h;
    // Scale on whichever design ratio is tighter, so every metric below keeps
    // its proportion to the board in both orientations.
    g_scale  = fminf(h / kDesignHeight, w / kDesignWidth);

    // ── Build the appearance transform ──────────────────────────────────────
    // Scale grows from kHudScaleMin to 1.0 as anim goes 0→1; alpha follows the
    // same anim. The centre stays fixed (the board's own centre), so the
    // scale grows toward and away from the middle instead of from a corner —
    // the "the panel pops out toward you" feel the user asked for.
    Xf xf;
    xf.scale  = kHudScaleMin + (1.0f - kHudScaleMin) * g_hudAnim;
    xf.alpha  = g_hudAnim;
    xf.center = ImVec2(g_rect.x + g_rect.w * 0.5f, g_rect.y + g_rect.h * 0.5f);
    g_hudAlpha = g_hudAnim;  // widgets read this and fade themselves.

    drawBoard(ImGui::GetForegroundDrawList(), g_rect, g_scale, xf);

    // Toasts last of all: they belong to the layer rather than to any page, and
    // a status line that the board can cover is a status line the user has to
    // hunt for. Drawn above the float button too, because the button sits in a
    // corner and a compiling notice must not be hidden behind it.
    notify::draw();

    // The float button is the always-on toggle that opens and closes this
    // board. Draw it last so it sits above every other piece of UI the menu
    // paints, and draw it even when the board itself is hidden (handled above)
    // so closing the menu never closes the only way back into it.
    drawFloatButton();
#endif
}

}  // namespace ui
}  // namespace aimbotng
