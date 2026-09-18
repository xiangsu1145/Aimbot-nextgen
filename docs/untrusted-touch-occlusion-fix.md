# 从根源修：让 `aimbot-ui` 图层不再被算作"遮挡窗口"

> 前置结论见 `docs/untrusted-touch-occlusion.md`（注入链路正常，触摸被 `InputDispatcher` 判为 untrusted 丢掉）。
> 本文补上**源码级定论** + **样貌零改动的修法** + **本机可自测的三层验证**。
> 日期：2026-09-16

---

## 1. 一句话

`ShellLayerHost` 建的是**裸 SF 图层**，从来没给它填过 `InputWindowInfo`。SurfaceFlinger 在这种情况下会把窗口的 `touchOcclusionMode` 留成默认枚举值 `0 = BLOCK_UNTRUSTED`（AOSP 源码可证，下节）。于是只要图层是**全屏 + alpha=1**，任何从它下面穿过去的触摸（我们的 IM 注入）在 `InputDispatcher` 里就是"不可信触摸"。

修法只有一步：**给图层填一份 `InputWindowInfo`，把 `touchOcclusionMode` 设成 `ALLOW`**。不碰 alpha、不碰 geometry、不碰绘制 ⇒ **画面 100% 不变**。

---

## 2. 源码级证据（不是猜的）

### 2.1 枚举语义 —— `libs/gui/android/gui/TouchOcclusionMode.aidl`（AOSP main）

```aidl
@Backing(type="int") enum TouchOcclusionMode {
    /** Touches that pass through this window will be blocked if they are
     *  consumed by a different UID and this window is not trusted. */
    BLOCK_UNTRUSTED,          // = 0
    /** The window's opacity will be taken into consideration ... */
    USE_OPACITY,              // = 1
    /** The window won't count for touch occlusion rules if the touch passes through it. */
    ALLOW,                    // = 2   ← 我们要的就是这个
    ftl_last=ALLOW,
}
```

### 2.2 值是谁填的 —— `Layer::fillTouchOcclusionMode()`

```cpp
void Layer::fillTouchOcclusionMode(WindowInfo& info) {
    sp<Layer> p = sp<Layer>::fromExisting(this);
    while (p != nullptr && !p->hasInputInfo()) {
        p = p->mDrawingParent.promote();          // 自己没有 input info → 往上找父层
    }
    if (p != nullptr) {
        info.touchOcclusionMode = p->mDrawingState.inputInfo.touchOcclusionMode;
    }
}
```

我们这条图层：**从未 `setInputWindowInfo`** ⇒ `hasInputInfo()==false`；又是 root layer（无父层）⇒ 循环爬到 `nullptr` ⇒ **函数什么都不写**，`WindowInfo` 里那个字段保持默认值 `0` = `BLOCK_UNTRUSTED`。

反向读同一段代码：**只要我们填了 input info，`p` 就是自己，SF 会把我们填的值原样搬进 `info`**。所以 `ALLOW` 这条路是通的（不需要 root、不需要改 SF）。

**但这里还有第二道门 —— 实现时才发现，它才是整个方案的前提：**

```cpp
bool Layer::hasInputInfo() const { return mDrawingState.inputInfo.token != nullptr; }
```

`hasInputInfo()` 判的是 **`token != nullptr`**，不是"设没设过 input info"。我们那条图层是 root layer：`p = this` → `hasInputInfo()==false` → 沿 `mDrawingParent` 往上爬 → 没有父层 → `p == nullptr` → **哪怕我们已经把自己那份 `inputInfo.touchOcclusionMode` 写成 `ALLOW`，它也不会被搬进 `info`**。

⇒ **必须同时给窗口塞一个非空 `token`**（`android.os.Binder()` 就够）。这是"字段都填了却毫无变化"的唯一原因，也是最容易踩空的一步。实测反证：见 §4，加 token 前窗口根本不出现，加 token 后窗口带着 `ALLOW` 出现。

`fillInputInfo()` 末尾相关三行：

