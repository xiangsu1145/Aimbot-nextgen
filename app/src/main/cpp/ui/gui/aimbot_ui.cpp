// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui — ImGui over Vulkan (implementation)
//
//  Renders ImGui into an ANativeWindow (a SurfaceView surface) through a
//  VK_KHR_android_surface swapchain. Every Vulkan object is created on the
//  render thread so the caller's thread never blocks on driver work.
//
//  Alpha: the swapchain is created with VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
//  (when supported) and cleared to fully transparent black, so everything
//  outside the ImGui windows stays see-through and the app underneath shows
//  through.
// ─────────────────────────────────────────────────────────────────────────────
#include "aimbot_ui.h"

#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "capture/capture.h"
#include "config/config_manager.h"
#include "inference/model_runtime.h"
#include "input/skip_screenshot.h"
#include "ui/gui/float_button.h"
#include "ui/gui/notify.h"
#include "ui/gui/sections/settings_section.h"
// For sections::aimIsDriving(), which the frame-rate tier below consults so the
// aim loop runs at the display's rate while a target is engaged.
#include "ui/gui/sections/aim_section.h"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#ifdef HAS_IMGUI
#include "fonts.h"
#include "hud.h"
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_vulkan.h"
// Only for enumerating windows (ImGuiContext::Windows) to publish their
// rectangles — see getInteractiveRegions().
#include "imgui_internal.h"
#endif

#define TAG "AimbotNg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace aimbotng {
namespace ui {
namespace {

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
constexpr uint32_t kApiVersion = VK_API_VERSION_1_1;

// Target render rate for the pacing sleep at the bottom of the loop. Present is
// MAILBOX (non-blocking), so the loop has to pace itself; this is the *period*
// it aims for, not a sleep added on top of the frame's own cost.
//
// We pace to the refresh rate we already voted for above
// (ANativeWindow_setFrameRate(120)): on a 120 Hz panel this lands every vsync,
// and on a 60 Hz panel SurfaceFlinger already throttles us down. A higher
// number here would just spend CPU on frames that SF then throws away, which
// is what made the menu feel uneven when this was 60 on a 120 Hz device.
constexpr int kTargetFps     = 120;
constexpr int kFramePeriodUs = 1000000 / kTargetFps;  // 8333 µs

// ── Adaptive pacing ─────────────────────────────────────────────────────────
// Rendering every frame at the refresh rate is the single largest source of
// heat in this daemon: an untouched menu repaints a byte-identical 2K layer
// 120 times a second, and the GPU never gets a long enough gap to drop out of
// its high-power state. The measured render cost is ~2.4 ms per frame — the
// loop is idle for two thirds of every period and still burns power in all of
// them.
//
// So the rate follows what is actually changing:
//
//   active   120 Hz  a finger is down, or the show/hide ease is running
//   preview   30 Hz  the Capture page is up and frames are arriving
//   idle      10 Hz  the board is on screen and settled — nothing to redraw
//   dormant    2 Hz  the board is hidden; only the float button is left
//
// Dropping the *render* rate does not cost responsiveness. The sleep at the
// bottom of the loop is sliced, and a touch landing mid-slice ends it early,
// so a tap is on screen within one poll rather than waiting out the period.
constexpr int     kPreviewFps     = 30;
constexpr int     kIdleFps        = 10;
constexpr int     kDormantFps     = 2;
/// The rate the loop holds while inference is running.
///
/// Inference is fed from this loop, so the loop's pace is the detector's frame
/// supply — and the idle tiers are far below what a detector wants. This was
/// found the hard way: at the idle tier the loop runs at 10 fps, so a model that
/// could have run twenty times a second got ten frames to run on. It is not the
/// render that needs the rate, it is the pump hanging off it.
///
/// 60 rather than the full 120 because the extra frames reach nothing: a
/// detector is happy with 60 and the loop's own work is ~3 ms a frame, so 30
/// more redraws a second would buy 10% of a core for no result.
constexpr int     kInferenceFps   = 60;
constexpr int64_t kPreviewPeriodUs = 1000000 / kPreviewFps;
constexpr int64_t kIdlePeriodUs    = 1000000 / kIdleFps;
constexpr int64_t kDormantPeriodUs = 1000000 / kDormantFps;
constexpr int64_t kInferencePeriodUs = 1000000 / kInferenceFps;
/// How long after the last touch or ease the loop keeps the full rate, so a
/// tap that starts an animation is not throttled mid-transition.
constexpr int64_t kActiveHoldUs = 600'000;
/// Sleep slice. Bounds how long a touch can sit unhandled while idle.
constexpr int64_t kPollUs       = 4'000;

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Milliseconds between a steady_clock mark and now. Used only by the
/// render-thread bring-up timing, where the point is to say how long Vulkan and
/// ImGui took on this device rather than to be precise to the microsecond.
static double msSince(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

// ── Refresh-rate vote ───────────────────────────────────────────────────────
// Voting 120 Hz once at start-up pins the *panel* to 120 Hz for as long as the
// layer exists — SurfaceFlinger recomposes a 2K screen 120 times a second on
// our behalf even while the loop is coasting at 10. Rendering less is half the
// fix; the vote has to follow, or the panel keeps burning it back.
//
// The symbol is API 30 and absent from NDK 29's libandroid.so, so it is
// resolved at runtime and may simply be missing.
using SetFrameRateT = int (*)(ANativeWindow*, float, int);
SetFrameRateT g_setFrameRate = nullptr;
float         g_votedFps     = -1.0f;
// Defined below, once g_window exists.
static void voteFrameRate(float fps);

/// How long a *lower* rate has to hold before the panel is asked to follow.
/// Raising is immediate (a tap must not feel laggy); lowering waits, because
/// switching display modes is visible and must not chase every idle blip.
constexpr int64_t kVoteHoldUs = 2'000'000;

// ── Window / thread ──────────────────────────────────────────────────────────
ANativeWindow* g_window = nullptr;  // owned; released by stop()
std::atomic<bool> g_running{false};
pthread_t g_thread = 0;
/// Render-thread-local. Set once, when the first present has been queued, so
/// the bring-up timing is logged exactly once and a later frame cannot repeat
/// it. Render thread only — no synchronisation needed and none wanted.
bool g_firstFrameLogged = false;
// The ImGui demo was the placeholder while the renderer was being built; the
// real GUI (ui/gui/hud.*) now draws itself with the draw list. Flip this back
// to true to get the demo window alongside the board for inspection.
bool g_showDemo = false;

/// Guards ownership of `g_window` between setWindow()/stop(). The render thread
/// never takes it: it only touches the window while start()/stop() bracket it.
std::mutex g_sessionMutex;

// ── Vulkan core ──────────────────────────────────────────────────────────────
VkInstance       g_instance    = VK_NULL_HANDLE;
VkSurfaceKHR     g_vkSurface   = VK_NULL_HANDLE;
VkPhysicalDevice g_physDevice  = VK_NULL_HANDLE;
VkDevice         g_device      = VK_NULL_HANDLE;
uint32_t         g_queueFamily = 0;
VkQueue          g_queue       = VK_NULL_HANDLE;

// ── Swapchain + render targets ───────────────────────────────────────────────
VkSwapchainKHR             g_swapchain  = VK_NULL_HANDLE;
VkFormat                   g_swapFormat = VK_FORMAT_UNDEFINED;
VkExtent2D                 g_extent     = {0, 0};
VkRenderPass               g_renderPass = VK_NULL_HANDLE;
VkCommandPool              g_cmdPool    = VK_NULL_HANDLE;
std::vector<VkImage>       g_images;
std::vector<VkImageView>   g_imageViews;
std::vector<VkFramebuffer> g_framebuffers;
std::vector<VkCommandBuffer> g_cmdBuffers;      // MAX_FRAMES_IN_FLIGHT
std::vector<VkSemaphore>     g_imageAvailable;  // MAX_FRAMES_IN_FLIGHT
std::vector<VkSemaphore>     g_renderFinished;  // one per swapchain image
std::vector<VkFence>         g_inFlight;        // MAX_FRAMES_IN_FLIGHT
uint32_t                     g_frame = 0;

// ── Swapchain health counters ────────────────────────────────────────────────
//
// A swapchain can be reported as merely *suboptimal* on every single frame: all
// it takes is a `preTransform` that disagrees with the surface's
// `currentTransform`, which is the normal state in portrait on this panel (the
// platform asks for ROTATE_270, we present IDENTITY and let the compositor do
// the turn — that is what keeps the board upright).
//
// A suboptimal swapchain still presents correctly, so it must NOT be rebuilt:
// rebuilding costs a vkDeviceWaitIdle plus a fresh set of full-screen images, and
// doing that per frame is what pinned portrait to ~45 fps while landscape (whose
// surface transform is IDENTITY, so the swapchain never goes suboptimal) ran at
// 110+. Only VK_ERROR_OUT_OF_DATE_KHR — the swapchain is genuinely unusable —
// earns a rebuild. Suboptimal is counted and logged instead.
std::atomic<int> g_recreateCount{0};
std::atomic<int> g_suboptimalCount{0};

#ifdef HAS_IMGUI
ImGui_ImplVulkan_PipelineInfo g_pipelineInfo{};

// ── Capture preview texture ──────────────────────────────────────────────────
//
// Frames arrive from the Java side already packed as RGBA (see
// capture/capture.h), so showing one is just an upload: staging buffer -> image
// -> the descriptor set ImGui samples. The texture is rebuilt whenever the
// frame's size changes, which is exactly when the menu's size slider moves.
//
// Only frames that are actually new get uploaded. The producer runs well under
// the render loop's rate, so most frames have nothing to do here.
//
// The staging side is a small ring, not one buffer, because the upload is
// submitted asynchronously: with a single buffer the CPU would have to wait for
// its own submit to finish before touching it again, which is a GPU stall per
// frame for no reason. With kCapStagingSlots buffers, slot N is only waited on
// three uploads after it was last used — by which time the fence has almost
// always signalled already.
VkImage          g_capImage      = VK_NULL_HANDLE;
VkDeviceMemory   g_capImageMem   = VK_NULL_HANDLE;
VkImageView      g_capView       = VK_NULL_HANDLE;
VkSampler        g_capSampler    = VK_NULL_HANDLE;
VkDescriptorSet  g_capTexture    = VK_NULL_HANDLE;
int              g_capTexW       = 0;
int              g_capTexH       = 0;
uint64_t         g_capSeenId     = 0;

/// Whether the image has been written at least once, which decides the layout
/// the upload barrier has to come *from*. Until then it is still UNDEFINED.
bool             g_capImageWritten = false;

constexpr int    kCapStagingSlots = 3;
VkBuffer         g_capStaging        [kCapStagingSlots] = {};
VkDeviceMemory   g_capStagingMem     [kCapStagingSlots] = {};
void*            g_capStagingPtr     [kCapStagingSlots] = {};
VkFence          g_capStagingFence   [kCapStagingSlots] = {};
size_t           g_capStagingLen     = 0;
int              g_capStagingNext    = 0;

/// The command buffers uploads re-record, plus the pool they came from.
///
/// Deliberately NOT g_cmdPool: that one is destroyed and rebuilt on every
/// swapchain recreate, which would leave these buffers dangling. A pool of its
/// own lives exactly as long as the texture does.
///
/// One per ring slot, not one shared: a command buffer must not be re-recorded
/// while a submit that uses it is still pending, and the only fence this code
/// waits on is the slot's own. Sharing one buffer would mean re-recording the
/// previous slot's submit — which, with the wait moved off the queue, is no
/// longer guaranteed to have finished.
VkCommandPool    g_capCmdPool               = VK_NULL_HANDLE;
VkCommandBuffer  g_capCmd[kCapStagingSlots] = {};

/// Index of a memory type that both fits `typeBits` and carries all of `wanted`.
uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(g_physDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}
#endif  // HAS_IMGUI

// ── Input queue (producer: caller threads, consumer: render thread) ──────────
struct TouchEvent {
    int   action;
    float x;
    float y;
};

std::mutex            g_inputMutex;
std::vector<TouchEvent> g_inputQueue;
float                 g_pendingScroll = 0.0f;

// ── Interactive regions ──────────────────────────────────────────────────────
// Flattened [x, y, w, h] quads of the windows currently under the finger.
// Written by the render thread once per frame, read by whoever calls
// getInteractiveRegions().
constexpr int kMaxRegions = 8;

std::mutex       g_regionMutex;
std::vector<int> g_regions;

bool vkOk(VkResult err, const char* what) {
    if (err != VK_SUCCESS) {
        LOGE("%s failed (VkResult=%d)", what, (int)err);
        return false;
    }
    return true;
}

void checkVkResult(VkResult err) {
    if (err != VK_SUCCESS) LOGE("ImGui Vulkan backend VkResult=%d", (int)err);
}

// ── Instance / device ────────────────────────────────────────────────────────

bool createInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "AimbotNg";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "AimbotNg";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = kApiVersion;

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;

    return vkOk(vkCreateInstance(&ci, nullptr, &g_instance), "vkCreateInstance");
}

bool createSurface() {
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = g_window;
    return vkOk(vkCreateAndroidSurfaceKHR(g_instance, &ci, nullptr, &g_vkSurface),
                "vkCreateAndroidSurfaceKHR");
}

bool deviceSupportsSwapchain(VkPhysicalDevice dev) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) return true;
    }
    return false;
}

