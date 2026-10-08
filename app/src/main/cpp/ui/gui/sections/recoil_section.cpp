// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::recoil — implementation. See recoil_section.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "recoil_section.h"

#include "ui/gui/hud.h"
#include "ui/gui/ime.h"
#include "ui/gui/notify.h"
#include "ui/gui/theme.h"
#include "ui/gui/sections/aim_section.h"
#include "ui/gui/sections/backflash_section.h"
#include "ui/gui/sections/trigger_section.h"

#ifdef HAS_IMGUI

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "capture/capture.h"
#include "imgui.h"
#include "input/touch_reader.h"
#include "input/uinput_inject.h"
#include "tracking/crosshair_recoil.h"
#include "tracking/recoil_recorder.h"
#include "tracking/valorant_recoil.h"
#include "ui/gui/sections/capture_section.h"

#define TAG "AimbotRecoil"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace ui {
namespace sections {

PageRecoil g_pageRecoil;

namespace {

using namespace theme;

// ── Layout metrics, design px against a 1200 px board ───────────────────────
constexpr float kRowGap   = 14.0f;
constexpr float kLabelGap = 6.0f;
constexpr float kLeftShare = 0.56f;
constexpr float kColGap   = 26.0f;
constexpr float kFieldH   = 62.0f;
constexpr float kBtnH     = 60.0f;
constexpr float kTextS    = 22.0f;
constexpr float kSmallS   = 19.0f;

// ── Overlay look ────────────────────────────────────────────────────────────
constexpr ImU32 kFireLine   = IM_COL32(255, 176,  32, 235);
constexpr ImU32 kFireFill   = IM_COL32(255, 176,  32,  28);
constexpr ImU32 kMarkerLine = IM_COL32( 90, 220, 255, 235);
constexpr ImU32 kMarkerFill = IM_COL32( 90, 220, 255,  30);
constexpr ImU32 kSquareLine = IM_COL32(255, 255, 255,  70);

// ── Burst / contact state ───────────────────────────────────────────────────
//
// One burst at a time, and one owner of the look contact at a time. Which
// owner it is depends on the Aim page's master switch rather than on whether
// the aim happens to be driving this frame, and that is deliberate: a rule
// keyed on "is the aim driving" would hand the slot back and forth inside a
// single burst (the aim drops its contact the moment the target leaves the
// screen), and each hand-over is a lift+press on the one contact the game is
// using for the camera.
recoil::Player g_player;
bool  g_playerArmed = false;

bool  g_ownHeld = false;              ///< we pressed UINPUT_SLOT_PRIMARY
float g_ownBaseX = 0.0f, g_ownBaseY = 0.0f;   ///< its home, before compensation
float g_ownX = 0.0f, g_ownY = 0.0f;           ///< its current position = home + accumulated

bool  g_wasFiring = false;
float g_emitX = 0.0f, g_emitY = 0.0f;         ///< this frame's aim delta

uint32_t g_rng = 0x5EED1234u;
float rnd01() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return static_cast<float>((g_rng >> 8) & 0xFFFFFFu) / 16777216.0f;
}

bool pointIn(float x, float y, const widgets::Rect& r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/// Screen rectangle of the Aim page's touch area.
///
/// This is the region the compensation is allowed to displace, and the home of
/// the synthetic contact when nobody else is holding one. It falls back to a
/// square in the middle of the screen when the user has never placed it, which
/// is where the Aim page would put it on its own first frame — so the two pages
/// agree about the region even before anyone drags it.
widgets::Rect aimTouchRect() {
    const TouchAreaOverlay& a = g_pageAim.touchArea;
    if (a.placed && a.w > 4.0f && a.h > 4.0f) return widgets::Rect{a.x, a.y, a.w, a.h};
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const float side = std::min(d.x, d.y) * 0.35f;
    return widgets::Rect{d.x * 0.5f - side * 0.5f, d.y * 0.5f - side * 0.5f, side, side};
}

/// The capture pipeline's centre square in screen pixels — the region the
/// recorder can actually see. Returns false when no frame has arrived.
bool captureSquare(widgets::Rect& out) {
    int w = 0, h = 0;
    int cx = 0, cy = 0;
    if (!capture::peekSize(w, h) || w <= 0) return false;
    if (!capture::cropOrigin(cx, cy)) return false;
    out = widgets::Rect{static_cast<float>(cx), static_cast<float>(cy),
                        static_cast<float>(w), static_cast<float>(h)};
    return true;
}

bool anyPhysicalFingerIn(const widgets::Rect& r) {
    ReaderPointer buf[READER_MAX_POINTERS];
    const int n = reader_read_pointers(buf, READER_MAX_POINTERS);
    for (int i = 0; i < n; ++i) {
        if (pointIn(buf[i].x, buf[i].y, r)) return true;
    }
    return false;
}

/// Is the burst firing right now?
///
/// Two sources, because the two ways of shooting are visible in different
/// places: a finger on the game's fire button arrives through the panel (and is
/// therefore in the pointer list), while a tap injected by the Trigger page goes
/// out through uinput and never appears there at all.
bool firingNow() {
    PageRecoil& pg = g_pageRecoil;
    if (triggerIsFiring()) return true;
    if (pg.continuousTrigger.value) return true;
    if (!pg.fire.placed) return false;
    ReaderPointer buf[READER_MAX_POINTERS];
    const int n = reader_read_pointers(buf, READER_MAX_POINTERS);
    const float r2 = pg.fire.r * pg.fire.r;
    for (int i = 0; i < n; ++i) {
        const float dx = buf[i].x - pg.fire.cx;
        const float dy = buf[i].y - pg.fire.cy;
        if (dx * dx + dy * dy <= r2) return true;
    }
    return false;
}

void dashedRect(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx, ImU32 col,
                float dash, float thick) {
    for (float x = mn.x; x < mx.x; x += dash * 2.0f) {
        dl->AddLine(ImVec2(x, mn.y), ImVec2(std::min(x + dash, mx.x), mn.y), col, thick);
        dl->AddLine(ImVec2(x, mx.y), ImVec2(std::min(x + dash, mx.x), mx.y), col, thick);
    }
    for (float y = mn.y; y < mx.y; y += dash * 2.0f) {
        dl->AddLine(ImVec2(mn.x, y), ImVec2(mn.x, std::min(y + dash, mx.y)), col, thick);
        dl->AddLine(ImVec2(mx.x, y), ImVec2(mx.x, std::min(y + dash, mx.y)), col, thick);
    }
}

// ── Catalogue ───────────────────────────────────────────────────────────────

void refreshCatalog(PageRecoil& pg) {
    if (!pg.catalogDirty) return;
    pg.catalogDirty = false;

    const std::string keep = (pg.loaded >= 0 && pg.loaded < static_cast<int>(pg.profiles.size()))
                                 ? pg.profiles[static_cast<size_t>(pg.loaded)].name
                                 : std::string();

    pg.profiles = recoil::listProfiles();
    pg.names.clear();
    pg.namePtrs.clear();
    pg.names.reserve(pg.profiles.size());
    for (const recoil::Profile& p : pg.profiles) pg.names.push_back(p.name);
    for (const std::string& n : pg.names) pg.namePtrs.push_back(n.c_str());

    pg.loaded = -1;
    for (size_t i = 0; i < pg.profiles.size(); ++i) {
        if (pg.profiles[i].name == keep) { pg.loaded = static_cast<int>(i); break; }
    }
    // A name the config file asked for beats the carry-over above: it is an
    // instruction ("this is the weapon I was using"), not a request to keep
    // whatever happened to be loaded.
    if (!pg.wantName.empty()) {
        for (size_t i = 0; i < pg.profiles.size(); ++i) {
            if (pg.profiles[i].name == pg.wantName) {
                pg.loaded = static_cast<int>(i);
                break;
            }
        }
        pg.wantName.clear();
    }

    if (pg.loaded < 0 && !pg.profiles.empty()) pg.loaded = 0;
    if (pg.loaded >= 0) pg.pick.value = pg.loaded;

    LOGI("recoil: catalogue has %zu profile(s)", pg.profiles.size());
}

recoil::Profile* activeProfile(PageRecoil& pg) {
    if (pg.loaded < 0 || pg.loaded >= static_cast<int>(pg.profiles.size())) return nullptr;
    return &pg.profiles[static_cast<size_t>(pg.loaded)];
}

/// Degrees of view rotation per finger pixel for one axis, and the axis'
/// peak-to-peak excursion — the pair the compensation scale is built from.
/// Returns false when the profile cannot drive that axis.
bool axisScale(const recoil::Profile& p, int axis, float travelFp,
               float& outDegPerFp, float& outPeakDeg) {
    outPeakDeg = (axis == 0) ? p.dxPeak : p.dyPeak;
    outDegPerFp = 0.0f;
    if (outPeakDeg < 0.03f || travelFp < 1.0f) return false;
    outDegPerFp = outPeakDeg / travelFp;   // deg per finger px
    return true;
}

// ── The vertical pattern chart ──────────────────────────────────────────────
//
// x is the weapon's swing on the edited axis, y is time (shot 1 at the top).
// Two curves: the measurement, dashed and grey, and the tuned one, solid. The
// scale is fixed from the MEASUREMENT's peak-to-peak rather than from whatever
// the tuned curve currently spans — a chart that re-scaled itself as the user
// dragged would move the node out from under the finger that is holding it.
bool drawPatternChart(ImDrawList* dl, const widgets::Rect& r, float s, const Xf& xf,
                      PageRecoil& pg) {
    ImFont* font = ImGui::GetFont();
    dl->AddRectFilled(xf.pt(ImVec2(r.x, r.y)), xf.pt(ImVec2(r.x + r.w, r.y + r.h)),
                      xf.col(ListBg), xf.s(12.0f * s));
    dl->AddRect(xf.pt(ImVec2(r.x, r.y)), xf.pt(ImVec2(r.x + r.w, r.y + r.h)),
                xf.col(Edge), xf.s(12.0f * s), 0, xf.s(1.0f * s));

    recoil::Profile* p = activeProfile(pg);
    if (p == nullptr || !p->loaded || p->knots.size() < 2) {
        textCentred(dl, xf.pt(ImVec2(r.x, r.y)), xf.pt(ImVec2(r.x + r.w, r.y + r.h)),
                    xf.s(22.0f * s), xf.col(TextMuted), "还没有档案");
        return false;
    }

    const int   axis = pg.editVertical.value ? 1 : 0;
    const float travelFp = pg.editVertical.value ? pg.travelY.value : pg.travelX.value;
    float degPerFp = 0.0f, peakDeg = 0.0f;
    if (!axisScale(*p, axis, travelFp, degPerFp, peakDeg)) {
        textCentred(dl, xf.pt(ImVec2(r.x, r.y)), xf.pt(ImVec2(r.x + r.w, r.y + r.h)),
                    xf.s(20.0f * s), xf.col(TextMuted),
                    pg.editVertical.value ? "该轴没有位移" : "水平方向几乎没有位移");
        return false;
    }

    const float pad = 12.0f * s;
    const float top = r.y + pad, bot = r.y + r.h - pad;
    const float cx  = r.x + r.w * 0.5f;
    // ±42 % of the width covers the full peak-to-peak swing with a margin; the
    // margin is where "you have over-corrected" is visible as a clipped node.
    const float kx = (r.w * 0.42f) / (peakDeg * 0.5f);
    const float span = std::max(1.0f, p->spanMs());

    // The staircase, expanded once: cumulative finger pixels per shot.
    std::vector<float> tune = pg.editVertical.value ? p->tuneY : p->tuneX;
    const int shots = p->shots > 0 ? p->shots
                                   : static_cast<int>(p->marks.size());

    auto xOf = [&](float deg, float tuneDeg) {
        return cx + (deg + tuneDeg) * kx;
    };
    auto yOf = [&](float tMs) { return top + (tMs / span) * (bot - top); };
    auto degAt = [&](size_t i, bool vertical) {
        return vertical ? p->knots[i].dy : p->knots[i].dx;
    };
    /// Cumulative tune (finger px) in force from the knot at index `i` onward.
    auto stairAt = [&](size_t i) {
        float acc = 0.0f;
        const float t = p->knots[i].tMs;
        for (size_t k = 0; k < p->marks.size(); ++k) {
            if (p->marks[k].tMs > t) break;
            if (k < tune.size()) acc += tune[k];
        }
        return acc;
    };

    // Zero line, so "no swing" is where the eye expects it.
    dl->AddLine(xf.pt(ImVec2(cx, top)), xf.pt(ImVec2(cx, bot)),
                xf.col(IM_COL32(255, 255, 255, 26)), xf.s(1.0f * s));

    // ── Measurement, dashed ─────────────────────────────────────────────────
    {
        const ImU32 col = IM_COL32(150, 156, 170, 160);
        ImVec2 prev(0, 0);
        bool have = false;
        for (size_t i = 0; i < p->knots.size(); ++i) {
            const ImVec2 pt(xOf(degAt(i, pg.editVertical.value != 0), 0.0f),
                            yOf(p->knots[i].tMs));
            if (have && ((i & 1u) == 0u)) {
                dl->AddLine(xf.pt(prev), xf.pt(pt), xf.col(col), xf.s(2.0f * s));
            }
            prev = pt;
            have = true;
        }
    }

    // ── Tuned, solid ────────────────────────────────────────────────────────
    {
        ImVec2 prev(0, 0);
        bool have = false;
        for (size_t i = 0; i < p->knots.size(); ++i) {
            const float tuneDeg = stairAt(i) / degPerFp;
            const ImVec2 pt(xOf(degAt(i, pg.editVertical.value != 0), tuneDeg),
                            yOf(p->knots[i].tMs));
            if (have) dl->AddLine(xf.pt(prev), xf.pt(pt), xf.col(Accent), xf.s(2.6f * s));
            prev = pt;
            have = true;
        }
    }

    // ── Shot nodes ──────────────────────────────────────────────────────────
    const float grip = 24.0f * s;
    const ImGuiIO& io = ImGui::GetIO();
    const bool fresh = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    for (size_t k = 0; k < p->marks.size(); ++k) {
        const float t = p->marks[k].tMs;
        // The curve's value at this shot, interpolated across the knot grid.
        const size_t i = static_cast<size_t>(
            std::min(p->knots.size() - 1,
                     static_cast<size_t>(std::max(0.0f, t) / 10.0f + 0.5f)));
        const float tuneDeg = stairAt(i) / degPerFp;
        const ImVec2 pt(xOf(degAt(i, pg.editVertical.value != 0), tuneDeg), yOf(t));

        const bool hot = (pg.dragShot == static_cast<int>(k));
        dl->AddCircleFilled(xf.pt(pt), xf.s((hot ? 9.0f : 6.0f) * s),
                            xf.col(hot ? Accent : TextMuted), 16);

        if (fresh && std::fabs(io.MousePos.x - pt.x) < grip &&
            std::fabs(io.MousePos.y - pt.y) < grip) {
            pg.dragShot = static_cast<int>(k);
            pg.dragActive = true;
            pg.dragStartX = io.MousePos.x;
            pg.dragBaseTune = (k < tune.size()) ? tune[k] : 0.0f;
            widgets::setWidgetDragging();
            widgets::consumeGesture();
        }
    }

    // ── Drag ────────────────────────────────────────────────────────────────
    if (pg.dragActive && pg.dragShot >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Horizontal screen pixels -> degrees -> finger pixels. The first
            // step is the chart's own scale, the second is the user's 整梭行程;
            // skipping the second would store a correction in degrees and lose
            // it the moment S changed.
            const float dpx = io.MousePos.x - pg.dragStartX;
            const float dDeg = dpx / kx;
            const float dFp = dDeg / degPerFp;
            std::vector<float>& tv = pg.editVertical.value ? p->tuneY : p->tuneX;
            if (tv.size() < static_cast<size_t>(shots)) tv.resize(static_cast<size_t>(shots), 0.0f);
            if (pg.dragShot < static_cast<int>(tv.size())) {
                tv[static_cast<size_t>(pg.dragShot)] = pg.dragBaseTune + dFp;
            }
        } else {
            pg.dragActive = false;
            pg.dragShot = -1;
            widgets::clearWidgetDragging();
            // Note: `catalogDirty` is deliberately NOT touched here. The drag
            // changed values in memory, not files on disk, and clearing a
            // pending rescan would lose a catalogue refresh that was queued by
            // a just-dismissed recorder card.
        }
    }

    // Header line: which axis, and what the scale currently means.
    char head[128];
    std::snprintf(head, sizeof(head), "%s  峰峰 %.2f°  ·  %.1f fp/°",
                  pg.editVertical.value ? "垂直轴" : "水平轴", peakDeg, 1.0f / degPerFp);
    dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                xf.pt(ImVec2(r.x + pad, r.y + pad)),
                xf.col(TextMuted), head);
    return true;
}

