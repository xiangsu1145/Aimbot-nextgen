// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::notify — implementation. See notify.h for the why.
// ─────────────────────────────────────────────────────────────────────────────
#include "ui/gui/notify.h"

#include "ui/gui/hud.h"
#include "ui/gui/theme.h"

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace aimbotng {
namespace ui {
namespace notify {

namespace {

#define LOG_TAG "AimbotNg"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Look ────────────────────────────────────────────────────────────────────

/// Plate geometry, in screen pixels at 1x. The HUD draws itself at a scale that
/// follows the panel, and so does this: the toasts are read at the same
/// distance as the board, so they have to grow with it or they read as a
/// system overlay pasted on top.
constexpr float kMinWidthPx   = 260.0f;   // a short message still gets a real card
constexpr float kMaxWidthFrac = 0.42f;    // never more than this much of the screen
constexpr float kPadXPx       = 16.0f;
constexpr float kPadYPx       = 12.0f;
constexpr float kBarHeightPx  = 4.0f;     // the progress sweep, bottom edge
constexpr float kFontSizePx   = 20.0f;
constexpr float kCornerPx      = 6.0f;
constexpr float kGapPx        = 8.0f;     // between stacked entries
constexpr float kEdgeMarginPx = 16.0f;    // from the screen edge

/// Slides in over this long, and out over the same when it expires. Fast: a
/// toast that takes a moment to arrive is a toast that is read late.
constexpr float kSlideSeconds = 0.18f;

/// Start opacity once fully arrived. Not 1.0 — these cover the game, and a
/// solid black card in the corner of the screen is a lot of screen. The plate
/// is translucent black, the text is opaque, and that combination reads
/// clearly against any background while still letting the game show through.
constexpr float kPlateAlpha = 0.78f;

/// How many entries may be on screen at once. Older ones are pushed off the
/// bottom of the stack (and fade) rather than being drawn past the top of the
/// screen. Six is what fits above the corner without reaching mid-screen on a
/// phone held in landscape.
constexpr int kMaxVisible = 6;

/// The accent colours, which are the theme's own so a toast and the control it
/// is reporting on are visibly the same thing.
ImU32 accentFor(Kind k) {
    switch (k) {
        case Kind::Success: return IM_COL32( 72, 199, 116, 255);
        case Kind::Warning: return IM_COL32(240, 180,  60, 255);
        case Kind::Error:   return IM_COL32(235,  80,  80, 255);
        case Kind::Info:
        default:            return theme::Accent;
    }
}

// ── Entry ───────────────────────────────────────────────────────────────────

struct Entry {
    /// Coalescing key. A non-empty tag updates the existing entry in place;
    /// an empty one always appends.
    std::string tag;
    std::string message;   ///< the one and only line
    Kind        kind    = Kind::Info;

    float duration   = 4.0f;   ///< seconds it stays once fully arrived
    float held       = 0.0f;   ///< seconds it has been shown
    /// Wall-clock seconds since this entry was created, and since it began
    /// leaving. NOT an accumulation of ImGuiIO::DeltaTime — see reveal().
    float age        = 0.0f;
    float leavingAt  = -1.0f;  ///< age at which `expiring` was set; <0 = alive
    float progress   = -1.0f;  ///< <0 = no bar; else 0..1

    /// Set when the message is replaced but the entry should still animate out.
    bool  expiring   = false;

    /// 0 = off-screen, 1 = fully arrived, derived from `age` on read.
    ///
    /// Derived from wall-clock age rather than integrated per frame, and that
    /// is the whole point. This is drawn by the HUD, whose frame loop paces
    /// itself by the *situation*: 120 fps while the board settles, 60 while a
    /// detector runs, and **2 fps when the board is hidden and nothing else is
    /// happening**. A per-frame ease (`reveal += (1-reveal) * rate*dt`) with
    /// `rate = 1/0.18` sees `dt ≈ 0.5s` at 2 fps, so `rate*dt` clamps to 1 and
    /// the slide completes in a single frame — no entrance animation at all,
    /// exactly on the path where a toast is most likely to be raised (a finger
    /// held on the inference area with the menu down).
    ///
    /// Reading it off `age` makes the animation frame-rate independent: the
    /// same 0.18s slide happens whether the loop is running at 2 fps or 120.
    /// The cost is that it *only* looks right when enough frames land inside
    /// the slide window — at 2 fps a quarter-second slide is one or two frames
    /// — but a bar that slides correctly whenever the loop is fast, and
    /// degrades to "just appears" when it is slow, is strictly better than one
    /// that never slides anywhere.
    float reveal() const {
        const float t = expiring ? (age - leavingAt) : age;
        const float u = std::clamp(t / kSlideSeconds, 0.0f, 1.0f);
        return expiring ? (1.0f - u) : u;
    }
};

std::vector<Entry> gEntries;

/// Progress values published from worker threads, keyed by tag. The render
/// thread drains this into the matching entry each frame. An atomic per tag
/// rather than a mutex: the writes are single floats from a thread that may not
/// block, and a lost update is a bar that advances one frame later.
struct ProgressSlot {
    char  tag[48] = {};
    std::atomic<float> value{-1.0f};
};
/// Fixed-size, lock-free. More concurrent bars than this do not exist here —
/// the only publisher is the model-compile thread — and a fixed array keeps the
/// worker path free of allocation.
constexpr int kMaxSlots = 8;
ProgressSlot gSlots[kMaxSlots];

ProgressSlot* slotFor(const char* tag, bool createIfMissing) {
    if (tag == nullptr || tag[0] == '\0') return nullptr;
    for (ProgressSlot& s : gSlots) {
        if (std::strncmp(s.tag, tag, sizeof(s.tag)) == 0) {
            if (s.tag[0] == '\0') break;   // empty trailer: no match beyond here
            return &s;
        }
    }
    if (!createIfMissing) return nullptr;
    for (ProgressSlot& s : gSlots) {
        if (s.tag[0] == '\0') {
            std::snprintf(s.tag, sizeof(s.tag), "%s", tag);
            return &s;
        }
    }
    return nullptr;   // full — the bar simply does not update
}

Entry* findByTag(const char* tag) {
    if (tag == nullptr || tag[0] == '\0') return nullptr;
    for (Entry& e : gEntries) {
        if (e.tag == tag) return &e;
    }
    return nullptr;
}

/// Longest prefix of `text` that fits `maxWidth` at `size`, with an ellipsis.
/// The messages carry model names and reasons, which are not bounded — a card
/// that grows past the screen edge is worse than a truncated word.
std::string ellipsize(ImFont* font, float size, const std::string& text, float maxWidth) {
    if (font == nullptr) return text;
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= maxWidth) return text;

    const char* ell = "...";
    const float ellW = font->CalcTextSizeA(size, FLT_MAX, 0.0f, ell).x;
    std::string out;
    // Byte-wise would split a UTF-8 sequence and draw a box; the messages are
    // Chinese as often as not, so step whole code points.
    size_t i = 0;
    while (i < text.size()) {
        size_t len = 1;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if      ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > text.size()) break;

        std::string candidate = out + text.substr(i, len);
        if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, candidate.c_str()).x + ellW > maxWidth) {
            break;
        }
        out = std::move(candidate);
        i += len;
    }
    return out + ell;
}

