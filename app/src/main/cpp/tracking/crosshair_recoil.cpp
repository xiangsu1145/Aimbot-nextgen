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
#include "ui/gui/notify.h"
#include "ui/gui/sections/recoil_section.h"

#define TAG "AimbotRecoil"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace crosshair {

namespace {

constexpr const char* kDir = "/data/local/tmp/aimbotng/crosshair";
constexpr int64_t kSampleMs = 1500;   // colour sample window, like F8
constexpr int kCoastFrames = 6;       // keep last crosshair position this long

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
        ui::notify::error("crosshair.sample", g_sampleStatus.c_str(), 5.0f);
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
        ui::notify::error("crosshair.sample", g_sampleStatus.c_str(), 5.0f);
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
        const int midX = g_frameW / 2, midY = g_frameH / 2;
        for (int attempt = 0; attempt < 3; ++attempt) {
            ColorRange probe_cfg;
            rangeFromSample(H, S, V, probe_cfg,
                            /*hTol=*/6 + attempt * 2, /*svSlack=*/60 + attempt * 20);
            Result probe = byFeature(g_frame.data(), g_frameW, g_frameH,
                                     g_frameW * 4, probe_cfg, midX, midY,
                                     /*radiusCover=*/40);
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
        ui::notify::error("crosshair.sample", g_sampleStatus.c_str(), 5.0f);
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
                      "采样完成：HSV(%d,%d,%d)，共 %d 帧，未测得像素——已按无尺寸配置保存（准星若在画面中心 30px 外，把它挪回中间再追踪）",
                      H, S, V, g_sFrames);
    }
    g_sampleStatus = buf;
    g_sampleOk = true;
    ui::notify::success("crosshair.sample", g_sampleStatus.c_str(), 5.0f);
    LOGI("crosshair: sampled '%s' HSV(%d,%d,%d) expectPix=%d frames=%d",
         g_sampleName.c_str(), H, S, V, cfg.expectPix, g_sFrames);
    reloadConfigs();
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
    ui::notify::info("crosshair.sample", "已取消准星采样", 3.0f);
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
        // 红色准星保护（防止混入黄色导致飞出）及白墙兼容：
        // 若为红色（跨越 0/180，或纯红 0~15 或 165~179）：
        // 1. 严格收缩上界至 <= 11，彻底杜绝混入黄色（黄色起始为 H >= 23）。
        // 2. 下界拉至 >= 169。
        // 3. 放宽 sLo 至 35，兼容白墙场景下的半透明/抗锯齿边缘（白墙本身 S < 15）。
        if (c.hLo > c.hHi) {
            if (c.hHi > 11) c.hHi = 11;
            if (c.hLo < 169) c.hLo = 169;
            if (c.sLo > 35) c.sLo = 35;
        } else if (c.hHi <= 15) {
            if (c.hHi > 11) c.hHi = 11;
            if (c.sLo > 35) c.sLo = 35;
        } else if (c.hLo >= 165) {
            if (c.hLo < 169) c.hLo = 169;
            if (c.sLo > 35) c.sLo = 35;
        } else if (c.hLo != 0 || c.hHi != 179) {
            // 其他彩色准星：白墙兼容放宽
            if (c.sLo > 45) c.sLo = 45;
        }
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
    LOGI("crosshair: tracking %s", on ? "on" : "off");
}
bool tracking() { return g_tracking; }

void resetTracking() {
    g_havePos = false;
    g_missed = 0;
}

bool getCrosshairPosition(float& screenX, float& screenY) {
    const auto& pg = ui::sections::g_pageRecoil;
    if (!pg.enabled.value ||
        pg.mode.value != ui::sections::PageRecoil::kModeCrosshair ||
        !g_tracking || !g_havePos) {
        return false;
    }
    int ox = 0, oy = 0;
    capture::cropOrigin(ox, oy);
    screenX = (float)ox + g_px;
    screenY = (float)oy + g_py;
    return true;
}

void pumpPreview() {
    ensureLoaded();
    if (g_cfgs.empty()) return;

    // Throttle preview detection to 30 Hz to save compute and battery
    static int64_t s_lastPreviewMs = 0;
    const int64_t tNow = nowMs();
    if (tNow - s_lastPreviewMs < 30) return;
    s_lastPreviewMs = tNow;

    if (!grabFrame()) return;

    const auto& pg = ui::sections::g_pageRecoil;
    const int pick = pg.xPick.value;
    std::vector<ColorRange> activeCfgs;
    if (pick >= 0 && pick < static_cast<int>(g_cfgs.size())) {
        activeCfgs.push_back(g_cfgs[pick]);
    } else {
        activeCfgs = g_cfgs;
    }
    if (activeCfgs.empty()) return;

    int cx = g_frameW / 2;
    int cy = g_frameH / 2;
    int searchRad = 50;
    if (g_havePos) {
        cx = std::clamp((int)std::round(g_px), 0, g_frameW - 1);
        cy = std::clamp((int)std::round(g_py), 0, g_frameH - 1);
        searchRad = 36;
    }

    Result r = multi(g_frame.data(), g_frameW, g_frameH,
                     g_frameW * 4, activeCfgs, cx, cy, searchRad);
    if (!r.found && g_havePos) {
        r = multi(g_frame.data(), g_frameW, g_frameH,
                  g_frameW * 4, activeCfgs, g_frameW / 2, g_frameH / 2, 54);
    }

    bool acceptCandidate = false;
    if (r.found) {
        const float dxMid = r.x - g_frameW * 0.5f;
        const float dyMid = r.y - g_frameH * 0.5f;
        if (std::sqrt(dxMid * dxMid + dyMid * dyMid) <= 65.0f) {
            if (g_havePos) {
                const float dxPrev = r.x - g_px;
                const float dyPrev = r.y - g_py;
                if (std::sqrt(dxPrev * dxPrev + dyPrev * dyPrev) <= 25.0f) {
                    acceptCandidate = true;
                }
            } else {
                acceptCandidate = true;
            }
        }
    }

    g_lastHits = r.hits;
    g_lastScore = r.score;
    g_lastCfg = (pick >= 0 && pick < static_cast<int>(g_cfgs.size())) ? pick : r.cfg;
    if (acceptCandidate) {
        if (g_havePos) {
            g_px = g_px * 0.35f + r.x * 0.65f;
            g_py = g_py * 0.35f + r.y * 0.65f;
        } else {
            g_px = r.x;
            g_py = r.y;
        }
        g_havePos = true;
        g_missed = 0;
    } else if (g_havePos && g_missed < kCoastFrames) {
        ++g_missed;
    } else {
        g_havePos = false;
    }
}

