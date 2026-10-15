#include "inference/gpu_preprocessor.h"

#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#define TAG "AimbotGpuPre"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace {

using Clock = std::chrono::steady_clock;

const char* kComputeShaderFloat = R"glsl(#version 310 es
precision highp float;

layout (local_size_x = 16, local_size_y = 16) in;

layout (binding = 0) uniform sampler2D u_image;

layout (std430, binding = 1) buffer OutputTensor {
    float out_data[];
};

uniform int   u_targetW;
uniform int   u_targetH;
uniform vec4  u_cropRect;   // srcX, srcY, srcW, srcH in pixels
uniform vec2  u_srcSize;    // frame.width, frame.height
uniform vec2  u_pad;        // padX, padY
uniform vec2  u_scaledSize; // newW, newH
uniform float u_scale;
uniform vec3  u_mean;
uniform vec3  u_stdv;
uniform int   u_bgr;        // 1 if BGR
uniform int   u_nchw;       // 1 if NCHW
uniform float u_padValue;
uniform int   u_normalize;

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    if (gid.x >= u_targetW || gid.y >= u_targetH) return;

    int pixelIdx = gid.y * u_targetW + gid.x;
    int planeSize = u_targetW * u_targetH;

    // Check letterbox border padding
    if (float(gid.x) < u_pad.x || float(gid.x) >= (u_pad.x + u_scaledSize.x) ||
        float(gid.y) < u_pad.y || float(gid.y) >= (u_pad.y + u_scaledSize.y)) {
        
        vec3 bar = vec3(u_padValue);
        if (u_nchw == 1) {
            out_data[pixelIdx] = bar.r;
            out_data[planeSize + pixelIdx] = bar.g;
            out_data[2 * planeSize + pixelIdx] = bar.b;
        } else {
            out_data[pixelIdx * 3 + 0] = bar.r;
            out_data[pixelIdx * 3 + 1] = bar.g;
            out_data[pixelIdx * 3 + 2] = bar.b;
        }
        return;
    }

    vec2 rel = (vec2(gid) - u_pad + 0.5) / u_scaledSize;
    vec2 srcPx = u_cropRect.xy + rel * u_cropRect.zw;
    vec2 uv = srcPx / u_srcSize;

    vec4 px = texture(u_image, uv);
    vec3 c = (u_bgr == 1) ? px.bgr : px.rgb;

    if (u_normalize == 1) {
        c = (c * 255.0 - u_mean) / u_stdv;
    } else {
        c = c * 255.0 * u_scale;
    }

    if (u_nchw == 1) {
        out_data[pixelIdx] = c.r;
        out_data[planeSize + pixelIdx] = c.g;
        out_data[2 * planeSize + pixelIdx] = c.b;
    } else {
        out_data[pixelIdx * 3 + 0] = c.r;
        out_data[pixelIdx * 3 + 1] = c.g;
        out_data[pixelIdx * 3 + 2] = c.b;
    }
}
)glsl";

const char* kComputeShaderU8 = R"glsl(#version 310 es
precision highp float;

layout (local_size_x = 16, local_size_y = 16) in;

layout (binding = 0) uniform sampler2D u_image;

layout (std430, binding = 1) buffer OutputTensorU8 {
    uint out_words[];
};

uniform int   u_targetW;
uniform int   u_targetH;
uniform vec4  u_cropRect;
uniform vec2  u_srcSize;
uniform vec2  u_pad;
uniform vec2  u_scaledSize;
uniform int   u_bgr;
uniform float u_padValue; // e.g. 114.0

uvec3 samplePixel(int x, int y) {
    if (float(x) < u_pad.x || float(x) >= (u_pad.x + u_scaledSize.x) ||
        float(y) < u_pad.y || float(y) >= (u_pad.y + u_scaledSize.y)) {
        uint pv = uint(clamp(u_padValue, 0.0, 255.0));
        return uvec3(pv, pv, pv);
    }
    vec2 rel = (vec2(float(x), float(y)) - u_pad + 0.5) / u_scaledSize;
    vec2 srcPx = u_cropRect.xy + rel * u_cropRect.zw;
    vec2 uv = srcPx / u_srcSize;
    vec4 px = texture(u_image, uv);
    vec3 c = ((u_bgr == 1) ? px.bgr : px.rgb) * 255.0;
    return uvec3(clamp(c + 0.5, 0.0, 255.0));
}

