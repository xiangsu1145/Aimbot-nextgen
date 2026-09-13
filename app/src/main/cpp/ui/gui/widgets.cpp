// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::widgets — implementation. See widgets.h for the rationale.
// ─────────────────────────────────────────────────────────────────────────────
#include "ui/gui/widgets.h"

#ifdef HAS_IMGUI
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstring>

#include "imgui.h"
#include "ui/gui/hud.h"
#include "ui/gui/theme.h"

namespace aimbotng {
namespace ui {
namespace widgets {

namespace {

using namespace theme;

// Live HUD alpha, written by drawHud() every frame from the show/hide anim.
// widgets.cpp cannot take a Xf parameter cleanly (every helper would have to
// grow one), so it reads this instead — the result is identical: every colour
// any widget writes is multiplied by the same fade the HUD itself is doing.
// The declaration is in ui/gui/hud.h (already included above) at aimbotng::ui
// scope, which matches the definition in hud.cpp.

/** Multiplies a colour's alpha by the live HUD fade, so every widget honours
 *  the show/hide animation without having to thread Xf through its API.
 *
 *  The colour's *own* alpha is multiplied, not overwritten. Replacing it made
 *  every translucent fill opaque: a Ghost button's "white at alpha 22" came out
 *  as a solid white slab, and its white label sat on top of it invisible. */
static inline ImU32 fadeCol(ImU32 c) {
    const uint32_t src = (c >> IM_COL32_A_SHIFT) & 0xFFu;
    const uint32_t a = (uint32_t)(src * g_hudAlpha);
    return (c & 0x00FFFFFFu) | (a << IM_COL32_A_SHIFT);
}

// ── Metrics (design pixels @1200 px board, scaled by `s`) ────────────────────
constexpr float kLabelSize      = 26.0f;
constexpr float kValueSize      = 24.0f;

// kSwitchTrackW / kSwitchTrackH come from widgets.h — the header places a bare
// toggle, so it needs the groove's size too.
constexpr float kSwitchKnobR    = 19.0f;
constexpr float kSwitchAnimRate = 15.0f;

constexpr float kSliderTitleCy  = 24.0f;   // from the row's top edge
constexpr float kSliderTrackCy  = 58.0f;
constexpr float kSliderTrackH   = 8.0f;
constexpr float kSliderThumbR   = 10.0f;   // resting size, design px
constexpr float kSliderThumbGrow = 0.45f;  // +45% radius while the finger is on it
constexpr float kSliderEaseRate = 22.0f;   // higher = snappier thumb
constexpr float kSliderPressRate = 26.0f;  // ease-out rate for the thumb's squeeze

constexpr float kDropdownPadX   = 22.0f;
constexpr float kDropdownRound  = 12.0f;
constexpr float kDropdownGap    = 6.0f;    // selector -> open list
constexpr float kDropdownAnimRate = 18.0f;
constexpr float kDropdownInsetY = 5.0f;    // selector sits inset inside the row
constexpr float kChevronW       = 17.0f;   // x extent of the "v" — clearly wider
constexpr float kChevronH       = 8.0f;    // y extent — than it is tall, so the arms read as stubby
constexpr float kChevronThick   = 3.4f;
constexpr float kPi             = 3.14159265358979323846f;

// ── Small helpers ────────────────────────────────────────────────────────────

bool inside(const ImVec2& p, const Rect& r) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

/** Frame-time-scaled exponential approach — the same curve as the rail glide:
 *  each step covers a fraction of what is *left*, so it decelerates. */
float approach(float current, float target, float rate, float dt) {
    if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
    if (fabsf(target - current) < 0.0005f) return target;  // < half a thousandth: arrive
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

ImU32 lerpColor(ImU32 a, ImU32 b, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float u = 1.0f - t;
    const int r  = (int)(((a >> IM_COL32_R_SHIFT) & 0xFF) * u + ((b >> IM_COL32_R_SHIFT) & 0xFF) * t);
    const int g  = (int)(((a >> IM_COL32_G_SHIFT) & 0xFF) * u + ((b >> IM_COL32_G_SHIFT) & 0xFF) * t);
    const int bl = (int)(((a >> IM_COL32_B_SHIFT) & 0xFF) * u + ((b >> IM_COL32_B_SHIFT) & 0xFF) * t);
    const int al = (int)(((a >> IM_COL32_A_SHIFT) & 0xFF) * u + ((b >> IM_COL32_A_SHIFT) & 0xFF) * t);
    return IM_COL32(r, g, bl, al);
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

void textLeft(ImDrawList* dl, float x, float centreY, float size, ImU32 col, const char* text) {
    ImFont* f = ImGui::GetFont();
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(f, size, ImVec2(x, centreY - ts.y * 0.5f), fadeCol(col), text);
}

void textRight(ImDrawList* dl, float xRight, float centreY, float size, ImU32 col, const char* text) {
    ImFont* f = ImGui::GetFont();
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(f, size, ImVec2(xRight - ts.x, centreY - ts.y * 0.5f), fadeCol(col), text);
}

/**
 * One stroke of a chevron: a thick segment rounded at *both* ends.
 *
 * A three-point AddPolyline would be the obvious way to draw "<", but it has
 * two problems here — its free ends are flat-cut, and at a corner this sharp
 * the joint geometry can shear and read as crooked. Two of these instead: a
 * quad for the body, plus a disc at each end, so every corner is a clean arc.
 */
void roundSegment(ImDrawList* dl, const ImVec2& a, const ImVec2& b, float r, ImU32 col) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = sqrtf(dx * dx + dy * dy);
    const ImU32 fc = fadeCol(col);
    if (len < 0.0001f) {
        dl->AddCircleFilled(a, r, fc);
        return;
    }
    const float nx = -dy / len * r;  // unit normal, scaled to the half-width
    const float ny = dx / len * r;
    const ImVec2 body[4] = {
        ImVec2(a.x + nx, a.y + ny), ImVec2(b.x + nx, b.y + ny),
        ImVec2(b.x - nx, b.y - ny), ImVec2(a.x - nx, a.y - ny),
    };
    dl->AddConvexPolyFilled(body, 4, fc);
    dl->AddCircleFilled(a, r, fc);
    dl->AddCircleFilled(b, r, fc);
}

/**
 * Paints a toggle groove and its knob, right-aligned inside `r`.
 *
 * Shared by the labelled row ([switchButton]) and the label-less header toggle
 * ([switchToggle]) so the two can never drift apart: the groove blends
 * grey -> accent as it turns on while the knob crosses it on the very same eased
 * value, which is what makes colour and motion read as one movement.
 */
void drawSwitchTrack(ImDrawList* dl, const Rect& r, float anim, float s) {
    const float cy     = r.y + r.h * 0.5f;
    const float trackW = kSwitchTrackW * s;
    const float trackH = kSwitchTrackH * s;
    const ImVec2 min(r.x + r.w - trackW, cy - trackH * 0.5f);

    dl->AddRectFilled(min, ImVec2(min.x + trackW, min.y + trackH),
                      fadeCol(lerpColor(Track, Accent, anim)), trackH * 0.5f);

    const float knobX = min.x + trackH * 0.5f + (trackW - trackH) * anim;
    dl->AddCircleFilled(ImVec2(knobX, cy), kSwitchKnobR * s, fadeCol(Knob));
}

}  // namespace

// ── Switch ───────────────────────────────────────────────────────────────────

bool switchButton(ImDrawList* dl, const Rect& r, SwitchState& st,
                  const char* label, float s) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    st.anim = approach(st.anim, st.value ? 1.0f : 0.0f, kSwitchAnimRate, io.DeltaTime);

