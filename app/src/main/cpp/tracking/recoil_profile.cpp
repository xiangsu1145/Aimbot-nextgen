// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::recoil — implementation. See recoil_profile.h for the contract.
// ─────────────────────────────────────────────────────────────────────────────
#include "tracking/recoil_profile.h"

#include <android/log.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "nlohmann/json.hpp"

#define TAG "AimbotRecoil"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace recoil {

using nlohmann::json;

namespace {

/// Reads a whole file into `out`. False when it cannot be opened.
bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t wrote = std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return wrote == data.size();
}

/// `"AKM"` from `"AKM.json"` / `"AKM.tune.json"`.
std::string stemOf(const char* filename) {
    std::string s(filename);
    const std::string tune = kTuneExt;
    if (s.size() > tune.size() &&
        s.compare(s.size() - tune.size(), tune.size(), tune) == 0) {
        return s.substr(0, s.size() - tune.size());
    }
    const std::string ext = kProfileExt;
    if (s.size() > ext.size() &&
        s.compare(s.size() - ext.size(), ext.size(), ext) == 0) {
        return s.substr(0, s.size() - ext.size());
    }
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// A knot as written by either producer. Both `{"t":0,"x":0,"y":0}` and the
/// terse `[0, 0, 0]` form are accepted: the PC converter emits objects, and the
/// device recorder used to emit arrays — a profile measured before this build
/// must keep loading.
bool readKnot(const json& e, Knot& k) {
    if (e.is_object()) {
        if (!e.contains("t")) return false;
        k.tMs = e.value("t", 0.0f);
        k.dx  = e.value("x", 0.0f);
        k.dy  = e.value("y", 0.0f);
        return true;
    }
    if (e.is_array() && e.size() >= 3) {
        k.tMs = e[0].get<float>();
        k.dx  = e[1].get<float>();
        k.dy  = e[2].get<float>();
        return true;
    }
    return false;
}

json knotJson(const Knot& k) {
    return json{{"t", k.tMs}, {"x", k.dx}, {"y", k.dy}};
}

/// `<dir>/<stem>.tune.json` — the overlay that sits beside `<stem>.json`.
std::string tunePathOf(const std::string& stem) {
    return std::string(kProfileDir) + "/" + stem + kTuneExt;
}

}  // namespace

// ── Profile ─────────────────────────────────────────────────────────────────

float Profile::endDx() const { return knots.empty() ? 0.0f : knots.back().dx; }
float Profile::endDy() const { return knots.empty() ? 0.0f : knots.back().dy; }

float Profile::spanMs() const { return knots.empty() ? 0.0f : knots.back().tMs; }

void Profile::resetTune() {
    // One slot per shot, so the editor can address shot k directly. Sized off
    // whichever count is larger: `shots` is what the producer claimed, and
    // `marks.size()` is what actually came out of the file — a mismatch (a
    // truncated burst) must not index past the vector.
    const size_t n = std::max<size_t>(
        1, std::max(static_cast<size_t>(shots), marks.size()));
    tuneX.assign(n, 0.0f);
    tuneY.assign(n, 0.0f);
}

