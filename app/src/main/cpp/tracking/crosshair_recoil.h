// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::crosshair — crosshair-recoil mode runtime (准心识别压枪)
//
//  Three jobs:
//    1. SAMPLING  — "开始识别" samples ~1.5 s of frames and extracts the
//                   crosshair's colour range from the screen centre (the same
//                   background-rejecting scheme as the reference F8 sampler),
//                   then stores it as one config under the given name.
//    2. TRACKING  — while "准心追踪" is on, every tick runs the recogniser on
//                   the newest captured frame and keeps the crosshair position.
//    3. CONTROL   — while firing, a small PID pulls the crosshair into the
//                   aim's target box (falling back to the screen centre when
//                   the tracker has no target). The output is finger px per
//                   frame, consumed by recoil_section's carrier plumbing.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

namespace aimbotng {
namespace crosshair {

struct Params {
    float kp   = 4.0f;   // P: finger px/s per screen px of error (before gain)
    float ki   = 1.5f;   // I
    float kd   = 0.8f;   // D
    float gain = 1.0f;   // finger px of travel per screen px of error
};

void setParams(const Params& p);
Params params();

// ── Sampling ────────────────────────────────────────────────────────────────
/// Starts a 1.5 s colour sample that will be saved as `<name>`. Returns false
/// (with `err`) when the name is unusable or a sample is already running.
bool startSample(const std::string& name, std::string& err);
void abortSample();
bool sampling();
/// Progress of the running sample in [0,1]; 0 when idle.
float sampleProgress();
/// Result line of the last finished sample ("" = none yet).
const std::string& sampleStatus();
bool sampleOk();

/// Called every UI tick while a sample runs: pulls a frame and accumulates;
/// finishes and writes the config when the window elapses.
void pumpSampling();

// ── Configs ─────────────────────────────────────────────────────────────────
/// Rescans the config directory. Returns how many usable configs were found.
int  reloadConfigs();
int  configCount();
std::vector<std::string> configNames();
bool deleteConfig(const std::string& name);

// ── Tracking / control ──────────────────────────────────────────────────────
void setTracking(bool on);
bool tracking();

/// One control tick. When firing, accumulates the PID's output into
/// `accX/accY` (the burst-so-far finger displacement, for the view-offset and
/// own-contact carriers) and reports this frame's increment in `dx/dy`.
/// When not firing, resets the burst. Returns true when compensation is live.
bool tick(float dt, bool firing, float& accX, float& accY, float& dx, float& dy);

/// Whether frames must keep flowing (sampling or tracking active).
bool wantsFrames();

// ── Preview ─────────────────────────────────────────────────────────────────
struct Preview {
    bool  found = false;
    float fx = 0.0f, fy = 0.0f;    // crosshair position in frame coords
    int   fw = 0, fh = 0;          // frame size
    int   hits = 0;
    float score = 0.0f;
    int   cfg = -1;
    int   nCfgs = 0;
    bool  haveTarget = false;      // aim tracker's current target, screen px
    float tx = 0.0f, ty = 0.0f, tw = 0.0f, th = 0.0f;
};
bool preview(Preview& out);

}  // namespace crosshair
}  // namespace aimbotng
