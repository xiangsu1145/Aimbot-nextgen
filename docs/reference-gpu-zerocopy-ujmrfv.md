# 参考实现：ujmrfv 的 GPU 零拷贝截图链路（逆向）

> 来源：`D:\破解\New World_1.0.2_at_decrypted\lib\arm64-v8a\libQnncpu.so`
> md5 `250c5ba9645a1a772f28f001069b7ed3`
> 逆向日期 2026-09-18。目标：把它的 `capture_gpu_direct` 链路完整复刻到本项目。

## 结论一句话

AHB 经 `eglGetNativeClientBufferANDROID` → `eglCreateImageKHR` →
`glEGLImageTargetTexture2DOES` 绑成 `GL_TEXTURE_EXTERNAL_OES`，再由一个
fragment shader 一次性完成 **裁剪 + 缩放 + RGB 重排 + 量化**。
从 AHB 到模型输入全程 CPU 不碰内存。

**全部 API 都是公开的 EGL/GLES 扩展（API 26+ 全设备可用），没有一处私有 API。**

---

## 一、扩展能力探测（`sub_5164A0`，0x5164A0）

一次性初始化。**四个扩展必须全部存在**，缺一个就整条 GPU 直通不可用，
降级到 CPU 路线（`capture_cpu_direct`）：

```c
off_5CECC8 = eglGetProcAddress("eglCreateImageKHR");
off_5CECB8 = eglGetProcAddress("eglDestroyImageKHR");
off_5CECC0 = eglGetProcAddress("eglGetNativeClientBufferANDROID");  // ⭐
off_5CECD8 = eglGetProcAddress("glEGLImageTargetTexture2DOES");     // ⭐
if (!任何一个是 0)  ->  标记不可用
```

这解释了它的 UI 为什么有 `capture_gpu_direct` / `capture_cpu_direct` /
`capture_system_gpu_direct` / `capture_system_cpu_direct` 四个选项——
运行时探测决定给用户看到哪一个。

---

## 二、核心：AHB → 外部纹理（`sub_517254`，0x517254）

```c
Status ahbToExternalTexture(AHardwareBuffer* ahb, FrameSlot* out) {
    if (!ahb || !out) return 0;
    out->valid = 0; out->tex = 0;
    if (!zcInitOnce()) return 0;                    // ① 四扩展探测
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) return 0;   // ② 挂 context

    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(ahb, &d);              // ③ 宽高
    if (d.width < 1 || d.height < 1) goto fail;

    // ④ AHB -> EGLClientBuffer（不拷贝）
    EGLClientBuffer cbuf = eglGetNativeClientBufferANDROID(ahb);
    if (!cbuf) goto fail;

    // ⑤ EGLImage 属性：PRESERVED + NONE
    EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR /*0x30D2*/, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT,
                                        EGL_NATIVE_BUFFER_ANDROID /*0x3140*/,
                                        cbuf, attrs);
    if (!img)                                        // ⑥ 用当前 ctx 重试一次
        img = eglCreateImageKHR(g_dpy, g_ctx, EGL_NATIVE_BUFFER_ANDROID, cbuf, attrs);
    if (!img) goto fail;

    // ⑦ 建纹理并设好参数
    GLuint tex; glGenTextures(1, &tex);
    while (glGetError()) {}                          // 清干净
    glBindTexture(GL_TEXTURE_EXTERNAL_OES /*36197*/, tex);
    glTexParameteri(36197, GL_TEXTURE_MIN_FILTER,  GL_NEAREST /*9728*/);
    glTexParameteri(36197, GL_TEXTURE_MAG_FILTER,  GL_NEAREST);
    glTexParameteri(36197, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE /*33071*/);
    glTexParameteri(36197, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // ⑧ 零拷贝绑定
    glEGLImageTargetTexture2DOES(36197, img);
    if (glGetError()) {
        // ⑨ fallback：一部分驱动只支持绑到 GL_TEXTURE_2D
        glBindTexture(GL_TEXTURE_2D /*3553*/, tex);
        ... 同样的四个 texParameter ...
        glEGLImageTargetTexture2DOES(3553, img);
        if (glGetError()) goto fail;                 // 两条都失败 -> 放弃
    }

    // ⑩ 成功
    out->valid = 1;
    out->tex   = tex;
    out->w     = d.width;
    out->h     = d.height;
    out->image = img;      // 必须留着，destroy 时 eglDestroyImageKHR
    out->ahb   = ahb;      // 必须留着，destroy 时 AHardwareBuffer_release
    return 1;

fail:
    glDeleteTextures(1, &tex);
    off_5CECB8(g_dpy, img);   // eglDestroyImageKHR
    out->valid = 0;
    return 0;
}
```

### 两个容易漏的细节

1. **`eglCreateImageKHR` 调两次**。先传 `EGL_NO_CONTEXT`，失败再传当前 context。
   不同驱动（Mali vs Adreno）对这个参数的要求不一致，这是兼容性兜底。
2. **`GL_TEXTURE_2D` fallback**。主流是 `EXTERNAL_OES`，但驱动有差异，
   两条路都试过才放弃。

---

## 三、Shader：一次完成全部预处理（0x235b92，1048 字节）

