// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — the ONNX Runtime engine
//
//  Runs .onnx models through ONNX Runtime. XNNPACK and plain CPU are two
//  settings of the same object rather than two engines, because they are two
//  settings of the same library: the Android package ships the XNNPACK
//  execution provider inside libonnxruntime.so, and the only difference is
//  whether we register it before creating the session.
//
//  On the difference between the two, since it is not the one people expect:
//  XNNPACK does not replace the CPU provider, it takes the nodes it can
//  implement and leaves the rest to fall back to the default provider. So
//  "XNNPACK" never means "everything ran on XNNPACK" — it means the conv-heavy
//  part did. That is also why the CPU option exists at all: for a model whose
//  nodes XNNPACK mostly cannot take, registering it only adds a partition
//  boundary, and the plain path is faster.
//
//  The library is loaded at runtime with dlopen() rather than linked. See
//  libpath.h for why, and ort_engine.cpp for the mechanism.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/engine.h"
#include "inference/postprocess.h"
#include "inference/preprocess.h"

#include <memory>
#include <string>
#include <vector>

namespace aimbotng {
namespace infer {

class OrtEngine final : public Engine {
public:
    explicit OrtEngine(Ep ep);
    ~OrtEngine() override;

    OrtEngine(const OrtEngine&) = delete;
    OrtEngine& operator=(const OrtEngine&) = delete;

    Status load(const ModelSpec&) override;
    void   unload() override;
    bool   ready() const override;

    std::vector<Box> run(const FrameView&, const Roi&) override;

    Runtime runtime() const override { return Runtime::OnnxRuntime; }
    Ep      ep() const override { return ep_; }
    void    setConfidence(float) override;
    const char* activeName() const override;
    Timing      lastTiming() const override { return timing_; }
    const Status& lastStatus() const override { return status_; }
    std::string describe() const override;

    /// The element type the loaded model's input actually declares (fp16 / fp32 /
    /// int8 / …), as a short label, for the menu's model list.
    std::string inputDtypeName() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    Ep          ep_;
    Timing      timing_;
    Status      status_;
    std::string activeName_ = "ONNX Runtime";
};

}  // namespace infer
}  // namespace aimbotng