// ── 准心识别 mode ───────────────────────────────────────────────────────────

/// Live crosshair preview: the capture square mapped into a panel, with the
/// detected crosshair, the screen centre and the aim's target box drawn on it.
void drawCrosshairPreview(ImDrawList* dl, const widgets::Rect& r, float s,
                          const Xf& xf) {
    const PageRecoil& pg = g_pageRecoil;
    const bool active = pg.enabled.value && pg.xTrack.value &&
                        (pg.continuousTrigger.value || recoilIsFiring());
    if (active) {
        crosshair::pumpPreview();
    }

    using crosshair::Preview;
    Preview pv;
    crosshair::preview(pv);

    const ImU32 bg    = IM_COL32(16, 18, 22, 255);
    const ImU32 line  = IM_COL32(255, 255, 255, 40);
    const ImU32 grey  = IM_COL32(150, 150, 150, 200);
    const ImU32 green = IM_COL32( 80, 230, 120, 255);
    const ImU32 red   = IM_COL32(255,  80,  80, 220);
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), bg, 8.0f * s);
    dl->AddRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), line, 8.0f * s);

    ImFont* font = ImGui::GetFont();
    auto text = [&](const char* t, float px, float py, ImU32 col, float size) {
        dl->AddText(font, tsize(size * s) * xf.scale, ImVec2(px, py), col, t);
    };

    // Map a frame point into the panel. Without a frame there is nothing to
    // draw but the caption.
    widgets::Rect sq{};
    const bool haveSq = captureSquare(sq);
    auto toPanel = [&](float fx, float fy) {
        const float u = (sq.w > 1.0f) ? (fx - sq.x) / sq.w : 0.5f;
        const float v = (sq.h > 1.0f) ? (fy - sq.y) / sq.h : 0.5f;
        return ImVec2(r.x + u * r.w, r.y + v * r.h);
    };

    if (pv.haveTarget && haveSq) {
        const ImVec2 a = toPanel(pv.tx, pv.ty);
        const ImVec2 b = toPanel(pv.tx + pv.tw, pv.ty + pv.th);
        dl->AddRect(a, b, red, 0.0f, 0, 2.0f * s);
    }
    if (haveSq) {
        const ImVec2 c = toPanel(sq.x + sq.w * 0.5f, sq.y + sq.h * 0.5f);
        const float arm = 9.0f * s;
        dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), grey, 2.0f * s);
        dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), grey, 2.0f * s);
    }
    if (pv.found && haveSq && active) {
        const ImVec2 c = toPanel(pv.fx, pv.fy);
        const float arm = 11.0f * s;
        dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), green, 2.5f * s);
        dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), green, 2.5f * s);
        dl->AddCircle(c, 8.0f * s, green, 0, 1.5f * s);
    }

    char info[160];
    if (!pg.enabled.value) {
        std::snprintf(info, sizeof(info), "准心预览 · 压枪总开关已关闭");
    } else if (!pg.xTrack.value) {
        std::snprintf(info, sizeof(info), "准心预览 · 准心追踪未开启");
    } else if (!pg.continuousTrigger.value && !recoilIsFiring()) {
        std::snprintf(info, sizeof(info), "准心预览 · 等待触发（未按住触发区域）");
    } else if (pv.nCfgs == 0) {
        std::snprintf(info, sizeof(info), "准心预览 · 无配置，先开始识别");
    } else if (!haveSq) {
        std::snprintf(info, sizeof(info), "准心预览 · 等待画面…");
    } else if (pv.found) {
        std::snprintf(info, sizeof(info),
                      "准心预览 · 配置 %d/%d · %d px · 得分 %.2f",
                      pv.cfg + 1, pv.nCfgs, pv.hits, pv.score);
    } else {
        std::snprintf(info, sizeof(info), "准心预览 · 未识别到准星");
    }
    text(info, r.x + 8.0f * s, r.y + r.h - tsize(kSmallS * s) * 1.4f * xf.scale,
         TextMuted, kSmallS);
}

