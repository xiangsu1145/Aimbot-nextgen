# 屏幕采集（录屏）方案研究

> 目标：在**不使用 MediaProjection（安卓给 App 的录屏权限）**的前提下，用**高效、纯 C++** 的方式拿到屏幕画面，供后续录屏使用。
> 研究基准：本机 OnePlus（ColorOS / Android 15，Shell uid 2000，有 KernelSU），scrcpy 4.0 源码（`G:\scrcpy`）。

---

## 0. 结论先行

1. **scrcpy 之所以不需要录屏权限**，是因为它根本不是一个 App——它跑在 **shell 进程（uid 2000，`app_process` 启动的 Java 进程）** 里，直接用隐藏 API 指挥 SurfaceFlinger 建一块"虚拟显示"来**镜像主屏**，再把这块虚拟显示的输出直接接到 **MediaCodec 的输入 Surface** 上。真正的编码在 **mediaserver** 进程里做，图形缓冲区以**句柄**在进程间传递（零拷贝），所以又高效又低延迟。

2. **这套机器本身就有纯 C++ 版本**——就是系统自带的 `/system/bin/screenrecord`。我在这台设备上核对了它的符号表，取屏 + 编码所需的**全部原生符号都在**。也就是说，"纯 C++ 取屏"不需要我们发明轮子，**照抄 screenrecord 的调用序列即可**。

3. **推荐路线**：在**现有的 shell 守护进程**（已经在用 SurfaceFlinger 图层渲染 ImGui 的那个）里，用 `dlopen` 拿 `libgui` + `libstagefright`，按 `SurfaceComposerClient::createVirtualDisplay` → `Transaction::setDisplaySurface/Projection/LayerStack` → `MediaCodec::createInputSurface/configure/start` 的顺序实现。**不依赖任何 App 权限，不用 MediaProjection，CPU 侧几乎零开销。**

4. **路线取决于你要"码流"还是"像素"**（2026-09-10 补充）：第 3 条是**要 H.264 码流**（录屏 / 推流）的走法。若目标是**拿 RGBA 像素喂推理 / CV**，就**不要接编码器**——编码器只吐码流，想拿回像素还得解码一次，白绕两趟。改成把虚拟显示的输出接到 **ImageReader**（NDK 的 `AImageReader`），或走 `captureDisplay` 按帧拉——两者都直接返回 `GraphicBuffer`，格式 **`RGBA_8888`**。详见 §3 路线 B。

---

## 1. scrcpy 的取屏链路（源码级拆解）

### 1.1 进程模型：它不是 App

| 角色 | 进程 | uid | 关键点 |
|---|---|---|---|
| 客户端 | PC 上的 `scrcpy` | — | 纯 C，负责解码 + 显示 + 转发输入 |
| 服务端 | 手机上的 `app_process` 起的 Java 进程 | **2000 (shell)** | 真正取屏 + 编码的地方 |
| 编码 | `mediaserver` | system | MediaCodec 硬件编码在这里跑 |

服务端不是 APK，是用 `CLASSPATH=scrcpy-server app_process` 直接拉起的 Java 进程（`app/src/server.c`）。因为 uid=2000 属于**平台签名的系统包**权限域，SurfaceFlinger、DisplayManager、MediaCodec 这些服务对它全部放行——**这就是"免录屏权限"的全部秘密**。MediaProjection 是给普通 App 开的那扇门，shell 走的是后门，压根不用进门。

### 1.2 取屏：VirtualDisplay 镜像主屏

`ScreenCapture` / `NewDisplayCapture` 干的事，本质是**建一块虚拟显示，让它复制主屏的图层栈**：

