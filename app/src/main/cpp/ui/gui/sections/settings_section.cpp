// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::settings — implementation. See settings_section.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "settings_section.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "capture/capture.h"
#include "input/exit_request.h"
#include "input/touch_reader.h"
#include "inference/model_runtime.h"
#include "inference/model_store.h"
#include "ui/gui/aimbot_ui.h"
#include "ui/gui/hud.h"
#include "ui/gui/notify.h"
#include "ui/gui/theme.h"
#include "tracking/kalman_tracker.h"

namespace aimbotng {
namespace ui {
namespace sections {

PageSettings g_pageSettings;

namespace {

// ── Emergency exit ──────────────────────────────────────────────────────────
//
// This row exists for exactly one situation: the daemon holds the panel with
// EVIOCGRAB, which makes it the device's only source of touch — and on some
// device the other half of that bargain (uinput → InputReader → whatever the
// OEM put above it) stops delivering. The phone then draws this menu, because
// the menu reads the grabbed events directly, and answers nothing else. There
// is no other reachable control: the app under it cannot be touched, so its
// own 停止 button is unusable at exactly the moment it is wanted.
//
// Killing the daemon is the one repair that cannot be blocked by whatever is
// broken further down — a closed fd releases the grab even if every layer
// past it is dead. Hence: a button here, reached by the one input path that
// still works.
//
// It is two-stepped on purpose. A row that tears the daemon down on one stray
// finger mid-match would be worse than the fault it escapes, so the first tap
// only arms it and says so; the arm expires by itself so a tap that was
// reconsidered cannot fire minutes later. Counted in frames because the menu
// has no business reading a clock for this, and ~180 is three seconds at 60.
constexpr int kExitConfirmFrames = 180;

bool g_exitArmed = false;
int  g_exitArmFrames = 0;

// ── Inference-area overlay constants (same pattern as trigger hold circle) ───────
constexpr float kInfTrigHandleHitR = 36.0f;
constexpr float kInfTrigHandleDraw = 9.0f;
constexpr float kInfTrigThickness  = 2.5f;
constexpr float kInfTrigMinR       = 60.0f;
constexpr int   kInfTrigSegments   = 64;
constexpr float kInfOverlayLabelSize = 38.0f;

constexpr ImU32 kInfOverlayBorder = IM_COL32(255, 180,  80, 215);
constexpr ImU32 kInfOverlayHandle = IM_COL32(255, 255, 255, 235);
constexpr ImU32 kInfOverlayFill   = IM_COL32(255, 140,  20,  28);
constexpr ImU32 kInfOverlayLabel  = IM_COL32(255, 220, 180, 235);

inline float dist2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

// ── Inference-area helpers ──────────────────────────────────────────────────

void ensureInferenceAreaPlaced(float screenW, float screenH) {
    PageSettings::InferenceAreaOverlay& a = g_pageSettings.inferenceArea;
    if (!a.placed) {
        a.cx = screenW * 0.5f;
        a.cy = screenH * 0.5f;
        a.placed = true;
    }
}

bool realFingerInInferenceArea() {
    PageSettings::InferenceAreaOverlay& a = g_pageSettings.inferenceArea;
    if (!reader_is_ready() || !a.placed) return false;
    ReaderPointer pts[READER_MAX_POINTERS];
    const int n = reader_read_pointers(pts, READER_MAX_POINTERS);
    const float r2 = a.r * a.r;
    for (int i = 0; i < n; ++i) {
        const float dx = pts[i].x - a.cx;
        const float dy = pts[i].y - a.cy;
        if (dx * dx + dy * dy <= r2) return true;
    }
    return false;
}

// The hold intent, kept in one place and re-derived once per frame.
//
// Two readers want the same fact on the same frame: the edge handler below,
// which decides arm()/disarm(), and the render loop (aimbot_ui.cpp), which
// decides how fast to run while the finger is down. Deriving it twice would
// sample the reader twice — and the reader is updated by its own thread, so two
// samples on one frame can legitimately disagree, which would show up as the
// pace flickering between the inference tier and the dormant tier while the
// finger sits still.
//
// This block sits in the file's anonymous namespace and the function below is
// defined at `sections` scope deliberately: the declaration in the header has to
// resolve to exactly one definition, and a definition inside the anonymous
// namespace would be a *different* function that merely shadows it — the linker
// would then fail on the header's (undefined) one.
namespace {
bool g_holdHeld  = false;
bool g_holdValid = false;
}  // namespace

}  // namespace

bool holdToInferHeld() {
    // Only meaningful in triggered mode: with continuous inference on, the
    // Model page owns the runtime and the circle is not on screen at all.
    if (g_pageSettings.continuousInference.value) return false;
    if (!g_holdValid) {
        g_holdHeld  = realFingerInInferenceArea();
        g_holdValid = true;
    }
    return g_holdHeld;
}

namespace {

/// Re-samples the hold state. Called once per frame from the top of
/// `syncSettingsPage()`, which is the frame's single authority on the hold
/// state and runs before both readers (see drawHud).
void refreshHoldIntent() {
    g_holdHeld  = realFingerInInferenceArea();
    g_holdValid = true;
}

PageSettings::InferenceAreaOverlay::Drag hitTestInferenceCircle(
        const PageSettings::InferenceAreaOverlay& a, ImVec2 mouse) {
    using Drag = PageSettings::InferenceAreaOverlay::Drag;
    const float hr = kInfTrigHandleHitR;
    if (dist2(mouse.x, mouse.y, a.cx,        a.cy - a.r) <= hr * hr) return Drag::ResizeN;
    if (dist2(mouse.x, mouse.y, a.cx + a.r, a.cy       ) <= hr * hr) return Drag::ResizeE;
    if (dist2(mouse.x, mouse.y, a.cx,        a.cy + a.r) <= hr * hr) return Drag::ResizeS;
    if (dist2(mouse.x, mouse.y, a.cx - a.r, a.cy       ) <= hr * hr) return Drag::ResizeW;
    if (dist2(mouse.x, mouse.y, a.cx, a.cy) <= a.r * a.r) return Drag::Move;
    return Drag::None;
}

void driveInferenceCircle(PageSettings::InferenceAreaOverlay& a,
                         ImVec2 mouse, float screenW, float screenH) {
    using Drag = PageSettings::InferenceAreaOverlay::Drag;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (a.drag == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Drag hit = hitTestInferenceCircle(a, mouse);
            if (hit != Drag::None) {
                a.drag    = hit;
                a.anchorX = mouse.x;
                a.anchorY = mouse.y;
                a.startCX = a.cx;
                a.startCY = a.cy;
                a.startR  = a.r;
            }
        }
        return;
    }
    if (!down) { a.drag = Drag::None; return; }