```glsl
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
precision highp samplerExternalOES;

uniform samplerExternalOES uTex;   // AHB 绑上来的外部纹理
uniform vec4  uCrop;               // 裁剪矩形 (x, y, w, h)，源坐标系
uniform vec2  uOutSize;            // 输出尺寸 = 模型输入尺寸
uniform vec2  uTexSize;            // 源纹理尺寸
uniform float uScale;              // 量化 scale
uniform float uZeroPoint;          // 量化 zero point
uniform int   uUint8;              // 1=uint8, 0=int8
out vec4 fragColor;

float encodeQuant(float qf) {
  if (uUint8 != 0) {
    int t = int(clamp(qf, 0.0, 255.0));
    return float(t) / 255.0;
  }
  int t = int(clamp(qf, -128.0, 127.0));
  int ub = t < 0 ? t + 256 : t;    // 负数补码 -> 无符号，存进 8bit 纹理
  return float(ub) / 255.0;
}

void main() {
  ivec2 gid = ivec2(gl_FragCoord.xy);
  if (gid.x >= int(uOutSize.x) || gid.y >= int(uOutSize.y)) discard;

  // 输出像素 -> 源坐标（整数运算，避免浮点误差导致采样偏移）
  int sx = int(uCrop.x) + gid.x * int(uCrop.z) / int(uOutSize.x);
  int sy = int(uCrop.y) + gid.y * int(uCrop.w) / int(uOutSize.y);

  vec2 uv = (vec2(float(sx), float(sy)) + 0.5) / uTexSize;
  vec3 rgb = texture(uTex, uv).rgb;        // 外部纹理采样，硬件线性/最近邻

  vec3 qf = rgb / uScale + uZeroPoint;     // 反量化到实数域
  vec3 enc = vec3(encodeQuant(qf.x), encodeQuant(qf.y), encodeQuant(qf.z));

  fragColor = vec4(enc, 1.0);              // 输出 RGB（丢掉 A）
}
```

### 另有一个不量化的简化版（0x2364c2，526 字节）

少了 `uScale` / `uZeroPoint` / `uUint8`，直接 `fragColor = texture(uTex, uv);`
—— 用于 fp32 模型或只做预览。

### 还有一个"颜色匹配"变体（0x2366d0，968 字节）

在 shader 里做逐像素颜色容差比对（`uTargets[16]` + `uFuzz`），
返回是否命中。说明他们把**颜色筛选也塞进 GPU** 了。

---

## 四、关键常量速查

| 常量 | 值 | 说明 |
|---|---|---|
| `EGL_NATIVE_BUFFER_ANDROID` | `0x3140` | AHB 作为 EGL native buffer |
| `EGL_IMAGE_PRESERVED_KHR` | `0x30D2` | 保留内容 |
| `GL_TEXTURE_EXTERNAL_OES` | `36197` | 能吃 EGLImage 的外部纹理 |
| `GL_TEXTURE_2D` | `3553` | fallback 目标 |
| `GL_NEAREST` | `9728` | 采样过滤（性能优先） |
| `GL_CLAMP_TO_EDGE` | `33071` | wrap 模式 |

---

## 五、导入符号清单（可直接对照实现）

取屏侧：
```
AImageReader_acquireNextImage / AImage_getTimestamp
AImage_getWidth / AImage_getHeight
AImage_getHardwareBuffer / AHardwareBuffer_acquire / AHardwareBuffer_release
AImageReader_setImageListener        <- 回调模式，与本项目一致
ANativeWindow_fromSurface / ANativeWindow_release
```

GPU 侧：
```
eglMakeCurrent / eglCreateContext / eglCreateWindowSurface
eglCreatePbufferSurface / eglGetProcAddress / eglGetCurrentContext
glEGLImageTargetTexture2DOES / eglCreateImageKHR / eglDestroyImageKHR
eglGetNativeClientBufferANDROID
glGenTextures / glBindTexture / glTexParameteri / glDeleteTextures
glGenVertexArrays / glBindVertexArray / glBufferData / glVertexAttribPointer
glCreateShader / glShaderSource / glCompileShader / glCreateProgram
glUseProgram / glDrawElements / glViewport / glScissor
```

推理侧：
```
TfLiteInterpreterGetInputTensor / TfLiteInterpreterInvoke
TfLiteInterpreterOptionsAddDelegate
```

**注意：导入表里没有 `AHardwareBuffer_lock`。**
说明 GPU 直通模式下它完全不 map 内存 —— 这是零拷贝的直接证据。

---

## 六、对本项目的行动项

1. **修崩溃优先**。当前 tombstone_05 是 `ImageReader-300` 线程的
   use-after-free，根因是 app 退出导致 display 被系统释放。GPU 链路再快，
   进程死了也没意义。
2. 修好之后按本文档实现 GPU 预处理：
   - 新增 `capture/egl_bridge.{h,cpp}`：四扩展探测 + AHB→纹理
   - 新增 `capture/gpu_preprocess.{h,cpp}`：EGL context + FBO + shader
   - 输出接 `AhbMailbox`，推理侧读 AHB
3. **保留 CPU 回退路径**。四个扩展缺任一就降级，这与 ujmrfv 的做法一致，
   也是"能适配设备"的要求。
