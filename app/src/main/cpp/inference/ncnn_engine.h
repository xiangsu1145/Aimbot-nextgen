// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — the NCNN engine (CPU & Vulkan GPU)
//
//  Runs .param + .bin models through Tencent NCNN.
//  Supports both CPU (multi-threaded via OpenMP) and GPU (Vulkan compute).
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

class NcnnEngine final : public Engine {
public:
    explicit NcnnEngine(Ep ep);
    ~NcnnEngine() override;

    NcnnEngine(const NcnnEngine&) = delete;
    NcnnEngine& operator=(const NcnnEngine&) = delete;

    Status load(const ModelSpec&) override;
    void   unload() override;
    bool   ready() const override;

    std::vector<Box> run(const FrameView&, const Roi&) override;

    Runtime runtime() const override { return Runtime::Ncnn; }
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

    Ep          ep_;
    Timing      timing_;
    Status      status_;
    std::string activeName_ = "NCNN";
};

/// Validates whether the given files form a valid NCNN model (.param and .bin).
/// Returns true on success; fills outError on failure.
bool validateNcnnModel(const std::string& paramPath,
                       const std::string& binPath,
                       std::string& outError);

}  // namespace infer
}  // namespace aimbotng