    // Label on the left, toggle flush with the row's right edge — the layout a
    // settings pane wants, rather than the label-hugging-a-dot look.
    textLeft(dl, r.x, r.y + r.h * 0.5f, kLabelSize * s, TextPrimary, label);
    drawSwitchTrack(dl, r, st.anim, s);

    // Fire on release: a finger that lands here, slides off and lifts should
    // not toggle anything, and there is no hover to tell us that on touch.
    bool changed = false;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && inside(mouse, r)) {
        st.value = !st.value;
        changed = true;
    }
    return changed;
}

bool switchToggle(ImDrawList* dl, const Rect& r, SwitchState& st, float s,
                  bool interactive) {
    st.anim = approach(st.anim, st.value ? 1.0f : 0.0f, kSwitchAnimRate,
                       ImGui::GetIO().DeltaTime);
    drawSwitchTrack(dl, r, st.anim, s);

    // Same release semantics as the labelled switch — see switchButton.
    // `interactive` lets a modal layer above paint the switch dead: it still
    // animates, it just stops listening.
    bool changed = false;
    if (interactive && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        inside(ImGui::GetIO().MousePos, r)) {
        st.value = !st.value;
        changed = true;
    }
    return changed;
}

// ── Slider ───────────────────────────────────────────────────────────────────

bool sliderFloat(ImDrawList* dl, const Rect& r, SliderState& st,
                 const char* label, int decimals, float s) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    const float span = (st.max - st.min) != 0.0f ? (st.max - st.min) : 1.0f;
    if (!st.placed) {  // first frame: land on the current value, do not fly in
        st.ease = st.target = clamp01((st.value - st.min) / span);
        st.placed = true;
    }

    // Grab anywhere on the row, not just the 8 px groove — touch has no hover,
    // so a thin target is a missed target.
    if (!st.dragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside(mouse, r)) {
        st.dragging = true;
    }
    const bool dragging = st.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // The thumb breathes: it swells under the finger and settles back on
    // release, on the same decelerating curve as everything else here. The
    // *mapping* below always uses the resting radius, so the geometry the
    // finger is read against does not shift while the thumb is growing.
    st.press = approach(st.press, dragging ? 1.0f : 0.0f, kSliderPressRate, io.DeltaTime);

    const float titleCy = r.y + kSliderTitleCy * s;
    const float trackCy = r.y + kSliderTrackCy * s;
    const float trackH  = kSliderTrackH * s;
    const float thumbR  = kSliderThumbR * s * (1.0f + kSliderThumbGrow * st.press);
    const float trackX0 = r.x + kSliderThumbR * s;
    const float trackX1 = r.x + r.w - kSliderThumbR * s;
    const float trackW  = trackX1 - trackX0;

    const float before = st.value;

    if (dragging) {
        // While the finger is down it writes the *target* only. The value is
        // derived from it so the readout tracks the finger live.
        st.target = clamp01((mouse.x - trackX0) / trackW);
        st.value = st.min + st.target * span;
    } else if (st.dragging) {
        st.dragging = false;
        // Released. Snap the value onto a step boundary and aim the drawn thumb
        // at where that ended up, so it eases in instead of jerking — this is
        // the behaviour worth keeping from Rise's renderNumberSetting.
        if (st.step > 0.0f) {
            st.value = st.min + roundf((st.value - st.min) / st.step) * st.step;
            st.value = st.value < st.min ? st.min : (st.value > st.max ? st.max : st.value);
        }
        st.target = clamp01((st.value - st.min) / span);
    }

    st.ease = approach(st.ease, st.target, kSliderEaseRate, io.DeltaTime);

    // ── Draw ─────────────────────────────────────────────────────────────────
    textLeft(dl, r.x, titleCy, kLabelSize * s, TextPrimary, label);

    char readout[48];
    snprintf(readout, sizeof(readout), "%.*f", decimals, st.value);
    // The readout brightens while the finger is on the row — but eased, so it
    // fades in/out instead of snapping. st.press is the same 0..1 that grows
    // the thumb, which is why "press" already does the right thing here: it is
    // 1 the instant the finger lands and eases back to 0 on release.
    textRight(dl, r.x + r.w, titleCy, kValueSize * s,
              lerpColor(TextMuted, TextPrimary, st.press), readout);

    dl->AddRectFilled(ImVec2(trackX0, trackCy - trackH * 0.5f),
                      ImVec2(trackX1, trackCy + trackH * 0.5f),
                      fadeCol(Track), trackH * 0.5f);

    const float thumbX = trackX0 + st.ease * trackW;
    dl->AddRectFilled(ImVec2(trackX0, trackCy - trackH * 0.5f),
                      ImVec2(thumbX, trackCy + trackH * 0.5f),
                      fadeCol(Accent), trackH * 0.5f);
    dl->AddCircleFilled(ImVec2(thumbX, trackCy), thumbR, fadeCol(Knob));

    return st.value != before;
}

// ── Dropdown ─────────────────────────────────────────────────────────────────

float dropdownListHeight(int count, float s) {
    return count * kDropdownItemH * s;
}

bool dropdown(ImDrawList* dl, const Rect& r, DropdownState& st,
              const char* label, const char* const* items, int count, float s,
              bool drawListInline, const bool* disabled) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    st.anim = approach(st.anim, st.open ? 1.0f : 0.0f, kDropdownAnimRate, io.DeltaTime);

