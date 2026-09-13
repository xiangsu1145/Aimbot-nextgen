// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — the LiteRT (TensorFlow Lite) engine
//
//  Runs .tflite models through the LiteRT C API. Four execution modes:
//
//  * Ep::Cpu      — no delegate; whatever the runtime picked by default.
//  * Ep::Xnnpack  — TfLiteXNNPackDelegateCreate, applied on top of the default.
//  * Ep::Nnapi    — TfLiteNnapiDelegateCreate, with a per-model token so the
//                   platform delegate can cache compiled graphs across runs.
//  * Ep::Gpu      — TfLiteGpuDelegateV2Create (OpenCL).
//  * Ep::Htp      — QNN HTP via TfLiteQnnDelegateCreate.
//
//  Output heads: single concatenated [4+nc, N] AND split boxes+scores
//  ([4, N] + [nc, N], either order) are decoded; the split pair is merged
//  into one buffer so postprocess::decode is shared.
//
//  No auto-fallback. The user picks one EP and that is what runs — a different
//  EP is a different choice with different costs, not a silent better one.
//  The choice a delegate falls back to internally (NNAPI rejecting an op and
//  dropping to the default CPU kernel, for example) is reported through
//  activeName() so a slow detector can still be told apart from a wrong one.
//
//  preprocess() and decode() come from preprocess.h / postprocess.h, the
//  same ones OrtEngine uses. The model_type.cpp probe runs TFLite too, so
//  the menu's fp32 / fp16 / int8 label works for .tflite entries without the
//  UI knowing which engine produced the answer.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/engine.h"
#include "inference/postprocess.h"
#include "inference/preprocess.h"

#include <memory>
#include <string>

namespace aimbotng {
namespace infer {

class LiteRtEngine final : public Engine {
public:
    explicit LiteRtEngine(Ep ep);
    ~LiteRtEngine() override;

    LiteRtEngine(const LiteRtEngine&) = delete;
    LiteRtEngine& operator=(const LiteRtEngine&) = delete;

    Status load(const ModelSpec&) override;
    void   unload() override;
    bool   ready() const override;

    std::vector<Box> run(const FrameView&, const Roi&) override;

    Runtime runtime() const override { return Runtime::LiteRT; }
    Ep      ep() const override { return ep_; }
    void    setConfidence(float) override;
    const char* activeName() const override;
    Timing      lastTiming() const override { return timing_; }
    const Status& lastStatus() const override { return status_; }
    std::string describe() const override;
    std::string inputDtypeName() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// Copies the interpreter's output tensor into `dst`, dequantising when
    /// the graph declares int8/uint8 and decoding the IEEE binary16 bit
    /// pattern when it declares float16. Lives on the class rather than
    /// in an anonymous namespace because `Impl` is private and the helper
    /// touches its fields.
    static bool readOutputInto(Impl* m, std::vector<float>& dst);

    Ep          ep_;
    Timing      timing_;
    Status      status_;
    std::string activeName_ = "LiteRT";
};

}  // namespace infer
}  // namespace aimbotng