- **新路径（`NewDisplayCapture`，A10+ 首选）**：反射 `DisplayManagerGlobal` → `DisplayManager.createVirtualDisplay(name,w,h,dpi,surface,flags)`，并带上 `VIRTUAL_DISPLAY_FLAG_PUBLIC|PRESENTATION|OWN_CONTENT_ONLY|SUPPORTS_TOUCH|...`。即"建一个真实的虚拟显示设备"，并把它的**显示输出 Surface 指向编码器输入面**。
- **旧路径（`ScreenCapture` 回退）**：`SurfaceControl.createDisplay("scrcpy", secure)` → `openTransaction()` → `setDisplaySurface(display, encoderSurface)` + `setDisplayProjection(...)` + `setDisplayLayerStack(display, layerStack)` → `closeTransaction()`。
  - 这里 `layerStack` 取的是**主屏的 layerStack（通常 0）**——把虚拟显示的图层栈设成主屏那一套，它就"变成"了主屏的镜像。**这是整个镜像机制的核心一句话。**

### 1.3 编码：MediaCodec + Surface 输入（关键在零拷贝）

```java
format.setInteger(KEY_COLOR_FORMAT, COLOR_FormatSurface);   // 走 Surface 而非 ByteBuffer
mediaCodec.configure(format, null, null, CONFIGURE_FLAG_ENCODE);
Surface surface = mediaCodec.createInputSurface();          // ← 编码器的输入面
capture.start(surface);                                     // ← 把它交给虚拟显示
mediaCodec.start();
// 之后只需 dequeueOutputBuffer 取码流
```

因为输入是 **Surface**，帧从 SurfaceFlinger 直接进编码器，**我们进程里从头到尾看不到一帧像素**。docs 里的原话说得很清楚：*"The full frames are never seen by the screenrecord process."*

媒体格式上还有几个提高实时性的关键 key：`KEY_LATENCY=1`（来一帧编一帧）、`KEY_REPEAT_PREVIOUS_FRAME_AFTER=100000`（无新帧时重发上一帧、首帧立即出画）、`KEY_PRIORITY=0`（实时优先级）、私有 key `max-fps-to-encoder`。

### 1.4 输出

`dequeueOutputBuffer` 拿到 H.264/HEVC 码流，scrcpy 经 socket 推给 PC；screenrecord 则用 MediaMuxer 写成 MP4。

---

## 2. 本机验证：纯 C++ 所需的原生符号**全在**

我在设备上逐个核对了符号（`strings <so> | grep <mangled>`）：

### 2.1 `libgui.so` —— 显示/合成控制（建虚拟显示、镜像）

| 符号（demangle 后） | 用途 |
|---|---|
| `SurfaceComposerClient::getDefault()` | 拿客户端单例 |
| `SurfaceComposerClient::createVirtualDisplay(const std::string& name, bool secure, const std::string& uniqueId, float refreshRate)` → `sp<IBinder>` | **建虚拟显示**（A15 新签名） |
| `SurfaceComposerClient::destroyVirtualDisplay(sp<IBinder>)` | 销毁 |
| `SurfaceComposerClient::getPhysicalDisplayIds()` / `getPhysicalDisplayToken(PhysicalDisplayId)` | 拿主屏 token |
| `SurfaceComposerClient::getDisplayState(sp<IBinder>, ui::DisplayState*)` | 读主屏 layerStack / 分辨率 |
| `Transaction::setDisplaySurface(sp<IBinder>, sp<IGraphicBufferProducer>)` | **把编码器输入面接到虚拟显示** |
| `Transaction::setDisplayProjection(sp<IBinder>, ui::Rotation, Rect, Rect)` | 投影/旋转 |
| `Transaction::setDisplayLayerStack(sp<IBinder>, ui::LayerStack)` | **镜像主屏（设成主屏的 layerStack）** |
| `Transaction::setDisplaySize(sp<IBinder>, uint32 w, uint32 h)` | 改虚拟显示尺寸 |
| `Transaction::apply(bool, bool)` | 提交 |

> ⚠️ **重要变化**：老 API `SurfaceComposerClient::createDisplay(...)` / `destroyDisplay(...)` 在 A15 的 `libgui.so` 里**已经不再导出**（我 grep 结果为空）。所以 scrcpy 那条"旧回退路径"在 A15 上其实走不通——**必须用新的 `createVirtualDisplay(name, secure, uniqueId, refreshRate)`**。

