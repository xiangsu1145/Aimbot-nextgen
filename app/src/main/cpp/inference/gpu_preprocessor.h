#pragma once

#include "inference/inference_types.h"
#include "inference/preprocess.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace aimbotng {
namespace infer {

/**
 * GPU / OpenGL ES 3.1 Compute Shader hardware preprocessor.
 *
 * Offloads bilinear letterbox resampling, aspect ratio padding, channel
 * reordering (RGB <-> BGR), NCHW/NHWC layout transposition, and floating-point
 * normalisation completely from the CPU onto the Adreno/Mali GPU hardware ALUs.
 */
class GpuPreprocessor {
public:
    static GpuPreprocessor& instance();

    /// Initialises offscreen EGL context and compiles the Compute Shader.
    /// Thread-safe, idempotent, lazily called on first use.
    bool init();

    /// Releases EGL and GL resources.
    void release();

    /// Whether the GPU Compute pipeline is ready to run.
    bool available() const { return m_ready; }

    /// Executes Letterbox crop, resize, and normalisation on the GPU via Compute Shader,
    /// directly writing into `out` float tensor buffer.
    /// Returns true if GPU computation succeeded; false on failure.
    bool run(const FrameView& frame, const Roi& roi,
             const PreprocessConfig& cfg, float* out, LetterboxMap& outMap);

    /// Executes Letterbox crop and resize on the GPU directly writing into `out` uint8 tensor buffer.
    bool runU8(const FrameView& frame, const Roi& roi,
               const PreprocessConfig& cfg, uint8_t* out, LetterboxMap& outMap);

private:
    GpuPreprocessor() = default;
    ~GpuPreprocessor() { release(); }

    bool initGlLocked();
    void cleanupGlLocked();

    std::mutex m_mutex;
    bool m_ready = false;
    bool m_initAttempted = false;

    void* m_display = nullptr;   // EGLDisplay
    void* m_context = nullptr;   // EGLContext
    void* m_surface = nullptr;   // EGLSurface

    unsigned int m_programFloat = 0;
    unsigned int m_programU8 = 0;
    unsigned int m_inputTexture = 0;
    unsigned int m_ssbo = 0;
    size_t       m_ssboCapacity = 0;
    int          m_texAllocW = 0;
    int          m_texAllocH = 0;
};

}  // namespace infer
}  // namespace aimbotng