    const float cy    = r.y + r.h * 0.5f;
    const float padX  = kDropdownPadX * s;
    const float itemH = kDropdownItemH * s;

    // The row is split down the middle: the title keeps the left half, the
    // selector takes the right. Inside the selector the value is flush left and
    // the chevron flush right, so the pair reads as one control even though the
    // title sits outside it.
    const float insetY = kDropdownInsetY * s;
    const Rect  field{r.x + r.w * 0.5f, r.y + insetY, r.w * 0.5f, r.h - insetY * 2.0f};
    const float fieldCy = field.y + field.h * 0.5f;

    const float listTop = field.y + field.h + kDropdownGap * s;
    const float listH   = count * itemH;
    const float shownH  = listH * st.anim;

    const char* current = (st.value >= 0 && st.value < count) ? items[st.value] : "";

    // ── Hit test first, so a tap on an item is not also read as a tap on the
    //    row underneath it ────────────────────────────────────────────────────
    int picked = -1;
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (released) {
        if (st.open) {
            for (int i = 0; i < count; ++i) {
                const Rect item{field.x, listTop + i * itemH, field.w, itemH};
                if (inside(mouse, item)) { picked = i; break; }
            }
        }
        if (picked >= 0) {
            // A disabled entry consumes the tap (so the list closes as it
            // would for any item) but does NOT change st.value — the user
            // sees the entry there but cannot pick it.
            if (disabled == nullptr || !disabled[picked]) {
                st.value = picked;
            }
            st.open = false;
        } else if (inside(mouse, r)) {
            st.open = !st.open;          // tapping the row toggles the list
        } else if (st.open) {
            st.open = false;             // tapping anywhere else closes it
        }
    }

    // ── Row ──────────────────────────────────────────────────────────────────
    textLeft(dl, r.x, cy, kLabelSize * s, TextPrimary, label);

    const bool rowHeld = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, r);
    dl->AddRectFilled(ImVec2(field.x, field.y),
                      ImVec2(field.x + field.w, field.y + field.h),
                      fadeCol(rowHeld ? ControlBgHi : ControlBg), kDropdownRound * s);

    const float chevHalfW = kChevronW * s * 0.5f;
    const float chevHalfH = kChevronH * s * 0.5f;
    const float chevStroke = kChevronThick * s * 0.5f;
    // Pull the centre left by the stroke's own radius so the *drawn* right edge,
    // cap included, lands on the padding and not half a stroke past it.
    const float chevCx = field.x + field.w - padX - chevHalfW - chevStroke;

    textLeft(dl, field.x + padX, fieldCy, kValueSize * s, TextMuted, current);

    // The chevron turns as the list opens: it spins counter-clockwise on the way
    // out and unwinds clockwise on the way back, both driven by the same eased
    // value that grows the panel — so the arrow and the reveal are one motion.
    // Screen Y points down, so a *negative* angle is the counter-clockwise turn.
    const float ca = st.anim * -kPi;
    const float cc = cosf(ca);
    const float cs = sinf(ca);
    auto turn = [&](float px, float py) {
        return ImVec2(chevCx + px * cc - py * cs, fieldCy + px * cs + py * cc);
    };
    const ImVec2 apex = turn(0.0f, chevHalfH);    // points down at rest
    roundSegment(dl, apex, turn(-chevHalfW, -chevHalfH), chevStroke, TextMuted);
    roundSegment(dl, apex, turn(chevHalfW, -chevHalfH), chevStroke, TextMuted);

    // ── Open list ────────────────────────────────────────────────────────────
    // Drawn on the *foreground* draw list, so it paints on top of every other
    // control in the dialog — the engine row, the sliders, and the Cancel/Save
    // buttons below it. A dropdown is a popover, not an in-flow row: drawing it
    // into the same `dl` as the rest would let later controls paint over it
    // (that is exactly why the list used to appear "under" the other widgets).
    // Clipped to the revealed height so the panel grows out of the row rather
    // than appearing all at once.
    //
    // When `drawListInline` is false the list is NOT painted here; the caller
    // defers it to dropdownList() (drawn last) so it floats above the dialog's
    // own buttons, which are added to `dl` after this row.
    if (st.anim > 0.001f && drawListInline) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 listMin(field.x, listTop);
        const ImVec2 listMax(field.x + field.w, listTop + listH);
        fg->PushClipRect(listMin, ImVec2(listMax.x, listMin.y + shownH), true);

        fg->AddRectFilled(listMin, listMax, fadeCol(ListBg), kDropdownRound * s);

        for (int i = 0; i < count; ++i) {
            const Rect item{field.x, listTop + i * itemH, field.w, itemH};
            const bool selected = (i == st.value);
            const bool isDisabled = (disabled != nullptr && disabled[i]);
            const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, item);

            if (selected && !isDisabled) {
                fg->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                                  ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                                  fadeCol(Accent), kDropdownRound * s);
            } else if (held && !isDisabled) {
                fg->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                                  ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                                  fadeCol(ControlBgHi), kDropdownRound * s);
            }

            // Disabled entries draw dimmer than muted — the user should see
            // they exist but not confuse them with a real choice.
            const ImU32 itemCol = isDisabled
                                    ? lerpColor(PaneRight, TextMuted, 0.45f)
                                    : (selected ? TextOnAccent : TextMuted);
            textLeft(fg, item.x + padX, item.y + item.h * 0.5f, kLabelSize * s,
                     itemCol, items[i]);
        }

        fg->PopClipRect();
    }

    return picked >= 0;
}