/** Picks a physical device that has a queue family supporting graphics + present. */
bool pickPhysicalDevice() {
    uint32_t count = 0;
    if (!vkOk(vkEnumeratePhysicalDevices(g_instance, &count, nullptr), "vkEnumeratePhysicalDevices") ||
        count == 0) {
        LOGE("no Vulkan physical device available");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g_instance, &count, devices.data());

    for (VkPhysicalDevice dev : devices) {
        if (!deviceSupportsSwapchain(dev)) continue;

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, families.data());

        for (uint32_t i = 0; i < qCount; i++) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, g_vkSurface, &present);
            if (present != VK_TRUE) continue;

            g_physDevice = dev;
            g_queueFamily = i;
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(dev, &props);
            LOGI("Vulkan device: %s (api %u.%u.%u)", props.deviceName,
                 VK_VERSION_MAJOR(props.apiVersion),
                 VK_VERSION_MINOR(props.apiVersion),
                 VK_VERSION_PATCH(props.apiVersion));
            return true;
        }
    }
    LOGE("no queue family with graphics + present support");
    return false;
}

bool createDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = extensions;
    ci.pEnabledFeatures = &features;

    if (!vkOk(vkCreateDevice(g_physDevice, &ci, nullptr, &g_device), "vkCreateDevice")) return false;
    vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);
    return true;
}

// ── Swapchain ────────────────────────────────────────────────────────────────

