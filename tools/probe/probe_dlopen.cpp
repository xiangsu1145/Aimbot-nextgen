// probe_dlopen.cpp —— 取屏可行性探针
//
// 独立 aarch64 可执行文件，推到 /data/local/tmp 由 shell 运行。
// 回答两个问题：
//
//   1. linker namespace 放行吗？
//      配置上应该放行 —— /linkerconfig/ld.config.txt 里
//        dir.unrestricted = /data/local/tmp
//      而 [unrestricted] 段的 default namespace 是
//        isolated = false
//        search.paths = /system/${LIB} + /system_ext + /odm + /vendor + /product
//      比 [system] 段（app_process 走的那段）还宽松。所以这一条只是确认。
//
//   2. SELinux 放行吗？
//      未知，只能实测。shell 域能不能 dlopen /system/lib64/libgui.so，
//      以及能不能真的调通 SurfaceFlinger 的 binder。
//
// 刻意不链接任何平台库：libgui 等一律运行时 dlopen，
// 所以编译只需要 NDK，不需要 AOSP 头文件与随包 so。
//
// 三级验证：
//   L1  dlopen 各系统库
//   L2  dlsym 关键符号（用从设备上抓到的精确 mangled 名，避免手写错）
//   L3  真的调用 SurfaceFlinger：getDefault() / getPhysicalDisplayIds()
//       —— 这一步才是"能不能干活"的证据，前两步只证明"能加载"。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void ok(const char* what, const char* detail = nullptr) {
    printf("  [ OK ] %s%s%s\n", what, detail ? "  " : "", detail ? detail : "");
}

void bad(const char* what, const char* detail = nullptr) {
    printf("  [FAIL] %s%s%s\n", what, detail ? "  " : "", detail ? detail : "");
    ++g_fail;
}

std::string selinuxContext() {
    FILE* f = fopen("/proc/self/attr/current", "r");
    if (!f) return "(unreadable)";
    char buf[256] = {0};
    const bool got = fgets(buf, sizeof(buf), f) != nullptr;
    fclose(f);
    if (!got) return "(empty)";
    return std::string(buf, strcspn(buf, "\n"));
}

void* load(const char* lib) {
    dlerror();
    void* h = dlopen(lib, RTLD_NOW);
    if (h) {
        ok(lib);
    } else {
        bad(lib, dlerror());
    }
    return h;
}

void* find(void* h, const char* mangled, const char* human) {
    if (!h) {
        printf("  [SKIP] %s\n", human);
        return nullptr;
    }
    void* p = dlsym(h, mangled);
    if (p) {
        ok(human);
    } else {
        bad(human, "符号不存在");
    }
    return p;
}

/// ABI 探针：与 sp<T> 同布局（一个指针），但**对调用而言是非平凡类型**。
///
/// 为什么要这个：`sp<T>` 和 `std::vector` 都有用户定义的析构，属于 Itanium ABI
/// 里 "non-trivial for the purposes of calls" 的返回类型 —— 这类返回值不用寄存器
/// 直接回，而是由 caller 提供一块隐藏的 sret 槽。**AArch64 上这个槽的地址走 x8**，
/// 而不是像 x86-64 那样当第一个参数传（在 x0）。
///
/// 所以不能用 `void (*)(void*)` 去硬套符号：那样编译器会把槽地址放 x0，
/// 而被调方读的是 x8（垃圾值）→ 往野地址写 → SIGSEGV，且崩在库内部看起来像权限问题。
/// 正确做法是让返回类型的声明带上非平凡析构，编译器就会自己生成正确的序列。
struct SpSlot {
    void* ptr = nullptr;
    ~SpSlot() {}
};

}  // namespace

