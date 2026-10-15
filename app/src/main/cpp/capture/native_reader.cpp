#include "capture/native_reader.h"
#include "capture/capture.h"
#include "inference/model_runtime.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

#define TAG "AimbotNatCap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace aimbotng {
namespace capture {
namespace {

std::mutex             g_readerMutex;
AImageReader*          g_reader = nullptr;
ANativeWindow*         g_window = nullptr;
std::atomic<uint64_t>  g_generation{0};
std::atomic<bool>      g_active{false};
std::atomic<int>       g_originX{0};
std::atomic<int>       g_originY{0};

void onNativeImageAvailable(void* context, AImageReader* reader) {
    if (reader == nullptr) return;
    const uint64_t gen = reinterpret_cast<uintptr_t>(context);
    if (gen != g_generation.load(std::memory_order_relaxed)) return;

    // ── Drain queue and retain only the freshest frame (Qnncpu sub_39523CC) ──
    AImage* latestImage = nullptr;
    AImage* img = nullptr;
    while (AImageReader_acquireNextImage(reader, &img) == AMEDIA_OK && img != nullptr) {
        if (latestImage != nullptr) {
            AImage_delete(latestImage);
        }
        latestImage = img;
    }

    if (latestImage == nullptr) return;

    int32_t width = 0, height = 0;
    AImage_getWidth(latestImage, &width);
    AImage_getHeight(latestImage, &height);

    int64_t tsNs = 0;
    AImage_getTimestamp(latestImage, &tsNs);

    uint8_t* planeData = nullptr;
    int planeLen = 0;
    int32_t rowStride = 0;

    if (AImage_getPlaneData(latestImage, 0, &planeData, &planeLen) == AMEDIA_OK &&
        AImage_getPlaneRowStride(latestImage, 0, &rowStride) == AMEDIA_OK &&
        planeData != nullptr && width > 0 && height > 0) {

        int side = capture::size();
        if (side <= 0) side = 640;
        side = std::min(side, std::min(width, height));
        int cropX = (width - side) / 2;
        int cropY = (height - side) / 2;

        capture::pushFrame(planeData, width, height, rowStride,
                           cropX, cropY, side, static_cast<uint64_t>(tsNs));

        // Event-driven direct pump: immediately wake inference worker with 0ms UI delay
        if (infer::runtime::wantsFrames()) {
            infer::runtime::pump();
        }
    }

    AImage_delete(latestImage);
}

}  // namespace

jobject createNativeCaptureReader(JNIEnv* env, int width, int height) {
    std::lock_guard<std::mutex> lock(g_readerMutex);
    destroyNativeCaptureReader();

    if (width <= 0 || height <= 0) {
        LOGE("createNativeCaptureReader: invalid dimensions %dx%d", width, height);
        return nullptr;
    }

    const uint64_t gen = ++g_generation;

    // Matches Qnncpu sub_3951D80: CPU read often + GPU sampled image (0x103)
    media_status_t status = AImageReader_newWithUsage(
        width, height,
        AIMAGE_FORMAT_RGBA_8888,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        4,
        &g_reader
    );

    if (status != AMEDIA_OK || g_reader == nullptr) {
        LOGE("AImageReader_newWithUsage(%dx%d) failed with status %d", width, height, status);
        g_reader = nullptr;
        return nullptr;
    }

    status = AImageReader_getWindow(g_reader, &g_window);
    if (status != AMEDIA_OK || g_window == nullptr) {
        LOGE("AImageReader_getWindow failed with status %d", status);
        AImageReader_delete(g_reader);
        g_reader = nullptr;
        g_window = nullptr;
        return nullptr;
    }

    AImageReader_ImageListener listener{
        .context = reinterpret_cast<void*>(static_cast<uintptr_t>(gen)),
        .onImageAvailable = onNativeImageAvailable
    };
    AImageReader_setImageListener(g_reader, &listener);

    // Convert ANativeWindow to Java Surface. The returned Surface holds a reference.
    jobject surface = ANativeWindow_toSurface(env, g_window);
    if (surface == nullptr) {
        LOGE("ANativeWindow_toSurface returned null");
        AImageReader_delete(g_reader);
        g_reader = nullptr;
        g_window = nullptr;
        return nullptr;
    }

    g_active.store(true, std::memory_order_release);
    LOGI("Native AImageReader created %dx%d (gen=%llu)", width, height, (unsigned long long)gen);
    return surface;
}

void destroyNativeCaptureReader() {
    if (g_reader != nullptr) {
        ++g_generation;  // invalidate any pending callback
        AImageReader_delete(g_reader);
        g_reader = nullptr;
        g_window = nullptr;
        g_active.store(false, std::memory_order_release);
        LOGI("Native AImageReader destroyed");
    }
}

bool isNativeCaptureReaderActive() {
    return g_active.load(std::memory_order_relaxed);
}

}  // namespace capture
}  // namespace aimbotng