### 2.2 `libstagefright.so` —— 编码（C++ MediaCodec，screenrecord 用的就是它）

`MediaCodec::CreateByType(sp<ALooper>, const AString&, bool encoder, int*, int, int)`、`createInputSurface(sp<IGraphicBufferProducer>*)`、`configure(sp<AMessage>, sp<Surface>, sp<ICrypto>, uint32_t)`、`start/stop`、`dequeueOutputBuffer(...)`、`getOutputFormat(...)`、`releaseOutputBuffer(...)`，以及 `MediaCodecList::getInstance()` —— **整条 C++ 编码 API 都在**。

### 2.3 `libmediandk.so` —— NDK 编码（备选，更"正规"）

`AMediaCodec_createEncoderByType`、`AMediaCodec_createInputSurface`、`AMediaCodec_configure`、`AMediaCodec_dequeueOutputBuffer`、`AMediaCodec_getOutputBuffer` …… NDK 的 `AMediaCodec_*` 全套可用。**但注意**：NDK 的 `createInputSurface` 给的是 `ANativeWindow*`，要把它转成 `sp<IGraphicBufferProducer>` 再交给 `setDisplaySurface` 反而绕；**用 C++ 的 `MediaCodec::createInputSurface` 能直接拿到 IGraphicBufferProducer，更适合**。

### 2.4 像素路（要 RGBA 时这才是主线）

`libui.so` 有 `GraphicBuffer::lock/unlock/toAHardwareBuffer`——`lock` 直接给你 **CPU 可读的像素指针 + stride + format**；`libnativewindow.so` 有整套 `AHardwareBuffer_*`；`libmediandk.so` 有整套 `AImageReader_*` / `AImage_*`（**NDK 公共 API，连私有库都不必 dlopen**）。`libandroid.so` 里还导出了 `ASurfaceControl_create` / `ASurfaceTransaction_apply`。

实测本机主屏图层 `default-format=1`——在 Android PixelFormat 里 **1 就是 `RGBA_8888`**，每像素 4 字节、字节序 R→G→B→A。当前 color mode 为 SDR `ColorMode::SRGB`（dataspace `V0_SRGB`），正是 CV 模型要的标准 sRGB。

### 2.5 铁证

`/system/bin/screenrecord` 的符号表里**原封不动带着上面这一整套**（`SurfaceComposerClient::createVirtualDisplay`、`Transaction::setDisplaySurface/setDisplayLayerStack/setDisplayProjection`、`MediaCodec::CreateByType/createInputSurface/configure/dequeueOutputBuffer`、`MediaCodecList::getInstance`）。

> **AOSP 的 `frameworks/av/cmds/screenrecord/screenrecord.cpp` 就是"纯 C++ 取屏 + 编码"的官方参考实现**，我们要做的只是把它搬进现有守护进程。

---

## 3. 可落地路线（按推荐度排序）

### ★ 路线 A（要码流走这条）：虚拟显示镜像 + 硬编，全原生

伪代码（逻辑顺序）：

