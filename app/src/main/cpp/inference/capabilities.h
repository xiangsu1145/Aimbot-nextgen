// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — what this device can do
//
//  The menu needs to grey out impossible choices *before* the user picks one and
//  waits. Doing that from a SoC string is the easy way and the wrong one: it
//  says "Snapdragon, so HTP works" on a device whose HTP architecture the
//  shipped skel library does not cover, and the failure surfaces two seconds
//  later as a compile error inside a vendor library.
//
//  So this header separates the two questions that are usually conflated:
//
//    * capabilities() — what the hardware *is*. Cheap facts, read once, shown
//      in the UI as context ("SM8650, HTP v75, 8 cores").
//    * epAvailable()  — what will actually run. Owned by backend.h, answered by
//      asking the runtime.
//
//  Neither is allowed to answer the other's question.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

namespace aimbotng {
namespace infer {

/// The device, as read from system properties and /proc. Descriptive only —
/// nothing in the code makes a *decision* from these.
struct Capabilities {
    std::string socModel;    // ro.soc.model, e.g. "SM8650"
    std::string board;       // ro.board.platform, e.g. "pineapple"
    int  apiLevel = 0;       // ro.build.version.sdk
    int  cpuCores = 0;       // hardware threads actually online

    bool isQualcomm = false;
    bool isMediaTek = false;

    /// Hexagon generation as an integer — 68, 69, 73, 75, 79, 81 — or 0 when
    /// the device is not a Snapdragon. Read from the SoC model, so it is only
    /// as right as that property is; it exists to pick which `libQnnHtpV*Skel`
    /// to name in a log line, and to tell the user why their .bin was rejected.
    /// It is deliberately NOT what gates a backend — epAvailable() is.
    int htpArch = 0;

    /// The Vulkan/GL renderer string, when it could be read. Which Adreno is
    /// not something to branch on, but it is the first thing anyone asks when
    /// a GPU backend misbehaves.
    std::string gpuRenderer;

    /// One line per (runtime, ep) pair in allPairs() order: the verdict and,
    /// when negative, the reason. Pre-formatted because both the Settings page
    /// and the log want exactly this and neither should be assembling it.
    std::vector<std::string> backendNotes;
};

/// Probed on first call and cached. Probing dlopens vendor libraries, so it is
/// not free and not something to do per frame.
const Capabilities& capabilities();

/// Drops the cache. Pairs with resetProbes() for a "re-detect" action.
void resetCapabilities();

/// Multi-line dump: identity, then every backend with its verdict. This is the
/// block to paste into a bug report, so it is complete rather than tidy.
std::string capabilitiesReport();

/// True when the device has the vendor stack this project was built and tested
/// against — i.e. Snapdragon with an HTP generation we ship a skel for. False
/// does not mean unusable: ONNX Runtime and NCNN run anywhere arm64 does.
bool isSupportedDevice();

}  // namespace infer
}  // namespace aimbotng
