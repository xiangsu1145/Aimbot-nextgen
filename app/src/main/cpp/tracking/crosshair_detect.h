// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::crosshair — crosshair recognition (准星识别)
//
//  Port of the user-supplied 准星识别.h (Windows, BGR) to this daemon:
//  RGBA input from the capture pipeline, no Windows.h, no elastic storage —
//  config files are plain text handled by crosshair_recoil.cpp.
//
//  Two recognisers:
//    byColor   — pure HSV centroid, fastest, clean backgrounds
//    byFeature — HSV + connected components + multi-factor scoring,
//                robust to same-colour backgrounds, handles dot / cross /
//                dot+ring shapes
//  Both search a small window around an expected centre, which is what keeps
//  them cheap enough to run every frame.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <utility>
#include <algorithm>

namespace aimbotng {
namespace crosshair {

// OpenCV-convention HSV: H in [0,180), S/V in [0,255].
inline void rgb2hsv(int r, int g, int b, int& H, int& S, int& V) {
    const int M = std::max(r, std::max(g, b));
    const int m = std::min(r, std::min(g, b));
    V = M;
    const int delta = M - m;
    if (M == 0) { S = 0; H = 0; return; }
    S = (255 * delta) / M;
    if (delta == 0) { H = 0; return; }
    int hue;
    if (M == r)      hue = 30 * (g - b) / delta;
    else if (M == g) hue = 60 + 30 * (b - r) / delta;
    else             hue = 120 + 30 * (r - g) / delta;
    if (hue < 0) hue += 180;
    H = hue;
}

// One sampled colour range. Default (empty S/V range) matches nothing, so an
// unsampled / unloaded config can never fire the detector by accident.
struct ColorRange {
    int hLo = 0, hHi = 0;
    int sLo = 255, sHi = 0;
    int vLo = 255, vHi = 0;
    int radius   = 30;   // search radius around the expected centre, px
    int minPix   = 4;    // fewest matching pixels accepted
    int expectPix = 0;   // pixel count measured at sample time; 0 = invalid
};

struct Result {
    bool  found = false;
    float x = 0.0f, y = 0.0f;   // absolute frame coordinates on failure = expected centre
    int   hits = 0;
    float score = 0.0f;         // [0,1]
    int   cfg   = -1;           // which config matched (multi-config)
};

// H wraps at 0/180 (red spans the seam) — a "wrapped" range has hLo > hHi.
inline bool hInRange(int h, int lo, int hi) {
    if (lo <= hi) return h >= lo && h <= hi;
    return h >= lo || h <= hi;
}

inline bool matches(const ColorRange& c, int H, int S, int V) {
    return hInRange(H, c.hLo, c.hHi) &&
           S >= c.sLo && S <= c.sHi &&
           V >= c.vLo && V <= c.vHi;
}

// Builds a range from one sampled HSV, mirrors 配置由采样设置().
inline void rangeFromSample(int H, int S, int V, ColorRange& c,
                            int hTol = 7, int svSlack = 60) {
    if (S < 30) {
        // Grey/white crosshair
        c.hLo = 0; c.hHi = 179;
        c.sLo = 0; c.sHi = std::min(255, S + svSlack);
    } else {
        const bool isRed = (H <= 12 || H >= 168);
        const int actualHTol = isRed ? 6 : std::min(hTol, 7);
        int lo = H - actualHTol, hi = H + actualHTol;
        if (lo < 0)       { c.hLo = lo + 180; c.hHi = hi; }
        else if (hi > 179){ c.hLo = lo;       c.hHi = hi - 180; }
        else              { c.hLo = lo;       c.hHi = hi; }

        if (isRed) {
            // 严格限制红色上界 <= 11，彻底杜绝混入黄色/橙色（黄色起始为 H >= 23）。
            // 下界限制 >= 169。
            if (c.hHi > 11 && c.hHi < 100) c.hHi = 11;
            if (c.hLo < 169 && c.hLo > 100) c.hLo = 169;
            // 白墙背景适配：
            // 红色准星（无论是纯色还是“中间白外圈红”）在白墙背景上，因抗锯齿或半透明与白色混合，
            // 边缘饱和度 S 会降低至 35~60。白墙本身 S < 15。
            // 将 sLo 下限定为 35，既能稳定锁住白墙上的红准星，又绝不误识白墙。
            c.sLo = 35;
        } else {
            c.sLo = std::max(40, S - svSlack);
        }
        c.sHi = 255;
    }
    c.vLo = std::max(50, V - svSlack);
    c.vHi = 255;
}

// ── 方案一: pure HSV centroid ────────────────────────────────────────────────
inline Result byColor(const uint8_t* rgba, int w, int h, int stride,
                      const ColorRange& cfg,
                      int cx = -1, int cy = -1, int radiusCover = -1) {
    Result r;
    r.x = w * 0.5f; r.y = h * 0.5f;
    if (rgba == nullptr || w <= 0 || h <= 0) return r;

    const int ex = (cx >= 0) ? cx : w / 2;
    const int ey = (cy >= 0) ? cy : h / 2;
    const int rad = std::max(1, radiusCover > 0 ? radiusCover : cfg.radius);
    const int x0 = std::max(0, ex - rad), y0 = std::max(0, ey - rad);
    const int x1 = std::min(w, ex + rad + 1), y1 = std::min(h, ey + rad + 1);
    if (x0 >= x1 || y0 >= y1) return r;

    long long sx = 0, sy = 0, n = 0;
    for (int y = y0; y < y1; ++y) {
        const uint8_t* row = rgba + (size_t)y * stride + (size_t)x0 * 4;
        for (int x = x0; x < x1; ++x, row += 4) {
            const int r_c = row[0], g_c = row[1], b_c = row[2];
            const int M = std::max(r_c, std::max(g_c, b_c));
            if (M < cfg.vLo || M > cfg.vHi) continue;
            const int m_c = std::min(r_c, std::min(g_c, b_c));
            const int delta = M - m_c;
            if (255 * delta < cfg.sLo * M) continue;

            int H, S, V;
            rgb2hsv(r_c, g_c, b_c, H, S, V);
            if (matches(cfg, H, S, V)) { sx += x; sy += y; ++n; }
        }
    }
    if ((int)n >= std::max(1, cfg.minPix)) {
        r.found = true;
        r.x = (float)((double)sx / (double)n);
        r.y = (float)((double)sy / (double)n);
        r.hits = (int)n;
        r.score = 1.0f;
    }
    return r;
}

// ── 方案二: HSV + connected components + scoring ─────────────────────────────
struct Component {
    int  pix = 0;
    long long sx = 0, sy = 0;
    int l = INT_MAX, r = 0, t = INT_MAX, b = 0;
    float cx() const { return pix ? (float)((double)sx / pix) : 0.0f; }
    float cy() const { return pix ? (float)((double)sy / pix) : 0.0f; }
    int w() const { return r - l + 1; }
    int h() const { return b - t + 1; }
};

inline std::vector<Component> extractComponents(const uint8_t* mask,
                                                uint8_t* visited,
                                                int w, int h, int ox, int oy) {
    std::vector<Component> out;
    std::vector<std::pair<int,int>> q;
    // visited is scratch owned by the caller (pre-zeroed by byFeature);
    // copying it per frame was a pointless 64KB memcpy on every detection.
    std::fill(visited, visited + (size_t)w * h, 0);
    for (int sy = 0; sy < h; ++sy) {
        const uint8_t* row = mask + (size_t)sy * w;
        for (int sx = 0; sx < w; ++sx) {
            if (!row[sx] || visited[(size_t)sy * w + sx]) continue;
            Component c;
            q.clear();
            q.push_back({sx, sy});
            visited[(size_t)sy * w + sx] = 1;
            size_t qh = 0;
            while (qh < q.size()) {
                const auto [x, y] = q[qh++];
                c.pix++;
                c.sx += x + ox; c.sy += y + oy;
                if (x < c.l) c.l = x;
                if (x > c.r) c.r = x;
                if (y < c.t) c.t = y;
                if (y > c.b) c.b = y;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= h) continue;
                    const uint8_t* nr = mask + (size_t)ny * w;
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        if (nx < 0 || nx >= w) continue;
                        if (nr[nx] && !visited[(size_t)ny * w + nx]) {
                            visited[(size_t)ny * w + nx] = 1;
                            q.push_back({nx, ny});
                        }
                    }
                }
            }
            if (c.pix >= 2) out.push_back(c);
        }
    }
    return out;
}

