# daemon 崩溃根因分析 + 零拷贝推理链路修复方案

日期：2026-09-18
设备：一加 `1ddb632b`（ColorOS / A15 / SM8650 / Adreno 750）
证据：`tombstones_pulled/tombstone_30`、`tombstone_31`（均从 `/data/tombstones` 现拉）
       `tombstones_pulled/tombstone_probe0`（我方隔离复现）

---

## 一、崩溃栈

`tombstone_30`（12:35:44）与 `tombstone_31`（12:41:04）**完全同型**：

```
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0000000000000004
Cause: null pointer dereference
    x0  = 0x1            <- 传进 incStrong 的 this 指针 = 1（垃圾）
    x1  = 0x4            <- 要 ldadd 的目标地址
    pc  = libutils.so+0x17b80   __aarch64_ldadd4_relax
```

```
#00 __aarch64_ldadd4_relax+16                     libutils.so
#01 android::RefBase::incStrong(void const*) const+32   libutils.so
#02 android::nativeWriteToParcel(_JNIEnv*, _jclass*, long, _jobject*)+80   libandroid_runtime.so
#03 art_jni_trampoline
#04 android.view.Surface.writeToParcel+276
#05 android.os.Parcel.writeTypedObject+132
#07 android.hardware.display.VirtualDisplayConfig.writeToParcel+54
#08 android.os.Parcel.writeTypedObject+132
#10 IDisplayManager$Stub$Proxy.createVirtualDisplay+36
#12 DisplayManagerGlobal.createVirtualDisplay+38
#14 DisplayManager.createVirtualDisplay+64
#22 io.github...aimbotnextgen.shell.ScreenCapture.start+1190     <- 真凶现场
#24 ShellServerEntry.startCaptureSupervisor$lambda$15+320
#26 ShellServerEntry.$r8$lambda$250XDb-...
#28 ShellServerEntry$$ExternalSyntheticLambda7.run
#29 java.lang.Thread.run
```

崩溃线程是 `aimbot-capture-`（`HandlerThread("aimbot-capture")`），
进程存活仅 12–15s ⇒ 每次启动后第一次建虚拟显示就死。

### 指令级确认

从设备拉 `libutils.so`，`pc = 0x17b80` 处：

```
0x17b80:  b8200020     ldadd  w0, w0, [x1]      <- x1 = 0x4 = 崩溃地址
0x17b84:  d65f03c0     ret
```

`incStrong`（`0x152a0`）反汇编：`mov w0,#1` → `mov x1,x19`（arg）→ `bl 0x17b80`。
即它是把 `RefBase* this` 直接当 `ldadd` 目标传下去，`this` 为 1 ⇒ 撞 0x4。
**没有任何分支会检查 this 是否为空**——只要传进来的是垃圾就必崩。

---

## 二、根因

### 2.1 直接原因：Surface 的 native 句柄已失效

`DisplayManager.createVirtualDisplay(...)` 在 A15 上走：
`VirtualDisplayConfig.writeToParcel` → 其中 `Surface.writeToParcel` →
`nativeWriteToParcel` → `incStrong` 那个 `sp<Surface>` 暴露出来的 native 对象。

崩在 `incStrong`，`this` 是 1，说明**包里那个 Surface 的 mNativeObject 已经指向被释放/无效的内存**。

### 2.2 谁让句柄失效 —— `native_reader.cpp` 的所有权错位

关键代码 `native_reader.cpp:134-155`：

```cpp
if (AImageReader_getWindow(g_reader, &g_window) != 0 || !g_window) { ... }
jobject surf = nullptr;
if (g_createSurface) surf = g_createSurface(env, g_window);   // android_view_Surface_createFromANativeWindow
else                 surf = createSurfaceViaReflection(env, g_window);
...
g_surface = env->NewGlobalRef(surf);      // <-- 把 Surface jobject 长期攥着
```

两处叠加出问题：

**(a) `android_view_Surface_createFromANativeWindow` 的引用语义在 A15 上变了。**
该隐藏函数本应「把 ANativeWindow 的强引用转交给新的 Java Surface」（历史上它自己 `incStrong` 一次）。
但 `AImageReader_getWindow` 在这台机器的 `libmediandk` 上**不会** `incStrong`（09-18 上午已实测：反汇编 `0x325b4` 只做 `str x8,[x5]` 把 `reader[0x88]` 拷出来）。
⇒ 我们以为拿到一个 ref 的 `ANativeWindow*`，其实**从来没持有过 ref**。

