// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::crosshair — implementation. See crosshair_recoil.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "tracking/crosshair_recoil.h"

#include <android/log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "capture/capture.h"
#include "tracking/crosshair_detect.h"
#include "tracking/kalman_tracker.h"

#define TAG "AimbotRecoil"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace crosshair {

namespace {

constexpr const char* kDir = "/data/local/tmp/aimbotng/crosshair";
constexpr int64_t kSampleMs = 1500;   // colour sample window, like F8
constexpr int kCoastFrames = 6;       // keep last crosshair position this long

Params g_params;

// ── Sampling state ──────────────────────────────────────────────────────────
bool        g_sampling   = false;
bool        g_sampleOk   = false;
std::string g_sampleName;
std::string g_sampleStatus;
int64_t     g_sampleT0Ms = 0;
long long   g_sB = 0, g_sG = 0, g_sR = 0;
int         g_sFrames = 0;

// ── Configs / tracking state ────────────────────────────────────────────────
std::vector<ColorRange> g_cfgs;
std::vector<std::string> g_names;
bool g_tracking = false;

// Tracker-state for the crosshair itself: coast through short misses.
bool  g_havePos = false;
float g_px = 0.0f, g_py = 0.0f;
int   g_missed = 0;

// PID state, per axis. Reset when the burst stops.
float g_ix = 0.0f, g_iy = 0.0f;
float g_dvx = 0.0f, g_dvy = 0.0f;
float g_pex = 0.0f, g_pey = 0.0f;
bool  g_pidFresh = false;
float g_accX = 0.0f, g_accY = 0.0f;

// Last detection, for the preview readout.
int   g_lastHits = 0;
float g_lastScore = 0.0f;
int   g_lastCfg = -1;

// Frame consumer state.
std::vector<uint8_t> g_frame;
int g_frameW = 0, g_frameH = 0;
uint64_t g_lastId = 0;

int64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool validStem(const std::string& s) {
    if (s.empty() || s.size() > 48) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
              (static_cast<unsigned char>(c) >= 0x80)))
            return false;
    }
    return true;
}

bool grabFrame() {
    int w = 0, h = 0;
    if (!capture::peekSize(w, h) || w <= 0 || h <= 0) return false;
    const size_t bytes = (size_t)w * h * 4;
    if (g_frame.size() != bytes) g_frame.resize(bytes);
    int fw = 0, fh = 0;
    if (!capture::copyLatestInto(g_frame.data(), g_frame.size(), fw, fh, g_lastId))
        return false;
    g_frameW = fw;
    g_frameH = fh;
    return true;
}

/// One frame of the colour sample: pick the most saturated pixel within 20 px
/// of the frame centre whose colour departs from the scene's background mean
/// (the reference sampler's scheme), and accumulate it.
void pumpSampleFrame() {
    const int w = g_frameW, h = g_frameH;
    const int cx = w / 2, cy = h / 2;

    // Background mean, skipping the inner 60 px disc, sampled every 3 px.
    long long bB = 0, bG = 0, bR = 0, bN = 0;
    const int bgInner = 60;
    for (int y = 0; y < h; y += 3) {
        const int dy = y - cy;
        for (int x = 0; x < w; x += 3) {
            const int dx = x - cx;
            if (dx * dx + dy * dy < bgInner * bgInner) continue;
            const uint8_t* p = g_frame.data() + ((size_t)y * w + x) * 4;
            bB += p[2]; bG += p[1]; bR += p[0]; ++bN;
        }
    }
    if (bN < 100) return;
    const int mbB = (int)(bB / bN), mbG = (int)(bG / bN), mbR = (int)(bR / bN);

    // Best candidate: biggest colour departure with some saturation, scored
    // like the reference (S*2 + V).
    int best = -1, bB2 = 0, bG2 = 0, bR2 = 0;
    const int winR = 20;
    const int d2Gate = 60 * 60;
    for (int dy = -winR; dy <= winR; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= h) continue;
        for (int dx = -winR; dx <= winR; ++dx) {
            const int x = cx + dx;
            if (x < 0 || x >= w) continue;
            const uint8_t* p = g_frame.data() + ((size_t)y * w + x) * 4;
            const int ddB = p[2] - mbB, ddG = p[1] - mbG, ddR = p[0] - mbR;
            if (ddB * ddB + ddG * ddG + ddR * ddR < d2Gate) continue;
            int H, S, V;
            rgb2hsv(p[0], p[1], p[2], H, S, V);
            if (S < 50) continue;
            const int sc = S * 2 + V;
            if (sc > best) { best = sc; bB2 = p[2]; bG2 = p[1]; bR2 = p[0]; }
        }
    }
    if (best > 0) { g_sB += bB2; g_sG += bG2; g_sR += bR2; ++g_sFrames; }
}

