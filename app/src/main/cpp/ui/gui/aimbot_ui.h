// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui — ImGui over Vulkan
//
//  Owns the entire rendering stack: the Vulkan instance / device / swapchain,
//  the ImGui context and both backends (Android platform + Vulkan renderer),
//  and runs them on a dedicated render thread.
//
//  This was split out of aimbotng.cpp so that the JNI shim there stays a thin
//  translation layer — it only converts Java objects into native handles and
//  forwards the call here. All rendering/initialisation knowledge lives in
//  this module.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <android/native_window.h>

namespace aimbotng {
namespace ui {

/// Touch actions, mirroring android.view.MotionEvent's action constants.
enum TouchAction {
    kTouchDown = 0,
    kTouchMove = 1,
    kTouchUp = 2,
    kTouchCancel = 3,
};

/// Binds the renderer to `window`.
///
/// Takes ownership of one window reference: it is released by stop(). Calling
/// this while a session is running stops it first, so it is safe to hand over
/// a fresh surface after a configuration change.
void setWindow(ANativeWindow* window);

/// Spawns the render thread for the bound window.
/// Returns false when no window is bound or a thread is already running.
bool start();

/// Joins the render thread, tears down every GPU resource and releases the
/// bound window. Idempotent — safe to call twice.
void stop();

/// True while the render thread is alive.
bool isRunning();

/// Toggles the menu layer's "skip screenshot" flag.
///
/// Lives here rather than on a dedicated module because it is conceptually
/// a renderer concern (the menu's visibility on screen) even though the
/// actual SurfaceControl transaction runs back on the Kotlin side — the
/// SurfaceControl handle is held by ShellLayerHost via reflection and the
/// native render thread has no reference to it. Called from
/// [sections::syncSettingsPage] when the ImGui Settings page's switch
/// changes; safe to call when the layer is not up (the value is recorded
/// for the next build).
void setSkipScreenshot(bool on);

/// Size of the bound surface's buffer, in pixels; (0,0) when there is none.
///
/// This is the space ImGui draws in (`DisplaySize`), and the rectangle the menu
/// layer's crop and display frame must describe. [ShellLayerHost] reads it back
/// the moment a Surface is bound so it can put the frame on the *actual* buffer:
/// a frame that disagrees with the buffer is scaled by SurfaceFlinger — one axis
/// more than the other — and the menu comes out squashed.
void windowSize(int* width, int* height);

/// The live capture preview as an ImGui texture id, or 0 when nothing has
/// arrived yet (or the build has no ImGui).
///
/// Deliberately an integer rather than an ImTextureID: this header is included
/// by modules that have no business knowing about ImGui, and the one caller
/// that does (hud.cpp) can cast. Render thread only, between NewFrame() and
/// Render().
unsigned long long capturePreviewTexture();

// ── Interactive regions ──────────────────────────────────────────────────
//  The menu lives above an app that keeps receiving touches: the shell daemon
//  grabs the physical panel and mirrors every finger back out through uinput so
//  the app underneath still works. A finger that lands on the menu must NOT be
//  mirrored, otherwise ImGui and the app below would both react to it.
//
//  The daemon cannot see ImGui (separate process, no ImGui context), so the
//  geometry is published to it: this call hands out the window rectangles each
//  frame, the JNI shim passes them to Kotlin, and Kotlin forwards them down the
//  ADB stream as `REGION` commands.

/// Writes up to `maxRects` rectangles as [x, y, w, h] quads, in the same pixel
/// space as onTouch()'s coordinates, and returns how many were written.
/// An empty result means "no menu on screen" — mirror everything.
/// Thread-safe: the render thread writes, the caller's thread reads.
int getInteractiveRegions(int* out, int maxRects);

// ── Input ────────────────────────────────────────────────────────────────
// Thread-safe. Events are queued here and drained by the render thread at the
// start of the next frame, where they are pushed into ImGui's IO queue. This
// deliberately avoids ImGui's own IO queue from the caller's thread because
// that queue is single-threaded.
void onTouch(int action, float x, float y);
void onScroll(float dy);

}  // namespace ui
}  // namespace aimbotng