```cpp
// 1) 打开平台私有库（shell uid 下 dlopen 无障碍）
void* gui = dlopen("libgui.so", RTLD_NOW);
void* sf  = dlopen("libstagefright.so", RTLD_NOW);

// 2) 建编码器 → 拿输入面
sp<ALooper> looper = new ALooper; looper->setName("cap"); looper->start();
sp<MediaCodec> codec;
MediaCodec::CreateByType(looper, "video/avc", /*encoder*/true, &err);
sp<AMessage> fmt = new AMessage;
fmt->setString("mime", "video/avc");
fmt->setInt32("width", W); fmt->setInt32("height", H);
fmt->setInt32("bitrate", BPS);
fmt->setInt32("color-format", 0x7F000789);   // COLOR_FormatSurface
fmt->setInt32("i-frame-interval", 1);
fmt->setInt32("priority", 0);                 // 实时
fmt->setInt32("latency", 1);                  // 来一帧编一帧
codec->configure(fmt, nullptr, nullptr, MediaCodec::CONFIGURE_FLAG_ENCODE);

sp<IGraphicBufferProducer> encSurface;
codec->createInputSurface(&encSurface);       // ← 关键：直接拿到 IGBP

// 3) 建虚拟显示，让它镜像主屏，输出接到编码器输入面
auto* c = SurfaceComposerClient::getDefault();
sp<IBinder> dpy = c->createVirtualDisplay("capture", /*secure*/false, /*uniqueId*/"", fps);
ui::DisplayState st; c->getDisplayState(mainToken, &st);   // 读主屏 layerStack
SurfaceComposerClient::Transaction t;
t.setDisplaySurface(dpy, encSurface);                       // 输出 → 编码器
t.setDisplayProjection(dpy, ui::ROTATION_0, srcRect, dstRect);
t.setDisplayLayerStack(dpy, st.layerStack);                 // ← 镜像主屏的那一步
t.apply(/*synchronous*/false, /*oneWay*/true);

// 4) 起编，循环取码流（帧从没进过我们的进程）
codec->start();
while (running) {
    size_t idx, off, size; int64_t pts; uint32_t flags;
    if (codec->dequeueOutputBuffer(&idx,&off,&size,&pts,&flags, 250000) == OK) {
        // 写文件 / 推 socket / 喂自己的容器
        codec->releaseOutputBuffer(idx);
    }
}
```

**不变量 & 坑：**
- **`setDisplayLayerStack` 必须是主屏的 layerStack**（一般 `0`）——这是"镜像"二字的全部含义，写错就得到一块空屏。
- **A15 上没有 `createDisplay` 了**，别照抄老教程。
- `apply(false, true)`（异步、one-way）足够；用同步版会阻塞我们的渲染线程。
- 编码在 **mediaserver** 里跑，我们只持有 `IGraphicBufferProducer`，**CPU 侧几乎为 0**。
- 帧率天然被面板 vsync 驱动，上限 = 屏幕刷新率。

### ★ 路线 B（要像素走这条）：不接编码器，直接拿 RGBA

**这是"给 YOLO / CV 喂帧"的正确路线。** 与路线 A 前半段完全相同（建虚拟显示 + `setDisplayLayerStack(主屏)`），区别只在**输出口接什么消费者**。

#### B.1 虚拟显示 + ImageReader（推荐，持续高帧）

输出口接一个 **ImageReader**——它本质就是个 BufferQueue 消费者，SF 持续把合成结果渲进去，我们按需 acquire。

NDK 侧叫 `AImageReader`（`libmediandk.so`，**公共 API**）：

| 符号 | 用途 |
|---|---|
| `AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_RGBA_8888, usage, maxImages, &reader)` | 建 reader |
| `AImageReader_getWindow(reader, &window)` | 拿 `ANativeWindow*` → 转 `sp<IGraphicBufferProducer>` 交 `setDisplaySurface` |
| `AImageReader_acquireLatestImage` / `acquireNextImage` | 取一帧 |
| `AImage_getPlaneData(img, 0, &data, &len)` | **RGBA 像素指针** |
| `AImage_getPlaneRowStride(img, 0, &stride)` | 行跨度（像素） |
| `AImage_getHardwareBuffer(img, &ahb)` | **零拷贝给 NPU 的入口** |
| `AImage_delete(img)` | 释放（归还 buffer 池） |

- `usage`：CPU 读用 `AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | GPU_COLOR_OUTPUT`；纯给 NPU 用 `GPU_SAMPLED_IMAGE | GPU_COLOR_OUTPUT`。
- **优点**：buffer 池复用、按需 acquire，适合 30–120fps 持续喂模型；帧率上限 = 面板刷新率。
- **缺点**：要管虚拟显示生命周期与旋转/尺寸变化（复用已有的整层重建逻辑）。

#### B.2 `captureDisplay` 按帧拉取（按需抓一帧）

