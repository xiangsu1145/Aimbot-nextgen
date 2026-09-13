// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::input::skip_screenshot — C++ → Kotlin reverse bridge
//
//  SurfaceControl lives on the Kotlin side: ShellLayerHost.build() reflects
//  `android.view.SurfaceControl.Builder.build()` and stashes the result in a
//  private field. The native render thread has no handle to it, so the only
//  way to flip the layer's `setSkipScreenshot` flag without rebuilding the
//  layer is to ask Kotlin to do it.
//
//  This is the seam: [applySkipScreenshot] is a plain C++ function the render
//  thread calls when the ImGui Settings page's "Anti Screenshot" switch
//  changes. The implementation lives next to the other JNI shims
//  (input_jni.cpp) because that is where the JVM is cached and where class /
//  method lookups belong — keeping the bridge in one place makes the daemon
//  easier to audit.
//
//  Called on the render thread. The first call from a thread pays for a JNI
//  attach; subsequent calls reuse the cached env. We do not detach on the
//  way out: the render thread lives for the life of the daemon, so leaving
//  the JVM reference is the right call (the daemon's own JVM cleanup frees
//  it).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

namespace aimbotng {
namespace input {

/// Toggles the menu layer's "skip screenshot" flag (the layer's hidden-from-
/// screenshots / recordings / cast-displays bit). Equivalent to a
/// SurfaceControl.Transaction.setSkipScreenshot(sc, on) applied immediately;
/// the layer is not rebuilt and the renderer keeps drawing.
///
/// @return true when the flag reached the running layer, false when the
///         layer is not up yet (the value is still recorded for the next
///         build), when the platform has no `setSkipScreenshot` method, or
///         when the JNI bridge could not be reached.
bool applySkipScreenshot(bool on);

}  // namespace input
}  // namespace aimbotng
