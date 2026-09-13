// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — runtime and accelerator identity
//
//  The engine space here is two-dimensional, and flattening it into one enum
//  was an early mistake worth naming: "NNAPI" is not a peer of "ONNX Runtime",
//  it is something ONNX Runtime can be told to use. So the pair (Runtime, Ep)
//  is the real identity, and this header is the only place the matrix lives.
//
//                     CPU  XNNPACK  NNAPI   GPU  Vulkan   HTP
//    ONNX Runtime      -     -       -      .      .      -     QNN EP needs a
//                                                               custom ORT build
//    LiteRT            -     -       -      -      .      -     XNNPACK *is* its
//                                                               CPU path
//    NCNN              -     .       .      .      -      .     Vulkan needs a
//                                                               rebuild (see note)
//    QNN (direct)      -     .       .      -      .      -     runs a
//                                                               pre-compiled .bin
//
//  '-' implemented, '.' not. validPair() is the authority; the table is a
//  summary and the code is the truth.
//
//  Note on the prebuilt ncnn: the arm64-v8a package carried over from the old
//  project was configured with NCNN_VULKAN 0 (see include/ncnn/platform.h), so
//  the Vulkan cell above is aspirational until ncnn is rebuilt with it on. The
//  header is generated at ncnn build time, so that flag is the ground truth and
//  not a guess.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>
#include <cstdint>

namespace aimbotng {
namespace infer {

/// Which runtime executes a model. For the file formats that only one runtime
/// can open, this is decided by the extension rather than chosen.
enum class Runtime : uint8_t {
    Unknown = 0,
    OnnxRuntime,   // .onnx         — one library, many execution providers
    LiteRT,        // .tflite       — delegates rather than execution providers
    Ncnn,          // .param + .bin — CPU only unless ncnn is rebuilt with Vulkan
    Qnn,           // .bin          — a pre-compiled QNN context, run directly
    Count
};

/// Which accelerator, *within* a runtime.
///
/// One enum serves every runtime on purpose, even though the names collide
/// across them: `Xnnpack` is an ORT execution provider and the LiteRT CPU
/// delegate, and both are literally the XNNPACK kernel library. `Gpu` is
/// LiteRT's GPU delegate; NCNN spells the same idea `Vulkan` because that is
/// the API it goes through. Naming them the same thing they are is better than
/// inventing per-runtime vocabulary the user then has to learn twice.
enum class Ep : uint8_t {
    Default = 0,   // whatever the runtime prefers on its own
    Cpu,           // explicitly the plain CPU kernels
    Xnnpack,       // CPU, XNNPACK kernels
    Nnapi,         // Android's own delegate; opaque about what it actually picks
    Gpu,           // LiteRT's GPU delegate
    Vulkan,        // NCNN's Vulkan backend
    Htp,           // Hexagon Tensor Processor — the Snapdragon NPU
    Count
};

/// A (runtime, accelerator) combination. The unit the menu offers and the
/// factory accepts; there is no way to ask for an Ep without a Runtime, which
/// is the point.
struct Pair {
    Runtime runtime = Runtime::Unknown;
    Ep      ep      = Ep::Default;
};

/// "ONNX Runtime", "LiteRT", "NCNN", "QNN". For the menu and the log.
const char* runtimeLabel(Runtime);

/// The file extension this runtime opens, without the dot.
const char* runtimeExtension(Runtime);

/// "Default", "XNNPACK", "QNN HTP", ... Note that the label of an Ep read on
/// its own is ambiguous by design; use pairLabel() when both are known.
const char* epLabel(Ep);

/// "ONNX Runtime / XNNPACK". The name a list row and a log line should use.
const char* pairLabel(Runtime, Ep);
/// Same, into a caller's buffer — pairLabel()'s result is a static, and a
/// runtime that fell back internally needs to name something not in the table.
void pairLabelTo(char* out, size_t outSize, Runtime, Ep);

// ── The matrix ──────────────────────────────────────────────────────────────

/// Every pair the code implements, whether or not it can run here. Terminated
/// by a Runtime::Unknown entry, so the array is also safe to walk without the
/// count. The menu iterates this; nothing hard-codes an engine list.
const Pair* allPairs(int& count);

/// Whether the pair exists in the code at all. Cheap — no probing, no dlopen.
/// Use this to reject nonsense before doing real work.
bool validPair(Runtime, Ep);

/// The pairs that make sense for a file format, in menu order. `count` is set.
/// Calling it with Runtime::Unknown yields the empty list.
const Pair* pairsForRuntime(Runtime, int& count);

/// A backend with no implementation yet still has a name, so the menu can grey
/// it out honestly instead of hiding it and looking like it forgot.
const char* epImplementationState(Runtime, Ep);

// ── Probing ─────────────────────────────────────────────────────────────────

/// Whether this pair will actually run on the device in front of us.
///
/// The implementation *asks the runtime* — dlopens the library, creates the
/// delegate, and throws it away — rather than guessing from the SoC string. The
/// old project's `isQualcommSnapdragon()` check is exactly the kind of guess
/// that is wrong on a device nobody tested, and it is wrong in the expensive
/// direction: it claims support the runtime then refuses to provide.
///
/// Cached after the first call per pair; probing QNN costs a dlopen.
bool epAvailable(Runtime, Ep);

/// A short human sentence for why epAvailable() said no, suitable for a tooltip
/// or a log line. Empty when the pair is available.
const char* epUnavailableReason(Runtime, Ep);

/// Drops every cached probe result. For a Settings "re-detect" action, and for
/// tests that need to watch the probe happen.
void resetProbes();

}  // namespace infer
}  // namespace aimbotng