void main() {
    int chunkX = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    int startX = chunkX * 4;

    if (startX >= u_targetW || y >= u_targetH) return;

    uvec3 p0 = samplePixel(startX + 0, y);
    uvec3 p1 = (startX + 1 < u_targetW) ? samplePixel(startX + 1, y) : uvec3(0u);
    uvec3 p2 = (startX + 2 < u_targetW) ? samplePixel(startX + 2, y) : uvec3(0u);
    uvec3 p3 = (startX + 3 < u_targetW) ? samplePixel(startX + 3, y) : uvec3(0u);

    uint w0 = (p0.r & 0xFFu) | ((p0.g & 0xFFu) << 8) | ((p0.b & 0xFFu) << 16) | ((p1.r & 0xFFu) << 24);
    uint w1 = (p1.g & 0xFFu) | ((p1.b & 0xFFu) << 8) | ((p2.r & 0xFFu) << 16) | ((p2.g & 0xFFu) << 24);
    uint w2 = (p2.b & 0xFFu) | ((p3.r & 0xFFu) << 8) | ((p3.g & 0xFFu) << 16) | ((p3.b & 0xFFu) << 24);

    int pixelBase = y * u_targetW + startX;
    int wordBase = (pixelBase * 3) / 4;

    out_words[wordBase + 0] = w0;
    if (startX + 2 < u_targetW) out_words[wordBase + 1] = w1;
    if (startX + 3 < u_targetW) out_words[wordBase + 2] = w2;
}
)glsl";

GLuint compileComputeProgram(const char* src) {
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        LOGE("Compute shader compilation failed: %s", infoLog);
        glDeleteShader(shader);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        LOGE("Compute program linking failed: %s", infoLog);
        glDeleteProgram(program);
        glDeleteShader(shader);
        return 0;
    }

    glDeleteShader(shader);
    return program;
}

}  // namespace

GpuPreprocessor& GpuPreprocessor::instance() {
    static GpuPreprocessor s_instance;
    return s_instance;
}

bool GpuPreprocessor::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ready) return true;
    if (m_initAttempted) return m_ready;
    m_initAttempted = true;
    return initGlLocked();
}

bool GpuPreprocessor::initGlLocked() {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_NONE
    };

    EGLConfig cfg;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(dpy, configAttribs, &cfg, 1, &numConfigs) || numConfigs <= 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, contextAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }

    const EGLint pbufferAttribs[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE
    };

    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbufferAttribs);
    if (surf == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed");
        eglDestroyContext(dpy, ctx);
        return false;
    }

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        LOGE("eglMakeCurrent failed");
        eglDestroySurface(dpy, surf);
        eglDestroyContext(dpy, ctx);
        return false;
    }

    m_display = dpy;
    m_context = ctx;
    m_surface = surf;

    m_programFloat = compileComputeProgram(kComputeShaderFloat);
    m_programU8    = compileComputeProgram(kComputeShaderU8);
    if (m_programFloat == 0 || m_programU8 == 0) {
        cleanupGlLocked();
        return false;
    }

    glGenTextures(1, &m_inputTexture);
    glBindTexture(GL_TEXTURE_2D, m_inputTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenBuffers(1, &m_ssbo);

    m_ready = true;
    LOGI("GPU Compute preprocessor initialised (Float + U8 pipelines on GLES 3.1)");
    return true;
}

void GpuPreprocessor::cleanupGlLocked() {
    if (m_programFloat != 0) {
        glDeleteProgram(m_programFloat);
        m_programFloat = 0;
    }
    if (m_programU8 != 0) {
        glDeleteProgram(m_programU8);
        m_programU8 = 0;
    }
    if (m_inputTexture != 0) {
        glDeleteTextures(1, &m_inputTexture);
        m_inputTexture = 0;
    }
    if (m_ssbo != 0) {
        glDeleteBuffers(1, &m_ssbo);
        m_ssbo = 0;
    }
    if (m_display != nullptr) {
        eglMakeCurrent(static_cast<EGLDisplay>(m_display), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surface != nullptr) {
            eglDestroySurface(static_cast<EGLDisplay>(m_display), static_cast<EGLSurface>(m_surface));
            m_surface = nullptr;
        }
        if (m_context != nullptr) {
            eglDestroyContext(static_cast<EGLDisplay>(m_display), static_cast<EGLContext>(m_context));
            m_context = nullptr;
        }
    }
    m_ready = false;
    m_initAttempted = false;
}

void GpuPreprocessor::release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    cleanupGlLocked();
}