/// The whole 准心识别 layout, left and right columns. Returns the taller of
/// the two column ends.
float drawCrosshairMode(ImDrawList* dl, float x, float y, float w,
                        float s, float es, const Xf& xf, Scroll& sc) {
    if (crosshair::sampling()) {
        crosshair::pumpSampling();
    }
    PageRecoil& pg = g_pageRecoil;
    ImFont* font = ImGui::GetFont();
    const float gap    = csize(kRowGap) * s;
    const float colGap = csize(kColGap) * s;
    const float rowSw  = csize(widgets::kSwitchRowH) * s;
    const float rowSl  = csize(widgets::kSliderRowH) * s;
    const float fieldH = csize(kFieldH) * s;
    const float btnH   = csize(kBtnH) * s;
    const float leftW  = (w - colGap) * kLeftShare;
    const float rightW = w - colGap - leftW;
    auto wRect = [&](float wx, float wy, float ww, float wh) {
        const ImVec2 p = xf.pt(wx, wy);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    // ── LEFT ──
    float yL = y;
    dl->AddText(font, tsize(kTextS * s) * xf.scale,
                xf.pt(ImVec2(x, sc.screenY(yL))), xf.col(TextMuted), "配置名");
    yL += csize(kTextS + kLabelGap) * s;

    const widgets::Rect fieldR = wRect(x, sc.screenY(yL), leftW, fieldH);
    widgets::textField(dl, fieldR, pg.nameBuf, "例如 红点 AKM", es);
    if (pg.nameBuf.focused) {
        if (!ime::isOpen()) ime::open(&pg.nameBuf);
    } else if (ime::isOpen() && ime::field() == &pg.nameBuf) {
        ime::close();
    }
    yL += fieldH + gap;

    // 开始识别 / 取消 — one button whose meaning follows the sampler's state.
    {
        const char* name = "%.*s";
        char buf[64];
        std::snprintf(buf, sizeof(buf), name, pg.nameBuf.len, pg.nameBuf.buf);
        const widgets::Rect br = wRect(x, sc.screenY(yL), leftW, btnH);
        if (crosshair::sampling()) {
            if (widgets::button(dl, br, "取消识别", widgets::ButtonVariant::Ghost, es)) {
                crosshair::abortSample();
            }
        } else if (widgets::button(dl, br, "开始识别", widgets::ButtonVariant::Primary, es)) {
            std::string err;
            if (!crosshair::startSample(buf, err)) {
                notify::error("", err.c_str());
            } else {
                if (!g_pageCapture.enabled.value) {
                    g_pageCapture.enabled.value = true;
                    capture::setEnabled(true);
                    notify::info("capture.auto", "采样需要画面，已自动开启截图", 4.0f);
                }
                notify::info("", "识别中：把准星对准画面中心，保持 1.5 秒不动");
            }
        }
        yL += btnH + gap;
    }

    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.xTrack,
                          "准心追踪", es);
    if (pg.xTrack.value != crosshair::tracking()) {
        crosshair::setTracking(pg.xTrack.value);
    }
    yL += rowSw + gap;

    // ── 持续触发开关（默认打开，关掉时显示触发区域） ──
    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.continuousTrigger,
                          "持续触发", es);
    yL += rowSw + gap;

    if (!pg.continuousTrigger.value) {
        widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.fireToggle,
                              "触发区域", es);
        yL += rowSw + gap;
    }

    dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                xf.pt(ImVec2(x, sc.screenY(yL))), xf.col(TextMuted),
                "准星识别作为自瞄真实准心，无需额外压枪参数");
    yL += csize(kSmallS + kLabelGap + 4.0f) * s;

    // Status: the sampler's last word, then the tracker's.
    {
        std::string st;
        if (crosshair::sampling()) {
            char b[96];
            std::snprintf(b, sizeof(b), "采样中… %d%%",
                          (int)(crosshair::sampleProgress() * 100.0f));
            st = b;
        } else if (!crosshair::sampleStatus().empty()) {
            st = crosshair::sampleStatus();
        } else if (crosshair::configCount() == 0) {
            st = "还没有准星配置。填好配置名后点「开始识别」。";
        } else {
            st = "已加载 " + std::to_string(crosshair::configCount()) + " 个准星配置";
        }
        const float ts = tsize(kSmallS * s) * xf.scale;
        float ty = sc.screenY(yL);
        size_t start = 0;
        while (start <= st.size()) {
            const size_t nl = st.find('\n', start);
            const std::string line = st.substr(
                start, (nl == std::string::npos ? st.size() : nl) - start);
            dl->AddText(font, ts, xf.pt(ImVec2(x, ty)), xf.col(TextMuted), line.c_str());
            ty += ts * 1.35f;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        yL = ty + sc.offset + gap;
    }

    // ── RIGHT ── the crosshair-config list + the live preview.
    float yR = y;
    const float rx = x + leftW + colGap;
    {
        // Rebuild the catalogue every frame: names come back by value from
        // crosshair::configNames(), and the pointer table must reference this
        // frame's strings.
        pg.xNames = crosshair::configNames();
        pg.xNamePtrs.clear();
        pg.xNamePtrs.reserve(pg.xNames.size());
        for (const std::string& n : pg.xNames) pg.xNamePtrs.push_back(n.c_str());
        if (pg.xPick.value >= static_cast<int>(pg.xNamePtrs.size())) pg.xPick.value = -1;
    }
    widgets::dropdown(dl, wRect(rx, sc.screenY(yR), rightW, csize(widgets::kDropdownRowH) * s),
                      pg.xPick, "准星配置", pg.xNamePtrs.empty() ? nullptr : pg.xNamePtrs.data(),
                      static_cast<int>(pg.xNamePtrs.size()), es, false);
    yR += csize(widgets::kDropdownRowH) * s + gap;

    // 删除该配置 — only for a real selection; the config file is removed and
    // the catalogue rebuilds itself next frame.
    if (pg.xPick.value >= 0 && pg.xPick.value < static_cast<int>(pg.xNames.size())) {
        const widgets::Rect delR = wRect(rx, sc.screenY(yR), rightW, csize(kBtnH) * s);
        if (widgets::button(dl, delR, "删除该配置", widgets::ButtonVariant::Danger, es)) {
            const std::string name = pg.xNames[pg.xPick.value];
            crosshair::deleteConfig(name);
            pg.xPick.value = -1;
            LOGI("recoil: crosshair config '%s' deleted", name.c_str());
        }
        yR += csize(kBtnH) * s + gap;
    }

    {
        const float ph = std::clamp(rightW * 1.1f, 200.0f * s, 460.0f * s);
        drawCrosshairPreview(dl, wRect(rx, sc.screenY(yR), rightW, ph), s, xf);
        yR += ph + gap;
    }

    if (ime::isOpen()) yL += ime::height(s);

    // The crosshair-config dropdown's deferred option list, painted after the
    // preview so it floats above it (same ordering contract as the pattern
    // mode).
    widgets::dropdownList(dl, wRect(rx, sc.screenY(y), rightW,
                                    csize(widgets::kDropdownRowH) * s),
                          pg.xPick, pg.xNamePtrs.empty() ? nullptr : pg.xNamePtrs.data(),
                          static_cast<int>(pg.xNamePtrs.size()), es);
    return std::max(yL, yR);
}

