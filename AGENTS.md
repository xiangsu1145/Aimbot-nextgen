# AGENTS.md

## Project Overview

**AimbotNextgen** — 高性能 Android AI 辅助与自瞄系统。采用 YOLO 目标检测 + uinput 触摸注入架构，运行于 Shell 权限守护进程（通过 `adb shell app_process` 运行，UID 2000，类似 Shizuku 机制，无需 Root）。
系统级渲染基于 EGL + OpenGL ES 3.0 + Dear ImGui 悬浮窗，提供全套参数调整面板与中文字体/拼音搜索支持。

## Tech Stack

- **Platform & Build**: AGP 8.10.0, Kotlin 2.0.21, Jetpack Compose BOM 2024.09.00
- **SDK Targets**: compileSdk 35, targetSdk 35, minSdk 31 (Android 12+)
- **Native (C++/NDK)**: CMake 3.22+, C++17, Android NDK r27/r29 (`arm64-v8a`)
- **Graphics & UI**: OpenGL ES 3.0, EGL, Dear ImGui (with Android & OpenGL3 backends), 自研原生拼音搜索/软键盘与现代化 Fluent UI 设计
- **Inference Runtimes**:
  - **NCNN (20240410)**: 支持 CPU (多线程 OpenMP) 与 GPU (Vulkan Compute 加速)
  - **ONNX Runtime (1.20+)**: 支持 CPU, XNNPACK, NNAPI
  - **LiteRT (TensorFlow Lite 2.x)**: 支持 CPU (XNNPACK), GPU, NNAPI, NPU (NeuroPilot / MTK APU / Neuron)
  - **Qualcomm QNN**: 支持 CPU, GPU, HTP (Hexagon NPU 调度与性能档位)
- **Input Injection**: Linux `/dev/input/uinput` 物理触摸模拟与事件派发

## Architecture & Subsystems

1. **Shell Server / Daemon (`app_process`)**
   - 入口：`io.github.xiangsu1145.aimbotnextgen.daemon.ShellServerEntry`
   - 权限：Shell UID (2000)，授予并维持浮窗与系统权限，加载核心 native 依赖（如 `libomp.so` 等）。

2. **Inference Subsystem (`app/src/main/cpp/inference/`)**
   - `Engine` 抽象基类统一多后端模型生命周期（`load`、`run`、`reset`）。
   - `ncnn_engine.h/.cpp`：NCNN 推理后端，支持 CPU/GPU (Vulkan)，并提供 `validateNcnnModel` 模型结构与权重校验。
   - `onnx_engine.h/.cpp`、`litert_engine.h/.cpp`、`qnn_engine.h/.cpp`：其余 AI 后端适配。
   - `model_store.h/.cpp`：模型管理中心，负责持久化配置、检测格式识别与引擎匹配（支持 NCNN `.param` + `.bin` 双文件路径绑定）。
   - `postprocess.h/.cpp`：高效 YOLO 锚框解码、坐标映射与 NMS 筛选。

3. **Overlay & GUI (`app/src/main/cpp/ui/gui/`)**
   - `hud.h/.cpp`、`aimbot_ui.h/.cpp`：ImGui 悬浮窗界面管理器。
   - `sections/model_section.h/.cpp`：模型管理页面。
     - **NCNN 双文件选择**：支持选择 `.param` 或 `.bin` 任意文件后动态展开副文件控件，并在同一目录下自动补齐同名配对文件。
     - **双文件导入拦截校验**：点击“添加”前严格执行 `validateNcnnModel` 魔数及网络图校验，若非法则弹出模态对话框并拦截添加。
   - `sections/`：包含 Aim（自瞄参数）、Trigger（触发）、Capture（截屏/采集）、Touch（触摸与摇杆）、Recoil（压枪与后坐力补偿）、Backflash（回录）、Settings（系统设置）等子模块。

4. **Tracking & Touch (`app/src/main/cpp/tracking/`)**
   - 卡尔曼预测滤波、准星防抖、多段压枪轨迹录制与自适应插值补偿。

## Key Directory Structure

```
app/
├── src/main/java/io/github/xiangsu1145/aimbotnextgen/
│   ├── MainActivity.kt              # App Compose 主界面
│   └── daemon/ShellServerEntry.kt   # Shell 守护进程启动入口与依赖预加载
├── src/main/jniLibs/arm64-v8a/      # 打包进 APK 的共享库 (libomp.so 等)
├── src/main/cpp/
│   ├── CMakeLists.txt               # CMake 构建脚本，链接 NCNN/Vulkan/OpenMP/ImGui 等
│   ├── ncnn/arm64-v8a/              # 预编译 NCNN 静态库 (libncnn.a, libglslang.a, libSPIRV.a)
│   ├── include/                     # C++ 头文件 (ncnn, imgui, onnxruntime, tflite)
│   ├── inference/                   # 多推理引擎核心实现 (ncnn_engine, model_store 等)
│   ├── ui/gui/                      # ImGui 悬浮窗渲染、各功能 Section 与 Widgets
│   ├── tracking/                    # 预测算法、轨迹规划与后坐力处理
│   └── capture/                     # 屏幕图像采集与格式转换
```

## Build, Install & Debug Workflow

### 1. 编译与安装（长期记忆）

```bash
# 检查设备连接
adb devices

# 编译 Debug APK（包含 CMake 编译 native 核心代码）
./gradlew assembleDebug

# 一键安装到已连接设备（支持增量部署）
./gradlew installDebug

# 清理并全量重新编译安装
./gradlew clean installDebug
```

> **注意**：当用户要求安装时，直接调用 `./gradlew installDebug` 进行安装。

### 2. TCP 实时调参及遥测通道

项目内置专用 TCP 调参服务（监听端口 `18086`，随浮窗自动启动），免重启、免点界面即可实时调节 PID/卡尔曼参数。

> **⚠️ 使用前提（必读）**：`adb forward tcp:18086` 只在 **Agent 需要主动调参 / 读取遥测（GET / SET / STATUS / STREAM）** 时才执行。
> 仅查看延迟、耗时、日志等诊断信息时**不要**转发此端口、不要连接此通道——改用 logcat（`adb logcat`）即可。
> 用户明确要求：日常任务禁止默认执行 `adb forward tcp:18086`。

```bash
# 映射设备端口到本地（仅调参/遥测时执行，见上方使用前提）
adb forward tcp:18086 tcp:18086

# 支持的 TCP 调参命令（可通过 nc 127.0.0.1 18086 发送）：
# GET                     - 读取当前所有自瞄与追踪参数
# SET kp 0.04             - 实时设置比例系数
# SET ki 0.01             - 实时设置积分系数
# SET kd 0.15             - 实时设置微分阻尼系数
# SET kf 0.60             - 实时设置前馈系数
# SET smooth 1.0          - 输出平滑
# SET deadzone 0.05       - 死区比例
# SET delay 0             - 延迟补偿帧数
# SET terminate 5         - 丢失保留帧数
# SET predict 0           - 丢框预测帧数
# STATUS                  - 读取瞬时 PID 分量及与目标 px 误差
# STREAM 1                - 开启 20Hz 实时遥测推流
# LOG 1                   - 开启 Logcat 详细逐帧调试日志
```