bool createSwapchain(int width, int height) {
    VkSurfaceCapabilitiesKHR caps{};
    if (!vkOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physDevice, g_vkSurface, &caps),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }

    // Format: prefer an 8-bit RGBA format so the surface carries alpha.
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_physDevice, g_vkSurface, &formatCount, nullptr);
    if (formatCount == 0) {
        LOGE("surface reports no formats");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_physDevice, g_vkSurface, &formatCount, formats.data());

    g_swapFormat = formats[0].format;
    VkColorSpaceKHR colorSpace = formats[0].colorSpace;
    const VkFormat wanted[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    for (VkFormat w : wanted) {
        bool hit = false;
        for (const auto& f : formats) {
            if (f.format == w) {
                g_swapFormat = w;
                colorSpace = f.colorSpace;
                hit = true;
                break;
            }
        }
        if (hit) break;
    }

    // Surface frame rate vote: tell SurfaceFlinger to schedule our layer at the
    // panel's peak rate rather than let it fall back to whatever the system is
    // currently in. Without this hint the surface can be throttled to 50/60 Hz
    // even on a 120 Hz panel — that is the "render says 117 fps but the
    // animation looks jerky" failure mode. We ask for 120 fps; the platform
    // picks the closest rate the panel supports.
    //
    // The symbol is API 30, so it is NOT in NDK 29's libandroid.so by default.
    // We resolve it at runtime via dlsym: the linker bails otherwise.
    if (g_window != nullptr) {
        g_setFrameRate = reinterpret_cast<SetFrameRateT>(
            dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
        if (g_setFrameRate != nullptr) {
            const int err = g_setFrameRate(g_window, 120.0f, 1 /*DEFAULT*/);
            g_votedFps = (err == 0) ? 120.0f : -1.0f;
            LOGI("surface frame-rate vote: 120 fps, err=%d", err);
        } else {
            LOGW("ANativeWindow_setFrameRate unavailable — surface will use the"
                 " panel default (the menu may look uneven at < refresh rate)");
        }
    }

    // Present mode: MAILBOX when available (lower latency), else FIFO (always supported).
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_physDevice, g_vkSurface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_physDevice, g_vkSurface, &modeCount, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) presentMode = m;
    }

    // ── Extent: the WINDOW's size, never `caps.currentExtent` ────────────────
    //
    // `currentExtent` is quoted in the display's logical orientation, which is
    // NOT the size of the buffer we end up with once the buffer has to carry a
    // transform. The window's size *is* that buffer size, and it is also the
    // rectangle the layer's crop and display frame are expressed in and what
    // ImGui receives as DisplaySize. Keeping all three the same rectangle is
    // what stops SurfaceFlinger from scaling the buffer into its frame.
    //
    // (That scale is not a clipping — it is non-uniform. A landscape buffer in a
    // portrait frame compresses x by 0.71 and stretches y by 1.42, which is the
    // "landscape layout, squashed flat" portrait symptom.)
    const uint32_t winW = static_cast<uint32_t>(std::max(0, ANativeWindow_getWidth(g_window)));
    const uint32_t winH = static_cast<uint32_t>(std::max(0, ANativeWindow_getHeight(g_window)));

    VkExtent2D extent;
    if (winW > 0 && winH > 0) {
        extent.width  = winW;
        extent.height = winH;
    } else if (caps.currentExtent.width != 0xFFFFFFFFu) {
        extent = caps.currentExtent;
    } else {
        extent.width  = static_cast<uint32_t>(width);
        extent.height = static_cast<uint32_t>(height);
    }
    // Stay inside what the surface allows. With preTransform = IDENTITY the caps
    // are quoted in the same space the window is, so this is a straight clamp.
    if (caps.minImageExtent.width > 0) {
        const uint32_t clampedW = std::max(caps.minImageExtent.width,
                                           std::min(caps.maxImageExtent.width, extent.width));
        const uint32_t clampedH = std::max(caps.minImageExtent.height,
                                           std::min(caps.maxImageExtent.height, extent.height));
        if (clampedW != extent.width || clampedH != extent.height) {
            LOGI("extent clamped to surface caps: %ux%u -> %ux%u",
                 extent.width, extent.height, clampedW, clampedH);
            extent.width  = clampedW;
            extent.height = clampedH;
        }
    }
    if (extent.width == 0 || extent.height == 0) {
        LOGE("swapchain extent is 0x0");
        return false;
    }

    LOGI("surface: window=%ux%u currentExtent=%ux%u currentTransform=0x%x "
         "preTransform=IDENTITY -> extent=%ux%u",
         winW, winH, caps.currentExtent.width, caps.currentExtent.height,
         static_cast<unsigned>(caps.currentTransform), extent.width, extent.height);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    // Composite alpha: INHERIT lets the Surface's own alpha (PixelFormat.TRANSLUCENT)
    // drive compositing, which is what keeps the overlay see-through.
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR preferredAlpha[] = {
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    };
    for (auto a : preferredAlpha) {
        if (caps.supportedCompositeAlpha & a) {
            compositeAlpha = a;
            break;
        }
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_vkSurface;
    ci.minImageCount = imageCount;
    ci.imageFormat = g_swapFormat;
    ci.imageColorSpace = colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // ── Pre-rotation is deliberately NOT claimed ─────────────────────────────
    //
    // The platform's hint (`caps.currentTransform`) is for apps that want to
    // render already-rotated content and skip a compositor pass. Claiming it
    // makes the buffer itself come out rotated — i.e. at a 90/270 hint a
    // request for 2120x3000 allocates a 3000x2120 buffer — while the layer's
    // crop / frame and ImGui's DisplaySize still describe the logical 2120x3000
    // rectangle. SurfaceFlinger then scales the two axes by different factors
    // (it fits the buffer into the frame), and the menu is drawn as a landscape
    // board squeezed upright: the portrait bug.
    //
    // Declaring IDENTITY asks the compositor to do the rotation for us. The
    // buffer is then exactly the size the window says it is, in the same space
    // as the layer's frame, and all four agree — buffer, crop, frame,
    // DisplaySize. The extra compositor pass is irrelevant for a UI.
    ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    ci.compositeAlpha = compositeAlpha;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    if (!vkOk(vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain), "vkCreateSwapchainKHR")) {
        return false;
    }

    uint32_t actual = 0;
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &actual, nullptr);
    if (actual < 2) {
        LOGE("swapchain returned %u images (need >= 2)", actual);
        return false;
    }
    g_images.resize(actual);
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &actual, g_images.data());
    g_extent = extent;

    LOGI("swapchain %ux%u images=%u fmt=%d alpha=0x%x present=%d",
         extent.width, extent.height, actual, (int)g_swapFormat, (int)compositeAlpha, (int)presentMode);
    return true;
}

bool createImageViews() {
    g_imageViews.resize(g_images.size());
    for (size_t i = 0; i < g_images.size(); i++) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = g_images[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = g_swapFormat;
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.layerCount = 1;
        if (!vkOk(vkCreateImageView(g_device, &ci, nullptr, &g_imageViews[i]), "vkCreateImageView")) {
            return false;
        }
    }
    return true;
}

bool createRenderPass() {
    VkAttachmentDescription color{};
    color.format = g_swapFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    return vkOk(vkCreateRenderPass(g_device, &ci, nullptr, &g_renderPass), "vkCreateRenderPass");
}

bool createFramebuffers() {
    g_framebuffers.resize(g_imageViews.size());
    for (size_t i = 0; i < g_imageViews.size(); i++) {
        VkImageView attachments[] = {g_imageViews[i]};
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass = g_renderPass;
        ci.attachmentCount = 1;
        ci.pAttachments = attachments;
        ci.width = g_extent.width;
        ci.height = g_extent.height;
        ci.layers = 1;
        if (!vkOk(vkCreateFramebuffer(g_device, &ci, nullptr, &g_framebuffers[i]),
                  "vkCreateFramebuffer")) {
            return false;
        }
    }
    return true;
}

bool createCommandBuffers() {
    g_cmdBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    return vkOk(vkAllocateCommandBuffers(g_device, &ai, g_cmdBuffers.data()),
                "vkAllocateCommandBuffers");
}

bool createSyncObjects() {
    g_imageAvailable.resize(MAX_FRAMES_IN_FLIGHT);
    g_inFlight.resize(MAX_FRAMES_IN_FLIGHT);
    g_renderFinished.resize(g_images.size());

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (!vkOk(vkCreateSemaphore(g_device, &si, nullptr, &g_imageAvailable[i]),
                  "vkCreateSemaphore(imageAvailable)") ||
            !vkOk(vkCreateFence(g_device, &fi, nullptr, &g_inFlight[i]), "vkCreateFence")) {
            return false;
        }
    }
    for (size_t i = 0; i < g_renderFinished.size(); i++) {
        if (!vkOk(vkCreateSemaphore(g_device, &si, nullptr, &g_renderFinished[i]),
                  "vkCreateSemaphore(renderFinished)")) {
            return false;
        }
    }
    return true;
}

// ── Teardown ─────────────────────────────────────────────────────────────────

/** Destroys everything that depends on the swapchain (safe to call repeatedly). */
void destroySwapchainDependent() {
    if (g_device == VK_NULL_HANDLE) return;

    for (VkFence f : g_inFlight) if (f) vkDestroyFence(g_device, f, nullptr);
    for (VkSemaphore s : g_imageAvailable) if (s) vkDestroySemaphore(g_device, s, nullptr);
    for (VkSemaphore s : g_renderFinished) if (s) vkDestroySemaphore(g_device, s, nullptr);
    g_inFlight.clear();
    g_imageAvailable.clear();
    g_renderFinished.clear();

    if (g_cmdPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(g_device, g_cmdPool, nullptr);
        g_cmdPool = VK_NULL_HANDLE;
    }
    g_cmdBuffers.clear();

    for (VkFramebuffer fb : g_framebuffers) if (fb) vkDestroyFramebuffer(g_device, fb, nullptr);
    g_framebuffers.clear();

    for (VkImageView v : g_imageViews) if (v) vkDestroyImageView(g_device, v, nullptr);
    g_imageViews.clear();

    if (g_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
    }
    g_images.clear();

    if (g_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_device, g_renderPass, nullptr);
        g_renderPass = VK_NULL_HANDLE;
    }
}

/** Rebuilds the swapchain after a resize / genuinely out-of-date result. */
// ── Capture preview: build / upload / tear down ──────────────────────────────