**(b) 我们把 `Surface` jobject 存成 global ref 长期持有，而它背后的 native 窗口归
AImageReader 的 `mSurfaceTexture` / `mNativeWindow` 两个 `sp<>` 所有。**
一旦 `stop()` 走 `AImageReader_delete`，那两个 `sp<>` 析构，SurfaceTexture 被释放，
但我们手里的 Java `Surface` 对象**仍然缓存着旧指针**。此时若任何人再对它
`writeToParcel`（或 `createVirtualDisplay` 复用），`incStrong` 就在释放内存上做。

### 2.3 触发时序

```
startCaptureSupervisor 循环
  → ScreenCapture.start()
      → stop()                     // 先拆旧的：AImageReader_delete ➜ SurfaceTexture 释放
          runCatching { virtualDisplay?.release() }
          nativeDestroyCaptureReader()          // ★ 内存在这里被释放
      → nativeCreateCaptureReader()             // 建新的 reader
      → dm.createVirtualDisplay(..., surface)   // ★ 对「已释放内存」的 Surface 做 writeToParcel
                                                 //    => nativeWriteToParcel -> incStrong -> SIGSEGV
```

`start()` 第 130 行的「已在跑就直接返回」分支一旦因尺寸不一致落到 `stop()` + 重建
（`needsRestart()` 轮询屏幕形状），就会踩中上面这条路径。
设备 uptime 只有 12s ⇒ 就是冷启动 supervisor 的首轮重建。

### 2.4 隔离复现（铁证）

写了一个只做反射的最小程序（`SurfaceReflectProbe.java`），
构造一个 `mNativeObject` 被写成 `0x4` 的 `Surface`，调它的 `writeToParcel`：

```
fresh Surface mNativeObject = 0x0
writeToParcel(handle=0) -> returned normally     <- 句柄为 0 时框架自己挡住了
Segmentation fault                                <- 句柄为垃圾非 0 时崩
```

对应 tombstone（`tombstone_probe0`）：

```
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x000000000000000c
    x21 = 0x4
#00 android::RefBase::incStrong(void const*) const+16   libutils.so       (BuildId 90817f...c65c 同)
#01 android::nativeWriteToParcel(...)+80                 libandroid_runtime.so (BuildId f6ece2... 同)
#04 android.view.Surface.writeToParcel+276
```

**同一 `libutils.so` BuildId、同一 `+80` 偏移、同一 `incStrong` 帧**，与 daemon 崩溃完全同构。

### 2.5 一句话结论

> 崩溃不是「虚拟显示建不起来」，而是**我们把一个 Java `Surface` 的 global ref 攥过了它底层
> `SurfaceTexture` 的寿命**；`stop()` 释放 AImageReader 后，`start()` 又把那个「壳还活着、
> 内核已死」的 Surface 递给 `createVirtualDisplay`，parcel 时 `incStrong` 撞垃圾指针。

顺带纠正一个认知：`AImageReader_getWindow` 在这台机器上**不加引用**，
所以「getWindow 给的 ANativeWindow 我持有 ref」这个前提本来就是错的
（与 09-18 上午 `natcap::destroy` 那条 SIGSEGV 同源，当时只删了 `ANativeWindow_release`，
没解决更根本的 **Surface jobject 寿命** 问题）。

---

## 三、修复方案（面向「0 拷贝推理链路」）

### 3.0 设计约束（先认清边界，避免做无用功）

已确认的硬约束：

| 约束 | 值 | 后果 |
|---|---|---|
| 模型输入 | 恒为方阵 `[1,S,S,3]`，S=256/640 | **形状**不匹配 |
| 模型 dtype | fp32 / int8 / uint8 | **dtype** 不匹配 |
| 截图格式 | RGBA_8888，4 通道 | **通道数**不匹配 |
| 截图尺寸 | 屏幕分辨率（2400×1080 级） | 还得裁剪+缩放 |

⇒ **「AHB 直接当模型输入」在物理上不成立**（没有任何引擎能吃 RGBA_8888 全屏方阵当 `[1,S,S,3]`）。
所以「0 拷贝」的正确目标不是「消灭所有 memcpy」，而是：
**让数据一次都不经过 CPU 地完成「裁剪 + 缩放 + 通道重排」，落成模型能吃的那张张量。**