void dropdownList(ImDrawList* dl, const Rect& r, const DropdownState& st,
                  const char* const* items, int count, float s,
                  const bool* disabled) {
    if (st.anim <= 0.001f) return;

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    const float padX  = kDropdownPadX * s;
    const float itemH = kDropdownItemH * s;
    const float insetY = kDropdownInsetY * s;
    const Rect  field{r.x + r.w * 0.5f, r.y + insetY, r.w * 0.5f, r.h - insetY * 2.0f};
    const float listTop = field.y + field.h + kDropdownGap * s;
    const float listH   = count * itemH;
    const float shownH  = listH * st.anim;

    const ImVec2 listMin(field.x, listTop);
    const ImVec2 listMax(field.x + field.w, listTop + listH);
    dl->PushClipRect(listMin, ImVec2(listMax.x, listMin.y + shownH), true);

    dl->AddRectFilled(listMin, listMax, fadeCol(ListBg), kDropdownRound * s);

    for (int i = 0; i < count; ++i) {
        const Rect item{field.x, listTop + i * itemH, field.w, itemH};
        const bool selected = (i == st.value);
        const bool isDisabled = (disabled != nullptr && disabled[i]);
        const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, item);

        if (selected && !isDisabled) {
            dl->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                              ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                              fadeCol(Accent), kDropdownRound * s);
        } else if (held && !isDisabled) {
            dl->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                              ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                              fadeCol(ControlBgHi), kDropdownRound * s);
        }

        const ImU32 itemCol = isDisabled
                                ? lerpColor(PaneRight, TextMuted, 0.45f)
                                : (selected ? TextOnAccent : TextMuted);
        textLeft(dl, item.x + padX, item.y + item.h * 0.5f, kLabelSize * s,
                 itemCol, items[i]);
    }

    dl->PopClipRect();
}

// ── Multi-select ─────────────────────────────────────────────────────────────
//
// Visually a clone of dropdown() at the row level (label + pill-field +
// chevron), but the open list renders a checkbox-style indicator per item:
// a left-side check mark drawn from two AddLine segments, no surrounding box,
// and the row stays open across taps so the user can tick several items
// without re-opening it.
//
// Animation: the *panel* reveals like dropdown() — a `ListBg` background
// whose clip rect grows from the top (height 0 → full, alpha steady), so the
// rows appear to drop down from the row rather than scaling as one block. The
// *check mark* pops in *per item*: each tick owns its own `checkAnim[i]` ease
// (scale kChkScaleStart → 1.0 and alpha 0 → 1) that tracks whether that item is
// selected, so tapping an item grows/fades its tick in instead of snapping it
// on. The whole list opening also re-triggers the pop (see `wasOpen`).
namespace {
constexpr float kMSAnimRate     = 16.0f;
constexpr float kChkScaleStart  = 0.6f;    // check-mark scale when checkAnim == 0
constexpr float kMSCheckW       = 11.0f;   // check-mark x extent, design px
constexpr float kMSCheckH       = 12.0f;   // check-mark y extent, design px
constexpr float kMSCheckStroke  = 2.8f;    // check-mark line thickness, design px
constexpr int   kMSMaxChars     = 64;      // collapsed-state label cap
constexpr int   kMSMaxItems     = 32;      // bitmask width (== kMultiSelectMax)

/// Multiplies by both g_hudAlpha (the standard fadeCol) AND `anim` (the
/// open/close ease), so per-item colour goes to 0 as the list collapses.
inline ImU32 fadeColAnim(ImU32 c, float anim) {
    const uint32_t src = (c >> IM_COL32_A_SHIFT) & 0xFFu;
    const uint32_t a   = (uint32_t)(src * g_hudAlpha * anim);
    return (c & 0x00FFFFFFu) | (a << IM_COL32_A_SHIFT);
}
}  // namespace

/// Paints a multi-select's open list. Shared by multiSelect() (inline mode)
/// and multiSelectList() (deferred mode — the Add-Model-dialog pattern where
/// the caller paints the list after every other control, so it floats above
/// rows drawn later in the same frame).
///
/// Needed because the board paints into the foreground draw list: an inline
/// list is covered by any row drawn after it in the same frame (that was the
/// 瞄准类别 list disappearing under the Y死区 sliders).
static void paintMultiSelectList(ImDrawList* fg, const Rect& r, MultiSelectState& st,
                                 const char* const* items, int count, float s) {
    if (count <= 0 || items == nullptr) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    const float padX  = kDropdownPadX * s;
    const float insetY = kDropdownInsetY * s;
    const Rect  field{r.x + r.w * 0.5f, r.y + insetY, r.w * 0.5f, r.h - insetY * 2.0f};
    const float itemH = kDropdownItemH * s;
    const float listTop = field.y + field.h + kDropdownGap * s;
    const float listH = count * itemH;

    // Reveal like dropdown(): a clip rect that grows downward with `anim`,
    // no whole-list scale. The panel background is a steady `ListBg` slab;
    // rows appear to drop down from the row rather than the block squashing.
    const float shownH = listH * st.anim;
    const ImVec2 listMin(field.x, listTop);
    const ImVec2 listMax(field.x + field.w, listTop + listH);
    fg->PushClipRect(listMin, ImVec2(listMax.x, listMin.y + shownH), true);

    // Panel background — dropdown-style, no per-item blue.
    fg->AddRectFilled(listMin, listMax, fadeCol(ListBg), kDropdownRound * s);

    // Re-trigger the per-item pop the moment the list opens, so the ticks
    // grow in rather than sitting pre-drawn. Closing leaves them at 1, so
    // the next open replays the animation.
    if (st.open && !st.wasOpen) {
        for (int j = 0; j < kMultiSelectMax; ++j) st.checkAnim[j] = 0.0f;
    }
    st.wasOpen = st.open;

    for (int i = 0; i < count; ++i) {
        const Rect item{field.x, listTop + i * itemH, field.w, itemH};
        const bool isSel = (i < kMSMaxItems) && ((st.mask & (1u << i)) != 0);
        const bool held  = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, item);

        // Ease this item's check pop (scale + alpha) toward its selected
        // state every frame, so toggling an item grows/fades the tick in
        // instead of snapping on/off. Frame-rate independent.
        if (i < kMultiSelectMax) {
            const float target = isSel ? 1.0f : 0.0f;
            const float k = 1.0f - expf(-io.DeltaTime * 16.0f);
            st.checkAnim[i] += (target - st.checkAnim[i]) * k;
        }
        const float chkA = (i < kMultiSelectMax) ? st.checkAnim[i]
                                                : (isSel ? 1.0f : 0.0f);

        // Touch-held feedback only — selection is shown by the check, not
        // by a blue fill (which would read like the single-pick dropdown).
        if (held) {
            fg->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                              ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                              fadeColAnim(ControlBgHi, st.anim), kDropdownRound * s);
        }

        // Check mark — ONE rounded polyline (rounded joins AND caps), so it
        // reads as a smooth tick instead of two square-ended stubs. Scales
        // (kChkScaleStart → 1.0) and fades (0 → 1) with this item's own
        // `checkAnim`, so it pops in on select/open and fades on deselect.
        if (chkA > 0.01f) {
            const float checkX = item.x + padX * 1.4f + kMSCheckW * s * 0.5f;
            const float checkY = item.y + item.h * 0.5f;
            const float w = kMSCheckW * s;
            const float h = kMSCheckH * s;
            const float cScale = kChkScaleStart + (1.0f - kChkScaleStart) * chkA;
            const auto cscale = [&](float lx, float ly) {
                return ImVec2(checkX + (lx - checkX) * cScale,
                               checkY + (ly - checkY) * cScale);
            };
            ImVec2 pts[3] = {
                cscale(checkX - w * 0.55f, checkY + h * 0.05f),
                cscale(checkX - w * 0.10f, checkY + h * 0.40f),
                cscale(checkX + w * 0.55f, checkY - h * 0.45f),
            };
            const uint32_t ca = (uint32_t)(255.0f * g_hudAlpha * chkA);
            const ImU32 col = (TextOnAccent & 0x00FFFFFFu) | (ca << IM_COL32_A_SHIFT);
            fg->AddPolyline(pts, 3, col, kMSCheckStroke * s,
                            ImDrawFlags_RoundCornersAll);
        }

        // Label — white on selected rows (reads on ListBg), muted otherwise.
        const ImU32 labelCol = isSel ? TextOnAccent : TextMuted;
        textLeft(fg, item.x + padX * 2.0f + kMSCheckW * s, item.y + item.h * 0.5f,
                 kLabelSize * s, fadeColAnim(labelCol, st.anim), items[i]);
    }

    fg->PopClipRect();
}

