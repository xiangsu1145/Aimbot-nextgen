// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::recoil — the bullet-pattern profile, and the player that replays it
//
//  A recoil profile is the *view-angle trajectory* one burst of one weapon
//  produces, measured once and reused forever. It is a static property of the
//  weapon; the only thing that varies with the player is how many finger pixels
//  it takes to produce one degree of view rotation.
//
//  Units, and why the two halves are kept apart:
//
//    * the profile stores degrees. `+x` is right, `+y` is DOWN (matching the way
//      live screen coordinates read: the gun kicks up, so the world moves down
//      and dy is positive).
//    * the compensation is emitted in FINGER pixels, because that is the only
//      currency the touch layer accepts.
//
//  The conversion is one scalar per axis:
//
//        scale = S / Θpp          [finger px per degree]
//        c(t)  = -scale * θ(t)    [finger px]
//
//  where Θpp is the axis' peak-to-peak excursion over the burst. Peak-to-peak
//  rather than "the value at the end": the horizontal pattern is not monotone
//  (it swings right, then left), so an endpoint normaliser would divide by a
//  number that can sit near zero — and the user's knob ("how far does this
//  burst need to be dragged, horizontally") is a peak-to-peak quantity anyway.
//  For an axis that does not reverse the two are identical.
//
//  The knob is therefore S (整梭行程, finger px) and the user never has to know
//  α = deg per finger px. Reverse-solved, α = S/Θpp — a property of the game's
//  sensitivity setting and NOT of the weapon, so the next weapon only needs
//  `S = α · Θpp_new` to be right without touching anything.
//
//  Two layers, one file each:
//
//    <name>.json       the measurement. Written by the recorder, treated as
//                      read-only by the UI — it costs a burst and a recording
//                      to produce one, so a hand-tweak must not overwrite it.
//    <name>.tune.json  the hand-tweak overlay. Per-shot ABSOLUTE finger pixels,
//                      applied as a cumulative staircase (see applyTune).
//
//  Absolute, not relative: the user reasons "this shot needs 3 px more to the
//  left", which is a quantity in the output space — re-scaling S later must not
//  scale a correction that was measured in pixels.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aimbotng {
namespace recoil {

/// Where profiles live. Same directory as config.json (adb push, no rebuild).
inline constexpr const char* kProfileDir = "/data/local/tmp/aimbotng/profiles";
inline constexpr const char* kProfileExt = ".json";
inline constexpr const char* kTuneExt    = ".tune.json";

/// One sample of the view-angle trajectory, relative to the first shot.
struct Knot {
    float tMs = 0.0f;   ///< ms since the first shot
    float dx  = 0.0f;   ///< view yaw, degrees, + = turned right
    float dy  = 0.0f;   ///< view pitch, degrees, + = looked down (gun kicked up)
};

/// The frame a shot went off on. `n` is 1-based.
struct ShotMark {
    int   n   = 0;
    float tMs = 0.0f;
};

/// A measured burst. `knots` is the whole shape; `marks` is the per-shot anchor.
struct Profile {
    // ── Identity ────────────────────────────────────────────────────────────
    std::string name;        ///< display name (the .json file's stem)
    std::string path;        ///< absolute path of the .json it came from
    int         schema = 1;

    // ── Provenance ──────────────────────────────────────────────────────────
    /// "analyser" = converted from the PC recoil-analyser's JSON; "device" =
    /// measured on the phone by the built-in marker tracker.
    std::string source = "device";
    float       fps      = 0.0f;   ///< rate the measurement was taken at
    int         srcW     = 0;
    int         srcH     = 0;
    float       fovDeg   = 80.0f;  ///< vertical FOV the px→deg conversion assumed
    float       focalPx  = 0.0f;
    std::string note;              ///< free text, shown next to the name

    // ── Quality gates (§1.5 of the plan) ────────────────────────────────────
    /// "standard" is the only class the player may drive. The others are kept so
    /// a bad measurement is visibly bad instead of silently missing.
    std::string recoilClass = "unknown";
    float       minConf   = 0.0f;  ///< worst per-frame match confidence
    float       meanConf  = 0.0f;
    int         shots     = 0;
    bool        shotsMatchMagazine = false;

    // ── Shape ───────────────────────────────────────────────────────────────
    float dxPeak = 0.0f;   ///< peak-to-peak yaw, degrees (non-negative)
    float dyPeak = 0.0f;   ///< peak-to-peak pitch, degrees (non-negative)
    std::vector<Knot>     knots;
    std::vector<ShotMark> marks;

    // ── Hand-tweak overlay (absolute finger px, per shot) ───────────────────
    // Index i is the correction applied to shot i+1; entries past `shots` are
    // kept so a re-measure with fewer shots does not silently inherit them.
    std::vector<float> tuneX;
    std::vector<float> tuneY;

    bool loaded = false;
    std::string error;   ///< why it failed to load, when loaded == false

    /// Total degrees at the last knot, signed — for display.
    float endDx() const;
    float endDy() const;

    /// True when the profile may drive the player (§1.5: only "standard").
    bool drivable() const { return loaded && recoilClass == "standard"; }

    /// Seconds the measured burst spans.
    float spanMs() const;

    void resetTune();

