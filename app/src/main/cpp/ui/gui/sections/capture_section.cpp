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
#include "ui/gui/theme.h"

namespace aimbotng {
namespace ui {
namespace sections {

PageCapture g_pageCapture;

namespace {
using namespace theme;

constexpr float kPreviewRound = 12.0f;
constexpr float kHintSize     = 24.0f;

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
                        float bottomY, float s, float es, const Xf& xf) {
    const float gap   = 12.0f * s;
    const float rowSl = widgets::kSliderRowH * s;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        const ImVec2 p = xf.pt(wx, wy);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    widgets::sliderFloat(dl, wRect(x, y, w, rowSl), g_pageCapture.size, "Size", 0, es);
    y += rowSl + gap;
    // Everything left below the slider belongs to the preview.
    drawCapturePreview(dl, x, y, w, bottomY - y, s, es, xf);
}

void syncCapturePage() {
    // The switch means "I want the preview's frames". Inference is a second,
    // independent reason for the producer to run, and it is ORed in *here*
    // rather than left to the runtime to arrange, because `capture::enabled()`
    // is what the frame thread obeys: a detector that asked for frames while
    // this said off would get nothing at all and look broken, with the reason
    // three modules away.
    //
    // The page stays the single author either way — one function writes both
    // values, so the switch cannot end up contradicting the module.
    const bool inferWants = infer::runtime::wantsFrames();
    capture::setEnabled(g_pageCapture.enabled.value || inferWants);
    capture::setSize(static_cast<int>(g_pageCapture.size.value));
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