```cpp
    info.setInputConfig(WindowInfo::InputConfig::NOT_VISIBLE, !isVisibleForInput());
    info.alpha = getAlpha();
    fillTouchOcclusionMode(info);
    handleDropInputMode(info);
```

注意 `info.alpha = getAlpha()`：**SF 自己把图层 alpha 填给窗口**，我们改不了也不需要改。

### 2.3 dispatcher 侧怎么用

`InputDispatcher::computeTouchOcclusionInfoLocked()`（Android 12 CL 原文摘要）：

> * 若触点在**触摸消费窗口之上**、**不同 UID**、**未被信任**、且 `touchOcclusionMode == BLOCK_UNTRUSTED` 的窗口存在 → 触摸被拦（`hasBlockingOcclusion = true`）。
> * 否则若存在 `USE_OPACITY` 的窗口 → 与其 alpha 和 `maximum_obscuring_opacity_for_touch`（默认 0.8）比较。
> * 否则不拦。
> * `ALLOW`：**"The window won't count for touch occlusion rules"**。

`isTouchTrustedLocked()` 里两条日志分支：

```cpp
    if (occlusionInfo.hasBlockingOcclusion) { ALOGW("Untrusted touch due to occlusion by %s/%d"); return false; }
    if (occlusionInfo.obscuringOpacity > mMaximumObscuringOpacityForTouch) {
        ALOGW("Untrusted touch due to occlusion by %s/%d (obscuring opacity = %.2f, maximum allowed = %.2f) ..."); return false; }
```

### 2.4 现场 dumpsys 逐字印证（`aShellYou__20260916133226.txt` 内嵌的 `dumpsys input`）

```
name=[BBQ] aimbot-ui#31822#31823, id=…, displayId=0, inputConfig=NO_INPUT_CHANNEL,
miuiFlags=…, alpha=1.0, frame=[0,0][1280,2772], globalScale=1.0,
applicationInfo.name=, applicationInfo.token=<null>, touchableRegion=<empty>,
ownerPid=…, ownerUid=2000, …, touchOcclusionMode=BLOCK_UNTRUSTED
```

同文件 810 条 `Untrusted touch due to occlusion by /2000/[BBQ] aimbot-ui#…`。

### 2.5 ⚠️ 由此推翻的两个"想当然"的修法

| 想法 | 为什么没用 |
|---|---|
| "把图层 alpha 降到 0.8 以下就行"（旧项目就是半透明） | 日志里**没有** `obscuring opacity = …` 后缀 ⇒ 走的是 `hasBlockingOcclusion` 硬拦分支，**跟 alpha 无关**。旧项目能用不是因为"半透明"，而是 WM 的 SAW 走 `USE_OPACITY` + 0.8 上限这两条恰好都不命中，而我们的裸 SF 图层走的是 `BLOCK_UNTRUSTED`。 |
| "把图层挖空 / 缩小到菜单矩形" | `touchableRegion=<empty>` 一样被拦 ⇒ 遮挡判定**不看可触摸区域**，只看 z 序 + UID + trusted 标记 + occlusion mode。 |

也就是说：**唯一正确的旋钮就是 `touchOcclusionMode`（外加 `trustedOverlay` 做保险）。**

### 2.6 为什么影破（YingPo）不受影响

`D:\破解\YingPo_RES.apk (5)\classes1` 全量 grep `touchOcclusionMode|setInputWindowInfo|setTrustedOverlay` → **0 命中**。它压根不用 SF 图层，菜单是普通 `WindowManager` 窗口（alpha ≤ 0.8 的 SAW）⇒ 天生走 `USE_OPACITY` 分支且 opacity ≤ 0.8 ⇒ 不拦。老项目同理。**这条差异正好解释了"老项目人人能用、新项目只有 ColorOS 能用"**（ColorOS 的 InputDispatcher 对这条规则宽松/未启用）。

---

## 3. 已实施（方案 A：只改图层）

改动只有一个文件、一个函数：`ShellLayerHost.kt` 里新增 `applyInputWindowInfo(...)`，在 `applyGeometry()` 的**同一个 transaction** 里、`show()` 之前下发。没碰 C++、没碰 App UI、没碰权限声明、没碰 alpha/geometry/绘制。