void multiSelectList(ImDrawList* dl, const Rect& r, MultiSelectState& st,
                     const char* const* items, int count, float s) {
    if (st.anim <= 0.001f) return;
    paintMultiSelectList(dl, r, st, items, count, s);
}

bool multiSelect(ImDrawList* dl, const Rect& r, MultiSelectState& st,
                 const char* label, const char* const* items, int count,
                 float s, bool drawListInline) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    st.anim = approach(st.anim, st.open ? 1.0f : 0.0f, kMSAnimRate, io.DeltaTime);

    const float cy    = r.y + r.h * 0.5f;
    const float padX  = kDropdownPadX * s;
    const float insetY = kDropdownInsetY * s;
    const Rect  field{r.x + r.w * 0.5f, r.y + insetY, r.w * 0.5f, r.h - insetY * 2.0f};
    const float fieldCy = field.y + field.h * 0.5f;
    const float itemH = kDropdownItemH * s;
    const float listTop = field.y + field.h + kDropdownGap * s;
    const float listH = count * itemH;

    // ── Build the collapsed-state label from selected items ────────────────
    // "(无)" when nothing is selected; otherwise a comma-joined list, capped
    // at kMSMaxChars (mid-item truncation adds an ellipsis so the user can
    // tell there's more than what's shown).
    std::string collapsed;
    int selectedCount = 0;
    int shown = 0;
    int used = 0;
    bool first = true;
    for (int i = 0; i < count; ++i) {
        if ((st.mask & (1u << i)) == 0) continue;
        ++selectedCount;
        const char* it = items[i];
        const int len = (int)strlen(it);
        const int addLen = len + (first ? 0 : 2);
        if (used + addLen > kMSMaxChars) break;
        if (!first) collapsed += ", ";
        collapsed += it;
        used += addLen;
        first = false;
        ++shown;
    }
    if (selectedCount == 0) {
        collapsed = "(无)";
    } else if (shown < selectedCount) {
        collapsed += "…";
    }

    // ── Hit test ───────────────────────────────────────────────────────────
    // Priority: a tap on an open-list item flips its bit *and* keeps the list
    // open (multi-select convention). A tap on the row toggles open/close.
    // A tap outside both closes.
    int toggledIdx = -1;
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (released) {
        if (st.open) {
            for (int i = 0; i < count; ++i) {
                const Rect item{field.x, listTop + i * itemH, field.w, itemH};
                if (inside(mouse, item)) { toggledIdx = i; break; }
            }
        }
        if (toggledIdx >= 0) {
            if (toggledIdx < kMSMaxItems) {
                st.mask ^= (1u << toggledIdx);
            }
            // Keep open — multi-select does not auto-close.
        } else if (inside(mouse, r)) {
            st.open = !st.open;
        } else if (st.open) {
            st.open = false;
        }
    }

    // ── Row ────────────────────────────────────────────────────────────────
    textLeft(dl, r.x, cy, kLabelSize * s, TextPrimary, label);

    const bool rowHeld = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, r);
    dl->AddRectFilled(ImVec2(field.x, field.y),
                      ImVec2(field.x + field.w, field.y + field.h),
                      fadeCol(rowHeld ? ControlBgHi : ControlBg), kDropdownRound * s);

    textLeft(dl, field.x + padX, fieldCy, kValueSize * s,
             selectedCount > 0 ? TextPrimary : TextMuted, collapsed.c_str());

    // Chevron — same counter-clockwise opening as the dropdown.
    const float chevHalfW = kChevronW * s * 0.5f;
    const float chevHalfH = kChevronH * s * 0.5f;
    const float chevStroke = kChevronThick * s * 0.5f;
    const float chevCx = field.x + field.w - padX - chevHalfW - chevStroke;
    const float ca = st.anim * -kPi;
    const float cc = cosf(ca);
    const float cs = sinf(ca);
    auto turn = [&](float px, float py) {
        return ImVec2(chevCx + px * cc - py * cs, fieldCy + px * cs + py * cc);
    };
    const ImVec2 apex = turn(0.0f, chevHalfH);
    roundSegment(dl, apex, turn(-chevHalfW, -chevHalfH), chevStroke, TextMuted);
    roundSegment(dl, apex, turn(chevHalfW, -chevHalfH), chevStroke, TextMuted);

    // ── Open list ──────────────────────────────────────────────────────────
    // Inline mode only. The board paints into the foreground draw list, so
    // an inline list is covered by rows drawn later in the same frame (that
    // was 瞄准类别 under the Y死区 sliders). Callers with rows below the
    // multi-select pass drawListInline=false and paint the list last via
    // multiSelectList() instead (the Add-Model-dialog pattern).
    if (st.anim > 0.001f && drawListInline) {
        paintMultiSelectList(ImGui::GetForegroundDrawList(), r, st, items, count, s);
    }

    return toggledIdx >= 0;
}

