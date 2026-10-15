
// 克里斯控制器 3314356530  
#include "chris_pid.h"
#define PID_DISABLE_THEMIDA_SDK 1
#if defined(PID_DISABLE_THEMIDA_SDK)
#define VM_FISH_WHITE_START
#define VM_FISH_WHITE_END
#else
#include "ThemidaSDK.h"
#endif

#if defined(_MSC_VER)
#define PID_NOINLINE PID_NOINLINE
#else
#define PID_NOINLINE __attribute__((noinline))
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>

// Android's <linux/wait.h> (pulled in through the C++ headers above) defines
// P_PID as a macro (=1, the waitid() target flag). The reference's controller
// class is also called P_PID, so the macro turns every `P_PID` in this file into
// `1`. Drop the macro here — nothing in this translation unit uses waitid().
#ifdef P_PID
#undef P_PID
#endif

namespace {

#if defined(PID_ENABLE_TEST_HOOKS)
thread_local double g_testFrameSeconds = 0.0;
#endif

constexpr double kNominalFrameSeconds = 1.0 / 144.0;
constexpr double kMinFrameSeconds = 0.001;
constexpr double kMaxFrameSeconds = 0.050;
constexpr double kFrameTimeSmoothing = 0.08;
constexpr double kMinFrameScale = 0.35;
constexpr double kMaxFrameScale = 2.50;
constexpr double kMinKalmanFrameInterval = 0.35;
constexpr double kMaxKalmanFrameInterval = 2.50;
constexpr double kErrorObserverProcessNoise = 0.25;
constexpr double kErrorObserverMeasurementNoise = 2.25;
constexpr double kErrorObserverVelocityRetention = 0.45;
constexpr double kThreeSigmaInnovationNis = 9.0;
constexpr double kMaxPredictionLatencySeconds = 0.120;
constexpr double kLatencyRiseSmoothing = 0.65;
constexpr double kLatencyFallSmoothing = 0.12;
constexpr double kMaxPredictionLookaheadFrames = 3.50;
constexpr double kMaximumInnovationCovarianceInflation = 5.0;
constexpr double kAdaptiveProcessNoiseMaxScale = 20.0;
constexpr double kAdaptiveProcessNoiseRiseRate = 0.72;
constexpr double kAdaptiveProcessNoiseFallRate = 0.22;
constexpr double kAdaptiveProcessNoiseIsolatedCap = 4.0;
constexpr double kIntegralLeakLog = -0.015113637810048184;
constexpr double kIntegralEpsilon = 0.000001;
constexpr double kMaxPidGain = 200.0;
constexpr double kMaxKiGain = 20.0;
constexpr double kMaxPredictGain = 6.0;
constexpr double kOutputGainThreshold = 640.0;
constexpr double kIntegralGainThreshold = 50.0;
constexpr double kIntegralGainRate = 0.025;
constexpr double kPredictionGainRate = 0.05;
constexpr double kOutputGainResetFloor = 0.35;
constexpr double kInitialOutputGainNear = 0.45;
constexpr double kInitialOutputGainFar = 0.70;
constexpr double kInitialOutputGainErrorStart = 8.0;
constexpr double kInitialOutputGainErrorFull = 80.0;
constexpr double kInvInitialOutputGainErrorRange = 1.0 / (kInitialOutputGainErrorFull - kInitialOutputGainErrorStart);
constexpr double kHighSpeedVelocityScale = 8.0;
constexpr double kHighSpeedAccelScale = 6.0;
constexpr double kDefaultHighSpeedPredictBoost = 0.0;
constexpr double kDefaultHighSpeedFeedForwardGain = 0.0;
constexpr double kDirectionChangeVelocityThreshold = 2.5;
constexpr double kHighSpeedReversalGateStrength = 5.2;
constexpr double kHighSpeedStopGateStrength = 3.6;
constexpr double kHighSpeedMemoryBleedStrength = 0.90;
constexpr double kHighSpeedMaxMemoryBleed = 0.90;
constexpr double kHighSpeedNearDistanceTrustFloor = 0.42;
constexpr double kHighSpeedDistanceTrustErrorScale = 22.0;
constexpr double kInvHighSpeedDistanceTrustErrorScale = 1.0 / kHighSpeedDistanceTrustErrorScale;
constexpr double kHighSpeedTrustRiseRate = 0.32;
constexpr double kHighSpeedTrustFallRate = 0.72;
constexpr double kHighSpeedIntegralMemoryBleedStrength = 0.95;
constexpr double kHighSpeedMaxIntegralMemoryBleed = 0.95;
constexpr double kVelocitySnrResponseStart = 1.35;
constexpr double kVelocitySnrResponseFull = 5.50;
constexpr double kVelocitySnrResponseMax = 0.72;
constexpr double kMicroStationaryErrorScale = 3.0;
constexpr double kMicroStationaryStepScale = 1.5;
constexpr double kMicroStationaryVelocityScale = 2.0;
constexpr double kMicroStationaryVelocityDamping = 0.55;
constexpr double kVelocityCoherenceSpeedScale = 2.2;
constexpr double kVelocityCoherenceRiseRate = 0.34;
constexpr double kVelocityCoherenceFallRate = 0.74;
constexpr double kVelocityCoherenceNeutralFloor = 0.30;
constexpr double kVelocityCoherenceLeadFloor = 0.10;
constexpr double kVelocityCoherenceDerivativeFloor = 0.48;
constexpr double kVelocityCoherenceVelocityFloor = 0.22;
constexpr double kVelocityCoherenceAccelerationFloor = 0.12;
constexpr double kVelocityCoherenceCenterScale = 20.0;

constexpr std::size_t kPredictionHistoryCapacity = 16;
constexpr std::size_t kPredictionHistoryMask = kPredictionHistoryCapacity - 1;
constexpr std::size_t kPredictionSamples = 6;
constexpr double kPredictionHistoryConfidenceRiseRate = 0.78;
constexpr double kPredictionHistoryConfidenceFallRate = 0.82;
constexpr double kPredictionWeights[kPredictionSamples] = {
    0.05, 0.09, 0.13, 0.17, 0.24, 0.32
};
static_assert((kPredictionHistoryCapacity & kPredictionHistoryMask) == 0, "Prediction history capacity must be a power of two");

double clamp01(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

double clampRange(double value, double minValue, double maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

double clampFinite(double value, double fallback, double minValue, double maxValue) {
    if (!std::isfinite(value)) {
        value = fallback;
    }
    return clampRange(value, minValue, maxValue);
}

double SoftLimit(double value, double baseLimit, double limit) {
    return std::atan2(value, baseLimit) * (baseLimit - limit);
}

double SoftLimitPrediction(double value, double baseLimit, double predictionLimit) {
    return std::atan2(value, baseLimit) * (baseLimit - predictionLimit * 0.1);
}

double LowFpsAdaptationScale(double frameSeconds) {
    constexpr double kAdaptationStartSeconds = 1.0 / 60.0;
    constexpr double kAdaptationFullSeconds = 1.0 / 30.0;
    const double blend = clamp01(
        (frameSeconds - kAdaptationStartSeconds) /
        (kAdaptationFullSeconds - kAdaptationStartSeconds));
    const double rawScale = std::clamp(
        frameSeconds / kNominalFrameSeconds,
        1.0,
        kMaxFrameScale);
    return 1.0 + (rawScale - 1.0) * blend;
}

double ScaleRateForLowFps(double rate, double frameSeconds) {
    rate = clamp01(rate);
    return 1.0 - std::pow(
        1.0 - rate,
        LowFpsAdaptationScale(frameSeconds));
}

double SoftLimitMagnitude(double value, double limit) {
    if (limit <= 0.0) {
        return value;
    }
    constexpr double kHalfPi = 1.57079632679489661923;
    return (limit / kHalfPi) * std::atan(value * kHalfPi / limit);
}

struct PidCoreInput {
    double currentError;
    double normalizedErrorDiff;
    double derivativeNoiseScale;
    double absErrorDiff;
    double integralTerm;
    double predictionVelocity;
    double predictionAcceleration;
    double predictionLookaheadFrames;
    double kp;
    double kd;
    double predictionGain;
    double highSpeedTrust;
    double auxiliaryLeadConfidence;
    double highSpeedPredictBoost;
    double highSpeedFeedForwardGain;
    double predictionLimit;
    double feedForwardLimit;
    double baseLimit;
    double kpLimit;
    double kiLimit;
    double kdLimit;
    double outputLimit;
    double kiDeadband;
    double outputGain;
    int kiMode;
};

struct AdaptiveTrackingState {
    double velocityMemoryScale = 1.0;
    double integralMemoryScale = 1.0;
    double smoothedTrust = 0.0;
};


PID_NOINLINE double ComputeProtectedPidCore(const PidCoreInput& input) {
    double output = 0.0;
    VM_FISH_WHITE_START
    {
        const double absError = std::fabs(input.currentError);
        const double nearBlend = std::fmin(1.0, std::fmax(0.0, (absError - 0.5) / 12.0));
        const double nearScale = 0.62 + 0.38 * nearBlend;
        const double farBlend = std::fmin(1.0, std::fmax(0.0, (absError - 24.0) / 80.0));
        const double farBoost = 1.0 + 0.28 * farBlend;
        const double nonlinearError = std::copysign(absError * nearScale * farBoost, input.currentError);

        const double nearFactor = std::exp(-absError / 12.0);
        const double stableError = std::exp(-input.absErrorDiff / 3.0);
        const double kpScale = 1.0 + 0.20 * input.highSpeedTrust;
        const double kdScale = 1.0 +
            0.40 * input.highSpeedTrust +
            0.20 * nearFactor * (1.0 - stableError);
        const double predictionScale =
            1.0 + input.highSpeedPredictBoost * input.auxiliaryLeadConfidence;
        double feedForwardTerm =
            input.predictionVelocity * input.highSpeedFeedForwardGain *
            input.auxiliaryLeadConfidence;

        double pTerm = nonlinearError * (input.kp * kpScale);
        double dTerm = input.normalizedErrorDiff *
            (input.kd * kdScale * input.derivativeNoiseScale);
        const double nearIntegral = nearFactor * std::sqrt(nearFactor);
        double integralScale = std::fmin(1.0, std::fmax(
            0.0,
            0.25 + 0.75 * nearIntegral * stableError));
        integralScale *= 1.0 - 0.65 * input.highSpeedTrust;
        double integralTerm = input.integralTerm * integralScale;
        const double scaledPredictionLookahead = input.predictionLookaheadFrames * predictionScale;
        const double predictionDisplacement =
            input.predictionVelocity * scaledPredictionLookahead +
            0.5 * input.predictionAcceleration *
                scaledPredictionLookahead * scaledPredictionLookahead;
        double predictionTerm = predictionDisplacement * input.predictionGain;

        if (input.kpLimit != 0.0) {
            pTerm = SoftLimit(pTerm, input.baseLimit, input.kpLimit);
        }
        if (input.kiLimit != 0.0 && integralTerm != 0.0) {
            integralTerm = SoftLimit(integralTerm, input.baseLimit, input.kiLimit);
        }
        if (input.kdLimit != 0.0 && dTerm != 0.0) {
            dTerm = SoftLimit(dTerm, input.baseLimit, input.kdLimit);
        }
        if (input.predictionLimit != 0.0 && predictionTerm != 0.0) {
            predictionTerm = SoftLimitPrediction(
                predictionTerm,
                input.baseLimit,
                input.predictionLimit);
        }
        if (input.feedForwardLimit != 0.0 && feedForwardTerm != 0.0) {
            feedForwardTerm = SoftLimitMagnitude(feedForwardTerm, input.feedForwardLimit);
        }

        const double predictionOutsideDeadband = static_cast<double>(
            std::fabs(predictionTerm) > input.kiDeadband * 0.5);
        predictionTerm *= predictionOutsideDeadband;

        const double integralModeEnabled = static_cast<double>(input.kiMode <= 1);
        double iTotal = integralTerm * integralModeEnabled;
        const double integralOutsideDeadband = static_cast<double>(
            std::fabs(iTotal) > input.kiDeadband);
        iTotal *= integralOutsideDeadband;

        const double gatedAuxiliaryOutput =
            (iTotal + dTerm + predictionTerm + feedForwardTerm) *
            input.outputGain;
        output = pTerm + gatedAuxiliaryOutput;
        if (input.outputLimit != 0.0 && output != 0.0) {
            output = SoftLimit(output, input.baseLimit, input.outputLimit);
        }
    }
    VM_FISH_WHITE_END
    return output;
}

struct KalmanFilter {
    double q = 1.0;
    double r = 1.0;
    double x = 0.0;
    double p = 1.0;
    double innovationNis = 0.0;
    double covarianceInflation = 1.0;
    double adaptiveProcessNoiseScale = 1.0;
    double robustWeight = 1.0;
    int innovationRun = 0;
    double previousInnovation = 0.0;
    double previousInnovationNis = 0.0;

    KalmanFilter() = default;

    explicit KalmanFilter(double processNoise, double measurementNoise = 1.0, double covariance = 1.0)
        : q(processNoise), r(measurementNoise), x(0.0), p(covariance) {}

    double update(
        double measurement,
        double frameSeconds = kNominalFrameSeconds,
        bool adaptToInnovation = false) {
        const double normalizedFrameInterval = clampRange(
            frameSeconds / kNominalFrameSeconds,
            kMinKalmanFrameInterval,
            kMaxKalmanFrameInterval);
        const double basePredictedP = p + q * normalizedFrameInterval;
        const double innovation = measurement - x;
        const double baselineInnovationVariance = std::max(
            basePredictedP + r,
            kIntegralEpsilon);
        const double baselineInnovationNis =
            innovation * innovation / baselineInnovationVariance;
        double predictedP = basePredictedP;
        covarianceInflation = 1.0;
        robustWeight = 1.0;
        if (adaptToInnovation) {
            constexpr double kThreeSigmaNis = 9.0;
            const bool significantBaselineInnovation =
                baselineInnovationNis >= kThreeSigmaNis;
            const bool previousInnovationWasSignificant =
                previousInnovationNis >= kThreeSigmaNis;
            const bool sameInnovationDirection =
                innovation * previousInnovation > 0.0;
            const bool persistentBaselineManeuver =
                significantBaselineInnovation &&
                previousInnovationWasSignificant &&
                sameInnovationDirection;
            const double innovationSigma = std::sqrt(baselineInnovationNis);
            const double maneuverEvidence = clamp01(
                (innovationSigma - 1.25) / (4.50 - 1.25));
            double targetProcessScale = 1.0 +
                (kAdaptiveProcessNoiseMaxScale - 1.0) *
                maneuverEvidence * maneuverEvidence;
            if (significantBaselineInnovation && !persistentBaselineManeuver) {
                targetProcessScale = std::min(
                    targetProcessScale,
                    kAdaptiveProcessNoiseIsolatedCap);
            }
            const double processNoiseRate = targetProcessScale >
                adaptiveProcessNoiseScale
                ? kAdaptiveProcessNoiseRiseRate
                : kAdaptiveProcessNoiseFallRate;
            const double scaledProcessNoiseRate = ScaleRateForLowFps(
                processNoiseRate,
                frameSeconds);
            adaptiveProcessNoiseScale = clampRange(
                adaptiveProcessNoiseScale +
                    (targetProcessScale - adaptiveProcessNoiseScale) *
                    scaledProcessNoiseRate,
                1.0,
                kAdaptiveProcessNoiseMaxScale);
            predictedP = p + q * adaptiveProcessNoiseScale *
                normalizedFrameInterval;
        } else {
            adaptiveProcessNoiseScale = 1.0;
        }
        const double innovationVariance = std::max(
            predictedP + r,
            kIntegralEpsilon);
        innovationNis = innovation * innovation / innovationVariance;
        if (adaptToInnovation) {
            constexpr double kThreeSigmaNis = 9.0;

            const bool significantInnovation =
                baselineInnovationNis >= kThreeSigmaNis;
            const bool previousInnovationWasSignificant =
                previousInnovationNis >= kThreeSigmaNis;
            const bool sameInnovationDirection =
                innovation * previousInnovation > 0.0;
            innovationRun = significantInnovation
                ? (sameInnovationDirection && previousInnovationWasSignificant
                    ? std::min(innovationRun + 1, 1000)
                    : 1)
                : 0;

            const bool largeInnovation = baselineInnovationNis > kThreeSigmaNis;
            const bool persistentManeuver = largeInnovation && innovationRun >= 2;
            if (persistentManeuver) {
                covarianceInflation = clampRange(
                    baselineInnovationNis / kThreeSigmaNis,
                    1.0,
                    kMaximumInnovationCovarianceInflation);
                predictedP *= covarianceInflation;
            } else if (largeInnovation) {
                robustWeight = std::sqrt(
                    kThreeSigmaNis / baselineInnovationNis);
            }
        }
        const double effectiveMeasurementNoise =
            r / std::max(robustWeight * robustWeight, kIntegralEpsilon);
        const double gain = predictedP / (predictedP + effectiveMeasurementNoise);
        x += innovation * gain;
        p = (1.0 - gain) * predictedP;
        previousInnovation = innovation;
        previousInnovationNis = adaptToInnovation
            ? baselineInnovationNis
            : innovationNis;
        return x;
    }

    double uncertaintyScale(double frameSeconds) const {
        const double normalizedFrameInterval = clampRange(
            frameSeconds / kNominalFrameSeconds,
            kMinKalmanFrameInterval,
            kMaxKalmanFrameInterval);
        const double processVariance = q * normalizedFrameInterval;
        const double steadyStateVariance = 0.5 * (
            std::sqrt(
                processVariance * processVariance +
                4.0 * r * processVariance) -
            processVariance);
        if (steadyStateVariance <= kIntegralEpsilon) {
            return 1.0;
        }
        return clampRange(
            std::sqrt(std::max(p, 0.0) / steadyStateVariance),
            1.0,
            2.0);
    }

    void reset() {
        x = 0.0;
        p = 0.0;
        innovationNis = 0.0;
        covarianceInflation = 1.0;
        adaptiveProcessNoiseScale = 1.0;
        robustWeight = 1.0;
        innovationRun = 0;
        previousInnovation = 0.0;
        previousInnovationNis = 0.0;
    }
};

struct ErrorObserver {
    bool initialized = false;
    double position = 0.0;
    double velocity = 0.0;
    double positionVariance = kErrorObserverMeasurementNoise;
    double positionVelocityCovariance = 0.0;
    double velocityVariance = kErrorObserverProcessNoise;
    double innovationNis = 0.0;
    double robustWeight = 1.0;
    double covarianceInflation = 1.0;
    int innovationRun = 0;
    double previousInnovation = 0.0;
    double previousInnovationNis = 0.0;

    double update(double measurement, double appliedMove, double frameSeconds, bool acceptMeasurement = true) {
        const double dt = clampRange(
            frameSeconds / kNominalFrameSeconds,
            kMinKalmanFrameInterval,
            kMaxKalmanFrameInterval);
        if (!initialized) {
            initialized = true;
            position = measurement;
            velocity = 0.0;
            positionVariance = kErrorObserverMeasurementNoise;
            positionVelocityCovariance = 0.0;
            velocityVariance = kErrorObserverProcessNoise;
            return position;
        }

        const double velocityRetention = std::pow(
            kErrorObserverVelocityRetention, dt);
        const double priorPosition = position;
        position += velocity * dt - appliedMove;
        velocity *= velocityRetention;

        const double dt2 = dt * dt;
        const double dt3 = dt2 * dt;
        const double dt4 = dt2 * dt2;
        double predictedPositionVariance =
            positionVariance +
            2.0 * dt * positionVelocityCovariance +
            dt2 * velocityVariance +
            0.25 * kErrorObserverProcessNoise * dt4;
        double predictedPositionVelocityCovariance =
            velocityRetention * (
                positionVelocityCovariance + dt * velocityVariance) +
            0.5 * kErrorObserverProcessNoise * dt3;
        double predictedVelocityVariance =
            velocityRetention * velocityRetention * velocityVariance +
            kErrorObserverProcessNoise * dt2;

        if (!acceptMeasurement) {
            if (priorPosition * position < 0.0) {
                position = 0.0;
                velocity = 0.0;
            }
            positionVariance = std::max(predictedPositionVariance, 0.0);
            positionVelocityCovariance = predictedPositionVelocityCovariance;
            velocityVariance = std::max(predictedVelocityVariance, 0.0);
            innovationNis = 0.0;
            robustWeight = 1.0;
            covarianceInflation = 1.0;
            return position;
        }

        const double innovation = measurement - position;
        const double innovationVariance = std::max(
            predictedPositionVariance + kErrorObserverMeasurementNoise,
            kIntegralEpsilon);
        innovationNis = innovation * innovation / innovationVariance;
        const bool significantInnovation =
            innovationNis >= kThreeSigmaInnovationNis;
        const bool previousInnovationWasSignificant =
            previousInnovationNis >= kThreeSigmaInnovationNis;
        const bool sameInnovationDirection =
            innovation * previousInnovation > 0.0;
        innovationRun = significantInnovation
            ? (sameInnovationDirection && previousInnovationWasSignificant
                ? std::min(innovationRun + 1, 1000)
                : 1)
            : 0;

        robustWeight = 1.0;
        covarianceInflation = 1.0;
        if (significantInnovation && innovationRun >= 2) {
            covarianceInflation = clampRange(
                innovationNis / kThreeSigmaInnovationNis,
                1.0,
                kMaximumInnovationCovarianceInflation);
            predictedPositionVariance *= covarianceInflation;
            predictedPositionVelocityCovariance *= covarianceInflation;
            predictedVelocityVariance *= covarianceInflation;
        } else if (significantInnovation) {
            robustWeight = std::sqrt(
                kThreeSigmaInnovationNis / innovationNis);
        }

        const double acceptedError = position + robustWeight * innovation;

        const double effectiveMeasurementNoise =
            kErrorObserverMeasurementNoise /
            std::max(robustWeight * robustWeight, kIntegralEpsilon);
        const double posteriorInnovationVariance = std::max(
            predictedPositionVariance + effectiveMeasurementNoise,
            kIntegralEpsilon);
        const double positionGain =
            predictedPositionVariance / posteriorInnovationVariance;
        const double velocityGain =
            predictedPositionVelocityCovariance / posteriorInnovationVariance;
        position += positionGain * innovation;
        velocity += velocityGain * innovation;

        const double priorPositionVelocityCovariance =
            predictedPositionVelocityCovariance;
        positionVariance = std::max(
            (1.0 - positionGain) * predictedPositionVariance,
            0.0);
        positionVelocityCovariance =
            (1.0 - positionGain) * predictedPositionVelocityCovariance;
        velocityVariance = std::max(
            predictedVelocityVariance -
                velocityGain * priorPositionVelocityCovariance,
            0.0);
        previousInnovation = innovation;
        previousInnovationNis = innovationNis;

        return acceptedError;
    }

    double uncertaintyScale() const {
        return clampRange(
            std::sqrt(
                1.0 + positionVariance /
                    std::max(kErrorObserverMeasurementNoise, kIntegralEpsilon)),
            1.0,
            2.0);
    }

    void reset() {
        initialized = false;
        position = 0.0;
        velocity = 0.0;
        positionVariance = kErrorObserverMeasurementNoise;
        positionVelocityCovariance = 0.0;
        velocityVariance = kErrorObserverProcessNoise;
        innovationNis = 0.0;
        robustWeight = 1.0;
        covarianceInflation = 1.0;
        innovationRun = 0;
        previousInnovation = 0.0;
        previousInnovationNis = 0.0;
    }
};

class P_PID {
public:
    using Clock = std::chrono::steady_clock;

    P_PID()
        : kp_(0.0),
          ki_(0.0),
          kd_(0.0),
          baseLimit_(10000.0),
          kpLimit_(0.0),
          kiLimit_(0.0),
          kdLimit_(0.0),
          outputLimit_(0.0),
          predict_(0.0),
          kiDeadband_(0.3),
          kiMode_(0),
          integral_(0.0),
          lastError_(0.0),
          lastRawError_(0.0),
          hasLastRawError_(false),
          lastOutput_(0.0),
          lastUpdateTime_(),
          hasLastUpdateTime_(false),
          smoothedFrameSeconds_(kNominalFrameSeconds),
          observationTimeHintSeconds_(0.0),
          lastObservationTimeSeconds_(0.0),
          hasObservationTimeHint_(false),
          hasLastObservationTime_(false),
          predictionLatencySeconds_(kNominalFrameSeconds),
          hasLatencyHint_(false),
          outputGain_(kOutputGainResetFloor),
          outputGainRate_(0.03),
          iGain_(0.0),
          predictionGain_(0.0),
          highSpeedPredictBoost_(kDefaultHighSpeedPredictBoost),
          highSpeedFeedForwardGain_(kDefaultHighSpeedFeedForwardGain),
          predictionLimit_(0.0),
          feedForwardLimit_(0.0),
          auxLimitsExplicit_(false),
          highSpeedTrust_(0.0),
          predictionHead_(0),
          predictionMoveCount_(0),
          predictionHistoryConfidence_(0.0),
          lastPredictionVelocity_(0.0),
          lastCoherenceVelocity_(0.0),
          hasLastCoherenceVelocity_(false),
          velocityCoherence_(1.0),
          tuningStateRefreshPending_(false),
          diagnostics_(),
          errorObserver_(),
          derivativeFilter_(1.0),
          derivativeSmoothFilter_(1.0) {
        resetPredictionHistory();
    }

    bool init(double kp, double ki, double kd, double predict, double rate) {
        baseLimit_ = 10000.0;
        kpLimit_ = 9000.0;
        kiLimit_ = 1000.0;
        kdLimit_ = 9000.0;
        outputLimit_ = 0.0;
        kiDeadband_ = 0.3;
        kiMode_ = 1;
        derivativeFilter_.q = 0.1;
        derivativeSmoothFilter_.q = 0.1;
        predictionLimit_ = kiLimit_;
        feedForwardLimit_ = 0.0;
        auxLimitsExplicit_ = false;
        highSpeedTrust_ = 0.0;
        lastCoherenceVelocity_ = 0.0;
        hasLastCoherenceVelocity_ = false;
        velocityCoherence_ = 1.0;

        applyTuning(kp, ki, kd, predict, rate);
        return true;
    }

    void setBase(int kiMode, double kpLimit, double kiLimit, double kdLimit, double limit, double kiRate, double kiDeadband) {
        kiMode_ = std::clamp(kiMode, 0, 2);
        kpLimit_ = clampFinite(kpLimit, kpLimit_, 0.0, baseLimit_);
        kiLimit_ = clampFinite(kiLimit, kiLimit_, 0.0, baseLimit_);
        if (!auxLimitsExplicit_) {
            predictionLimit_ = kiLimit_;
        }
        kdLimit_ = clampFinite(kdLimit, kdLimit_, 0.0, baseLimit_);
        outputLimit_ = clampFinite(limit, outputLimit_, 0.0, baseLimit_);
        derivativeFilter_.q = clampFinite(kiRate, derivativeFilter_.q, 0.0001, 1.0);
        kiDeadband_ = clampFinite(kiDeadband, kiDeadband_, 0.0, 20.0);
    }

    PID_NOINLINE double update(double error, bool predictionAllowed = true) {
        if (!std::isfinite(error)) {
            reset();
            return 0.0;
        }

        const auto now = Clock::now();

        double currentError = error;
        if (std::fabs(currentError) < 0.3) {
            currentError = 0.0;
        }

        const double rawError = currentError;
        const bool tuningStateRefresh = tuningStateRefreshPending_;
        if (tuningStateRefresh) {

            tuningStateRefreshPending_ = false;
            lastError_ = currentError;
            lastRawError_ = rawError;
            hasLastRawError_ = true;
            lastOutput_ = 0.0;
        }

        const double frameSeconds = updateFrameTiming(now);
        const double measurementFrameSeconds = consumeMeasurementFrameSeconds(frameSeconds);
        const double integralFrameScale = frameScale(frameSeconds, smoothedFrameSeconds_);

        const double derivativeFrameScale = frameScale(
            smoothedFrameSeconds_, measurementFrameSeconds);
        const bool canUsePreviousRawError =
            hasLastRawError_ || tuningStateRefresh;

        currentError = errorObserver_.update(
            rawError,
            lastOutput_,
            measurementFrameSeconds,
            predictionAllowed);

        const double errorDiff = currentError - lastError_;
        const double absCurrentError = std::fabs(currentError);
        const double absErrorDiff = std::fabs(errorDiff);
        const double normalizedErrorDiff = errorDiff * derivativeFrameScale;
        if (predictionAllowed) {
            prepareIntegralForError(currentError);
            iGain_ = adjustGain(
                iGain_,
                absCurrentError,
                kIntegralGainThreshold,
                kIntegralGainRate,
                frameSeconds);
            predictionGain_ = adjustGain(
                predictionGain_,
                absCurrentError,
                kIntegralGainThreshold,
                kPredictionGainRate,
                frameSeconds);
            if (!canUsePreviousRawError) {
                outputGain_ = std::max(outputGain_, initialOutputGainForError(absCurrentError));
            }
            outputGain_ = adjustGain(
                outputGain_,
                absCurrentError,
                kOutputGainThreshold,
                outputGainRate_,
                frameSeconds);
        } else if (!canUsePreviousRawError) {
            outputGain_ = std::max(outputGain_, initialOutputGainForError(absCurrentError));
        }

        double predictionVelocity = 0.0;
        double predictionLookahead = 0.0;
        double predictionHistoryResponse = 0.0;
        double velocityShrinkage = 0.0;
        double instantaneousVelocity = 0.0;
        double velocityCoherenceGain = 1.0;
        const double previousPredictionVelocity = lastPredictionVelocity_;
        const bool predictionEnabled = predictionAllowed && predictionVelocityEnabled();
        if (predictionEnabled) {

            const double rawVelocity =
                (errorDiff + lastOutput_) * derivativeFrameScale;
            instantaneousVelocity = rawVelocity;
            velocityCoherenceGain = updateVelocityCoherenceGain(
                instantaneousVelocity,
                absCurrentError,
                measurementFrameSeconds);
            double filteredVelocity = derivativeFilter_.update(
                rawVelocity,
                measurementFrameSeconds,
                true);
            if (absCurrentError < 1.0 && absErrorDiff < 0.1) {
                const double nearZeroVelocity =
                    (errorDiff + lastOutput_ * 0.5) * derivativeFrameScale;
                filteredVelocity = filteredVelocity * 0.35 + nearZeroVelocity * 0.65;
            }

            double smoothedVelocity = derivativeSmoothFilter_.update(
                filteredVelocity,
                measurementFrameSeconds);
            const double velocityPower =
                smoothedVelocity * smoothedVelocity;
            velocityShrinkage = velocityPower / (
                velocityPower +
                std::max(derivativeSmoothFilter_.p, kIntegralEpsilon));
            smoothedVelocity *= velocityShrinkage;

            const double historyVelocity =
                updatePredictionVelocity(
                    smoothedVelocity, measurementFrameSeconds);
            const double covarianceResponse = clamp01(
                (derivativeFilter_.covarianceInflation - 1.0) /
                (kMaximumInnovationCovarianceInflation - 1.0));
            const double velocityDisagreement =
                std::fabs(smoothedVelocity - historyVelocity);
            const double disagreementUncertainty = std::sqrt(std::max(
                derivativeFilter_.p + derivativeSmoothFilter_.p + derivativeFilter_.r,
                kIntegralEpsilon));
            const double disagreementResponse =
                derivativeFilter_.robustWeight < 0.999
                ? 0.0
                : clamp01(
                    1.0 - disagreementUncertainty /
                    std::max(velocityDisagreement, kIntegralEpsilon));
            const double velocitySignalToNoise =
                std::fabs(smoothedVelocity) / disagreementUncertainty;
            const double coherentDirection =
                (historyVelocity == 0.0 ||
                 smoothedVelocity == 0.0 ||
                 sameSign(smoothedVelocity, historyVelocity))
                ? 1.0
                : 0.0;
            const double cruiseObserverResponse =
                derivativeFilter_.robustWeight < 0.999
                ? 0.0
                : coherentDirection * kVelocitySnrResponseMax * clamp01(
                    (velocitySignalToNoise - kVelocitySnrResponseStart) /
                    (kVelocitySnrResponseFull - kVelocitySnrResponseStart));
            predictionHistoryResponse =
                std::max(
                    std::max(covarianceResponse, disagreementResponse),
                    cruiseObserverResponse);
            predictionVelocity = historyVelocity +
                (smoothedVelocity - historyVelocity) * predictionHistoryResponse;
            predictionLookahead = predictionLookaheadFrames();
        }

        AdaptiveTrackingState adaptive{};
        adaptive.smoothedTrust = highSpeedTrust_;
        if (predictionEnabled) {
            adaptive = computeAdaptiveTracking(
                absCurrentError,
                predictionVelocity,
                previousPredictionVelocity,
                highSpeedTrust_,
                derivativeFilter_.uncertaintyScale(measurementFrameSeconds),
                measurementFrameSeconds);
            highSpeedTrust_ = adaptive.smoothedTrust;
            if (adaptive.integralMemoryScale < 0.999) {
                integral_ *= adaptive.integralMemoryScale;
                iGain_ *= adaptive.integralMemoryScale;
            }
            if (adaptive.velocityMemoryScale < 0.999) {
                scalePredictionMemory(adaptive.velocityMemoryScale);
            }
        } else {
            velocityCoherenceGain = velocityCoherence_;
        }
        const double coherenceVelocityScale =
            kVelocityCoherenceVelocityFloor +
            (1.0 - kVelocityCoherenceVelocityFloor) * velocityCoherenceGain;
        const double coherenceDerivativeScale =
            kVelocityCoherenceDerivativeFloor +
            (1.0 - kVelocityCoherenceDerivativeFloor) * velocityCoherenceGain;
        const double coherenceAccelerationScale =
            kVelocityCoherenceAccelerationFloor +
            (1.0 - kVelocityCoherenceAccelerationFloor) * velocityCoherenceGain;
        const double coherenceLeadScale =
            kVelocityCoherenceLeadFloor +
            (1.0 - kVelocityCoherenceLeadFloor) * velocityCoherenceGain;

        const double derivativeNoiseScale = clampRange(
            (0.35 + 0.65 * clamp01(derivativeFilter_.robustWeight)) /
            derivativeFilter_.uncertaintyScale(measurementFrameSeconds),
            0.25,
            1.0);
        double controlPredictionVelocity =
            predictionVelocity * adaptive.velocityMemoryScale * coherenceVelocityScale;
        const double microStationaryEvidence =
            std::exp(-absCurrentError / kMicroStationaryErrorScale) *
            std::exp(-absErrorDiff / kMicroStationaryStepScale);
        const double microMotionEvidence =
            1.0 - std::exp(
                -std::fabs(controlPredictionVelocity) /
                kMicroStationaryVelocityScale);
        const double microVelocityScale =
            1.0 -
            kMicroStationaryVelocityDamping *
            microStationaryEvidence *
            (1.0 - microMotionEvidence);
        controlPredictionVelocity *= microVelocityScale;
        const double predictionAcceleration =
            predictionVelocity - previousPredictionVelocity;
        const double accelerationUncertainty = std::sqrt(std::max(
            derivativeFilter_.p + derivativeSmoothFilter_.p,
            kIntegralEpsilon));
        const double predictionAccelerationConfidence =
            derivativeFilter_.robustWeight < 0.999
            ? 0.0
            : clamp01(
                1.0 - accelerationUncertainty /
                std::max(std::fabs(predictionAcceleration), kIntegralEpsilon));
        const double controlPredictionAcceleration =
            predictionAcceleration * predictionAccelerationConfidence *
            adaptive.velocityMemoryScale * coherenceAccelerationScale;
        double auxiliaryLeadConfidence = 0.0;
        if (predictionEnabled &&
            std::fabs(controlPredictionVelocity) > kIntegralEpsilon) {
            const double directionProduct =
                controlPredictionVelocity * instantaneousVelocity;
            auxiliaryLeadConfidence = 1.0;
            if (directionProduct < 0.0) {
                const double directionalVariance = std::max(
                    derivativeFilter_.r + derivativeFilter_.p +
                        derivativeSmoothFilter_.p,
                    kIntegralEpsilon);
                auxiliaryLeadConfidence = std::exp(
                    directionProduct / directionalVariance);
            }
            auxiliaryLeadConfidence *= coherenceLeadScale;
        }

        const double integralTerm = predictionAllowed
            ? updateIntegralTerm(
                currentError,
                integralFrameScale,
                frameSeconds)
            : integral_;
        const PidCoreInput coreInput{
            currentError,
            normalizedErrorDiff * coherenceDerivativeScale,
            derivativeNoiseScale,
            absErrorDiff,
            integralTerm,
            controlPredictionVelocity,
            controlPredictionAcceleration,
            predictionLookahead,
            kp_,
            kd_,
            predictionGain_,
            highSpeedTrust_,
            auxiliaryLeadConfidence,
            highSpeedPredictBoost_,
            highSpeedFeedForwardGain_,
            predictionLimit_,
            feedForwardLimit_,
            baseLimit_,
            kpLimit_,
            kiLimit_,
            kdLimit_,
            outputLimit_,
            kiDeadband_,
            outputGain_,
            kiMode_
        };
        double output = ComputeProtectedPidCore(coreInput);
        double lowFpsOutputScale = 1.0;
        constexpr double kLowFpsScaleStartSeconds = 1.0 / 30.0;
        if (frameSeconds > kLowFpsScaleStartSeconds && output * currentError > 0.0) {
            lowFpsOutputScale = clampRange(
                frameSeconds / kLowFpsScaleStartSeconds,
                1.0,
                1.50);
            output *= lowFpsOutputScale;
        }

        double feedForwardOutput =
            controlPredictionVelocity * highSpeedFeedForwardGain_ *
            auxiliaryLeadConfidence;
        if (feedForwardLimit_ != 0.0 && feedForwardOutput != 0.0) {
            feedForwardOutput = SoftLimitMagnitude(
                feedForwardOutput,
                feedForwardLimit_);
        }
        feedForwardOutput *= outputGain_ * lowFpsOutputScale;

        const double predictionScale =
            1.0 + highSpeedPredictBoost_ * auxiliaryLeadConfidence;
        const double scaledPredictionLookahead =
            predictionLookahead * predictionScale;
        const double predictionAccelerationTerm =
            0.5 * controlPredictionAcceleration *
            scaledPredictionLookahead * scaledPredictionLookahead * predictionGain_;
        double predictionOutput = (
            controlPredictionVelocity * scaledPredictionLookahead +
            0.5 * controlPredictionAcceleration *
                scaledPredictionLookahead * scaledPredictionLookahead) *
            predictionGain_;
        if (predictionLimit_ != 0.0 && predictionOutput != 0.0) {
            predictionOutput = SoftLimitPrediction(
                predictionOutput,
                baseLimit_,
                predictionLimit_);
        }
        if (std::fabs(predictionOutput) <= kiDeadband_ * 0.5) {
            predictionOutput = 0.0;
        }
        predictionOutput *= outputGain_ * lowFpsOutputScale;

        diagnostics_.raw_error = rawError;
        diagnostics_.posterior_error = currentError;
        diagnostics_.feedback_output = output - feedForwardOutput;
        diagnostics_.feed_forward_output = feedForwardOutput;
        diagnostics_.estimated_velocity = predictionVelocity;
        diagnostics_.estimated_acceleration =
            predictionVelocity - previousPredictionVelocity;
        diagnostics_.measurement_variance = errorObserver_.positionVariance;
        diagnostics_.observer_innovation_nis = errorObserver_.innovationNis;
        diagnostics_.observer_robust_weight = errorObserver_.robustWeight;
        diagnostics_.observer_innovation_run = errorObserver_.innovationRun;
        diagnostics_.observer_covariance_inflation =
            errorObserver_.covarianceInflation;
        diagnostics_.observer_uncertainty_scale =
            errorObserver_.uncertaintyScale();
        diagnostics_.observer_velocity_variance = errorObserver_.velocityVariance;
        diagnostics_.velocity_process_noise_scale =
            derivativeFilter_.adaptiveProcessNoiseScale;
        diagnostics_.observer_velocity_shrinkage = velocityShrinkage;
        diagnostics_.prediction_history_response = predictionHistoryResponse;
        diagnostics_.prediction_maneuver_scale = adaptive.velocityMemoryScale;
        diagnostics_.prediction_acceleration_confidence =
            predictionAccelerationConfidence;
        diagnostics_.prediction_acceleration_term =
            predictionAccelerationTerm * outputGain_ * lowFpsOutputScale;
        diagnostics_.prediction_lookahead_frames = predictionLookahead;
        diagnostics_.auxiliary_lead_confidence = auxiliaryLeadConfidence;
        diagnostics_.high_speed_trust = highSpeedTrust_;
        diagnostics_.output_gain = outputGain_;
        diagnostics_.integral_gain = iGain_;
        diagnostics_.prediction_gain = predictionGain_;
        diagnostics_.integral_term = integralTerm;
        diagnostics_.prediction_term = predictionOutput;
        diagnostics_.frame_seconds = frameSeconds;
        diagnostics_.smoothed_frame_seconds = smoothedFrameSeconds_;
        diagnostics_.output = output;
        diagnostics_.velocity_memory_scale =
            adaptive.velocityMemoryScale / derivativeFilter_.covarianceInflation;
        diagnostics_.integral_memory_scale = adaptive.integralMemoryScale;
        diagnostics_.hard_reset = 0;
        diagnostics_.transient_reset = 0;
        diagnostics_.prediction_allowed = predictionAllowed ? 1 : 0;
        diagnostics_.prediction_enabled = predictionEnabled ? 1 : 0;

        lastError_ = currentError;
        lastRawError_ = rawError;
        hasLastRawError_ = true;

        return output;
    }

    void reset() {
        outputGain_ = kOutputGainResetFloor;
        iGain_ = 0.0;
        predictionGain_ = 0.0;
        integral_ = 0.0;
        lastError_ = 0.0;
        lastRawError_ = 0.0;
        hasLastRawError_ = false;
        lastOutput_ = 0.0;
        hasLastUpdateTime_ = false;
        smoothedFrameSeconds_ = kNominalFrameSeconds;
        observationTimeHintSeconds_ = 0.0;
        lastObservationTimeSeconds_ = 0.0;
        hasObservationTimeHint_ = false;
        hasLastObservationTime_ = false;
        highSpeedTrust_ = 0.0;
        lastCoherenceVelocity_ = 0.0;
        hasLastCoherenceVelocity_ = false;
        velocityCoherence_ = 1.0;
        tuningStateRefreshPending_ = false;
        resetPredictionHistory();
        errorObserver_.reset();
        derivativeFilter_.reset();
        derivativeSmoothFilter_.reset();
        diagnostics_ = {};
    }

    void updateParams(double kp, double ki, double kd, double predict, double rate) {
        applyTuning(kp, ki, kd, predict, rate);
    }

    void setHighSpeedTuning(double predictBoost, double feedForwardGain) {
        const bool wasPredictionVelocityEnabled = predictionVelocityEnabled();
        highSpeedPredictBoost_ = clampFinite(predictBoost, kDefaultHighSpeedPredictBoost, 0.0, 3.0);
        highSpeedFeedForwardGain_ = clampFinite(feedForwardGain, kDefaultHighSpeedFeedForwardGain, 0.0, 1.0);
        if (wasPredictionVelocityEnabled != predictionVelocityEnabled()) {
            resetPredictionTracking();
        }
    }

    void setAuxLimits(double predictionLimit, double feedForwardLimit) {
        predictionLimit_ = clampFinite(predictionLimit, predictionLimit_, 0.0, baseLimit_);
        feedForwardLimit_ = clampFinite(feedForwardLimit, feedForwardLimit_, 0.0, baseLimit_);
        auxLimitsExplicit_ = true;
    }

    void setLatencySeconds(double latencySeconds) {
        if (!std::isfinite(latencySeconds)) {
            return;
        }

        const double clampedLatency = clampRange(
            latencySeconds,
            kNominalFrameSeconds,
            kMaxPredictionLatencySeconds
        );
        if (!hasLatencyHint_) {
            predictionLatencySeconds_ = clampedLatency;
            hasLatencyHint_ = true;
            return;
        }
        const double latencyDelta =
            clampedLatency - predictionLatencySeconds_;
        const double smoothing = latencyDelta > 0.0
            ? kLatencyRiseSmoothing
            : kLatencyFallSmoothing;
        predictionLatencySeconds_ += latencyDelta * smoothing;
    }

    void setObservationTimeSeconds(double observationTimeSeconds) {
        if (!std::isfinite(observationTimeSeconds)) {
            return;
        }
        observationTimeHintSeconds_ = observationTimeSeconds;
        hasObservationTimeHint_ = true;
    }

    void setAppliedMove(double move) {
        lastOutput_ = std::isfinite(move) ? move : 0.0;
    }

    PID_Diagnostics diagnostics() const {
        return diagnostics_;
    }

private:
    double consumeMeasurementFrameSeconds(double fallbackFrameSeconds) {
        if (!hasObservationTimeHint_) {
            return fallbackFrameSeconds;
        }

        hasObservationTimeHint_ = false;
        const double observationTimeSeconds = observationTimeHintSeconds_;
        if (!hasLastObservationTime_) {
            lastObservationTimeSeconds_ = observationTimeSeconds;
            hasLastObservationTime_ = true;
            return fallbackFrameSeconds;
        }

        const double elapsedSeconds =
            observationTimeSeconds - lastObservationTimeSeconds_;
        if (elapsedSeconds <= 0.0) {

            return fallbackFrameSeconds;
        }

        lastObservationTimeSeconds_ = observationTimeSeconds;
        return elapsedSeconds <= 0.1 ? elapsedSeconds : fallbackFrameSeconds;
    }

    void applyTuning(double kp, double ki, double kd, double predict, double rate) {
        const double nextKp = clampFinite(kp, kp_, 0.0, kMaxPidGain);
        const double nextKi = clampFinite(ki, ki_, 0.0, kMaxKiGain);
        const double nextKd = clampFinite(kd, kd_, 0.0, kMaxPidGain);
        const double nextPredict = clampFinite(predict, predict_, 0.0, kMaxPredictGain);
        const double nextRate = clampFinite(rate, outputGainRate_, 0.0, 1.0);
        const bool wasPredictionEnabled = predictionVelocityEnabled();
        const bool kiChanged = std::fabs(nextKi - ki_) > kIntegralEpsilon;
        const bool kdChanged = std::fabs(nextKd - kd_) > kIntegralEpsilon;
        const bool predictChanged = std::fabs(nextPredict - predict_) > kIntegralEpsilon;

        kp_ = nextKp;
        ki_ = nextKi;
        kd_ = nextKd;
        predict_ = nextPredict;
        outputGainRate_ = nextRate;

        if (kiChanged) {
            integral_ = 0.0;
            iGain_ = 0.0;
        }
        if (kdChanged || predictChanged || wasPredictionEnabled != predictionVelocityEnabled()) {
            lastError_ = 0.0;
            lastRawError_ = 0.0;
            hasLastRawError_ = false;
            lastOutput_ = 0.0;
            errorObserver_.reset();
            resetPredictionTracking();
            predictionGain_ = 0.0;
            highSpeedTrust_ = 0.0;
            tuningStateRefreshPending_ = true;
        }
    }

    static double adjustGain(
        double gain,
        double absError,
        double threshold,
        double rate,
        double frameSeconds) {
        if (threshold <= kIntegralEpsilon) {
            return 1.0;
        }

        const double errorRatio = absError / (threshold + absError);
        const double targetGain = 1.0 - errorRatio;
        constexpr double kFarErrorGainRate = 0.1;
        double gainRate = rate + (kFarErrorGainRate - rate) * errorRatio;
        gainRate = ScaleRateForLowFps(gainRate, frameSeconds);
        return clamp01(gain + (targetGain - gain) * gainRate);
    }

    static double initialOutputGainForError(double absError) {
        const double blend = clamp01(
            (absError - kInitialOutputGainErrorStart) * kInvInitialOutputGainErrorRange
        );
        return kInitialOutputGainNear + (kInitialOutputGainFar - kInitialOutputGainNear) * blend;
    }

    static bool sameSign(double a, double b) {
        return (a * b) > 0.0;
    }

    double updateVelocityCoherenceGain(
        double velocity,
        double absError,
        double frameSeconds) {
        if (!hasLastCoherenceVelocity_) {
            lastCoherenceVelocity_ = velocity;
            hasLastCoherenceVelocity_ = true;
            velocityCoherence_ = 1.0;
            return 1.0;
        }

        const double absVelocity = std::fabs(velocity);
        const double absPreviousVelocity = std::fabs(lastCoherenceVelocity_);
        const double minVelocity = std::min(absVelocity, absPreviousVelocity);
        const double speedConfidence =
            1.0 - std::exp(-minVelocity / kVelocityCoherenceSpeedScale);
        const double normalizedCorrelation =
            (velocity * lastCoherenceVelocity_) /
            (absVelocity * absPreviousVelocity + kIntegralEpsilon);
        const double directionalCoherence = clamp01(
            kVelocityCoherenceNeutralFloor +
            (1.0 - kVelocityCoherenceNeutralFloor) *
            clamp01((normalizedCorrelation + 0.20) / 1.20));
        const double targetCoherence =
            (1.0 - speedConfidence) + speedConfidence * directionalCoherence;

        const double rate = targetCoherence > velocityCoherence_
            ? kVelocityCoherenceRiseRate
            : kVelocityCoherenceFallRate;
        const double scaledRate = ScaleRateForLowFps(rate, frameSeconds);
        velocityCoherence_ = clamp01(
            velocityCoherence_ + (targetCoherence - velocityCoherence_) * scaledRate);

        lastCoherenceVelocity_ = velocity;


        const double nearCenterBlend = std::exp(-absError / kVelocityCoherenceCenterScale);
        return clamp01(velocityCoherence_ * (1.0 - 0.35 * nearCenterBlend) +
            targetCoherence * 0.35 * nearCenterBlend);
    }

    static AdaptiveTrackingState computeAdaptiveTracking(
        double absError,
        double predictionVelocity,
        double previousPredictionVelocity,
        double previousHighSpeedTrust,
        double observerUncertaintyScale,
        double frameSeconds) {
        AdaptiveTrackingState state{};

        const double absVelocity = std::fabs(predictionVelocity);
        const double absPreviousVelocity = std::fabs(previousPredictionVelocity);
        const double absAccel = std::fabs(predictionVelocity - previousPredictionVelocity);

        const double velocityProduct = predictionVelocity * previousPredictionVelocity;
        const bool activeVelocityPair =
            absVelocity > kDirectionChangeVelocityThreshold &&
            absPreviousVelocity > kDirectionChangeVelocityThreshold;
        double reversal = 0.0;
        if (activeVelocityPair && velocityProduct < 0.0) {
            reversal = -velocityProduct / (absVelocity * absPreviousVelocity + kIntegralEpsilon);
        }

        double drop = 0.0;
        if (absPreviousVelocity > kDirectionChangeVelocityThreshold && absPreviousVelocity > absVelocity) {
            drop = (absPreviousVelocity - absVelocity) / (absPreviousVelocity + kIntegralEpsilon);
        }

        double targetTrust = 0.0;
        const double distanceTrust =
            kHighSpeedNearDistanceTrustFloor +
            (1.0 - kHighSpeedNearDistanceTrustFloor) *
            clamp01(absError * kInvHighSpeedDistanceTrustErrorScale);
        if (absVelocity > kIntegralEpsilon) {
            const double velocityScale =
                kHighSpeedVelocityScale * observerUncertaintyScale;
            const double accelerationScale =
                kHighSpeedAccelScale * observerUncertaintyScale;
            const double speedFactor =
                1.0 - std::exp(-absVelocity / velocityScale);
            const double stableVelocity =
                std::exp(-absAccel / accelerationScale);
            const double reversalGate = reversal > 0.0
                ? std::exp(-kHighSpeedReversalGateStrength * reversal)
                : 1.0;
            const double stopGate = drop > 0.0
                ? std::exp(-kHighSpeedStopGateStrength * drop)
                : 1.0;
            targetTrust = clamp01(
                speedFactor *
                stableVelocity *
                reversalGate *
                stopGate *
                distanceTrust);
        }
        double trustRate = targetTrust < previousHighSpeedTrust
            ? kHighSpeedTrustFallRate
            : kHighSpeedTrustRiseRate;
        trustRate = ScaleRateForLowFps(trustRate, frameSeconds);
        const double highSpeedTrust = clamp01(
            previousHighSpeedTrust + (targetTrust - previousHighSpeedTrust) * trustRate);
        state.smoothedTrust = highSpeedTrust;

        const double reversalDrop = reversal + drop;
        const double memoryBleed = clampRange(
            reversalDrop * kHighSpeedMemoryBleedStrength,
            0.0,
            kHighSpeedMaxMemoryBleed);
        state.velocityMemoryScale = 1.0 - memoryBleed;
        const double integralMemoryBleed = clampRange(
            reversalDrop * kHighSpeedIntegralMemoryBleedStrength,
            0.0,
            kHighSpeedMaxIntegralMemoryBleed);
        state.integralMemoryScale = 1.0 - integralMemoryBleed;
        return state;
    }

    double updateFrameTiming(Clock::time_point now) {
        if (!hasLastUpdateTime_) {
            lastUpdateTime_ = now;
            hasLastUpdateTime_ = true;
            smoothedFrameSeconds_ = kNominalFrameSeconds;
            return kNominalFrameSeconds;
        }

#if defined(PID_ENABLE_TEST_HOOKS)
        if (g_testFrameSeconds > 0.0) {
            const double elapsedSeconds = clampRange(
                g_testFrameSeconds,
                kMinFrameSeconds,
                kMaxFrameSeconds
            );
            lastUpdateTime_ = now;
            smoothedFrameSeconds_ +=
                (elapsedSeconds - smoothedFrameSeconds_) * kFrameTimeSmoothing;
            return elapsedSeconds;
        }
#endif

        const double elapsedSeconds = clampRange(
            std::chrono::duration<double>(now - lastUpdateTime_).count(),
            kMinFrameSeconds,
            kMaxFrameSeconds
        );
        lastUpdateTime_ = now;

        if (smoothedFrameSeconds_ <= 0.0 || smoothedFrameSeconds_ > kMaxFrameSeconds) {
            smoothedFrameSeconds_ = elapsedSeconds;
        } else {
            smoothedFrameSeconds_ += (elapsedSeconds - smoothedFrameSeconds_) * kFrameTimeSmoothing;
        }
        return elapsedSeconds;
    }

    static double frameScale(double numeratorSeconds, double denominatorSeconds) {
        if (numeratorSeconds <= 0.0 || denominatorSeconds <= 0.0) {
            return 1.0;
        }
        return clampRange(
            numeratorSeconds / denominatorSeconds,
            kMinFrameScale,
            kMaxFrameScale);
    }

    double predictionLookaheadFrames() const {
        const double latencyFrames = predictionLatencySeconds_ / kNominalFrameSeconds;
        const double effectivePredict = predict_ > kIntegralEpsilon ? predict_ : highSpeedPredictBoost_;
        return clampRange(effectivePredict * latencyFrames, 0.0, kMaxPredictionLookaheadFrames);
    }

    bool predictionVelocityEnabled() const {
        return predict_ > kIntegralEpsilon ||
            highSpeedPredictBoost_ > kIntegralEpsilon ||
            highSpeedFeedForwardGain_ > kIntegralEpsilon;
    }

    double integralStateLimit() const {
        const double configuredLimit = kiLimit_ > 0.0 ? kiLimit_ : baseLimit_;
        return clampRange(configuredLimit, std::max(1.0, kiDeadband_), baseLimit_);
    }

    void prepareIntegralForError(double currentError) {
        if (kiMode_ > 1 || std::fabs(ki_) <= kIntegralEpsilon || currentError == 0.0) {
            integral_ = 0.0;
            iGain_ = 0.0;
            return;
        }
        if (integral_ != 0.0 && !sameSign(currentError, integral_)) {
            integral_ = 0.0;
            iGain_ = 0.0;
        }
    }

    double updateIntegralTerm(
        double currentError,
        double integralFrameScale,
        double frameSeconds) {
        if (kiMode_ > 1 || std::fabs(ki_) <= kIntegralEpsilon) {
            integral_ = 0.0;
            return 0.0;
        }

        if (integral_ != 0.0) {
            const double normalizedFrameInterval = clampRange(
                frameSeconds / kNominalFrameSeconds,
                kMinFrameScale,
                kMaxFrameScale);
            integral_ *= std::exp(kIntegralLeakLog * normalizedFrameInterval);
        }
        integral_ += currentError * ki_ * iGain_ * integralFrameScale;
        const double stateLimit = integralStateLimit();
        integral_ = clampRange(integral_, -stateLimit, stateLimit);
        return integral_;
    }

    void resetPredictionHistory() {
        for (double &value : predictionHistory_) {
            value = 0.0;
        }
        predictionHead_ = 0;
        predictionMoveCount_ = 0;
        predictionHistoryConfidence_ = 0.0;
        lastPredictionVelocity_ = 0.0;
    }

    void resetPredictionTracking() {
        resetPredictionHistory();
        derivativeFilter_.reset();
        derivativeSmoothFilter_.reset();
        lastCoherenceVelocity_ = 0.0;
        hasLastCoherenceVelocity_ = false;
        velocityCoherence_ = 1.0;
    }

    void scalePredictionMemory(double scale) {
        scale = clampRange(scale, 0.0, 1.0);
        if (predictionMoveCount_ == 0 && lastPredictionVelocity_ == 0.0) {
            return;
        }
        for (double& value : predictionHistory_) {
            value *= scale;
        }
        lastPredictionVelocity_ *= scale;
        predictionHistoryConfidence_ *= scale;
    }

    double updatePredictionVelocity(double velocity, double frameSeconds) {
        predictionHistory_[predictionHead_] = velocity;
        predictionHead_ = (predictionHead_ + 1) & kPredictionHistoryMask;
        if (predictionMoveCount_ < kPredictionSamples) {
            ++predictionMoveCount_;
        }

        const std::size_t requiredSamples = static_cast<std::size_t>(clampRange(
            std::round(static_cast<double>(kPredictionSamples) /
                       std::max(
                           frameSeconds / kNominalFrameSeconds,
                           kIntegralEpsilon)),
            2.0,
            static_cast<double>(kPredictionSamples)));
        if (predictionMoveCount_ < requiredSamples) {
            return 0.0;
        }

        double weightedVelocity = 0.0;
        double weightedMagnitude = 0.0;
        double weightTotal = 0.0;
        std::size_t index = (predictionHead_ - requiredSamples) & kPredictionHistoryMask;
        const std::size_t weightBegin = kPredictionSamples - requiredSamples;
        for (std::size_t sample = 0; sample < requiredSamples; ++sample) {
            const double weight = kPredictionWeights[weightBegin + sample];
            weightedVelocity += predictionHistory_[index] * weight;
            weightedMagnitude += std::fabs(predictionHistory_[index]) * weight;
            weightTotal += weight;
            index = (index + 1) & kPredictionHistoryMask;
        }
        weightedVelocity /= weightTotal;
        weightedMagnitude /= weightTotal;

        const double directionalConfidence = clamp01(
            std::fabs(weightedVelocity) /
            std::max(weightedMagnitude, kIntegralEpsilon));
        const bool directionContinuous =
            sameSign(lastPredictionVelocity_, weightedVelocity) ||
            lastPredictionVelocity_ == 0.0 || weightedVelocity == 0.0;
        const double targetConfidence = directionContinuous
            ? directionalConfidence
            : 0.0;
        double confidenceRate = targetConfidence < predictionHistoryConfidence_
            ? kPredictionHistoryConfidenceFallRate
            : kPredictionHistoryConfidenceRiseRate;
        confidenceRate = ScaleRateForLowFps(confidenceRate, frameSeconds);
        predictionHistoryConfidence_ = clamp01(
            predictionHistoryConfidence_ +
            (targetConfidence - predictionHistoryConfidence_) * confidenceRate);

        if (!directionContinuous) {
            for (double& value : predictionHistory_) {
                value = velocity;
            }
            weightedVelocity = velocity;
        }

        lastPredictionVelocity_ =
            weightedVelocity * predictionHistoryConfidence_;
        return lastPredictionVelocity_;
    }

    double kp_;
    double ki_;
    double kd_;
    double baseLimit_;
    double kpLimit_;
    double kiLimit_;
    double kdLimit_;
    double outputLimit_;
    double predict_;
    double kiDeadband_;
    int kiMode_;

    double integral_;
    double lastError_;
    double lastRawError_;
    bool hasLastRawError_;
    double lastOutput_;
    Clock::time_point lastUpdateTime_;
    bool hasLastUpdateTime_;
    double smoothedFrameSeconds_;
    double observationTimeHintSeconds_;
    double lastObservationTimeSeconds_;
    bool hasObservationTimeHint_;
    bool hasLastObservationTime_;
    double predictionLatencySeconds_;
    bool hasLatencyHint_;

    double outputGain_;
    double outputGainRate_;
    double iGain_;
    double predictionGain_;
    double highSpeedPredictBoost_;
    double highSpeedFeedForwardGain_;
    double predictionLimit_;
    double feedForwardLimit_;
    bool auxLimitsExplicit_;
    double highSpeedTrust_;
    double predictionHistory_[kPredictionHistoryCapacity];
    std::size_t predictionHead_;
    unsigned int predictionMoveCount_;
    double predictionHistoryConfidence_;
    double lastPredictionVelocity_;
    double lastCoherenceVelocity_;
    bool hasLastCoherenceVelocity_;
    double velocityCoherence_;
    bool tuningStateRefreshPending_;
    PID_Diagnostics diagnostics_;
    ErrorObserver errorObserver_;
    KalmanFilter derivativeFilter_;
    KalmanFilter derivativeSmoothFilter_;
};

} 

extern "C" PID_Handle pid_create(void) {
    try {
        return new P_PID();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void pid_destroy(PID_Handle handle) {
    delete static_cast<P_PID *>(handle);
}

extern "C" int pid_init(PID_Handle handle, double kp, double ki, double kd, double predict, double rate) {
    if (!handle) {
        return 0;
    }
    return static_cast<P_PID *>(handle)->init(kp, ki, kd, predict, rate) ? 1 : 0;
}

extern "C" void pid_set_base(PID_Handle handle, double ki_mode, double kp_Limit, double ki_Limit, double kd_Limit, double Limit, double ki_rate, double ki_deadband) {
    if (handle) {
        const int kiMode = std::isfinite(ki_mode) ? static_cast<int>(clampRange(ki_mode, 0.0, 2.0)) : 0;
        static_cast<P_PID *>(handle)->setBase(kiMode, kp_Limit, ki_Limit, kd_Limit, Limit, ki_rate, ki_deadband);
    }
}

extern "C" void pid_set_latency(PID_Handle handle, double latency_seconds) {
    if (handle) {
        static_cast<P_PID *>(handle)->setLatencySeconds(latency_seconds);
    }
}

extern "C" void pid_set_observation_time_seconds(PID_Handle handle, double observation_time_seconds) {
    if (handle) {
        static_cast<P_PID *>(handle)->setObservationTimeSeconds(observation_time_seconds);
    }
}

extern "C" void pid_set_applied_move(PID_Handle handle, double move) {
    if (handle) {
        static_cast<P_PID *>(handle)->setAppliedMove(move);
    }
}

extern "C" double pid_update(PID_Handle handle, double error) {
    if (!handle) {
        return 0.0;
    }
    return static_cast<P_PID *>(handle)->update(error);
}

extern "C" double pid_update_with_prediction_state(PID_Handle handle, double error, int prediction_allowed) {
    if (!handle) {
        return 0.0;
    }
    return static_cast<P_PID *>(handle)->update(error, prediction_allowed != 0);
}

extern "C" void pid_reset(PID_Handle handle) {
    if (handle) {
        static_cast<P_PID *>(handle)->reset();
    }
}

extern "C" void pid_update_params(PID_Handle handle, double kp, double ki, double kd, double predict, double rate) {
    if (handle) {
        static_cast<P_PID *>(handle)->updateParams(kp, ki, kd, predict, rate);
    }
}

extern "C" void pid_set_high_speed_tuning(PID_Handle handle, double predict_boost, double feed_forward_gain) {
    if (handle) {
        static_cast<P_PID *>(handle)->setHighSpeedTuning(predict_boost, feed_forward_gain);
    }
}

extern "C" void pid_set_aux_limits(PID_Handle handle, double prediction_limit, double feed_forward_limit) {
    if (handle) {
        static_cast<P_PID *>(handle)->setAuxLimits(prediction_limit, feed_forward_limit);
    }
}

extern "C" void pid_get_diagnostics(PID_Handle handle, PID_Diagnostics *diagnostics) {
    if (!diagnostics) {
        return;
    }
    *diagnostics = handle
        ? static_cast<P_PID *>(handle)->diagnostics()
        : PID_Diagnostics{};
}

#if defined(PID_ENABLE_TEST_HOOKS)
extern "C" void pid_test_set_frame_seconds(double frame_seconds) {
    g_testFrameSeconds = std::isfinite(frame_seconds)
        ? clampRange(frame_seconds, 0.0, kMaxFrameSeconds)
        : 0.0;
}
#endif