### 3.1 实际填进去的字段

载体类 = **`android.view.InputWindowHandle`**，构造 `(InputApplicationHandle, int)`（`InputApplicationHandle` 也真造了一个：`(Binder, name, 5000ms)`，避免把 `null` 递进 JNI 转换路径）。

| 字段 | 实测值（OnePlus 15 / ColorOS） | 作用 |
|---|---|---|
| **`token`** | `android.os.Binder()` | ★**前提**：让 `hasInputInfo()` 为真，否则下面全部作废 |
| **`touchOcclusionMode`** | **`2 = ALLOW`** | ★核心：不再计入遮挡规则 |
| `inputConfig` | `0x10d` = `TRUSTED_OVERLAY(0x100)｜NOT_TOUCHABLE(0x08)｜NO_INPUT_CHANNEL(0x04)｜NOT_FOCUSABLE(0x01)` | 第二道保险 + 保证不截获触摸 |
| `layoutParamsFlags` | `0x30` = `FLAG_NOT_TOUCHABLE｜FLAG_NOT_TOUCH_MODAL` | 同上（老版本 dispatcher 读这个字段） |
| `name` / `packageName` | `aimbot-ui` / `io.github.xiangsu1145.aimbotnextgen` | dumpsys 里好认 |
| `ownerUid` / `ownerPid` | `2000` / `Process.myPid()` | 与图层真实身份一致 |
| `displayId` | `0` | 与 `setLayerStack(sc, 0)` 对齐 |
| `frame` | `Rect(0,0,w,h)` | final 字段，就地 `set()` |
| `touchableRegion` | 空（默认） | 保持"完全不截获触摸" |
| `scaleFactor` / `alpha` | `1.0f` / `1.0f` | `0` 会变成零尺寸输入窗口 |
| `canOccludePresentation` | `false` | 不参与呈现遮挡 |
| `contentSize` | `Size(w,h)` | 语义完整 |

⚠️ **常量一律按名字反射解析、不写字面量**：`android.os.InputConfig` 的真实位值实测是 `NOT_FOCUSABLE=0x01 / NO_INPUT_CHANNEL=0x04 / NOT_TOUCHABLE=0x08 / TRUSTED_OVERLAY=0x100` —— 若按"想当然"的顺序猜（`NOT_TOUCHABLE=0x02`）就会设错位。`ALLOW` 同理：从 `android.gui.TouchOcclusionMode.ALLOW` 取（枚举取 `ordinal`），兜底才用 `2`。

> 若某 ROM 连常量容器类都取不到，本实现选择**整段跳过 `inputConfig`**（而不是猜）；此时窗口仍因 `touchableRegion=<empty>` 而不截获触摸、且 `ALLOW` 已经下发，功能不受影响。真要兜底可写死上面这组实测值（`0x01|0x04|0x08|0x100`），它们是 ABI 数值。

### 3.2 容错阶梯（一台不认就退一层，永远不影响画面）

1. 载体类按 `android.view.InputWindowHandle` → `android.window.InputWindowHandle` → `android.window.InputWindowInfo` 顺序找，找不到就整体放弃（只打日志）。
2. 每个字段：先公开字段直接赋值，再 `getDeclaredField`，再同名 `setXxx`；**拒绝写 final 字段**（`frame`/`touchableRegion` 走"就地改内容"）。
3. `Transaction.setInputWindowInfo` / `setTrustedOverlay` 都用"按名字 + 参数类型可赋"匹配重载，`NoSuchMethodException` 只记日志。

### 3.3 日志（远程复测就靠这几行，tag `aimbot_layer`）

```
I aimbot_layer: occlusion ALLOW = 2 (from android.gui.TouchOcclusionMode.ALLOW)
I aimbot_layer: inputConfig = 0x10d (from android.os.InputConfig)
I aimbot_layer: input window: InputWindowHandle[token=android.os.Binder@… name=aimbot-ui … touchOcclusionMode=2 scaleFactor=1.0 alpha=1.0 frame=[0,0,3000,2120]] missing=trustedOverlay
```

