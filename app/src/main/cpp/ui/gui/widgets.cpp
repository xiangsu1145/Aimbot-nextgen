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

// Set the moment any widget acts on the press/release this frame. Lets every
// other widget bail out so a tap cannot leak through to a control painted
// underneath (the "触摸穿透" fix). Reset once per frame by beginFrame().
//
// Lives at namespace scope rather than inside the per-file anonymous namespace
// because the switch code sits *between* the two anonymous namespaces below
// and needs to read this flag too. Same goes for the press-origin latch
// (g_pressPos / g_pressValid) — they would be invisible from that gap, so
// they live here too. pollPressOrigin() itself stays in the second anonymous
// namespace with the other click helpers, since the switch code calls it but
// does not need its symbol to be visible — only to be reachable, which is the
// same thing once `g_gestureConsumed` and the latch are visible above.
bool g_gestureConsumed = false;
ImVec2 g_pressPos{0.0f, 0.0f};
bool   g_pressValid = false;
/// Set the frame a widget grabs a drag (e.g. slider press-down). Stays set
/// on every subsequent frame until the drag releases, so the page's scroll
/// handler can leave the page still while the thumb is moving under the
/// finger. Reset every frame by beginFrame() before any widget runs.
bool   g_widgetDragging = false;

// Forward-declared so the switch code in the gap between the two anonymous
// namespaces can call it; the definition lives further down in the second
// anonymous namespace near the click helpers.
void pollPressOrigin();

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

// ── Dropdown / multi-select shared placement ─────────────────────────────────
//
// The open list expands downward from the selector by default. When the full
// list would run past the bottom of the screen it is cut off there (the list
// paints on the foreground draw list, so the panel edge is the only clip) —
// the bottom entries would be unreachable. In that case, and when there is
// actually room above, the list opens upward from above the field instead.
// Both the paint paths and the hit-test paths take their `listTop` from here,
// so they can never disagree about where the items are.
struct DropdownGeom {
    Rect  field;    // the selector pill inside the row
    float listTop;  // top edge of the open list
    bool  upward;   // true = opened upward (reveal grows from the bottom edge)
};

DropdownGeom dropdownGeom(const Rect& r, int count, float s) {
    const float insetY = csize(kDropdownInsetY) * s;
    DropdownGeom g;
    g.field = Rect{r.x + r.w * 0.5f, r.y + insetY, r.w * 0.5f, r.h - insetY * 2.0f};
    g.upward = false;

    const float listH  = count * csize(kDropdownItemH) * s;
    const float gap    = csize(kDropdownGap) * s;
    const float downTop = g.field.y + g.field.h + gap;
    const float screenH = ImGui::GetIO().DisplaySize.y;

    if (downTop + listH > screenH - gap &&
        g.field.y - gap - listH >= 0.0f) {
        g.upward  = true;
        g.listTop = g.field.y - gap - listH;   // bottom edge sits just above the field
    } else {
        g.listTop = downTop;
    }
    return g;
}

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
    size = tsize(size);  // readability boost — measure and draw on the same size
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->AddText(f, size, ImVec2(x, centreY - ts.y * 0.5f), fadeCol(col), text);
}

