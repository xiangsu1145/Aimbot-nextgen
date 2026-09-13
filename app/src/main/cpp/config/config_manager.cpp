// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::config — implementation. See config_manager.h for the contract.
//
//  JSON comes from the vendored nlohmann/json single header (MIT, under
//  include/nlohmann/) — no hand-rolled parsing. Only `.value` of each
//  widget is stored; animation fields (ease/target/anim/placed) are either
//  re-derived on first draw (sliders snap when placed==false) or set
//  alongside (switch anim).
// ─────────────────────────────────────────────────────────────────────────────
#include "config/config_manager.h"

#include <android/log.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "nlohmann/json.hpp"
#include "ui/gui/sections/aim_section.h"
#include "ui/gui/sections/capture_section.h"
#include "ui/gui/sections/settings_section.h"
#include "ui/gui/sections/trigger_section.h"

#define TAG "AimbotNg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace config {
namespace {

using nlohmann::json;

constexpr const char* kDir  = "/data/local/tmp/aimbotng";
constexpr const char* kPath = "/data/local/tmp/aimbotng/config.json";

// ── Widget helpers: store/restore `.value` only ─────────────────────────────

void putSlider(json& o, const char* k, const ui::widgets::SliderState& s) {
    o[k] = s.value;
}

void getSlider(const json& o, const char* k, ui::widgets::SliderState& s) {
    const auto it = o.find(k);
    if (it == o.end() || !it->is_number()) return;
    // Sliders snap to .value on their first draw (placed==false), so setting
    // .value alone is enough — no fly-in animation from the default.
    s.value = std::clamp(it->get<float>(), s.min, s.max);
}

void putSwitch(json& o, const char* k, const ui::widgets::SwitchState& s) {
    o[k] = s.value;
}

void getSwitch(const json& o, const char* k, ui::widgets::SwitchState& s) {
    const auto it = o.find(k);
    if (it == o.end() || !it->is_boolean()) return;
    s.value = it->get<bool>();
    s.anim  = s.value ? 1.0f : 0.0f;
}

// Box overlay (touch / fire area) and circle overlay (trigger / hold area).
// Geometry is in screen px; restored only when it looks sane, otherwise the
// page keeps "not placed yet" and centres itself on first draw as usual.
void putBox(json& o, const char* k, bool on,
            float x, float y, float w, float h, bool placed) {
    o[k] = {{"on", on}, {"x", x}, {"y", y}, {"w", w}, {"h", h}, {"placed", placed}};
}

bool getBox(const json& o, const char* k, bool& on,
            float& x, float& y, float& w, float& h, bool& placed) {
    const auto it = o.find(k);
    if (it == o.end() || !it->is_object()) return false;
    const json& b = *it;
    if (!b.value("placed", false)) return false;
    const float bw = b.value("w", 0.0f), bh = b.value("h", 0.0f);
    if (!(bw > 0.0f) || !(bh > 0.0f)) return false;
    on     = b.value("on", false);
    x      = b.value("x", -1.0f);
    y      = b.value("y", -1.0f);
    w      = bw;
    h      = bh;
    placed = true;
    return true;
}

void putCircle(json& o, const char* k, bool on, float cx, float cy, float r, bool placed) {
    o[k] = {{"on", on}, {"cx", cx}, {"cy", cy}, {"r", r}, {"placed", placed}};
}

bool getCircle(const json& o, const char* k, bool& on,
               float& cx, float& cy, float& r, bool& placed) {
    const auto it = o.find(k);
    if (it == o.end() || !it->is_object()) return false;
    const json& c = *it;
    if (!c.value("placed", false)) return false;
    const float cr = c.value("r", 0.0f);
    if (!(cr > 0.0f)) return false;
    on     = c.value("on", false);
    cx     = c.value("cx", -1.0f);
    cy     = c.value("cy", -1.0f);
    r      = cr;
    placed = true;
    return true;
}

// ── Serialize: pages -> json ────────────────────────────────────────────────

json serialize() {
    json j;
    {
        const auto& a = ui::sections::g_pageAim;
        json o;
        putSwitch(o, "on", a.enabled);
        putSlider(o, "kp", a.kp);
        putSlider(o, "ki", a.ki);
        putSlider(o, "kd", a.kd);
        putSlider(o, "kf", a.kf);
        putSlider(o, "dz", a.deadzone);
        putBox(o, "touch", a.touchArea.toggle.value,
               a.touchArea.x, a.touchArea.y, a.touchArea.w, a.touchArea.h,
               a.touchArea.placed);
        putSwitch(o, "fusion", a.fusion);
        putSwitch(o, "cont", a.continuousTrigger);
        putCircle(o, "trig", a.triggerArea.toggle.value,
                  a.triggerArea.cx, a.triggerArea.cy, a.triggerArea.r,
                  a.triggerArea.placed);
        o["mask"] = a.aimCategory.sel.mask;
        json ydz = json::object();
        for (const auto& [idx, s] : a.yFollow) ydz[std::to_string(idx)] = s.value;
        o["ydz"] = std::move(ydz);
        j["aim"] = std::move(o);
    }
    {
        const auto& t = ui::sections::g_pageTrigger;
        json o;
        putSwitch(o, "on", t.enabled);
        putSlider(o, "reaction", t.reaction);
        putSlider(o, "cooldown", t.cooldown);
        putSlider(o, "up", t.upFluct);
        putSlider(o, "down", t.downFluct);
        putSlider(o, "touch", t.touchDuration);
        putBox(o, "fire", t.touchArea.toggle.value,
               t.touchArea.x, t.touchArea.y, t.touchArea.w, t.touchArea.h,
               t.touchArea.placed);
        putSwitch(o, "cont", t.continuousTrigger);
        putCircle(o, "hold", t.holdArea.toggle.value,
                  t.holdArea.cx, t.holdArea.cy, t.holdArea.r,
                  t.holdArea.placed);
        o["mask"] = t.category.sel.mask;
        j["trigger"] = std::move(o);
    }
    {
        const auto& c = ui::sections::g_pageCapture;
        json o;
        putSlider(o, "size", c.size);  // size only — the page switch is not stored
        j["capture"] = std::move(o);
    }
    {
        const auto& s = ui::sections::g_pageSettings;
        json o;
        putSwitch(o, "fps", s.fpsOverlay);
        putSwitch(o, "antiShot", s.antiScreenshot);
        putSwitch(o, "boxes", s.showDetections);
        j["settings"] = std::move(o);
    }
    return j;
}

// ── Apply: json -> pages ────────────────────────────────────────────────────

void apply(const json& j) {
    if (const auto it = j.find("aim"); it != j.end() && it->is_object()) {
        auto& a = ui::sections::g_pageAim;
        const json& o = *it;
        getSwitch(o, "on", a.enabled);
        getSlider(o, "kp", a.kp);
        getSlider(o, "ki", a.ki);
        getSlider(o, "kd", a.kd);
        getSlider(o, "kf", a.kf);
        getSlider(o, "dz", a.deadzone);
        bool on = false, placed = false;
        float x = -1, y = -1, w = 0, h = 0;
        if (getBox(o, "touch", on, x, y, w, h, placed)) {
            a.touchArea.toggle.value = on;
            a.touchArea.toggle.anim  = on ? 1.0f : 0.0f;
            a.touchArea.x = x; a.touchArea.y = y;
            a.touchArea.w = w; a.touchArea.h = h;
            a.touchArea.placed = true;
        }
        getSwitch(o, "fusion", a.fusion);
        getSwitch(o, "cont", a.continuousTrigger);
        float cx = -1, cy = -1, r = 0;
        if (getCircle(o, "trig", on, cx, cy, r, placed)) {
            a.triggerArea.toggle.value = on;
            a.triggerArea.toggle.anim  = on ? 1.0f : 0.0f;
            a.triggerArea.cx = cx; a.triggerArea.cy = cy; a.triggerArea.r = r;
            a.triggerArea.placed = true;
        }
        if (const auto m = o.find("mask"); m != o.end() && m->is_number_unsigned()) {
            // A saved mask means the user has configured this page — claim it
            // now so the first model sync keeps it instead of selecting all.
            a.aimCategory.sel.mask = m->get<uint32_t>();
            a.aimCategory.synced   = true;
        }
        if (const auto y = o.find("ydz"); y != o.end() && y->is_object()) {
            a.yFollow.clear();
            for (const auto& [k, v] : y->items()) {
                if (!v.is_number()) continue;
                const int idx = std::atoi(k.c_str());
                if (idx < 0 || idx >= 32) continue;
                ui::widgets::SliderState s{
                    std::clamp(v.get<float>(), 0.0f, 1.0f), 0.0f, 1.0f, 0.1f};
                a.yFollow.emplace(idx, s);
            }
        }
    }
    if (const auto it = j.find("trigger"); it != j.end() && it->is_object()) {
        auto& t = ui::sections::g_pageTrigger;
        const json& o = *it;
        getSwitch(o, "on", t.enabled);
        getSlider(o, "reaction", t.reaction);
        getSlider(o, "cooldown", t.cooldown);
        getSlider(o, "up", t.upFluct);
        getSlider(o, "down", t.downFluct);
        getSlider(o, "touch", t.touchDuration);
        bool on = false, placed = false;
        float x = -1, y = -1, w = 0, h = 0;
        if (getBox(o, "fire", on, x, y, w, h, placed)) {
            t.touchArea.toggle.value = on;
            t.touchArea.toggle.anim  = on ? 1.0f : 0.0f;
            t.touchArea.x = x; t.touchArea.y = y;
            t.touchArea.w = w; t.touchArea.h = h;
            t.touchArea.placed = true;
        }
        getSwitch(o, "cont", t.continuousTrigger);
        float cx = -1, cy = -1, r = 0;
        if (getCircle(o, "hold", on, cx, cy, r, placed)) {
            t.holdArea.toggle.value = on;
            t.holdArea.toggle.anim  = on ? 1.0f : 0.0f;
            t.holdArea.cx = cx; t.holdArea.cy = cy; t.holdArea.r = r;
            t.holdArea.placed = true;
        }
        if (const auto m = o.find("mask"); m != o.end() && m->is_number_unsigned()) {
            t.category.sel.mask = m->get<uint32_t>();
            t.category.synced   = true;
        }
    }
    if (const auto it = j.find("capture"); it != j.end() && it->is_object()) {
        getSlider(*it, "size", ui::sections::g_pageCapture.size);
    }
    if (const auto it = j.find("settings"); it != j.end() && it->is_object()) {
        auto& s = ui::sections::g_pageSettings;
        const json& o = *it;
        getSwitch(o, "fps", s.fpsOverlay);
        getSwitch(o, "antiShot", s.antiScreenshot);
        getSwitch(o, "boxes", s.showDetections);
    }
}

}  // namespace

void load() {
    FILE* f = std::fopen(kPath, "rb");
    if (!f) {
        LOGI("config: no %s, using defaults", kPath);
        return;
    }
    std::string data;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    std::fclose(f);

    const json j = json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LOGW("config: %s is corrupt, using defaults", kPath);
        return;
    }
    apply(j);
    LOGI("config: loaded from %s", kPath);
}

void pollAutosave() {
    using clock = std::chrono::steady_clock;
    static auto lastPoll = clock::now() - std::chrono::seconds(10);
    static std::string lastWritten;
    if (clock::now() - lastPoll < std::chrono::seconds(2)) return;
    lastPoll = clock::now();

    const std::string cur = serialize().dump();
    if (cur == lastWritten) return;

    mkdir(kDir, 0777);  // exists after first save; failure is fopen's problem
    FILE* f = std::fopen(kPath, "wb");
    if (!f) {
        LOGW("config: cannot write %s", kPath);
        return;
    }
    std::fwrite(cur.data(), 1, cur.size(), f);
    std::fclose(f);
    lastWritten = cur;
}

}  // namespace config
}  // namespace aimbotng
