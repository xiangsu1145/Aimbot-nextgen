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
#include "ui/gui/sections/touch_section.h"
#include "ui/gui/sections/trigger_section.h"
#include "ui/gui/sections/backflash_section.h"

#define TAG "AimbotNg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace config {
namespace {

using nlohmann::json;

constexpr const char* kDir  = "/data/local/tmp/aimbotng";
constexpr const char* kPath = "/data/local/tmp/aimbotng/config.json";

// ── Controller-gain schema version ──────────────────────────────────────────
//
// Bumped whenever the MEANING of kp/ki/kd changes, which forces the stored
// values to be discarded in favour of the new defaults. It has now happened
// twice, and both times a stale file would have been actively harmful:
//
//   v1 → the original predictive PPID, gains on a 0–50 scale.
//   v2 → the DsAi-unit rewrite. A stored kp = 25 would have clamped to the new
//        ceiling of 1.0, i.e. 2.5x past the stability limit.
//   v3 → the per-frame-reset fix (see tracking/pid_controller.h). This one is
//        the important one: while the bug was live the controller had NO
//        integral and the derivative was a hidden proportional gain of kd/dt,
//        so EVERY number the user arrived at was a setting that worked around
//        a broken plant. kd = 0 in particular is the value the shake forced
//        them onto; carrying it forward would silently throw away the damping
//        that the working derivative now provides. There is no way to tell
//        "chose 0 deliberately" from "chose 0 because it was the only quiet
//        option", so the gains are re-seeded and the user starts from a
//        controller that actually means what its labels say.
//   v4 → the units-and-bounds repair. kd is PER STEP now instead of per-second
//        (its old real weight was kd*120, so every stored value is off by that
//        factor and its old ceiling of 0.008 was a sliver of the new one), the
//        derivative gained a low-pass so it stops being a noise amplifier, the
//        integral got its own tighter ceiling and conditional integration, the
//        output ceiling stopped being a slider, and 速度前馈's range widened to
//        cover 1/alpha because that is what it actually is. A stored
//        kd = 0.002 read under the new units is a kd of 0.002 — i.e. off — so
//        the damping that repairs the loop would silently vanish. Same argument
//        as v3, one layer deeper.
//   v5 → the feed-forward repair. 速度前馈 stopped being a 1/alpha calibration
//        and became a scale on a self-calibrating estimator inside the
//        controller.
//   v6 → the feed-forward DELETED. The estimator was a bare integrator in a
//        delay loop and it oscillated (1453 px peak-to-peak at alpha = 2.5); its
//        gate could not work either, because the tracker reports the box's
//        SCREEN velocity, which the aim itself nulls. The integral is the term
//        that holds a moving target now (leash 24 -> 56, ki is no longer a small
//        mop-up gain), and 速度前馈's row is 灵敏度补偿 — a master multiplier on
//        all three gains that absorbs the game's sensitivity. Every stored kp/ki/
//        kd was tuned against a controller with an oscillator inside it, and the
//        stored 速度前馈 is a scale on a term that no longer exists, so all four
//        are re-seeded. See notes 1-4 in tracking/pid_controller.h.
//   v7 → the feed-forward REPAIRED, and the ceilings made per-sensitivity. v6
//        was right that the old estimator was a bare integrator, and wrong that
//        the answer was deletion: the remaining loop had no DC carrier, so it
//        either lagged (P alone: e_ss = ΔT/(alpha·kp)) or wound up (the integral
//        alone: 137 px of overshoot and 842 frames to settle at alpha = 0.1).
//        The repair was that the reconstruction constant must be 1/kf — the SAME
//        number as the gain — and that 1/alpha is not measurable, so the number
//        had to come from the user. v7 supplied it TWICE: once as 灵敏度补偿
//        (≈ 1/alpha, scaling the gains AND the ceilings) and once as 前馈增益.
//        That scaling layer is gone; see v8.
//   v8 → the scaling layer REMOVED, and the feed-forward's sample DELAY-ALIGNED.
//        There is no 灵敏度补偿 any more and no 输出限幅 scaling: kp, ki, kd and
//        kf go into the controller as written, and the ceilings are the constants
//        kOutLimitPx (180) / kTrimLimitPx (90). Two reasons, both measured:
//
//        (a) The reconstruction was using u(k−1). But Δe(k) = w(k) − alpha·u(k−L),
//            so the sample to subtract is the one L steps back; with u(k−1) the
//            leftover is proportional to our own rate of change and is read back
//            as target velocity. At alpha = 1 NO value of kf repaired it
//            (pp 656…871 px for kf 0.5…8). With u(k−L) the sway is 5–16 px at
//            every alpha from 0.08 to 1.0 when kf = 1/alpha — and it does not
//            care if L is wrong, so L is a constant (5), not a setting.
//        (b) With one number instead of two, kf alone carries the plant gain and
//            the rule becomes exactly kf = 1/alpha, usable band kf·alpha ≈
//            0.75…1.5. Note a SMALL non-zero kf is WORSE than 0: it makes
//            alpha_hat = 1/kf huge and F degenerates into "repeat your own
//            delayed command". Stored values are therefore re-seeded, not kept.
//
//        kp/ki/kd keep their per-step meanings, but the ceilings they work
//        against changed, so the whole set is re-seeded. Default kf = 3, correct
//        for a low-sensitivity game and safe down to alpha ≈ 0.05.
//        See scripts/aim_pidf_bench.py 表14 and tracking/pid_controller.h.
//
//  8 → 9  kf CHANGED MEANING, so the stored number is not migrated, only
//        re-seeded. It was the feed-forward gain AND, through its reciprocal,
//        the reconstruction constant — one number doing two jobs, which forced
//        the residual self-term to be (1 − kf·alpha)·u. Expanded, that is
//        ff = kf·LPF(Δe) + LPF(u(k−L)) — the second term has gain exactly 1,
//        i.e. the controller was adding its own delayed command back into its
//        input: a pole at z = 1, an accelerator with no brake. It tracked (a DC
//        pole integrates a steady target to zero error) and it never settled
//        (the same pole is marginally stable, so detector noise kept it moving)
//        and it overshot when the target stopped, WITH ki = 0 — and ki simply
//        stacked a second integrator on top (measured 3.3 px overshoot at ki = 0
//        against 1554 px at ki = 0.5).
//
//        In 9 the reconstruction constant is estimated on-line (AlphaEstimator,
//        derived from the fact that our own command history is known exactly and
//        is the one regressor a target's smooth motion does not mimic) and kf is
//        a plain STRENGTH.
//
//        In 10 that strength's RANGE changes from 0…0.20 to 0…2.0 and the default
//        from 0.20 to 1.00. The reconstruction made kf scale-free — its share of
//        the DC carrier is kf·alpha/alpha_hat ≈ 0.8·kf, the same on every game —
//        so the correct value is the CONSTANT ≈1.0 and 0.20 was simply below it:
//        that ceiling is why the user's aim could not keep up while showing no
//        overshoot. A stored 9 value (0.08…0.20) is a strength on the same scale
//        but a wrong one, and reading it back would keep the user in the failure
//        zone, so the set is re-seeded rather than migrated. See the round-12
//        section of tracking/pid_controller.h and 表6 of
//        scripts/aim_gate_lockout_bench.py.
// v11 (round 14): kf's RANGE and WORKING VALUE both moved — 0.00–1.20 with 0.80
// as the value, where the residual self-copy of our own delayed command is
// strictly inside the unit circle for any estimator output. A stored 1.00 is
// the fragile edge rather than an error, so it is deliberately NOT carried
// across. kp/ki/kd are: they have meant the same thing in every schema, and
// re-seeding them would throw away tuning the user did himself.
// v12 (round 15): kf's range became the user's own — 0.00–0.50 with 0.05 as the
// working value — after his own tuning (kf 0, kp 0.05, ki 0.20, kd 0.26) showed
// the INTEGRAL carrying the DC command on its own. The meaning did not change
// (still a plain strength), but a stored 0.80 is above the new ceiling and would
// load as 0.50 — a value he never chose — so the row is re-seeded rather than
// clamped into place. kp/ki/kd are NOT touched: they have meant the same thing in
// every schema, and re-seeding them would throw away his tuning.
// v13 (round 22): the controller was REPLACED, not retuned — see the header of
// tracking/predictive_pid.h. kp/ki/kd are re-seeded for the first time, and 前馈
// (still stored under the "kf" key, still the row in the same place) changes
// meaning from "a strength on a feed-forward scaled by 1/alpha_hat" to "the
// fraction of the carrier the prediction term supplies", range 0.00–0.80 with
// 0.80 as the working value. Everything the old file stored about the loop is
// therefore not merely mistuned, it is about a different loop, which is exactly
// the case this constant exists for.
constexpr int kCtlSchema = 14;

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
        putSlider(o, "outSmooth", a.outSmooth);
        // The gains above are only meaningful under the schema that produced
        // them — see kCtlSchema. Written every save so a file from an older
        // build is recognised on the next load and re-seeded rather than
        // reinterpreted. "predictX"/"predictY"/"rate" are gone entirely: old
        // files simply carry them as dead keys.
        o["ctl"] = kCtlSchema;
        putSlider(o, "delay", a.aimDelayFrames);
        // "kf" is the feed-forward STRENGTH (0…0.20) — a plain knob since v9. It
        // used to be the gain AND, through its reciprocal, the reconstruction
        // constant; that is what made ff add our own delayed command back into
        // the input at gain 1. The constant is now derived on-line and nothing
        // else about it is stored. "ff" (v7's 灵敏度补偿) is gone.
        putSlider(o, "kf", a.ffGain);
        // "trackPred" is NOT written here. It moved to the settings object in
        // round 18 with the slider itself (see the "settings" block below):
        // 丢框预测帧数 is tracker behaviour, not a gain, and the file now says so.
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
        // Back-Flash page — every user-tuned value persisted so the swipe
        // gesture keeps its settings across restarts. Same shape as the Aim /
        // Trigger pages: master switch, the four sliders, the touch-area box,
        // and the flash-category mask.
        const auto& b = ui::sections::g_pageBackFlash;
        json o;
        putSwitch(o, "on", b.enabled);
        putSlider(o, "flash", b.flashDelay);
        putSlider(o, "reset", b.resetDelay);
        putSlider(o, "slide", b.slideDistancePx);
        putSlider(o, "ppf", b.maxPerFramePx);
        putBox(o, "touch", b.touchArea.toggle.value,
               b.touchArea.x, b.touchArea.y, b.touchArea.w, b.touchArea.h,
               b.touchArea.placed);
        o["mask"] = b.category.sel.mask;
        j["backflash"] = std::move(o);
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
        putSwitch(o, "tracking", s.showTracking);
        putSwitch(o, "touchPass", s.touchPassthrough);
        putSwitch(o, "contInf", s.continuousInference);
        putSlider(o, "trackIou",  s.trackIou);
        putSlider(o, "trackConf", s.trackConfirm);
        putSlider(o, "trackTerm", s.trackTerminate);
        // trackPred lives on the Aim page now → persisted under aim.trackPred
        putCircle(o, "infArea", s.inferenceArea.toggle.value,
                  s.inferenceArea.cx, s.inferenceArea.cy, s.inferenceArea.r,
                  s.inferenceArea.placed);
        j["settings"] = std::move(o);
    }
    {
        // The Touch page's own setting. Its panel dropdown is deliberately NOT
        // stored — which /dev/input node is the touchscreen is a property of the
        // device, not a preference, and a saved path would be wrong the moment
        // the same config meets a second phone.
        const auto& t = ui::sections::g_pageTouch;
        json o;
        o["inject"] = t.injectBackend.value;
        j["touch"] = std::move(o);
    }
    return j;
}

