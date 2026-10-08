// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of ncnn_engine.h
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/ncnn_engine.h"

#include "inference/capabilities.h"
#include <ncnn/cpu.h>
#include <ncnn/gpu.h>
#include <ncnn/layer.h>
#include <ncnn/mat.h>
#include <ncnn/net.h>

#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace {

using Clock = std::chrono::steady_clock;

float msSince(Clock::time_point t0) {
    return static_cast<float>(
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
}

bool endsWith(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void resolveModelPaths(const std::string& inputPath,
                       std::string& outParam,
                       std::string& outBin) {
    size_t sep = inputPath.find(';');
    if (sep != std::string::npos) {
        std::string p1 = inputPath.substr(0, sep);
        std::string p2 = inputPath.substr(sep + 1);
        if (endsWith(p1, ".param")) {
            outParam = p1;
            outBin = p2;
        } else if (endsWith(p2, ".param")) {
            outParam = p2;
            outBin = p1;
        } else {
            outParam = p1;
            outBin = p2;
        }
        return;
    }

    if (endsWith(inputPath, ".param")) {
        outParam = inputPath;
        outBin = inputPath.substr(0, inputPath.size() - 6) + ".bin";
    } else if (endsWith(inputPath, ".bin")) {
        outBin = inputPath;
        outParam = inputPath.substr(0, inputPath.size() - 4) + ".param";
    } else {
        outParam = inputPath + ".param";
        outBin = inputPath + ".bin";
    }
}

bool isNcnnInt8Model(const std::string& paramPath, const std::string& binPath) {
    // 1. Scan .param for Quantize / Dequantize or int8 convolution scale terms
    std::ifstream pf(paramPath);
    if (pf.is_open()) {
        std::string line;
        while (std::getline(pf, line)) {
            if (line.rfind("Quantize", 0) == 0 || line.rfind("Dequantize", 0) == 0) {
                return true;
            }
            if (line.find("Convolution") != std::string::npos &&
                (line.find(" 8=1") != std::string::npos || line.find(" 8=2") != std::string::npos)) {
                return true;
            }
        }
    }
    // 2. Check .bin 32-bit tag
    std::ifstream bf(binPath, std::ios::binary);
    if (bf.is_open()) {
        uint32_t tag = 0;
        if (bf.read(reinterpret_cast<char*>(&tag), sizeof(tag))) {
            if (tag == 0x000D4B38) return true;
        }
    }
    return false;
}

}  // namespace

bool validateNcnnModel(const std::string& paramPath,
                       const std::string& binPath,
                       std::string& outError) {
    if (paramPath.empty()) {
        outError = "未指定 NCNN 结构文件 (.param)";
        return false;
    }
    if (binPath.empty()) {
        outError = "未指定 NCNN 权重文件 (.bin)";
        return false;
    }

    if (access(paramPath.c_str(), R_OK) != 0) {
        outError = "结构文件 (.param) 不存在或无读取权限: " + paramPath;
        return false;
    }

    struct stat stBin{};
    if (stat(binPath.c_str(), &stBin) != 0) {
        outError = "权重文件 (.bin) 不存在或无读取权限: " + binPath;
        return false;
    }
    if (stBin.st_size <= 0) {
        outError = "权重文件 (.bin) 大小为空 (0 字节)";
        return false;
    }

    // Check header of .param
    std::ifstream pf(paramPath.c_str(), std::ios::binary);
    if (!pf.is_open()) {
        outError = "无法打开结构文件: " + paramPath;
        return false;
    }

    char headBuf[32] = {};
    pf.read(headBuf, sizeof(headBuf) - 1);
    std::string head(headBuf, pf.gcount());

    // NCNN magic number: 7767517 (text or binary 0x007685B5)
    bool magicOk = false;
    if (head.find("7767517") == 0) {
        magicOk = true;
    } else if (head.size() >= 4) {
        const uint8_t* u = reinterpret_cast<const uint8_t*>(head.data());
        uint32_t magic = static_cast<uint32_t>(u[0]) |
                        (static_cast<uint32_t>(u[1]) << 8) |
                        (static_cast<uint32_t>(u[2]) << 16) |
                        (static_cast<uint32_t>(u[3]) << 24);
        if (magic == 7767517 || magic == 0x007685B5) {
            magicOk = true;
        }
    }

    if (!magicOk) {
        outError = "所选文件不是合法的 NCNN 模型（结构文件缺少 7767517 标识）";
        return false;
    }

    // Try parsing the network graph using ncnn::Net
    ncnn::Net testNet;
    int retParam = testNet.load_param(paramPath.c_str());
    if (retParam != 0) {
        outError = "NCNN 解析结构文件失败 (错误码 " + std::to_string(retParam) + ")";
        return false;
    }

    return true;
}

struct NcnnEngine::Impl {
    ncnn::Net           net;
    std::atomic<float>  confidence{0.5f};
    PreprocessConfig    pre;
    PostprocessConfig   post;
    std::vector<float>  input;

    int   inW = 640;
    int   inH = 640;
    int   anchors = 0;
    int   channels = 0;
    bool  channelsFirst = false;
    bool  isFp16 = false;
    bool  isInt8 = false;

    std::string inName = "in0";
    std::string outName = "out0";

    std::string inShapeText;
    std::string outShapeText;

#if NCNN_VULKAN
    ncnn::VulkanDevice* vkdev = nullptr;
    ncnn::VkAllocator*  blob_vkallocator = nullptr;
    ncnn::VkAllocator*  staging_vkallocator = nullptr;

    ~Impl() {
        if (vkdev) {
            if (blob_vkallocator) {
                vkdev->reclaim_blob_allocator(blob_vkallocator);
                blob_vkallocator = nullptr;
            }
            if (staging_vkallocator) {
                vkdev->reclaim_staging_allocator(staging_vkallocator);
                staging_vkallocator = nullptr;
            }
        }
    }
#endif
};

NcnnEngine::NcnnEngine(Ep ep) : ep_(ep) {
    if (ep == Ep::Vulkan) {
        activeName_ = "NCNN / GPU (Vulkan)";
    } else {
        activeName_ = "NCNN / CPU";
    }
    status_ = Status::good();
}

NcnnEngine::~NcnnEngine() {
    unload();
}

void NcnnEngine::unload() {
    impl_.reset();
    status_ = Status::bad("not loaded");
    timing_ = Timing{};
}

bool NcnnEngine::ready() const {
    return impl_ != nullptr && impl_->anchors > 0;
}

const char* NcnnEngine::activeName() const {
    return activeName_.c_str();
}

void NcnnEngine::setConfidence(float c) {
    if (impl_ != nullptr) {
        impl_->confidence.store(std::clamp(c, 0.01f, 0.99f), std::memory_order_relaxed);
    }
}

std::string NcnnEngine::inputDtypeName() const {
    if (impl_ && impl_->isInt8) return "int8";
    if (impl_ && impl_->isFp16) return "fp16";
    return (ep_ == Ep::Vulkan) ? "fp16" : "fp32";
}

std::string NcnnEngine::describe() const {
    if (!ready()) return "";
    char buf[128];
    snprintf(buf, sizeof(buf), "%s, input %dx%d, %d classes, %d anchors",
             activeName_.c_str(), impl_->inW, impl_->inH,
             impl_->post.numClasses, impl_->anchors);
    return buf;
}

Status NcnnEngine::load(const ModelSpec& spec) {
    unload();
    const auto t0 = Clock::now();

    if (spec.path.empty()) {
        return status_ = Status::bad("no model path specified");
    }

    std::string paramPath;
    std::string binPath;
    resolveModelPaths(spec.path, paramPath, binPath);

    std::string valErr;
    if (!validateNcnnModel(paramPath, binPath, valErr)) {
        return status_ = Status::bad(valErr);
    }

    auto impl = std::make_unique<Impl>();

    // Detect if .bin model weights are FP16 or INT8
    impl->isInt8 = isNcnnInt8Model(paramPath, binPath);
    if (!impl->isInt8) {
        std::ifstream bf(binPath, std::ios::binary);
        if (bf.is_open()) {
            uint32_t tag = 0;
            if (bf.read(reinterpret_cast<char*>(&tag), sizeof(tag))) {
                if (tag == 0x01306B47) {
                    impl->isFp16 = true;
                }
            }
        }
    }

    // Register custom layers for compatibility
    impl->net.register_custom_layer(
        "QuantizeLinear",
        [](void*) -> ncnn::Layer* { return ncnn::create_layer("Quantize"); },
        [](ncnn::Layer* layer, void*) { delete layer; });
    impl->net.register_custom_layer(
        "DequantizeLinear",
        [](void*) -> ncnn::Layer* { return ncnn::create_layer("Dequantize"); },
        [](ncnn::Layer* layer, void*) { delete layer; });
    impl->net.register_custom_layer(
        "pnnx.Expression",
        [](void*) -> ncnn::Layer* { return ncnn::create_layer("MemoryData"); },
        [](ncnn::Layer* layer, void*) { delete layer; });

    // Enable high performance layer optimizations
    impl->net.opt.lightmode = true;
    impl->net.opt.use_packing_layout = true;
    impl->net.opt.use_winograd_convolution = true;
    impl->net.opt.use_sgemm_convolution = true;

    // Hardware accelerator configuration
    if (ep_ == Ep::Vulkan) {
        if (impl->isInt8) {
            return status_ = Status::bad("NCNN Vulkan (GPU) 不支持 INT8 量化模型（会触发驱动闪退），请在模型设置中将推理后端切换为 NCNN / CPU");
        }
#if NCNN_VULKAN
        int gpuCount = ncnn::get_gpu_count();
        if (gpuCount <= 0) {
            return status_ = Status::bad("NCNN Vulkan GPU unavailable: no Vulkan GPU found");
        }
        int gpuIndex = ncnn::get_default_gpu_index();
        impl->vkdev = ncnn::get_gpu_device(gpuIndex);
        if (impl->vkdev == nullptr) {
            return status_ = Status::bad("NCNN failed to acquire Vulkan GPU device");
        }
        impl->net.set_vulkan_device(impl->vkdev);
        impl->net.opt.use_vulkan_compute = true;

        const ncnn::GpuInfo& ginfo = ncnn::get_gpu_info(gpuIndex);
        impl->net.opt.use_fp16_packed = ginfo.support_fp16_packed();
        impl->net.opt.use_fp16_storage = ginfo.support_fp16_storage();
        impl->net.opt.use_fp16_arithmetic = ginfo.support_fp16_arithmetic();

        // Memory pool allocators to eliminate per-layer VkBuffer allocation overhead
        impl->blob_vkallocator = impl->vkdev->acquire_blob_allocator();
        impl->staging_vkallocator = impl->vkdev->acquire_staging_allocator();
        impl->net.opt.blob_vkallocator = impl->blob_vkallocator;
        impl->net.opt.staging_vkallocator = impl->staging_vkallocator;
        impl->net.opt.workspace_vkallocator = impl->blob_vkallocator;

        activeName_ = "NCNN / GPU (Vulkan)";
#else
        return status_ = Status::bad("NCNN Vulkan is not enabled in this build");
#endif
    } else {
        impl->net.opt.use_vulkan_compute = false;
        const int threads = (spec.cpuThreads > 0) ? spec.cpuThreads : 4;
        impl->net.opt.num_threads = threads;
        ncnn::set_omp_num_threads(threads);
        // Bind to big/performance cores (e.g. Snapdragon Cortex-X / Cortex-A7xx)
        ncnn::set_cpu_powersave(0);
        if (impl->isInt8) {
            // For INT8 models, disable FP16 storage and arithmetic to avoid redundant
            // fp16 <-> fp32 <-> int8 double-casting on ARM NEON
            impl->net.opt.use_fp16_packed = false;
            impl->net.opt.use_fp16_storage = false;
            impl->net.opt.use_fp16_arithmetic = false;
            impl->net.opt.use_int8_inference = true;
            impl->net.opt.openmp_blocktime = 20;
            activeName_ = "NCNN / CPU (" + std::to_string(threads) + "T INT8)";
        } else {
            // Enable ARMv8.2-A FP16 vector acceleration on CPU
            impl->net.opt.use_fp16_packed = true;
            impl->net.opt.use_fp16_storage = true;
            impl->net.opt.use_fp16_arithmetic = true;
            impl->net.opt.use_int8_inference = false;
            impl->net.opt.openmp_blocktime = 0;
            activeName_ = "NCNN / CPU (" + std::to_string(threads) + "T FP16)";
        }
    }

    int retParam = impl->net.load_param(paramPath.c_str());
    if (retParam != 0) {
        return status_ = Status::bad("NCNN load_param failed (" + std::to_string(retParam) + ")");
    }
    int retModel = impl->net.load_model(binPath.c_str());
    if (retModel != 0) {
        return status_ = Status::bad("NCNN load_model failed (" + std::to_string(retModel) + ")");
    }

    // Determine input and output blob names
    const std::vector<const char*>& inNames = impl->net.input_names();
    if (!inNames.empty() && inNames[0] != nullptr) {
        impl->inName = inNames[0];
    } else {
        impl->inName = "in0";
    }

    const std::vector<const char*>& outNames = impl->net.output_names();
    if (!outNames.empty() && outNames[0] != nullptr) {
        impl->outName = outNames[0];
    } else {
        impl->outName = "out0";
    }

    const int edge = (spec.inputSize > 0) ? spec.inputSize : 640;
    impl->inW = edge;
    impl->inH = edge;

    impl->pre.targetW = edge;
    impl->pre.targetH = edge;
    impl->pre.keepAspect = true;
    impl->pre.padValue = 114.0f / 255.0f;
    impl->pre.bgr = false;
    impl->pre.scale = 1.0f / 255.0f;
    impl->pre.nchw = true;

    // Run test forward on a blank frame to inspect output dimensions & geometry
    try {
        ncnn::Extractor ex = impl->net.create_extractor();
        ncnn::Mat dummyIn(edge, edge, 3);
        dummyIn.fill(0.0f);
        ex.input(impl->inName.c_str(), dummyIn);
        ncnn::Mat dummyOut;
        ex.extract(impl->outName.c_str(), dummyOut);

        if (!dummyOut.empty()) {
            if (dummyOut.elempack != 1) {
                ncnn::Mat unpacked;
                ncnn::convert_packing(dummyOut, unpacked, 1, impl->net.opt);
                dummyOut = unpacked;
            }
            if (dummyOut.elemsize == 2) {
                ncnn::Mat fp32;
                ncnn::cast_float16_to_float32(dummyOut, fp32, impl->net.opt);
                dummyOut = fp32;
            }
        }

        if (dummyOut.empty()) {
            LOGW("NCNN blank frame probe returned empty output");
            impl->anchors = (edge == 640) ? 8400 : ((edge * edge) / 64);
            impl->channels = 5;
            impl->channelsFirst = false;
        } else if (dummyOut.dims == 2) {
            if (dummyOut.h > dummyOut.w) {
                impl->anchors = dummyOut.h;
                impl->channels = dummyOut.w;
                impl->channelsFirst = false;
            } else {
                impl->anchors = dummyOut.w;
                impl->channels = dummyOut.h;
                impl->channelsFirst = true;
            }
        } else if (dummyOut.dims == 3) {
            impl->anchors = dummyOut.w * dummyOut.h;
            impl->channels = dummyOut.c;
            impl->channelsFirst = true;
        } else {
            impl->anchors = dummyOut.w;
            impl->channels = 1;
            impl->channelsFirst = false;
        }
    } catch (const std::exception& e) {
        LOGW("NCNN shape detection forward threw: %s", e.what());
        impl->anchors = 8400;
        impl->channels = 5;
        impl->channelsFirst = false;
    }

    impl->post.layout = impl->channelsFirst ? HeadLayout::V8ChannelsFirst
                                            : HeadLayout::V8Rows;
    impl->post.numClasses = (impl->channels > 4) ? (impl->channels - 4) : 1;
    impl->post.confidence = spec.confidence;
    impl->post.iou = (spec.iou > 0.0f) ? spec.iou : 0.45f;
    impl->post.boxXyxy = spec.boxXyxy;
    impl->post.inputW = edge;
    impl->post.inputH = edge;
    impl->post.maxDetections = 64;

    const size_t need = preprocessBufferBytes(impl->pre, static_cast<int>(sizeof(float)));
    impl->input.assign(need / sizeof(float), 0.0f);

    impl_ = std::move(impl);

    const float loadMs = msSince(t0);
    LOGI("NCNN loaded: %s + %s, input=%dx%d, anchors=%d, channels=%d (%s, %.1f ms)",
         paramPath.c_str(), binPath.c_str(), impl_->inW, impl_->inH,
         impl_->anchors, impl_->channels, activeName_.c_str(), loadMs);

    status_ = Status::good();
    timing_ = Timing{};
    return status_;
}

std::vector<Box> NcnnEngine::run(const FrameView& frame, const Roi& roi) {
    if (!ready()) {
        status_ = Status::bad("no model loaded");
        return {};
    }
    if (!frame.valid()) {
        status_ = Status::bad("frame is not usable");
        return {};
    }

    Impl& m = *impl_;

    try {
        // ── Preprocess ──
        const auto tPre = Clock::now();
        const LetterboxMap map = letterboxRgba(frame, roi, m.pre, m.input.data());
        timing_.preMs = msSince(tPre);

        // ── Infer ──
        const auto tInfer = Clock::now();
        ncnn::Mat in(m.inW, m.inH, 3, m.input.data());
        ncnn::Extractor ex = m.net.create_extractor();
        ex.input(m.inName.c_str(), in);
        ncnn::Mat out;
        ex.extract(m.outName.c_str(), out);
        timing_.inferMs = msSince(tInfer);

        // ── Postprocess ──
        const auto tPost = Clock::now();
        std::vector<Box> boxes;
        if (!out.empty()) {
            if (out.elempack != 1) {
                ncnn::Mat unpacked;
                ncnn::convert_packing(out, unpacked, 1, m.net.opt);
                out = unpacked;
            }
            if (out.elemsize == 2) {
                ncnn::Mat fp32;
                ncnn::cast_float16_to_float32(out, fp32, m.net.opt);
                out = fp32;
            }

            OutputTensor tensor;
            tensor.data = reinterpret_cast<const float*>(out.data);
            tensor.rows = m.anchors;
            tensor.cols = m.channels;
            tensor.channelsFirst = m.channelsFirst;

            m.post.confidence = m.confidence.load(std::memory_order_relaxed);
            boxes = decode(tensor, m.post, map, frame);
        }
        timing_.postMs = msSince(tPost);
        status_ = Status::good();
        return boxes;
    } catch (const std::exception& e) {
        status_ = Status::bad(std::string("NCNN inference error: ") + e.what());
        return {};
    }
}

}  // namespace infer
}  // namespace aimbotng