bool GpuPreprocessor::run(const FrameView& frame, const Roi& roi,
                          const PreprocessConfig& cfg, float* out, LetterboxMap& outMap) {
    if (!frame.valid() || out == nullptr || cfg.targetW <= 0 || cfg.targetH <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready && !initGlLocked()) return false;

    const auto t0 = Clock::now();

    eglMakeCurrent(static_cast<EGLDisplay>(m_display),
                   static_cast<EGLSurface>(m_surface),
                   static_cast<EGLSurface>(m_surface),
                   static_cast<EGLContext>(m_context));

    const Roi src = Roi::resolve(roi, frame);
    if (src.empty()) return false;

    const float scale = std::min(static_cast<float>(cfg.targetW) / static_cast<float>(src.w),
                                 static_cast<float>(cfg.targetH) / static_cast<float>(src.h));

    int newW = cfg.targetW;
    int newH = cfg.targetH;
    float padX = 0.0f, padY = 0.0f;
    if (cfg.keepAspect) {
        newW = std::max(1, static_cast<int>(std::lround(src.w * scale)));
        newH = std::max(1, static_cast<int>(std::lround(src.h * scale)));
        padX = static_cast<float>(cfg.targetW - newW) * 0.5f;
        padY = static_cast<float>(cfg.targetH - newH) * 0.5f;
    }

    outMap.scale = static_cast<float>(newW) / static_cast<float>(src.w);
    outMap.padX  = padX;
    outMap.padY  = padY;
    outMap.roiX  = src.x;
    outMap.roiY  = src.y;

    // 1. Upload or update input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_inputTexture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.rowStrideBytes / 4);

    if (m_texAllocW != frame.width || m_texAllocH != frame.height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels);
        m_texAllocW = frame.width;
        m_texAllocH = frame.height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                        GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // 2. Prepare SSBO
    const size_t outFloats = static_cast<size_t>(cfg.targetW) * static_cast<size_t>(cfg.targetH) * 3u;
    const size_t outBytes = outFloats * sizeof(float);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
    if (m_ssboCapacity < outBytes) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(outBytes), nullptr, GL_DYNAMIC_COPY);
        m_ssboCapacity = outBytes;
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_ssbo);

    // 3. Configure program uniforms
    glUseProgram(m_programFloat);
    glUniform1i(glGetUniformLocation(m_programFloat, "u_targetW"), cfg.targetW);
    glUniform1i(glGetUniformLocation(m_programFloat, "u_targetH"), cfg.targetH);
    glUniform4f(glGetUniformLocation(m_programFloat, "u_cropRect"),
                static_cast<float>(src.x), static_cast<float>(src.y),
                static_cast<float>(src.w), static_cast<float>(src.h));
    glUniform2f(glGetUniformLocation(m_programFloat, "u_srcSize"),
                static_cast<float>(frame.width), static_cast<float>(frame.height));
    glUniform2f(glGetUniformLocation(m_programFloat, "u_pad"), padX, padY);
    glUniform2f(glGetUniformLocation(m_programFloat, "u_scaledSize"),
                static_cast<float>(newW), static_cast<float>(newH));
    glUniform1f(glGetUniformLocation(m_programFloat, "u_scale"), cfg.scale);
    glUniform3f(glGetUniformLocation(m_programFloat, "u_mean"), cfg.mean[0], cfg.mean[1], cfg.mean[2]);
    glUniform3f(glGetUniformLocation(m_programFloat, "u_stdv"), cfg.stdv[0], cfg.stdv[1], cfg.stdv[2]);
    glUniform1i(glGetUniformLocation(m_programFloat, "u_bgr"), cfg.bgr ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_programFloat, "u_nchw"), cfg.nchw ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_programFloat, "u_padValue"), cfg.padValue);
    glUniform1i(glGetUniformLocation(m_programFloat, "u_normalize"), cfg.normalize ? 1 : 0);

    // 4. Dispatch Compute Shader
    const GLuint numGroupsX = static_cast<GLuint>((cfg.targetW + 15) / 16);
    const GLuint numGroupsY = static_cast<GLuint>((cfg.targetH + 15) / 16);
    glDispatchCompute(numGroupsX, numGroupsY, 1);

    // 5. Memory barrier & readback directly into destination tensor data
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                    static_cast<GLsizeiptr>(outBytes),
                                    GL_MAP_READ_BIT);
    if (mapped != nullptr) {
        std::memcpy(out, mapped, outBytes);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    } else {
        return false;
    }

    static int s_gpuRuns = 0;
    if (++s_gpuRuns % 60 == 1) {
        const double gpuMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        LOGI("GPU Compute preprocessed #%d (Float): %dx%d -> %dx%d in %.2fms",
             s_gpuRuns, frame.width, frame.height, cfg.targetW, cfg.targetH, gpuMs);
    }

    return true;
}

