// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::config — page parameters persisted as JSON.
//
//  Saves what the user can see and touch: every slider / switch /
//  category-mask on the Aim and Trigger pages, the Capture size, and the
//  visible Settings switches — into /data/local/tmp/aimbotng/config.json.
//  Every row the user can SEE is stored, and that includes the tracker tuning
//  on Settings (trackIou / 确认帧 / 丢失帧 / 丢框预测帧数) and the aim-time rows
//  on the Aim page (死区 / 延迟补偿 / 输出平滑). An earlier version of this
//  comment claimed the tracker rows were "deliberately NOT stored" and that
//  Aim 提前量 was hidden — both stopped being true the moment those rows got
//  sliders, and a stale note here is a trap for the next reader.
//
//  Two entry points, both cheap:
//    load()         — read the file into the page states. Called once from
//                     ui::start(), before the render thread spins up, so the
//                     first frame already uses the saved values.
//    pollAutosave() — called every frame from drawHud(); serializes the
//                     pages at most once per 2 s and rewrites the file only
//                     when something actually changed. No per-widget hooks:
//                     the sections never know this exists.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

namespace aimbotng {
namespace config {

/// Read config.json into g_pageAim / g_pageTrigger / g_pageCapture /
/// g_pageSettings. Missing or corrupt file = keep the compiled-in defaults.
void load();

/// Throttled dirty-check + write. Safe to call every frame.
void pollAutosave();

}  // namespace config
}  // namespace aimbotng