/// 无畏契约智能压枪 UI 布局 (两列)
float drawValorantMode(ImDrawList* dl, float x, float y, float w,
                       float s, float es, const Xf& xf, Scroll& sc) {
    PageRecoil& pg = g_pageRecoil;
    ImFont* font = ImGui::GetFont();
    const float gap    = csize(kRowGap) * s;
    const float colGap = csize(kColGap) * s;
    const float rowSw  = csize(widgets::kSwitchRowH) * s;
    const float rowSl  = csize(widgets::kSliderRowH) * s;
    const float rowDd  = csize(widgets::kDropdownRowH) * s;
    const float leftW  = (w - colGap) * kLeftShare;
    const float rightW = w - colGap - leftW;
    auto wRect = [&](float wx, float wy, float ww, float wh) {
        const ImVec2 p = xf.pt(wx, wy);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    // ── LEFT: 武器选择 + 开火区域复用 + 分段控制参数 ──
    float yL = y;

    static const char* kWeapons[] = {"狂徒 (Vandal)", "幻影 (Phantom)", "自定义"};
    const int oldW = pg.vWeapon.value;
    const float yWeaponDdTop = yL;
    widgets::dropdown(dl, wRect(x, sc.screenY(yL), leftW, rowDd), pg.vWeapon,
                      "武器预设", kWeapons, 3, es, /*drawListInline=*/false);
    yL += rowDd + gap;

    // 切换预设时自动填入参考参数 (已应用 0.6x 默认微调)
    if (pg.vWeapon.value != oldW) {
        if (pg.vWeapon.value == 0) { // 狂徒 (9.75rps: 前5发阶段一, 紧接3发阶段二)
            pg.vStage1Px.value = 66.0f;
            pg.vStage2Px.value = 90.0f;
            pg.vResetMs.value = 300.0f;
            pg.vReturnSpeed.value = 220.0f;
        } else if (pg.vWeapon.value == 1) { // 幻影 (11rps: 前6发阶段一, 紧接3发阶段二)
            pg.vStage1Px.value = 57.0f;
            pg.vStage2Px.value = 78.0f;
            pg.vResetMs.value = 250.0f;
            pg.vReturnSpeed.value = 220.0f;
        }
    }

    // 持续触发与触发区域开关
    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.continuousTrigger,
                          "持续触发", es);
    yL += rowSw + gap;

    if (!pg.continuousTrigger.value) {
        widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.fireToggle,
                              "触发区域", es);
        yL += rowSw + gap;
    }

    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.vReturnHome,
                          "停火平滑回中", es);
    yL += rowSw + gap;

    // 参数微调
    dl->AddText(font, tsize(kTextS * s) * xf.scale,
                xf.pt(ImVec2(x, sc.screenY(yL))), xf.col(TextMuted), "分段行程控制 (独立调节)");
    yL += csize(kTextS + kLabelGap) * s;

    const char* s1Label = (pg.vWeapon.value == 1) ? "一阶段下压 (前6发)" : "一阶段下压 (前5发)";
    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.vStage1Px,
                         s1Label, 0, es);
    yL += rowSl + gap;

    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.vStage2Px,
                         "二阶段下压 (大跳变·压3发)", 0, es);
    yL += rowSl + gap;

    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.vReturnSpeed,
                         "停火回正速度", 0, es);
    yL += rowSl + gap;

    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.vResetMs,
                         "后坐重置窗口", 0, es);
    yL += rowSl + gap;

    // ── RIGHT: 实时运行状态与弹道分段说明卡片 ──
    float yR = y;
    const float rx = x + leftW + colGap;

    const valorant_recoil::Status st = valorant_recoil::currentStatus();
    const bool isFiring = firingNow();

    // 状态监视卡片
    const float cardH = 150.0f * s;
    const widgets::Rect cardR = wRect(rx, sc.screenY(yR), rightW, cardH);
    dl->AddRectFilled(ImVec2(cardR.x, cardR.y), ImVec2(cardR.x + cardR.w, cardR.y + cardR.h),
                      xf.col(ControlBg), 8.0f * s);
    dl->AddRect(ImVec2(cardR.x, cardR.y), ImVec2(cardR.x + cardR.w, cardR.y + cardR.h),
                xf.col(isFiring ? Accent : Edge), 8.0f * s, 0, 1.5f * s);

    float cy = yR + 14.0f * s;
    char buf[128];
    if (isFiring) {
        const char* phaseDesc = "阶段一: 前段微调 (首段平滑)";
        if (st.phase == 2) phaseDesc = "阶段二: 大跳变段 (压3发)";
        else if (st.phase == 3) phaseDesc = "阶段三: 垂直保持住 (锁死恒定)";
        std::snprintf(buf, sizeof(buf), "状态: 开火中 · 第 %d 发", st.currentShot);
        dl->AddText(font, tsize(kTextS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(cy))), xf.col(Accent), buf);
        cy += 28.0f * s;
        dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(cy))), xf.col(TextPrimary), phaseDesc);
        cy += 24.0f * s;
        std::snprintf(buf, sizeof(buf), "当前下压: %.1f px (目标上限: %.1f px)",
                      st.currentY, st.totalTargetY);
        dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(cy))), xf.col(TextMuted), buf);
        cy += 24.0f * s;
        std::snprintf(buf, sizeof(buf), "开火时长: %.2f 秒", st.burstDurationSec);
        dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(cy))), xf.col(TextMuted), buf);
    } else {
        dl->AddText(font, tsize(kTextS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(cy))), xf.col(TextMuted), "状态: 待机中 (未开火)");
        cy += 28.0f * s;
        std::snprintf(buf, sizeof(buf), "总下压量: %.0f px + %.0f px = %.0f px",
                      pg.vStage1Px.value, pg.vStage2Px.value, pg.vStage1Px.value + pg.vStage2Px.value);
        dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(cy))), xf.col(TextPrimary), buf);
        cy += 24.0f * s;
        dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(cy))), xf.col(TextMuted), "开火区域已激活，按下即可自动压枪");
        cy += 24.0f * s;
        dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(cy))), xf.col(TextMuted), "开火区域调试完毕关闭显示即可");
    }
    yR += cardH + gap;

    // 机制说明卡片
    const float guideH = 210.0f * s;
    const widgets::Rect guideR = wRect(rx, sc.screenY(yR), rightW, guideH);
    dl->AddRectFilled(ImVec2(guideR.x, guideR.y), ImVec2(guideR.x + guideR.w, guideR.y + guideR.h),
                      xf.col(ControlBg), 8.0f * s);
    dl->AddRect(ImVec2(guideR.x, guideR.y), ImVec2(guideR.x + guideR.w, guideR.y + guideR.h),
                xf.col(Edge), 8.0f * s, 0, 1.0f * s);

    float gy = yR + 14.0f * s;
    dl->AddText(font, tsize(kTextS * s) * xf.scale,
                xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(gy))), xf.col(TextPrimary), "双阶段智能压枪说明：");
    gy += 28.0f * s;
    const char* tips[] = {
        "1. 狂徒前5发 / 幻影前6发垂直较小，由一阶段控制。",
        "2. 一阶段二阶贝塞尔，二阶段三阶贝塞尔平滑衔接。",
        "3. 二阶段压完后垂直彻底锁死，停火回正速度可调。",
        "4. 开火区域仅调整时开启，实战关闭后仍生效。",
        "5. 阶段时间根据射速（狂徒9.75 / 幻影11）自动推导。"
    };
    for (const char* tip : tips) {
        dl->AddText(font, tsize(kSmallS * s) * xf.scale,
                    xf.pt(ImVec2(rx + 16.0f * s, sc.screenY(gy))), xf.col(TextMuted), tip);
        gy += 22.0f * s;
    }
    yR += guideH + gap;

    // 武器预设下拉框列表延后绘制 (确保浮在最上层)
    widgets::dropdownList(dl, wRect(x, sc.screenY(yWeaponDdTop), leftW, rowDd), pg.vWeapon,
                          kWeapons, 3, es);

    return std::max(yL, yR);
}

}  // namespace

// ── The page ────────────────────────────────────────────────────────────────