/** Releases everything behind the preview texture. Safe to call at any time. */
void destroyCaptureTexture() {
    if (g_device == VK_NULL_HANDLE) return;
    // Uploads are in flight on the same queue the menu draws with, so a full
    // idle is the only honest way to know the image and the ring are safe to
    // release. This runs when the size changes, not per frame.
    vkDeviceWaitIdle(g_device);

    if (g_capTexture != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(g_capTexture);
        g_capTexture = VK_NULL_HANDLE;
    }
    if (g_capSampler != VK_NULL_HANDLE) {
        vkDestroySampler(g_device, g_capSampler, nullptr);
        g_capSampler = VK_NULL_HANDLE;
    }
    if (g_capView != VK_NULL_HANDLE) {
        vkDestroyImageView(g_device, g_capView, nullptr);
        g_capView = VK_NULL_HANDLE;
    }
    if (g_capImage != VK_NULL_HANDLE) {
        vkDestroyImage(g_device, g_capImage, nullptr);
        g_capImage = VK_NULL_HANDLE;
    }
    if (g_capImageMem != VK_NULL_HANDLE) {
        vkFreeMemory(g_device, g_capImageMem, nullptr);
        g_capImageMem = VK_NULL_HANDLE;
    }

    for (int i = 0; i < kCapStagingSlots; ++i) {
        if (g_capStaging[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(g_device, g_capStaging[i], nullptr);
            g_capStaging[i] = VK_NULL_HANDLE;
        }
        if (g_capStagingMem[i] != VK_NULL_HANDLE) {
            vkFreeMemory(g_device, g_capStagingMem[i], nullptr);
            g_capStagingMem[i] = VK_NULL_HANDLE;
        }
        if (g_capStagingFence[i] != VK_NULL_HANDLE) {
            vkDestroyFence(g_device, g_capStagingFence[i], nullptr);
            g_capStagingFence[i] = VK_NULL_HANDLE;
        }
        g_capStagingPtr[i] = nullptr;
    }

    // The command buffers are freed with their pool, so nothing to free first.
    if (g_capCmdPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(g_device, g_capCmdPool, nullptr);
        g_capCmdPool = VK_NULL_HANDLE;
    }
    for (int i = 0; i < kCapStagingSlots; ++i) g_capCmd[i] = VK_NULL_HANDLE;

    g_capImageWritten = false;
    g_capStagingNext  = 0;
    g_capStagingLen   = 0;
    g_capTexW = 0;
    g_capTexH = 0;
}

/**
 * Builds the image / view / sampler / descriptor for a w x h frame, plus the
 * staging buffer that feeds it.
 *
 * Rebuilt from scratch when the size changes. Given the slider's range that is
 * a handful of rebuilds per session rather than one per frame, so the simple
 * "tear it all down and start again" is the right trade.
 */
bool createCaptureTexture(int w, int h) {
    if (w <= 0 || h <= 0) return false;

    // Nearest filtering: a capture is something to inspect, and a grid of
    // pixels should stay a grid of pixels rather than be smoothed into mush.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!vkOk(vkCreateSampler(g_device, &sci, nullptr, &g_capSampler), "vkCreateSampler(capture)")) {
        return false;
    }

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!vkOk(vkCreateImage(g_device, &ici, nullptr, &g_capImage), "vkCreateImage(capture)")) {
        return false;
    }

    VkMemoryRequirements imgReq{};
    vkGetImageMemoryRequirements(g_device, g_capImage, &imgReq);
    const uint32_t imageType = findMemoryType(imgReq.memoryTypeBits, 0);
    if (imageType == UINT32_MAX) {
        LOGE("capture texture: no memory type for the image");
        return false;
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = imgReq.size;
    mai.memoryTypeIndex = imageType;
    if (!vkOk(vkAllocateMemory(g_device, &mai, nullptr, &g_capImageMem), "vkAllocateMemory(capture image)") ||
        !vkOk(vkBindImageMemory(g_device, g_capImage, g_capImageMem, 0), "vkBindImageMemory(capture)")) {
        return false;
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = g_capImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (!vkOk(vkCreateImageView(g_device, &vci, nullptr, &g_capView), "vkCreateImageView(capture)")) {
        return false;
    }

    g_capTexture = ImGui_ImplVulkan_AddTexture(
        g_capSampler, g_capView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (g_capTexture == VK_NULL_HANDLE) {
        LOGE("capture texture: ImGui_ImplVulkan_AddTexture failed");
        return false;
    }

    // Staging is host-visible *and* coherent, so an upload is a memcpy plus one
    // barrier — nothing to flush, nothing to invalidate. One buffer per ring
    // slot, each with the fence that says when the GPU has finished reading it.
    g_capStagingLen = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = g_capStagingLen;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    for (int i = 0; i < kCapStagingSlots; ++i) {
        if (!vkOk(vkCreateBuffer(g_device, &bci, nullptr, &g_capStaging[i]),
                  "vkCreateBuffer(capture staging)")) {
            return false;
        }
        VkMemoryRequirements bufReq{};
        vkGetBufferMemoryRequirements(g_device, g_capStaging[i], &bufReq);
        const uint32_t stagingType = findMemoryType(
            bufReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (stagingType == UINT32_MAX) {
            LOGE("capture texture: no host-visible memory type");
            return false;
        }
        VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        smai.allocationSize = bufReq.size;
        smai.memoryTypeIndex = stagingType;
        if (!vkOk(vkAllocateMemory(g_device, &smai, nullptr, &g_capStagingMem[i]),
                  "vkAllocateMemory(capture staging)") ||
            !vkOk(vkBindBufferMemory(g_device, g_capStaging[i], g_capStagingMem[i], 0),
                  "vkBindBufferMemory(capture)") ||
            !vkOk(vkMapMemory(g_device, g_capStagingMem[i], 0, g_capStagingLen, 0,
                              &g_capStagingPtr[i]),
                  "vkMapMemory(capture staging)")) {
            return false;
        }

        // Created signalled: an unused slot must never look like it is in use.
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!vkOk(vkCreateFence(g_device, &fci, nullptr, &g_capStagingFence[i]),
                  "vkCreateFence(capture staging)")) {
            return false;
        }
    }
    g_capStagingNext = 0;

    // A pool and one reusable command buffer *per slot*, so the record below is
    // a reset rather than an allocation, and so a swapchain rebuild (which
    // destroys g_cmdPool) cannot leave these buffers dangling.
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g_queueFamily;
    if (!vkOk(vkCreateCommandPool(g_device, &cpci, nullptr, &g_capCmdPool),
              "vkCreateCommandPool(capture)")) {
        return false;
    }
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = g_capCmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kCapStagingSlots;
    if (!vkOk(vkAllocateCommandBuffers(g_device, &cbai, g_capCmd),
              "vkAllocateCommandBuffers(capture)")) {
        return false;
    }

    g_capTexW = w;
    g_capTexH = h;
    LOGI("capture texture ready (%dx%d)", w, h);
    return true;
}

/** Makes sure the texture matches the frame size, rebuilding it if it does not. */
bool ensureCaptureTexture(int w, int h) {
    if (g_capImage != VK_NULL_HANDLE && w == g_capTexW && h == g_capTexH) return true;
    destroyCaptureTexture();
    return createCaptureTexture(w, h);
}

/**
 * Pushes one frame to the GPU.
 *
 * Goes through a staging buffer rather than a host-visible image, because the
 * image wants to live in device-local memory for the sampler.
 *
 * The submit is asynchronous. A slot is only waited on when the ring comes back
 * round to it, three uploads later, by which point its fence has normally
 * signalled already — so the render thread no longer stops for the GPU on every
 * frame that carries a new capture. What it used to do is why this is shaped the
 * way it is: one staging buffer meant a vkQueueWaitIdle, and that drains the
 * *whole* queue, including the menu frame submitted a moment earlier. That is a
 * pipeline bubble per upload, and the producer runs at a large fraction of the
 * render rate.
 */
void uploadCaptureFrame(const void* pixels, int frameW, int frameH, int slot) {
    if (!ensureCaptureTexture(frameW, frameH)) return;
    if (g_capStagingPtr[slot] == nullptr || g_capCmd[slot] == VK_NULL_HANDLE) return;

    // The pixels are already in `slot`'s staging memory by the time we get here:
    // the copy is done under the capture lock so the frame thread cannot rewrite
    // the buffer mid-upload. Everything from here on is GPU bookkeeping and must
    // stay outside that lock — vkWaitForFences below can block on the hardware.

    // Only ever waits on the upload that last used *this* slot. If it is still
    // running then the GPU really is behind and blocking is right; the common
    // case is that it finished long ago and this returns at once.
    vkWaitForFences(g_device, 1, &g_capStagingFence[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(g_device, 1, &g_capStagingFence[slot]);

    VkCommandBuffer cmd = g_capCmd[slot];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_capImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    // The whole image is overwritten, so its old contents never need preserving —
    // but the *read* of them does have to finish before this write lands. The
    // first upload comes from UNDEFINED; every one after that comes from the
    // layout the menu has been sampling it in, and says so, which orders the
    // previous frame's fragment reads ahead of this transfer. Under the old
    // vkQueueWaitIdle that ordering came for free; with an asynchronous submit it
    // has to be stated, or the copy may overwrite pixels still being read.
    const bool reused = g_capImageWritten;
    barrier.oldLayout = reused ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = reused ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         reused ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(frameW),
                          static_cast<uint32_t>(frameH), 1};
    vkCmdCopyBufferToImage(cmd, g_capStaging[slot], g_capImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (!vkOk(vkQueueSubmit(g_queue, 1, &submit, g_capStagingFence[slot]),
              "vkQueueSubmit(capture)")) {
        // The fence was reset and nothing will ever signal it, so the ring would
        // block on this slot forever next time round. Tear the capture resources
        // down instead; the next frame rebuilds them from scratch.
        LOGE("capture upload: submit failed — rebuilding capture resources");
        destroyCaptureTexture();
        return;
    }
    g_capImageWritten = true;
}

/** Uploads a newly arrived frame, if there is one. Render thread only. */
/// Returns true when a *new* frame was uploaded this pass — i.e. the preview
/// actually changed and the next frame is worth drawing promptly.
bool updateCaptureTexture() {
    // Nothing samples the preview unless the Capture page is the one on screen,
    // so away from it the upload is skipped entirely — it is a 1.6 MB memcpy plus
    // a transfer on every frame the producer delivers, which is not free. The
    // producer keeps running regardless, so returning to the page shows a live
    // frame at once rather than after a restart.
    // Being on the Capture page is not the same as the preview being visible:
    // the board can be hidden with that page still selected, and a closed menu
    // cost this thread more than the whole rest of the daemon — the crop runs
    // on the producer's every frame and nothing is on screen to show it.
    //
    // Gating on visibility stops the crop and the upload. The producer itself
    // keeps running either way, so reopening the menu gets a live frame at
    // once.
    //
    // Inference is ORed in rather than left to fend for itself. `consuming` is
    // what makes the frame thread cut the crop out at all — with it false the
    // frame is counted and thrown away before any consumer can see it — so a
    // preview-only gate starves the detector exactly when the menu is closed,
    // which is when someone is aiming and the frames are the point. The two
    // reasons are kept separate because they end separately: the preview wants
    // frames while it is on screen, inference wants them while a model is
    // running.
    const bool wantsPreview = (currentSection() == MenuSection::Capture) && !hudHidden();
    const bool wantsInfer   = infer::runtime::wantsFrames();
    // Told to the frame thread rather than kept private: it is the one doing the
    // crop, so it is the only place the work can actually be avoided.
    capture::setConsuming(wantsPreview || wantsInfer);
    // The supervisor that owns the virtual display reads this one to decide
    // whether a mirror is worth holding open, and it has to answer "is anyone
    // reading pixels" — which includes inference. It used to be handed the
    // preview half alone, and that is what made the detector need the Capture
    // page open: with continuous inference on and the page closed, the supervisor
    // saw no reader, stopped the mirror, and inference had no frame source.
    //
    // The switch stays the user's authority and is checked separately
    // (`wanted = switch && reader`, see ShellServerEntry.startCaptureSupervisor),
    // so this does not resurrect the older bug where holding to infer kept a
    // mirror alive on a switch that read "off" — capture::enabled() is still
    // false in that case and the AND still fails.
    capture::setPreviewWanted(wantsPreview || wantsInfer);

    // ── The frame generation, and why it must not be gated on the preview ───
    //
    // `g_frameId` is bumped by the *producer* (capture::pushFrame) on every frame
    // it delivers, and it is the only thing `copyLatestInto` compares to answer
    // "is this newer". So this gate does not control the counter — what it
    // controls is whether the producer is running at all: `capture::previewWanted`
    // is what the shell's capture supervisor reads to decide whether a virtual
    // display is worth holding open (see ShellServerEntry.startCaptureSupervisor,
    // `wanted = switch && preview`).
    //
    // While this function returned early on `!wantsPreview`, that OR was missing
    // from the *supervisor*'s view too: with inference running and the Capture
    // page closed, previewWanted stayed false, the supervisor stopped the mirror,
    // and the detector had no frame source at all. Opening the Capture page was
    // what started the producer — hence "inference only works with that page
    // open", which looked like the page was doing the inferring.
    //
    // The two flags stay separate in meaning — inference must not be able to hold
    // a mirror open behind a switch that reads "off" (capture.h) — but "capture
    // is on and something is reading" has to count inference as a reader. The
    // switch is what the user controls; the *reader* half is what this function
    // reports, and inference is a reader.
    if (!wantsPreview && !wantsInfer) return false;

    // Preview-specific from here down: staging slot, texture, upload. Inference
    // needs none of it — it takes its own copy of the pixels in pump() — so a
    // frame nobody is previewing leaves this function without touching them.
    if (!wantsPreview) return false;

    int frameW = 0, frameH = 0;
    if (!capture::peekSize(frameW, frameH)) return false;
    if (!ensureCaptureTexture(frameW, frameH)) return false;

    const int slot = g_capStagingNext;
    if (g_capStagingPtr[slot] == nullptr) return false;
    const size_t bytes = static_cast<size_t>(frameW) * static_cast<size_t>(frameH) * 4u;
    if (bytes > g_capStagingLen) return false;  // cannot happen — ensure() sized it

    // The frame lands straight in staging memory, under the capture lock, so no
    // second copy is needed and the producer cannot overwrite it halfway through.
    if (!capture::copyLatestInto(g_capStagingPtr[slot], g_capStagingLen,
                                 frameW, frameH, g_capSeenId)) return false;
    g_capStagingNext = (g_capStagingNext + 1) % kCapStagingSlots;

    uploadCaptureFrame(g_capStagingPtr[slot], frameW, frameH, slot);
    // Visible in logcat even though the menu itself is screenshot-invisible, so
    // "the preview is blank" can be split into "no frames" vs "frames but not
    // drawn" without eye-balling the device.
    static uint64_t uploaded = 0;
    if (++uploaded == 1 || uploaded % 60 == 0) {
        LOGI("preview upload #%llu (%dx%d tex=%dx%d)",
             (unsigned long long)uploaded, frameW, frameH,
             g_capTexW, g_capTexH);
    }
    return true;
}

bool recreateSwapchain(const char* reason) {
    int width = ANativeWindow_getWidth(g_window);
    int height = ANativeWindow_getHeight(g_window);
    if (width <= 0 || height <= 0) return false;

    vkDeviceWaitIdle(g_device);
    destroySwapchainDependent();

    if (!createSwapchain(width, height)) return false;
    if (!createImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_queueFamily;
    if (!vkOk(vkCreateCommandPool(g_device, &pci, nullptr, &g_cmdPool), "vkCreateCommandPool")) {
        return false;
    }
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;

#ifdef HAS_IMGUI
    // The pipeline bakes in the render pass, so rebuild it for the new one.
    ImGui_ImplVulkan_SetMinImageCount((uint32_t)g_images.size());
    g_pipelineInfo.RenderPass = g_renderPass;
    g_pipelineInfo.Subpass = 0;
    g_pipelineInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_CreateMainPipeline(&g_pipelineInfo);
#endif
    LOGI("swapchain recreated (%dx%d) reason=%s #%d", width, height, reason,
         g_recreateCount.fetch_add(1) + 1);
    return true;
}

// ── Frame ────────────────────────────────────────────────────────────────────

/** Returns false when the frame could not be presented and the loop should slow down. */
bool drawFrame() {
    vkWaitForFences(g_device, 1, &g_inFlight[g_frame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX,
                                             g_imageAvailable[g_frame], VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        return recreateSwapchain("acquire-out-of-date");
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR failed (VkResult=%d)", (int)acquire);
        return false;
    }
    vkResetFences(g_device, 1, &g_inFlight[g_frame]);

    VkCommandBuffer cmd = g_cmdBuffers[g_frame];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &begin);

    // Transparent clear so anything ImGui does not paint stays see-through.
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_renderPass;
    rp.framebuffer = g_framebuffers[imageIndex];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = g_extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
#ifdef HAS_IMGUI
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
#endif
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSemaphore waitSem = g_imageAvailable[g_frame];
    VkSemaphore signalSem = g_renderFinished[imageIndex];
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &waitSem;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signalSem;
    if (!vkOk(vkQueueSubmit(g_queue, 1, &submit, g_inFlight[g_frame]), "vkQueueSubmit")) return false;

    VkSwapchainKHR swapchain = g_swapchain;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &signalSem;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    VkResult presented = vkQueuePresentKHR(g_queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain("present-out-of-date");
    } else if (presented == VK_SUBOPTIMAL_KHR) {
        // Working, just not optimal — usually a preTransform the compositor is
        // compensating for. Rebuilding here is the per-frame death spiral
        // described at g_recreateCount; log the first few and then go quiet.
        const int seen = g_suboptimalCount.fetch_add(1) + 1;
        if (seen <= 3 || seen % 600 == 0) {
            LOGI("present is suboptimal (#%d) — keeping the swapchain", seen);
        }
    } else if (presented != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR failed (VkResult=%d)", (int)presented);
    }

    g_frame = (g_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    return true;
}

// ── Interactive regions ──────────────────────────────────────────────────────

/**
 * Snapshots the on-screen ImGui windows into `g_regions`.
 *
 * Called on the render thread once the frame's windows exist (after NewFrame
 * and the window building, before Render). Only windows a finger can actually
 * land on are published: a hidden or mouse-transparent window must not swallow
 * touches. When the menu is closed the list is empty and every touch passes
 * through, which is exactly the pre-overlay behaviour.
 */
void updateRegions() {
#ifdef HAS_IMGUI
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return;

    std::vector<int> regions;

    // The menu board is drawn with the draw list, so it is not an ImGui window
    // and the loop below cannot see it. It is nevertheless *the* menu: a finger
    // landing on it must not be mirrored back to the game. Publish it first.
    const HudRect board = hudRect();
    if (board.valid()) {
        regions.push_back(static_cast<int>(board.x));
        regions.push_back(static_cast<int>(board.y));
        regions.push_back(static_cast<int>(board.w));
        regions.push_back(static_cast<int>(board.h));
    }

    // The float button is drawn by drawHud() (or directly when the HUD is
    // hidden) and so is also invisible to the ImGui-window loop below. It is
    // the only menu affordance that survives the HUD being toggled off, so it
    // is *always* a hit-test surface — publish its rect regardless of board
    // validity.
    const HudRect btn = floatButtonRect();
    if (btn.valid()) {
        regions.push_back(static_cast<int>(btn.x));
        regions.push_back(static_cast<int>(btn.y));
        regions.push_back(static_cast<int>(btn.w));
        regions.push_back(static_cast<int>(btn.h));
    }

    for (ImGuiWindow* window : ctx->Windows) {
        if (!window->WasActive || window->Hidden) continue;
        if (window->Flags & (ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoInputs)) continue;
        if (window->Size.x <= 0.0f || window->Size.y <= 0.0f) continue;

        // Pos/Size cover the whole window, title bar included — that is the
        // area ImGui itself hit-tests for "the pointer is on this window".
        regions.push_back(static_cast<int>(window->Pos.x));
        regions.push_back(static_cast<int>(window->Pos.y));
        regions.push_back(static_cast<int>(window->Size.x));
        regions.push_back(static_cast<int>(window->Size.y));
        if (static_cast<int>(regions.size()) >= kMaxRegions * 4) break;
    }

    std::lock_guard<std::mutex> lock(g_regionMutex);
    g_regions.swap(regions);
#endif
}

// ── Input ────────────────────────────────────────────────────────────────────

/** Moves everything queued since the last frame into ImGui's IO event queue. */
/// 0 means "no preference" — hands the panel back to whatever the rest of the
/// system wants, which is what we want while the board is hidden.
static void voteFrameRate(float fps) {
    if (g_setFrameRate == nullptr || g_window == nullptr) return;
    if (fabsf(fps - g_votedFps) < 0.5f) return;
    if (g_setFrameRate(g_window, fps, 1 /*DEFAULT*/) == 0) {
        LOGI("refresh vote: %.0f -> %.0f fps", (double)g_votedFps, (double)fps);
        g_votedFps = fps;
    }
}

/// Cheap "is a finger waiting?" test, used to cut the idle sleep short. Takes
/// the input lock, so it stays out of the render path.
static bool inputPending() {
    std::lock_guard<std::mutex> lock(g_inputMutex);
    return !g_inputQueue.empty() || g_pendingScroll != 0.0f;
}

/// Returns true when this frame consumed input — the render loop treats that
/// as "something is happening" and stays at the full rate for it.
bool drainInput() {
#ifdef HAS_IMGUI
    std::vector<TouchEvent> events;
    float scroll = 0.0f;
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        events.swap(g_inputQueue);
        scroll = g_pendingScroll;
        g_pendingScroll = 0.0f;
    }
    if (events.empty() && scroll == 0.0f) return false;

    ImGuiIO& io = ImGui::GetIO();
    for (const TouchEvent& e : events) {
        io.AddMousePosEvent(e.x, e.y);
        switch (e.action) {
            case kTouchDown:   io.AddMouseButtonEvent(0, true);  break;
            case kTouchUp:     io.AddMouseButtonEvent(0, false); break;
            case kTouchCancel: io.AddMouseButtonEvent(0, false); break;
            default: break;  // kTouchMove: position only
        }
    }
    if (scroll != 0.0f) io.AddMouseWheelEvent(0.0f, scroll);
    return true;
#else
    return false;
#endif
}

// ── Render thread ────────────────────────────────────────────────────────────

bool initVulkan() {
    bool ok = createInstance() && createSurface() && pickPhysicalDevice() && createDevice();

    int width = ANativeWindow_getWidth(g_window);
    int height = ANativeWindow_getHeight(g_window);
    if (ok) {
        ok = createSwapchain(width, height) && createImageViews() && createRenderPass() &&
             createFramebuffers();

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = g_queueFamily;
        if (ok && !vkOk(vkCreateCommandPool(g_device, &pci, nullptr, &g_cmdPool),
                        "vkCreateCommandPool")) {
            ok = false;
        }
        if (ok) ok = createCommandBuffers() && createSyncObjects();
    }
    return ok;
}

bool initImGui() {
#ifdef HAS_IMGUI
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    // The defaults are desktop-sized; on a phone panel every padding and the
    // base font size have to come up to physical (dp) dimensions. TouchExtra-
    // Padding is applied after the scaling so it adds a fixed touch slack on
    // top rather than being scaled from zero.
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(uiScale());
    style.TouchExtraPadding = ImVec2(2.0f, 2.0f);
    ImGui_ImplAndroid_Init(g_window);

    // Register the embedded fonts on the atlas *before* the Vulkan backend is
    // initialised, so the very first frame already has the correct texture.
    loadFonts(io);

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = kApiVersion;
    info.Instance = g_instance;
    info.PhysicalDevice = g_physDevice;
    info.Device = g_device;
    info.QueueFamily = g_queueFamily;
    info.Queue = g_queue;
    info.DescriptorPool = VK_NULL_HANDLE;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    info.MinImageCount = (uint32_t)g_images.size();
    info.ImageCount = (uint32_t)g_images.size();
    info.PipelineCache = VK_NULL_HANDLE;
    info.PipelineInfoMain.RenderPass = g_renderPass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.Allocator = nullptr;
    info.CheckVkResultFn = checkVkResult;
    g_pipelineInfo = info.PipelineInfoMain;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LOGE("ImGui_ImplVulkan_Init failed");
        return false;
    }
    return true;
#else
    LOGE("built without ImGui (HAS_IMGUI=0)");
    return false;
#endif
}

void shutdownImGui() {
#ifdef HAS_IMGUI
    if (g_device != VK_NULL_HANDLE) vkDeviceWaitIdle(g_device);
    // Before the backend goes: the texture's descriptor belongs to its pool.
    destroyCaptureTexture();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();
#endif
}

void destroyVulkan() {
    if (g_device != VK_NULL_HANDLE) vkDeviceWaitIdle(g_device);
    destroySwapchainDependent();

    if (g_device != VK_NULL_HANDLE) {
        vkDestroyDevice(g_device, nullptr);
        g_device = VK_NULL_HANDLE;
    }
    if (g_vkSurface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(g_instance, g_vkSurface, nullptr);
        g_vkSurface = VK_NULL_HANDLE;
    }
    if (g_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
    }
    g_physDevice = VK_NULL_HANDLE;
    g_queue = VK_NULL_HANDLE;
    g_frame = 0;
}

void* renderThread(void*) {
    // The gap this closes: uiStart() returns the instant pthread_create does,
    // so from Kotlin's side "LAYER renderer started" means "the thread exists",
    // not "the menu is drawing". Everything that can actually be slow — Vulkan
    // device + swapchain creation, the embedded font atlases, the first
    // descriptor pool — happens here, after the JNI call has already answered.
    //
    // So the phases are timed and logged individually. A capture that shows
    // LAYER renderer started with no `render: first frame` after it is a
    // capture of a render thread stuck in initVulkan, and the last phase line
    // names which call it is inside.
    const auto tInit0 = std::chrono::steady_clock::now();
    const bool vkOk = initVulkan();
    LOGI("render: initVulkan %s in %.1fms", vkOk ? "ok" : "FAILED", msSince(tInit0));
    const auto tImGui0 = std::chrono::steady_clock::now();
    const bool imguiOk = vkOk && initImGui();
    LOGI("render: initImGui %s in %.1fms (total %.1fms)",
         imguiOk ? "ok" : "FAILED", msSince(tImGui0), msSince(tInit0));

    bool ok = vkOk && imguiOk;
    if (!ok) {
        LOGE("Vulkan/ImGui initialisation failed, render thread exiting");
        // Clean up whatever got created before the failure.
        if (g_instance != VK_NULL_HANDLE) {
#ifdef HAS_IMGUI
            if (ImGui::GetCurrentContext()) shutdownImGui();
#endif
            destroyVulkan();
        }
        g_running = false;
        return nullptr;
    }

    // ── Render-rate probe ────────────────────────────────────────────────────
    // The FPS readout lives on the board, and the board is deliberately excluded
    // from screenshots, so the one number worth having during a bring-up cannot
    // be read by looking. The same figure goes to logcat every couple of seconds
    // instead: `fps` is the real loop rate, `work` the worst frame's own cost
    // (sleep excluded), and the counters expose a swapchain that keeps being
    // rebuilt — the signature of the portrait slowdown.
    auto statsAt = std::chrono::steady_clock::now();
    int  statsFrames = 0;
    double statsWorstMs = 0.0;

    // Touch the wall clock once before the loop so the first frame is not
    // treated as having been idle since the epoch.
    int64_t lastActivityUs = nowUs();
    // The rate the loop is currently pacing to, for the stats line below.
    int     pacedFps = kTargetFps;
    // When the next frame is due, as an absolute deadline. Pacing off the
    // previous frame's start rather than off "the period minus what this frame
    // cost" means the work can never be slept off twice or skipped: jitter does
    // not accumulate into drift, and — the reason it matters here — nothing
    // that happens inside a frame can pull the next one forward.
    int64_t nextFrameDueUs = nowUs();
    // Pending refresh-rate vote and when it started holding, for kVoteHoldUs.
    float   pendingVoteFps  = -1.0f;
    int64_t heldVoteSinceUs = nowUs();
    // Highest rate paced during the current stats window. A touch only holds
    // the full rate for kActiveHoldUs, which is a fraction of a stats window,
    // so without this the line averages it away and the throttle looks broken
    // when it is in fact working.
    int     pacedFpsPeak = pacedFps;

    while (g_running) {
        const auto frameStart = std::chrono::steady_clock::now();
        const int64_t frameStartUs = nowUs();
        bool touched   = false;
        bool animating = false;
#ifdef HAS_IMGUI
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        touched = drainInput();
        ImGui::NewFrame();
        // Pull in whatever the capture side produced since the last frame —
        // before drawHud(), so a fresh frame and the menu showing it land in
        // the same frame rather than the preview trailing by one.
        updateCaptureTexture();
        // Then hand the newest frame to the detector. Here rather than on a
        // timer of its own because this is already the once-per-frame tick, and
        // it is where the capture gate above has just been opened for inference
        // — pumping before that would race the very flag that lets frames
        // through. Non-blocking either way: the worker drops whatever it cannot
        // keep up with.
        infer::runtime::pump();
        // Guarded call, not ShowDemoWindow(&g_showDemo): this vendored copy of
        // imgui_demo.cpp is missing upstream's `if (p_open && !*p_open) return;`
        // early-out, so a false *p_open only removes the close button — the
        // window is still built and drawn. The guard keeps the toggle working.
        if (g_showDemo) ImGui::ShowDemoWindow(&g_showDemo);
        // The menu board, laid out from the current surface size and appended
        // to the foreground draw list (drawn over everything else).
        drawHud();
        // Read *after* drawHud(): that is what advances the ease, so an
        // animation that finished this frame is already settled here.
        animating = !hudSettled();
        // The board and any ImGui windows exist now → publish their rectangles
        // to the shell daemon, so it stops mirroring fingers that land on the
        // menu.
        updateRegions();
        ImGui::Render();
        drawFrame();
        // The end of the bring-up, and the line to look for. `drawFrame()`
        // having returned means the swapchain acquired, the command buffers
        // recorded and the present was queued — i.e. there is something on the
        // layer. Anything before this is "the menu is not up yet"; this line is
        // "the menu is up".
        if (!g_firstFrameLogged) {
            g_firstFrameLogged = true;
            LOGI("render: first frame presented %.1fms after the render thread started",
                 msSince(tInit0));
        }
#endif
        // ── Pick the rate for this period ───────────────────────────────────
        // A frame that changed nothing does not need redrawing, and a board
        // that is not on screen needs nothing at all. Only a touch or a
        // running ease earns the refresh rate; everything else coasts.
        if (touched || animating) lastActivityUs = frameStartUs;
        const int64_t idleFor = frameStartUs - lastActivityUs;

        const bool hidden = hudHidden();
        // The detector counts as "wanted" while the finger is down even if
        // `wantsFrames()` is still false — see holdToInferHeld(). On a cold
        // start arm() is deferred until the compile lands, so the runtime flag
        // stays false for the whole compile; without this OR the loop would sit
        // in the dormant tier at 2 fps and then feed the detector at 2 fps for
        // as long as the finger was held. Checked before `hidden` for the same
        // reason wantsFrames() is: a hidden menu with a finger down is a person
        // aiming, which is exactly the case the dormant tier must not catch.
        const bool inferring = infer::runtime::wantsFrames() ||
                               sections::holdToInferHeld();
        // Aiming outranks the detector tier, and the reason is arithmetic rather
        // than preference. The finger is advanced once per render frame, so a
        // 720 px/s target moves it 12 px per step at 60 fps and 6 px at 120 fps:
        // the 12 px staircase is what "不丝滑" looks like. Worse, the control
        // loop's delay is counted in FRAMES, so halving the frame period halves
        // the delay in milliseconds and doubles the phase margin — which is what
        // makes a tighter lock (higher Kp) affordable instead of oscillatory.
        // The tracker is fed from this same pass, so at 60 fps it was being shown
        // every other detection and half a 120 fps detector's output was dropped
        // on the floor. This overlap with `inferring` is deliberate: the detector
        // wants 60, the aim wants the native rate, and the native rate is the
        // safe superset. Paid only while a target is engaged — the moment the aim
        // lets go, the tier falls back on its own.
        const bool aiming = sections::aimIsDriving();
        int64_t periodUs;
        if (idleFor < kActiveHoldUs) {
            periodUs = kFramePeriodUs;
            pacedFps = kTargetFps;
        } else if (aiming) {
            periodUs = kFramePeriodUs;
            pacedFps = kTargetFps;
        } else if (inferring) {
            // Checked before `hidden`, not after: the case that matters most is a
            // hidden menu with a detector running, which is a person aiming. The
            // dormant tier would starve it to 2 frames a second and the only
            // visible symptom would be a detector that "does not work".
            periodUs = kInferencePeriodUs;
            pacedFps = kInferenceFps;
        } else if (hidden) {
            periodUs = kDormantPeriodUs;
            pacedFps = kDormantFps;
        } else if (capture::consuming() && capture::frameRate() > 0) {
            // Live preview: smooth enough to watch, a quarter of the redraws.
            periodUs = kPreviewPeriodUs;
            pacedFps = kPreviewFps;
        } else {
            periodUs = kIdlePeriodUs;
            pacedFps = kIdleFps;
        }

        // ── Follow with the panel vote ──────────────────────────────────────
        // The fully-hidden case hands the refresh rate back — but NOT while the
        // aim is driving a finger, and that exception is load-bearing. The loop's
        // delay, in SECONDS, is what sets the largest stable kp: halving the
        // panel rate doubles that delay and halves the phase margin, so a kp the
        // user tuned while the menu was open starts ringing the moment they
        // dismiss it and actually play. Measured in scripts/aim_loop_sim.py: at
        // alpha = 1.0 the first gain that rings goes from 0.25 at L = 6 steps to
        // 0.10 at L = 9, and L is measured in frames. So aiming keeps the vote.
        //
        // While the board is up there is usually a game on screen behind it, and
        // this layer does not get to throttle someone else's frame rate — a panel
        // vote is a device-wide decision, not a per-layer one. Raising is
        // immediate; lowering waits out kVoteHoldUs so closing and reopening the
        // menu quickly does not strobe the display.
        {
            const float wantVote = (hidden && !aiming)
                                       ? 0.0f
                                       : static_cast<float>(kTargetFps);
            if (wantVote > g_votedFps) {
                voteFrameRate(wantVote);
                heldVoteSinceUs = frameStartUs;
            } else if (wantVote < g_votedFps) {
                if (wantVote != pendingVoteFps) {
                    pendingVoteFps  = wantVote;
                    heldVoteSinceUs = frameStartUs;
                } else if (frameStartUs - heldVoteSinceUs > kVoteHoldUs) {
                    voteFrameRate(wantVote);
                }
            }
        }

        // Own cost of this frame, sleep excluded — reported, not paced with.
        const auto spentUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - frameStart)
                                 .count();
        // MAILBOX present does not block, so pace the loop ourselves — without
        // this the thread spins and burns CPU clearing a full-screen image.
        //
        // The wait is to an absolute deadline, not "the period minus what this
        // frame cost": that keeps render jitter out of the long-run rate and,
        // more to the point, keeps the rate out of reach of anything that
        // happens during a frame.
        const int64_t dueUs = frameStartUs + periodUs;
        // A frame that overran by more than a period would otherwise leave the
        // deadline permanently in the past and never sleep again.
        nextFrameDueUs = dueUs;
        const int64_t nowForDueUs = nowUs();
        if (nextFrameDueUs < nowForDueUs - periodUs) nextFrameDueUs = nowForDueUs;

        long remaining = static_cast<long>(nextFrameDueUs - nowForDueUs);
        // Sliced, so a finger landing during a long idle stretch is picked up
        // in kPollUs rather than after the whole period. usleep costs nothing
        // to repeat; it is the render that costs power, and that stays skipped.
        //
        // The early wake is a latency trick for the *slow* tiers only, and it
        // must never apply at the active rate: a held finger keeps new events
        // arriving while the frame is being drawn (the digitizer samples far
        // faster than we draw), so an ungated poll fires on most frames,
        // cancels the whole sleep and lets the loop run as fast as the GPU
        // allows — measured 180-200 fps against a 120 target with a finger
        // down. At the active rate the sleep *is* the rate, not a delay to cut.
        const bool wakeOnInput = periodUs > kFramePeriodUs;
        if (!wakeOnInput) {
            // Nothing can shorten this wait, so it does not need slicing — and
            // sleeping to the absolute deadline in one call is both truer (two
            // usleep slices overshoot by about a millisecond, which is why the
            // active tier used to sit near 110 rather than 120) and cheaper,
            // since it is one wakeup instead of two.
            struct timespec ts;
            ts.tv_sec  = static_cast<time_t>(nextFrameDueUs / 1000000LL);
            ts.tv_nsec = static_cast<long>((nextFrameDueUs % 1000000LL) * 1000LL);
            while (g_running && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                                &ts, nullptr) == EINTR) {
            }
        } else {
            while (remaining > 0 && g_running) {
                if (inputPending()) break;
                const long slice = remaining < kPollUs ? remaining : kPollUs;
                usleep(static_cast<useconds_t>(slice));
                remaining -= static_cast<long>(kPollUs);
            }
        }

        ++statsFrames;
        if (pacedFps > pacedFpsPeak) pacedFpsPeak = pacedFps;
        if ((double)spentUs * 0.001 > statsWorstMs) statsWorstMs = (double)spentUs * 0.001;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - statsAt).count();
        if (elapsed >= 2.0) {
            LOGI("fps=%.1f/%d peak=%d work=%.2fms worst=%.2fms swapchain rebuilds=%d suboptimal=%d",
                 statsFrames / elapsed, pacedFps, pacedFpsPeak, (double)spentUs * 0.001,
                 statsWorstMs, g_recreateCount.load(), g_suboptimalCount.load());
            statsFrames = 0;
            statsWorstMs = 0.0;
            pacedFpsPeak = pacedFps;
            statsAt = now;
        }
    }

    shutdownImGui();
    destroyVulkan();

    LOGI("render thread exited");
    return nullptr;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

void setWindow(ANativeWindow* window) {
    // A running session owns the old window; tear it down before swapping in.
    if (g_running) stop();
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    if (g_window) ANativeWindow_release(g_window);
    g_window = window;
}

bool start() {
    if (g_running) return true;
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    if (!g_window) {
        LOGE("start called before setWindow");
        return false;
    }
    // Saved page parameters apply before the first frame, so the menu opens
    // with the user's last setup instead of the compiled-in defaults.
    config::load();
    g_running = true;
    if (pthread_create(&g_thread, nullptr, renderThread, nullptr) != 0) {
        LOGE("pthread_create failed");
        g_running = false;
        return false;
    }
    LOGI("render thread started");
    return true;
}

void stop() {
    g_running = false;
    if (g_thread) {
        pthread_join(g_thread, nullptr);
        g_thread = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_sessionMutex);
        if (g_window) {
            ANativeWindow_release(g_window);
            g_window = nullptr;
        }
    }
    // Drop anything queued while no renderer was alive.
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_inputQueue.clear();
        g_pendingScroll = 0.0f;
    }
    // No renderer means no windows: report "nothing swallows touches".
    {
        std::lock_guard<std::mutex> lock(g_regionMutex);
        g_regions.clear();
    }
    // Toasts belong to the render session, not to the daemon: leaving them
    // would have a freshly started renderer fade in notices about a compile
    // that finished before the last stop(), with lifetimes already expired.
    aimbotng::ui::notify::clear();
    LOGI("stopped");
}