void pumpTracking(bool enabled) {
    ensureLoaded();
    pumpSampling();

    const auto& pg = ui::sections::g_pageRecoil;
    const bool isTriggering = pg.continuousTrigger.value || ui::sections::recoilIsFiring();
    const bool active = enabled && pg.enabled.value &&
                        pg.mode.value == ui::sections::PageRecoil::kModeCrosshair &&
                        g_tracking && isTriggering;
    if (!active) {
        if (g_havePos) {
            g_havePos = false;
            g_missed = 0;
        }
        return;
    }

    // ── Throttled detection: ~30 Hz (every 30ms) to reduce CPU compute, power, and heat ──
    static int64_t s_lastDetectMs = 0;
    const int64_t tNow = nowMs();
    if (tNow - s_lastDetectMs < 30) {
        return;
    }
    s_lastDetectMs = tNow;

    if (!grabFrame() || g_cfgs.empty()) return;

    // ── 1. 优先使用用户在下拉框选定的当前准星配置，避免匹配到其他不相关准星 ──
    const int pick = pg.xPick.value;
    std::vector<ColorRange> activeCfgs;
    if (pick >= 0 && pick < static_cast<int>(g_cfgs.size())) {
        activeCfgs.push_back(g_cfgs[pick]);
    } else {
        activeCfgs = g_cfgs;
    }
    if (activeCfgs.empty()) return;

    // ── 2. 自适应 ROI 窗口 ──
    int cx = g_frameW / 2;
    int cy = g_frameH / 2;
    int searchRad = 50;
    if (g_havePos) {
        cx = std::clamp((int)std::round(g_px), 0, g_frameW - 1);
        cy = std::clamp((int)std::round(g_py), 0, g_frameH - 1);
        searchRad = 36;
    }

    Result r = multi(g_frame.data(), g_frameW, g_frameH,
                     g_frameW * 4, activeCfgs, cx, cy, searchRad);

    // 丢失后若连续 miss，尝试扩大至中心范围重新抓取
    if (!r.found && g_havePos && g_missed >= 2) {
        r = multi(g_frame.data(), g_frameW, g_frameH,
                  g_frameW * 4, activeCfgs, g_frameW / 2, g_frameH / 2, 54);
    }

    // ── 3. 严格防飞保护 (Jump / Teleportation Protection) ──
    // 物理枪械后坐力在 30ms 内准星位移极小 (通常 <= 15px)。
    // 凡是突变位移 > 24px，或者漂移出画面中心 65px 之外的孤立亮斑，直接判定为误检（火花/UI/敌人标志），拒绝锁跳！
    bool acceptCandidate = false;
    if (r.found) {
        const float dxMid = r.x - g_frameW * 0.5f;
        const float dyMid = r.y - g_frameH * 0.5f;
        const float distMid = std::sqrt(dxMid * dxMid + dyMid * dyMid);
        if (distMid <= 65.0f) {
            if (g_havePos) {
                const float dxPrev = r.x - g_px;
                const float dyPrev = r.y - g_py;
                const float jumpDist = std::sqrt(dxPrev * dxPrev + dyPrev * dyPrev);
                if (jumpDist <= 24.0f) {
                    acceptCandidate = true;
                }
            } else {
                acceptCandidate = true;
            }
        }
    }

    g_lastHits = r.hits;
    g_lastScore = r.score;
    g_lastCfg = (pick >= 0 && pick < static_cast<int>(g_cfgs.size())) ? pick : r.cfg;
    if (acceptCandidate) {
        if (!g_havePos) {
            LOGI("crosshair: locked at (%.1f,%.1f) hits=%d cfg=%d",
                 r.x, r.y, r.hits, r.cfg);
            g_px = r.x;
            g_py = r.y;
        } else {
            // 指数平滑：平滑防抖，彻底消除准星瞬间抖动和跳变
            g_px = g_px * 0.35f + r.x * 0.65f;
            g_py = g_py * 0.35f + r.y * 0.65f;
        }
        g_havePos = true;
        g_missed = 0;
    } else if (g_havePos && g_missed < kCoastFrames) {
        ++g_missed;   // coast on the last position
    } else {
        g_havePos = false;
    }
}

bool wantsFrames() {
    if (g_sampling) return true;
    const auto& pg = ui::sections::g_pageRecoil;
    const bool isTriggering = pg.continuousTrigger.value || ui::sections::recoilIsFiring();
    return pg.enabled.value &&
           pg.mode.value == ui::sections::PageRecoil::kModeCrosshair &&
           g_tracking && isTriggering;
}

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
