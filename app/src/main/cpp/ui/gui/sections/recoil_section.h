// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::recoil — the Recoil page (压枪)
//
//  What this page is for: it turns a measured bullet pattern into a touch
//  offset that is added to the finger which already owns the screen's "look"
//  contact, for as long as the player is firing.
//
//  Two columns, as designed:
//
//    left  — the scalars. Every knob here is a number the user has to find by
//            trying: how far the burst has to be dragged (整梭行程), how much of
//            that lands on each axis, and the two direction flips that fix a
//            profile measured under the other sign convention.
//
//    right — the profile and its picture. A narrow column holding the profile
//            picker, the VERTICAL pattern chart, and 恢复 / 保存.
//
//  The chart is vertical because the thing it draws is: the x axis of the chart
//  is the weapon's horizontal swing and the y axis is time (shot 1 at the top).
//  Laid out horizontally — the obvious first guess — the narrow column would
//  have to fit 34 knots across ~500 design px, and the parameter that actually
//  needs aiming (how far left does shot 12 drift) is compressed into the
//  direction the column has least of.
//
//  Editing is per SHOT, dragging horizontally, and it is a staircase: moving
//  shot k displaces every frame from shot k onward, because what is being
//  corrected is "how much this shot moved the view", and a curve is the sum of
//  those. Moving one knot alone would draw a spike the physics cannot produce.
//  The measurement stays drawn underneath as a dashed line, so the size of the
//  correction is visible rather than remembered.
//
//  The page owns three screen-space overlays — the fire area, the marker box and
//  the pattern chart's hit targets are board-space, the first two are not — and
//  one modal layer: the recording / analysing progress card.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

#include "tracking/recoil_profile.h"
#include "ui/gui/widgets.h"

struct ImDrawList;

namespace aimbotng {
namespace ui {
namespace sections {

// ── The fire area ───────────────────────────────────────────────────────────
//
// Where the game's fire button is. While a real finger is down inside it, the
// burst is considered to be firing and the compensation runs. It is drawn as a
// circle with the same drag/resize interaction as the Trigger page's 触发区域
// (interior = move, N/E/S/W handles = scale), and it is deliberately a
// SEPARATE region from 触摸区域 and 触发区域: those two decide where the aim's
// own contact and the hold test live, and re-using either of them would couple
// "am I shooting" to "is the aimbot holding a finger", which are independent
// questions.
struct RecoilFireArea {
    float cx = -1.0f, cy = -1.0f;
    float r  = 200.0f;
    bool  placed = false;

    enum class Drag {
        None, Move,
        ResizeN, ResizeE, ResizeS, ResizeW,
    };
    Drag  drag    = Drag::None;
    float anchorX = 0.0f, anchorY = 0.0f;
    float startCX = 0.0f, startCY = 0.0f;
    float startR  = 0.0f;
};

// ── The marker box ──────────────────────────────────────────────────────────
//
// The patch the recorder correlates. A small square the user drags onto a
// high-contrast detail of the world (a door frame, a sign, a rock) before
// recording. It must sit inside the capture pipeline's centre square — the page
// draws that square while the marker switch is on, so "inside" is something the
// user can see rather than something the log tells them afterwards.
struct RecoilMarkerBox {
    float x = -1.0f, y = -1.0f;
    float size = 48.0f;
    bool  placed = false;
    bool  dragging = false;
    float anchorX = 0.0f, anchorY = 0.0f;
    float startX  = 0.0f, startY  = 0.0f;
};

// ── The page ────────────────────────────────────────────────────────────────

struct PageRecoil {
    /// The page's master switch. Drawn on the header line like every other
    /// page's — there is deliberately no "启用压枪" row, because a page whose
    /// whole body is one feature does not need a row to say so twice.
    widgets::SwitchState enabled{false};

    /// ── 压枪模式 ── 0 = 准心识别, 1 = 无畏契约智能压枪 (弹道分析已隐藏)
    widgets::DropdownState mode{0};
    static constexpr int kModeCrosshair = 0;
    static constexpr int kModeValorant  = 1;
    static constexpr int kModePattern   = 2;  // 弹道分析（已从 UI 移除）

    // ── Profile catalogue ───────────────────────────────────────────────────
    std::vector<recoil::Profile> profiles;
    std::vector<std::string>     names;
    std::vector<const char*>     namePtrs;
    widgets::DropdownState       pick;        ///< index into `names`
    int  loaded  = -1;                        ///< which profile the player holds
    bool catalogDirty = true;                 ///< rescan on the next frame
    /// A profile to select as soon as the catalogue is (re)read. The config
    /// file stores the profile's NAME rather than its index, because the index
    /// moves whenever a profile file is added or removed — and a saved "3"
    /// would then silently mean a different weapon.
    std::string wantName;

    // ── Crosshair-config catalogue (准心识别 mode's right column) ───────────
    // Separate from the pattern profiles above: the two modes list different
    // things, and sharing one dropdown state made 准心识别 show 弹道分析's
    // weapon profiles.
    std::vector<std::string> xNames;
    std::vector<const char*> xNamePtrs;
    widgets::DropdownState   xPick;           ///< index into `xNames`

    // ── Name field + recorder ───────────────────────────────────────────────
    widgets::DialogField nameBuf;             ///< the profile name to record into
    std::string          seedHint;            ///< why 录制 refused last time

