// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::trigger — the Trigger page (扳机).
//
//  Ported from the old project (G:\ai\Aimbot-ai\android-client):
//    controller/TriggerController.kt — reaction/cooldown timing state machine,
//      screen-centre-in-box hit test, fire-tap at a random point of the fire
//      area, manual-fire suppression while a finger is on the fire zone.
//    view/GuiPanelView.kt (buildTriggerbot) — slider ranges/defaults.
//    manager/ConfigManager.kt — parameter defaults.
//
//  Changed vs. the old project ("学一下自瞄"):
//    * 触摸区域 — the fire area is a draggable/resizable dashed-box overlay,
//      same interaction as the Aim page's touch area. Taps land at a random
//      point inside it (old: AREA_INDEX_FIRE random tap).
//    * 触发区域 — a draggable/scalable circle overlay, same interaction as the
//      Aim page's trigger area. With 持续触发 OFF, firing requires a real
//      finger held inside this circle (old: hold-to-aim fire-zone semantics);
//      with 持续触发 ON the circle's switch is hidden and fire needs no finger.
//    * 持续触发 — continuous trigger switch, same as the Aim page.
//    * Slider ranges/defaults are taken straight from the old project.
//
//  NOTE on 上下波动 (up/down fluctuation): the old project stored both values
//  and showed them as ms sliders but never read them in TriggerController.
//  Here they humanise the timing: each shot's required wait is
//      wait = base + U(-downFluct, +upFluct) ms  (clamped to >= 10 ms)
//  re-rolled per shot, so the fire rhythm is not metronome-regular.
//
//  Tap injection uses UINPUT_SLOT_TRIGGER / UINPUT_ID_TRIGGER, a different
//  slot from the Aim finger (PRIMARY), so aim and trigger never fight over
//  one touch point. The tap is non-blocking: down now, up once
//  `touchDuration` ms have elapsed (checked every frame), because the render
//  loop runs at 60 Hz while inference is active — a blocking sleep would
//  jank the menu.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/gui/widgets.h"

namespace aimbotng {
namespace ui {
namespace sections {

// ── Trigger touch-area overlay ──────────────────────────────────────────
// The fire zone: taps land at a random point inside this rectangle.
// Same drag/resize interaction as the Aim page's touch area.
struct TriggerTouchArea {
    widgets::SwitchState toggle;
    float x = -1.0f, y = -1.0f;
    float w = 600.0f, h = 400.0f;
    bool  placed = false;
    enum class Drag {
        None,
        Move,
        EdgeL, EdgeR, EdgeT, EdgeB,
        CornerTL, CornerTR, CornerBL, CornerBR,
    };
    Drag  drag    = Drag::None;
    float anchorX = 0.0f, anchorY = 0.0f;
    float startX  = 0.0f, startY  = 0.0f;
    float startW  = 0.0f, startH  = 0.0f;
};

// ── Trigger-area overlay ────────────────────────────────────────────────
// The hold zone: with continuous-trigger OFF, a real finger must be held
// inside this circle for the trigger to fire. Same drag/resize interaction
// as the Aim page's trigger area (interior = move, N/E/S/W handles = scale).
struct TriggerHoldArea {
    widgets::SwitchState toggle;
    float cx = -1.0f, cy = -1.0f;
    float r  = 200.0f;
    bool  placed = false;
    enum class Drag {
        None,
        Move,
        ResizeN, ResizeE, ResizeS, ResizeW,
    };
    Drag  drag    = Drag::None;
    float anchorX = 0.0f, anchorY = 0.0f;
    float startCX = 0.0f, startCY = 0.0f;
    float startR  = 0.0f;
};

// ── Trigger-category filter ─────────────────────────────────────────────
// Same shape as the Aim page's AimCategoryState: the widget mask selects
// which model classes may fire the trigger. First sync with a non-empty
// class list selects everything.
struct TriggerCategoryState {
    widgets::MultiSelectState sel;
    std::vector<std::string> items;
    std::vector<const char*> itemPtrs;
    bool synced = false;

    void syncFromModel(const std::vector<std::string>& modelClasses);

    bool isClassSelected(int cls) const {
        if (cls < 0 || cls >= 32) return false;
        return (sel.mask & (1u << cls)) != 0;
    }

    bool hasAnySelected() const { return sel.mask != 0; }
};

// ── The Trigger page itself ─────────────────────────────────────────────
struct PageTrigger {
    widgets::SwitchState enabled{true};

    // Timing + tap sliders — ranges/defaults from the old project's
    // GuiPanelView.buildTriggerbot().
    widgets::SliderState reaction{100.0f, 10.0f, 500.0f, 1.0f};   // 反应速度 ms
    widgets::SliderState cooldown{200.0f, 10.0f, 1000.0f, 1.0f};  // 冷却时间 ms
    widgets::SliderState upFluct{3.0f, 0.0f, 50.0f, 1.0f};         // 向上波动 ms
    widgets::SliderState downFluct{3.0f, 0.0f, 50.0f, 1.0f};       // 向下波动 ms
    widgets::SliderState touchDuration{10.0f, 1.0f, 50.0f, 1.0f};  // 触摸时间 ms

    /// 持续触发 — when on, the hold-area switch is hidden on the page and
    /// fire needs no finger. The hold area's `toggle.value` is left untouched
    /// so the configured circle comes back when this is turned off again.
    widgets::SwitchState continuousTrigger{false};

    /// 触摸区域 (fire zone) + 触发区域 (hold zone) overlays.
    TriggerTouchArea touchArea;
    TriggerHoldArea  holdArea;

    /// 触发类别 filter over the model's classes.
    TriggerCategoryState category;

    // ── Firing state machine (old: lastTriggerMs / triggerFired) ────────
    int64_t enterMs = 0;        // when the crosshair entered the target (0 = off target)
    bool    fired = false;      // first shot of this engagement done
    int64_t currentWaitMs = 0;  // jittered wait for the pending shot
    // ── Pending tap (non-blocking down/up pair) ─────────────────────────
    bool    tapDown = false;    // trigger finger currently down
    int64_t tapUpAtMs = 0;      // steady-clock ms when the up is due
    uint32_t rng = 0x7E16u;     // LCG state for tap-point + jitter randoms
};

extern PageTrigger g_pageTrigger;

void drawTriggerSection(ImDrawList* dl, float x, float& y, float w,
                        float bottomY, float s, float es, const Xf& xf);

/// Draw the Trigger page's screen-space overlays (touch-area dashed box and
/// hold-area circle), including all drag/interaction handling. Called from
/// drawHud() every frame, regardless of menu visibility.
void drawTriggerOverlays();

/// Per-frame trigger loop: gates (master switch, uinput, model, hold), the
/// screen-centre-in-box hit test, the reaction/cooldown state machine, and
/// the non-blocking tap injection. Called from drawHud() after syncAimPage()
/// (which keeps the Kalman tracker warm) so the trigger reads smoothed
/// tracks.
void syncTriggerPage();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
