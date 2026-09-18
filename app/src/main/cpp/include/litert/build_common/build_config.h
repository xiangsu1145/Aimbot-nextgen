// Generated-equivalent of litert/build_common/build_config.h.in from
// litert_cc_sdk (LiteRT 2.1.1).
//
// The upstream file is a CMake template (`#cmakedefine01`) that only turns two
// feature toggles on or off. We consume the C API as headers-only — every call
// goes through dlsym at runtime, we never compile or link LiteRT — so the only
// thing that matters here is that the two DISABLE macros stay undefined: they
// are what gate the accelerator enums and the NPU buffer types in
// litert_common.h. Keep this file in sync with the SDK if that template ever
// grows a third toggle.

#ifndef LITERT_BUILD_COMMON_BUILD_CONFIG_H_
#define LITERT_BUILD_COMMON_BUILD_CONFIG_H_

// Intentionally left undefined: we ship NPU dispatch libraries and the GPU /
// NPU code paths must stay visible to the headers.
// #define LITERT_BUILD_CONFIG_DISABLE_GPU 0
// #define LITERT_BUILD_CONFIG_DISABLE_NPU 0

#endif  // LITERT_BUILD_COMMON_BUILD_CONFIG_H_
