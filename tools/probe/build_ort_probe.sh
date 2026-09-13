#!/usr/bin/env bash
#
# 编译 ORT 加载探针为独立 aarch64 可执行文件。
#
#   bash tools/probe/build_ort_probe.sh
#
# 同 build.sh：静态链接 C++ 运行时，产出单文件，push 到 /data/local/tmp 直接跑。

set -euo pipefail

NDK="${ANDROID_NDK:-/d/Android/Sdk/ndk/29.0.14206865}"
API="${API:-26}"
HOST_TAG="${HOST_TAG:-windows-x86_64}"

BIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CXX="$BIN/aarch64-linux-android${API}-clang++"
[ -f "$CXX" ] || CXX="$CXX.cmd"
READELF="$BIN/llvm-readelf"

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/probe_ort.cpp"
OUT="$HERE/probe_ort"

if [ ! -f "$CXX" ]; then
    echo "找不到交叉编译器: $CXX" >&2
    echo "用 ANDROID_NDK=<ndk 根目录> 覆盖默认路径" >&2
    exit 1
fi

# clang 是 Windows 原生程序，不认 MSYS 风格的 /g/... 路径。
if command -v cygpath >/dev/null 2>&1; then
    SRC="$(cygpath -m "$SRC")"
    OUT="$(cygpath -m "$OUT")"
fi

"$CXX" -std=c++17 -O2 -fPIE -pie -static-libstdc++ -Wall -Wextra \
    "$SRC" -o "$OUT" -ldl

echo "built -> $OUT"
echo
echo "--- 动态依赖（应为空：探针本身要自包含）---"
"$READELF" -d "$OUT" | grep NEEDED || echo "(无)"
