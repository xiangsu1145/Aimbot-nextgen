// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::capture — the hand-off between the frame producer and the renderer
//
//  The frames themselves are produced on the Java side: the shell daemon has no
//  Context, so a virtual display is built through the framework's own
//  `DisplayManager` (see ScreenCapture.kt) and its output lands in an
//  `ImageReader`. That leaves one thing to move across the JNI boundary — the
//  address of the RGBA plane — which is what `pushFrame` takes.
//
//  From there this module is the only state the two threads share:
//
//    * the frame thread (Kotlin) writes — pushFrame()
//    * the render thread (Vulkan) reads — copyLatestInto()
//
//  and two values the menu authors and the frame thread obeys:
//
//    * wanted() / wantedSize() — written by the menu, read by the frame thread
//    * running()               — written by the frame thread, read by the menu
//
//  Everything is copied exactly once, on entry: the plane's buffer is owned by
//  the ImageReader and is recycled the moment the Image is closed, so a pointer
//  could not be kept even if we wanted to. That copy is also where the work
//  happens — a padded stride is squeezed out and the square the menu asked for
//  is cut out of the middle, so whoever consumes a frame downstream can assume
//  tightly packed rows of exactly the requested size.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>

namespace aimbotng {
namespace capture {

/// One frame, as handed to the renderer.
///
/// `data` points into this module's own buffer and stays valid until the next
/// pushFrame(); upload it before asking for another.
struct Frame {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row — always width * 4, padded rows are unpacked
    uint64_t id = 0; // bumps on every frame, so a consumer can skip stale ones
};

// ── Frame thread -> renderer ─────────────────────────────────────────────────

/// Stores the square centre-crop of one RGBA frame, unpacking a padded row
/// stride if the producer used one. Called from the frame thread; safe to call
/// with anything, including null (a dropped frame is never worth a crash).
///
/// The producer mirrors the whole screen at its native size — a virtual display
/// built any smaller makes the framework *scale* the mirror into the surface
/// rather than let us choose a region, which is what turned a 2K screen into a
/// blurry thumbnail (see ScreenCapture.kt). So the crop is done here instead:
/// `side x side` pixels starting at (`cropX`, `cropY`) are copied out and the
/// rest of the screen is discarded, which keeps the copy proportional to what is
/// actually used rather than to the screen.
///
/// Out-of-range arguments are ignored rather than clamped: a crop that does not
/// fit its source means the two sides disagree about the screen shape, and
/// showing a garbage frame would hide that.
void pushFrame(const void* rgba, int srcWidth, int srcHeight, int stride,
               int cropX, int cropY, int side);

/// Dimensions of the newest frame, or false when none has arrived yet. Lets the
/// caller size its destination before asking for the pixels.
bool peekSize(int& outW, int& outH);

/// Invalidates every frame produced so far.
///
/// Called when the *producer* goes away (the virtual display is torn down). The
/// frame id is process-lifetime and never rewound, so without this the next
/// session's first copy would look "new" to a consumer that had already seen the
/// previous session's last frame — and the consumer would run on pixels from a
/// display that no longer exists, with the crop origin of a screen that may have
/// since changed shape.
///
/// The id is *bumped* rather than zeroed: every existing holder of a last-seen
/// id then fails its comparison and re-reads, which is exactly the intent, and
/// it reuses the one mechanism already in place instead of adding a second
/// "is this stale" flag for callers to forget.
void invalidateFrames();

/// Copies the newest frame into `dst` if it is newer than the id in `lastId`
/// (which is updated in place). Returns false when there is nothing new, or when
/// `dstBytes` is too small for it.
///
/// The copy happens *inside* the lock, into memory the caller owns. Lending a
/// pointer out instead — which is what this used to do — left the render thread
/// reading the one buffer the frame thread writes, so a frame could be swapped
/// underneath an upload and present half of each. Copying here costs nothing
/// extra: the bytes had to reach the caller's buffer either way, and this way
/// the lock is held for a memcpy instead of for a Vulkan submit.
bool copyLatestInto(void* dst, size_t dstBytes, int& outW, int& outH,
                    uint64_t& lastId);

/// How many frames per second the producer is actually delivering, or 0 when it
/// has gone quiet. Measured over a rolling window rather than since start-up, so
/// it tracks a producer that speeds up or stalls.
int frameRate();

/// Where the square in `peekSize()` sits on the screen, in screen pixels.
///
/// The frames this module hands out are a centre crop, so a coordinate inside
/// one is meaningless on screen until the origin is added back. Inference needs
/// this: a detection is computed on the crop and has to be drawn and aimed at on
/// the screen. Returns false when there is no frame yet.
bool cropOrigin(int& outX, int& outY);

/// Whether anything is currently consuming frames. The menu lives on one page at
/// a time, so for most of its life nobody is looking at the preview — and a
/// crop of every frame that nobody will see is pure waste. The renderer sets
/// this as it enters and leaves the capture page; the frame thread skips the
/// copy while it is false, while still counting frames so frameRate() stays
/// honest about what the producer is doing.
void setConsuming(bool on);
bool consuming();

/// Whether the menu is sampling frames — the *reader* half of the supervisor's
/// decision, independent of the capture switch (`enabled()`).
///
/// The capture supervisor holds a virtual display open on `enabled() &&
/// previewWanted()`. The point of the split is that the switch must mean what it
/// says: if the second half were `consuming()` on its own, holding to infer would
/// keep a mirror alive with the switch off, and turning capture off would change
/// nothing visible — the failure that made this flag necessary.
///
/// It is deliberately NOT "the preview page is open". Inference is a reader of
/// frames in its own right, so it belongs on this side of the AND: with the
/// Capture page closed and a model running, the supervisor must still see a
/// reader, or it tears the mirror down and the detector is starved — which is
/// exactly how "inference only works with the Capture page open" happened. The
/// switch remains the user's authority; a decoder that wants frames with the
/// switch off still gets nothing, because that half of the AND is false.
void setPreviewWanted(bool on);
bool previewWanted();

// ── Renderer -> frame thread ─────────────────────────────────────────────────

/// Whether frames should be pulled at all — the menu's capture switch.
void setEnabled(bool on);
bool enabled();

/// Side of the square cut out of the centre of the screen, in pixels.
void setSize(int side);
int size();

// ── Frame thread -> renderer (status) ───────────────────────────────────────

/// Whether a producer is actually live. Kept apart from enabled(): the menu may
/// ask for frames while the producer is still coming up, or has just failed, and
/// the preview needs to tell those apart.
void setRunning(bool on);
bool running();

}  // namespace capture
}  // namespace aimbotng