/// The HUD's scale factor for this frame, clamped to something readable.
///
/// `hudScale()` is recomputed only while the board is being laid out, and the
/// hidden early-return in drawHud() deliberately leaves it at its last value —
/// which is exactly what the toasts want, since they have to stay legible when
/// the board is out of the way (a person aiming at a game). The clamp is for the
/// degenerate end: a panel small enough to drive the ratio to nearly zero would
/// otherwise draw the cards at a couple of pixels tall.
float uiScale() {
    const float s = hudScale();
    if (!(s > 0.0f)) return 1.0f;   // never laid out yet
    return std::clamp(s, 0.6f, 2.0f);
}

}  // namespace

// ── Public: publishing ──────────────────────────────────────────────────────

void post(const char* tag, Kind kind, const char* message,
          float durationSec, float progress) {
    if (message == nullptr) return;

    // Negative means "no lifetime" — the entry stays until something replaces
    // it. That is what a progress bar wants: a compile that takes forty seconds
    // must not have its notice expire at four and leave the user staring at
    // nothing while it is still working. (Zero and below are distinct: zero
    // would be an entry that expires on the frame it appears.)
    const float lifetime = (durationSec < 0.0f) ? -1.0f
                         : (durationSec > 0.0f ? durationSec : 4.0f);

    const bool coalesce = (tag != nullptr && tag[0] != '\0');
    Entry* existing = coalesce ? findByTag(tag) : nullptr;

    if (existing != nullptr) {
        // Update in place. The message may have changed (the compile step
        // advanced), the lifetime always resets, and `age` is left alone so a
        // bar that is already on screen does not slide in again every frame
        // the worker re-posts it.
        existing->message = message;
        existing->kind    = kind;
        existing->held    = 0.0f;
        existing->expiring = false;
        existing->leavingAt = -1.0f;
        existing->duration = lifetime;
        // A plain re-post of a bar-carrying entry must not erase the bar.
        if (progress >= 0.0f) existing->progress = progress;
        return;
    }

    Entry e;
    if (coalesce) e.tag = tag;
    e.message  = message;
    e.kind     = kind;
    e.duration = lifetime;
    e.progress = progress;
    gEntries.push_back(std::move(e));
}

