// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::float_button — the always-on toggle that opens and closes the HUD
//
//  The HUD is a 4:3 board pinned to the centre of the surface (see hud.h), and
//  on its own it has no affordance to leave: a finger inside the board is
//  consumed by the menu, a finger outside it falls through to the game. This
//  button is the one fixed UI element that survives the HUD being toggled
//  off — and is the only way to bring it back.
//
//  It starts centred on the screen, and the user can drag it anywhere within
//  the surface (clamped at the edges so the whole button stays grabbable).
//  A press that releases inside the button — without having dragged past a
//  small threshold — counts as a tap and toggles the HUD; a press that moves
//  further than that commits to a drag and the toggle never fires.
//
//  Drawn straight onto the foreground ImDrawList (like the board itself),
//  after every other piece of UI has appended to it, so the button is always
//  on top. Hit testing follows the same pattern as drawRail/widgets.cpp — the
//  rectangle is hand-tested against io.MousePos, no ImGui window needed.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "ui/gui/hud.h"

namespace aimbotng {
namespace ui {

/// Draws the float button on the foreground draw list, always last so it sits
/// above every other piece of UI the menu draws. Must run on the render thread
/// between ImGui::NewFrame() and ImGui::Render().
void drawFloatButton();

/// Rectangle the button occupies on the last drawFloatButton() call. Same space
/// the touch coordinates live in — what updateRegions() publishes for it.
HudRect floatButtonRect();

/// Whether the menu board's show/hide target is currently "shown". Toggled by
/// a tap on the float button; drawHud() animates g_hudAnim towards this every
/// frame so the question is "do we want it open", not "is it on screen right
/// now" (during the transition the answer to those differs).
bool hudVisible();

/// What the float button writes when tapped; the draw loop eases g_hudAnim
/// towards this each frame. Lives here so the button can flip it without
/// taking on a C++ dependency on the file that holds drawHud().
extern bool g_hudVisibleTarget;

}  // namespace ui
}  // namespace aimbotng