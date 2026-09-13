// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::float_button — implementation. See float_button.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "float_button.h"

#include <cmath>

#include "imgui.h"

namespace aimbotng {
namespace ui {

// Defined in hud.cpp — the float button is the only place that flips it.
extern bool g_hudVisibleTarget;

namespace {

// ── Design constants ─────────────────────────────────────────────────────────
// All in surface pixels — the button is a fixed-size UI element pinned to the
// surface, not part of the board's 1200-px-tall design space, so it does not
// scale with hudScale(). 50 px is the requested size; the inset leaves a
// chunky black square inside the white shell.
constexpr float kButtonSize          = 50.0f;
constexpr float kButtonMargin        = 12.0f;   // keep this much of the button on-screen
constexpr float kButtonRounding      = 12.0f;
constexpr float kButtonInnerInset    = 12.0f;   // black square padding
constexpr float kButtonInnerRounding = 4.0f;

// A press that moves the finger this far in surface pixels before releasing is
// treated as a drag rather than a tap. Below this distance the press toggles
// the HUD; above it the press only repositions the button. 8 px is "the
// finger obviously meant to drag" without making a tap require an unnaturally
// still finger.
constexpr float kDragThreshold = 8.0f;

// ── State ────────────────────────────────────────────────────────────────────
// Render thread only. No lock needed.
HudRect g_buttonRect;

// Last position the user dragged the button to, in surface pixels. Initial
// position is overwritten on the first frame to centre the button, but a
// placeholder here so it never reads uninitialised in the very first draw
// (before any surface size is known).
float g_btnX = 0.0f;
float g_btnY = 0.0f;
bool  g_positionInitialised = false;

// Drag state.
bool  g_dragging     = false;
float g_dragStartX   = 0.0f;
float g_dragStartY   = 0.0f;
float g_dragDownBtnX = 0.0f;
float g_dragDownBtnY = 0.0f;
bool  g_dragMoved    = false;   // crossed the threshold yet?

/** Centres the button on the current surface, leaving g_positionInitialised set. */
void centreOnSurface(float dw, float dh) {
    g_btnX = (dw - kButtonSize) * 0.5f;
    g_btnY = (dh - kButtonSize) * 0.5f;
    g_positionInitialised = true;
}

/** Clamps the button so its full size stays inside [0, dw] x [0, dh]. */
void clampToSurface(float dw, float dh) {
    if (g_btnX < kButtonMargin) g_btnX = kButtonMargin;
    if (g_btnY < kButtonMargin) g_btnY = kButtonMargin;
    if (g_btnX + kButtonSize > dw - kButtonMargin) g_btnX = dw - kButtonMargin - kButtonSize;
    if (g_btnY + kButtonSize > dh - kButtonMargin) g_btnY = dh - kButtonMargin - kButtonSize;
}

}  // namespace

HudRect floatButtonRect() { return g_buttonRect; }
bool    hudVisible()      { return g_hudVisibleTarget; }

void drawFloatButton() {
#ifdef HAS_IMGUI
    ImGuiIO& io = ImGui::GetIO();
    const float dw = io.DisplaySize.x;
    const float dh = io.DisplaySize.y;
    if (dw <= 1.0f || dh <= 1.0f) return;

    // First frame on a new surface: centre the button. Subsequent frames
    // remember the last dragged position. The clamp is idempotent — running
    // it on every frame is cheap and catches window-resize cases where a
    // previously-valid position is now off-screen.
    if (!g_positionInitialised) centreOnSurface(dw, dh);
    clampToSurface(dw, dh);

    g_buttonRect = HudRect{g_btnX, g_btnY, kButtonSize, kButtonSize};
    const ImVec2 mn(g_btnX, g_btnY);
    const ImVec2 mx(g_btnX + kButtonSize, g_btnY + kButtonSize);
    const ImVec2 mouse = io.MousePos;

    const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool inside = mouse.x >= mn.x && mouse.x < mx.x &&
                        mouse.y >= mn.y && mouse.y < mx.y;

    // ── Drag tracking ─────────────────────────────────────────────────────────
    // Press-down inside the button starts a drag: remember where both the
    // finger and the button were, so a finger delta becomes a button delta.
    if (mouseClicked && inside && !g_dragging) {
        g_dragging     = true;
        g_dragMoved    = false;
        g_dragStartX   = mouse.x;
        g_dragStartY   = mouse.y;
        g_dragDownBtnX = g_btnX;
        g_dragDownBtnY = g_btnY;
    }

    if (g_dragging && mouseDown) {
        const float dx = mouse.x - g_dragStartX;
        const float dy = mouse.y - g_dragStartY;
        if (!g_dragMoved && (std::fabsf(dx) > kDragThreshold ||
                             std::fabsf(dy) > kDragThreshold)) {
            g_dragMoved = true;
        }
        if (g_dragMoved) {
            g_btnX = g_dragDownBtnX + dx;
            g_btnY = g_dragDownBtnY + dy;
            clampToSurface(dw, dh);
            g_buttonRect = HudRect{g_btnX, g_btnY, kButtonSize, kButtonSize};
        }
    }

    if (mouseReleased && g_dragging) {
        // Tap = press started inside, never crossed the threshold, released
        // inside (the down-inside + never-moved already implies that).
        if (!g_dragMoved) g_hudVisibleTarget = !g_hudVisibleTarget;
        g_dragging = false;
    }

    // ── Drawing ───────────────────────────────────────────────────────────────
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // White rounded shell.
    dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, 255), kButtonRounding);

    // Black square inside — inset on every side so it sits centred with a
    // visible white border.
    const float inset = kButtonInnerInset;
    dl->AddRectFilled(ImVec2(g_btnX + inset, g_btnY + inset),
                      ImVec2(g_btnX + kButtonSize - inset, g_btnY + kButtonSize - inset),
                      IM_COL32(0, 0, 0, 255), kButtonInnerRounding);
#endif
}

}  // namespace ui
}  // namespace aimbotng