    /// Reads <path>. Missing / malformed leaves `loaded == false` and `error` set.
    bool load(const std::string& jsonPath);
    /// Writes the measurement half (no tune fields).
    bool save(const std::string& jsonPath) const;
    /// Reads the <stem>.tune.json overlay. Absent is not an error.
    bool loadTune(const std::string& baseStem);
    /// Writes the overlay. `saveTune` with an empty tune removes the file.
    bool saveTune(const std::string& baseStem) const;
};

/// Every profile in kProfileDir, sorted by name. Rebuilds the directory each
/// call, so a profile pushed over adb shows up on the next refresh.
std::vector<Profile> listProfiles();

/// Creates kProfileDir if it does not exist. Safe to call repeatedly.
void ensureProfileDir();

/// True when the stem is safe to use as a file name (no '/', no "..", non-empty).
bool validStem(const std::string& stem);

/// `<stem>.json` inside kProfileDir.
std::string profilePath(const std::string& stem);

/// ── Tune application ────────────────────────────────────────────────────────

/// Expands the per-shot tune overlay into a per-knot addend, in finger pixels.
///
/// The overlay is a STAIRCASE, not a set of points: correcting shot k means
/// "every frame from shot k onward is displaced by this much, on top of
/// everything already applied". Dragging one knot of the drawn curve therefore
/// shifts the whole tail of the curve, which is what the physics says happens —
/// a single moved sample would be a teleport out and back.
///
/// `out` is resized to knots.size().
void applyTune(const Profile& p, std::vector<float>& outX, std::vector<float>& outY);

// ── The player ──────────────────────────────────────────────────────────────

/// Replays one burst and hands out the per-frame compensation.
///
/// The integration is the one piece worth lifting out of `trajectory_mover.cpp`
/// (the user's reference): a knot list with cumulative time, a cursor that
/// advances by dt, and — the part that matters — the REMAINDER CARRIED ACROSS
/// SEGMENTS. Without that carry a frame whose dt straddles two knots loses the
/// whole segment it skipped, and the loss is proportional to the frame time, so
/// the compensation would silently retune itself between 60 Hz and 120 Hz.
///
/// Interpolation is LINEAR. The reference's Catmull-Rom spline is deliberately
/// not used: the measured knot list is already a ~10 ms sampling of the truth,
/// and recoil is a per-shot step, not a smooth curve. A spline overshoots where
/// the sample spacing is uneven — and the AKM pattern (up, then right, then up
/// again) is exactly such a corner, so the overshoot would be spent "dragging
/// too far" for those frames.
struct Player {
    /// Arms the player for one burst. `scaleX/Y` is finger px per degree
    /// (S/Θpp, already weighted); `signX/Y` is the ±1 direction flip the user
    /// can set when a profile was measured under a different sign convention.
    /// `budgetPx` is how far the contact may travel before it must be
    /// re-anchored — 0 disables the accounting.
    void begin(const Profile& p, float scaleX, float scaleY,
               float signX, float signY, float weightX, float weightY,
               float budgetPx);

    /// Disarms without emitting anything further. The caller keeps whatever
    /// offset it has accumulated; nothing here moves the contact back.
    void stop();
    bool running() const { return running_; }

    /// Advances by `dt` seconds and reports THIS FRAME'S delta in finger px.
    /// The delta is what the touch layer needs: it consumes finger *motion*,
    /// so adding the whole absolute compensation every frame would multiply the
    /// burst by its own frame count.
    void step(float dt, float& outDx, float& outDy);

    /// Elapsed ms since the burst's first shot (clamped to the measured span).
    float elapsedMs() const { return tMs_; }
    /// Which shot the replay is between, 1-based; 0 before the first mark.
    int shotIndex() const;
    /// Total compensation emitted so far, finger px (signed).
    float accumX() const { return accumX_; }
    float accumY() const { return accumY_; }
    /// True when the accounting stopped emitting because the drag budget ran out.
    bool clamped() const { return clamped_; }
    /// How far the burst has been replayed, 0..1.
    float progress() const;

  private:
    float sample(int axis, float tMs) const;   // linear interp on the knot list

    std::vector<float> cx_, cy_;   ///< effective compensation curve, finger px
    std::vector<float> ct_;        ///< its time base, ms
    std::vector<float> mt_;        ///< shot-mark times, ms (ascending)
    float span_    = 0.0f;
    float tMs_     = 0.0f;
    float lastX_   = 0.0f, lastY_ = 0.0f;
    float accumX_  = 0.0f, accumY_ = 0.0f;
    float budget_  = 0.0f;
    bool  running_ = false;
    bool  clamped_ = false;
};

// ── Cross-module hand-off ───────────────────────────────────────────────────
//
// The compensation has two consumers, and they live in different translation
// units: the aim's own contact path (in aim_section.cpp, for the frames where
// the aimbot is driving a finger) and the recoil page's own path (for the
// frames where it is not). The page publishes the frame's delta here; the
// aim's two inject sites add it, and nothing else reads it.
//
// It lives in this header rather than in the page's so that aim_section.cpp
// does not have to include a UI *section* to get at one pair of floats — which
// would also have made the two headers include each other.

/// Publishes this frame's compensation delta, in finger px. Called once per
/// frame by the recoil page, before the aim's sync runs.
void publishAimDelta(float dx, float dy);

/// Reads it back. (0, 0) when no burst is running.
void aimDelta(float& dx, float& dy);

/// True while the aim's contact is the one carrying the compensation — i.e. the
/// page decided the aimbot is driving and published a delta for it.
bool aimDeltaLive();

}  // namespace recoil
}  // namespace aimbotng