// Merge nearby components (dot+ring / dot+cross shapes come apart).
inline void mergeNearby(std::vector<Component>& list, double mergeDist) {
    if (list.size() < 2) return;
    const int th = (int)mergeDist;
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].pix == 0) continue;
            for (size_t j = i + 1; j < list.size(); ++j) {
                if (list[j].pix == 0) continue;
                const int hgap = std::max(0, std::max(list[i].l - list[j].r,
                                                      list[j].l - list[i].r));
                const int vgap = std::max(0, std::max(list[i].t - list[j].b,
                                                      list[j].t - list[i].b));
                if (std::max(hgap, vgap) > th) continue;
                list[i].pix += list[j].pix;
                list[i].sx  += list[j].sx;
                list[i].sy  += list[j].sy;
                list[i].l = std::min(list[i].l, list[j].l);
                list[i].r = std::max(list[i].r, list[j].r);
                list[i].t = std::min(list[i].t, list[j].t);
                list[i].b = std::max(list[i].b, list[j].b);
                list[j].pix = 0;
                merged = true;
            }
        }
    }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [](const Component& c) { return c.pix == 0; }),
               list.end());
}

// Shape score [0,1] for one component.
inline double componentScore(const Component& c,
                             double ex, double ey, int radius,
                             int minPix, int maxPix, int expectPix) {
    if (c.pix < minPix || c.pix > maxPix) return 0.0;
    double sSize = 1.0;
    if (expectPix > 0) {
        const double ratio = (double)c.pix / (double)expectPix;
        // 白墙背景适配：在白墙上部分抗锯齿边缘红色变淡，通过像素可能减少至原来的 15%，
        // 放宽比例下限至 0.12，避免白墙上直接返回 0 分导致漏检
        if (ratio < 0.12 || ratio > 3.5) return 0.0;
        const double dev = (ratio >= 1.0) ? (ratio - 1.0) : (1.0 - ratio);
        sSize = 1.0 - std::min(0.5, dev * 0.25);
    }
    const int bw = c.w(), bh = c.h();
    if (bw <= 0 || bh <= 0) return 0.0;
    const double ar = (double)std::min(bw, bh) / (double)std::max(bw, bh);
    const double s2 = 0.3 + 0.7 * ar;
    const double fill = (double)c.pix / (double)(bw * bh);
    // 适配纯色准星（如实心红点）：实心圆形/方块准星 fill 在 0.75~1.0 之间，
    // 原先 fill > 0.65 直接 return 0 会导致纯色实心准星被 100% 拒绝。
    // 这里允许高 fill 正常得分。
    double s3 = 1.0;
    if (fill > 0.85) {
        s3 = 0.85; // 实心圆/方块准星赋予良好得分
    } else if (fill > 0.5) {
        s3 = 1.0 - (fill - 0.5) / 0.70;
    }
    const double dx = (double)c.cx() - ex, dy = (double)c.cy() - ey;
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double maxDist = radius * 1.2;
    const double s4 = (dist <= maxDist) ? (1.0 - dist / maxDist) : 0.0;
    return sSize * (0.20 * s2 + 0.25 * s3 + 0.25 * s4 + 0.30);
}

