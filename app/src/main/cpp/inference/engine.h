// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — what a loaded model looks like from the outside
//
//  One interface, implemented once per runtime. Everything runtime-specific —
//  graph compilation, tensor binding, delegation, caching — is behind these
//  seven methods. Everything shared — cropping, letterboxing, decoding,
//  suppression — sits in preprocess.h and postprocess.h and is written once.
//
//  That split is the whole design. The old project put a full copy of the
//  pre/post chain inside each engine (`litert_engine.cpp` 487 lines,
//  `ncnn_engine.cpp` 379 lines, largely the same code), so a fix to the YOLO
//  decoder had to be found and applied four times. An engine here is only
//  responsible for the middle: given a tensor, produce a tensor.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/backend.h"
#include "inference/inference_types.h"

#include <memory>
#include <string>
#include <vector>

namespace aimbotng {
namespace infer {

/// A model that has been loaded and is ready to run.
///
/// Instances are never constructed directly; create() is the only door, which
/// is what lets a backend fail with a message instead of an exception, and lets
/// the factory hand back a stub for a backend that has no implementation yet.
class Engine {
public:
    virtual ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Loads the model. Repeatable: calling it again unloads first, so a
    /// Settings change is one call rather than a destroy/recreate dance the
    /// caller has to get the order right on.
    virtual Status load(const ModelSpec&) = 0;

    /// Frees everything the runtime holds. Safe to call when not loaded, and
    /// safe to call twice — the destructor does it too.
    virtual void unload() = 0;

    /// Whether run() would do anything. Distinct from "load() returned ok": a
    /// runtime that asynchronously gives up after a successful init should be
    /// able to say so here rather than failing every subsequent frame.
    virtual bool ready() const = 0;

    /// Runs one frame.
    ///
    /// An empty result means "nothing found" and also means "this frame
    /// failed"; lastStatus() tells them apart. Callers that only care about
    /// boxes can ignore it, but a UI that wants to show an error must not have
    /// to guess from an empty vector.
    ///
    /// Not reentrant. The Session owns one worker thread precisely so that no
    /// caller has to think about this.
    virtual std::vector<Box> run(const FrameView&, const Roi&) = 0;

    virtual Runtime runtime() const = 0;
    virtual Ep      ep() const = 0;

    /// What actually ran, as a string — "ONNX Runtime / XNNPACK", or whatever
    /// the runtime fell back to. Not always what was asked for: ONNX Runtime
    /// will silently run unsupported nodes on the CPU, and QNN will refuse a
    /// graph and leave the delegate covering nothing. A backend that falls back
    /// must say so here, because a silent fallback is indistinguishable from a
    /// slow model, and those have opposite fixes.
    virtual const char* activeName() const = 0;

    /// Cost of the most recent run(). Zeroed before the first one.
    virtual Timing lastTiming() const = 0;

    /// Changes the detection threshold.
    ///
    /// Cheap by contract — it is a live control in the menu, so it is called
    /// while a slider is being dragged. An engine that cannot honour the change
    /// without rebuilding its graph must still accept it and return the reload
    /// it performs through the normal path, not make the caller guess whether a
    /// reload is needed. Engines whose decode reads the value per frame (which
    /// is all of them here) just store it.
    virtual void setConfidence(float) = 0;

    /// Result of the most recent load() or run().
    virtual const Status& lastStatus() const = 0;

    /// Free-form facts worth logging once after a load: which delegate
    /// actually claimed nodes, how long the graph took to compile, the tensor
    /// shapes that were bound. Empty when there is nothing to say. This is the
    /// string a bug report needs and the reason `activeName()` is one line.
    virtual std::string describe() const { return {}; }

    /// Detected input element type as a short label ("fp32" / "fp16" / …), or "".
    virtual std::string inputDtypeName() const { return {}; }

protected:
    Engine() = default;
};

/// Creates the engine for a pair.
///
/// Returns null and fills `why` when the pair does not exist, has no
/// implementation, or cannot run here. Never throws — see Status for why.
///
/// This does not load a model; call load() on the result. The two are separate
/// because the menu needs to build an engine to *ask it questions* (what will
/// you fall back to? how long do you take to compile?) before the user has
/// committed to running anything.
std::unique_ptr<Engine> create(Runtime, Ep, std::string* why = nullptr);

/// A loaded engine plus the model it holds, for callers that want them
/// together. `engine` is non-null whenever `status.ok`.
struct LoadedEngine {
    std::unique_ptr<Engine> engine;
    ModelSpec               spec;
    Status                  status;

    bool ok() const { return status.ok && engine != nullptr; }
};

/// create() then load(), which is what almost every real caller wants. Kept
/// next to the two-line version so the pair cannot drift.
LoadedEngine createAndLoad(const ModelSpec&, Runtime, Ep);

}  // namespace infer
}  // namespace aimbotng
