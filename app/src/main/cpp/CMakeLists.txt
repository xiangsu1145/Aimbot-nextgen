cmake_minimum_required(VERSION 3.22.1)
project("aimbotng" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Debug builds get -O2 as well as -g.
#
# The NDK's default Debug profile is -O0, and this library is not a program that
# spends its time in syscalls — the frame path alone is a 1.6 MB resample per
# inference plus a full-screen Vulkan present, and at -O0 that resample measured
# ~50 ms against ~2 ms optimised. Timings taken from a -O0 build are not "a bit
# pessimistic", they are wrong by an order of magnitude, and the whole point of
# the Model page's readout is to be trusted. Line tables and symbols are
# unaffected, so debugging stays intact.
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O2")

# Android system libraries
find_library(log-lib log)
find_library(android-lib android)
find_library(VULKAN-lib vulkan)

# ImGui sources (cloned into include/imgui/)
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/imgui)
set(IMGUI_SOURCES "")
set(HAS_IMGUI OFF)

if(EXISTS ${IMGUI_DIR}/imgui.cpp)
    set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_android.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    )
    set(HAS_IMGUI ON)
else()
    message(WARNING "ImGui not found in ${IMGUI_DIR}, building without ImGui")
endif()

# Your native source files
set(NATIVE_SOURCES
    ui/gui/aimbot_ui.cpp            # ImGui over Vulkan (renderer + input)
    ui/gui/hud.cpp                  # the menu board shell (rail, layout, animation)
    ui/gui/float_button.cpp         # the always-on HUD toggle button
    ui/gui/widgets.cpp              # switch / slider / dropdown / button / text-field controls
    ui/gui/fonts.cpp                # embedded Latin + Chinese ImGui fonts
    ui/gui/sections/aim_section.cpp         # Aim page (自瞄)
    tracking/kalman_tracker.cpp             # multi-target Kalman tracker (smoothing + lead)
    ui/gui/sections/trigger_section.cpp     # Trigger page (扳机)
    ui/gui/sections/model_section.cpp       # Model page + Add-Model dialog + file browser
    ui/gui/sections/touch_section.cpp       # Touch page (触摸)
    ui/gui/sections/capture_section.cpp     # Capture page + live preview
    ui/gui/sections/settings_section.cpp    # Settings page (设置)
    config/config_manager.cpp         # page parameters <-> config.json
    capture/capture.cpp             # frame hand-off between the capture thread and the renderer
    inference/model_store.cpp       # the Add-Model page's persistent model list
    inference/preprocess.cpp        # frame -> tensor, shared by every runtime
    inference/postprocess.cpp       # tensor -> boxes, shared by every runtime
    inference/engine.cpp            # the factory, and backend.h's table + probes
    inference/libpath.cpp           # locating and preloading the vendor .so files
    inference/capabilities.cpp      # what this device is, described not decided
    inference/model_type.cpp        # file-level dtype probe (ONNX + TFLite)
    inference/ort_engine.cpp        # ONNX Runtime: XNNPACK and plain CPU
    inference/litert_engine.cpp     # LiteRT (TFLite): CPU/XNNPACK/NNAPI/GPU delegates
    inference/session.cpp           # worker thread + drop-newest frame mailbox
    inference/model_runtime.cpp     # the platform's front door: start/stop/pump
    input/uinput_inject.cpp         # /dev/uinput touch injection + mirroring
    input/touch_reader.cpp          # EVIOCGRAB + evdev physical panel reader
    input/input_jni.cpp             # JNI surface used by the shell daemon
)