`missing=trustedOverlay` 是**预期**的：`InputWindowHandle` 没有 `trustedOverlay` 字段，这个开关是走 `inputConfig` 的 `TRUSTED_OVERLAY` 位实现的。三行齐 = 反射链全部打通。

### 3.4 其它行为确认（不会引入新问题）

- 图层**本来就没有 input channel**（`NO_INPUT_CHANNEL` + 空 region 已是既成事实）⇒ 菜单触摸今天也不是系统派发给它的，而是 daemon 读 `/dev/input` 后自行决定吞/回注。填了 input info 后这一点不变，`touchPass` / `exclusive` 语义不变。
- 不给图层加任何尺寸/alpha/裁剪 ⇒ **样貌逐像素不变**。
- `setSkipScreenshot` 不受影响（同一 transaction 里并行下发）。

---

## 4. 实测（OnePlus OPD2404 / ColorOS 15 / Android 15，2026-09-16）

### 4.1 两条决定性读数

**改前**（旧 APK，图层活着，屏幕已唤醒）：

```
$ dumpsys input | grep -c 'name=aimbot-ui'     →  0
$ dumpsys SurfaceFlinger --list | grep aimbot  →  RequestedLayerState{aimbot-ui#2210 z=2147483647}
```

**改后**（新 APK，同一台机）：

```
0: name=aimbot-ui, id=3774, displayId=0,
   inputConfig=NO_INPUT_CHANNEL | NOT_FOCUSABLE | NOT_TOUCHABLE | TRUSTED_OVERLAY,
   alpha=1, frame=[0,0][2120,3000], globalScale=1,
   applicationInfo.name=aimbot-ui, applicationInfo.token=0xb4000071559ac180,
   touchableRegion=<empty>, ownerPid=16993, ownerUid=2000,
   dispatchingTimeout=0ms, token=0x0, touchOcclusionMode=ALLOW      ← ★
```

三件事一次性证完：① 窗口从"不存在"变成"存在"（⇒ `token` 让 `hasInputInfo()` 为真）；② **`touchOcclusionMode=ALLOW`**（⇒ SurfaceFlinger 采纳了我们的值，而不是它的默认 `BLOCK_UNTRUSTED`）；③ `NOT_TOUCHABLE` + 空 region（⇒ 仍然完全不截获触摸，穿透语义没变）。UI_OFF 后该窗口随之消失（再查 = 0）。

### 4.2 ⚠️ 重大副产物：这台机为什么从来"能用"

ColorOS 的 SurfaceFlinger **根本不给这种裸 buffer 图层发布 input window**（`needsInputInfo()` 退化成 `hasInputInfo()`），所以这台机上**从来就没有遮挡规则可触发** —— 这与"我和少数人能用的原因是 ROM 宽松"其实更精确地是：**ColorOS 压根没把这一层当作输入窗口**。HyperOS（有问题那台）相反：它的 SF 会给 buffer 层生成 input info（现场 log 里那行 `name=[BBQ] aimbot-ui#… touchOcclusionMode=BLOCK_UNTRUSTED` 就是证据）。

⇒ 顺带解释了为什么只有部分人失败。也说明**本机永远复现不出 bug**，只能验"机制生效"（上面 4.1）。

### 4.3 回归读数

| 检查 | 结果 |
|---|---|
| 渲染 | `AimbotNg: fps=9.5/10 peak=10 work≈4ms swapchain rebuilds=0` —— 与改前同量级，绘制没被碰 |
| 注入相关日志 | 无 `LAYER FAIL`、无异常栈 |
| 遮挡告警 | 把系统切成"只警告不拦"（`settings put global block_untrusted_touches 1`）后下拉/上滑真手势：`Untrusted touch` = **0 条** |
| 触摸送达 | 该机锁屏/通知栏占据焦点，焦点变化这条判据不适用（不是触摸没送到）；功能回归留给严格 ROM 上的用户 |

### 4.4 ⚠️ 采集陷阱（会让远程复测误判）