    switch (a.drag) {
        case Drag::Move: {
            a.cx = a.startCX + (mouse.x - a.anchorX);
            a.cy = a.startCY + (mouse.y - a.anchorY);
            break;
        }
        case Drag::ResizeN:
        case Drag::ResizeE:
        case Drag::ResizeS:
        case Drag::ResizeW: {
            const float ndx = mouse.x - a.cx;
            const float ndy = mouse.y - a.cy;
            float nr = sqrtf(ndx * ndx + ndy * ndy);
            if (nr < kInfTrigMinR) nr = kInfTrigMinR;
            a.r = nr;
            break;
        }
        default: break;
    }

    if (a.cx - a.r < 0.0f)      a.cx = a.r;
    if (a.cy - a.r < 0.0f)      a.cy = a.r;
    if (a.cx + a.r > screenW)  a.cx = screenW - a.r;
    if (a.cy + a.r > screenH)  a.cy = screenH - a.r;
}

}  // namespace

void drawSettingsSection(ImDrawList* dl, float x, float& y, float w,
                         float /*bottomY*/, float s, float es, const Xf& xf,
                         Scroll& sc) {
    const float gap   = 12.0f * s;
    const float rowSw = widgets::kSwitchRowH   * s;
    const float rowSl = widgets::kSliderRowH   * s;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        const ImVec2 p = xf.pt(wx, wy - sc.offset);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.fpsOverlay, "FPS 显示", es);
    y += rowSw + gap;
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.antiScreenshot, "防截屏", es);
    y += rowSw + gap;
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.showDetections, "显示检测框", es);
    y += rowSw + gap;

    // ── Continuous inference ──────────────────────────────────────────────
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.continuousInference,
                          "持续推理", es);
    y += rowSw + gap;

    // ── Inference-area circle (visible only when continuous inference is OFF) ────
    if (!g_pageSettings.continuousInference.value) {
        widgets::switchButton(dl, wRect(x, y, w, rowSw),
                              g_pageSettings.inferenceArea.toggle,
                              "推理区域", es);
        y += rowSw + gap;
    }

    // ── Touch passthrough ──────────────────────────────────────────────────
    // The escape hatch for the one behaviour this build still keeps from 1.2.1
    // only by default: gestures that begin on the menu are swallowed so the game
    // underneath does not receive them too (our menu is a layer, not a window —
    // see the note on kSwallowMenuGesturesDefault in touch_reader.cpp). With it
    // on, every gesture is mirrored and the reader is 1.2.1 exactly; the cost is
    // that tapping the menu also taps whatever is beneath it.
    //
    // Applied by syncSettingsPage() on the transition, not by this call, for the
    // same reason the Anti Screenshot switch is: the value has to survive the
    // board being hidden or reopened.
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.touchPassthrough, "允许触摸穿透", es);
    y += rowSw + gap;

    // HIDDEN 2026-09-13: predictive / tracker UI rows are no longer drawn while
    // the predictive F-term path is disabled (the user reverted to the old
    // project's smoothVel-only F term). The widget states (showTracking,
    // trackIou, trackConfirm, trackTerminate) and the syncSettingsPage writes
    // into trackerConfig() are kept — they cost almost nothing and make
    // re-enabling the predictive path a one-line change. To re-enable:
    // uncomment the four blocks below.
    //
    // widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.showTracking, "显示追踪框", es);
    // y += rowSw + gap;
    // widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageSettings.trackIou,        "追踪IoU",   2, es); y += rowSl + gap;
    // widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageSettings.trackConfirm,    "确认帧",    0, es); y += rowSl + gap;
    // widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageSettings.trackTerminate,  "丢失帧",    0, es); y += rowSl + gap;

    // ── Exit / restore touch ───────────────────────────────────────────────
    // Last row on the page: the control you reach for when nothing else
    // responds. See the note above [g_exitArmed] for why it is here and why
    // it takes two taps.
    if (widgets::button(dl, wRect(x, y, w, rowSw),
                        g_exitArmed ? "再点一次: 退出并恢复触摸" : "退出并恢复触摸",
                        g_exitArmed ? widgets::ButtonVariant::Danger
                                    : widgets::ButtonVariant::Ghost,
                        es)) {
        if (g_exitArmed) {
            aimbotng::input::requestDaemonExit();
            g_exitArmed = false;
        } else {
            g_exitArmed = true;
            g_exitArmFrames = 0;
        }
    }
    if (g_exitArmed && ++g_exitArmFrames > kExitConfirmFrames) g_exitArmed = false;
    y += rowSw + gap;
}