void finishSample() {
    g_sampling = false;
    if (g_sFrames == 0) {
        g_sampleStatus = "采样失败：画面中心没有可识别的彩色像素";
        g_sampleOk = false;
        LOGW("crosshair: sample failed, no usable frames");
        return;
    }
    const int B = (int)(g_sB / g_sFrames);
    const int G = (int)(g_sG / g_sFrames);
    const int R = (int)(g_sR / g_sFrames);
    int H, S, V;
    rgb2hsv(R, G, B, H, S, V);
    if (S < 30) {
        g_sampleStatus = "采样被拒绝：白色/灰色准星与场景高光冲突，请换有色准星";
        g_sampleOk = false;
        LOGW("crosshair: sample rejected, grey/white (S=%d)", S);
        return;
    }

    ColorRange cfg;
    rangeFromSample(H, S, V, cfg);

    // Measure how many pixels the crosshair itself occupies, so runtime size
    // filtering (0.3x..2x expectPix) can reject same-colour background blobs.
    // A single strict pass reads 0 whenever the crosshair's shading sits a
    // step off the sampled mean (bright core, anti-aliased rim), so probe
    // with widening tolerance — and if even that finds nothing, save the
    // config as unsized (expectPix=0) instead of throwing the sample away.
    bool measured = false;
    // The sampling pump just consumed the newest frame, so a fresh grab here
    // usually finds nothing newer — probe on the in-memory frame instead of
    // skipping the whole measurement (which is what zeroed expectPix every
    // time and made every sample "unsized").
    if (g_frameW > 0 || grabFrame()) {
        // Full-frame probe: the frame is only 256x256, so a whole-frame scan
        // is cheap and removes any assumption about where the crosshair sits.
        for (int attempt = 0; attempt < 3; ++attempt) {
            ColorRange probe_cfg;
            rangeFromSample(H, S, V, probe_cfg,
                            /*hTol=*/12 + attempt * 12, /*svSlack=*/60 + attempt * 40);
            Result probe = byColor(g_frame.data(), g_frameW, g_frameH,
                                   g_frameW * 4, probe_cfg, -1, -1,
                                   std::max(g_frameW, g_frameH));
            if (probe.found) { cfg.expectPix = probe.hits; measured = true; break; }
        }
        // Diagnostic dump: the exact frame the probe ran on, so a "0 px" can
        // be settled by looking at the pixels instead of guessing.
        FILE* dump = std::fopen("/data/local/tmp/aimbotng/crosshair_last_frame.raw", "wb");
        if (dump != nullptr) {
            std::fwrite(g_frame.data(), 1, g_frame.size(), dump);
            std::fclose(dump);
        }
        LOGI("crosshair: probe %s (hits=%d) on %dx%d frame",
             measured ? "measured" : "found nothing", cfg.expectPix, g_frameW, g_frameH);
    } else {
        LOGW("crosshair: no frame available for probe");
    }
    if (!measured) cfg.expectPix = 0;

    ::mkdir(kDir, 0777);
    const std::string path = std::string(kDir) + "/" + g_sampleName + ".dat";
    char line[128];
    std::snprintf(line, sizeof(line), "%d %d %d %d %d %d %d %d %d\n",
                  cfg.hLo, cfg.hHi, cfg.sLo, cfg.sHi, cfg.vLo, cfg.vHi,
                  cfg.radius, cfg.minPix, cfg.expectPix);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        g_sampleStatus = "写入配置失败";
        g_sampleOk = false;
        return;
    }
    std::fwrite(line, 1, std::strlen(line), f);
    std::fclose(f);

    char buf[192];
    if (cfg.expectPix > 0) {
        std::snprintf(buf, sizeof(buf),
                      "采样完成：HSV(%d,%d,%d) 命中 %d px，共 %d 帧",
                      H, S, V, cfg.expectPix, g_sFrames);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "采样完成：HSV(%d,%d,%d)，未测得像素——已按无尺寸配置保存（准星若在画面中心 30px 外，把它挪回中间再追踪）",
                      H, S, V, g_sFrames);
    }
    g_sampleStatus = buf;
    g_sampleOk = true;
    LOGI("crosshair: sampled '%s' HSV(%d,%d,%d) expectPix=%d frames=%d",
         g_sampleName.c_str(), H, S, V, cfg.expectPix, g_sFrames);
    reloadConfigs();
}

