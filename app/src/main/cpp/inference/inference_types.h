// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — the value types every other inference header shares
//
//  Nothing here knows about a runtime, a file format, or the model list. These
//  are the nouns: a frame, a region of interest, a detection, a model
//  description, a cost, and a failure. Keeping them in their own header is what
//  lets the engine layer and the menu layer both include them without one
//  dragging in the other.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aimbotng {
namespace infer {

// ── Geometry ────────────────────────────────────────────────────────────────

/// A detection, in *screen* pixels. The engine is the one that undoes its own
/// letterbox, so by the time a caller sees a Box the coordinates are already
/// usable for touch injection and drawing.
struct Box {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float score = 0.0f;
    int   cls = 0;

    float w() const { return x2 - x1; }
    float h() const { return y2 - y1; }
    float cx() const { return (x1 + x2) * 0.5f; }
    float cy() const { return (y1 + y2) * 0.5f; }
    float area() const { return w() * h(); }
};

/// One frame handed to an engine.
///
/// This is deliberately a *view*: the capture path already owns the pixels and
/// nothing in the inference layer copies them. The lifetime rule is the same
/// one `capture::copyLatestInto` already uses — a view is valid only inside the
/// call it was passed to.
struct FrameView {
    const uint8_t* pixels = nullptr;
    int width  = 0;   // full frame, in pixels
    int height = 0;

    /// Bytes between the starts of two rows. **Not** `width * 4`.
    ///
    /// This project has already been bitten once by a GraphicBuffer whose
    /// stride exceeded its visible width; the capture side documented it, and
    /// every consumer that assumes tight packing is a latent horizontal smear.
    int rowStrideBytes = 0;

    bool valid() const {
        return pixels != nullptr && width > 0 && height > 0 &&
               rowStrideBytes >= width * 4;
    }
};

/// The sub-rectangle of a frame an engine should look at, in full-frame pixels.
///
/// A default-constructed (zero-sized) Roi means "the whole frame", which is the
/// common case: the crop exists so a caller can aim at a band of the screen
/// without the engine knowing that band is special.
struct Roi {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool empty() const { return w <= 0 || h <= 0; }

    /// The rectangle that is actually valid for `frame`: the caller's crop,
    /// clipped to the frame, or the whole frame when the crop is empty.
    static Roi resolve(const Roi& wanted, const FrameView& frame);

    static Roi whole() { return Roi{}; }
};

// ── What to run ─────────────────────────────────────────────────────────────

/// Everything an engine needs to know about a model, and nothing about where
/// the model list is stored.
///
/// This is a boundary, not a convenience: the engine layer must not include
/// `model_store.h`, or a UI refactor turns into a rebuild of every backend.
/// model_store owns the user's list; the Session converts one entry into one
/// ModelSpec at load time and never looks back.
struct ModelSpec {
    /// Absolute path. `.onnx`, `.tflite`, or — for NCNN — the `.param`, whose
    /// sibling `.bin` is found by replacing the extension.
    std::string path;

    /// Class names by index; index 0 is class 0. Empty means the model's
    /// output is used verbatim with numeric class ids.
    std::vector<std::string> classes;

    int   inputSize  = 640;    // square edge
    float confidence = 0.5f;   // detections below this are discarded
    float iou        = 0.45f;  // NMS overlap threshold
    int   cpuThreads = 0;      // 0 = derive from the core count
    /// HTP (QNN) performance-mode vote: one of
    /// TfLiteQnnDelegateHtpPerformanceMode. 0 (kHtpDefault) means "let the
    /// system decide"; 1 (kHtpSustainedHighPerformance) is the UI default.
    /// Ignored by non-HTP engines. Stored as the raw enum int so the engine
    /// layer and the menu layer never share the QNN header.
    int   htpPerfMode = 1;     // default: Sustained High Performance

    /// Per-model scratch key. Engines that compile a graph on the device —
    /// QNN HTP is the one that matters — cache the compiled artifact under
    /// this name, so a warm launch skips a compile measured in seconds.
    /// Defaults to the file's basename when left empty.
    std::string cacheToken;
};

// ── What it cost ────────────────────────────────────────────────────────────

/// Cost of the most recent run(), in milliseconds.
///
/// Engines fill in what they can and leave the rest at zero: a runtime that has
/// no separate post-process step has a genuine zero there, which is different
/// from a runtime we have not instrumented. The UI shows the three separately
/// because "which part is slow" is the only question worth asking of them.
struct Timing {
    float preMs   = 0.0f;
    float inferMs = 0.0f;
    float postMs  = 0.0f;

    float totalMs() const { return preMs + inferMs + postMs; }
};

// ── How it went ─────────────────────────────────────────────────────────────

/// Why an operation failed, in enough detail to put in front of a user.
///
/// The platform does not use exceptions. The `adb` target is built with
/// `-fno-exceptions`, the daemon is a single process shared with the renderer,
/// and a throwing vendor library would take the whole menu down with it.
/// Returning a Status costs one string on the failure path and nothing on the
/// success path.
struct Status {
    bool        ok = true;
    std::string message;

    static Status good() { return Status{}; }
    static Status bad(std::string why) { return Status{false, std::move(why)}; }

    /// "ok" or the message, for logs.
    const char* cstr() const { return ok ? "ok" : message.c_str(); }
};

}  // namespace infer
}  // namespace aimbotng
