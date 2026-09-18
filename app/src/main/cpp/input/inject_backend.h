// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng input — which way a touch leaves this process
//
//  There are two ways to give Android a touch we made up, and they fail for
//  completely different reasons:
//
//    UINPUT         a virtual touchscreen in /dev/uinput. Touches go in through
//                   the input device layer, so they are indistinguishable from
//                   a real panel's — and that is also the problem: everything
//                   the platform (or the OEM) does between "device registered"
//                   and "app receives a pointer" is in the path, and none of it
//                   reports anything when it decides to drop the events.
//
//    INPUT_MANAGER  MotionEvent -> InputManager.injectInputEvent(). Handed
//                   straight to InputDispatcher, never touching EventHub or
//                   InputReader, so nothing about input devices can interfere.
//                   This is what scrcpy does; it is the path that works on the
//                   devices where uinput's touches vanish.
//
//  Everything up to the last hop is shared — the reader, the grab, the menu
//  rectangles, the slot bookkeeping in uinput_inject.cpp, including the touch
//  fusion that lets the aim drive a real finger. Only the final delivery
//  changes, which is why this is a mode on that module rather than a second
//  implementation of the whole thing.
//
//  Switching is destructive on purpose. The two backends must never both be
//  live: with uinput still registered while InputManager injects, every real
//  finger would arrive twice (once mirrored through the virtual device, once
//  through the injected copy of the mirrored finger), and the phone would show
//  two pointers under one thumb. So selecting INPUT_MANAGER lifts everything
//  the virtual device is holding and then DESTROYS it, and selecting UINPUT
//  lifts everything InputManager is holding before the device is built again.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// 0 = UINPUT, 1 = INPUT_MANAGER. Matches the Touch page's dropdown order, and
/// is what gets persisted, so do not renumber.
#define INJECT_BACKEND_UINPUT        0
#define INJECT_BACKEND_INPUT_MANAGER 1

/// Switches the injection backend and does the teardown the switch requires
/// (see the file header). Idempotent. Returns 1 when the backend is ready to
/// carry touches, 0 when it is not — a caller must treat 0 as "touches we
/// produce will not arrive" and say so, never as "carry on quietly".
///
/// Safe to call at any time, including before the reader exists.
int  inject_set_backend(int backend);

/// The backend currently selected (not necessarily ready).
int  inject_get_backend(void);

/// Whether the selected backend can actually deliver a frame right now.
int  inject_is_ready(void);

/// Lifts every pointer the current backend is holding, without switching
/// anything. Used when the daemon is about to stop injecting (grab released,
/// sink off, shutdown) so nothing is left pressed on the far side.
void inject_release_all(void);

#ifdef __cplusplus
}

namespace aimbotng {
namespace input {

/// Reverse JNI into InputManagerInjector (Kotlin). See input_jni.cpp for the
/// bridge mechanics; declared here because uinput_inject.cpp calls them.

/// Sets up the reflective InputManager handle. Returns false when it could not
/// be obtained — the caller reports that, it never falls back silently.
bool imBridgeInit(void);

/// Publishes the complete set of pointers that should be down right now:
/// `n` entries, `ids[k]` the caller's stable finger id, `xs[k]`/`ys[k]` SCREEN
/// pixels. The Kotlin side owns the diffing (it is the only writer, so it can
/// be) and turns a change into exactly one MotionEvent.
void imBridgePushFrame(const int* ids, const int* xs, const int* ys, int n);

/// Publishes an empty set — i.e. lifts whatever is down. Called when the
/// backend is being swapped out from under the injected pointers.
void imBridgeReleaseAll(void);

/// Last failure the InputManager path reported, for the log/UI. Never null;
/// empty when there has not been one.
const char* imBridgeLastError(void);

/// Whether the Kotlin side currently believes it can inject.
bool imBridgeIsReady(void);

}  // namespace input
}  // namespace aimbotng
#endif
