// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — finding the vendor libraries
//
//  The daemon is not an app. It is `app_process` — a system binary in
//  /system/bin — and our library is dlopen()ed out of the APK by absolute path.
//  The dynamic linker resolves a library's DT_NEEDED entries against the
//  *executable's* directory and the system paths, and ours is in neither. The
//  existing build already works around this once, by linking libc++ statically
//  (see the CMake comment about the static C++ runtime under defaultConfig).
//
//  Vendor inference libraries cannot be statically linked into anything: they
//  are shipped as .so files, several of them, and they dlopen their own siblings
//  by bare name from inside. So the workaround has to be explicit — find the
//  directory, then load each dependency by absolute path with RTLD_GLOBAL before
//  the library that needs it is touched.
//
//  Everything that talks to a vendor .so goes through this one module. Doing it
//  per engine is what produced the old project's `m_preloaded` /
//  `m_native_lib_dir` pair duplicated in three files, with three slightly
//  different ideas of the search order.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "inference/backend.h"

#include <cstddef>
#include <string>

namespace aimbotng {
namespace infer {

/// Absolute path of the directory holding our own shared libraries, with a
/// trailing slash. Resolved once, on first call, from the address of a symbol
/// in this library — the daemon has no Context and no AssetManager, so there is
/// no property to read and no API to ask.
///
/// Returns an empty string if it cannot be determined, which makes every
/// preload() below fail loudly rather than pick up a same-named system library.
const char* libraryDir();

/// dlopen()s `name` out of libraryDir() and keeps it resident.
///
/// `name` is a bare file name, e.g. "libQnnHtp.so" — never a path. RTLD_NOW so a
/// library that is going to fail does it here, while the caller still has a
/// place to put the message, rather than halfway through a load. RTLD_GLOBAL so
/// the vendor library's own bare-name lookups resolve against what we loaded.
///
/// Returns the handle, or nullptr with `why` filled in. Already-loaded is a
/// success, not a duplicate.
void* preload(const char* name, std::string* why = nullptr);

/// Preloads a null-terminated list in order, stopping at the first failure.
/// Order matters: QNN wants its backend interface before the skel that
/// implements it, and the skel before the delegate.
bool preloadAll(const char* const* names, std::string* why = nullptr);

/// The vendor libraries a pair needs, in load order, null-terminated.
///
/// Takes the Ep and not just the Runtime because for LiteRT it is the
/// accelerator that decides which file is required: XNNPACK and NNAPI live in
/// `libtensorflowlite_jni.so`, the GPU delegate is a separate library, and the
/// QNN delegate is a third. A Runtime-only signature would have to return the
/// union, which would then try to load a GPU delegate on a CPU-only request.
///
/// Empty list when the pair needs none, which is most of them: ONNX Runtime and
/// ncnn link their dependencies normally and are loaded by the linker as
/// DT_NEEDED. QNN and LiteRT are the exceptions, because they are the ones that
/// come as a directory of cooperating .so files rather than as one library.
const char* const* runtimeLibraries(Runtime, Ep, int& count);

/// Whether a file of that name exists in libraryDir(). Used by epAvailable() so
/// a backend whose .so files were stripped from the APK reports "not shipped"
/// rather than "failed to initialise", which are different bugs.
bool libraryPresent(const char* name);

/// Absolute path of a library in libraryDir(), into a caller's buffer. For log
/// lines; preload() is what actually loads.
std::string libraryPath(const char* name);

/// The dlopen error from the most recent failure, `dlerror()` already consumed.
/// Kept because dlerror() is a one-shot and the interesting call site is rarely
/// the one that can show a message.
const char* lastLoadError();

}  // namespace infer
}  // namespace aimbotng
