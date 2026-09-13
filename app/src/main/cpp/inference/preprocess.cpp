// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of preprocess.h
//
//  The resampling is a two-pass separable filter built from per-axis weight
//  tables. That shape was chosen for a specific reason: the correct filter
//  depends on the direction of the scale, and a detector notices when it is
//  wrong.
//
//    * Downscaling (a 640 crop into a 320 model, the common case) needs an area
//      average. Bilinear at a factor of two reads two of the four source pixels
//      per output and aliases the other two away; thin structures — a limb, a
//      weapon — flicker in and out between frames, which shows up as a box that
//      cannot make up its mind rather than as an obviously blurry image.
//    * Upscaling needs interpolation. There is no area to average.
//
//  One weight table handles both: each output sample covers a span of source
//  centre coordinates, and the taps are the source pixels that span touches,
//  weighted by how much of it they cover. A span shorter than one pixel
//  degenerates to the two adjacent samples, i.e. linear interpolation, and a
//  span of two pixels degenerates to a uniform four-tap average. The tables are
//  built once per shape instead of per frame, so the inner loop is a weighted
//  sum with no branching.
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/preprocess.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace aimbotng {
namespace infer {
namespace {

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

using Tap = std::pair<int, float>;   // source index, weight

/// Weight table for one axis.
///
/// `offset[i]` .. `offset[i + 1]` indexes `taps` for output sample `i`. An empty
/// range means the output sample fell entirely in the padding, which is how the
/// letterbox bars stay untouched without a special case in the pixel loop.
///
/// `origin` is the source coordinate of output sample 0's left edge; `step` is
/// how many source pixels one output sample covers. Both in source pixels.
struct Axis {
    std::vector<int> offset;
    std::vector<Tap> taps;

    bool has(int i) const { return offset[i + 1] > offset[i]; }
    int  begin(int i) const { return offset[i]; }
    int  end(int i) const { return offset[i + 1]; }
};

Axis buildAxis(int srcLen, int dstLen, float origin, float step) {
    Axis ax;
    ax.offset.reserve(static_cast<size_t>(dstLen) + 1);
    // Two taps per output sample is the common case (a downscale averages two
    // neighbours an axis, an upscale interpolates between two); reserving up to
    // four covers a downscale by more than two without a reallocation. The
    // tables are rebuilt every frame, so this is a per-frame allocation either
    // way — the reserve just makes it one instead of eleven.
    ax.taps.reserve(static_cast<size_t>(dstLen) * 4u);

    // Two regimes, because the correct filter depends on which way the scale
    // goes — and getting this wrong is not subtle.
    //
    //   * step < 1 (upscaling): an output sample is narrower than a source
    //     pixel, so exactly one pixel always owns it. Area averaging would
    //     therefore pick that one pixel and produce a nearest-neighbour image —
    //     blocky and stair-stepped. Interpolate between the two samples whose
    //     centres straddle it instead.
    //   * step >= 1 (downscaling, the common case): average the covered pixels,
    //     weighted by coverage. Interpolating here would read two of the four
    //     pixels a 2x reduction covers and alias the other two away, which shows
    //     up as thin structures flickering in and out between frames.
    const bool upscale = step < 1.0f;

    for (int i = 0; i < dstLen; ++i) {
        ax.offset.push_back(static_cast<int>(ax.taps.size()));

        // lo and hi are *edges* in source-pixel space, where pixel k spans
        // [k, k+1). Mixing that with a centre convention (pixel k at k+0.5) is
        // what turns a symmetric 2x2 average into a filter that samples half a
        // pixel off-centre and darkens the last column.
        const float lo = origin + step * static_cast<float>(i);
        const float hi = lo + step;

        if (upscale) {
            const float centre = lo + step * 0.5f - 0.5f;   // to centre space
            const int   base   = static_cast<int>(std::floor(centre));
            const float t      = centre - static_cast<float>(base);
            int k0 = base;
            int k1 = base + 1;
            if (srcLen == 1) {
                k0 = k1 = 0;
            } else {
                k0 = std::max(0, std::min(srcLen - 1, k0));
                k1 = std::max(0, std::min(srcLen - 1, k1));
            }
            // Clamping at an edge collapses the pair onto one sample, which is
            // the correct edge behaviour: there is nothing outside to reach for.
            if (k0 == k1) {
                ax.taps.push_back({k0, 1.0f});
            } else {
                ax.taps.push_back({k0, 1.0f - t});
                ax.taps.push_back({k1, t});
            }
            continue;
        }

        int first = static_cast<int>(std::floor(lo));
        int last  = static_cast<int>(std::ceil(hi)) - 1;
        if (last < first) last = first;

        first = std::max(first, 0);
        last  = std::min(last, srcLen - 1);
        if (first > last) continue;   // wholly in the letterbox bar: leave the pad

        float sum = 0.0f;
        for (int k = first; k <= last; ++k) {
            const float kLo = static_cast<float>(k);
            const float kHi = kLo + 1.0f;
            const float w = std::max(0.0f, std::min(hi, kHi) - std::max(lo, kLo));
            ax.taps.push_back({k, w});
            sum += w;
        }
        if (sum <= 0.0f) {
            ax.taps.resize(ax.taps.size() - static_cast<size_t>(last - first + 1));
            continue;
        }
        for (size_t t = ax.taps.size() - static_cast<size_t>(last - first + 1);
             t < ax.taps.size(); ++t) {
            ax.taps[t].second /= sum;
        }
    }
    ax.offset.push_back(static_cast<int>(ax.taps.size()));
    return ax;
}

/// Geometry shared by both output formats: where the source rect is, how it
/// maps into the target square, and the axis tables to do it with.
struct Plan {
    bool        ok = false;
    int         srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    int         targetW = 0, targetH = 0;
    LetterboxMap map;
    Axis        x, y;
};

Plan makePlan(const FrameView& frame, const Roi& roi, const PreprocessConfig& cfg) {
    Plan p;
    if (!frame.valid() || cfg.targetW <= 0 || cfg.targetH <= 0) return p;

    const Roi src = Roi::resolve(roi, frame);
    if (src.empty()) return p;

    p.srcX = src.x;
    p.srcY = src.y;
    p.srcW = src.w;
    p.srcH = src.h;
    p.targetW = cfg.targetW;
    p.targetH = cfg.targetH;

    float scaleX = static_cast<float>(cfg.targetW) / static_cast<float>(src.w);
    float scaleY = static_cast<float>(cfg.targetH) / static_cast<float>(src.h);
    float scale  = cfg.keepAspect ? std::min(scaleX, scaleY) : 1.0f;

    int newW = cfg.targetW;
    int newH = cfg.targetH;
    float padX = 0.0f, padY = 0.0f;
    if (cfg.keepAspect) {
        // Rounded, not truncated: the padding is computed from the rounded size,
        // so a truncated 320.7 would leave a half-pixel bar of unwritten output
        // on one side that no tap ever covers.
        newW = std::max(1, static_cast<int>(std::lround(src.w * scale)));
        newH = std::max(1, static_cast<int>(std::lround(src.h * scale)));
        padX = static_cast<float>(cfg.targetW - newW) * 0.5f;
        padY = static_cast<float>(cfg.targetH - newH) * 0.5f;
    }

    // Output -> source. Inverted so a tap index is `(dst * step) + origin`.
    const float stepX = static_cast<float>(src.w) / static_cast<float>(newW);
    const float stepY = static_cast<float>(src.h) / static_cast<float>(newH);
    p.x = buildAxis(src.w, cfg.targetW, -padX * stepX, stepX);
    p.y = buildAxis(src.h, cfg.targetH, -padY * stepY, stepY);

    // The map the decoder needs: target pixels back to frame pixels.
    p.map.scale = static_cast<float>(newW) / static_cast<float>(src.w);
    p.map.padX  = padX;
    p.map.padY  = padY;
    p.map.roiX  = src.x;
    p.map.roiY  = src.y;

    p.ok = true;
    return p;
}

/// Reads an RGBA8888 pixel as three 0..255 floats. The byte order is R,G,B,A —
/// the capture module documented the GraphicBuffer as RGBA_8888 and the whole
/// chain honours it, so `bgr` is handled by swapping at the write, not by
/// reinterpreting here.
inline void readRgb(const uint8_t* px, float& r, float& g, float& b) {
    r = static_cast<float>(px[0]);
    g = static_cast<float>(px[1]);
    b = static_cast<float>(px[2]);
}

/// Resamples one frame into `dst`, three channels, using `write` to place a
/// finished pixel. Keeping the placement in a lambda is what lets the float and
/// uint8 variants share everything else — the resampling is identical and only
/// the arithmetic at the end differs.
template <typename WritePixel>
bool resample(const Plan& p, const FrameView& frame, WritePixel write) {
    if (!p.ok) return false;

    const uint8_t* base = frame.pixels +
                          static_cast<size_t>(p.srcY) * static_cast<size_t>(frame.rowStrideBytes) +
                          static_cast<size_t>(p.srcX) * 4u;

    for (int y = 0; y < p.targetH; ++y) {
        if (!p.y.has(y)) continue;   // letterbox bar
        for (int x = 0; x < p.targetW; ++x) {
            if (!p.x.has(x)) continue;

            float acc[3] = {0.0f, 0.0f, 0.0f};
            for (int ty = p.y.begin(y); ty < p.y.end(y); ++ty) {
                const float wy = p.y.taps[static_cast<size_t>(ty)].second;
                const size_t row = static_cast<size_t>(p.y.taps[static_cast<size_t>(ty)].first) *
                                   static_cast<size_t>(frame.rowStrideBytes);
                for (int tx = p.x.begin(x); tx < p.x.end(x); ++tx) {
                    const float w = wy * p.x.taps[static_cast<size_t>(tx)].second;
                    const uint8_t* px = base + row +
                                        static_cast<size_t>(p.x.taps[static_cast<size_t>(tx)].first) * 4u;
                    float r, g, b;
                    readRgb(px, r, g, b);
                    acc[0] += r * w;
                    acc[1] += g * w;
                    acc[2] += b * w;
                }
            }
            write(x, y, acc);
        }
    }
    return true;
}

}  // namespace

// ── Roi ─────────────────────────────────────────────────────────────────────
// Defined here rather than in a types-only translation unit because the rule it
// encodes — "empty means the whole frame, otherwise clip" — is the same rule the
// preprocessor applies, and splitting them is how they drift.

Roi Roi::resolve(const Roi& wanted, const FrameView& frame) {
    if (wanted.empty()) return Roi{0, 0, frame.width, frame.height};

    int x0 = std::max(0, wanted.x);
    int y0 = std::max(0, wanted.y);
    int x1 = std::min(frame.width, wanted.x + wanted.w);
    int y1 = std::min(frame.height, wanted.y + wanted.h);
    if (x1 <= x0 || y1 <= y0) return Roi{};
    return Roi{x0, y0, x1 - x0, y1 - y0};
}

// ── LetterboxMap ────────────────────────────────────────────────────────────

void LetterboxMap::toFrame(float& x1, float& y1, float& x2, float& y2) const {
    if (scale <= 0.0f) return;
    const float inv = 1.0f / scale;
    x1 = (x1 - padX) * inv + static_cast<float>(roiX);
    y1 = (y1 - padY) * inv + static_cast<float>(roiY);
    x2 = (x2 - padX) * inv + static_cast<float>(roiX);
    y2 = (y2 - padY) * inv + static_cast<float>(roiY);
}

// ── The two output variants ─────────────────────────────────────────────────

LetterboxMap letterboxRgba(const FrameView& frame, const Roi& roi,
                           const PreprocessConfig& cfg, float* out) {
    const Plan p = makePlan(frame, roi, cfg);
    if (out == nullptr || !p.ok) return LetterboxMap::identity();

    const int w = p.targetW;
    const int h = p.targetH;

    // Padding first, so the bars are written even in the regions no tap covers.
    const float pad = clamp01(cfg.padValue);
    const size_t plane = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < plane; ++i) {
        if (cfg.nchw) {
            out[i]           = pad;
            out[plane + i]   = pad;
            out[2 * plane + i] = pad;
        } else {
            out[i * 3 + 0] = pad;
            out[i * 3 + 1] = pad;
            out[i * 3 + 2] = pad;
        }
    }

    const float sc = cfg.scale;
    resample(p, frame, [&](int x, int y, const float* rgb) {
        const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
        const int c0 = cfg.bgr ? 2 : 0;
        const int c2 = cfg.bgr ? 0 : 2;
        float v[3];
        for (int c = 0; c < 3; ++c) {
            v[c] = rgb[c] * sc;
            if (cfg.normalize) {
                // Means and stddevs are conventionally given in 0..255 units even
                // when the model wants 0..1, so the divide happens after the
                // scale and the offset is scaled with it.
                v[c] = (rgb[c] - cfg.mean[c]) * (1.0f / cfg.stdv[c]);
            }
        }
        if (cfg.nchw) {
            out[0 * plane + i] = v[c0];
            out[1 * plane + i] = v[1];
            out[2 * plane + i] = v[c2];
        } else {
            out[i * 3 + 0] = v[c0];
            out[i * 3 + 1] = v[1];
            out[i * 3 + 2] = v[c2];
        }
    });

    return p.map;
}

LetterboxMap letterboxRgbaU8(const FrameView& frame, const Roi& roi,
                             const PreprocessConfig& cfg, uint8_t* out) {
    const Plan p = makePlan(frame, roi, cfg);
    if (out == nullptr || !p.ok) return LetterboxMap::identity();

    const int w = p.targetW;
    const int h = p.targetH;
    const uint8_t pad = static_cast<uint8_t>(
        std::lround(clamp01(cfg.padValue) * 255.0f));
    const size_t plane = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < plane; ++i) {
        if (cfg.nchw) {
            out[i] = out[plane + i] = out[2 * plane + i] = pad;
        } else {
            out[i * 3 + 0] = out[i * 3 + 1] = out[i * 3 + 2] = pad;
        }
    }

    resample(p, frame, [&](int x, int y, const float* rgb) {
        const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
        const int c0 = cfg.bgr ? 2 : 0;
        const int c2 = cfg.bgr ? 0 : 2;
        const uint8_t q[3] = {
            static_cast<uint8_t>(std::lround(rgb[0])),
            static_cast<uint8_t>(std::lround(rgb[1])),
            static_cast<uint8_t>(std::lround(rgb[2])),
        };
        if (cfg.nchw) {
            out[0 * plane + i] = q[c0];
            out[1 * plane + i] = q[1];
            out[2 * plane + i] = q[c2];
        } else {
            out[i * 3 + 0] = q[c0];
            out[i * 3 + 1] = q[1];
            out[i * 3 + 2] = q[c2];
        }
    });

    return p.map;
}

size_t preprocessBufferBytes(const PreprocessConfig& cfg, int bytesPerChannel) {
    if (cfg.targetW <= 0 || cfg.targetH <= 0 || bytesPerChannel <= 0) return 0;
    return static_cast<size_t>(cfg.targetW) * static_cast<size_t>(cfg.targetH) * 3u *
           static_cast<size_t>(bytesPerChannel);
}

PreprocessConfig preprocessFor(int inputSize) {
    PreprocessConfig cfg;
    const int side = inputSize > 0 ? inputSize : 640;
    cfg.targetW = side;
    cfg.targetH = side;
    return cfg;
}

}  // namespace infer
}  // namespace aimbotng