### 3.1 总体思路：把变换搬进 GPU，CPU 只碰模型输入那一张

```
现状（5 段 CPU 拷贝，≈8.4 MB/帧）：
  SF 合成全屏(10.4MB) → pushFrame 裁剪(1.64MB) → copyLatestInto(1.64MB)
  → Session::submit(1.64MB) → worker slot→local(1.64MB) → letterbox(读1.64/写0.2)

目标（CPU ≈0.2 MB/帧，只动模型输入）：
  SF 合成 → [GPU: crop + scale + RGBA→RGB] → AImageReader(尺寸=S×S, 格式=模型可直接吃)
           → Vulkan/EGL 预处理(已在跑, 1.22ms) → 模型输入张量
```

关键洞察（来自 `screen-capture.md`）：**GPU 预处理已经默认开启且只要 1.22ms**
（vs CPU 7.16ms）。所以要做的不是「加一个 GPU 阶段」，而是**把裁剪和缩放也交给它**，
让后面 4 段全帧 memcpy 自然消失。

### 3.2 分三步落地

#### S1（立即，修崩溃 + 止血）—— 修正 Surface 生命周期

**这是必须先做的，不做后面全是空谈。**

1. **不再长期持有 `Surface` 的 global ref。**
   `nativeCreate(env,w,h,&outSurface)` 改成：创建 reader → 造 Surface → **把 ownership 交给
   Kotlin 侧一次用完**，native 侧不留 `g_surface`。Kotlin 在 `start()` 里局部用完即随
   局部变量释放；`stop()` 不再去 `DeleteGlobalRef`。

2. **`stop()` 顺序改为「先 release display，再 destroy reader」，且保证不跨代复用。**
   ```
   stop():
     running=false
     virtualDisplay?.release()   // ← 必须最先，display 还引用着 SF 侧
     virtualDisplay=null
     nativeDestroyCaptureReader() // ← SurfaceTexture 在这里才允许释放
   ```
   现有代码正是这个顺序，问题出在 **`start()` 里 `stop()` 之后紧接着又把旧 surface 递给
   `createVirtualDisplay`**；所以第 1 条（不跨代持有）才是根治。

3. **`useNative` 路径彻底不落 `reader`**：`nativeSurf` 用完只做局部引用，不进字段。
   （现在的 `this.reader = null` 是对的，但 `g_surface` 这条 native 全局引用是漏的。）

4. **加一道「代（generation）」守卫**：`native_reader` 内加 `g_gen`，每次 create/destroy 自增；
   `nativeDestroy` 里对 `g_reader` 的操作校验 `gen` 未变，避免 supervisor 快速重建时
   对已 delete 的 reader 再 `setImageListener`（这是并发的第二条同类死路）。

> 做完 S1，崩溃消失，链路回到「5 段拷贝但能跑」。

#### S2（核心，真正拿 0 拷贝）—— 原生虚拟显示 + `setDisplayProjection`

把「裁剪 + 缩放」从 CPU 搬到 SurfaceFlinger（GPU）：

```cpp
// 目标形态（native，绕开 Java 侧整条 parcel 路径）
SurfaceComposerClient::createVirtualDisplay("aimbot-capture",
        /*width=*/ S, /*height=*/ S, /*pixelFormat=*/ RGBA_8888, /*flags=*/ ...);  // A15 签名
SurfaceComposerClient::Transaction t;
t.setDisplayLayerStack(token, 主屏 layerStack);
t.setDisplayProjection(token, ROT_0,
        Rect(cropX, cropY, cropX+side, cropY+side),   // src：屏幕上的裁剪区
        Rect(0, 0, S, S));                            // dst：直接缩放到模型尺寸
t.apply();
```

配套：
- 输出接 **`AImageReader_newWithUsage(S, S, RGBA_8888, GPU_SAMPLED|CPU_READ_RARELY, ...)`**
  ⇒ 一帧只有 S×S×4（256²=256 KB / 640²=1.6 MB），全屏 10.4 MB 那步彻底不再进 CPU。
- `crop` 变化只改 `setDisplayProjection` 的 `srcRect` ⇒ **滑块拖动不再重建 display**，
  比现在 `needsRestart()` 那条重建路径轻得多。
