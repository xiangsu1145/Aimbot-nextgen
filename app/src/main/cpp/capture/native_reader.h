#pragma once

#include <jni.h>

namespace aimbotng {
namespace capture {

/// Creates an NDK AImageReader of given dimensions, registers native listener,
/// and returns a new Java Surface jobject for VirtualDisplay rendering.
/// The caller (Kotlin) receives local ownership of the Surface.
jobject createNativeCaptureReader(JNIEnv* env, int width, int height);

/// Destroys the active AImageReader and frees native pipeline resources.
void destroyNativeCaptureReader();

/// Whether the native AImageReader is currently active.
bool isNativeCaptureReaderActive();

}  // namespace capture
}  // namespace aimbotng