void resetPid() {
    g_ix = g_iy = 0.0f;
    g_dvx = g_dvy = 0.0f;
    g_pex = g_pey = 0.0f;
    g_pidFresh = false;
    g_accX = g_accY = 0.0f;
}

/// Configs are scanned once, lazily — the daemon starts long before the user
/// has anything sampled, and rescanning per frame would be waste.
bool g_loaded = false;
void ensureLoaded() {
    if (g_loaded) return;
    g_loaded = true;
    reloadConfigs();
}

}  // namespace

// ── Params ──────────────────────────────────────────────────────────────────

void setParams(const Params& p) { g_params = p; }
Params params() { return g_params; }

// ── Sampling ────────────────────────────────────────────────────────────────

bool startSample(const std::string& name, std::string& err) {
    if (g_sampling) { err = "已在采样中"; return false; }
    if (!validStem(name)) { err = "配置名只能有字母数字下划线中文，且不能为空"; return false; }
    g_sampleName = name;
    g_sB = g_sG = g_sR = 0;
    g_sFrames = 0;
    g_sampleT0Ms = nowMs();
    g_sampling = true;
    g_sampleOk = false;
    g_sampleStatus = "采样中…对准准星保持 1.5 秒";
    LOGI("crosshair: sampling '%s'", name.c_str());
    return true;
}

void abortSample() {
    if (!g_sampling) return;
    g_sampling = false;
    g_sampleStatus = "采样已取消";
    g_sampleOk = false;
}

bool sampling() { return g_sampling; }
float sampleProgress() {
    if (!g_sampling) return 0.0f;
    const float t = (float)(nowMs() - g_sampleT0Ms) / (float)kSampleMs;
    return std::clamp(t, 0.0f, 1.0f);
}
const std::string& sampleStatus() { return g_sampleStatus; }
bool sampleOk() { return g_sampleOk; }

/// Called every UI tick while sampling: pulls a frame and accumulates.
void pumpSampling() {
    if (!g_sampling) return;
    ensureLoaded();
    if (grabFrame()) pumpSampleFrame();
    if (nowMs() - g_sampleT0Ms >= kSampleMs) finishSample();
}

// ── Configs ─────────────────────────────────────────────────────────────────