bool GpuPreprocessor::runU8(const FrameView& frame, const Roi& roi,
                            const PreprocessConfig& cfg, uint8_t* out, LetterboxMap& outMap) {
    if (!frame.valid() || out == nullptr || cfg.targetW <= 0 || cfg.targetH <= 0) {
        return false;
    }
    // Chunks require width to be a multiple of 4
    if ((cfg.targetW % 4) != 0) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready && !initGlLocked()) return false;

    const auto t0 = Clock::now();

    eglMakeCurrent(static_cast<EGLDisplay>(m_display),
                   static_cast<EGLSurface>(m_surface),
                   static_cast<EGLSurface>(m_surface),
                   static_cast<EGLContext>(m_context));

    const Roi src = Roi::resolve(roi, frame);
    if (src.empty()) return false;

    const float scale = std::min(static_cast<float>(cfg.targetW) / static_cast<float>(src.w),
                                 static_cast<float>(cfg.targetH) / static_cast<float>(src.h));

    int newW = cfg.targetW;
    int newH = cfg.targetH;
    float padX = 0.0f, padY = 0.0f;
    if (cfg.keepAspect) {
        newW = std::max(1, static_cast<int>(std::lround(src.w * scale)));
        newH = std::max(1, static_cast<int>(std::lround(src.h * scale)));
        padX = static_cast<float>(cfg.targetW - newW) * 0.5f;
        padY = static_cast<float>(cfg.targetH - newH) * 0.5f;
    }

    outMap.scale = static_cast<float>(newW) / static_cast<float>(src.w);
    outMap.padX  = padX;
    outMap.padY  = padY;
    outMap.roiX  = src.x;
    outMap.roiY  = src.y;

    // 1. Upload or update input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_inputTexture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.rowStrideBytes / 4);

    if (m_texAllocW != frame.width || m_texAllocH != frame.height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels);
        m_texAllocW = frame.width;
        m_texAllocH = frame.height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                        GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // 2. Prepare SSBO
    const size_t outBytes = static_cast<size_t>(cfg.targetW) * static_cast<size_t>(cfg.targetH) * 3u;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
    if (m_ssboCapacity < outBytes) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(outBytes), nullptr, GL_DYNAMIC_COPY);
        m_ssboCapacity = outBytes;
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_ssbo);

    // 3. Configure program uniforms
    glUseProgram(m_programU8);
    glUniform1i(glGetUniformLocation(m_programU8, "u_targetW"), cfg.targetW);
    glUniform1i(glGetUniformLocation(m_programU8, "u_targetH"), cfg.targetH);
    glUniform4f(glGetUniformLocation(m_programU8, "u_cropRect"),
                static_cast<float>(src.x), static_cast<float>(src.y),
                static_cast<float>(src.w), static_cast<float>(src.h));
    glUniform2f(glGetUniformLocation(m_programU8, "u_srcSize"),
                static_cast<float>(frame.width), static_cast<float>(frame.height));
    glUniform2f(glGetUniformLocation(m_programU8, "u_pad"), padX, padY);
    glUniform2f(glGetUniformLocation(m_programU8, "u_scaledSize"),
                static_cast<float>(newW), static_cast<float>(newH));
    glUniform1i(glGetUniformLocation(m_programU8, "u_bgr"), cfg.bgr ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_programU8, "u_padValue"), cfg.padValue * 255.0f);

    // 4. Dispatch Compute Shader: each invocation handles 4 pixels horizontally
    const GLuint chunksX = static_cast<GLuint>((cfg.targetW / 4 + 15) / 16);
    const GLuint numGroupsY = static_cast<GLuint>((cfg.targetH + 15) / 16);
    glDispatchCompute(chunksX, numGroupsY, 1);

    // 5. Memory barrier & readback directly into destination tensor data
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    void* mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                    static_cast<GLsizeiptr>(outBytes),
                                    GL_MAP_READ_BIT);
    if (mapped != nullptr) {
        std::memcpy(out, mapped, outBytes);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    } else {
        return false;
    }

    static int s_gpuU8Runs = 0;
    if (++s_gpuU8Runs % 60 == 1) {
        const double gpuMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        LOGI("GPU Compute preprocessed #%d (U8): %dx%d -> %dx%d in %.2fms",
             s_gpuU8Runs, frame.width, frame.height, cfg.targetW, cfg.targetH, gpuMs);
    }

    return true;
}

}  // namespace infer
}  // namespace aimbotng