void textRight(ImDrawList* dl, float xRight, float centreY, float size, ImU32 col, const char* text) {
    ImFont* f = ImGui::GetFont();
    size = tsize(size);  // readability boost — measure and draw on the same size
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
 *
 * `trackRect` (out) receives the groove's rectangle in the same coordinate
 * space as `r`, so the caller can hit-test the toggle on its actual visual
 * target instead of the row's full bounding box. The hit area is the groove
 * itself, NOT the whole row — a tap on the label or the empty space beside
 * the track is inert, which is what makes the switch feel precise.
 */
void drawSwitchTrack(ImDrawList* dl, const Rect& r, float anim, float s,
                     Rect* trackRect, float dim = 1.0f) {
    const float cy     = r.y + r.h * 0.5f;
    const float trackW = csize(kSwitchTrackW) * s;
    const float trackH = csize(kSwitchTrackH) * s;
    const ImVec2 min(r.x + r.w - trackW, cy - trackH * 0.5f);

    // `dim` lerps the accent contribution toward zero so an "off because the
    // caller decided it cannot fire" switch still reads as a switch but in
    // the muted-track colour, not the on colour. Default 1.0 leaves the
    // active state untouched — only switchToggle() with interactive=false
    // passes a smaller value.
    dl->AddRectFilled(min, ImVec2(min.x + trackW, min.y + trackH),
                      fadeCol(lerpColor(Track, Accent, anim * dim)),
                      trackH * 0.5f);

    const float knobX = min.x + trackH * 0.5f + (trackW - trackH) * anim;
    // Knob alpha tracks dim the same way so the two halves fade together —
    // a half-bright knob on a half-bright track would read as a broken
    // gradient rather than a "disabled" state.
    const uint32_t knobA = static_cast<uint32_t>(
        (static_cast<float>((Knob >> IM_COL32_A_SHIFT) & 0xFFu)) * dim);
    const ImU32 knob = (Knob & 0x00FFFFFFu) | (knobA << IM_COL32_A_SHIFT);
    dl->AddCircleFilled(ImVec2(knobX, cy), csize(kSwitchKnobR) * s, fadeCol(knob));

    if (trackRect != nullptr) {
        trackRect->x = min.x;
        trackRect->y = min.y;
        trackRect->w = trackW;
        trackRect->h = trackH;
    }
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
    Rect track{};
    drawSwitchTrack(dl, r, st.anim, s, &track);

    // Hit area = the groove's own rectangle, NOT the row. The label and the
    // empty space beside the track are inert — the switch only flips when the
    // finger lands on the visible toggle. A small vertical pad lets a thumb
    // hit the rounded ends without having to land the pixel on the curve.
    const float padY = 10.0f * s;
    const Rect hit{track.x, track.y - padY, track.w, track.h + padY * 2.0f};

    // Fire on release: a finger that lands here, slides off and lifts should
    // not toggle anything, and there is no hover to tell us that on touch.
    // Also require the press to have *begun* inside the hit area — otherwise
    // a scroll drag that starts on empty space and ends over the switch
    // would flip it. A prior widget that already claimed the gesture skips us.
    bool changed = false;
    if (!g_gestureConsumed) {
        pollPressOrigin();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            g_pressValid && inside(g_pressPos, hit) && inside(mouse, hit)) {
            st.value = !st.value;
            changed = true;
            consumeGesture();
        }
    }
    return changed;
}