// ── Apply: json -> pages ────────────────────────────────────────────────────

void apply(const json& j) {
    if (const auto it = j.find("aim"); it != j.end() && it->is_object()) {
        auto& a = ui::sections::g_pageAim;
        const json& o = *it;
        getSwitch(o, "on", a.enabled);
        // The controller gains are only restored when the file was written by a
        // build whose gains MEAN the same thing — see kCtlSchema. Reading them
        // across a schema change is worse than discarding them: a stored 25
        // (0–50 scale) clamps to the top of a 0–1 range, which is 2.5x past the
        // stability limit, and a stored kd = 0 (the value the old shake forced
        // the user onto) silently disables the damping the fixed derivative
        // provides.
        const auto ctlIt = o.find("ctl");
        const int  storedCtl = (ctlIt != o.end() && ctlIt->is_number())
                                   ? static_cast<int>(ctlIt->get<double>())
                                   : 0;
        // kp/ki/kd were read UNCONDITIONALLY from round 14 to round 21, on the
        // grounds that they had meant the same thing in every schema and that
        // re-seeding them discards tuning the user did himself. ROUND 22 IS THE
        // EXCEPTION, and it is worth being precise about why, because the rule
        // is a good one and this is not a licence to break it casually:
        //
        // their UNITS did not change (px of output per px of error, per second,
        // per step), but the LOOP THEY WERE TUNED AGAINST no longer exists. The
        // controller was replaced wholesale — the integral stopped being the
        // carrier, a prediction term took over 80 % of it, and nothing is
        // estimated on-line any more. A stored 0.04 was arrived at by balancing
        // kp against an integral that had to hold the whole standing command;
        // against a loop where the prediction term holds it, 0.04 is simply too
        // soft, and it would read as "跟枪跟不上" on the first engagement and
        // look like a fault in the new controller. Re-seeding is the lesser
        // evil, and it happens once.
        const bool sameSchema = (storedCtl == kCtlSchema);
        if (sameSchema) {
            getSlider(o, "kp", a.kp);
            getSlider(o, "ki", a.ki);
            getSlider(o, "kd", a.kd);
            getSlider(o, "outSmooth", a.outSmooth);
            getSlider(o, "delay", a.aimDelayFrames);
        } else {
            // The v14 defaults, written here rather than read back off the
            // sliders, so that the values re-seeded are the shipped ones and not
            // whatever a partially-applied file left behind.
            //
            // 输出平滑 and 延迟补偿 moved INTO this block in v14, where they used
            // to be read unconditionally. They belong to the loop, and a value
            // tuned for the old loop is not a value for this one: 延迟补偿 in
            // particular is a third self-referential path (setpoint += box
            // velocity x τ, and the box's screen velocity contains our own
            // output) that the new controller does not want on by default, so a
            // stored 0.90 — found by trial against the old loop — would arrive
            // here as an unexplained oscillator and look like the new code being
            // broken again.
            a.kp.value = 0.05f;
            a.ki.value = 0.10f;
            a.kd.value = 0.15f;
            a.ffGain.value = tracking::kLookaheadDefault;
            a.outSmooth.value = 1.0f;
            a.aimDelayFrames.value = 0.0f;
            LOGI("config: controller schema %d (file had %d) — the loop was "
                 "replaced, so the whole gain block is re-seeded: "
                 "kp/ki/kd/前馈/输出平滑/延迟补偿 = %.2f/%.2f/%.2f/%.2f/%.2f/%.2f",
                 kCtlSchema, storedCtl, a.kp.value, a.ki.value, a.kd.value,
                 a.ffGain.value, a.outSmooth.value, a.aimDelayFrames.value);
        }
        // kf is read only from a same-schema file, because its range and its
        // working value have moved at almost every revision (v7 scaled it by
        // 灵敏度补偿, v8 made it 1/alpha, v9 stopped its range at 0.20, v10 let it
        // reach 1.00, v11 narrows the range to 1.20 around a working value of
        // 0.80). Reading it unconditionally would silently overwrite the value
        // re-seeded above — the "kf re-seeded" log line would be a lie, and a
        // number that meant something else under the old controller would return.
        //
        // ROUND 19 widens the ceiling to 0.80 and deliberately leaves kCtlSchema
        // where it is. A schema bump exists to stop a stored number being
        // REINTERPRETED across a change of meaning, and nothing about the meaning
        // moved here: 0.05 is still 0.05, and a stored 0.50 is still legal under
        // the wider range. Bumping would throw away the user's own kf for no
        // reason, which is the one behaviour this whole block exists to avoid.
        // The gain that actually REACHES the axes is capped separately, and only
        // while alpha_hat is uncorroborated — see kFfGuardStrengthMax.
        if (storedCtl == kCtlSchema) {
            getSlider(o, "kf", a.ffGain);
        }
        // "trackPred" — the LEGACY location. Round 18 moved the slider to the
        // Settings page, so a new file carries it under settings.trackPred and
        // this read simply finds nothing; an older file still has it here, and
        // reading it into the (now settings-owned) slider is what lets that value
        // survive the move. The settings read runs LATER, so a new-format file's
        // value still wins.
        getSlider(o, "trackPred", ui::sections::g_pageSettings.trackPredictHoldFrames);
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
    if (const auto it = j.find("backflash"); it != j.end() && it->is_object()) {
        auto& b = ui::sections::g_pageBackFlash;
        const json& o = *it;
        getSwitch(o, "on", b.enabled);
        getSlider(o, "flash", b.flashDelay);
        getSlider(o, "reset", b.resetDelay);
        getSlider(o, "slide", b.slideDistancePx);
        getSlider(o, "ppf", b.maxPerFramePx);
        bool on = false, placed = false;
        float x = -1, y = -1, w = 0, h = 0;
        if (getBox(o, "touch", on, x, y, w, h, placed)) {
            b.touchArea.toggle.value = on;
            b.touchArea.toggle.anim  = on ? 1.0f : 0.0f;
            b.touchArea.x = x; b.touchArea.y = y;
            b.touchArea.w = w; b.touchArea.h = h;
            b.touchArea.placed = true;
        }
        if (const auto m = o.find("mask"); m != o.end() && m->is_number_unsigned()) {
            // A saved mask means the user has configured this page — claim it
            // now so the first model sync keeps it instead of selecting all.
            b.category.sel.mask = m->get<uint32_t>();
            b.category.synced   = true;
        }
    }
    if (const auto it = j.find("settings"); it != j.end() && it->is_object()) {
        auto& s = ui::sections::g_pageSettings;
        const json& o = *it;
        getSwitch(o, "fps", s.fpsOverlay);
        getSwitch(o, "antiShot", s.antiScreenshot);
        getSwitch(o, "boxes", s.showDetections);
        getSwitch(o, "tracking", s.showTracking);
        getSwitch(o, "touchPass", s.touchPassthrough);
        getSwitch(o, "contInf", s.continuousInference);
        getSlider(o, "trackIou",  s.trackIou);
        getSlider(o, "trackConf", s.trackConfirm);
        getSlider(o, "trackTerm", s.trackTerminate);
        // 丢框预测帧数 — the slider moved here in round 18. See the legacy read in
        // the aim block above for how an older file's value survives the move.
        getSlider(o, "trackPred", s.trackPredictHoldFrames);
        bool on = false, placed = false;
        float cx = -1, cy = -1, r = 0;
        if (getCircle(o, "infArea", on, cx, cy, r, placed)) {
            s.inferenceArea.toggle.value = on;
            s.inferenceArea.toggle.anim  = on ? 1.0f : 0.0f;
            s.inferenceArea.cx = cx;
            s.inferenceArea.cy = cy;
            s.inferenceArea.r  = r;
            s.inferenceArea.placed = placed;
        }
    }
    if (const auto it = j.find("touch"); it != j.end() && it->is_object()) {
        if (const auto v = it->find("inject"); v != it->end() && v->is_number_integer()) {
            // Clamped to the two backends that exist. A config written by a
            // build with three of them must not index past the dropdown's items
            // — the widget trusts this number.
            const int wanted = v->get<int>();
            auto& backend = ui::sections::g_pageTouch.injectBackend;
            backend.value = (wanted == 0 || wanted == 1) ? wanted : 0;
        }
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
