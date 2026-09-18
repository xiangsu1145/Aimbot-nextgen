// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::tracking::PPID — velocity-type PID with adaptive gain, soft
//  saturation, and Kalman-filtered state.
//
//  Ported from the reference pid.cpp. Inputs/outputs are in screen pixels per
//  60 Hz reference frame. The caller converts to per-frame displacement by
//  dividing velocity by 60.
//
//  Key design decisions from the reference:
//    * velocity output (not position) — naturally handles the velocity-damping
//      brake without extra terms.
//    * smoothTerm() soft-saturation — bounds the control authority gracefully
//      under large errors without hard clipping.
//    * kp_gain / integral_gain — adaptive gains that ramp up as the crosshair
//      closes on the target, giving fast convergence and stable holding.
//    * One-dimensional Kalman filters on velocity and integral inputs — cleans
//      jitter from both feedback paths.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cmath>

namespace aimbotng {
namespace tracking {

namespace {
// ── Soft-saturation helper ───────────────────────────────────────────────────
//
// A smooth, differentiable replacement for hard clamping. Above the bandwidth
// the gain falls off as O(1/ratio²), staying finite everywhere without a
// discontinuity at the boundary. The numerator/denominator coefficients come
// from the reference.
static inline float smoothTerm(float value, float bandwidth, float outputScale) {
    constexpr float SOFT_LIMIT_NUMERATOR   = 4.0f / 15.0f;
    constexpr float SOFT_LIMIT_DENOMINATOR  = 3.0f / 5.0f;
    const float ratio   = value / bandwidth;
    const float squared = ratio * ratio;
    return (ratio * (1.0f + SOFT_LIMIT_NUMERATOR * squared) /
            (1.0f + SOFT_LIMIT_DENOMINATOR * squared)) * outputScale;
}
}  // namespace

// ── Velocity-type PID ─────────────────────────────────────────────────────────
//
// Unlike a position PID which outputs displacement, this outputs velocity
// (px/frame @ 60 Hz).  The caller converts to displacement by dividing by 60
// before adding to the finger position.
//
// Template parameter TIME controls whether internal state uses float or double.
// Default is float for performance in the Android render loop.
class PPID {
public:
    PPID() = default;

    // ── Parameters ────────────────────────────────────────────────────────────
    //
    // kp, kd     : standard PID gains
    // predict    : integral amplification (from reference: 3.0 for X, 0.0 for Y)
    // rate       : kp_gain convergence rate (reference: 0.3)
    // smooth     : soft-saturation bandwidth offset (reference: 9900.0)
    //             The actual bandwidth is fixed at 10000; `smooth` is subtracted
    //             from it, so 9900 means only ±100 px of the input range sees
    //             the transition between linear and saturated regime.

    bool init(float kp_, float kd_, float predict_, float rate_, float smooth_) {
        kp        = kp_;
        kd        = kd_;
        bandwidth = 10000.0f;
        smooth    = smooth_;
        predict   = predict_;
        kp_gain_rate = rate_;
        ki_deadband  = 0.5f;
        reset();
        return true;
    }

    // ── Core update ───────────────────────────────────────────────────────────
    //
    // error : current position error (target - current), px
    // Returns: velocity in px/frame @ 60 Hz.  Divide by 60 for displacement.
    float update(float error) {
        // Error snap — tiny errors are treated as zero to avoid integrator drift.
        if (std::abs(error) < 0.3f) error = 0.0f;

        // Spike guard: if the error jumped more than 30 px this frame (camera
        // cut, detection glitch) the state is too stale to be useful — reset.
        if (std::abs(error - last_error) > 30.0f) reset();

        adjustIntegral(error);
        adjustKpGain(error);

        const float error_diff    = error - last_error;
        const float target_velocity = error_diff + last_u;
        const float vel = updateVelocityFilter(target_velocity);

        // Near-zero band: blend toward half-rate velocity to reduce noise.
        float raw = vel;
        if (std::abs(error) < 1.0f && std::abs(error_diff) < 0.1f) {
            raw = error_diff + last_u * 0.5f;
        }

        // Integral path: only active above the deadband, amplified by predict.
        float ki_raw = raw;
        if (std::abs(ki_raw) > 0.5f) {
            ki_raw *= predict * integral_gain;
        } else {
            ki_raw = 0.0f;
        }
        ki_raw = updateIntegralFilter(ki_raw);

        // Raw PID terms.
        float K_p = kp * error;
        float K_i = ki_raw;
        float K_d = kd * (error - last_error);

        // Soft saturation on all three terms.
        if (smooth > 0.0f) {
            K_p = smoothTerm(K_p,       bandwidth, bandwidth - smooth);
            K_i = smoothTerm(K_i,       bandwidth, bandwidth - 1000.0f);
            K_d = smoothTerm(K_d,       bandwidth, bandwidth - smooth);
        }

        float u = K_p + K_i + K_d;
        u_filtered = u * kp_gain;
        last_u     = u_filtered;
        last_error = error;
        return u_filtered;
    }

