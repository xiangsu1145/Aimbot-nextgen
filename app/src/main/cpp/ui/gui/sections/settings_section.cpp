// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::settings — implementation. See settings_section.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "settings_section.h"

#include "ui/gui/aimbot_ui.h"
#include "ui/gui/hud.h"
#include "ui/gui/theme.h"
#include "tracking/kalman_tracker.h"

namespace aimbotng {
namespace ui {
namespace sections {

PageSettings g_pageSettings;

void drawSettingsSection(ImDrawList* dl, float x, float& y, float w,
                         float /*bottomY*/, float s, float es, const Xf& xf) {
    const float gap   = 12.0f * s;
    const float rowSw = widgets::kSwitchRowH   * s;
    const float rowSl = widgets::kSliderRowH   * s;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        const ImVec2 p = xf.pt(wx, wy);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.fpsOverlay, "FPS 显示", es);
    y += rowSw + gap;
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.antiScreenshot, "防截屏", es);
    y += rowSw + gap;
    widgets::switchButton(dl, wRect(x, y, w, rowSw), g_pageSettings.showDetections, "显示检测框", es);
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
}

void syncSettingsPage() {
    // Push the tracker tuning sliders into the global tracker config. Cheap;
    // runs every frame but only writes a few scalars.
    auto& cfg = tracking::trackerConfig();
    cfg.iouThreshold    = g_pageSettings.trackIou.value;
    cfg.confirmFrames   = static_cast<int>(g_pageSettings.trackConfirm.value + 0.5f);
    cfg.terminateFrames = static_cast<int>(g_pageSettings.trackTerminate.value + 0.5f);

    // Today only the Anti Screenshot switch on this page is wired to
    // anything outside the menu; the rest are pure UI state and stay here.
    // Watching just that switch keeps this function cheap (one compare per
    // frame) and matches the rhythm syncModelPage() / syncCapturePage()
    // already use — see drawHud() for the why-it-lives-here story.
    static bool lastAnti = true;
    const bool nowAnti = g_pageSettings.antiScreenshot.value;
    if (nowAnti == lastAnti) return;
    lastAnti = nowAnti;
    aimbotng::ui::setSkipScreenshot(nowAnti);
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