bool Profile::load(const std::string& jsonPath) {
    loaded = false;
    error.clear();
    path = jsonPath;

    std::string data;
    if (!readFile(jsonPath, data)) {
        error = "cannot open";
        return false;
    }
    const json j = json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "malformed json";
        return false;
    }

    schema = j.value("schema", 1);
    name   = j.value("name", stemOf(jsonPath.c_str()));
    source = j.value("source", std::string("analyser"));
    fps    = j.value("fps", 0.0f);
    if (const auto r = j.find("res"); r != j.end() && r->is_array() && r->size() >= 2) {
        srcW = (*r)[0].get<int>();
        srcH = (*r)[1].get<int>();
    }
    fovDeg  = j.value("fov_deg", 80.0f);
    focalPx = j.value("focal_px", 0.0f);
    note    = j.value("note", std::string());

    if (const auto q = j.find("quality"); q != j.end() && q->is_object()) {
        recoilClass          = q->value("recoil_class", std::string("unknown"));
        minConf              = q->value("min_conf", 0.0f);
        meanConf             = q->value("mean_conf", minConf);
        shots                = q->value("shots", 0);
        shotsMatchMagazine   = q->value("shots_match_magazine", true);
    }

    // Peak-to-peak. Read from the file when the producer measured it; derived
    // from the knots otherwise, so a hand-written profile still works.
    if (const auto pk = j.find("peak_deg"); pk != j.end() && pk->is_object()) {
        dxPeak = pk->value("x", 0.0f);
        dyPeak = pk->value("y", 0.0f);
    }

    knots.clear();
    if (const auto kn = j.find("knots"); kn != j.end() && kn->is_array()) {
        knots.reserve(kn->size());
        for (const auto& e : *kn) {
            Knot k;
            if (readKnot(e, k)) knots.push_back(k);
        }
    }
    marks.clear();
    if (const auto mk = j.find("marks"); mk != j.end() && mk->is_array()) {
        marks.reserve(mk->size());
        for (const auto& e : *mk) {
            if (!e.is_object() || !e.contains("t")) continue;
            ShotMark m;
            m.n   = e.value("n", static_cast<int>(marks.size()) + 1);
            m.tMs = e.value("t", 0.0f);
            marks.push_back(m);
        }
    }

    if (knots.empty()) {
        error = "no knots";
        return false;
    }
    std::sort(knots.begin(), knots.end(),
              [](const Knot& a, const Knot& b) { return a.tMs < b.tMs; });

    if (shots <= 0) shots = static_cast<int>(marks.size());

    if (dxPeak <= 0.0f || dyPeak <= 0.0f) {
        float mnx = 0.0f, mxx = 0.0f, mny = 0.0f, mxy = 0.0f;
        for (const Knot& k : knots) {
            mnx = std::min(mnx, k.dx); mxx = std::max(mxx, k.dx);
            mny = std::min(mny, k.dy); mxy = std::max(mxy, k.dy);
        }
        if (dxPeak <= 0.0f) dxPeak = mxx - mnx;
        if (dyPeak <= 0.0f) dyPeak = mxy - mny;
    }

    loaded = true;
    return true;
}

bool Profile::save(const std::string& jsonPath) const {
    json j;
    j["schema"]    = schema;
    j["name"]      = name;
    j["source"]    = source;
    j["fps"]       = fps;
    j["res"]       = json::array({srcW, srcH});
    j["fov_deg"]   = fovDeg;
    j["focal_px"]  = focalPx;
    if (!note.empty()) j["note"] = note;

    j["quality"] = json{
        {"recoil_class", recoilClass},
        {"min_conf", minConf},
        {"mean_conf", meanConf},
        {"shots", shots},
        {"shots_match_magazine", shotsMatchMagazine},
    };
    j["peak_deg"] = json{{"x", dxPeak}, {"y", dyPeak}};

    json kn = json::array();
    for (const Knot& k : knots) kn.push_back(knotJson(k));
    j["knots"] = std::move(kn);

    json mk = json::array();
    for (const ShotMark& m : marks) mk.push_back(json{{"n", m.n}, {"t", m.tMs}});
    j["marks"] = std::move(mk);

    ensureProfileDir();
    const bool ok = writeFile(jsonPath, j.dump(1));
    if (!ok) LOGW("cannot write %s", jsonPath.c_str());
    return ok;
}

bool Profile::loadTune(const std::string& baseStem) {
    std::string data;
    if (!readFile(tunePathOf(baseStem), data)) {
        return false;   // absent is normal
    }
    const json j = json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LOGW("tune overlay for %s is malformed; ignored", baseStem.c_str());
        return false;
    }
    auto pull = [&](const char* key, std::vector<float>& out) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_array()) return;
        out.clear();
        out.reserve(it->size());
        for (const auto& v : *it) out.push_back(v.is_number() ? v.get<float>() : 0.0f);
    };
    pull("tune_x", tuneX);
    pull("tune_y", tuneY);
    return true;
}

bool Profile::saveTune(const std::string& baseStem) const {
    bool any = false;
    auto anyOf = [&](const std::vector<float>& v) {
        for (float x : v) {
            if (std::fabs(x) > 1e-4f) { any = true; return; }
        }
    };
    anyOf(tuneX);
    anyOf(tuneY);

    const std::string path = tunePathOf(baseStem);

    if (!any) {
        // An all-zero overlay and no overlay are the same state; deleting keeps
        // the directory honest about what has actually been hand-edited.
        std::remove(path.c_str());
        return true;
    }

    json j;
    j["schema"] = 1;
    j["tune_x"] = tuneX;
    j["tune_y"] = tuneY;
    ensureProfileDir();
    return writeFile(path, j.dump(1));
}

