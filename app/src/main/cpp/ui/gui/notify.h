// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::notify — bottom-right corner toasts.
//
//  What this is for: the daemon does several things that take a noticeable and
//  unpredictable amount of time — compiling a graph onto the APU or the HTP,
//  loading a model, waiting for the first frame out of the screen-capture
//  pipeline — and until now the only place any of it surfaced was logcat. A
//  person holding a finger on the inference area while the graph compiles has
//  no way to tell "still compiling" from "this feature is broken".
//
//  Why not a modal card (the shape the Model page already uses): a modal is the
//  right answer for a *deliberate* action the user is waiting on — they tapped
//  Load and are looking at the screen. It is the wrong answer for the held-
//  finger path, where the user is aiming: a card over the board covers the
//  thing they are looking at, and `anyDialogOpen()` going true silences the
//  header switches for the duration.
//
//  So these are small, non-modal, and drawn from the same draw list as the rest
//  of the HUD. They take no touch (nothing hit-tests them), they survive the
//  board being hidden (which is exactly when someone is aiming), and they
//  vanish on their own.
//
//  Layout and look are adapted from the "Solstice" client's Notifications
//  module: a stack that grows upward from the bottom-right corner, each entry a
//  translucent black plate with a coloured fill sweeping across it as progress,
//  sliding in from off-screen and back out when it expires.
//
//  The one difference that matters: that implementation draws to
//  GetBackgroundDrawList() because it has a real window stack above it. Here
//  the menu layer *is* the top of the stack, so anything drawn to the
//  background list would be painted over by the board. Foreground list, always.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "imgui.h"

namespace aimbotng {
namespace ui {
namespace notify {

/// The tag used for the model-compile toast, shared rather than written twice
/// because two paths raise it — the Model page's switch (and its modal card)
/// and the held-finger handler's async prepare — and they must coalesce into
/// ONE entry. Two literals that drift apart would show two bars for one compile.
inline constexpr const char* kCompileTag = "model.compile";

/// Colour of an entry's accent. Not decoration: it is the only channel that
/// carries "this is a problem" versus "this is progress" without reading the
/// text, which matters when the text is being glanced at mid-match.
enum class Kind {
    Info,     ///< neutral progress — loading, compiling, "started"
    Success,  ///< finished, and the outcome was the one wanted
    Warning,  ///< finished, but degraded (no frames, fallback engine chosen)
    Error,    ///< finished, and it failed
};

/// Shows an entry that expires after `durationSec` of wall time.
///
/// Progress-bar variant: pass a non-negative `progress` and the entry keeps
/// updating as long as you re-publish under the same `tag`; pass a negative one
/// and it is a plain message with no bar.
///
/// `tag` is how a long-running entry is updated rather than duplicated. A
/// compiling model re-published every frame under the same tag will reuse its
/// entry — so the bar advances and the lifetime resets — instead of stacking
/// hundreds of identical cards. An empty tag means "never coalesce", which is
/// what a one-shot message wants.
///
/// Safe to call from the render thread only, like every other function here;
/// the values a worker thread wants to publish are staged through
/// [publishProgress] below.
void post(const char* tag, Kind kind, const char* message,
          float durationSec = 4.0f, float progress = -1.0f);

/// Convenience wrappers, because the kind is usually implied by the call site
/// and the four-argument form reads badly at the bottom of a deep branch.
void info(const char* tag, const char* message, float durationSec = 4.0f);
void success(const char* tag, const char* message, float durationSec = 4.0f);
void warn(const char* tag, const char* message, float durationSec = 5.0f);
void error(const char* tag, const char* message, float durationSec = 6.0f);

/// Updates (or creates) the entry that carries `tag` with a new progress value
/// in `0.0 .. 1.0`. Deliberately separable from [post] so a *worker* thread can
/// advance a bar without touching ImGui or the font atlas — it only writes an
/// atomic, and the render thread picks it up next frame.
///
/// This is the whole reason the progress bar is not a lambda: the compile runs
/// on a background thread, which must not call into ImGui, and it must not have
/// to own a string to report how far along it is.
void publishProgress(const char* tag, float progress01);

/// True while the entry carrying `tag` is on screen. The compile path uses this
/// to decide whether its "compiling" line should be re-posted (it should not,
/// once the entry has been dismissed by the user or has been replaced by the
/// success line).
bool alive(const char* tag);

/// Advances the animations, drops what has expired, and draws the stack.
/// Called once per frame from drawHud(), after the board, so the toasts sit on
/// top of everything the menu paints. Does nothing at all when the queue is
/// empty, which is the common case.
void draw();

/// Drops everything immediately. Used by stop() so a torn-down renderer does
/// not leave entries that would reappear on the next start with lifetimes from
/// a previous session.
void clear();

}  // namespace notify
}  // namespace ui
}  // namespace aimbotng
