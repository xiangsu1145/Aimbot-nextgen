// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::model — the Model page (模型).
//
//  Heavier than the other sections: it owns both dialogs — Add-Model and the
//  per-model Settings card — AND the custom file browser used to populate the
//  Add-Model one. drawModelSection() paints the list
//  in the right pane; drawModelOverlays() is called separately by drawBoard()
//  so the dialog can sit on top of the right pane without the rail having to
//  know about it.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "ui/gui/widgets.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace aimbotng {
namespace ui {
namespace sections {

/// The regular list view, or one of the dialogs open on top of it.
/// Compiling is a modal-only state — it is drawn by drawModelOverlays but
/// does not replace the underlying list view, which stays paintable behind
/// the scrim so the user has a frame of context while waiting.
/// NoModel is the Chinese hint shown when the inference switch is flipped on
/// while no entry carries the loaded flag — there is nothing to compile, so
/// the Compiling overlay must not appear at all.
enum class ModelDialogState { Closed, Adding, Settings, Compiling, NoModel };

struct PageModel {
    ModelDialogState dialog = ModelDialogState::Closed;
    bool             changed = false;     // set by JNI callbacks, read by add/remove

    /// The page's master switch: run the loaded model, or do not. Drawn on the
    /// header line like the Aim / Trigger / Capture switches, because the Model
    /// page is the same kind of thing to the user — a feature that is on or off.
    ///
    /// What it *does* is not symmetric with the others: flipping it on starts a
    /// Load of whichever entry has the loaded flag, so it can fail, and the
    /// reason is painted next to it rather than thrown away.
    widgets::SwitchState enabled;

    // ── Add-Model dialog ──────────────────────────────────────────────────
    // Holds a path, an engine choice and a confidence threshold: the display
    // name is taken from the file's basename, and class names were dropped.
    char  pathBuf[256]   = {};
    int   pathLen        = 0;
    bool  pathPicking    = false;
    bool  pathFailed     = false;
    char  pathErr[128]   = {};

    // The dropdown and the slider keep their state across frames — a dropdown
    // built fresh every frame forgets it was open, and a slider built fresh
    // every frame has nowhere to keep the ease it is animating.
    widgets::DropdownState engine;                        // -1 == no model picked yet
    widgets::SliderState   conf{0.5f, 0.05f, 0.95f, 0.01f};
    std::string            pathType;   // detected dtype label of the picked file
    // ONNX Runtime intra-op threads for the model being added. 1..8, integer.
    widgets::SliderState   threadsSlider{1.0f, 1.0f, 8.0f, 1.0f};
    // HTP (QNN) performance-mode vote: index == raw
    // TfLiteQnnDelegateHtpPerformanceMode value. Shown only for the QNN HTP
    // engine; default Sustained High Performance (index 1).
    widgets::DropdownState htpPerf{1};

    // ── Per-model Settings dialog ─────────────────────────────────────────
    // Both fields are copied out of the entry when the dialog opens and
    // written back on Save, so Cancel really is free: nothing touches the
    // store until the user confirms.
    int                    settingsId = 0;
    widgets::DropdownState settingsEngine;
    widgets::SliderState   settingsConf{0.5f, 0.05f, 0.95f, 0.01f};
    // ONNX Runtime intra-op threads for the edited model. 1..8, integer.
    widgets::SliderState   settingsThreadsSlider{1.0f, 1.0f, 8.0f, 1.0f};
    // HTP (QNN) performance-mode vote for the edited model.
    widgets::DropdownState settingsHtpPerf{1};

    // ── Compiling overlay ────────────────────────────────────────────────
    // Pushed up to model.h because the load runs on a background thread and
    // the result has to outlive the call that started it. Three things move
    // here from drawModelSection: the running session id (so the UI knows
    // which entry is loading), the error string (so a failed compile can be
    // shown instead of dismissed), and the worker itself.
    std::atomic<bool>      compileActive{false};   // true while the worker runs
    std::atomic<bool>      compileDone{false};     // true once the worker has exited
    bool                   compileOk = false;      // post-condition, only valid after compileDone
    int                    compileId = 0;          // which entry is loading
    std::string            compileError;           // message when compileOk == false
    std::unique_ptr<std::thread> compileThread;     // joined on each frame the overlay is open
};

extern PageModel g_pageModel;

/// Draw the Model page's list of entries + the "Add Model" button.
void drawModelSection(ImDrawList* dl, float x, float& y, float w,
                      float bottomY, float s, float es, const Xf& xf,
                      Scroll& sc);

/// Publishes the page's switch and the loaded-model choice to the inference
/// runtime. Called once per frame by the board, and cheap: it does work only on
/// a transition, which is what keeps a model load off the per-frame path.
void syncModelPage();

/// The page's switch, driven from outside the menu.
///
/// A finger and this function are the same author: both set the switch, and the
/// switch is what syncModelPage() reads. Going through the switch rather than
/// calling the runtime directly is what keeps one authority over whether
/// inference runs — a second entry point would let the switch read OFF while a
/// model was loaded and running.
///
/// Exists so a script can drive a page whose controls are ImGui primitives drawn
/// onto a SurfaceFlinger layer, where neither `adb shell input` nor uinput can
/// reach them. Same reason UITOUCH exists.
void setModelSwitch(bool on);
bool modelSwitch();

/// Draws whichever dialog is open — Add-Model, the per-model Settings card,
/// the Compiling overlay, the no-model Chinese hint, or the file browser
/// sitting on top of either. No-op when nothing is open.
void drawModelOverlays(ImDrawList* dl, float s, float es, const Xf& xf);

/// True while any modal layer is up (Add / Settings / Compiling dialog, or
/// the file browser). The HUD reads this to keep the rail and the header
/// master switch quiet while a dialog owns the board: taps outside the card
/// belong to the dialog's scrim-dismiss, never to the rows underneath.
bool anyDialogOpen();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