    // ── Reset all internal state ───────────────────────────────────────────────
    //
    // kp_gain and integral_gain start at 1.0 so the controller outputs full force
    // immediately on first press. The adaptive mechanism then modulates them DOWN
    // as the crosshair closes on target (far error → kp_gain decays, prevents
    // overshoot; near target → kp_gain holds at 1.0 for firm lock).
    void reset() {
        kp_gain          = 1.0f;
        integral_gain    = 1.0f;
        u_filtered       = 0.0f;
        last_error       = 0.0f;
        integral_term    = 0.0f;
        last_u           = 0.0f;
        velocity_filter_x = 0.0f;
        velocity_filter_p = 1.0f;
        integral_filter_x  = 0.0f;
        integral_filter_p  = 1.0f;
    }

private:
    // ── Adaptive integral gain ────────────────────────────────────────────────
    //
    // integral_gain drives how aggressively the I term is applied.  It ramps
    // toward 1.0 when |error| is small (close to target — time to hold) and
    // decays when the error is large (far from target — let P/D do the work).
    void adjustIntegral(float error) {
        const float abs_error = std::abs(error);
        if (abs_error < integral_gain_threshold) {
            const float ratio = 1.0f - (abs_error / integral_gain_threshold);
            integral_gain += (ratio - integral_gain) * integral_gain_rate;
        } else {
            const float ratio = integral_gain_threshold / abs_error;
            integral_gain += (ratio * integral_gain - integral_gain) * 0.1f;
        }
        integral_gain = std::clamp(integral_gain, 0.0f, 1.0f);
    }

    // ── Adaptive proportional gain ─────────────────────────────────────────────
    //
    // kp_gain_rate=0 disables the mechanism (kp_gain stays at 1.0 always).
    // Otherwise it ramps toward 1.0 when error < threshold (approach phase)
    // and decays toward 0 when error > threshold (to reduce overshoot on large steps).
    void adjustKpGain(float error) {
        if (kp_gain_rate <= 0.0f) return;  // mechanism disabled
        const float abs_error = std::abs(error);
        if (abs_error < kp_gain_threshold) {
            const float ratio = 1.0f - (abs_error / kp_gain_threshold);
            kp_gain += (ratio - kp_gain) * kp_gain_rate;
        } else {
            const float ratio = kp_gain_threshold / abs_error;
            kp_gain += (ratio * kp_gain - kp_gain) * kp_gain_rate;
        }
        kp_gain = std::clamp(kp_gain, 0.0f, 1.0f);
    }

    // ── 1-D Kalman filter on velocity input ────────────────────────────────────
    float updateVelocityFilter(float measurement) {
        constexpr float q = 0.5f;   // process noise — was 0.01; higher = faster response
        constexpr float r = 1.0f;   // measurement noise
        const float predicted_x  = velocity_filter_x;
        const float predicted_p  = velocity_filter_p + q;
        const float k = predicted_p / (predicted_p + r);
        velocity_filter_x = predicted_x + k * (measurement - velocity_filter_x);
        velocity_filter_p = (1.0f - k) * predicted_p;
        return velocity_filter_x;
    }

    // ── 1-D Kalman filter on integral input ───────────────────────────────────
    float updateIntegralFilter(float measurement) {
        constexpr float q = 0.5f;
        constexpr float r = 1.0f;
        const float predicted_x  = integral_filter_x;
        const float predicted_p  = integral_filter_p + q;
        const float k = predicted_p / (predicted_p + r);
        integral_filter_x = predicted_x + k * (measurement - predicted_x);
        integral_filter_p = (1.0f - k) * predicted_p;
        return integral_filter_x;
    }

    // ── Tunable parameters ─────────────────────────────────────────────────────
    float kp   = 0.0f;
    float kd   = 0.0f;
    float bandwidth = 0.0f;
    float smooth     = 0.0f;
    float predict    = 0.0f;
    float ki_deadband = 0.0f;

    // ── Filter state ───────────────────────────────────────────────────────────
    float u_filtered    = 0.0f;
    float integral_term = 0.0f;
    float last_integral_term = 0.0f;
    float last_error    = 0.0f;
    float last_u        = 0.0f;

    // ── Adaptive gain state ────────────────────────────────────────────────────
    float kp_gain = 0.0f;
    float kp_gain_threshold = 50.0f;   // px — below this error kp_gain holds at 1.0
    float kp_gain_rate = 0.001f;      // very slow decay so kp_gain doesn't collapse

    float integral_gain = 0.0f;
    float integral_gain_threshold = 50.0f; // px — from reference
    float integral_gain_rate = 0.025f;    // per-frame convergence rate

    // ── Kalman filter state ───────────────────────────────────────────────────
    float velocity_filter_x = 0.0f;
    float velocity_filter_p = 1.0f;
    float integral_filter_x  = 0.0f;
    float integral_filter_p  = 1.0f;
};

}  // namespace tracking
}  // namespace aimbotng
