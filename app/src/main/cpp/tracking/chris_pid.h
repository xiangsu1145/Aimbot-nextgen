#ifndef PID_H
#define PID_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(PID_BUILD_DLL)
#define PID_API __declspec(dllexport)
#elif defined(_WIN32) && defined(PID_USE_DLL)
#define PID_API __declspec(dllimport)
#else
#define PID_API
#endif

typedef void *PID_Handle;

typedef struct PID_Diagnostics {
    double raw_error;
    double posterior_error;
    double feedback_output;
    double feed_forward_output;
    double estimated_velocity;     // Pixels per nominal 1/144-second interval.
    double estimated_acceleration; // Pixels per nominal interval squared.
    double measurement_variance;
    double observer_innovation_nis;
    double observer_robust_weight;
    int observer_innovation_run;
    double observer_covariance_inflation;
    double observer_uncertainty_scale;
    double observer_velocity_variance;
    double velocity_process_noise_scale;
    double observer_velocity_shrinkage;
    double prediction_history_response;
    double prediction_maneuver_scale;
    double prediction_acceleration_confidence;
    double prediction_acceleration_term;
    double prediction_lookahead_frames;
    double auxiliary_lead_confidence;
    double high_speed_trust;
    double output_gain;
    double integral_gain;
    double prediction_gain;
    double integral_term;
    double prediction_term;
    double frame_seconds;
    double smoothed_frame_seconds;
    double output;
    double velocity_memory_scale;
    double integral_memory_scale;
    int hard_reset;
    int transient_reset;
    int prediction_allowed;
    int prediction_enabled;
} PID_Diagnostics;

PID_API PID_Handle pid_create(void);
PID_API void       pid_destroy(PID_Handle handle);
PID_API int        pid_init(PID_Handle handle, double kp, double ki, double kd, double predict, double rate);
PID_API void       pid_set_base(PID_Handle handle, double ki_mode, double kp_Limit, double ki_Limit, double kd_Limit, double Limit, double ki_rate, double ki_deadband);
PID_API void       pid_set_latency(PID_Handle handle, double latency_seconds);
PID_API void       pid_set_observation_time_seconds(PID_Handle handle, double observation_time_seconds);
// Report the dispatched displacement in error-coordinate units. The next
// update consumes it once; without another report subsequent updates use zero.
PID_API void       pid_set_applied_move(PID_Handle handle, double move);
// Optional timestamped feedback. Times share the observation clock; move uses
// error-coordinate units. Reports describe software dispatch, not device ACKs.
// Do not mix with the untimed setter within a tracking session.
PID_API void       pid_set_applied_move_at_seconds(PID_Handle handle, double move, double dispatch_time_seconds);
// One-update control offset (e.g. upstream lead), excluded from motion learning.
PID_API void       pid_set_control_offset(PID_Handle handle, double offset);
// Rebase measurement/prediction on a semantic point change while retaining
// capture-aligned dispatched feedback and the current output/gain state.
PID_API void       pid_rebase_measurement(PID_Handle handle);
PID_API double     pid_update(PID_Handle handle, double error);
PID_API double     pid_update_with_prediction_state(PID_Handle handle, double error, int prediction_allowed);
PID_API void       pid_reset(PID_Handle handle);
PID_API void       pid_update_params(PID_Handle handle, double kp, double ki, double kd, double predict, double rate);
PID_API void       pid_set_high_speed_tuning(PID_Handle handle, double predict_boost, double feed_forward_gain);
PID_API void       pid_set_aux_limits(PID_Handle handle, double prediction_limit, double feed_forward_limit);
PID_API void       pid_get_diagnostics(PID_Handle handle, PID_Diagnostics *diagnostics);
#if defined(PID_ENABLE_TEST_HOOKS)
PID_API void       pid_test_set_frame_seconds(double frame_seconds);
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <stdexcept>
class PidController {
public:
    PidController() : handle_(pid_create()) {
        if (!handle_) {
            throw std::runtime_error("controller initialization failed");
        }
    }

    ~PidController() {
        if (handle_) {
            pid_destroy(handle_);
        }
    }

    PidController(const PidController &) = delete;
    PidController &operator=(const PidController &) = delete;

    PidController(PidController &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    PidController &operator=(PidController &&other) noexcept {
        if (this != &other) {
            if (handle_) {
                pid_destroy(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    int init(double kp, double ki, double kd, double predict, double rate) {
        return pid_init(handle_, kp, ki, kd, predict, rate);
    }

    void setBase(double kiMode, double kpLimit, double kiLimit, double kdLimit, double limit, double kiRate, double kiDeadband) {
        pid_set_base(handle_, kiMode, kpLimit, kiLimit, kdLimit, limit, kiRate, kiDeadband);
    }

    void setLatency(double latencySeconds) {
        pid_set_latency(handle_, latencySeconds);
    }

    void setObservationTimeSeconds(double observationTimeSeconds) {
        pid_set_observation_time_seconds(handle_, observationTimeSeconds);
    }

    void setAppliedMove(double move) {
        pid_set_applied_move(handle_, move);
    }

    void setAppliedMoveAtSeconds(double move, double dispatchTimeSeconds) {
        pid_set_applied_move_at_seconds(handle_, move, dispatchTimeSeconds);
    }

    void setControlOffset(double offset) {
        pid_set_control_offset(handle_, offset);
    }

    void rebaseMeasurement() {
        pid_rebase_measurement(handle_);
    }

    double update(double error) {
        return pid_update(handle_, error);
    }

    double update(double error, bool predictionAllowed) {
        return pid_update_with_prediction_state(handle_, error, predictionAllowed ? 1 : 0);
    }

    void reset() {
        pid_reset(handle_);
    }

    void updateParams(double kp, double ki, double kd, double predict, double rate) {
        pid_update_params(handle_, kp, ki, kd, predict, rate);
    }

    void setHighSpeedTuning(double predictBoost, double feedForwardGain) {
        pid_set_high_speed_tuning(handle_, predictBoost, feedForwardGain);
    }

    void setAuxLimits(double predictionLimit, double feedForwardLimit) {
        pid_set_aux_limits(handle_, predictionLimit, feedForwardLimit);
    }

    PID_Diagnostics diagnostics() const {
        PID_Diagnostics result{};
        pid_get_diagnostics(handle_, &result);
        return result;
    }

    PID_Handle handle() const {
        return handle_;
    }

private:
    PID_Handle handle_;
};

#endif

#endif