    // ── Knobs ───────────────────────────────────────────────────────────────
    /// 整梭行程 (finger px) per axis — the ONE number the user tunes. See the
    /// units note in tracking/recoil_profile.h: the compensation scale is
    /// S / Θpp, so this is "how far does one burst have to be dragged", and the
    /// weapon's own angles never have to be understood.
    widgets::SliderState travelX{220.0f, 0.0f, 600.0f, 1.0f};
    widgets::SliderState travelY{0.0f, 0.0f, 600.0f, 1.0f};
    /// Per-axis share of the compensation. Both default so that only the
    /// horizontal axis moves — the vertical kick is the one a hand can follow.
    widgets::SliderState weightX{1.0f, 0.0f, 1.0f, 0.05f};
    widgets::SliderState weightY{0.0f, 0.0f, 1.0f, 0.05f};
    /// Vertical FOV of the measurement. Used for px → degrees when a pattern is
    /// recorded, and to display the reverse-solved α. It cancels out of the
    /// compensation (the scale is S/Θpp), so it only has to be CONSISTENT.
    widgets::SliderState fov{80.0f, 30.0f, 120.0f, 1.0f};
    /// Direction flips. A profile measured by the PC analyser and one measured
    /// here need not share a sign convention, and finding that out by editing
    /// code is the expensive way.
    widgets::SwitchState flipX{false};
    widgets::SwitchState flipY{false};

    // ── Triggers & Overlays ─────────────────────────────────────────────────
    /// Continuous trigger switch (持续触发) — defaults to true.
    /// When on, recoil activates continuously; when off, the "触发区域" switch
    /// is shown and recoil only activates when touching the trigger area circle.
    widgets::SwitchState continuousTrigger{true};

    widgets::SwitchState fireToggle{false};
    RecoilFireArea       fire;

    widgets::SwitchState markerToggle{false};
    RecoilMarkerBox      marker;

    // ── Pattern editor ──────────────────────────────────────────────────────
    /// Which axis the chart draws and the drag edits. 0 = 水平 (yaw),
    /// 1 = 垂直 (pitch).
    widgets::SwitchState editVertical{false};

    // ── 准心识别 mode knobs ─────────────────────────────────────────────────
    /// The crosshair tracking switch (准心追踪).
    widgets::SwitchState xTrack{false};

    // ── 无畏契约智能压枪 mode knobs ──────────────────────────────────────────
    widgets::DropdownState vWeapon{0};                                 ///< 0: 狂徒, 1: 幻影, 2: 自定义
    widgets::SliderState   vStage1Px{66.0f, 0.0f, 500.0f, 1.0f};      ///< 一阶段下压行程 (px, 狂徒前5发/幻影前6发)
    widgets::SliderState   vStage2Px{90.0f, 0.0f, 500.0f, 1.0f};      ///< 二阶段下压行程 (px, 紧接压3发)
    widgets::SliderState   vReturnSpeed{220.0f, 40.0f, 800.0f, 5.0f}; ///< 停火回正速度 (px/s)
    widgets::SliderState   vResetMs{300.0f, 100.0f, 600.0f, 10.0f};   ///< 后坐重置窗口 (ms)
    widgets::SwitchState   vReturnHome{true};                          ///< 停火平滑回中

    int   dragShot     = -1;    ///< shot index whose node is under the finger
    bool  dragActive   = false;
    float dragStartX   = 0.0f;  ///< surface px where the drag began
    float dragBaseTune = 0.0f;  ///< the node's tune value when the drag began

    // ── Reported state ──────────────────────────────────────────────────────
    std::string status;         ///< one line, always says what the page thinks
    float alphaX = 0.0f;        ///< reverse-solved deg per finger px, yaw
    float alphaY = 0.0f;        ///< the same for pitch
    float perShotX = 0.0f;      ///< average |Δθx| per shot, degrees
    float perShotY = 0.0f;
};

extern PageRecoil g_pageRecoil;

/// Draws the page's two columns starting at (x, *y). `y` advances in natural
/// (un-scrolled) coordinates; `sc` carries the offset — the same convention
/// every other section uses.
void drawRecoilSection(ImDrawList* dl, float x, float& y, float w,
                       float bottomY, float s, float es, const Xf& xf,
                       Scroll& sc);

/// Draws the screen-space overlays (fire area, marker box, capture square) and
/// handles their drags. Called from drawHud() every frame, board or no board.
void drawRecoilOverlays();

/// Per-frame drive: reads the fire area, advances the burst, injects the
/// compensation onto whichever contact owns the look, pumps the recorder, and
/// keeps the capture pipeline alive while recording. Runs even when the board
/// is hidden — that is the whole point, since firing happens with the menu out
/// of the way.
void syncRecoilPage();

/// Appends the swallow rectangles this page's overlays need — currently the
/// marker box while it is shown. A finger inside one is consumed by the
/// overlay (it drives the ImGui drag) and is NOT mirrored to the game, so
/// dragging the marker no longer rotates the view. The fire circle is
/// deliberately NOT here: it sits on the game's fire button, and the finger
/// pressing that button must reach the game.
void recoilSwallowRects(std::vector<int>& out);

/// Draws the recording / analysing card, if one is up. Called from
/// drawBoard's overlay pass, after the board, so the card floats above it
/// (the same contract as the Model page's dialogs).
void drawRecoilDialogs(ImDrawList* dl, float s, float es, const Xf& xf);

/// True while the progress card or the on-screen keyboard owns the board, so
/// the rail and the header switches go quiet.
bool anyRecoilModal();

/// The small readout that rides the header line next to the master switch —
/// which profile is loaded and how big its pattern is. Empty when the page is
/// switched off, so the header says nothing rather than something stale.
///
/// It is built here rather than in hud.cpp because the answer lives in the
/// profile catalogue, and the header line must not have to know what a
/// profile is to be able to print one.
std::string recoilHeaderText();

/// Whether the burst is currently firing (from auto-trigger or manual fire area).
bool recoilIsFiring();

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
