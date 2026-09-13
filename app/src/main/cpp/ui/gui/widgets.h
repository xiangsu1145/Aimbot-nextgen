// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::widgets — the controls that live on the right pane
//
//  Same philosophy as the board itself (see hud.h): these are drawn with
//  ImDrawList primitives and hit-tested by hand, not assembled out of ImGui's
//  window/widget system. There is no window here to own a widget, so a control
//  is "a rectangle I painted" plus "a test I run against the finger position".
//
//  Adapted from Rise's ClickGui — in particular renderNumberSetting, which
//  splits the slider into three pieces worth keeping:
//
//    * the finger writes a *target* while it is down, not the value,
//    * on release the value snaps to a step boundary, and
//    * the position actually drawn eases towards the target every frame.
//
//  That is what makes the thumb feel soft while dragging and still settle on a
//  clean number instead of stopping wherever the finger happened to lift.
//
//  Every call takes the layout scale `s` (hudScale()) so a control is the same
//  shape at any resolution, and returns whether it changed its value.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "ui/gui/hud.h"

struct ImDrawList;

namespace aimbotng {
namespace ui {
namespace widgets {

/// An axis-aligned rectangle in surface pixels — the same space the board and
/// the touch coordinates live in.
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

/// Row heights, in design pixels (multiply by the layout scale).
constexpr float kSwitchRowH    = 58.0f;
constexpr float kSliderRowH    = 80.0f;
constexpr float kDropdownRowH  = 58.0f;
constexpr float kDropdownItemH = 54.0f;

/// Size of the toggle's groove, in design pixels. Not every caller gets to hand
/// over a whole row — the page header places a bare toggle itself — so the
/// numbers live here rather than inside the implementation.
constexpr float kSwitchTrackW  = 78.0f;
constexpr float kSwitchTrackH  = 50.0f;

/// A round-ended toggle. `anim` is the eased 0..1 the caller must keep across
/// frames; it drives both the knob's travel and the groove's colour.
struct SwitchState {
    bool  value = false;
    float anim  = 0.0f;
};

/// A horizontal slider with a readout.
///
/// `ease` is the position currently drawn (0..1) and `target` the position it
/// is heading for; the gap between them is the whole animation. `placed` lets
/// the first frame snap instead of flying in from zero.
///
/// `press` is a separate 0..1 ease for the thumb's size: it swells while a
/// finger is on the row and settles back on release. It is deliberately not
/// folded into `ease` — the position tracks the finger, the size just reacts.
struct SliderState {
    float value    = 0.0f;
    float min      = 0.0f;
    float max      = 1.0f;
    float step     = 0.0f;   // 0 = continuous
    float ease     = 0.0f;
    float target   = 0.0f;
    float press    = 0.0f;
    bool  dragging = false;
    bool  placed   = false;
};

/// A collapsed row that expands into a list of choices. `anim` is shared: it
/// grows the list *and* turns the chevron (counter-clockwise opening, back the
/// other way closing), so the two never disagree.
struct DropdownState {
    int   value = 0;
    bool  open  = false;
    float anim  = 0.0f;   // eased 0..1, drives the list's reveal and the chevron
};

/// Max items a MultiSelectState can hold — the bitmask is 32 bits wide.
constexpr int kMultiSelectMax = 32;

/// A collapsed row that expands into a checklist. Looks like dropdown() but
/// the open list lets the user tick several items without closing: tapping an
/// item flips its bit in `mask`, and the row stays open until the user taps
/// the row again or taps outside.
///
/// Up to `kMultiSelectMax` items fit in the bitmask — anything more is ignored.
struct MultiSelectState {
    uint32_t mask = 0;    // bit i set = items[i] selected
    bool     open = false;
    float    anim = 0.0f; // eased 0..1, drives the list's reveal
    float    checkAnim[kMultiSelectMax] = {}; // per-item check pop ease (0..1)
    bool     wasOpen = false; // to re-trigger the pop when the list opens
};

// ── Draw + drive ─────────────────────────────────────────────────────────────

/// Returns true when the toggle was flipped.
bool switchButton(ImDrawList* dl, const Rect& r, SwitchState& st,
                  const char* label, float s);

/// The same toggle with no caption, right-aligned inside `r`.
///
/// For a control that shares a line with a title the caller draws itself — the
/// page header — where a full labelled row would waste the space. `r` is both
/// the paint bounds and the touch target, so callers pad it a little past the
/// groove: a finger is bigger than the thing it is aiming at.
///
/// `interactive` lets a modal layer above paint the switch dead: it still
/// animates, it just stops listening.
bool switchToggle(ImDrawList* dl, const Rect& r, SwitchState& st, float s,
                  bool interactive = true);

/// `decimals` is how many fraction digits the readout shows.
/// Returns true when the value changed this frame.
bool sliderFloat(ImDrawList* dl, const Rect& r, SliderState& st,
                 const char* label, int decimals, float s);

/// Returns true when a different item was picked. `drawListInline` controls
/// where the open list is painted: when true (the default) it is drawn here,
/// inline — used by the board's dropdowns. When false the caller paints it
/// itself afterwards via dropdownList(), so it floats above every other
/// control (used by the modal dialogs, whose Cancel/Add buttons would
/// otherwise paint over the list because the whole HUD shares one draw list).
///
/// `disabled` (optional) is a parallel array: a tap on an entry whose
/// `disabled[i]` is true is consumed but does NOT change `st.value`. The
/// entry is also drawn dimmer than the rest, so the user sees it is there
/// but cannot pick it — used by the Touch page to surface our own uinput
/// device without letting anyone try to mirror from it.
bool dropdown(ImDrawList* dl, const Rect& r, DropdownState& st,
              const char* label, const char* const* items, int count, float s,
              bool drawListInline = true,
              const bool* disabled = nullptr);

/// Paints an open dropdown's list (the popover) on `dl`. Call this AFTER every
/// other control in the dialog has been drawn, so the list floats above them.
/// Draws nothing when the list is closed (anim ≈ 0).
void dropdownList(ImDrawList* dl, const Rect& r, const DropdownState& st,
                  const char* const* items, int count, float s,
                  const bool* disabled = nullptr);

/// Vertical space an open dropdown list occupies below its row, so the caller
/// can leave room for it.
float dropdownListHeight(int count, float s);

/// Multi-select variant of dropdown(). Collapsed state shows the items
/// currently set in `mask`, joined by ", " (truncated to fit). Open state
/// lists every item with a left-side check mark when its bit is set. Tapping
/// an item toggles its bit; tapping outside the list closes it; tapping the
/// row itself toggles open/close, same as dropdown().
///
/// `drawListInline` mirrors dropdown()'s flag: when true the list is painted
/// inline right after the row. That only floats above *earlier* rows when the
/// caller paints into the foreground draw list (the board does) — anything
/// drawn later in the same frame covers it. Pages with rows below the
/// multi-select pass false and paint the list last via multiSelectList().
bool multiSelect(ImDrawList* dl, const Rect& r, MultiSelectState& st,
                 const char* label, const char* const* items, int count,
                 float s, bool drawListInline = true);

/// Paints an open multi-select's list (the popover). Call this AFTER every
/// other control on the page has been drawn, so the list floats above them —
/// the multi-select half of the dropdown()/dropdownList() pattern the
/// Add-Model dialog uses. Draws nothing when the list is closed (anim ≈ 0).
/// `r` must be the same row rect passed to multiSelect() that frame.
void multiSelectList(ImDrawList* dl, const Rect& r, MultiSelectState& st,
                     const char* const* items, int count, float s);

// ── Draw + drive (continued, used by the Add-Model dialog) ─────────────────

/// A row-sized button. The "X" / confirm / add / ... affordance the menu needs
/// but the Row enum (switch / slider / dropdown) does not provide.
enum class ButtonVariant {
    Primary,    // the one that does the thing — accent fill, white label
    Danger,     // a destructive "delete" — red fill, white label
    Ghost,      // a tertiary action ("cancel") — slate fill, white label
    Disabled,   // drawn but inert: never fires, label muted
};

/// A single edit field's state. The widget reads/writes it; the page owns the
/// memory of all fields. UTF-8 bytes straight through, so Chinese class names
/// carry no surprise.
struct DialogField {
    char buf[256] = {};
    int  len      = 0;
    int  caret    = 0;
    bool focused  = false;
};

/// True the frame the finger lifts inside `r` after pressing down inside it.
/// Every button in the menu fires through this (press-down alone never
/// activates anything), so a press that opens a layer can never trigger the
/// layer underneath on its way up.
bool clicked(const Rect& r);

/// Drops the in-flight gesture. Call it when a tap transitions layers
/// (opens or dismisses a dialog) so the release is not also read by the
/// other layer.
void consumeTap();

/// True the frame a tap fully outside `r` is released (press AND release
/// outside) — the scrim-dismiss gesture for dialogs.
bool clickedOutside(const Rect& r);

/// A row-sized button. Returns true the frame the tap lands inside it.
/// A Disabled button ignores taps and always returns false.
bool button(ImDrawList* dl, const Rect& r, const char* label,
            ButtonVariant variant, float s);

/// The dialog-title "close" affordance: a square Ghost shell carrying a drawn
/// × rather than the letter X, so it reads the same in every font.
bool closeButton(ImDrawList* dl, const Rect& r, float s);

/// Single-line text field. On press-down a field takes focus; pressing outside
/// any field drops focus on every field the page owns. Returns true the frame
/// `focused` flips either direction so the caller can open / close the IME.
bool textField(ImDrawList* dl, const Rect& r, DialogField& f,
               const char* placeholder, float s);

/// Multi-line text field. Same focus rules as [textField]; view-only —
/// editing happens through whatever input mechanism the caller wires up
/// (an on-screen keyboard drawn in the overlay, or the system IME).
bool multilineField(ImDrawList* dl, const Rect& r, DialogField& f,
                    const char* placeholder, float s);

/// Pill with the model file path on the left and a round "..." button on the
/// right. Returns true the frame the trailing button is tapped.
bool pathPill(ImDrawList* dl, const Rect& r, const char* path, int pathLen,
              bool picking, float s);

/// Where the dialog sits, given the board rectangle and the layout scale.
Rect dialogRect(const HudRect& board, float s);
float dialogHeaderHeight(float s);
float dialogInnerPad(float s);

}  // namespace widgets
}  // namespace ui
}  // namespace aimbotng