void info(const char* tag, const char* message, float durationSec) {
    post(tag, Kind::Info, message, durationSec, -1.0f);
}
void success(const char* tag, const char* message, float durationSec) {
    post(tag, Kind::Success, message, durationSec, -1.0f);
}
void warn(const char* tag, const char* message, float durationSec) {
    post(tag, Kind::Warning, message, durationSec, -1.0f);
}
void error(const char* tag, const char* message, float durationSec) {
    post(tag, Kind::Error, message, durationSec, -1.0f);
}

void publishProgress(const char* tag, float progress01) {
    ProgressSlot* s = slotFor(tag, true);
    if (s == nullptr) return;
    // A negative means "remove the bar", not "clamp to zero" — the failed-
    // compile path uses it to take the sweep off an entry it is about to
    // replace with an error line, and a bar frozen at 0% reads as "it is still
    // starting" rather than "it is over".
    s->value.store(progress01 < 0.0f ? -1.0f : std::clamp(progress01, 0.0f, 1.0f),
                   std::memory_order_relaxed);
}

bool alive(const char* tag) {
    const Entry* e = findByTag(tag);
    return e != nullptr && !e->expiring;
}

void clear() {
    gEntries.clear();
    for (ProgressSlot& s : gSlots) {
        s.tag[0] = '\0';
        s.value.store(-1.0f, std::memory_order_relaxed);
    }
}

// ── Public: drawing ─────────────────────────────────────────────────────────

