// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::ime — the overlay's own on-screen keyboard
//
//  Why not the system IME, and not a reflection shim to reach it:
//
//  The menu is not an Activity. It is a SurfaceFlinger layer built by a shell
//  process that has no Context (see ui/gui/aimbot_ui.h and the shell daemon),
//  so "ask the platform for a keyboard" means bouncing a request through the
//  app process, hoping an Activity is alive to host it, and living with a
//  system window that is drawn by another process — over a layer whose whole
//  point is that the game stays visible around it. That path is a pile of
//  moving parts for a field that needs to hold a profile name.
//
//  So the keyboard is drawn here, out of the same ImDrawList primitives as
//  every other control: rounded keys, an accent that lights on press, a
//  candidate strip. Two layers of keys (letters, numbers/symbols) plus a pinyin
//  mode, which is what "标准 26 键 + 中文 + 数字" comes to.
//
//  Chinese, honestly: there is no dictionary engine in this process and no
//  libpinyin to link, so candidates come from a built-in syllable table — the
//  most common characters for each of ~350 syllables, exact match first, then
//  prefix matches while the syllable is still being typed. That is enough to
//  name a profile (枪 / 狙 / 步 / 满配 / 红点 …) and it says so plainly instead
//  of pretending to be a full IME: a syllable the table does not know offers the
//  raw pinyin as its first candidate, so nothing is ever unreachable.
//
//  Editing rules live here too (insert / backspace / caret) because the field
//  widget is deliberately view-only — see the note on textField() in widgets.h.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

#include "ui/gui/hud.h"
#include "ui/gui/widgets.h"

namespace aimbotng {
namespace ui {
namespace ime {

/// How tall the keyboard is at this layout scale. The owning page reserves this
/// much room at the bottom of its content so the field it is editing is never
/// underneath the keyboard.
float height(float s);

/// The panel the keyboard draws into, inside `board`.
widgets::Rect panelRect(const HudRect& board, float s);

/// Opens the keyboard for `field`, which must outlive it (the page owns it).
/// Focusing the field and opening the keyboard are the same event.
void open(widgets::DialogField* field);
void close();
bool isOpen();
widgets::DialogField* field();

/// True when the panel drawn last frame contains this surface-pixel point. The
/// page uses it to decide whether a tap was meant for the keyboard or for the
/// scrim behind it.
bool hitPanel(const ImVec2& p);

/// Draws and drives the keyboard. Returns true on a frame where the text
/// changed, so the page can re-run its "is the name usable" check.
bool draw(ImDrawList* dl, const widgets::Rect& r, float s, const Xf& xf);

/// The pinyin buffer, or "" when the letter keys are emitting Latin directly.
/// The page shows it above the field so a half-typed syllable is visible.
const char* composition();

}  // namespace ime
}  // namespace ui
}  // namespace aimbotng
