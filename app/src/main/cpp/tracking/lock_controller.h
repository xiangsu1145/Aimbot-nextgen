#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aimbotng {
namespace tracking {

// Reference P_PID constants from G:\qq\qq1\锁死控制器.cpp
constexpr double PID_BANDWIDTH = 10000.0;
constexpr double SOFT_LIMIT_NUMERATOR = 4.0 / 15.0;
constexpr double SOFT_LIMIT_DENOMINATOR = 3.0 / 5.0;

inline double smoothTerm(double value, double bandwidth, double outputScale) {
    if (bandwidth <= 0.0) return value;
    double ratio = value / bandwidth;
    double squared = ratio * ratio;
    return (ratio * (1.0 + SOFT_LIMIT_NUMERATOR * squared) /
        (1.0 + SOFT_LIMIT_DENOMINATOR * squared)) * outputScale;
}

class LockController {
public:
    LockController() = default;

    void setParams(float kp, float ki, float kd, float outSmooth, float lookahead) {
        // Slider kp (e.g. 0.15) maps to P_PID kp (15.0) via * 100.0
        kp_ = static_cast<double>(kp) * 100.0;
        // Slider kd (e.g. 0.25) maps to P_PID kd (25.0) via * 100.0
        kd_ = static_cast<double>(kd) * 100.0;
        // Slider lookahead (前馈, e.g. 1.5) maps to predict
        predict_ = (lookahead <= 1.0f && lookahead > 0.0f)
                       ? (static_cast<double>(lookahead) * 3.0)
                       : static_cast<double>(lookahead);
        // Smooth parameter: 9900.0 default, bandwidth=10000.0
        smooth_ = 9000.0 + std::clamp(static_cast<double>(outSmooth), 0.0, 1.0) * 900.0;
        // Integral gain adaptation rate (reference uses 0.025)
        integral_gain_rate_ = (ki > 0.0f) ? static_cast<double>(ki) : 0.025;
    }

    void setTargetBoxPx(float px) {
        boxPx_ = px;
    }

    void setDeadband(float db) {
        deadband_ = (db > 0.0f) ? static_cast<double>(db) : 0.3;
    }

    float update(float error, float dt, bool frozen,
                 bool targetChanged, bool measurementCorrected) {
        (void)dt;
        (void)frozen;
        (void)measurementCorrected;

        if (!std::isfinite(error)) {
            reset();
            return 0.0f;
        }

        // Subpixel noise gate (reference uses 0.3 px)
        if (std::abs(error) < deadband_) {
            error = 0.0f;
        }

        // Target switch or fresh track: reset transient state so jump doesn't fake velocity
        if (targetChanged || !hasLastError_) {
            reset();
            last_error_ = error;
            hasLastError_ = true;
            return 0.0f;
        }

        // Jump detection: if target suddenly teleported > 45px or 0.8*box (e.g. track switch)
        double jumpThresh = std::max(45.0, static_cast<double>(boxPx_ * 0.8f));
        if (std::abs(error - last_error_) > jumpThresh) {
            reset();
            last_error_ = error;
            hasLastError_ = true;
            return 0.0f;
        }

        adjust_integral(error);
        kp_integral(error);

        double error_diff = error - last_error_;
        double target_velocity = error_diff + last_u_;
        target_velocity = update_velocity_filter(target_velocity);

        double raw_velocity_input = target_velocity;
        if (std::abs(error) < 1.0 && std::abs(error_diff) < 0.1) {
            raw_velocity_input = error_diff + last_u_ * 0.5;
        }

        double ki_raw = raw_velocity_input;
        ki_raw = (std::abs(ki_raw) > 0.5) ? ki_raw : 0.0;
        ki_raw = (ki_raw * predict_) * integral_gain_;
        ki_raw = update_integral_filter(ki_raw);

        double K_p = kp_ * error;
        double K_i = ki_raw;
        double K_d = kd_ * (error - last_error_);

        if (smooth_ > 0.0) {
            K_p = smoothTerm(K_p, bandwidth_, bandwidth_ - smooth_);
            K_i = smoothTerm(K_i, bandwidth_, bandwidth_ - 1000.0);
            K_d = smoothTerm(K_d, bandwidth_, bandwidth_ - smooth_);
        }

        double u = K_p + K_i + K_d;
        double u_filtered = u * kp_gain_;

        // Telemetry caching
        pDisp_ = static_cast<float>(K_p);
        iDisp_ = static_cast<float>(ki_raw);
        dDisp_ = static_cast<float>(K_d);
        fDisp_ = static_cast<float>(K_i);
        velNow_ = static_cast<float>(target_velocity);

        last_u_ = u_filtered;
        last_error_ = error;
        hasLastError_ = true;

        return static_cast<float>(u_filtered);
    }

