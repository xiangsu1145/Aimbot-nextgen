// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::capture — implementation. See capture.h for the rationale.
// ─────────────────────────────────────────────────────────────────────────────
#include "capture/capture.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#define TAG "AimbotCapture"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace aimbotng {
namespace capture {
namespace {

std::mutex         g_frameMutex;
std::vector<uint8_t> g_frame;      // latest frame, tightly packed RGBA
int                g_frameW = 0;
int                g_frameH = 0;
uint64_t           g_frameId = 0;
// Kept so a consumer can map a crop-space coordinate back onto the screen. The
// crop is centred by the producer, but the rounded origin is not something a
// consumer can recompute without knowing the full screen size — which is the
// producer's, not ours.
int                g_cropX = 0;
int                g_cropY = 0;

// Logged sparsely: the first frame proves the whole chain works, and one every
// couple of seconds after that is enough to tell "still delivering" from
// "stalled" without drowning the log.
uint64_t           g_frameTotal = 0;
constexpr uint64_t kLogEvery = 60;

std::atomic<bool>  g_enabled{false};
std::atomic<int>   g_size{640};
std::atomic<bool>  g_running{false};

// "Somebody is watching the pixels right now" — the *frames* question, as
// opposed to the *menu switch* question above. Two different things, and the
// gap between them is where the silent-capture bug lived: a caller that only
// needs frames (the inference pump) used to be counted as a reason to keep the
// virtual display alive, which made the capture switch look broken because
// turning it off changed nothing.
std::atomic<bool>  g_consuming{false};

// The reader half of the supervisor's `enabled() && previewWanted()`. Set to
// "somebody is reading pixels" — which includes inference, not just the preview
// page. See the header: keeping inference off this flag is what starved the
// detector whenever the Capture page was closed.
std::atomic<bool>  g_previewWanted{false};

// ── Frame rate ───────────────────────────────────────────────────────────────
//
// Counted on the way in, not on the way out: what the menu wants to know is how
// fast frames are arriving, which is the one thing this module sees and the
// renderer does not (it drops everything it cannot keep up with).
std::atomic<int>   g_fps{0};
std::chrono::steady_clock::time_point g_windowStart;
int                g_windowFrames = 0;
bool               g_windowOpen   = false;

// Written by the frame thread, read by the renderer — hence atomic, even though
// the values themselves are only ever used as "has it gone quiet".
std::atomic<long long> g_lastFrameMs{0};
std::atomic<bool>      g_everFramed{false};

constexpr auto kWindow = std::chrono::milliseconds(500);
constexpr auto kSilent = std::chrono::milliseconds(1000);

using Clock = std::chrono::steady_clock;

void noteFrame() {
    const auto now = Clock::now();
    if (!g_windowOpen) {
        g_windowStart  = now;
        g_windowFrames = 0;
        g_windowOpen   = true;
    }
    ++g_windowFrames;
    g_lastFrameMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count(),
                        std::memory_order_relaxed);
    g_everFramed.store(true, std::memory_order_relaxed);

    const auto elapsed = now - g_windowStart;
    if (elapsed >= kWindow) {
        g_fps.store(static_cast<int>(g_windowFrames * 1000 /
                                     std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
        g_windowStart  = now;
        g_windowFrames = 0;
    }
}

}  // namespace

void pushFrame(const void* rgba, int srcWidth, int srcHeight, int stride,
               int cropX, int cropY, int side) {
    if (rgba == nullptr || srcWidth <= 0 || srcHeight <= 0 || stride <= 0) return;
    if (cropX < 0 || cropY < 0 || side <= 0) return;
    // Short rows would make the column arithmetic below read past the row.
    if (static_cast<size_t>(stride) < static_cast<size_t>(srcWidth) * 4u) return;
    // A crop that does not fit means the producer and this module disagree about
    // the screen shape; drop it rather than show a torn frame.
    if (cropX + side > srcWidth || cropY + side > srcHeight) return;

    // Counted before anything can be skipped, so the rate reflects the producer
    // rather than whether the menu happens to be showing the page.
    noteFrame();

    // Nobody is sampling the preview. The frame is already counted; cutting the
    // square out of it would be work with no reader on the other end, and at the
    // panel's rate that is a few hundred MB/s of memcpy for nothing.
    if (!g_consuming.load(std::memory_order_relaxed)) return;

    const size_t srcPitch   = static_cast<size_t>(stride);
    const size_t dstRowBytes = static_cast<size_t>(side) * 4u;
    const auto*  src = static_cast<const uint8_t*>(rgba);

    uint64_t total;
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        // Sized on demand instead of every frame: resize() is cheap when nothing
        // changes, but it is still a call into the allocator's size check on a
        // path that runs a hundred times a second.
        if (g_frame.size() != dstRowBytes * static_cast<size_t>(side)) {
            g_frame.resize(dstRowBytes * static_cast<size_t>(side));
        }

        uint8_t* dst = g_frame.data();
        // Row padding is the classic cause of a picture that comes out sheared,
        // so it is normalised away here once rather than defended against at
        // every consumer. Rows may be padded; never short (guarded above).
        for (int y = 0; y < side; ++y) {
            memcpy(dst + static_cast<size_t>(y) * dstRowBytes,
                   src + (static_cast<size_t>(cropY + y)) * srcPitch
                       + static_cast<size_t>(cropX) * 4u,
                   dstRowBytes);
        }

        g_frameW = side;
        g_frameH = side;
        g_cropX  = cropX;
        g_cropY  = cropY;
        ++g_frameId;
        total = ++g_frameTotal;
    }