int main() {
    // 崩溃也要看得见输出 —— 全程行缓冲，每行立刻落盘。
    setvbuf(stdout, nullptr, _IOLBF, 0);

    printf("=== Aimbot 取屏探针 ===\n");
    printf("uid=%u  gid=%u\n", (unsigned)getuid(), (unsigned)getgid());
    const std::string ctx = selinuxContext();
    printf("selinux=%s\n", ctx.c_str());
    printf("(可执行文件所在目录决定 linker 段：/data/local/tmp 走 [unrestricted])\n");

    printf("\n-- L1  dlopen --\n");
    void* gui    = load("libgui.so");
    void* ui     = load("libui.so");
    void* binder = load("libbinder.so");
    void* utils  = load("libutils.so");
    void* mndk   = load("libmediandk.so");
    void* nwin   = load("libnativewindow.so");
    void* sf     = load("libstagefright.so");

    printf("\n-- L2  dlsym --\n");
    void* pGetDefault = find(gui,
        "_ZN7android21SurfaceComposerClient10getDefaultEv",
        "SurfaceComposerClient::getDefault");
    void* pCreateVD = find(gui,
        "_ZN7android21SurfaceComposerClient20createVirtualDisplayERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEbS9_f",
        "SurfaceComposerClient::createVirtualDisplay");
    void* pDestroyVD = find(gui,
        "_ZN7android21SurfaceComposerClient21destroyVirtualDisplayERKNS_2spINS_7IBinderEEE",
        "SurfaceComposerClient::destroyVirtualDisplay");
    void* pGetIds = find(gui,
        "_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv",
        "SurfaceComposerClient::getPhysicalDisplayIds");
    void* pGetToken = find(gui,
        "_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenENS_17PhysicalDisplayIdE",
        "SurfaceComposerClient::getPhysicalDisplayToken");
    void* pMirror = find(gui,
        "_ZN7android21SurfaceComposerClient13mirrorDisplayENS_9DisplayIdE",
        "SurfaceComposerClient::mirrorDisplay");

    void* pSetDpySurface = find(gui,
        "_ZN7android21SurfaceComposerClient11Transaction17setDisplaySurfaceERKNS_2spINS_7IBinderEEERKNS2_INS_22IGraphicBufferProducerEEE",
        "Transaction::setDisplaySurface");
    void* pSetLayerStack = find(gui,
        "_ZN7android21SurfaceComposerClient11Transaction20setDisplayLayerStackERKNS_2spINS_7IBinderEEENS_2ui10LayerStackE",
        "Transaction::setDisplayLayerStack");
    void* pSetProjection = find(gui,
        "_ZN7android21SurfaceComposerClient11Transaction20setDisplayProjectionERKNS_2spINS_7IBinderEEENS_2ui8RotationERKNS_4RectESB_",
        "Transaction::setDisplayProjection");
    void* pSetDpySize = find(gui,
        "_ZN7android21SurfaceComposerClient11Transaction14setDisplaySizeERKNS_2spINS_7IBinderEEEjj",
        "Transaction::setDisplaySize");
    void* pApply = find(gui,
        "_ZN7android21SurfaceComposerClient11Transaction5applyEbb",
        "Transaction::apply");

    find(ui, "_ZN7android13GraphicBuffer4lockEjPPvPiS3_",
        "GraphicBuffer::lock");
    find(ui, "_ZN7android13GraphicBuffer6unlockEv",
        "GraphicBuffer::unlock");
    find(ui, "_ZN7android13GraphicBuffer17toAHardwareBufferEv",
        "GraphicBuffer::toAHardwareBuffer");

    find(mndk, "AImageReader_newWithUsage", "AImageReader_newWithUsage");
    find(mndk, "AImageReader_getWindow",    "AImageReader_getWindow");
    find(mndk, "AImage_getPlaneData",       "AImage_getPlaneData");
    find(mndk, "AImage_getHardwareBuffer",  "AImage_getHardwareBuffer");

    printf("\n-- L3  真正调用 SurfaceFlinger --\n");
    // 返回类型必须声明成真正的非平凡类型，让编译器自己选对 sret 的传参寄存器。
    // 详见上面 SpSlot 的注释 —— 这一条写错会得到一个看起来像"权限被拒"的崩溃。
    if (pGetDefault) {
        using GetDefaultFn = SpSlot (*)();
        const SpSlot client = reinterpret_cast<GetDefaultFn>(pGetDefault)();
        if (client.ptr) {
            ok("getDefault() 拿到 SurfaceComposerClient");
        } else {
            bad("getDefault() 返回空（binder 被拒或 SF 不可达）");
        }
    }

    uint64_t mainDisplayId = 0;
    if (pGetIds) {
        using GetIdsFn = std::vector<uint64_t> (*)();
        const std::vector<uint64_t> ids = reinterpret_cast<GetIdsFn>(pGetIds)();
        printf("  [ OK ] getPhysicalDisplayIds() -> %zu 个显示\n", ids.size());
        for (uint64_t id : ids) {
            printf("         display id = %llu\n", (unsigned long long)id);
        }
        if (!ids.empty()) mainDisplayId = ids.front();
    }

    printf("\n-- L4  虚拟显示（纯 C++ 建屏，无 JNI）--\n");
    (void)pSetDpySurface; (void)pSetLayerStack; (void)pSetProjection;
    (void)pSetDpySize;    (void)pApply;        (void)pMirror;

    if (pGetToken && mainDisplayId) {
        // getPhysicalDisplayToken(PhysicalDisplayId) -> sp<IBinder>
        // 入参是 uint64，返回 sp —— 同样按 SpSlot 声明让编译器走对 sret。
        using GetTokenFn = SpSlot (*)(uint64_t);
        const SpSlot token = reinterpret_cast<GetTokenFn>(pGetToken)(mainDisplayId);
        if (token.ptr) {
            ok("getPhysicalDisplayToken() 拿到主屏 token");
        } else {
            bad("getPhysicalDisplayToken() 返回空");
        }
    }

    if (pCreateVD) {
        // createVirtualDisplay(const std::string&, bool, const std::string&, float) -> sp<IBinder>
        // std::string 跨 libc++ 边界是安全的：Android 保证 string/vector 的 ABI 稳定。
        using CreateVdFn = SpSlot (*)(const std::string*, bool, const std::string*, float);
        const std::string name = "aimbot-probe";
        const std::string uniqueId;                 // 空 = 不绑定物理显示
        SpSlot vd = reinterpret_cast<CreateVdFn>(pCreateVD)(&name, /*secure*/false,
                                                            &uniqueId, /*refreshRate*/60.0f);
        if (!vd.ptr) {
            bad("createVirtualDisplay() 返回空 —— 建屏被拒");
        } else {
            ok("createVirtualDisplay() 建出虚拟显示");
            if (pDestroyVD) {
                using DestroyVdFn = void (*)(const void*);   // 参数是 const sp<IBinder>&
                reinterpret_cast<DestroyVdFn>(pDestroyVD)(&vd);
                ok("destroyVirtualDisplay() 已释放");
            } else {
                printf("  [WARN] 无 destroyVirtualDisplay，虚拟显示会残留到重启\n");
            }
        }
    }

    printf("\n=== 失败项: %d ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