# ── Embedded fonts ───────────────────────────────────────────────────────
# The .ttf files are compiled straight into the library's .rodata with
# objcopy, so nothing is read from disk or from the APK at runtime. That
# matters because the shell daemon (app_process) has no Context/AssetManager.
#
# objcopy names the symbols after the file, so "simhei_regular.ttf" yields
# _binary_simhei_regular_ttf_start / _binary_simhei_regular_ttf_end.
set(FONT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res/font")
set(FONT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts")

if(NOT CMAKE_OBJCOPY)
    set(CMAKE_OBJCOPY "${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-objcopy")
endif()

if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(FONT_OBJCOPY_FORMAT "elf64-littleaarch64")
    set(FONT_OBJCOPY_ARCH "aarch64")
else()
    set(FONT_OBJCOPY_FORMAT "elf32-littlearm")
    set(FONT_OBJCOPY_ARCH "arm")
endif()

set(EMBEDDED_FONT_SOURCES
    "${FONT_SOURCE_DIR}/product_sans_medium.ttf"   # Latin / main font
    "${FONT_SOURCE_DIR}/simhei_regular.ttf"        # Chinese fallback
)

set(EMBEDDED_FONT_OBJECTS "")
foreach(FONT_FILE ${EMBEDDED_FONT_SOURCES})
    get_filename_component(FONT_NAME "${FONT_FILE}" NAME_WE)
    set(FONT_OBJ "${FONT_BINARY_DIR}/${FONT_NAME}.o")
    add_custom_command(
        OUTPUT "${FONT_OBJ}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${FONT_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT_FILE}" "${FONT_BINARY_DIR}/${FONT_NAME}.ttf"
        COMMAND ${CMAKE_OBJCOPY} -I binary -O ${FONT_OBJCOPY_FORMAT} -B ${FONT_OBJCOPY_ARCH}
                --rename-section .data=.rodata,alloc,load,readonly,data,contents
                "${FONT_NAME}.ttf" "${FONT_OBJ}"
        WORKING_DIRECTORY "${FONT_BINARY_DIR}"
        DEPENDS "${FONT_FILE}"
        COMMENT "Embedding font: ${FONT_NAME}"
    )
    list(APPEND EMBEDDED_FONT_OBJECTS "${FONT_OBJ}")
endforeach()

# Build shared library (output: libaimbotng.so)
add_library(${PROJECT_NAME} SHARED
    ${NATIVE_SOURCES}
    ${IMGUI_SOURCES}
    ${EMBEDDED_FONT_OBJECTS}
)

# Module headers are included relative to the cpp root, e.g.
#   #include "ui/gui/aimbot_ui.h"
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(HAS_IMGUI)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_IMGUI=1)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
    )
endif()

target_link_libraries(${PROJECT_NAME}
    ${log-lib}
    ${android-lib}
    ${VULKAN-lib}
)

# ── TensorFlow Lite runtime ────────────────────────────────────────────
# The .so is shipped under app/src/main/jniLibs/<abi>/libtensorflowlite_jni.so
# (it used to be dlopen'ed like libonnxruntime.so, but the JNI build of
# TFLite is on the AAR's "headers only" side and linking it directly is
# the simpler path that the old project followed). The C++ headers under
# app/src/main/cpp/include/tensorflow/ stay on the include path either way,
# so the engine code compiles unchanged.
#
# Only arm64-v8a is shipped today — the project does not build for
# armeabi-v7a or x86_64. A real release would need a fat APK; the current
# target device (OnePlus) is arm64-only.
#
# Loading into the shell daemon: the daemon runs under `app_process` in
# Android's compat library namespace (clns-1), which only knows about
# <uses-library> + system public libraries. jniLibs entries that are not
# in those lists are invisible to SONAME lookup, so DT_NEEDED resolution
# for `libtensorflowlite_jni.so` fails and `System.load(libaimbotng.so)`
# aborts. The daemon side preloads these .so by absolute path before
# System.load; see ShellServerEntry.kt::preloadDaemonDeps.
add_library(tensorflowlite_jni SHARED IMPORTED)
set_target_properties(tensorflowlite_jni PROPERTIES
    IMPORTED_LOCATION
    "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libtensorflowlite_jni.so"
)
target_link_libraries(${PROJECT_NAME} tensorflowlite_jni)