void drawRecoilSection(ImDrawList* dl, float x, float& y, float w,
                       float bottomY, float s, float es, const Xf& xf, Scroll& sc) {
    PageRecoil& pg = g_pageRecoil;
    refreshCatalog(pg);

    ImFont* font = ImGui::GetFont();
    const float gap     = csize(kRowGap) * s;
    const float colGap  = csize(kColGap) * s;
    const float rowSw   = csize(widgets::kSwitchRowH) * s;
    const float rowSl   = csize(widgets::kSliderRowH) * s;
    const float rowDd   = csize(widgets::kDropdownRowH) * s;
    const float fieldH  = csize(kFieldH) * s;
    const float btnH    = csize(kBtnH) * s;

    const float leftW  = (w - colGap) * kLeftShare;
    const float rightW = w - colGap - leftW;

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        const ImVec2 p = xf.pt(wx, wy);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    float yL = y;
    float yR = y;

    // ── 压枪模式: full-width dropdown right under the title ─────────────────
    static const char* kModeNames[] = {"准心识别", "无畏契约智能压枪"};
    const float yModeTop = y;
    {
        widgets::dropdown(dl, wRect(x, sc.screenY(y), w, rowDd), pg.mode,
                          "压枪模式", kModeNames, 2, es, /*drawListInline=*/false);
        const int m = pg.mode.value;
        // The deferred list must be painted on every exit path — it is the
        // only thing that makes the dropdown's own rect show options.
        if (m == PageRecoil::kModeCrosshair) {
            y = drawCrosshairMode(dl, x, y + rowDd + gap, w, s, es, xf, sc);
            widgets::dropdownList(dl, wRect(x, sc.screenY(yModeTop), w, rowDd), pg.mode,
                                  kModeNames, 2, es);
            return;
        }
        if (m == PageRecoil::kModeValorant) {
            y = drawValorantMode(dl, x, y + rowDd + gap, w, s, es, xf, sc);
            widgets::dropdownList(dl, wRect(x, sc.screenY(yModeTop), w, rowDd), pg.mode,
                                  kModeNames, 2, es);
            return;
        }
        y += rowDd + gap;
        yL = y;
        yR = y;
    }
    // ═══════════════════════════ LEFT: the scalars ══════════════════════════

    // ── Profile name + recorder ─────────────────────────────────────────────
    dl->AddText(font, tsize(kTextS * s) * xf.scale,
                xf.pt(ImVec2(x, sc.screenY(yL))), xf.col(TextMuted), "配置名");
    yL += csize(kTextS + kLabelGap) * s;

    const widgets::Rect fieldR = wRect(x, sc.screenY(yL), leftW, fieldH);
    widgets::textField(dl, fieldR, pg.nameBuf, "例如 AKM", es);
    // The field's focus, not the keyboard's state, decides whether the keyboard
    // is up. A tap on the scrim lowers the keyboard while the field keeps its
    // focus, and re-opening on "focus flipped" alone would then leave the field
    // focused with no way to type into it.
    if (pg.nameBuf.focused) {
        if (!ime::isOpen()) ime::open(&pg.nameBuf);
    } else if (ime::isOpen() && ime::field() == &pg.nameBuf) {
        ime::close();
    }
    yL += fieldH + gap;

    // Record / stop, side by side. The right-hand button changes label with the
    // recorder's state rather than being a pair of independent switches.
    {
        const float bw = (leftW - gap) * 0.5f;
        const recoil::RecState rs = recoil::state();
        // Busy covers the whole time the recorder owns the page — armed,
        // measuring, and crunching. The two buttons are a pair whose meaning
        // depends on that, so they are drawn as a pair rather than as two
        // independent switches whose labels happen to coincide.
        const bool busy = (rs == recoil::RecState::Armed ||
                           rs == recoil::RecState::Recording ||
                           rs == recoil::RecState::Analyzing);
        const bool measuring = (rs == recoil::RecState::Armed ||
                                rs == recoil::RecState::Recording);

        const widgets::Rect r1 = wRect(x, sc.screenY(yL), bw, btnH);
        const widgets::Rect r2 = wRect(x + bw + gap, sc.screenY(yL), bw, btnH);

        char name[64];
        std::snprintf(name, sizeof(name), "%.*s", pg.nameBuf.len, pg.nameBuf.buf);

        if (busy) {
            if (widgets::button(dl, r1, "取消", widgets::ButtonVariant::Ghost, es)) {
                recoil::abort();
            }
            // 结束录制 is inert while the analysis is already running — there is
            // nothing left to stop, and a live-looking button that does nothing
            // is worse than one that says so.
            if (widgets::button(dl, r2, "结束录制",
                                measuring ? widgets::ButtonVariant::Danger
                                          : widgets::ButtonVariant::Disabled, es)) {
                recoil::stopAndAnalyze();
            }
        } else {
            if (widgets::button(dl, r1, "开始录制", widgets::ButtonVariant::Primary, es)) {
                // Every outcome posts a toast, not just the status line: a
                // refusal used to surface only as one small line at the bottom
                // of the left column, which reads as "点了没反应".
                if (!recoil::validStem(name)) {
                    pg.seedHint = "配置名只能有字母数字下划线中文，且不能为空";
                    notify::error("", pg.seedHint.c_str());
                } else if (!pg.marker.placed) {
                    pg.seedHint = "先打开「标记框」并把它拖到墙上高对比、不重复花纹的位置";
                    notify::warn("", pg.seedHint.c_str());
                } else {
                    const ImVec2 d = ImGui::GetIO().DisplaySize;
                    const recoil::MarkerBox mb{pg.marker.x, pg.marker.y, pg.marker.size,
                                               pg.marker.placed};
                    if (recoil::start(name, pg.fov.value, mb, static_cast<int>(d.x),
                                      static_cast<int>(d.y))) {
                        pg.seedHint.clear();
                        notify::info("", "开始录制：打完整梭再点结束录制");
                    } else {
                        pg.seedHint = recoil::stats().message;
                        notify::error("", pg.seedHint.c_str());
                    }
                }
            }
            // Drawn disabled rather than omitted: the pair stays in the same
            // place in every state, so the finger that just stopped a take does
            // not have to find a moved target.
            widgets::button(dl, r2, "结束录制", widgets::ButtonVariant::Disabled, es);
        }
        yL += btnH + gap;
    }

    // ── The two travel knobs ────────────────────────────────────────────────
    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.travelX,
                         "整梭行程·水平 (finger px)", 0, es);
    yL += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.travelY,
                         "整梭行程·垂直 (finger px)", 0, es);
    yL += rowSl + gap;

    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.weightX,
                         "水平轴权重", 2, es);
    yL += rowSl + gap;
    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.weightY,
                         "垂直轴权重", 2, es);
    yL += rowSl + gap;

    widgets::sliderFloat(dl, wRect(x, sc.screenY(yL), leftW, rowSl), pg.fov,
                         "录制用 FOV (度)", 0, es);
    yL += rowSl + gap;

    // ── Switches ────────────────────────────────────────────────────────────
    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.flipX,
                          "左右翻转", es);
    yL += rowSw + gap;
    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.flipY,
                          "上下翻转", es);
    yL += rowSw + gap;
    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.editVertical,
                          "微调垂直轴", es);
    yL += rowSw + gap;

    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.fireToggle,
                          "显示开火区域", es);
    yL += rowSw + gap;
    widgets::switchButton(dl, wRect(x, sc.screenY(yL), leftW, rowSw), pg.markerToggle,
                          "显示标记框", es);
    yL += rowSw + gap;

    // ── Status ──────────────────────────────────────────────────────────────
    {
        recoil::Profile* p = activeProfile(pg);
        pg.status.clear();
        if (p == nullptr) {
            pg.status = "没有档案。把你的录屏丢给 PC 上的 recoil-analyser，"
                        "或直接在这里录制一段。";
        } else {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "%s · %s · %d 发 · 水平 %.2f° / 垂直 %.2f°",
                          p->name.c_str(), p->recoilClass.c_str(), p->shots,
                          p->dxPeak, p->dyPeak);
            pg.status = buf;
            if (!p->drivable()) {
                pg.status += "\n该档案不满足驱动条件（需要 standard 类且标记质量足够）。";
            }
        }
        if (!pg.seedHint.empty()) pg.status += "\n" + pg.seedHint;

        const float ts = tsize(kSmallS * s) * xf.scale;
        float ty = sc.screenY(yL);
        size_t start = 0;
        while (start <= pg.status.size()) {
            const size_t nl = pg.status.find('\n', start);
            const std::string line = pg.status.substr(
                start, (nl == std::string::npos ? pg.status.size() : nl) - start);
            dl->AddText(font, ts, xf.pt(ImVec2(x, ty)), xf.col(TextMuted), line.c_str());
            ty += ts * 1.35f;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        yL = ty + sc.offset + gap;
    }

    // ═══════════════════════════ RIGHT: profile + pattern ═══════════════════
    const float rx = x + leftW + colGap;

    widgets::dropdown(dl, wRect(rx, sc.screenY(yR), rightW, rowDd), pg.pick,
                      "档案", pg.namePtrs.empty() ? nullptr : pg.namePtrs.data(),
                      static_cast<int>(pg.namePtrs.size()), es,
                      /*drawListInline=*/false);
    if (pg.pick.value != pg.loaded && pg.pick.value >= 0 &&
        pg.pick.value < static_cast<int>(pg.profiles.size())) {
        pg.loaded = pg.pick.value;
        pg.dragShot = -1;
        pg.dragActive = false;
    }
    yR += rowDd + gap;

    {
        const float chartH = std::clamp(rightW * 1.9f, 240.0f * s, 620.0f * s);
        const widgets::Rect chartR = wRect(rx, sc.screenY(yR), rightW, chartH);
        drawPatternChart(dl, chartR, s, xf, pg);
        yR += chartH + gap;
    }

    {
        const float bw = (rightW - gap) * 0.5f;
        recoil::Profile* p = activeProfile(pg);
        if (widgets::button(dl, wRect(rx, sc.screenY(yR), bw, btnH), "恢复",
                            widgets::ButtonVariant::Ghost, es)) {
            if (p != nullptr) {
                p->resetTune();
                p->saveTune(p->name);
                pg.seedHint = "已恢复为测量值";
            }
        }
        if (widgets::button(dl, wRect(rx + bw + gap, sc.screenY(yR), bw, btnH), "保存",
                            widgets::ButtonVariant::Primary, es)) {
            if (p != nullptr) {
                p->saveTune(p->name);
                pg.seedHint = "微调已写入 " + p->name + ".tune.json";
            }
        }
        yR += btnH + gap;
    }

    // The deferred dropdown list, painted after everything else on the page —
    // the same ordering fix the Touch page's device row uses.
    {
        const float rowTop = y;   // the dropdown is the first right-column row
        widgets::dropdownList(dl, wRect(rx, sc.screenY(rowTop), rightW, rowDd), pg.pick,
                              pg.namePtrs.empty() ? nullptr : pg.namePtrs.data(),
                              static_cast<int>(pg.namePtrs.size()), es);
    }
    // The mode dropdown's list too: the pattern path falls through here rather
    // than returning early, and without this the tap toggles st.open but the
    // options never appear — the dropdown reads as dead.
    widgets::dropdownList(dl, wRect(x, sc.screenY(yModeTop), w, rowDd), pg.mode,
                          kModeNames, 2, es);

    // Room for the keyboard, so the field it serves can always be scrolled into
    // view instead of hiding underneath it.
    if (ime::isOpen()) {
        const float kh = ime::height(s);
        yL += kh;
        yR += kh;
    }

    y = std::max(yL, yR);
    (void)bottomY;
}

// ── Overlays ────────────────────────────────────────────────────────────────

namespace {

/// Where on the fire circle did the finger land? Handles first — a finger near
/// a handle is inherently inside the circle too, and the handle is the more
/// specific intent. Same interaction as the Trigger page's 触发区域.
RecoilFireArea::Drag fireHitTest(const RecoilFireArea& a, const ImVec2& m, float s) {
    using D = RecoilFireArea::Drag;
    const float hr = 36.0f * s;
    const float dx = m.x - a.cx, dy = m.y - a.cy;
    auto near = [&](float hx, float hy) {
        const float ex = m.x - hx, ey = m.y - hy;
        return ex * ex + ey * ey <= hr * hr;
    };
    if (near(a.cx, a.cy - a.r)) return D::ResizeN;
    if (near(a.cx + a.r, a.cy)) return D::ResizeE;
    if (near(a.cx, a.cy + a.r)) return D::ResizeS;
    if (near(a.cx - a.r, a.cy)) return D::ResizeW;
    if (dx * dx + dy * dy <= a.r * a.r) return D::Move;
    return D::None;
}

void applyFireDrag(RecoilFireArea& a, const ImVec2& m) {
    using D = RecoilFireArea::Drag;
    if (a.drag == D::Move) {
        a.cx = a.startCX + (m.x - a.anchorX);
        a.cy = a.startCY + (m.y - a.anchorY);
        return;
    }
    if (a.drag == D::ResizeN || a.drag == D::ResizeE ||
        a.drag == D::ResizeS || a.drag == D::ResizeW) {
        const float ndx = m.x - a.cx, ndy = m.y - a.cy;
        float nr = std::sqrt(ndx * ndx + ndy * ndy);
        if (nr < 60.0f) nr = 60.0f;
        a.r = nr;
    }
}

}  // namespace