bool switchToggle(ImDrawList* dl, const Rect& r, SwitchState& st, float s,
                  bool interactive) {
    st.anim = approach(st.anim, st.value ? 1.0f : 0.0f, kSwitchAnimRate,
                       ImGui::GetIO().DeltaTime);
    // When the caller says "do not fire" we still paint the track, but in the
    // muted colour rather than the accent one — the user can see the row is
    // there and which way it is pointing, while a tap that lands on it is
    // ignored. 0.0 would draw a flat muted track; 0.45 keeps enough accent
    // bleed that the "on" position is still distinguishable from the "off".
    const float dim = interactive ? 1.0f : 0.45f;
    Rect track{};
    drawSwitchTrack(dl, r, st.anim, s, &track, dim);

    // Hit area = the track itself, not the padded caller rect. The caller
    // passes `r` that already includes some touch-pad (a finger is bigger
    // than the toggle), so we re-derive the visual target instead of trusting
    // `r`. A small vertical pad keeps the rounded ends easy to hit.
    const float padY = 10.0f * s;
    const Rect hit{track.x, track.y - padY, track.w, track.h + padY * 2.0f};

    // Same release semantics as the labelled switch — see switchButton.
    // `interactive` lets a modal layer above paint the switch dead: it still
    // animates, it just stops listening.
    bool changed = false;
    if (interactive && !g_gestureConsumed) {
        pollPressOrigin();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            g_pressValid && inside(g_pressPos, hit) && inside(ImGui::GetIO().MousePos, hit)) {
            st.value = !st.value;
            changed = true;
            consumeGesture();
        }
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

    // Hit area = the track itself, with a generous vertical pad so a finger
    // can land near the groove without having to be precise. The label and
    // the value readout are inert — tapping them does nothing. Skip when a
    // sibling has already claimed the gesture (touch-through fix).
    const float titleCy = r.y + csize(kSliderTitleCy) * s;
    const float trackCy = r.y + csize(kSliderTrackCy) * s;
    const float trackH  = csize(kSliderTrackH) * s;
    const float thumbR  = csize(kSliderThumbR) * s * (1.0f + kSliderThumbGrow * st.press);
    const float trackX0 = r.x + csize(kSliderThumbR) * s;
    const float trackX1 = r.x + r.w - csize(kSliderThumbR) * s;
    const float trackW  = trackX1 - trackX0;
    // Vertical pad = thumb radius + a bit, so the rounded ends of the thumb
    // are as easy to land on as the line. Horizontal = the thumb at rest.
    const float hitPadY = (kSliderThumbR + 6.0f) * s;
    const Rect hit{trackX0 - thumbR, trackCy - hitPadY,
                   trackW + thumbR * 2.0f, hitPadY * 2.0f};
    if (!st.dragging && !g_gestureConsumed &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside(mouse, hit)) {
        st.dragging = true;
        consumeGesture();
        // Mark a drag is in flight for as long as the finger is down. The
        // page's scroll handler reads this and keeps the page still while the
        // user is moving the thumb — otherwise the page would scroll under
        // the finger while the slider was being dragged.
        setWidgetDragging();
    }
    const bool dragging = st.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // The thumb breathes: it swells under the finger and settles back on
    // release, on the same decelerating curve as everything else here. The
    // *mapping* below always uses the resting radius, so the geometry the
    // finger is read against does not shift while the thumb is growing.
    st.press = approach(st.press, dragging ? 1.0f : 0.0f, kSliderPressRate, io.DeltaTime);

    // Geometry already computed above (hit area uses the same constants).

    const float before = st.value;

    if (dragging) {
        // While the finger is down it writes the *target* only. The value is
        // derived from it so the readout tracks the finger live.
        st.target = clamp01((mouse.x - trackX0) / trackW);
        st.value = st.min + st.target * span;
    } else if (st.dragging) {
        st.dragging = false;
        clearWidgetDragging();
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
    return count * csize(kDropdownItemH) * s;
}

bool dropdown(ImDrawList* dl, const Rect& r, DropdownState& st,
              const char* label, const char* const* items, int count, float s,
              bool drawListInline, const bool* disabled) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    st.anim = approach(st.anim, st.open ? 1.0f : 0.0f, kDropdownAnimRate, io.DeltaTime);

    const float cy    = r.y + r.h * 0.5f;
    const float padX  = csize(kDropdownPadX) * s;
    const float itemH = csize(kDropdownItemH) * s;

    const DropdownGeom geom = dropdownGeom(r, count, s);
    const Rect  field = geom.field;
    const float fieldCy = field.y + field.h * 0.5f;

    const float listTop = geom.listTop;
    const float listH   = count * itemH;
    const float shownH  = listH * st.anim;

    const char* current = (st.value >= 0 && st.value < count) ? items[st.value] : "";

    // ── Hit test first, so a tap on an item is not also read as a tap on the
    //    row underneath it ────────────────────────────────────────────────────
    //
    // Press-down pre-claim: a slider painted right below would otherwise grab
    // the press on the press-down frame (sliders grab on press-down, not
    // release) and continue to drag through the release. Pre-claiming here
    // keeps the slider quiet. See the matching fix in multiSelect().
    if (!g_gestureConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool inMe = inside(mouse, r);
        if (st.open) {
            for (int i = 0; i < count; ++i) {
                const Rect item{field.x, listTop + i * itemH, field.w, itemH};
                if (inside(mouse, item)) { inMe = true; break; }
            }
        }
        if (inMe) consumeGesture();
    }

    int picked = -1;
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (!g_gestureConsumed && released) {
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
            consumeGesture();            // claim it so the row below doesn't also fire
        } else if (inside(mouse, r)) {
            st.open = !st.open;          // tapping the row toggles the list
            consumeGesture();
        } else if (st.open) {
            st.open = false;             // tapping anywhere else closes it
            consumeGesture();
        }
    }

    // ── Row ──────────────────────────────────────────────────────────────────
    textLeft(dl, r.x, cy, kLabelSize * s, TextPrimary, label);

    const bool rowHeld = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, r);
    dl->AddRectFilled(ImVec2(field.x, field.y),
                      ImVec2(field.x + field.w, field.y + field.h),
                      fadeCol(rowHeld ? ControlBgHi : ControlBg), csize(kDropdownRound) * s);

    const float chevHalfW = csize(kChevronW) * s * 0.5f;
    const float chevHalfH = csize(kChevronH) * s * 0.5f;
    const float chevStroke = csize(kChevronThick) * s * 0.5f;
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
        // Reveal grows out of the field's edge: downward lists grow from the
        // top, upward lists from the bottom — either way the anchored edge is
        // the one touching the field.
        const float clipY0 = geom.upward ? (listMax.y - shownH) : listMin.y;
        fg->PushClipRect(ImVec2(listMin.x, clipY0),
                         ImVec2(listMax.x, clipY0 + shownH), true);

        fg->AddRectFilled(listMin, listMax, fadeCol(ListBg), csize(kDropdownRound) * s);

        for (int i = 0; i < count; ++i) {
            const Rect item{field.x, listTop + i * itemH, field.w, itemH};
            const bool selected = (i == st.value);
            const bool isDisabled = (disabled != nullptr && disabled[i]);
            const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, item);

            if (selected && !isDisabled) {
                fg->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                                  ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                                  fadeCol(Accent), csize(kDropdownRound) * s);
            } else if (held && !isDisabled) {
                fg->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                                  ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                                  fadeCol(ControlBgHi), csize(kDropdownRound) * s);
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

    const float padX  = csize(kDropdownPadX) * s;
    const float itemH = csize(kDropdownItemH) * s;

    const DropdownGeom geom = dropdownGeom(r, count, s);
    const Rect field = geom.field;
    const float listTop = geom.listTop;
    const float listH   = count * itemH;
    const float shownH  = listH * st.anim;

    const ImVec2 listMin(field.x, listTop);
    const ImVec2 listMax(field.x + field.w, listTop + listH);
    // Same directional reveal as the inline path in dropdown().
    const float clipY0 = geom.upward ? (listMax.y - shownH) : listMin.y;
    dl->PushClipRect(ImVec2(listMin.x, clipY0),
                     ImVec2(listMax.x, clipY0 + shownH), true);

    dl->AddRectFilled(listMin, listMax, fadeCol(ListBg), csize(kDropdownRound) * s);

    for (int i = 0; i < count; ++i) {
        const Rect item{field.x, listTop + i * itemH, field.w, itemH};
        const bool selected = (i == st.value);
        const bool isDisabled = (disabled != nullptr && disabled[i]);
        const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, item);

        if (selected && !isDisabled) {
            dl->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                              ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                              fadeCol(Accent), csize(kDropdownRound) * s);
        } else if (held && !isDisabled) {
            dl->AddRectFilled(ImVec2(item.x + padX * 0.5f, item.y + 4.0f * s),
                              ImVec2(item.x + item.w - padX * 0.5f, item.y + item.h - 4.0f * s),
                              fadeCol(ControlBgHi), csize(kDropdownRound) * s);
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

    const float padX  = csize(kDropdownPadX) * s;
    const float itemH = csize(kDropdownItemH) * s;
    const DropdownGeom geom = dropdownGeom(r, count, s);
    const Rect  field = geom.field;
    const float listTop = geom.listTop;
    const float listH = count * itemH;

    // Reveal like dropdown(): a clip rect that grows out of the field's edge
    // with `anim` (downward lists grow from the top, upward ones from the
    // bottom), no whole-list scale. The panel background is a steady `ListBg`
    // slab; rows appear to drop down from the row rather than the block
    // squashing.
    const float shownH = listH * st.anim;
    const ImVec2 listMin(field.x, listTop);
    const ImVec2 listMax(field.x + field.w, listTop + listH);
    const float clipY0 = geom.upward ? (listMax.y - shownH) : listMin.y;
    fg->PushClipRect(ImVec2(listMin.x, clipY0),
                     ImVec2(listMax.x, clipY0 + shownH), true);

    // Panel background — dropdown-style, no per-item blue.
    fg->AddRectFilled(listMin, listMax, fadeCol(ListBg), csize(kDropdownRound) * s);

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
                              fadeColAnim(ControlBgHi, st.anim), csize(kDropdownRound) * s);
        }

        // Check mark — ONE rounded polyline (rounded joins AND caps), so it
        // reads as a smooth tick instead of two square-ended stubs. Scales
        // (kChkScaleStart → 1.0) and fades (0 → 1) with this item's own
        // `checkAnim`, so it pops in on select/open and fades on deselect.
        if (chkA > 0.01f) {
            const float checkX = item.x + padX * 1.4f + csize(kMSCheckW) * s * 0.5f;
            const float checkY = item.y + item.h * 0.5f;
            const float w = csize(kMSCheckW) * s;
            const float h = csize(kMSCheckH) * s;
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
            fg->AddPolyline(pts, 3, col, csize(kMSCheckStroke) * s,
                            ImDrawFlags_RoundCornersAll);
        }

        // Label — white on selected rows (reads on ListBg), muted otherwise.
        const ImU32 labelCol = isSel ? TextOnAccent : TextMuted;
        textLeft(fg, item.x + padX * 2.0f + csize(kMSCheckW) * s, item.y + item.h * 0.5f,
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
    const float padX  = csize(kDropdownPadX) * s;
    const DropdownGeom geom = dropdownGeom(r, count, s);
    const Rect  field = geom.field;
    const float fieldCy = field.y + field.h * 0.5f;
    const float itemH = csize(kDropdownItemH) * s;
    const float listTop = geom.listTop;
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
    //
    // The press-down half is what stops the touch-through: a slider painted
    // right below this row would otherwise grab the press the moment it
    // lands (sliders grab on press-down, not release), and on the release
    // frame the toggle would fire here *and* the slider would already have a
    // finger in its row. Pre-claiming on press-down keeps the slider quiet.
    int toggledIdx = -1;
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (!g_gestureConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (st.open) {
            for (int i = 0; i < count; ++i) {
                const Rect item{field.x, listTop + i * itemH, field.w, itemH};
                if (inside(mouse, item)) { consumeGesture(); break; }
            }
            if (!g_gestureConsumed && inside(mouse, r)) consumeGesture();
        } else if (inside(mouse, r)) {
            consumeGesture();
        }
    }
    if (!g_gestureConsumed && released) {
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
            // Keep open — multi-select does not auto-close. Claim the gesture so
            // the control below (whose row the list may overlap) does not fire.
            consumeGesture();
        } else if (inside(mouse, r)) {
            st.open = !st.open;
            consumeGesture();
        } else if (st.open) {
            st.open = false;
            consumeGesture();
        }
    }

    // ── Row ────────────────────────────────────────────────────────────────
    textLeft(dl, r.x, cy, kLabelSize * s, TextPrimary, label);

    const bool rowHeld = ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside(mouse, r);
    dl->AddRectFilled(ImVec2(field.x, field.y),
                      ImVec2(field.x + field.w, field.y + field.h),
                      fadeCol(rowHeld ? ControlBgHi : ControlBg), csize(kDropdownRound) * s);

    textLeft(dl, field.x + padX, fieldCy, kValueSize * s,
             selectedCount > 0 ? TextPrimary : TextMuted, collapsed.c_str());

    // Chevron — same counter-clockwise opening as the dropdown.
    const float chevHalfW = csize(kChevronW) * s * 0.5f;
    const float chevHalfH = csize(kChevronH) * s * 0.5f;
    const float chevStroke = csize(kChevronThick) * s * 0.5f;
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
// g_pressPos / g_pressValid / g_gestureConsumed live at namespace scope (see
// top of file) so the switch code between the two anonymous namespaces can
// read them too.
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
    if (g_gestureConsumed) return false;
    pollPressOrigin();
    const ImGuiIO& io = ImGui::GetIO();
    const bool fired = g_pressValid && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
           hit(io.MousePos, r) && hit(g_pressPos, r);
    if (fired) consumeGesture();
    return fired;
}