**实测 `libgui.so` 导出的同步可用入口**（Android 15 / OPlus）：

```
ScreenshotClient::captureDisplay(const gui::DisplayCaptureArgs&, const sp<IScreenCaptureListener>&)
ScreenshotClient::captureLayers (const gui::LayerCaptureArgs&, const sp<IScreenCaptureListener>&, bool)
```

> ⚠️ 网传的 `ISurfaceComposer::captureDisplay(..., sp<GraphicBuffer>* out)` **签名不成立**。A15 是 **AIDL + 异步 listener**：结果放进 `gui::ScreenCaptureResults` 回调给 `IScreenCaptureListener`。要同步就自己实现 `BnScreenCaptureListener` 子类（`libgui` 导出了 `Bn/BpScreenCaptureListener`）配 condition_variable 等回调——AOSP 的 `ScreenCaptureListener::create()` / `waitForResults()` 就是干这个的。

```cpp
gui::DisplayCaptureArgs args;
args.displayToken = mainToken;              // 主屏 token
args.width = W; args.height = H;            // 可小于屏幕：SF 用 GPU 直接缩放到这个尺寸
args.pixelFormat = PIXEL_FORMAT_RGBA_8888;
ScreenshotClient::captureDisplay(args, listener);
auto& r = /* wait 回调 */;
sp<GraphicBuffer> buf = r.buffer;           // ← RGBA_8888；r.fence 用于同步
```

- **优点**：无虚拟显示生命周期；帧率完全自控；`captureLayers` 可精确挑层 / **排除我们自己的 ImGui 图层**；`args.width/height` 能让 SF 顺手把缩放做掉。
- **缺点**：每帧一次 Binder 往返 + 一块 buffer 分配，比 B.1 贵。
- **适合**：几十 fps 的推理、事件触发的按需抓帧。>60fps 连续抓帧优先 B.1。

#### B.3 从 GraphicBuffer 拿像素 / 交给 NPU

```cpp
void* bits = nullptr; int32_t stride = 0, fmt = 0;
buf->lock(AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, &bits, &stride, &fmt);
// bits 即 RGBA 像素；stride 单位是像素，行偏移 = y * stride * 4
buf->unlock();

AHardwareBuffer* ahb = buf->toAHardwareBuffer();   // 零拷贝给 NPU
```

**数据布局（关键，别记错）**：

- 每像素 **4 字节**，字节序 **R → G → B → A**（小端下按 `uint32` 读是 `0xAABBGGRR`）；
- **`stride` 常大于 `width`**（对齐 padding）→ **行偏移必须用 `stride × 4`，不能假设 `width × 4`**。这是"图斜了/花屏"最常见的原因；
- 面板切到 HDR / DISPLAY_P3 时格式可能变 `RGBA_1010102` 或带色彩转换，**用前读 `fmt` 和 dataspace**。

### 路线 C：`mirrorDisplay` 一键镜像（本机独有，需实测）

本机 `libgui.so` 导出 `SurfaceComposerClient::mirrorDisplay(DisplayId)`（上游 AOSP 不一定有，**这台 OPlus 的定制 libgui 里确认有**，AIDL 侧签名 `ISurfaceComposerClient::mirrorDisplay`）。理论上能一步拿到"镜像某个 display"的 SurfaceControl。

- **优点**：最省事，语义就是"镜像"。
- **缺点**：**不可移植**（厂商定制/上游差异），返回对象如何使用需要实测确认。
- **定位**：可选优化，不当主线。

### 路线 D：直接 exec `/system/bin/screenrecord`

最省事：起进程、读它的输出或让它写文件。

- **限制**：它自己有 **3 分钟时长上限**、输出固定是 **MP4 文件**、参数不可编程、无法流式拿到每帧。
- **定位**：快速验证 / 临时兜底；不适合产品化。

### 路线 E：其它开源方案（结论：都不如 A）

