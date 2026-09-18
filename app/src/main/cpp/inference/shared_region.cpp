// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — shared_region.h
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/shared_region.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace infer {
namespace {

// ── The three mechanisms, resolved lazily and once ──────────────────────────
//
// dlsym() rather than a link-time dependency on purpose. A direct reference to
// ASharedMemory_create makes the whole .so fail to load on a device that does
// not export it, which turns "no zero-copy" into "no daemon". MediaTek's own
// delegate resolves it this way for the same reason.
using CreateFn = int (*)(const char*, size_t);

/// The ashmem ioctls, spelled out because <linux/ashmem.h> is not in the NDK.
/// Values taken from the kernel header: _IOW('a', 0, char*) and
/// _IOW('a', 3, size_t) on 64-bit.
constexpr unsigned long kAshmemSetName = 0x41007701u;
constexpr unsigned long kAshmemSetSize = 0x40087703u;

void* g_libandroid     = nullptr;
CreateFn g_shmCreate   = nullptr;   // ASharedMemory_create
CreateFn g_ashmemCreate = nullptr;  // ashmem_create_region
bool     g_resolved    = false;

void resolveOnce() {
    if (g_resolved) return;
    g_resolved = true;

    // libandroid.so is what exports both, and it is always present; the symbols
    // are what may or may not be.
    g_libandroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (g_libandroid) {
        g_shmCreate    = reinterpret_cast<CreateFn>(dlsym(g_libandroid, "ASharedMemory_create"));
        g_ashmemCreate = reinterpret_cast<CreateFn>(dlsym(g_libandroid, "ashmem_create_region"));
    }
    LOGI("shared_region: libandroid=%p ASharedMemory_create=%p ashmem_create_region=%p",
         g_libandroid, reinterpret_cast<void*>(g_shmCreate),
         reinterpret_cast<void*>(g_ashmemCreate));
}

/// Opens an ashmem fd straight from the device node. The last resort: present
/// since Android 1.0 and still there on everything we care about, but it needs
/// two ioctls to become usable and the kernel may refuse either.
int openAshmemNode(const char* key, size_t bytes) {
    const int fd = ::open("/dev/ashmem", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOGW("shared_region: open(/dev/ashmem) failed: %s", strerror(errno));
        return -1;
    }
    // Setting the name is cosmetic, so a rejection is not fatal — the size is
    // the one that has to take.
    if (ioctl(fd, kAshmemSetName, key) != 0) {
        LOGW("shared_region: ASHMEM_SET_NAME(%s) failed: %s", key, strerror(errno));
    }
    if (ioctl(fd, kAshmemSetSize, bytes) != 0) {
        LOGW("shared_region: ASHMEM_SET_SIZE(%zu) failed: %s", bytes, strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Page-rounded, because mmap() of a non-page multiple and the size the kernel
/// later reports would disagree, and that disagreement is exactly the kind of
/// thing that turns into a one-byte-past-the-end read under ASan.
size_t pageRound(size_t bytes) {
    const long page = sysconf(_SC_PAGESIZE);
    const size_t unit = page > 0 ? static_cast<size_t>(page) : 4096u;
    return (bytes + unit - 1u) / unit * unit;
}

}  // namespace

SharedRegion::~SharedRegion() { reset(); }

SharedRegion::SharedRegion(SharedRegion&& other) noexcept
    : base_(other.base_), size_(other.size_), fd_(other.fd_), whence_(other.whence_) {
    other.base_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
    other.whence_ = nullptr;
}

SharedRegion& SharedRegion::operator=(SharedRegion&& other) noexcept {
    if (this != &other) {
        reset();
        base_ = other.base_;
        size_ = other.size_;
        fd_ = other.fd_;
        whence_ = other.whence_;
        other.base_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
        other.whence_ = nullptr;
    }
    return *this;
}

bool SharedRegion::create(const char* key, size_t bytes) {
    reset();
    if (bytes == 0) return false;

    resolveOnce();

    const size_t want = pageRound(bytes);

    // ── Step 1: ASharedMemory_create (API 26+) ──────────────────────────────
    int fd = -1;
    const char* whence = nullptr;
    if (g_shmCreate) {
        fd = g_shmCreate(key, want);
        if (fd >= 0) whence = "ASharedMemory_create";
    }

    // ── Step 2: ashmem_create_region (the pre-26 spelling) ──────────────────
    if (fd < 0 && g_ashmemCreate) {
        fd = g_ashmemCreate(key, want);
        if (fd >= 0) whence = "ashmem_create_region";
    }

    // ── Step 3: the device node, two ioctls by hand ─────────────────────────
    if (fd < 0) {
        fd = openAshmemNode(key, want);
        if (fd >= 0) whence = "/dev/ashmem";
    }

    if (fd < 0) {
        LOGW("shared_region(%s): no mechanism produced an fd for %zu bytes", key, want);
        return false;
    }

    // MAP_SHARED is the whole point: MAP_PRIVATE here would give each side its
    // own copy-on-write pages and the copies would silently come back.
    void* p = mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        LOGW("shared_region(%s): mmap(%zu) via %s failed: %s", key, want, whence,
             strerror(errno));
        ::close(fd);
        return false;
    }

    base_ = p;
    size_ = want;
    fd_ = fd;
    whence_ = whence;
    LOGI("shared_region(%s): %zu bytes at %p via %s (fd %d)", key, want, p, whence, fd);
    return true;
}

void SharedRegion::reset() {
    if (base_) {
        munmap(base_, size_);
        base_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
    whence_ = nullptr;
}

}  // namespace infer
}  // namespace aimbotng
