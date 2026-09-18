// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::hud — the menu board, drawn straight onto an ImDrawList
//
//  Why not ImGui windows? ImGui's window system brings a whole apparatus with
//  it — a window stack, an ID stack, theming, docking, ini persistence, decor-
//  ation, scrolling. An in-game overlay wants none of that: it wants a board
//  pinned at a known place, pixels it fully controls, and hit testing it writes
//  itself. So the board is drawn imperatively with ImDrawList primitives, on
//  top of everything, and every widget added later is just another
//  AddRectFilled/AddText plus our own test against the finger position.
//
//  Geometry: the board holds a strict 4:3 aspect ratio and is derived from the
//  surface resolution every frame, so it lands the same way on a 1080p panel
//  and on the 2K panel of this device — no hardcoded pixel sizes, no density
//  lookups. Content sizes itself through hudScale(), the same way.
//
//  Colour: two greys — a darker content pane on the right, a lighter rail down
//  the left — behind a blue accent for the active rail entry. The swapchain is
//  cleared to transparent black, so whatever the board does not paint stays
//  see-through and the game below remains visible around it.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#ifdef HAS_IMGUI
#include <imgui.h>
#endif

namespace aimbotng {
namespace ui {

/// An axis-aligned rectangle in surface pixels — the same space onTouch()
/// receives and getInteractiveRegions() publishes.
struct HudRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool valid() const { return w > 0.0f && h > 0.0f; }
};

#ifdef HAS_IMGUI
/**
 * Per-frame appearance transform for the HUD.
 *
 * Every draw function inside the board takes one of these and applies it to
 * every pixel it writes: coordinates are scaled around `center`, sizes scale
 * with `scale`, and colours' alpha is multiplied by `alpha`. Together that
 * produces the show/hide animation (alpha 0→1, scale kHudScaleMin→1) without
 * any draw call having to know it is part of an animation.
 *
 * Lives in hud.h so each section translation unit (aim / trigger / ...) can
 * use the same helper without rebuilding it locally.
 */
struct Xf {
    float   scale;    // [kHudScaleMin, 1.0]
    float   alpha;    // [0, 1]
    ImVec2  center;   // origin for the scale — board centre in surface pixels

    ImVec2 pt(float x, float y) const {
        return ImVec2((x - center.x) * scale + center.x,
                      (y - center.y) * scale + center.y);
    }
    ImVec2 pt(const ImVec2& p) const { return pt(p.x, p.y); }
    float  s(float v) const { return v * scale; }
    /// Multiplies the layout scale by the appearance scale, so widget code
    /// that already takes a `s` parameter keeps working unchanged: passing
    /// `xf.effective_s(s)` instead of `s` makes every internal `* s` come
    /// out the right physical size for the current animation frame.
    float  effective_s(float s) const { return s * scale; }
    /// Multiplies the colour's *own* alpha by the fade. Overwriting it turned
    /// every translucent paint opaque — the dialog scrim (black @ 180) became a
    /// solid black sheet no card could be told apart from, and every hairline
    /// border (white @ 40) became a hard white outline.
    ImU32  col(ImU32 c) const {
        const uint32_t src = (c >> IM_COL32_A_SHIFT) & 0xFFu;
        const uint32_t a = (uint32_t)(src * alpha);
        return (c & 0x00FFFFFFu) | (a << IM_COL32_A_SHIFT);
    }
};

/** Straight-line blend between two packed colours; t is clamped to [0,1]. */
ImU32 lerpColor(ImU32 a, ImU32 b, float t);

/** Draws a label horizontally and vertically centred in [mn, mx]. */
void textCentred(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
                 float size, ImU32 col, const char* text);
#endif  // HAS_IMGUI

/// Lays the board out for the current display and draws it.
///
/// Must run on the render thread between ImGui::NewFrame() and
/// ImGui::Render() — it appends to the foreground draw list, which is only
/// valid in that window. Does nothing when there is no ImGui context.
void drawHud();

/// One page of the menu — the left rail picks exactly one of these at a time.
/// The right pane switches on it, so the order here is also the top-to-bottom
/// order of the rail buttons.
enum class MenuSection {
    Aim = 0,        // 自瞄
    Trigger,        // 扳机
    BackFlash,      // 背闪
    Model,          // 模型
    Touch,          // 触摸
    Capture,        // 画面 — picker size + live preview of what is being captured
    Settings,       // 设置
    Count,
};

/// Which pages get vertical scrolling. Capture is intentionally excluded —
/// its rows fit on a single screen and a scroll would just steal drag
/// gestures from the preview well the page already owns.
inline bool sectionIsScrollable(MenuSection s) {
    return s != MenuSection::Capture && s != MenuSection::Count;
}

/// The section currently highlighted in the rail. Persists across frames and
/// is only ever written by a tap that lands on a rail button.
MenuSection currentSection();

/// The rectangle the last drawHud() placed the board at. Only meaningful on
/// the render thread, right after drawHud() — that is how updateRegions()
/// learns about it.
HudRect hudRect();

/// Layout scale: 1.0 means "as designed for a 1200 px tall board". Multiply any
/// future content metric (padding, font size, row height) by this so the board
/// keeps its proportions on every resolution.
float hudScale();

/// Live alpha (0..1) the HUD is currently fading at — 0 when fully hidden, 1
/// when fully shown. Set by drawHud() every frame; widgets without a Xf
/// parameter read this through a thin helper so they too fade with the HUD.
extern float g_hudAlpha;

/// True once the show/hide ease has arrived: the board is neither opening nor
/// closing. The render loop uses it to drop to its idle rate — an ease that
/// has finished has no business holding the GPU at the refresh rate.
bool hudSettled();

/// True when the board is fully hidden, so drawHud() early-outed and only the
/// float button is on screen. Nothing on screen is moving and the loop can
/// crawl.
bool hudHidden();

/// Scroll context for one right-pane section. Bundles the per-page offset with
/// a small helper so each section paints at `y - sc.offset` but keeps `y`
/// advancing in the *natural* (un-scrolled) coordinate space, the way the
/// section's hit-test rectangles expect.
///
/// The natural-vs-shifted split lets the section report its true content
/// height back to drawContent by simply returning the final `y` value — which
/// is exactly the contract the scroll clamp needs (max scroll = content - view).
struct Scroll {
    float offset = 0.0f;          // surface px scrolled (>=0, <= maxOffset)
    float maxOffset = 0.0f;       // computed by drawContent each frame

    /// Surface-pixel y a row drawn at natural coordinate `y` should actually
    /// appear at. Sections use this everywhere a Rect is built or xf.pt() is
    /// called, so paint and hit-test stay in lock-step.
    float screenY(float naturalY) const { return naturalY - offset; }

    /// True when scrolling is worth driving on this page (content longer than
    /// the viewport). A page that fits without scrolling still reports its
    /// maxOffset == 0 so its offset stays parked at 0.
    bool active() const { return maxOffset > 0.5f; }
};

}  // namespace ui
}  // namespace aimbotng