| 方案 | 原理 | 判断 |
|---|---|---|
| **minicap** | 读 framebuffer / 调 screencap | 老、慢、依赖 fb0，**别用** |
| **sndcpy / droidVNC-NG / 官方录屏** | MediaProjection | **正是要避开的 App 权限路线**，排除 |
| **scrcpy** | shell + VirtualDisplay | **当前最优范式**，纯 C++ 直接照搬 screenrecord |
| **screenrecord** | shell + VirtualDisplay（纯 C++） | **我们的一号模板** |

---

## 4. 和现有架构的衔接（我们已经有半个轮子了）

关键洞察：**现有守护进程已经在用 SurfaceFlinger 自建图层渲染 ImGui**（`ShellLayerHost` + Vulkan）。这意味着"shell 进程 + SurfaceFlinger 权限 + 原生渲染"这三块我们都已跑通，**采屏线程直接加进同一个进程即可**，几乎零新增基础设施。

几个必须现在想清楚的点：

1. **会不会把自己的 overlay 录进去？**
   虚拟显示镜像的是**主屏 layer stack（0）**，而我们的 `aimbot-ui` 也是 stack 0 上的根图层 → **默认会被录进去**。
   - 要排除：给 overlay 单独分配一个 layer stack，或改用路线 B 的 `captureLayers` 显式挑层。
2. **`setSkipScreenshot` 的层在"虚拟显示镜像"下还隐不隐身？**
   那个开关是针对**截图/录屏（captureDisplay）**路径的；**对虚拟显示镜像不一定生效** → 需实测。这决定"录屏能不能录到我们的菜单"。
3. **GPU 竞争**：编码在 mediaserver，我们只当"搬运工"；但虚拟显示合成会占用 SurfaceFlinger 的 GPU —— 与我们的 ImGui 渲染**共用同一块 GPU**，高帧率录屏时要留意互相抢。
4. **权限**：现有 shell uid 已够，**不需要 root**（有也无害）。

---

## 5. 风险 / 待验证清单

| # | 问题 | 影响 | 验证方式 |
|---|---|---|---|
| 1 | `createVirtualDisplay` 在 ColorOS 上不被拦 | 决定路线 A 能否成立 | 守护进程里实测建层 |
| 2 | 非 root 下 `secure=false` 是否被要求 `secure=true`（A11+ 起 shell 建 secure display 受限） | 影响能否建成功 | 实测 |
| 3 | `setSkipScreenshot` 层在镜像下是否隐身 | 决定菜单会不会被录进去 | 录一段回放看 |
| 4 | 原生 MP4 封装 | MediaMuxer 是 Java-only；原生要用 `MPEG4Writer`(libstagefright) 或自写 | 若只要码流可先不做 |
| 5 | DRM 保护内容 | 受保护层不会出现在镜像里（与 MediaProjection 一致） | 已知局限 |
| 6 | 路线 C `mirrorDisplay` 返回对象语义 | 厂商定制，可能变化 | 不阻塞主线 |

---

## 6. 建议的下一步（分三步走，每步都可独立验证）

1. **P0 · 证明"能拿到屏"**：在守护进程里 `dlopen libgui`，`createVirtualDisplay` + `setDisplayLayerStack(主屏)`，把输出接到一个我们自己建的 `ANativeWindow`（比如复用现在那套 SurfaceFlinger 图层）→ **眼睛能看到屏幕内容**即成功。
2. **P1 · 接入编码**：`MediaCodec::createInputSurface` → `configure/start` → 循环 `dequeueOutputBuffer` → 落盘 **裸 H.264**（先不做容器，用 ffplay 验证）。
3. **P2 · 定容器/推流**：本地录屏 → 原生 MP4（MPEG4Writer）或自写；或照 scrcpy 走 socket 推流。

---

## 附：一句话记住的原理

> **SurfaceFlinger 会把主屏的所有图层合成好；我们只要建一块"图层栈和主屏一样"的虚拟显示，把它的输出口直接插到编码器的输入口上——帧从合成器直接流进硬件编码器，全程不经过我们的进程。**
