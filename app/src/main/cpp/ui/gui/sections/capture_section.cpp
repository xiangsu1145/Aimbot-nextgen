// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::capture — implementation. See capture_section.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "capture_section.h"

#include <cmath>
#include <cstdio>

#include "capture/capture.h"
#include "inference/model_runtime.h"
#include "ui/gui/aimbot_ui.h"
#include "ui/gui/hud.h"
#include "ui/gui/notify.h"
#include "ui/gui/theme.h"

namespace aimbotng {
namespace ui {
namespace sections {

PageCapture g_pageCapture;

namespace {
using namespace theme;

constexpr float kPreviewRound = 12.0f;
constexpr float kHintSize     = 24.0f;

/// Tag for the "inference turned capture on for you" toast. Named here rather
/// than left as a literal at the call site so that syncCapturePage() — which
/// runs every frame — cannot accidentally post a second entry under a
/// differently-spelled tag and defeat the coalescing.
constexpr const char* kCaptureAutoTag = "capture.auto";

/**
 * The capture page's live preview.
 *
 * Square, because the capture target is square — the frame arrives at exactly
 * the size the slider asks for, so a square well never letterboxes it. Until a
 * frame lands the well says which of the two reasons applies ("off", or on but
 * nothing has come back), which is the difference between "I forgot to flip the
 * switch" and "something upstream is broken".
 */
void drawCapturePreview(ImDrawList* dl, float x, float y, float w, float h,
                        float s, float es, const Xf& xf) {
    if (h < 8.0f) return;

    // Room is kept back for the rate line drawn underneath the well; without it
    // the square would eat the full height and the number would have nowhere to go.
    const float rateH = kHintSize * s * 2.0f;
    const float side = fminf(w, h - rateH);
    if (side < 8.0f) return;
    const ImVec2 mn(x + (w - side) * 0.5f, y);
    const ImVec2 mx(mn.x + side, y + side);
    const float rounding = kPreviewRound * s;

    dl->AddRectFilled(xf.pt(mn), xf.pt(mx), xf.col(PreviewWell), xf.s(rounding));

    const unsigned long long tex = capturePreviewTexture();
    if (tex != 0) {
        dl->AddImage((ImTextureID)tex, xf.pt(mn), xf.pt(mx));
    } else {
        textCentred(dl, xf.pt(mn), xf.pt(mx), xf.s(kHintSize * s),
                    xf.col(TextMuted),
                    g_pageCapture.enabled.value ? "waiting for frames"
                                                : "capture is off");
    }

    dl->AddRect(xf.pt(mn), xf.pt(mx), xf.col(Edge), xf.s(rounding), 0, xf.s(1.5f * s));

    // The rate the producer is delivering, not the rate this menu is drawing at.
    // They are different numbers and the difference is the point: 120 on the
    // screen with 30 here means frames are being thrown away before they arrive,
    // which is a producer problem and would be invisible from a render counter.
    const int fps = capture::frameRate();
    char rate[32];
    if (fps > 0) {
        snprintf(rate, sizeof(rate), "%d fps", fps);
    } else {
        // Read off the switch, not off the module: `enabled` now also carries
        // inference's demand, and "capture is off" has to keep meaning "you
        // turned it off" rather than "something else is using it".
        snprintf(rate, sizeof(rate), "%s",
                 g_pageCapture.enabled.value ? "no frames" : "capture is off");
    }
    const ImVec2 rmn(mn.x, mx.y + 4.0f * s);
    const ImVec2 rmx(mx.x, mx.y + 4.0f * s + kHintSize * s * 1.4f);
    textCentred(dl, xf.pt(rmn), xf.pt(rmx), xf.s(kHintSize * s), xf.col(TextMuted), rate);
}

}  // namespace

void drawCaptureSection(ImDrawList* dl, float x, float& y, float w,
                        float bottomY, float s, float es, const Xf& xf,
                        Scroll& sc) {
    const float gap   = 12.0f * s;
    const float rowSl = widgets::kSliderRowH * s;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        // Capture page does not scroll (see sectionIsScrollable), so the offset
        // is always 0 — left in for parity with the rest of the right pane and
        // to make a future "let Capture scroll too" a one-line change.
        const ImVec2 p = xf.pt(wx, wy - sc.offset);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageCapture.size, "Size", 0, es);
    y += rowSl + gap;
    // Everything left below the slider belongs to the preview.
    drawCapturePreview(dl, x, y, w, bottomY - y, s, es, xf);
}

void syncCapturePage() {
    // ── Why inference cannot silently enable capture, and what does ─────────
    //
    // The obvious shape here is `setEnabled(switch || inferWants)`, and it used
    // to be exactly that. It is wrong, for a reason that only shows up in the
    // user's hands: with the switch off, holding to infer made `inferWants`
    // true, the module's `enabled()` went true, and frames started flowing. The
    // switch said OFF and capture happened anyway — indistinguishable from a
    // broken switch, and it hid the real problem (there was no frame source at
    // all) behind a detector that returned stale boxes forever.
    //
    // So the OR is gone. Asking for inference with the switch off now *turns
    // the switch on*, visibly, in this page's own state — which is the whole
    // difference between "capture off means no capture" and "capture off unless
    // something quietly overrules it".
    //
    // ── Edge, not level — this is the part that is easy to get wrong ────────
    //
    // Re-deriving "inference wants frames ⇒ turn the switch on" every frame
    // looks equivalent and is not: inference keeps wanting frames for as long as
    // it runs, so the write would re-apply forever and the user could never turn
    // capture back off. Flip the switch, next frame it is on again — the exact
    // un-turn-off-able control this change exists to remove, just wearing a
    // different hat.
    //
    // So the handler fires on the *rising edge* of `inferWants` only. Once
    // inference is running the switch is left alone, and turning it off stops
    // the frames — with the detector starved, which is the honest consequence of
    // the switch the user just flipped, rather than a silent override.
    //
    // ── Ordering, and why it is load-bearing ───────────────────────────────
    //
    // hud.cpp calls syncCapturePage() *before* syncSettingsPage(), and the
    // warning that says "请先开启截图否则无法推理" lives in the latter, gated on
    // `!capture::enabled()`. So on the frame the user flips inference on with
    // capture off, this function has already written the switch on and called
    // setEnabled(true) by the time the Settings handler looks — it sees capture
    // on, and correctly stays quiet. The user gets one toast ("capture was
    // turned on for you"), not two contradicting ones.
    //
    // Swap those two lines in hud.cpp and both fire on the same frame: "you
    // must turn capture on" immediately followed by "capture is now on", which
    // reads as a glitch. Nothing else would notice, which is why it is written
    // down here rather than left to be rediscovered.
    static bool lastInferWants = false;
    const bool inferWants = infer::runtime::wantsFrames();
    if (inferWants && !lastInferWants && !g_pageCapture.enabled.value) {
        // Writing the switch is what makes the page honest rather than just the
        // module: the toggle is drawn from this same struct, so it flips visibly
        // instead of the module silently disagreeing with what is on screen.
        g_pageCapture.enabled.value = true;
        notify::info(kCaptureAutoTag,
                     "推理需要画面，已自动开启截图", 4.0f);
    }
    lastInferWants = inferWants;

    capture::setEnabled(g_pageCapture.enabled.value);
    capture::setSize(static_cast<int>(g_pageCapture.size.value));
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