void recoilSwallowRects(std::vector<int>& out) {
    PageRecoil& pg = g_pageRecoil;
    // Only the marker box. Touches on it are consumed by the overlay — the
    // ImGui drag needs them — and the mirror swallows the rest, which is what
    // stops "drag the marker" from also dragging the camera. The fire circle
    // must never be swallowed: the finger on the game's fire button has to
    // reach the game, or nothing fires at all.
    if (!pg.markerToggle.value || !pg.marker.placed) return;
    out.push_back(static_cast<int>(pg.marker.x));
    out.push_back(static_cast<int>(pg.marker.y));
    out.push_back(static_cast<int>(pg.marker.size));
    out.push_back(static_cast<int>(pg.marker.size));
}

void drawRecoilOverlays() {
    PageRecoil& pg = g_pageRecoil;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float s = hudScale();
    const float thick = std::max(1.0f, 3.0f * s);

    // ── Tracked crosshair marker (准心识别 mode) ─────────────────────────────
    // A purple cross pinned to where the recogniser sees the real crosshair,
    // so "is it tracking" is answerable at a glance on the game itself.
    if (pg.enabled.value && pg.mode.value == PageRecoil::kModeCrosshair && crosshair::tracking()) {
        crosshair::Preview pv;
        crosshair::preview(pv);
        if (pv.found) {
            const ImVec2 c(pv.fx, pv.fy);
            const ImU32 col = IM_COL32(190, 90, 255, 235);   // purple
            const float arm = 14.0f * s;
            const float gap = 4.0f * s;   // hollow centre: don't cover the crosshair
            auto seg = [&](const ImVec2& a, const ImVec2& b) {
                dl->AddLine(a, b, col, std::max(1.5f, 2.5f * s));
            };
            seg(ImVec2(c.x - arm, c.y), ImVec2(c.x - gap, c.y));
            seg(ImVec2(c.x + gap, c.y), ImVec2(c.x + arm, c.y));
            seg(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y - gap));
            seg(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + arm));
            dl->AddCircle(c, arm + 3.0f * s, col, 0, 1.2f * s);
        }
    }

    // ── Trigger area (circle, same look as the Trigger page's 触发区域) ───────
    if (!pg.continuousTrigger.value && pg.fireToggle.value) {
        if (!pg.fire.placed) {
            pg.fire.cx = io.DisplaySize.x * 0.5f;
            pg.fire.cy = io.DisplaySize.y * 0.72f;
            pg.fire.placed = true;
        }
        const ImVec2 c(pg.fire.cx, pg.fire.cy);
        const bool live = firingNow();
        const ImU32 line = live ? IM_COL32(255, 120, 60, 245) : kFireLine;
        dl->AddCircleFilled(c, pg.fire.r, live ? IM_COL32(255, 120, 60, 40) : kFireFill, 64);
        dl->AddCircle(c, pg.fire.r, line, 64, thick);
        dl->AddCircleFilled(ImVec2(c.x, c.y - pg.fire.r), 9.0f * s, line, 12);
        dl->AddCircleFilled(ImVec2(c.x + pg.fire.r, c.y), 9.0f * s, line, 12);
        dl->AddCircleFilled(ImVec2(c.x, c.y + pg.fire.r), 9.0f * s, line, 12);
        dl->AddCircleFilled(ImVec2(c.x - pg.fire.r, c.y), 9.0f * s, line, 12);
        {
            ImFont* font = ImGui::GetFont();
            const char* label = live ? "触发区域 · 生效中" : "触发区域";
            const float fs = tsize(22.0f * s);
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
            dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                        line, label);
        }

        if (!widgets::gestureConsumed() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const RecoilFireArea::Drag d = fireHitTest(pg.fire, io.MousePos, s);
            if (d != RecoilFireArea::Drag::None) {
                pg.fire.drag = d;
                pg.fire.anchorX = io.MousePos.x;
                pg.fire.anchorY = io.MousePos.y;
                pg.fire.startCX = pg.fire.cx;
                pg.fire.startCY = pg.fire.cy;
                pg.fire.startR = pg.fire.r;
                widgets::setWidgetDragging();
                widgets::consumeGesture();
            }
        }
        if (pg.fire.drag != RecoilFireArea::Drag::None) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                applyFireDrag(pg.fire, io.MousePos);
                // Keep the whole circle on screen, like the Trigger hold area.
                if (pg.fire.cx - pg.fire.r < 0.0f) pg.fire.cx = pg.fire.r;
                if (pg.fire.cy - pg.fire.r < 0.0f) pg.fire.cy = pg.fire.r;
                if (pg.fire.cx + pg.fire.r > io.DisplaySize.x)
                    pg.fire.cx = io.DisplaySize.x - pg.fire.r;
                if (pg.fire.cy + pg.fire.r > io.DisplaySize.y)
                    pg.fire.cy = io.DisplaySize.y - pg.fire.r;
            } else {
                pg.fire.drag = RecoilFireArea::Drag::None;
                widgets::clearWidgetDragging();
            }
        }
    }

    // ── The capture square, and the marker inside it ────────────────────────
    if (pg.markerToggle.value) {
        widgets::Rect sq;
        if (captureSquare(sq)) {
            dashedRect(dl, ImVec2(sq.x, sq.y), ImVec2(sq.x + sq.w, sq.y + sq.h),
                       kSquareLine, 10.0f * s, std::max(1.0f, 2.0f * s));
        }

        if (!pg.marker.placed) {
            // Default to the middle of the capture square: that is the only
            // place the recorder is guaranteed to be able to see.
            if (sq.w > 0.0f) {
                pg.marker.x = sq.x + sq.w * 0.5f - pg.marker.size * 0.5f;
                pg.marker.y = sq.y + sq.h * 0.5f - pg.marker.size * 0.5f;
            } else {
                pg.marker.x = io.DisplaySize.x * 0.5f - pg.marker.size * 0.5f;
                pg.marker.y = io.DisplaySize.y * 0.42f;
            }
            pg.marker.placed = true;
        }

        const ImVec2 mn(pg.marker.x, pg.marker.y);
        const ImVec2 mx(pg.marker.x + pg.marker.size, pg.marker.y + pg.marker.size);
        const bool inline_ = sq.w > 0.0f &&
                             mn.x >= sq.x && mn.y >= sq.y &&
                             mx.x <= sq.x + sq.w && mx.y <= sq.y + sq.h;
        const ImU32 line = inline_ ? kMarkerLine : IM_COL32(255, 80, 80, 240);

        dl->AddRectFilled(mn, mx, kMarkerFill);
        dashedRect(dl, mn, mx, line, 7.0f * s, thick);
        // The patch the correlation actually uses is inset from this outline —
        // drawn so the inset is visible rather than implied.
        const float inset = recoil::kPatchInset * s;
        dl->AddRect(ImVec2(mn.x + inset, mn.y + inset), ImVec2(mx.x - inset, mx.y - inset),
                    line, 0.0f, 0, std::max(1.0f, 1.5f * s));
        dl->AddText(ImGui::GetFont(), tsize(20.0f * s),
                    ImVec2(mn.x, mn.y - 24.0f * s), line,
                    inline_ ? "标记" : "标记 · 不在采样方形内");

        if (!widgets::gestureConsumed() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (io.MousePos.x >= mn.x && io.MousePos.x <= mx.x &&
                io.MousePos.y >= mn.y && io.MousePos.y <= mx.y) {
                pg.marker.dragging = true;
                pg.marker.anchorX = io.MousePos.x;
                pg.marker.anchorY = io.MousePos.y;
                pg.marker.startX = pg.marker.x;
                pg.marker.startY = pg.marker.y;
                widgets::setWidgetDragging();
                widgets::consumeGesture();
            }
        }
        if (pg.marker.dragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                pg.marker.x = pg.marker.startX + (io.MousePos.x - pg.marker.anchorX);
                pg.marker.y = pg.marker.startY + (io.MousePos.y - pg.marker.anchorY);
            } else {
                pg.marker.dragging = false;
                widgets::clearWidgetDragging();
            }
        }
    }
}

// ── The recording / analysing card ──────────────────────────────────────────

namespace {

void drawRecoilDialog(ImDrawList* dl, float s, float es, const Xf& xf) {
    const recoil::RecStats& st = recoil::stats();
    PageRecoil& pg = g_pageRecoil;
    const HudRect board = hudRect();

    widgets::Rect card = widgets::dialogRect(board, es);
    const ImVec2 mn(card.x, card.y), mx(card.x + card.w, card.y + card.h);

    dl->AddRectFilled(ImVec2(board.x, board.y),
                      ImVec2(board.x + board.w, board.y + board.h),
                      xf.col(IM_COL32(0, 0, 0, 180)));
    dl->AddRectFilled(xf.pt(mn), xf.pt(mx), xf.col(PaneRight), xf.s(20.0f * s));
    dl->AddRect(xf.pt(mn), xf.pt(mx), xf.col(Edge), xf.s(20.0f * s), 0, xf.s(1.0f * s));

    ImFont* font = ImGui::GetFont();
    const float padX = widgets::dialogInnerPad(es);
    const float titleH = widgets::dialogHeaderHeight(es);

    const char* title = "录制";
    switch (st.state) {
        case recoil::RecState::Armed:     title = "等待采集画面"; break;
        case recoil::RecState::Recording: title = "录制中"; break;
        case recoil::RecState::Analyzing: title = "分析中"; break;
        case recoil::RecState::Done:      title = "分析结果"; break;
        case recoil::RecState::Failed:    title = "失败"; break;
        default: break;
    }
    dl->AddText(font, tsize(32.0f * s) * xf.scale,
                ImVec2(mn.x + padX, mn.y + (titleH - 32.0f * s) * 0.5f),
                xf.col(TextPrimary), title);

    float y = mn.y + titleH + 12.0f * s;
    const float innerW = card.w - padX * 2.0f;
    const float ts = tsize(21.0f * s) * xf.scale;

    auto line = [&](const std::string& text, ImU32 col) {
        dl->AddText(font, ts, ImVec2(mn.x + padX, y), xf.col(col), text.c_str());
        y += ts * 1.45f;
    };

    if (st.state == recoil::RecState::Recording || st.state == recoil::RecState::Armed) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "已采样 %d 帧（其中可用 %d 帧） · %.1f 秒",
                      st.frames, st.good, st.seconds);
        line(buf, TextPrimary);
        // The menu must be closed to fire, so manual stop is the exception and
        // the countdown is the rule — say both, or the card reads as stuck.
        if (st.state == recoil::RecState::Recording && st.windowSec > 0.0f) {
            std::snprintf(buf, sizeof(buf), "%.0f 秒后自动结束并分析，也可现在点「结束录制」",
                          std::max(0.0f, st.windowSec - st.seconds));
            line(buf, TextPrimary);
        }
        line("关菜单开火，打完整梭。标记必须留在画面里，且不要移动视角。", TextMuted);