    if (total == 1 || (total % kLogEvery) == 0) {
        LOGI("frame #%llu %dx%d crop=(%d,%d) of %dx%d stride=%d",
             (unsigned long long)total, side, side, cropX, cropY,
             srcWidth, srcHeight, stride);
    }
}

bool peekSize(int& outW, int& outH) {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_frameW <= 0 || g_frame.empty()) return false;
    outW = g_frameW;
    outH = g_frameH;
    return true;
}

void invalidateFrames() {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    // Bumped, not zeroed — see the header. Every consumer holding a last-seen id
    // now finds it stale and re-reads, whichever path it came in through.
    ++g_frameId;
    // The dimensions go with it. A consumer that asks peekSize() before probing
    // for pixels must not be handed the shape of a frame that can no longer be
    // produced: the next producer may have a different screen behind it, and a
    // copy sized from the old geometry is a torn frame rather than a stale one.
    g_frameW = 0;
    g_frameH = 0;
    g_frame.clear();
    // The crop origin describes where the *gone* frame sat on the *gone* screen.
    // Kept, it would have onResult() translate detections from a dead frame by
    // the offset of a screen that may since have rotated.
    g_cropX = 0;
    g_cropY = 0;
    LOGI("frames invalidated (generation -> %llu)", (unsigned long long)g_frameId);
}

bool copyLatestInto(void* dst, size_t dstBytes, int& outW, int& outH,
                    uint64_t& lastId) {
    if (dst == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_frameId == lastId || g_frameW <= 0 || g_frame.empty()) return false;
    if (dstBytes < g_frame.size()) return false;

    memcpy(dst, g_frame.data(), g_frame.size());
    lastId = g_frameId;
    outW   = g_frameW;
    outH   = g_frameH;
    return true;
}

int frameRate() {
    // A producer that has stopped does not report the last rate it managed — it
    // reports nothing. Otherwise a stalled capture would sit there displaying
    // whatever it was doing before it died, which is exactly the failure the
    // number exists to reveal.
    if (!g_everFramed.load(std::memory_order_relaxed)) return 0;
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                Clock::now().time_since_epoch()).count();
    const long long lastMs = g_lastFrameMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs >= std::chrono::milliseconds(kSilent).count()) return 0;
    return g_fps.load(std::memory_order_relaxed);
}

void setConsuming(bool on) {
    const bool prev = g_consuming.exchange(on, std::memory_order_relaxed);
    if (prev != on) {
        LOGI("capture: consuming %s->%s", prev ? "on" : "off", on ? "on" : "off");
    }
}
bool consuming() { return g_consuming.load(std::memory_order_relaxed); }

void setPreviewWanted(bool on) {
    const bool prev = g_previewWanted.exchange(on, std::memory_order_relaxed);
    if (prev != on) {
        LOGI("capture: preview wanted %s->%s", prev ? "on" : "off", on ? "on" : "off");
    }
}
bool previewWanted() { return g_previewWanted.load(std::memory_order_relaxed); }

bool cropOrigin(int& outX, int& outY) {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_frameW <= 0 || g_frame.empty()) return false;
    outX = g_cropX;
    outY = g_cropY;
    return true;
}

void setEnabled(bool on) { g_enabled.store(on); }
bool enabled() { return g_enabled.load(); }

void setSize(int side) {
    if (side < 64) side = 64;
    if (side > 1024) side = 1024;
    g_size.store(side);
}
int size() { return g_size.load(); }

void setRunning(bool on) { g_running.store(on); }
bool running() { return g_running.load(); }

}  // namespace capture
}  // namespace aimbotng