// ── Directory ───────────────────────────────────────────────────────────────

void ensureProfileDir() {
    ::mkdir(kProfileDir, 0777);   // failure here surfaces as fopen's
}

bool validStem(const std::string& stem) {
    if (stem.empty() || stem.size() > 64) return false;
    if (stem == "." || stem == "..") return false;
    for (char c : stem) {
        if (c == '/' || c == '\\' || c == '\0') return false;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20) return false;
    }
    return true;
}

std::string profilePath(const std::string& stem) {
    return std::string(kProfileDir) + "/" + stem + kProfileExt;
}

std::vector<Profile> listProfiles() {
    std::vector<Profile> out;
    DIR* d = ::opendir(kProfileDir);
    if (d == nullptr) {
        LOGI("profiles: %s does not exist yet", kProfileDir);
        return out;
    }
    while (struct dirent* e = ::readdir(d)) {
        const std::string fn = e->d_name;
        if (!endsWith(fn, kProfileExt)) continue;
        if (endsWith(fn, kTuneExt)) continue;   // the overlay is not a profile
        const std::string stem = stemOf(fn.c_str());
        if (!validStem(stem)) continue;
        Profile p;
        if (p.load(profilePath(stem))) {
            p.loadTune(stem);
            out.push_back(std::move(p));
        } else {
            LOGW("profile %s unreadable: %s", fn.c_str(), p.error.c_str());
        }
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const Profile& a, const Profile& b) {
        return a.name < b.name;
    });
    return out;
}

// ── Tune ────────────────────────────────────────────────────────────────────

void applyTune(const Profile& p, std::vector<float>& outX, std::vector<float>& outY) {
    const size_t n = p.knots.size();
    outX.assign(n, 0.0f);
    outY.assign(n, 0.0f);

    // Walk the shot marks in time order, carrying a running sum, and stamp it
    // onto every knot that falls at or after that shot. Knots before the first
    // mark get the untouched 0 (there is no burst running yet).
    float runX = 0.0f, runY = 0.0f;
    size_t next = 0;   // first knot not yet stamped

    struct Mark { float t; float ddx, ddy; };
    std::vector<Mark> seq;
    seq.reserve(p.marks.size());
    for (size_t i = 0; i < p.marks.size(); ++i) {
        Mark m;
        m.t = p.marks[i].tMs;
        m.ddx = (i < p.tuneX.size()) ? p.tuneX[i] : 0.0f;
        m.ddy = (i < p.tuneY.size()) ? p.tuneY[i] : 0.0f;
        if (m.ddx != 0.0f || m.ddy != 0.0f) seq.push_back(m);
    }
    std::sort(seq.begin(), seq.end(), [](const Mark& a, const Mark& b) { return a.t < b.t; });

    for (const Mark& m : seq) {
        while (next < n && p.knots[next].tMs < m.t) {
            outX[next] += runX;
            outY[next] += runY;
            ++next;
        }
        runX += m.ddx;
        runY += m.ddy;
    }
    while (next < n) {
        outX[next] += runX;
        outY[next] += runY;
        ++next;
    }
}

// ── Player ──────────────────────────────────────────────────────────────────