        // A live scope of the marker's horizontal track. It answers the one
        // question the frame count cannot: is the marker actually being
        // followed, or is the correlation sitting on noise?
        const widgets::Rect scope{mn.x + padX, y, innerW, 92.0f * s};
        dl->AddRectFilled(ImVec2(scope.x, scope.y),
                          ImVec2(scope.x + scope.w, scope.y + scope.h),
                          xf.col(ListBg), xf.s(10.0f * s));
        const int n = recoil::sampleCount();
        if (n > 1) {
            float maxAbs = 1e-4f;
            for (int i = 0; i < n; ++i) {
                float t = 0.0f, dx = 0.0f, dy = 0.0f;
                if (recoil::sampleAt(i, t, dx, dy)) maxAbs = std::max(maxAbs, std::fabs(dx));
            }
            const float midY = scope.y + scope.h * 0.5f;
            const ImU32 col = IM_COL32(120, 200, 255, 230);
            ImVec2 prev(0, 0);
            for (int i = 0; i < n; ++i) {
                float t = 0.0f, dx = 0.0f, dy = 0.0f;
                if (!recoil::sampleAt(i, t, dx, dy)) continue;
                const ImVec2 pt(scope.x + scope.w * static_cast<float>(i) /
                                             static_cast<float>(n - 1),
                                midY - (dx / maxAbs) * (scope.h * 0.42f));
                if (i > 0) dl->AddLine(prev, pt, col, 2.0f * s);
                prev = pt;
            }
        }
        y = scope.y + scope.h + 14.0f * s;
    } else if (st.state == recoil::RecState::Analyzing) {
        line(st.stage.empty() ? "处理中…" : st.stage, TextPrimary);
        const widgets::Rect bar{mn.x + padX, y + 6.0f * s, innerW, 22.0f * s};
        dl->AddRectFilled(ImVec2(bar.x, bar.y), ImVec2(bar.x + bar.w, bar.y + bar.h),
                          xf.col(Track), xf.s(11.0f * s));
        dl->AddRectFilled(ImVec2(bar.x, bar.y),
                          ImVec2(bar.x + bar.w * std::clamp(st.progress, 0.0f, 1.0f),
                                 bar.y + bar.h),
                          xf.col(Accent), xf.s(11.0f * s));
        y = bar.y + bar.h + 18.0f * s;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(st.progress * 100.0f));
        line(buf, TextMuted);
    } else if (st.state == recoil::RecState::Done) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "档案 %s · %d 发 · %s",
                      st.outStem.c_str(), st.shots, st.klass.c_str());
        line(buf, TextPrimary);
        std::snprintf(buf, sizeof(buf), "水平峰峰 %.2f° · 垂直峰峰 %.2f°", st.dxPeak, st.dyPeak);
        line(buf, TextMuted);
        std::snprintf(buf, sizeof(buf), "标记匹配 最低 %.2f / 平均 %.2f · %.0f fps",
                      st.minConf, st.meanConf, st.fps);
        line(buf, TextMuted);
        if (st.klass != "standard") {
            line("这份档案不会驱动压枪：器械类或样本质量不足。", IM_COL32(255, 150, 60, 255));
        } else {
            line("去左栏调「整梭行程」，对着墙上标记打一梭看它留不留得住。", TextMuted);
        }
    } else if (st.state == recoil::RecState::Failed) {
        line(st.message.empty() ? "未知原因" : st.message, IM_COL32(255, 110, 110, 255));
    }

    // ── Buttons ─────────────────────────────────────────────────────────────
    const float bw = 190.0f * s, bh = 58.0f * s;
    const float by = mx.y - bh - 20.0f * s;
    if (st.state == recoil::RecState::Recording || st.state == recoil::RecState::Armed) {
        widgets::Rect r1{mn.x + padX, by, bw, bh};
        widgets::Rect r2{mx.x - padX - bw, by, bw, bh};
        if (widgets::button(dl, r1, "取消", widgets::ButtonVariant::Ghost, es)) {
            recoil::abort();
        }
        if (widgets::button(dl, r2, "结束录制", widgets::ButtonVariant::Danger, es)) {
            recoil::stopAndAnalyze();
        }
    } else if (st.state == recoil::RecState::Done ||
               st.state == recoil::RecState::Failed) {
        widgets::Rect r1{mx.x - padX - bw, by, bw, bh};
        if (widgets::button(dl, r1, "知道了", widgets::ButtonVariant::Primary, es)) {
            recoil::dismiss();
            pg.catalogDirty = true;
        }
    }
    y = by;
}

}  // namespace

void drawRecoilDialogs(ImDrawList* dl, float s, float es, const Xf& xf) {
    const recoil::RecState rs = recoil::state();
    if (rs != recoil::RecState::Idle) drawRecoilDialog(dl, s, es, xf);
    // The profile-name keyboard. The page opens it (see drawRecoilSection) and
    // reserves room for it, but without this call its state just sits at
    // `open=true` with nothing on screen — which is exactly the "点了不弹输入法"
    // report. Drawn after the recording card so it floats above everything.
    if (ime::isOpen()) {
        const HudRect board = hudRect();
        if (board.valid()) {
            ime::draw(dl, ime::panelRect(board, s), s, xf);
        }
    }
}

bool anyRecoilModal() {
    if (ime::isOpen()) return true;
    return recoil::state() != recoil::RecState::Idle;
}

std::string recoilHeaderText() {
    const PageRecoil& pg = g_pageRecoil;
    if (!pg.enabled.value) return std::string();

    if (pg.mode.value == PageRecoil::kModeCrosshair) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "准心识别 · %d 配置 · %s",
                      crosshair::configCount(),
                      crosshair::tracking() ? "追踪中" : "未追踪");
        return buf;
    }
    if (pg.mode.value == PageRecoil::kModeValorant) {
        char buf[96];
        static const char* kW[] = {"狂徒", "幻影", "自定义"};
        const char* wname = (pg.vWeapon.value >= 0 && pg.vWeapon.value < 3) ? kW[pg.vWeapon.value] : "预设";
        const valorant_recoil::Status st = valorant_recoil::currentStatus();
        if (st.firing) {
            std::snprintf(buf, sizeof(buf), "无畏契约 · %s · 第 %d 发", wname, st.currentShot);
        } else {
            std::snprintf(buf, sizeof(buf), "无畏契约 · %s · (%.0f+%.0f)px",
                          wname, pg.vStage1Px.value, pg.vStage2Px.value);
        }
        return buf;
    }

    const recoil::Profile* p = nullptr;
    if (pg.loaded >= 0 && pg.loaded < static_cast<int>(pg.profiles.size())) {
        p = &pg.profiles[static_cast<size_t>(pg.loaded)];
    }

    char buf[192];
    if (p == nullptr) {
        std::snprintf(buf, sizeof(buf), "未选择档案");
    } else if (!p->drivable()) {
        // The class is what disqualifies it, so say that rather than repeat the
        // peak-to-peak numbers the page body already prints.
        std::snprintf(buf, sizeof(buf), "%s · %d 发 · 不可驱动",
                      p->name.c_str(), p->shots);
    } else {
        std::snprintf(buf, sizeof(buf), "%s · %d 发 · 水平 %.2f° / 垂直 %.2f°",
                      p->name.c_str(), p->shots, p->dxPeak, p->dyPeak);
    }
    return buf;
}

// ── Per-frame drive ─────────────────────────────────────────────────────────