    void reset() {
        kp_gain_ = 0.0;
        integral_gain_ = 0.0;
        u_filtered_ = 0.0;
        last_error_ = 0.0;
        hasLastError_ = false;
        last_u_ = 0.0;
        velocity_filter_x_ = 0.0;
        velocity_filter_p_ = 0.0;
        integral_filter_x_ = 0.0;
        integral_filter_p_ = 0.0;
        pDisp_ = dDisp_ = iDisp_ = fDisp_ = 0.0f;
        velNow_ = 0.0f;
    }

    // Telemetry getters for logging and hud
    float integralValue()  const { return iDisp_; }
    float ffValue()        const { return fDisp_; }
    float pValue()         const { return pDisp_; }
    float dValue()         const { return dDisp_; }
    float velValue()       const { return velNow_; }
    float outGain()        const { return static_cast<float>(kp_gain_); }
    float trustValue()     const { return 1.0f; }
    float predGainValue()  const { return static_cast<float>(integral_gain_); }
    bool  velocityReady()  const { return true; }
    float outLimitPx()     const { return 100.0f; }
    float lookaheadValue() const { return static_cast<float>(predict_); }
    int   jumpSoftCount()  const { return 0; }
    int   jumpHardCount()  const { return 0; }

private:
    void adjust_integral(double error) {
        double abs_error = std::abs(error);
        if (abs_error < integral_gain_threshold_) {
            double ratio = 1.0 - (abs_error / integral_gain_threshold_);
            integral_gain_ += (ratio - integral_gain_) * integral_gain_rate_;
        } else {
            double ratio = integral_gain_threshold_ / abs_error;
            integral_gain_ += (ratio * integral_gain_ - integral_gain_) * 0.1;
        }
        integral_gain_ = std::clamp(integral_gain_, 0.0, 1.0);
    }

    void kp_integral(double error) {
        double abs_error = std::abs(error);
        if (abs_error < kp_gain_threshold_) {
            double ratio = 1.0 - (abs_error / kp_gain_threshold_);
            kp_gain_ += (ratio - kp_gain_) * kp_gain_rate_;
        } else {
            double ratio = kp_gain_threshold_ / abs_error;
            kp_gain_ += (ratio * kp_gain_ - kp_gain_) * 0.1;
        }
        kp_gain_ = std::clamp(kp_gain_, 0.0, 1.0);
    }

    double update_velocity_filter(double measurement) {
        constexpr double q = 0.01;
        constexpr double r = 1.0;
        double predicted_x = velocity_filter_x_;
        double predicted_p = velocity_filter_p_ + q;
        double k = predicted_p / (predicted_p + r);
        velocity_filter_x_ = predicted_x + k * (measurement - predicted_x);
        velocity_filter_p_ = (1.0 - k) * predicted_p;
        return velocity_filter_x_;
    }

    double update_integral_filter(double measurement) {
        constexpr double q = 0.5;
        constexpr double r = 1.0;
        double predicted_x = integral_filter_x_;
        double predicted_p = integral_filter_p_ + q;
        double k = predicted_p / (predicted_p + r);
        integral_filter_x_ = predicted_x + k * (measurement - predicted_x);
        integral_filter_p_ = (1.0 - k) * predicted_p;
        return integral_filter_x_;
    }

    double kp_ = 15.0;
    double kd_ = 25.0;
    double bandwidth_ = PID_BANDWIDTH;
    double smooth_ = 9900.0;
    double predict_ = 1.5;
    double ki_deadband_ = 0.5;
    double deadband_ = 0.3;

    double u_filtered_ = 0.0;
    double last_error_ = 0.0;
    bool   hasLastError_ = false;
    double last_u_ = 0.0;

    double kp_gain_ = 0.0;
    double kp_gain_threshold_ = 1920.0;
    double kp_gain_rate_ = 0.3;

    double integral_gain_ = 0.0;
    double integral_gain_threshold_ = 50.0;
    double integral_gain_rate_ = 0.025;

    double velocity_filter_x_ = 0.0;
    double velocity_filter_p_ = 0.0;
    double integral_filter_x_ = 0.0;
    double integral_filter_p_ = 0.0;

    float pDisp_ = 0.0f;
    float dDisp_ = 0.0f;
    float iDisp_ = 0.0f;
    float fDisp_ = 0.0f;
    float velNow_ = 0.0f;
    float boxPx_ = 0.0f;
};

// Aliased as PredictivePid so existing references compile directly
using PredictivePid = LockController;

constexpr float kLookaheadDefault = 1.5f;
constexpr float kLookaheadMax     = 3.0f;

} // namespace tracking
} // namespace aimbotng