// Cluster position: best component, widened by nearby siblings (same reticle ring/cross).
// 对纯色准星或“中间白色周围红色”准星，包围盒中心 (cl+cr)*0.5f, (ct+cb)*0.5f
// 均精准指向红圈/轮廓正中心的真实瞄准点（即白色点中心）。
inline void clusterPos(const std::vector<Component>& list, int bestIdx,
                       int minPix, float& ox, float& oy) {
    const Component& best = list[bestIdx];
    int cl = best.l, cr = best.r, ct = best.t, cb = best.b;
    int members = 1;
    const int noise = std::max(2, minPix / 2);
    for (size_t i = 0; i < list.size(); ++i) {
        if ((int)i == bestIdx) continue;
        if (list[i].pix < noise) continue;
        // 严格空间约束：仅合并与 best 相距在 18px 以内的同属结构（如同一准星光圈的断续孤岛或准星分叉），
        // 严禁将远处背景/火花/敌方标点合并进来，防止把准星坐标拉飞
        const float cdx = std::abs(list[i].cx() - best.cx());
        const float cdy = std::abs(list[i].cy() - best.cy());
        if (cdx > 18.0f || cdy > 18.0f) continue;

        cl = std::min(cl, list[i].l); cr = std::max(cr, list[i].r);
        ct = std::min(ct, list[i].t); cb = std::max(cb, list[i].b);
        ++members;
    }
    ox = (cl + cr) * 0.5f;
    oy = (ct + cb) * 0.5f;
}