// ──────────────────────────────────────────────────────────────────────────
//  Buttons, text fields, path pill (the Add-Model dialog widgets)
// ─────────────────────────────────────────────────────────────────────────
//
// Why are these widgets and not parts of a new "Page" in [hud]? Because the
// dialog is a page that *covers* a page, not a list — it owns its own touch
// routing, has its own outside-tap semantics, and lives in the same hit-test
// vocabulary as the row controls, so it makes sense for it to consume the same
// `widgets::*` namespace.

constexpr float kButtonHeightDesign = 80.0f;     // tall enough for a finger
constexpr float kButtonRoundDesign  = 14.0f;
constexpr float kButtonPressEaseRate = 22.0f;
constexpr float kGhostOutlineAlpha  = 70;          // out of 255
constexpr ImU32 kDangerFill = IM_COL32(204, 51, 68, 255);   // soft red

// A Ghost button used to be "white at alpha 22" — a fill so faint it read as
// a hole, and (once fadeCol stopped forcing it opaque) still too close to the
// pane to find with a finger. It is a real slate now: one step above the pane
// so the rectangle is visible, with a border to give it an edge to aim at.
constexpr ImU32 kGhostFill    = IM_COL32( 58,  60,  70, 255);
constexpr ImU32 kGhostFillHi  = IM_COL32( 78,  81,  92, 255);
constexpr ImU32 kDisabledFill = IM_COL32( 40,  41,  48, 255);

constexpr float kFieldHeightDesign = 78.0f;
constexpr float kFieldRoundDesign  = 12.0f;
constexpr float kFieldPadDesign    = 18.0f;
constexpr float kCaretWidthDesign  = 2.0f;

constexpr float kPathPillHeightDesign = 78.0f;
constexpr float kPathPillBtnSizeDesign = 60.0f;
constexpr float kPathPillBtnGapDesign  = 10.0f;
constexpr ImU32 kPathPillBtnFill = IM_COL32(60, 62, 70, 255);
constexpr ImU32 kPathPillBtnFillHi = IM_COL32(78, 81, 92, 255);

constexpr float kDialogSidePadDesign   = 36.0f;   // board-left/right pad for the dialog
constexpr float kDialogTopPadDesign    = 30.0f;
constexpr float kDialogHeaderHeightDesign = 90.0f;
constexpr float kDialogInnerPadDesign  = 26.0f;

// Holds the eased 0..1 for a button's press, kept across calls by reference.
// (The state lives on the page, but the per-frame math is here so the
// caller can't accidentally apply two eases at once.)
struct ButtonAnim { float press = 0.0f; };

/** True when the cursor is inside the rect (exclusive of right/bottom edge). */
bool hit(const ImVec2& p, const Rect& r) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

/** Returns true the frame the finger goes down inside the rect. */
bool tapped(const Rect& r) {
    const ImGuiIO& io = ImGui::GetIO();
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hit(io.MousePos, r);
    // (single-tap release rule would need a Held+Released pair; the menu has
    //  no hover, so a press that lands inside IS the user's intent)
}

// ── Click (press-inside + release-inside) ────────────────────────────────────
// Buttons used to fire on press-down (tapped() above). That leaks gestures
// across layers: the press that opens a dialog is still down when the dialog
// appears, so its controls fire without the finger ever lifting — tapping
// "Add Model" could land on the dialog background and close it again. Every
// button now fires only when the press began inside it AND the release lands
// inside it (the convention dropdown/multi-select already used).
namespace {
ImVec2 g_pressPos{0.0f, 0.0f};
bool   g_pressValid = false;
}  // namespace

/// Remembers the press origin once per gesture. Idempotent — every widget
/// calls it on its way to clicked()/clickedOutside(), whichever runs first
/// on the press frame wins and the rest just re-read the latch.
///
/// NOTE: the latch is cleared ONLY by consumeTap(), never implicitly here.
/// Clearing on !IsMouseDown would wipe it on the release frame before
/// clicked() gets to read it (release implies the finger is already up) —
/// which is exactly what made every button dead. A stale latch is harmless:
/// every new press re-latches on its IsMouseClicked frame, and a release
/// without a preceding press (finger already down at startup) finds the latch
/// invalid from the start.
void pollPressOrigin() {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_pressPos   = io.MousePos;
        g_pressValid = true;
    }
}

/** True the frame the finger lifts inside `r` after pressing down inside it. */
bool clicked(const Rect& r) {
    pollPressOrigin();
    const ImGuiIO& io = ImGui::GetIO();
    return g_pressValid && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
           hit(io.MousePos, r) && hit(g_pressPos, r);
}

/** Drops the in-flight gesture, so a layer transition (dialog open/close)
/// never hands its release to the layer underneath. */
void consumeTap() {
    g_pressValid = false;
}

/** True the frame a tap fully outside `r` is released (press AND release
/// outside) — the scrim-dismiss gesture for dialogs. Requiring the press to
/// start outside keeps a slider drag that slides out of the dialog from
/// counting as a dismiss. */
bool clickedOutside(const Rect& r) {
    pollPressOrigin();
    const ImGuiIO& io = ImGui::GetIO();
    return g_pressValid && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
           !hit(io.MousePos, r) && !hit(g_pressPos, r);
}

/** Drive the press ease for the duration the finger is on the button. */
void drivePress(const Rect& r, ButtonAnim& a, float dt) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && hit(io.MousePos, r);
    const float target = held ? 1.0f : 0.0f;
    if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
    a.press += (target - a.press) * (1.0f - expf(-kButtonPressEaseRate * dt));
}

/** Paints one button. Split out so a variant that never fires (Disabled) and
 *  the live variants share exactly one place the colours are written down. */
void paintButton(ImDrawList* dl, const Rect& r, const char* label,
                 ButtonVariant variant, float s, bool held);