// ── drawInferenceAreaOverlay ────────────────────────────────────────────────

void drawInferenceAreaOverlay() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    if (screenW <= 1.0f || screenH <= 1.0f) return;

    // Area always placed (centred) regardless of toggle — the aim/trigger loops
    // may read it even when the overlay is hidden.
    ensureInferenceAreaPlaced(screenW, screenH);

    if (!g_pageSettings.inferenceArea.toggle.value) return;

    PageSettings::InferenceAreaOverlay& a = g_pageSettings.inferenceArea;
    driveInferenceCircle(a, mouse, screenW, screenH);

    ImFont* font = ImGui::GetFont();
    const ImVec2 c(a.cx, a.cy);

    dl->AddCircleFilled(c, a.r, kInfOverlayFill, kInfTrigSegments);
    dl->AddCircle      (c, a.r, kInfOverlayBorder, kInfTrigSegments, kInfTrigThickness);
    dl->AddCircleFilled(ImVec2(a.cx,        a.cy - a.r), kInfTrigHandleDraw, kInfOverlayHandle);
    dl->AddCircleFilled(ImVec2(a.cx + a.r, a.cy       ), kInfTrigHandleDraw, kInfOverlayHandle);
    dl->AddCircleFilled(ImVec2(a.cx,        a.cy + a.r), kInfTrigHandleDraw, kInfOverlayHandle);
    dl->AddCircleFilled(ImVec2(a.cx - a.r, a.cy       ), kInfTrigHandleDraw, kInfOverlayHandle);

    const ImVec2 ts = font->CalcTextSizeA(kInfOverlayLabelSize, FLT_MAX, 0.0f, "推理区域");
    dl->AddText(font, kInfOverlayLabelSize,
                 ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                 kInfOverlayLabel, "推理区域");
}