int reloadConfigs() {
    g_cfgs.clear();
    g_names.clear();
    DIR* d = ::opendir(kDir);
    if (d == nullptr) return 0;
    while (struct dirent* e = ::readdir(d)) {
        const std::string fn = e->d_name;
        if (fn.size() < 5 || fn.substr(fn.size() - 4) != ".dat") continue;
        std::string body;
        if (!readFile(std::string(kDir) + "/" + fn, body)) continue;
        ColorRange c;
        // v2: hLo hHi sLo sHi vLo vHi radius minPix expectPix
        if (std::sscanf(body.c_str(), "%d %d %d %d %d %d %d %d %d",
                        &c.hLo, &c.hHi, &c.sLo, &c.sHi, &c.vLo, &c.vHi,
                        &c.radius, &c.minPix, &c.expectPix) != 9) continue;
        // expectPix==0 is legal: an unsized config runs without the size
        // filter (weaker background rejection) instead of being discarded —
        // discarding it left the user with a "successful" sample that could
        // never track anything.
        if (c.radius <= 0 || c.minPix <= 0) continue;
        g_cfgs.push_back(c);
        g_names.push_back(fn.substr(0, fn.size() - 4));
    }
    ::closedir(d);
    LOGI("crosshair: %zu config(s) loaded", g_cfgs.size());
    return (int)g_cfgs.size();
}

int configCount() { return (int)g_cfgs.size(); }
std::vector<std::string> configNames() { return g_names; }

bool deleteConfig(const std::string& name) {
    if (!validStem(name)) return false;
    const std::string path = std::string(kDir) + "/" + name + ".dat";
    if (std::remove(path.c_str()) != 0) return false;
    reloadConfigs();
    return true;
}

// ── Tracking / control ──────────────────────────────────────────────────────

void setTracking(bool on) {
    if (g_tracking == on) return;
    g_tracking = on;
    g_havePos = false;
    g_missed = 0;
    resetPid();
    LOGI("crosshair: tracking %s", on ? "on" : "off");
}
bool tracking() { return g_tracking; }

