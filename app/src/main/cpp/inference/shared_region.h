// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — shared frame memory
//
//  A frame handed from the capture thread to the inference worker used to be
//  copied twice: once by submit() into the mailbox slot, and once by the worker
//  taking it back out so the capture thread could not overwrite the pixels it
//  was reading. At 640x640 RGBA that is 1.6 MB per copy, and the capture side
//  produces at screen rate — so it was ~320 MB/s of pure memcpy inside a
//  pipeline whose whole point is to stay ahead of the screen.
//
//  This replaces both copies with a pair of shared-memory buffers, so the
//  capture thread and the worker look at the same physical pages instead of
//  two private ones. That is the mechanism MediaTek's own LiteRT delegate uses
//  (the module is compiled as MemoryFile_Android.cpp in libQnncpu.so); the
//  three-step ladder below is lifted from its open path, because the reason it
//  has three steps is the reason we need them: ASharedMemory_create is API 26+,
//  ashmem_create_region is the pre-26 spelling, and /dev/ashmem is what is left
//  on a device where libandroid.so exports neither.
//
//  Every step is best-effort. If all three fail, `ok()` is false and the caller
//  falls back to the copying path — a daemon that cannot aim because it could
//  not open a shared buffer would be a strictly worse trade than a daemon that
//  aims and copies.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>
#include <cstdint>

namespace aimbotng {
namespace infer {

/// One mmap()ed shared-memory region, plus the key that names it.
///
/// Move-only: two objects owning the same fd would both close it, and the
/// second munmap() of the same range is a no-op that hides the first mistake.
class SharedRegion {
public:
    SharedRegion() = default;
    ~SharedRegion();

    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;
    SharedRegion(SharedRegion&& other) noexcept;
    SharedRegion& operator=(SharedRegion&& other) noexcept;

    /// Creates a region of `bytes`, mapped read/write and shared.
    ///
    /// `key` is only a label — it shows up in /proc/<pid>/maps and in dumpsys,
    /// which is the difference between "some anonymous 1.6 MB mapping" and
    /// "the inference slot" when this is looked at six months from now. It does
    /// not need to be unique and is not used to find the region again.
    ///
    /// Returns false if the platform refused every mechanism. `bytes` is
    /// rounded up to a page boundary internally; `size()` reports the usable
    /// size that was actually obtained.
    bool create(const char* key, size_t bytes);

    /// Releases the mapping and the fd. Safe to call twice.
    void reset();

    bool   ok()   const { return base_ != nullptr; }
    void*  data() const { return base_; }
    size_t size() const { return size_; }
    int    fd()   const { return fd_; }

    /// Which of the three mechanisms was used, for the log line. Never empty
    /// when ok() is true.
    const char* whence() const { return whence_; }

private:
    void*       base_   = nullptr;
    size_t      size_   = 0;
    int         fd_     = -1;
    const char* whence_ = nullptr;
};

}  // namespace infer
}  // namespace aimbotng