inline Result byFeature(const uint8_t* rgba, int w, int h, int stride,
                        const ColorRange& cfg,
                        int cx = -1, int cy = -1, int radiusCover = -1,
                        int maxPix = 160, double mergeDist = 12.0,
                        double scoreGate = 0.40) {
    Result r;
    r.x = w * 0.5f; r.y = h * 0.5f;
    if (rgba == nullptr || w <= 0 || h <= 0) return r;

    const int ex = (cx >= 0) ? cx : w / 2;
    const int ey = (cy >= 0) ? cy : h / 2;
    const int rad = std::max(1, radiusCover > 0 ? radiusCover : cfg.radius);
    const int x0 = std::max(0, ex - rad), y0 = std::max(0, ey - rad);
    const int x1 = std::min(w, ex + rad + 1), y1 = std::min(h, ey + rad + 1);
    if (x0 >= x1 || y0 >= y1) return r;

    const int mw = x1 - x0, mh = y1 - y0;
    if (mw > 256 || mh > 256)   // degenerate window — fall back to centroid
        return byColor(rgba, w, h, stride, cfg, ex, ey, rad);

    // Scratch buffers are reused across frames: allocating two 64KB vectors
    // per detection at 60 Hz was pure allocator churn (and measurable heat).
    static thread_local std::vector<uint8_t> mask, visited;
    if ((int)mask.size() < mw * mh) {
        mask.assign((size_t)mw * mh, 0);
        visited.assign((size_t)mw * mh, 0);
    } else {
        std::fill(mask.begin(), mask.begin() + mw * mh, 0);
    }
    long long sx = 0, sy = 0, n = 0;
    for (int y = 0; y < mh; ++y) {
        const uint8_t* row = rgba + (size_t)(y0 + y) * stride + (size_t)x0 * 4;
        uint8_t* mrow = mask.data() + (size_t)y * mw;
        for (int x = 0; x < mw; ++x, row += 4) {
            const int r_c = row[0], g_c = row[1], b_c = row[2];
            const int M = std::max(r_c, std::max(g_c, b_c));
            if (M < cfg.vLo || M > cfg.vHi) continue;
            const int m_c = std::min(r_c, std::min(g_c, b_c));
            const int delta = M - m_c;
            if (255 * delta < cfg.sLo * M) continue;

            int H, S, V;
            rgb2hsv(r_c, g_c, b_c, H, S, V);
            if (matches(cfg, H, S, V)) {
                mrow[x] = 1;
                sx += x0 + x; sy += y0 + y; ++n;
            }
        }
    }
    if ((int)n < std::max(1, cfg.minPix)) return r;
    if ((int)n > maxPix) return r;

    auto comps = extractComponents(mask.data(), visited.data(), mw, mh, x0, y0);
    if (comps.size() > 1) mergeNearby(comps, mergeDist);

    int best = -1;
    double bestScore = 0.0;
    for (size_t i = 0; i < comps.size(); ++i) {
        const double s = componentScore(comps[i], ex, ey, rad,
                                        cfg.minPix, maxPix, cfg.expectPix);
        if (s > bestScore) { bestScore = s; best = (int)i; }
    }
    if (best >= 0 && bestScore >= scoreGate) {
        r.found = true;
        clusterPos(comps, best, cfg.minPix, r.x, r.y);
        r.hits = comps[best].pix;
        r.score = (float)bestScore;
    }
    return r;
}

// Multi-config: independent per-config runs, highest score wins (each config's
// own expectPix arbitrates size, so same-colour-different-size sights coexist).
inline Result multi(const uint8_t* rgba, int w, int h, int stride,
                    const std::vector<ColorRange>& cfgs,
                    int cx = -1, int cy = -1, int radiusCover = -1,
                    int maxPix = 160, double mergeDist = 12.0,
                    double scoreGate = 0.40) {
    Result best;
    best.x = w * 0.5f; best.y = h * 0.5f;
    if (cfgs.empty()) return best;
    for (size_t i = 0; i < cfgs.size(); ++i) {
        const int cap = (cfgs[i].expectPix > 0)
                            ? std::max(cfgs[i].expectPix * 3 / 2, 30)
                            : maxPix;
        Result r = byFeature(rgba, w, h, stride, cfgs[i], cx, cy, radiusCover,
                             cap, mergeDist, scoreGate);
        if (!r.found) continue;
        if (r.score > best.score ||
            (r.score == best.score && r.hits > best.hits)) {
            best = r;
            best.cfg = (int)i;
        }
    }
    return best;
}

}  // namespace crosshair
}  // namespace aimbotng