- 这条路径**不经过 `DisplayManager` ⇒ 不存在 `Surface.writeToParcel` ⇒ 那类崩溃根除**。

要注意的坑（已在 `screen-capture.md` 记过）：
- `SurfaceComposerClient` 是 libgui 私有 API，要自行声明 vtable 布局并 `dlopen("libgui.so")`。
- A15 的 `createVirtualDisplay` 签名与旧版不同（多了/换了参数），必须先
  `nm -D libgui.so` 或反汇编把签名钉死。
- token 目前 Java 侧拿不到（`VirtualDisplay` 只暴露 `IVirtualDisplayCallback`），
  这正是**必须走 native** 的原因。

#### S3（GPU 预处理接线）—— 让 preprocess 直接吃 S2 的输出

S2 之后，`AImageReader` 的 AHB 已经是「模型尺寸、RGBA」，
直接喂给已存在的 **GPU 预处理**（双槽 + 延迟读 + per-slot fence，1.22 ms）：

```
AHB(S×S, RGBA) --eglCreateImageKHR/EGLImage--> GPU 采样 --> GL_RGB 纹理
                                          --> RGBA→RGB + 归一化/量化 一步出张量
```

- 复用 `debug.aimbotng.gpuproc`（默认 1）那条路径，**去掉 `lock+memcpy` 回退**。
- `capturePushAHardwareBuffer` 里那段 `consuming()` 下的 `AHardwareBuffer_lock + pushFrame`
  **整段删除**——它是纯 preview 兼容代码，却是当前 CPU 拷贝的大头之一。
- 通道重排：RGBA→RGB 在 shader 里做（`tex.rgb`），零额外成本。
- 量化：int8/uint8 模型直接在 shader 里做 `*scale+zero_point`，输出 buffer 即模型输入
  ⇒ **CPU 侧只剩「GPU 写完 → NPU 读」的 fence 同步，不碰像素**。

> 到这里，「0 拷贝」在**像素通路**上成立：
> SurfaceFlinger(GPU) → AImageReader(AHB) → GPU 预处理(EGLImage) → 模型输入。
> CPU 只负责 fance/元数据。这正是 `npu_frame_receiving_analysis.md` 里
> `capture_gpu_direct` 那条路线的真实含义（它写的是 YUV420，通道数反而更不可能直喂——
> 所以它必然也带 GPU 预处理，与我们结论一致）。

### 3.3 落地顺序与验收

| 步骤 | 动作 | 验收标准 |
|---|---|---|
| S1 | 去掉 `g_surface` 长期持有 + 加 generation 守卫 | 连续启停 30 次无 tombstone；`/data/tombstones` 不新增 |
| S2a | 反汇编钉死 A15 `libgui.so` 的 `createVirtualDisplay` 签名 | `nm -D` 出来能对上 |
| S2b | native 建虚拟显示 + `setDisplayProjection` | `dumpsys SurfaceFlinger` 能看到 S×S 的 display；ImageReader 收到帧 |
| S2c | ImageReader 尺寸改为模型尺寸 | `AimbotTrace` 里 capture 段带宽骤降；`GC`/memory 压力下降 |
| S3 | GPU 预处理直吃 AHB，删 lock+memcpy | `timingTextDetailed()` 的 pre 段 < 1.5 ms，CPU 总拷贝 ≈0.2 MB/帧 |

**风险与回退**：
- S2 若在 ColorOS 上被 SELinux/私有 API 拦死 ⇒ 回退到 S1 状态（能跑），
  并保留 `DisplayManager` 路径作为 fallback（但把 Surface 生命周期按 S1 修好）。
- GPU 单趟大比例降采样（1080→256）会走样 ⇒ 先降到 2×S 再让 GPU 做第二次 box 采样；
  用 `tools/model_probe/compare.py` 做精度 A/B，不靠感觉。

---

## 四、附：本次取证产物

```
tombstones_pulled/
├── tombstone_30 / tombstone_31      崩溃现场（daemon）
├── tombstone_probe0                 隔离复现（同帧同 BuildId）
├── libutils.so                      用于指令级确认（pc 0x17b80 = ldadd）
├── SurfaceReflectProbe.java         最小复现程序
├── cls/ dexout/                     编译产物
└── dex/framework.jar                用来尝试 dump Surface.writeToParcel
```