void syncSettingsPage() {
    // Push the tracker tuning sliders into the global tracker config. Cheap;
    // runs every frame but only writes a few scalars.
    auto& cfg = tracking::trackerConfig();
    cfg.iouThreshold    = g_pageSettings.trackIou.value;
    cfg.confirmFrames   = static_cast<int>(g_pageSettings.trackConfirm.value + 0.5f);
    cfg.terminateFrames = static_cast<int>(g_pageSettings.trackTerminate.value + 0.5f);

    // Anything this page owns that reaches out of the menu is applied here, on
    // a transition, rather than being pushed by the draw call that flipped it
    // (see drawHud() for why it lives here). Today that is the Anti Screenshot
    // switch and the touch-passthrough switch — the 退出 row below acts on
    // itself, inside the widget's own if-frame, because there is nothing to keep
    // in sync afterwards.
    static bool lastAnti = true;
    const bool nowAnti = g_pageSettings.antiScreenshot.value;
    if (nowAnti != lastAnti) {
        lastAnti = nowAnti;
        aimbotng::ui::setSkipScreenshot(nowAnti);
    }

    // The initial value of lastPass must be the reader's own default (menu owns
    // its gestures), so that a config saying "off" pushes nothing and a config
    // saying "on" is applied on the first frame the menu draws.
    static bool lastPass = false;
    const bool nowPass = g_pageSettings.touchPassthrough.value;
    if (nowPass != lastPass) {
        lastPass = nowPass;
        ::reader_set_pass_through(nowPass);
    }

    // ── Continuous inference control ──────────────────────────────────────────
    // Continuous ON: inference always runs, and the USER opens it with the
    // Model page's master switch — deliberately manual, so a session does not
    // start detecting just because the menu was opened.
    //
    // Continuous OFF: the Model page's switch is forced off and greyed (see
    // hud.cpp's header-switch block), and this page's inference-area circle
    // takes over completely. A finger inside the circle runs the detector; a
    // finger outside it does not. That is the whole control surface in
    // triggered mode: no switch to visit, no page to open.
    //
    // Edge-triggered rather than level-triggered. A per-frame call to arm()
    // would still be cheap (arm() is a flag flip), but a per-frame call to
    // *prepare* would not: the runtime rebuilds the engine whenever it cannot
    // prove it already holds the right one, so a level-triggered handler would
    // restart a multi-second graph compile sixty times a second and never let
    // one finish. The symptom of getting this wrong is "hold the area, nothing
    // ever runs", which is why it is called out here rather than left implicit.
    static bool lastContinuous = true;
    static bool lastWants      = false;

    // Re-derive the hold state once for this frame, before anything reads it.
    // Both the edge logic below and holdToInferHeld() — which the render loop
    // calls later in the same frame for pacing — come off this one sample.
    refreshHoldIntent();

    const bool nowContinuous = g_pageSettings.continuousInference.value;
    if (nowContinuous != lastContinuous) {
        lastContinuous = nowContinuous;
        // Either direction starts from "nothing is triggered". Dropping to
        // triggered mode also disarms, because the Model page may have left
        // inference running and there is no switch on this page to turn it off
        // — the finger is now the only authority, and it is not down.
        lastWants = false;
        if (!nowContinuous) {
            infer::runtime::disarm();
        }
    }

    if (nowContinuous) {
        // Continuous mode: the Model page owns the runtime. Nothing to do here.
        return;
    }

    // ── Triggered mode ───────────────────────────────────────────────────────
    // Two questions, and they are different questions:
    //
    //   * is there a model to run       — a persistent choice, in the store
    //   * is its engine built yet       — volatile, and the one that matters
    //
    // The old code asked only the first (`model::loadedId() > 0`) and then
    // called arm(). When the engine had never been prepared — the user came
    // straight to this page, or changed the model, or the engine was stopped —
    // arm() recorded `pendingArm` and returned, and NOTHING on this path ever
    // triggered the compile that would have honoured it. The finger did
    // nothing, indefinitely.
    const bool haveModel = model::loadedId() > 0;
    const bool wants     = holdToInferHeld() && haveModel;

    if (wants != lastWants) {
        lastWants = wants;
        if (wants) {
            // The compile, if one is owed. Idempotent and returns immediately,
            // so calling it on the edge costs a compare-exchange on the common
            // (already-prepared) path. The toast it raises is what tells the
            // user why the first second of holding produces no boxes.
            if (!infer::runtime::engineReady()) {
                infer::runtime::prepareAsync();
            }
            // arm() is still called even when the engine is not ready: it
            // records the intent, and prepare() applies it on completion. That
            // is what makes the first press work instead of requiring a second.
            infer::runtime::arm();

            // The other half of "holding does nothing": frames. Inference with
            // no frame source runs on a stale frame and reports the same boxes
            // forever, which reads as a detector that is stuck. Say so, once,
            // rather than letting the user work it out.
            //
            // The question has to be `enabled()` — the menu's capture switch —
            // and NOT `consuming()`. consuming() is `wantsPreview || wantsInfer`
            // (see aimbot_ui.cpp, where the renderer publishes it), so the
            // moment this very branch calls arm() the "wantsInfer" half becomes
            // true and consuming() reads true regardless of the switch. The
            // warning would never fire on the only path that can reach it.
            if (!capture::enabled()) {
                notify::warn("infer.noframes",
                             "请先开启截图否则无法推理", 5.0f);
            }
        } else {
            infer::runtime::disarm();
        }
    }
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