bool tick(float dt, bool firing, float& accX, float& accY, float& dx, float& dy) {
    accX = accY = 0.0f;
    dx = dy = 0.0f;

    ensureLoaded();
    pumpSampling();

    if (!g_tracking) { resetPid(); return false; }

    // ── 1. locate the crosshair on the newest frame (whole frame) ───────────
    // Detection runs at half the UI rate (~30 Hz): the crosshair moves a few
    // px per frame at most, and the whole-frame scan ×2 recognisers was the
    // main source of the daemon's heat.
    static int s_detectSkip = 0;
    const bool detectFrame = (g_tracking && (s_detectSkip++ & 1) == 0);
    if (detectFrame && grabFrame() && !g_cfgs.empty()) {
        Result r = multi(g_frame.data(), g_frameW, g_frameH,
                         g_frameW * 4, g_cfgs, -1, -1,
                         std::max(g_frameW, g_frameH));
        // Firing floods the frame with red-family interference (muzzle flash,
        // damage markers). The shape-scored recogniser correctly rejects those
        // frames — its size/shape gates are exactly what a blob of flash
        // pixels fails — but that reads as "crosshair lost" every shot. Fall
        // back to the plain HSV centroid, but with a size gate: the fallback
        // has no shape opinion, so without it any brown dirt (same hue family
        // as a red crosshair) is accepted wholesale. The crosshair's own
        // sampled pixel count is the yardstick.
        if (!r.found) {
            for (size_t i = 0; i < g_cfgs.size() && !r.found; ++i) {
                Result c = byColor(g_frame.data(), g_frameW, g_frameH,
                                   g_frameW * 4, g_cfgs[i], -1, -1,
                                   std::max(g_frameW, g_frameH));
                if (!c.found) continue;
                if (g_cfgs[i].expectPix > 0 &&
                    (c.hits < g_cfgs[i].expectPix * 2 / 5 ||
                     c.hits > g_cfgs[i].expectPix * 3)) continue;
                r = c; r.cfg = (int)i; r.score = 0.5f;
            }
        }
        g_lastHits = r.hits;
        g_lastScore = r.score;
        g_lastCfg = r.cfg;
        if (r.found) {
            if (!g_havePos) {
                LOGI("crosshair: locked at (%.0f,%.0f) hits=%d cfg=%d",
                     r.x, r.y, r.hits, r.cfg);
            }
            g_havePos = true;
            g_missed = 0;
            g_px = r.x;
            g_py = r.y;
        } else if (g_havePos && g_missed < kCoastFrames) {
            ++g_missed;   // coast on the last position
        } else {
            g_havePos = false;
            resetPid();
        }
    }

    // ── 2. burst bookkeeping ────────────────────────────────────────────────
    if (!firing) {
        resetPid();
        return false;
    }

    // Goal: the aim's target box centre; without a target there is nothing to
    // hold the crosshair on, and the mode is documented as needing the aim.
    const auto& tracks = tracking::tracker().tracks();
    const tracking::TrackedTarget* tgt = nullptr;
    for (const auto& t : tracks) {
        if (t.confirmed && !t.lost) { tgt = &t; break; }
    }
    if (tgt == nullptr || !g_havePos) return false;

    int ox = 0, oy = 0;
    capture::cropOrigin(ox, oy);
    const float cxs = (float)ox + g_px;
    const float cys = (float)oy + g_py;
    const float gx = tgt->cx;
    const float gy = tgt->cy;
    const float ex = gx - cxs;
    const float ey = gy - cys;

    // ── 3. PID (per axis), output = finger px/s → this frame's increment ────
    const float kp = g_params.kp, ki = g_params.ki, kd = g_params.kd;
    const float px = ex * kp;
    const float py = ey * kp;
    if (g_pidFresh) {
        // Derivative on the error with a light low-pass: the raw per-frame
        // difference is mostly capture noise.
        const float a = std::clamp(dt / (dt + 0.05f), 0.0f, 1.0f);
        const float dvx = (ex - g_pex) / std::max(1e-4f, dt);
        const float dvy = (ey - g_pey) / std::max(1e-4f, dt);
        g_dvx = g_dvx + (dvx - g_dvx) * a;
        g_dvy = g_dvy + (dvy - g_dvy) * a;
    } else {
        g_dvx = g_dvy = 0.0f;
        g_pidFresh = true;
    }
    g_pex = ex;
    g_pey = ey;
    // Integrator clamp: with the gain in front, ±80 screen px of held error is
    // already a full-screen pull; anything larger is wind-up, not control.
    g_ix = std::clamp(g_ix + ex * dt, -80.0f, 80.0f);
    g_iy = std::clamp(g_iy + ey * dt, -80.0f, 80.0f);

    const float ux = (px + ki * g_ix + kd * g_dvx) * g_params.gain;
    const float uy = (py + ki * g_iy + kd * g_dvy) * g_params.gain;
    dx = ux * dt;
    dy = uy * dt;

    // Rate cap: the finger cannot teleport, and an uncapped step reads as a
    // flick. 600 finger px/s ≈ a fast but human swipe.
    constexpr float kMaxStepPerSec = 600.0f;
    const float mag = std::sqrt(dx * dx + dy * dy);
    const float cap = kMaxStepPerSec * dt;
    if (mag > cap && mag > 1e-4f) {
        dx *= cap / mag;
        dy *= cap / mag;
    }

    g_accX += dx;
    g_accY += dy;
    accX = g_accX;
    accY = g_accY;
    return true;
}

bool wantsFrames() { return g_sampling || g_tracking; }

bool preview(Preview& out) {
    // Screen coordinates, not frame coordinates: the preview panel maps
    // against the capture square's screen rect, so raw frame coords (0..255)
    // landed outside the panel and the green cross never showed.
    int ox = 0, oy = 0;
    capture::cropOrigin(ox, oy);
    out.found = g_havePos;
    out.fx = (float)ox + g_px; out.fy = (float)oy + g_py;
    out.fw = g_frameW; out.fh = g_frameH;
    out.hits = g_lastHits;
    out.score = g_lastScore;
    out.cfg = g_lastCfg;
    out.nCfgs = (int)g_cfgs.size();
    const auto& tracks = tracking::tracker().tracks();
    for (const auto& t : tracks) {
        if (t.confirmed && !t.lost) {
            out.haveTarget = true;
            out.tx = t.cx - t.w * 0.5f; out.ty = t.cy - t.h * 0.5f;
            out.tw = t.w;               out.th = t.h;
            break;
        }
    }
    return true;
}

}  // namespace crosshair
}  // namespace aimbotng
