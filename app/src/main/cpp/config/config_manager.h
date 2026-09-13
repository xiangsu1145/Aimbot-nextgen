// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::config — page parameters persisted as JSON.
//
//  Saves what the user can see and touch: every slider / switch /
//  category-mask on the Aim and Trigger pages, the Capture size, and the
//  visible Settings switches — into /data/local/tmp/aimbotng/config.json.
//  Rows that are compiled in but currently hidden (Aim 提前量, the tracker
//  tuning rows on Settings) are deliberately NOT stored.
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