bool isRunning() { return g_running; }

void setSkipScreenshot(bool on) {
    // Thin pass-through to the JNI bridge — the real work (building and
    // applying a SurfaceControl.Transaction) happens in Kotlin's
    // ShellLayerHost, which is the only place that holds the
    // SurfaceControl handle. See input/skip_screenshot.h.
    aimbotng::input::applySkipScreenshot(on);
}

unsigned long long capturePreviewTexture() {
#ifdef HAS_IMGUI
    // A C-style cast on purpose: VkDescriptorSet is a pointer on 64-bit ABIs and
    // a uint64_t on 32-bit ones, and neither static_cast nor reinterpret_cast
    // covers both.
    return (unsigned long long)g_capTexture;
#else
    return 0;
#endif
}

void windowSize(int* width, int* height) {
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    if (width)  *width  = g_window ? ANativeWindow_getWidth(g_window) : 0;
    if (height) *height = g_window ? ANativeWindow_getHeight(g_window) : 0;
}

int getInteractiveRegions(int* out, int maxRects) {
    if (!out || maxRects <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_regionMutex);
    const int limit = maxRects * 4;
    int n = 0;
    for (int value : g_regions) {
        if (n >= limit) break;
        out[n++] = value;
    }
    return n / 4;
}

void onTouch(int action, float x, float y) {
    std::lock_guard<std::mutex> lock(g_inputMutex);
    // Bound the queue so a stalled renderer cannot grow it without limit.
    if (g_inputQueue.size() > 256) g_inputQueue.erase(g_inputQueue.begin());
    g_inputQueue.push_back(TouchEvent{action, x, y});
}

void onScroll(float dy) {
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_pendingScroll += dy;
}

}  // namespace ui
}  // namespace aimbotng
