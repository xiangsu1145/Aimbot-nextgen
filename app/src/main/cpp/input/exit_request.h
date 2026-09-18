// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::input::exit_request — C++ → Kotlin reverse bridge (quit)
//
//  The ImGui menu lives inside the daemon, but the daemon's lifetime is owned
//  by the Kotlin main loop: `running` is what ends the accept loop, and
//  main()'s shutdown() — the ungrab, the reader stop, the uinput teardown —
//  runs on its way out. Nothing in native land can reach that flag directly,
//  so the menu's 退出 row goes through JNI exactly the way the Settings page's
//  "Anti Screenshot" switch does ([skip_screenshot.h] is the sibling seam).
//
//  This exists because of a failure mode, not for convenience: once the daemon
//  holds the panel with EVIOCGRAB it is the only source of touch the device
//  has. If anything downstream of the panel (uinput, InputReader, the OEM's
//  input stack) stops delivering on some device, the user is left with a phone
//  that still draws our menu — which reads the grabbed events directly — and
//  answers nothing else. A button inside that menu is the one control they can
//  still reach, and killing the daemon is the one thing guaranteed to release
//  the panel: a closed fd drops the grab even if every other layer is broken.
//
//  Called on the render thread. The Kotlin side returns immediately (it only
//  sets a flag), which matters — anything that waited on the main loop from
//  here would deadlock against the join that main() does on the way down.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

namespace aimbotng {
namespace input {

/// Asks the daemon to shut itself down and hand the touch panel back.
///
/// Equivalent to a client sending DESTROY, but requested from inside the menu.
/// The flag is cleared asynchronously: this returns before the process starts
/// tearing anything down, and the render thread simply stops being asked for
/// frames once shutdown() joins it.
///
/// @return true when the request reached Kotlin; false when the JVM is not
///         cached or the bridge could not be resolved. Either way nothing here
///         throws — a broken bridge must never take the render thread down.
bool requestDaemonExit();

}  // namespace input
}  // namespace aimbotng
