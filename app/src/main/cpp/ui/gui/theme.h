// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::theme — the one place the menu's colours are written down
//
//  Both the board itself (hud.cpp) and the controls that sit on it
//  (widgets.cpp) paint from here, so the accent cannot drift apart between
//  them. Values are packed IM_COL32 and fully opaque: the board is composited
//  as a shell-owned SurfaceFlinger layer, so nothing downstream scales alpha
//  any more (see hud.cpp — an overlay window carrying FLAG_NOT_TOUCHABLE gets
//  capped at 0.8 by this platform, which is what the old window-based menu had
//  to dance around).
//
//  The accent is a saturated blue in the neverlose vein.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#ifdef HAS_IMGUI
#include "imgui.h"

namespace aimbotng {
namespace ui {
namespace theme {

// Accent — every "this one is active" surface paints itself in it.
constexpr ImU32 Accent       = IM_COL32( 61, 123, 255, 255);

// ── Panes ────────────────────────────────────────────────────────────────────
// Two greys: a darker content pane and a lighter navigation rail. Neither is
// pure black — a menu floating over a game reads better as a slab of grey than
// as a hole punched through the scene.
constexpr ImU32 PaneRight    = IM_COL32( 20,  21,  24, 255);
constexpr ImU32 PaneRail     = IM_COL32( 32,  33,  38, 255);
constexpr ImU32 Edge         = IM_COL32(255, 255, 255,  40);
constexpr ImU32 Divider      = IM_COL32(255, 255, 255,  10);

// ── Controls ─────────────────────────────────────────────────────────────────
constexpr ImU32 ControlBg    = IM_COL32( 40,  41,  48, 255);  // dropdown row
constexpr ImU32 ControlBgHi  = IM_COL32( 52,  54,  62, 255);  // dropdown row, touch-held
constexpr ImU32 Track        = IM_COL32( 52,  54,  62, 255);  // slider groove / switch off
constexpr ImU32 ListBg       = IM_COL32( 28,  29,  34, 255);  // open dropdown panel
constexpr ImU32 Knob         = IM_COL32(255, 255, 255, 255);

// Backdrop of the capture page's preview well, before any frame has arrived.
// Darker than the pane on purpose, so an empty well reads as "a screen that is
// not showing anything yet" rather than as another control.
constexpr ImU32 PreviewWell  = IM_COL32( 12,  13,  16, 255);

// ── Text ─────────────────────────────────────────────────────────────────────
constexpr ImU32 TextPrimary  = IM_COL32(238, 240, 246, 255);
constexpr ImU32 TextMuted    = IM_COL32(146, 151, 165, 255);
constexpr ImU32 TextOnAccent = IM_COL32(255, 255, 255, 255);

// ── Detection overlay ────────────────────────────────────────────────────────
// The debug rectangles. Red rather than the accent blue, because the accent
// means "this control is active" and a box drawn around a target is not a
// control — the two would read as the same kind of thing. Opaque-ish because it
// is painted straight onto a game frame that can be any colour underneath.
constexpr ImU32 DetectBox   = IM_COL32(255,  64,  72, 235);
constexpr ImU32 DetectPlate = IM_COL32(255,  64,  72, 215);
constexpr ImU32 DetectText  = IM_COL32(255, 255, 255, 255);

// ── Tracking overlay ───────────────────────────────────────────────────────
// The smoothed, stable tracks the Kalman tracker emits (blue, to be visually
// distinct from the raw red detection boxes). Shows only the track id — no
// confidence, no class — because the point is to watch identity (does the id
// stay put on one enemy?) and smoothing, not the model's per-box score.
constexpr ImU32 TrackBox   = IM_COL32( 70, 150, 255, 240);
constexpr ImU32 TrackPlate = IM_COL32( 40,  90, 200, 220);
constexpr ImU32 TrackText  = IM_COL32(255, 255, 255, 255);

}  // namespace theme
}  // namespace ui
}  // namespace aimbotng
#endif  // HAS_IMGUI