void Player::begin(const Profile& p, float scaleX, float scaleY,
                   float signX, float signY, float weightX, float weightY,
                   float budgetPx) {
    ct_.clear();
    cx_.clear();
    cy_.clear();
    mt_.clear();
    running_ = false;
    clamped_ = false;
    tMs_     = 0.0f;
    lastX_ = lastY_ = 0.0f;
    accumX_ = accumY_ = 0.0f;
    budget_ = budgetPx;
    span_   = p.spanMs();

    if (!p.loaded || p.knots.empty()) return;

    std::vector<float> tuneX, tuneY;
    applyTune(p, tuneX, tuneY);

    // The compensation, in finger px, baked once. `-` because the aim must
    // rotate against the game's rotation; sign/weight/scale are the user's.
    const float kx = -scaleX * signX * weightX;
    const float ky = -scaleY * signY * weightY;

    const size_t n = p.knots.size();
    ct_.reserve(n);
    cx_.reserve(n);
    cy_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        ct_.push_back(p.knots[i].tMs);
        cx_.push_back(kx * p.knots[i].dx + tuneX[i]);
        cy_.push_back(ky * p.knots[i].dy + tuneY[i]);
    }

    // A zero-length measurement would divide by zero in progress() and would
    // never leave the first knot. Refuse rather than drive nonsense.
    if (span_ <= 0.0f) {
        ct_.clear(); cx_.clear(); cy_.clear();
        return;
    }

    mt_.reserve(p.marks.size());
    for (const ShotMark& m : p.marks) mt_.push_back(m.tMs);
    std::sort(mt_.begin(), mt_.end());

    running_ = true;
}

void Player::stop() { running_ = false; }

float Player::sample(int axis, float tMs) const {
    const std::vector<float>& c = (axis == 0) ? cx_ : cy_;
    const size_t n = c.size();
    if (n == 0) return 0.0f;
    if (tMs <= ct_.front()) return c.front();
    if (tMs >= ct_.back()) return c.back();

    // First knot strictly after t; the segment is [i-1, i].
    const size_t i = static_cast<size_t>(
        std::upper_bound(ct_.begin(), ct_.end(), tMs) - ct_.begin());
    const float t0 = ct_[i - 1], t1 = ct_[i];
    const float span = t1 - t0;
    const float u = (span > 1e-6f) ? (tMs - t0) / span : 0.0f;
    return c[i - 1] + (c[i] - c[i - 1]) * u;
}

void Player::step(float dt, float& outDx, float& outDy) {    outDx = 0.0f;
    outDy = 0.0f;
    if (!running_) return;

    if (dt <= 0.0f) dt = 1.0f / 120.0f;
    tMs_ += dt * 1000.0f;
    const float t = (tMs_ > span_) ? span_ : tMs_;

    const float nowX = sample(0, t);
    const float nowY = sample(1, t);

    float dx = nowX - lastX_;
    float dy = nowY - lastY_;
    lastX_ = nowX;
    lastY_ = nowY;

    // Drag-budget accounting. A synthetic contact that is dragged past the
    // touch layer's lift-and-repress threshold would be re-anchored mid-burst,
    // and a re-anchor that lands anywhere other than where the finger already
    // is reads to the game as a camera jump. Stopping is the honest failure:
    // "the compensation stops helping" beats "the crosshair teleports".
    if (budget_ > 0.0f) {
        if (std::fabs(accumX_ + dx) > budget_) { dx = 0.0f; clamped_ = true; }
        if (std::fabs(accumY_ + dy) > budget_) { dy = 0.0f; clamped_ = true; }
    }

    accumX_ += dx;
    accumY_ += dy;
    outDx = dx;
    outDy = dy;

    // Reaching the end of the measured burst holds the last value; the player
    // stays armed so the caller can tell "burst still replayed" from "idle".
    // It does NOT unwind: moving the finger back would rotate the view through
    // the whole pattern again (see §7.6 of docs/recoil-compensation-plan.md).
}

int Player::shotIndex() const {
    // "Which shot are we between": the last mark whose time has passed. 0 means
    // the replay has not reached the first shot yet.
    if (mt_.empty()) return 0;
    const size_t i = static_cast<size_t>(
        std::upper_bound(mt_.begin(), mt_.end(), tMs_) - mt_.begin());
    return static_cast<int>(i);
}

float Player::progress() const {
    if (span_ <= 0.0f) return 1.0f;
    const float u = tMs_ / span_;
    return u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
}

// ── Cross-module hand-off ───────────────────────────────────────────────────

namespace {
float g_aimDx = 0.0f, g_aimDy = 0.0f;
bool  g_aimLive = false;
}  // namespace

void publishAimDelta(float dx, float dy) {
    g_aimDx = dx;
    g_aimDy = dy;
    g_aimLive = (dx != 0.0f || dy != 0.0f);
}

void aimDelta(float& dx, float& dy) {
    dx = g_aimDx;
    dy = g_aimDy;
}

bool aimDeltaLive() { return g_aimLive; }

}  // namespace recoil
}  // namespace aimbotng