bool button(ImDrawList* dl, const Rect& r, const char* label,
            ButtonVariant variant, float s) {
    if (r.w <= 1.0f || r.h <= 1.0f) return false;
    if (variant == ButtonVariant::Disabled) {
        paintButton(dl, r, label, variant, s, false);
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && hit(io.MousePos, r);

    paintButton(dl, r, label, variant, s, held);
    return clicked(r);
}

void paintButton(ImDrawList* dl, const Rect& r, const char* label,
                 ButtonVariant variant, float s, bool held) {
    ImFont* font = ImGui::GetFont();
    const float rounding = kButtonRoundDesign * s;
    const ImVec2 mn(r.x, r.y);
    const ImVec2 mx(r.x + r.w, r.y + r.h);

    ImU32 fill, edge, label_col;
    switch (variant) {
        case ButtonVariant::Primary:
            fill = Accent;
            edge = Accent;
            label_col = TextOnAccent;
            break;
        case ButtonVariant::Danger:
            fill = kDangerFill;
            edge = kDangerFill;
            label_col = TextOnAccent;
            break;
        case ButtonVariant::Disabled:
            fill = kDisabledFill;
            edge = IM_COL32(255, 255, 255, 28);
            label_col = TextMuted;
            break;
        case ButtonVariant::Ghost:
        default:
            fill = held ? kGhostFillHi : kGhostFill;
            edge = IM_COL32(255, 255, 255, kGhostOutlineAlpha);
            label_col = TextPrimary;
            break;
    }

    dl->AddRectFilled(mn, mx, fadeCol(fill), rounding);
    dl->AddRect(mn, mx, fadeCol(edge), rounding, 0, 1.0f * s);

    const float sz = 28.0f * s;
    const ImVec2 ts = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, label);
    dl->AddText(font, sz,
                ImVec2(mn.x + (r.w - ts.x) * 0.5f,
                       mn.y + (r.h - ts.y) * 0.5f),
                fadeCol(label_col), label);
}

bool closeButton(ImDrawList* dl, const Rect& r, float s) {
    if (r.w <= 1.0f || r.h <= 1.0f) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && hit(io.MousePos, r);

    // Same shell as a Ghost button, but the mark is two strokes rather than a
    // glyph: the "X" letter is drawn from whatever the font happens to have and
    // sits badly inside a square, while two lines can be centred exactly.
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h),
                      fadeCol(held ? kGhostFillHi : kGhostFill),
                      kButtonRoundDesign * s);
    dl->AddRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h),
                fadeCol(IM_COL32(255, 255, 255, kGhostOutlineAlpha)),
                kButtonRoundDesign * s, 0, 1.0f * s);

    const float inset = r.w * 0.30f;
    const float w = 3.2f * s;
    const ImU32 col = fadeCol(TextPrimary);
    dl->AddLine(ImVec2(r.x + inset, r.y + inset),
                ImVec2(r.x + r.w - inset, r.y + r.h - inset), col, w);
    dl->AddLine(ImVec2(r.x + r.w - inset, r.y + inset),
                ImVec2(r.x + inset, r.y + r.h - inset), col, w);

    return clicked(r);
}

// ── Text fields ────────────────────────────────────────────────────────────
//
// The field never queries the keyboard: it is a view-only widget. Editing
// (caret tracking, character insertion, backspace) is owned by the caller —
// either an on-screen keyboard drawn inside the overlay, or an external IME
// the caller pipes back through its own machinery.

/** Splits `buf[len]` into graphemes and inserts a printable ASCII char at
 *  the caret. Multi-byte UTF-8 is kept intact so Chinese class names go in.
 *
 *  Not currently called — the renderer never types; the App-side IME owns the
 *  buffer while the keyboard is open and the result comes back as a single
 *  string. Kept here because typing *into* a multi-line field eventually lives
 *  on the renderer thread (else the caret could not follow an in-board cursor).
 */
[[maybe_unused]] static void insertChar(DialogField& f, const char* txt, int n) {
    if (n <= 0 || f.len + n > static_cast<int>(sizeof(f.buf)) - 1) return;
    memmove(&f.buf[f.caret + n], &f.buf[f.caret], f.len - f.caret);
    memcpy(&f.buf[f.caret], txt, n);
    f.caret += n;
    f.len   += n;
    f.buf[f.len] = '\0';
}

[[maybe_unused]] static void backspace(DialogField& f) {
    if (f.caret <= 0) return;
    int n = 1;
    while (f.caret - n > 0 && (f.buf[f.caret - n] & 0xC0) == 0x80) ++n;
    memmove(&f.buf[f.caret - n], &f.buf[f.caret], f.len - f.caret);
    f.caret -= n;
    f.len   -= n;
    f.buf[f.len] = '\0';
}

[[maybe_unused]] static void deleteForward(DialogField& f) {
    if (f.caret >= f.len) return;
    int n = 1;
    while (f.caret + n < f.len && (f.buf[f.caret + n] & 0xC0) == 0x80) ++n;
    memmove(&f.buf[f.caret], &f.buf[f.caret + n], f.len - f.caret - n);
    f.len -= n;
    f.buf[f.len] = '\0';
}

/** Renders the field's text and the caret, clipped to the box. */
void paintField(ImDrawList* dl, const Rect& r, const DialogField& f,
                const char* placeholder, float s, bool multiline) {
    ImFont* font = ImGui::GetFont();
    const float sz     = 24.0f * s;
    const float pad    = kFieldPadDesign * s;
    const float rounding = kFieldRoundDesign * s;
    const ImVec2 mn(r.x, r.y);
    const ImVec2 mx(r.x + r.w, r.y + r.h);
    const bool   empty = f.len == 0;

    dl->AddRectFilled(mn, mx,
        fadeCol(f.focused ? lerpColor(ControlBg, ControlBgHi, 0.5f) : ControlBg),
        rounding);
    dl->AddRect(mn, mx,
        fadeCol(f.focused ? Accent : Edge),
        rounding, 0, 1.0f * s);

    // Text and caret share one clip rect — without it a long path-name would
    // spill out the right edge and over the ... button.
    dl->PushClipRect(mn, mx, true);
    if (empty) {
        dl->AddText(font, sz, ImVec2(mn.x + pad, mn.y + (r.h - sz) * 0.5f),
                    fadeCol(TextMuted), placeholder);
    } else {
        const float vCentre = multiline ? pad : (r.h - sz) * 0.5f;
        dl->AddText(font, sz, ImVec2(mn.x + pad, mn.y + vCentre),
                    fadeCol(TextPrimary), f.buf);
    }
    if (f.focused && (static_cast<int>(ImGui::GetTime() * 2.0f) & 1) == 0) {
        // The caret sits at x = text_caret, but we have x in pixels and carets
        // in bytes; multiply by font's "caret column width" approximation.
        const float colW = sz * 0.55f;
        // For multiline we only honour the column on the last line — works
        // well enough for a class-names box.
        const float caretX = mn.x + pad + f.caret * colW;
        dl->AddRectFilled(ImVec2(caretX, mn.y + (r.h - sz) * 0.5f),
                          ImVec2(caretX + kCaretWidthDesign * s, mn.y + (r.h + sz) * 0.5f),
                          fadeCol(TextPrimary));
    }
    dl->PopClipRect();
}