- **`dumpsys input` 在 ColorOS 上是被裁剪的**：非系统窗口（连前台游戏自己的窗口）根本不列。**别用它判断"窗口有没有出现"**，要用 `dumpsys SurfaceFlinger --list` 或 `logcat` 的 `aimbot_layer` 三行。HyperOS 会列（现场 log 证明），所以给用户的命令照旧可用。
- `token=0x0` 是 dispatcher 侧的打印值：窗口没有 input channel，它永远不会成为触摸目标，这个字段取 0 不影响行为（SF 侧的 `inputInfo.token` 是另一个东西，见 §2.2）。
- 本机验证时 `UI_ON` 是我从 socket 直接驱动的，没走 App 的几何下发 ⇒ 图层建成了 `2120x3000` 竖屏尺寸。**正常流程（App 点"启动菜单"）会先发几何**，不受影响。

---

## 5. 给失败用户的复测包（新 APK）

一条命令（安卓终端里直接粘）：

```bash
logcat -d | grep -iE "aimbot|ShellManager|Untrusted touch|Dropping untrusted"
```

外加两条抽样（终端里跑，AimbotNG 的 shell 权限就够）：

```bash
settings get global block_untrusted_touches
dumpsys input | grep -A1 "aimbot-ui"
```

**期望读数（HyperOS 那类机）**：

| 看什么 | 修好 | 没修好 |
|---|---|---|
| `aimbot_layer: input window: InputWindowHandle[… touchOcclusionMode=2 …]` | 有这一行 | 没有该行 / 有 `unavailable` |
| `dumpsys input` 里的 `aimbot-ui` | `touchOcclusionMode=ALLOW` | `touchOcclusionMode=BLOCK_UNTRUSTED` |
| `Untrusted touch due to occlusion by … aimbot-ui` | 消失（0 条） | 照旧成百上千条 |
| `inject tick(im)` 的 `ok/fail` | `fail=0`，且手势能落到游戏 | 事件进了 dispatcher 又不见 |

**测试姿势很重要**（上一份 log 里注入的目标其实是**前台的本 App MainActivity**）：

- 要验"注入能不能用" → **让游戏在前台**再点，别在本 App 的界面里点。
- 复测前最好 `settings delete global block_untrusted_touches`，别让上轮留下的 `1` 掩盖真实结论。
- ColorOS/一加那类机（例如作者本机）`dumpsys input` **不列非系统窗口**，`touchOcclusionMode` 这一栏看不到 —— 那不是失败，换 `dumpsys SurfaceFlinger --list | grep aimbot` + `aimbot_layer` 那三行判。

---

## 6. 兜底阶梯（万一某 ROM 的 SF 不认我们填的值）

1. **已内置**：同一版一起下发 `TRUSTED_OVERLAY`（"不可信遮挡"和"透明叠加"两条路一起打掉）。
2. `dumpsys` 仍显示 `BLOCK_UNTRUSTED` → 看 `aimbot_layer` 日志确认命中哪个载体类，必要时在 `HANDLE_CLASSES` 里换顺序/补一个类名。
3. 仍不行 → 走设置：`settings put global block_untrusted_touches 0`（`1` 只是不拦但仍记录，`2` 是真拦）。
4. **保底（会改观感，尽量不用）**：把图层 alpha 降到 0.79 —— 只要 SF 给了 `USE_OPACITY` 就一定放行，但画面会变，仅当 1–3 全失效时启用。

---

## 7. 落地状态

| 文件 | 改动 | 状态 |
|---|---|---|
| `app/.../shell/ShellLayerHost.kt` | +`applyInputWindowInfo()`（约 200 行：字段表 + 常量解析 + 反射阶梯 + 日志），在 `applyGeometry` 同一 transaction 内、`show()` 之前下发 | ✅ 已实施并实测 |
| `tools/daemon_say.py` | 新增：从 PC 经 `adb forward` 直接给 daemon 发一行命令（用于不经过 App UI 的本机验证） | ✅ 新增 |

未改：C++、App UI、权限声明、`InputManagerInjector`、`ShellServerEntry` 命令处理。

APK：`app/build/outputs/apk/debug/app-debug.apk`（2026-09-16 14:22 构建，已装到一加测试机验证）。