void draw() {
    // Nothing queued and nothing mid-exit: the common case, and it has to cost
    // nothing — this runs every frame whether or not anything is being shown.
    if (gEntries.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (dl == nullptr) return;
    ImFont* font = ImGui::GetFont();
    if (font == nullptr) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    if (screenW <= 1.0f || screenH <= 1.0f) return;

    float dt = io.DeltaTime;
    if (!(dt > 0.0f)) dt = 1.0f / 60.0f;

    const float s = uiScale();
    const float padX      = kPadXPx * s;
    const float padY      = kPadYPx * s;
    const float barH      = kBarHeightPx * s;
    const float corner    = kCornerPx * s;
    const float gap       = kGapPx * s;
    const float edge      = kEdgeMarginPx * s;
    const float fontSize  = kFontSizePx * s;
    const float maxWidth  = screenW * kMaxWidthFrac;
    const float minWidth  = kMinWidthPx * s;

    // ── Advance, drain published progress, expire ──────────────────────────
    // `age` is wall-clock and monotonic per entry; the reveal is derived from
    // it in Entry::reveal() rather than integrated here, which is what keeps
    // the slide correct at 2 fps as well as at 120 (see the comment there).
    for (Entry& e : gEntries) {
        e.age += dt;
        if (!e.expiring) {
            // A negative duration is "hold until replaced". Counting still
            // happens so a diagnostic could read it; nothing expires.
            e.held += dt;
            if (e.duration >= 0.0f && e.held >= e.duration) {
                e.expiring  = true;
                e.leavingAt = e.age;
            }
        }
    }
    // An entry is only gone once it has slid fully out.
    gEntries.erase(
        std::remove_if(gEntries.begin(), gEntries.end(),
                       [](const Entry& e) { return e.expiring && e.reveal() <= 0.001f; }),
        gEntries.end());
    if (gEntries.empty()) return;

    // Pull in whatever a worker thread has published since last frame. Matched
    // by tag; a slot with no live entry is ignored (the compile finished while
    // the entry had already expired) and left for clear() to reclaim.
    for (ProgressSlot& slot : gSlots) {
        if (slot.tag[0] == '\0') continue;
        const float v = slot.value.load(std::memory_order_relaxed);
        if (v < 0.0f) continue;
        if (Entry* e = findByTag(slot.tag)) {
            e->progress = v;
        }
    }

    // ── Measure, then lay out bottom-up ────────────────────────────────────
    // Measuring first is what lets the plates line up as a right-aligned stack
    // whose widest member sets the width — rather than each being as wide as
    // its own text, which reads as a jagged pile.
    struct Laid {
        const Entry* e;
        float x, y, w, h, alpha, progressX;
    };
    std::vector<Laid> laid;
    laid.reserve(gEntries.size());

    float width = minWidth;
    for (const Entry& e : gEntries) {
        const std::string title = ellipsize(font, fontSize, e.message, maxWidth - 2 * padX);
        const float tw = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, title.c_str()).x;
        const float needed = tw + 2 * padX;
        width = std::max(width, std::min(needed, maxWidth));
    }

    float cursorY = screenH - edge;
    int shown = 0;
    for (auto it = gEntries.rbegin(); it != gEntries.rend(); ++it) {
        const Entry& e = *it;
        if (shown >= kMaxVisible) break;
        ++shown;

        const std::string title = ellipsize(font, fontSize, e.message, maxWidth - 2 * padX);
        const float titleH = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, title.c_str()).y;
        const float h = padY * 2 + titleH + (e.progress >= 0.0f ? barH : 0.0f);

        const float reveal = e.reveal();
        cursorY -= h;
        // Slide: unrevealed entries sit off the right edge by their own width,
        // so they leave and re-enter travelling the shortest possible distance.
        const float slideIn = (1.0f - reveal) * (width + edge);
        Laid l;
        l.e   = &e;
        l.w   = width;
        l.h   = h;
        l.x   = screenW - edge - width + slideIn;
        l.y   = cursorY;
        l.alpha = reveal;
        laid.push_back(l);

        cursorY -= gap;
    }

    // Painted oldest-first, which means the NEWEST plate is drawn last and
    // therefore ends up on top. That ordering matters because a fifth/sixth
    // entry is drawn past the bottom of its neighbours and a newer card's
    // plate has to cover it. `laid` was built newest-first (the stack grows
    // upward from the corner), so walking it backwards yields oldest-first.
    for (auto it = laid.rbegin(); it != laid.rend(); ++it) {
        const Laid& l = *it;
        const Entry& e = *l.e;
        if (l.alpha <= 0.01f) continue;

        const ImVec2 p0(l.x, l.y);
        const ImVec2 p1(l.x + l.w, l.y + l.h);
        const ImU32 accent = accentFor(e.kind);

        // The plate. Translucent black, not the theme's PaneRight: a toast is
        // read against arbitrary game content, so it carries its own contrast
        // rather than assuming the menu's.
        const int plateA = static_cast<int>(kPlateAlpha * l.alpha * 255.0f);
        dl->AddRectFilled(p0, p1, IM_COL32(10, 11, 14, plateA), corner);

        // The accent: a left edge bar, always, so the kind is visible even with
        // no progress at all; plus the sweep across the bottom when there is a
        // progress value to show.
        const float edgeW = 4.0f * s;
        dl->AddRectFilled(p0, ImVec2(p0.x + edgeW, p1.y), accent, corner,
                          ImDrawFlags_RoundCornersLeft);

        if (e.progress >= 0.0f) {
            const float sweepW = l.w * std::clamp(e.progress, 0.0f, 1.0f);
            if (sweepW > 1.0f) {
                // A dim wash above the bar, then the bar itself. A single
                // opaque line on a dark plate reads as a divider; the pair
                // reads as a level. The wash is the accent at low alpha —
                // recomposed rather than reused so only the alpha changes.
                const ImU32 wash = (accent & 0x00FFFFFFu) | (40u << 24);
                dl->AddRectFilled(ImVec2(p1.x - sweepW, p1.y - barH * 2.0f), p1,
                                  wash, 0.0f);
                dl->AddRectFilled(ImVec2(p1.x - sweepW, p1.y - barH), p1,
                                  accent, 0.0f);
            }
        }

        // The text. `title` is a named local rather than a temporary passed
        // straight into AddText: ellipsize() returns a std::string by value, and
        // relying on it outliving the call it is passed to is the kind of thing
        // that is true today and a dangling pointer after one refactor.
        const std::string title =
            ellipsize(font, fontSize, e.message, l.w - 2 * padX);
        const int textA = static_cast<int>(l.alpha * 255.0f);
        dl->AddText(font, fontSize, ImVec2(l.x + padX, l.y + padY),
                    (theme::TextPrimary & 0x00FFFFFFu) |
                        (static_cast<ImU32>(textA) << 24),
                    title.c_str());
    }
}

}  // namespace notify
}  // namespace ui
}  // namespace aimbotng
