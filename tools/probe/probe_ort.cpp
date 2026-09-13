// ─────────────────────────────────────────────────────────────────────────────
//  probe_ort — can the shell daemon load ONNX Runtime by absolute path?
//
//  This is the cheapest possible test of the assumption the whole inference
//  layout rests on. The daemon is `app_process`, a system binary in /system/bin,
//  so the dynamic linker resolves a library's DT_NEEDED entries against the
//  *executable's* directory — which contains none of our libraries. Every other
//  dependency in this project dodges that by linking statically; a vendor
//  inference runtime cannot be, so it has to be dlopen()ed by absolute path
//  instead, and that either works or the design is wrong.
//
//  It checks three things, in the order they can fail:
//
//    1. dlopen("/data/app/.../lib/arm64/libonnxruntime.so") succeeds — which
//       also proves ORT's OWN dependencies resolved, in a context that has no
//       libc++_shared.so anywhere on the path.
//    2. OrtGetApiBase resolves.
//    3. Calling through it returns a version string, i.e. the C API really is
//       reached and not just the ELF header parsed.
//
//  Deliberately builds with no include path and no ORT headers: it is testing
//  the library, not our build configuration, and a probe that shares headers
//  with the thing it is checking cannot tell the two failures apart.
//
//  Build with tools/probe/build_ort_probe.sh, push, run as shell:
//      probe_ort /data/app/<...>/lib/arm64
// ─────────────────────────────────────────────────────────────────────────────
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

// OrtApiBase is a fixed-ABI struct and its first two members are all we need.
// Mirroring it here is what lets this file stay header-free; the real
// declaration lives in include/onnxruntime/onnxruntime_c_api.h.
struct OrtApiBase {
    uint32_t     version;
    const char* (*GetVersionString)();
    const void* (*GetApi)(uint32_t version);
};

using GetApiBaseFn = const OrtApiBase* (*)();

/// Tries each name in `names` and reports what happened, without treating a
/// missing file as a failure — the QNN and LiteRT libraries are still to be
/// packaged, and a probe that shouts about them today is a probe nobody reads.
static int probeOne(const char* dir, const char* name, bool required) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    void* h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
        const char* err = dlerror();
        printf("  %-34s %s\n", name, err ? err : "failed");
        return required ? 1 : 0;
    }
    printf("  %-34s loaded\n", name);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: probe_ort <lib-dir>\n");
        return 2;
    }
    const char* dir = argv[1];
    printf("lib dir: %s\n\n", dir);

    // 1. The library every one of these depends on having resolved already.
    char ortPath[1024];
    snprintf(ortPath, sizeof(ortPath), "%s/libonnxruntime.so", dir);
    printf("file: libonnxruntime.so\n");
    printf("step 1  dlopen(RTLD_NOW|RTLD_GLOBAL)\n");

    void* ort = dlopen(ortPath, RTLD_NOW | RTLD_GLOBAL);
    if (ort == nullptr) {
        printf("  FAIL  %s\n", dlerror());
        printf("\nverdict: the absolute-path approach does not work here\n");
        return 1;
    }
    printf("  OK    loaded, and its own dependencies resolved\n");

    // 2. The entry point every ORT version has always had.
    printf("step 2  dlsym(\"OrtGetApiBase\")\n");
    auto getApiBase = reinterpret_cast<GetApiBaseFn>(dlsym(ort, "OrtGetApiBase"));
    if (getApiBase == nullptr) {
        printf("  FAIL  %s\n", dlerror());
        return 1;
    }
    printf("  OK    symbol present\n");

    // 3. Call it. Reaching a real string proves the C API is live, and printing
    //    it proves which runtime we are actually about to run against.
    printf("step 3  OrtGetApiBase()->GetVersionString()\n");
    const OrtApiBase* base = getApiBase();
    if (base == nullptr || base->GetVersionString == nullptr) {
        printf("  FAIL  null api base\n");
        return 1;
    }
    const char* version = base->GetVersionString();
    printf("  OK    version = %s\n", version ? version : "(null)");
    printf("        ORT_API_VERSION reported by the header we vendored = 29\n");

    // The rest are informational: not packaged yet, not a failure yet.
    printf("\nfile: the other runtimes (not required today)\n");
    static const char* const kOptional[] = {
        "libtensorflowlite_jni.so",
        "libtensorflowlite_gpu_jni.so",
        "libQnnTFLiteDelegate.so",
        "libQnnHtp.so",
        "libQnnSystem.so",
        "libQnnHtpV75Skel.so",
    };
    for (const char* n : kOptional) probeOne(dir, n, false);

    printf("\nverdict: absolute-path dlopen works from this context.\n");
    return 0;
}