/** Single-line text input. `placeholder` is shown when the box is empty. */
bool textField(ImDrawList* dl, const Rect& r, DialogField& f,
               const char* placeholder, float s) {
    if (r.w <= 1.0f || r.h <= 1.0f) return false;

    const ImGuiIO& io = ImGui::GetIO();
    bool focusChanged = false;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const bool inside = hit(io.MousePos, r);
        const bool wasFocused = f.focused;
        f.focused = inside;
        // First-frame caret: drop the caret at the end of the existing text.
        if (inside && !wasFocused) f.caret = f.len;
        focusChanged = wasFocused != inside;
    }
    paintField(dl, r, f, placeholder, s, /*multiline=*/false);
    return focusChanged;
}

/** Multi-line text input: identical hit-test rules, paints one line per row. */
bool multilineField(ImDrawList* dl, const Rect& r, DialogField& f,
                    const char* placeholder, float s) {
    if (r.w <= 1.0f || r.h <= 1.0f) return false;
    paintField(dl, r, f, placeholder, s, /*multiline=*/true);
    return false;   // view-only widget: the caller drives editing
}

/** A pill with the model path on the left and a "..." button on the right.
 *  The pill greys out while the file picker is open elsewhere. */
bool pathPill(ImDrawList* dl, const Rect& r, const char* path, int pathLen,
              bool picking, float s) {
    if (r.w <= 1.0f || r.h <= 1.0f) return false;
    ImFont* font = ImGui::GetFont();
    const float sz       = 22.0f * s;
    const float pad      = kFieldPadDesign * s;
    const float rounding = kFieldRoundDesign * s;
    const float btnSize  = kPathPillBtnSizeDesign * s;
    const float btnGap   = kPathPillBtnGapDesign * s;
    const ImVec2 mn(r.x, r.y);
    const ImVec2 mx(r.x + r.w, r.y + r.h);

    // The button lives *inside* the pill on the right.
    const ImVec2 btnMn(mx.x - btnSize - pad * 0.5f, mn.y + (r.h - btnSize) * 0.5f);
    const ImVec2 btnMx(mx.x - pad * 0.5f,        btnMn.y + btnSize);
    const Rect btnRect{btnMn.x, btnMn.y, btnSize, btnSize};

    const Rect pillRect{mn.x, mn.y, mx.x - mn.x - btnSize - btnGap, r.h};
    const ImVec2 pillMin(pillRect.x, pillRect.y);
    const ImVec2 pillMax(pillRect.x + pillRect.w, pillRect.y + pillRect.h);
    dl->AddRectFilled(pillMin, pillMax,
                      fadeCol(picking ? lerpColor(ControlBg, ControlBgHi, 0.5f) : ControlBg),
                      rounding);
    dl->AddRect(pillMin, pillMax, fadeCol(picking ? Accent : Edge), rounding, 0, 1.0f * s);

    // Path text — clipped so an absurd long file name does not overlap the ...
    if (path && pathLen > 0) {
        dl->PushClipRect(ImVec2(pillRect.x + pad, pillRect.y),
                         ImVec2(pillRect.x + pillRect.w - pad,
                                pillRect.y + pillRect.h),
                         true);
        // Truncate from the *right*: a path that's too long is more useful with
        // its tail visible (the file) than its head (parent dirs).
        char line[256];
        if (pathLen > static_cast<int>(sizeof(line)) - 4) {
            // "..." + first (shown - 3) chars.
            line[0] = line[1] = line[2] = '.';
            memcpy(line + 3, path, sizeof(line) - 4 - 3);
            line[sizeof(line) - 1] = '\0';
        } else {
            memcpy(line, path, pathLen);
            line[pathLen] = '\0';
        }
        dl->AddText(font, sz,
                    ImVec2(pillRect.x + pad, pillRect.y + (pillRect.h - sz) * 0.5f),
                    fadeCol(TextPrimary), line);
        dl->PopClipRect();
    } else {
        dl->AddText(font, sz,
                    ImVec2(pillRect.x + pad, pillRect.y + (pillRect.h - sz) * 0.5f),
                    fadeCol(TextMuted), picking ? "opening…" : "(no model selected)");
    }

    // The "..." button — a small disc.
    const bool btnHeld = ImGui::IsMouseDown(ImGuiMouseButton_Left) && hit(
        ImGui::GetIO().MousePos, btnRect);
    dl->AddRectFilled(btnMn, btnMx,
                      fadeCol(btnHeld ? kPathPillBtnFillHi : kPathPillBtnFill),
                      btnSize * 0.5f);
    // Three dots centred.
    const float dotR = 3.0f * s;
    const float cy = (btnMn.y + btnMx.y) * 0.5f;
    const float cx = (btnMn.x + btnMx.x) * 0.5f;
    const float off = 8.0f * s;
    dl->AddCircleFilled(ImVec2(cx - off, cy), dotR, fadeCol(TextPrimary), 12);
    dl->AddCircleFilled(ImVec2(cx,        cy), dotR, fadeCol(TextPrimary), 12);
    dl->AddCircleFilled(ImVec2(cx + off, cy), dotR, fadeCol(TextPrimary), 12);

    return clicked(btnRect);
}

// ── Dialog layout ──────────────────────────────────────────────────────────
//
// The page asks "where should this dialog sit" and gets a single rect. The
// content inside is the page's responsibility — widgets here cover the
// controls the dialog holds; the modal frame (shadow, header strip, "X" close)
// is the page's, because only the page knows what to title the dialog.

Rect dialogRect(const HudRect& board, float s) {
    Rect r;
    if (!board.valid()) return r;
    const float sidePad  = kDialogSidePadDesign * s;
    const float topPad   = kDialogTopPadDesign  * s;
    const float botPad   = kDialogTopPadDesign  * s;
    const float maxW     = 1300.0f * s;
    r.w = board.w - 2.0f * sidePad;
    if (r.w > maxW) r.w = maxW;
    r.x = board.x + (board.w - r.w) * 0.5f;
    r.y = board.y + topPad;
    r.h = board.h - topPad - botPad;
    if (r.h < 200.0f * s) r.h = 200.0f * s;
    return r;
}

float dialogHeaderHeight(float s) { return kDialogHeaderHeightDesign * s; }
float dialogInnerPad(float s)     { return kDialogInnerPadDesign * s; }

}  // namespace widgets
}  // namespace ui
}  // namespace aimbotng
#endif  // HAS_IMGUI