/** Drops the in-flight gesture, so a layer transition (dialog open/close)
/// never hands its release to the layer underneath. */
void consumeTap() {
    g_pressValid = false;
    g_gestureConsumed = true;
}

void beginFrame() {
    g_gestureConsumed = false;
    // g_widgetDragging stays true across frames (it represents an in-flight
    // drag, not a one-shot gesture). The widget that owns the drag clears it
    // when the drag releases; for safety the scroll handler also clears it
    // when its own drag ends, so a slider drag that finishes on the same
    // frame as a release leaves no stale latch behind.
}

bool gestureConsumed() {
    return g_gestureConsumed;
}

void consumeGesture() {
    g_gestureConsumed = true;
}

bool widgetDraggingActive() {
    return g_widgetDragging;
}

void setWidgetDragging() {
    g_widgetDragging = true;
}

void clearWidgetDragging() {
    g_widgetDragging = false;
}

/** True the frame a tap fully outside `r` is released (press AND release
/// outside) — the scrim-dismiss gesture for dialogs. Requiring the press to
/// start outside keeps a slider drag that slides out of the dialog from
/// counting as a dismiss. */
bool clickedOutside(const Rect& r) {
    if (g_gestureConsumed) return false;
    pollPressOrigin();
    const ImGuiIO& io = ImGui::GetIO();
    const bool fired = g_pressValid && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
           !hit(io.MousePos, r) && !hit(g_pressPos, r);
    if (fired) consumeGesture();
    return fired;
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
    const float rounding = csize(kButtonRoundDesign) * s;
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

    const float sz = tsize(28.0f) * s;
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
                      csize(kButtonRoundDesign) * s);
    dl->AddRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h),
                fadeCol(IM_COL32(255, 255, 255, kGhostOutlineAlpha)),
                csize(kButtonRoundDesign) * s, 0, 1.0f * s);

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
    const float sz     = tsize(24.0f) * s;
    const float pad    = csize(kFieldPadDesign) * s;
    const float rounding = csize(kFieldRoundDesign) * s;
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

    if (!g_gestureConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const bool inside = hit(io.MousePos, r);
        const bool wasFocused = f.focused;
        f.focused = inside;
        // First-frame caret: drop the caret at the end of the existing text.
        if (inside && !wasFocused) f.caret = f.len;
        focusChanged = wasFocused != inside;
        if (focusChanged) consumeGesture();
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

/** A pill with the model path on the left and a "..." glyph on the right.
 *  The whole pill is the hit target — the glyph is decoration only. The pill
 *  greys out while the file picker is open elsewhere.
 *
 *  NOTE: the click target used to be only the tiny "..." disc. That made the
 *  control feel dead: `clicked()` requires BOTH the press and the release to
 *  land inside the target rect, and the disc is only ~btnSize wide, so any
 *  sub-pixel finger drift on release slipped out of it and the tap was eaten.
 *  Now pressing anywhere on the pill lights it (held) and releasing anywhere
 *  on the pill opens the browser — "lift to open", with a target hundreds of
 *  pixels wide instead of a 40px dot. */
bool pathPill(ImDrawList* dl, const Rect& r, const char* path, int pathLen,
              bool picking, float s) {
    if (r.w <= 1.0f || r.h <= 1.0f) return false;
    ImFont* font = ImGui::GetFont();
    const float sz       = tsize(22.0f) * s;
    const float pad      = csize(kFieldPadDesign) * s;
    const float rounding = csize(kFieldRoundDesign) * s;
    const float btnSize  = csize(kPathPillBtnSizeDesign) * s;
    const float btnGap   = csize(kPathPillBtnGapDesign) * s;
    const ImVec2 mn(r.x, r.y);
    const ImVec2 mx(r.x + r.w, r.y + r.h);

    // The entire pill is the hit target.
    const Rect pillRect{mn.x, mn.y, mx.x - mn.x, r.h};
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

    // The "..." glyph — decoration only; the whole pill is clickable.
    const ImVec2 btnMn(mx.x - btnSize - pad * 0.5f, mn.y + (r.h - btnSize) * 0.5f);
    const ImVec2 btnMx(mx.x - pad * 0.5f,        btnMn.y + btnSize);
    const bool pillHeld = ImGui::IsMouseDown(ImGuiMouseButton_Left) && hit(
        ImGui::GetIO().MousePos, pillRect);
    dl->AddRectFilled(btnMn, btnMx,
                      fadeCol(pillHeld ? kPathPillBtnFillHi : kPathPillBtnFill),
                      btnSize * 0.5f);
    // Three dots centred.
    const float dotR = 3.0f * s;
    const float cy = (btnMn.y + btnMx.y) * 0.5f;
    const float cx = (btnMn.x + btnMx.x) * 0.5f;
    const float off = 8.0f * s;
    dl->AddCircleFilled(ImVec2(cx - off, cy), dotR, fadeCol(TextPrimary), 12);
    dl->AddCircleFilled(ImVec2(cx,        cy), dotR, fadeCol(TextPrimary), 12);
    dl->AddCircleFilled(ImVec2(cx + off, cy), dotR, fadeCol(TextPrimary), 12);

    // While the picker is already open, swallow the gesture so a stray release
    // inside the (now covered) pill does not re-trigger it.
    if (picking) return false;
    return clicked(pillRect);
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
    const float sidePad  = csize(kDialogSidePadDesign) * s;
    const float topPad   = csize(kDialogTopPadDesign)  * s;
    const float botPad   = csize(kDialogTopPadDesign)  * s;
    const float maxW     = 1300.0f * s;
    r.w = board.w - 2.0f * sidePad;
    if (r.w > maxW) r.w = maxW;
    r.x = board.x + (board.w - r.w) * 0.5f;
    r.y = board.y + topPad;
    r.h = board.h - topPad - botPad;
    if (r.h < 200.0f * s) r.h = 200.0f * s;
    return r;
}

float dialogHeaderHeight(float s) { return csize(kDialogHeaderHeightDesign) * s; }
float dialogInnerPad(float s)     { return csize(kDialogInnerPadDesign) * s; }

}  // namespace widgets
}  // namespace ui
}  // namespace aimbotng
#endif  // HAS_IMGUI