# ── QNN HTP runtime ──────────────────────────────────────────────────
# Same pattern as libtensorflowlite_jni: a directory of cooperating .so
# files shipped under app/src/main/jniLibs/<abi>/. Link-time resolution
# here, not dlopen — that is what the old project did and it sidesteps the
# whole "which library lives at which absolute path" problem that
# runtimeLibraries() exists for.
#
# What is linked in (aarch64 Android ELF, only):
#   libQnnTFLiteDelegate.so       the factory LiteRtEngine calls
#   libqnn_tflite_delegate_jni.so  JNI symbols, kept for completeness
#   libQnnHtp.so                   the HTP backend itself
#   libQnnHtpPrepare.so            graph preparation (offline compile)
#   libQnnSystem.so                QNN runtime system layer
#   libQnnHtpNetRunExtensions.so   HTP runtime extensions
#
# Loading into the shell daemon: see the libtensorflowlite_jni comment block
# above for the namespace problem. They are linked in here so the C++ code
# can resolve symbols at link time, and the daemon preloads them by absolute
# path before System.load(libaimbotng.so) — see ShellServerEntry.kt.
#
# What is shipped but NOT linked:
#   *Stub / *Skel                  per-SoC Hexagon DSP binaries (V68, V73,
#                                  …). They run on the Hexagon coprocessor,
#                                  not on the aarch64 CPU, so Android's
#                                  aarch64 linker rejects them at link time.
#                                  QNN's runtime finds and dlopen()s the
#                                  matching one at first call, using the
#                                  skel_library_dir we hand it — see
#                                  LiteRtEngine::buildDelegate.
#   *ProfilingReader               optional, used by htpoptrace path; not
#                                  needed for normal inference.
#
# The MTK user case (no QNN runtime on the device) is handled at runtime:
# engine.cpp::epAvailable(LiteRT, Htp) detects the device and refuses the
# load — but the engine itself is compiled in.
macro(add_qnn_lib basename)
    set(libname "${basename}.so")
    add_library(${basename} SHARED IMPORTED)
    set_target_properties(${basename} PROPERTIES
        IMPORTED_LOCATION
        "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/${libname}")
endmacro()

add_qnn_lib(libQnnTFLiteDelegate)
add_qnn_lib(libqnn_tflite_delegate_jni)
add_qnn_lib(libQnnHtp)
add_qnn_lib(libQnnHtpPrepare)
add_qnn_lib(libQnnSystem)
add_qnn_lib(libQnnHtpNetRunExtensions)

target_link_libraries(${PROJECT_NAME}
    libQnnTFLiteDelegate
    libqnn_tflite_delegate_jni
    libQnnHtp
    libQnnHtpPrepare
    libQnnSystem
    libQnnHtpNetRunExtensions
)

# ── Vendored third-party headers ─────────────────────────────────────────
# No libraries are linked here yet; this only puts the headers on the search
# path so the inference layer can be written and compiled while the runtimes
# are brought up one at a time. A missing tree is a warning, not an error —
# the daemon must keep building without any of them.
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/onnxruntime   # flat ORT public headers
)

foreach(VENDOR_TREE onnxruntime tensorflow qnn ncnn)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include/${VENDOR_TREE})
        message(WARNING "vendored headers missing: include/${VENDOR_TREE}")
    endif()
endforeach()

# ── ADB SPAKE2 pairing library ────────────────────────────────────
find_package(boringssl REQUIRED CONFIG)

set(ADB_JNI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../jni)

add_library(adb SHARED
    ${ADB_JNI_DIR}/adb_pairing.cpp
)

target_include_directories(adb PRIVATE ${ADB_JNI_DIR})
target_compile_options(adb PRIVATE -Werror=format -fno-exceptions -fno-rtti -fno-threadsafe-statics)
target_link_libraries(adb log boringssl::crypto_static)