void syncRecoilPage() {
    PageRecoil& pg = g_pageRecoil;

    // The recorder is a third reader of frames, alongside the preview and
    // inference. Announcing it here is what keeps the shell's capture
    // supervisor from tearing the mirror down while a burst is being measured.
    // (The flag itself is OR'd into capture::setConsuming/setPreviewWanted from
    // the render loop — see the note in aimbot_ui.cpp.)
    {
        const recoil::RecState before = recoil::state();
        recoil::pump();
        // Edge-triggered result toasts. Analysis usually finishes while the
        // menu is closed (it must be — firing needs the board out of the way),
        // so the Done/Failed card alone is invisible; toasts survive the board
        // being hidden, which is exactly when they are needed.
        const recoil::RecState after = recoil::state();
        if (after != before &&
            (after == recoil::RecState::Done || after == recoil::RecState::Failed)) {
            const recoil::RecStats& rs = recoil::stats();
            char msg[256];
            if (after == recoil::RecState::Done && rs.klass == "standard") {
                std::snprintf(msg, sizeof(msg), "分析完成：%s · %d 发，可用",
                              rs.outStem.c_str(), rs.shots);
                notify::success("", msg);
                pg.catalogDirty = true;
            } else if (after == recoil::RecState::Done) {
                std::snprintf(msg, sizeof(msg), "分析完成但不可驱动：%s",
                              rs.message.c_str());
                notify::warn("", msg);
                pg.catalogDirty = true;
            } else {
                notify::error("", rs.message.empty() ? "录制失败" : rs.message.c_str());
            }
        }
    }

    // The keyboard is a modal layer: while it is up, no control on the board
    // may react to a tap. Published before the board draws next frame.
    if (!ime::isOpen() && widgets::modalOwner()) widgets::setModalOwner(false);

    // A tap outside the keyboard dismisses it — the same scrim-dismiss rule the
    // Model dialogs use, expressed with the keyboard's own panel rect.
    if (ime::isOpen() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ime::hitPanel(ImGui::GetIO().MousePos)) {
        ime::close();
        // Drop the field's focus with it, so the two agree about being closed.
        // Without this the next tap on the field would be a no-op: the field is
        // already focused, so nothing reports a change.
        pg.nameBuf.focused = false;
    }

    // ── The burst ───────────────────────────────────────────────────────────
    const int mode = pg.mode.value;
    const bool firing = firingNow();
    recoil::Profile* p = activeProfile(pg);
    const bool canDrive = pg.enabled.value && p != nullptr && p->drivable();

    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 1.0f / 2000.0f, 0.2f);

    // Leaving 准心识别 mode or master switch off must stop its tracker, or it would keep reading
    // frames (and spend the capture pipeline) while the page shows something
    // else entirely.
    if ((!pg.enabled.value || mode != PageRecoil::kModeCrosshair) && crosshair::tracking()) {
        crosshair::resetTracking();
    }
    // 准星采样：只要处于采样状态中，必须每帧无条件 pumpSampling，直至 1.5s 完成并保存配置。
    // 绝不受压枪总开关、准星追踪或模式切换的阻断（此前若开启了总开关但没开准星追踪，
    // pumpSampling 被跳过导致采样进度卡在 100% 无法结束）。
    if (crosshair::sampling()) {
        crosshair::pumpSampling();
    }

    // What drives the burst, per mode. Both produce the same three outputs:
    // this frame's increment (dx,dy), the burst-so-far accumulation
    // (accX,accY) for the view-offset / own-contact carriers, and `driving`
    // — whether the compensation is live at all.
    float dx = 0.0f, dy = 0.0f;
    float accX = 0.0f, accY = 0.0f;
    bool driving = false;

    if (mode == PageRecoil::kModeCrosshair) {
        if (pg.enabled.value && pg.xTrack.value) {
            crosshair::pumpTracking(pg.enabled.value);
        } else {
            crosshair::resetTracking();
        }
        driving = false;
    } else if (mode == PageRecoil::kModeValorant) {
        valorant_recoil::Config vc;
        vc.weapon = static_cast<valorant_recoil::Weapon>(pg.vWeapon.value);
        vc.stage1Px = pg.vStage1Px.value;
        vc.stage2Px = pg.vStage2Px.value;
        vc.resetMs = pg.vResetMs.value;
        vc.returnHome = pg.vReturnHome.value;
        vc.returnSpeed = pg.vReturnSpeed.value;
        valorant_recoil::setConfig(vc);

        driving = valorant_recoil::tick(dt, firing && pg.enabled.value, pg.enabled.value,
                                        accX, accY, dx, dy);
        if (driving && !g_wasFiring) {
            LOGI("recoil: valorant burst start weapon=%d s1=%.1f s2=%.1f",
                 pg.vWeapon.value, vc.stage1Px, vc.stage2Px);
        }
    } else if (mode == PageRecoil::kModePattern) {
        if (canDrive && firing && !g_playerArmed) {
            float kx = 0.0f, ky = 0.0f, px = 0.0f, py = 0.0f;
            const bool okx = axisScale(*p, 0, pg.travelX.value, kx, px);
            const bool oky = axisScale(*p, 1, pg.travelY.value, ky, py);
            if (!okx) kx = 0.0f;
            if (!oky) ky = 0.0f;
            const float travel = std::max(pg.travelX.value, pg.travelY.value);
            g_player.begin(*p, 1.0f / std::max(1e-4f, kx), 1.0f / std::max(1e-4f, ky),
                           pg.flipX.value ? -1.0f : 1.0f,
                           pg.flipY.value ? -1.0f : 1.0f,
                           pg.weightX.value, pg.weightY.value,
                           /*budgetPx=*/std::max(60.0f, travel * 1.5f));
            g_playerArmed = true;
            LOGI("recoil: burst start profile=%s shots=%d S=(%.0f,%.0f)",
                 p->name.c_str(), p->shots, pg.travelX.value, pg.travelY.value);
        }
        if ((!firing || !canDrive) && g_playerArmed) {
            g_player.stop();
            g_playerArmed = false;
        }

        if (g_playerArmed && firing) g_player.step(dt, dx, dy);
        driving = g_playerArmed && firing;
        if (g_playerArmed) {
            accX = g_player.accumX();
            accY = g_player.accumY();
        }
    }
    g_emitX = dx;
    g_emitY = dy;

    // ── Who carries it ──────────────────────────────────────────────────────
    //
    // Keyed on whether the aim is ACTUALLY holding a contact right now
    // (Pressed or Holding), not on the Aim page's master switch. The switch
    // being on does not mean the aim has a finger down: with no target on
    // screen the aim sits in Idle, publishAimDelta() would hand the burst to
    // nobody, and the mirror offset path was zeroed here — three dead ends
    // that read on the device as "开火了不压枪". While the aim IS holding a
    // contact, its position moves go through emitContactMove(), which folds
    // the published delta in — that path needs no help here.
    //
    // When the aim is not holding anything the compensation rides whoever
    // IS on the glass: the player's own mirrored thumb (view offset — no
    // second finger, this is the fusion rule: never add a contact where one
    // exists) or, only when the look area is completely empty, our own
    // contact on UINPUT_SLOT_PRIMARY.
    //
    // The Idle → Pressed transition mid-burst swaps the owner, but the swap
    // is ordered by the frame: this code runs in the page pass and drops
    // g_ownHeld the moment the aim branch is taken, so the two branches are
    // never both holding a contact past this frame.
    const widgets::Rect touch = aimTouchRect();

    if (g_pageAim.enabled.value && aimIsDriving()) {
        // The aim owns the contact. Hand it the accumulated compensation delta and make sure we are not
        // also holding one: two contacts on the look area is the fault the
        // point guard exists to prevent.
        recoil::publishAimDelta(accX, accY);
        if (g_ownHeld) {
            // Ownership hand-over, mid-burst. syncAimPage() ran BEFORE this, so
            // the aim's uinput_down on UINPUT_SLOT_PRIMARY already happened —
            // and on MT-protocol slot reuse that is a MOVE of the contact we
            // were holding, not a second finger. Lifting here would kill the
            // aim's contact the frame it took over. Drop only our bookkeeping;
            // the point guard is the same rect the aim's publishPointGuard()
            // just armed, so it stays managed from the aim's side from here.
            g_ownHeld = false;
        }
        reader_set_view_offset(nullptr, 0.0f, 0.0f);
    } else {
        recoil::publishAimDelta(0.0f, 0.0f);

        // Nobody is aiming for us, so the compensation has to ride a contact
        // that already exists. If the player's own thumb is on the look area,
        // that thumb IS the contact — displacing it is the only option that
        // does not put a second finger on the glass.
        const bool handOnLook = anyPhysicalFingerIn(touch);
        if (handOnLook) {
            if (g_ownHeld) {
                uinput_up(UINPUT_SLOT_PRIMARY);
                g_ownHeld = false;
                reader_set_point_guard(nullptr, false, -1);
            }
            if (g_playerArmed || driving) {
                const int rect[4] = {static_cast<int>(touch.x), static_cast<int>(touch.y),
                                     static_cast<int>(touch.w), static_cast<int>(touch.h)};
                reader_set_view_offset(rect, accX, accY);
            } else {
                reader_set_view_offset(nullptr, 0.0f, 0.0f);
            }
        } else {
            reader_set_view_offset(nullptr, 0.0f, 0.0f);

            // Back-flash owns UINPUT_SLOT_PRIMARY for its whole cycle, and the
            // aim page is not in this branch because its switch is off — so
            // nobody else is enforcing that rule here. Pressing our own contact
            // on top of back-flash's would be two fingers on the look axis.
            const bool want = driving && !backflashIsBlockingAim();
            if (want && !g_ownHeld) {
                // Home. If the aim lifted its own contact THIS frame (target
                // lost mid-burst, still firing), seed from that contact's last
                // position: pressing where the aim lifted is invisible to the
                // game, while pressing at the jittered centre would jump the
                // view by the whole distance between the two points. Otherwise
                // use the middle of the aim touch area, jittered so the press
                // is not the same pixel every burst.
                const ImVec2& last = g_pageAim.touchAim.position;
                const ImVec2 disp = ImGui::GetIO().DisplaySize;
                if (g_pageAim.enabled.value && last.x > 0.0f && last.y > 0.0f &&
                    last.x < disp.x && last.y < disp.y) {
                    g_ownBaseX = last.x;
                    g_ownBaseY = last.y;
                } else {
                    g_ownBaseX = touch.x + touch.w * (0.35f + 0.30f * rnd01());
                    g_ownBaseY = touch.y + touch.h * (0.35f + 0.30f * rnd01());
                }
                g_ownX = g_ownBaseX;
                g_ownY = g_ownBaseY;
                // Guard first: a finger landing inside the area after this press
                // must not become a second look contact.
                const int rect[4] = {static_cast<int>(touch.x), static_cast<int>(touch.y),
                                     static_cast<int>(touch.w), static_cast<int>(touch.h)};
                reader_set_point_guard(rect, true, -1);
                uinput_down(UINPUT_SLOT_PRIMARY, UINPUT_ID_PRIMARY,
                            static_cast<int>(g_ownX), static_cast<int>(g_ownY));
                g_ownHeld = true;
            } else if (want && g_ownHeld) {
                g_ownX = g_ownBaseX + accX;
                g_ownY = g_ownBaseY + accY;
                uinput_move(UINPUT_SLOT_PRIMARY, static_cast<int>(g_ownX),
                            static_cast<int>(g_ownY));
            } else if (!want && g_ownHeld) {
                // Stop producing, do NOT unwind: moving the finger back to its
                // home would rotate the view through the whole pattern again,
                // in the opposite direction (see §7.6 of the plan).
                uinput_up(UINPUT_SLOT_PRIMARY);
                g_ownHeld = false;
                reader_set_point_guard(nullptr, false, -1);
                LOGI("recoil: own contact released sum=(%.1f,%.1f)", accX, accY);
            }
        }
    }

    g_wasFiring = firing;
}

bool recoilIsFiring() {
    return firingNow();
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng

#endif  // HAS_IMGUI
