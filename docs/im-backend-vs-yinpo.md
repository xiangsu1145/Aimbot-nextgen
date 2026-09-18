# InputManager 后端：本项目 vs 影破（com.yingP.runtime.m625）

对比对象：
- **本项目**：`app/src/main/java/io/github/xiangsu1145/aimbotnextgen/inject/InputManagerInjector.kt`（527 行）
  + native 驱动 `app/src/main/cpp/input/uinput_inject.cpp`、桥接 `input_jni.cpp:751-940`
- **影破**：`D:\破解\YingPo_RES.apk (5)\classes1\com\yingP\runtime\m625\service\TouchInjectionServer.java`（6123 行）

---

## 一句话结论

**两条路在"把 MotionEvent 交给 InputDispatcher"这一个动作上是一样的，但除此之外几乎处处不同。**
影破写的是**拟真注入**（真设备 id、仿真压力与触摸面积、DOWN 前清场、方法解析多路回退、运行时时间线），
本项目写的是 **scrcpy 式裸注入**（deviceId 恒 0、pressure/size 恒 1、无清场、只认一个方法签名、只有一句 lastError）。

更需要正视的是**接入层**：影破有"只注入、不读面板"（inject-only）模式，
本项目把 IM 初始化排在了 `readerInit` 硬门之后 —— 面板读不到的设备，连 IM 都拿不到。

---

## 一、总表

| # | 维度 | 本项目 | 影破 | 影响 |
|---|---|---|---|---|
| 1 | **句柄路线** | 反射 `android.hardware.input.InputManager`（Java 包装类） | `ServiceManager.getService("input")` → `IInputManager$Stub.asInterface` | 影破少一层包装，不受包装类变更影响 |
| 2 | **实例获取** | `getInstance()`，退路 `Context.getSystemService("input")` | 纯 binder proxy，不需要 Context | 本项目退路要拉起 ActivityThread 机制 |
| 3 | **方法解析** | 只认 `injectInputEvent(InputEvent, int)` | 二参优先 → 扫名字含 `inject` 的方法 → `supportsInjectMethod` 筛 | ★ AOSP 已注二参版"可能被移除"，本项目会**初始化即失败** |
| 4 | **参数填充** | 只支持 int（写死 mode=0） | int/boolean/long 全支持；名含 `ToTarget` 且位置 2 → `-1` | 本项目遇到 `injectInputEventToTarget` 形态直接无解 |
| 5 | **deviceId** | **恒 0** | `detectPhysicalRealTouchDeviceId()`：找 `sources & 0x1002` 的真触摸屏取 id | 拟真度差异；影破让事件看起来来自真面板 |
| 6 | **source** | 作 `obtain()` 第 13 参传 `SOURCE_TOUCHSCREEN` | 同样传 `0x1002` **并额外 `setSource(0x1002)`** | 冗余保险，无功能差 |
| 7 | **pressure** | **恒 `1f`** | `humanizedProgramPressureLocked()`：0.52~0.92 间随时间/位移正弦波动 | 满压是最"机器味"的特征 |
| 8 | **size / touchMajor / touchMinor** | `size = 1f`，**major/minor 不设（0）** | `size = f(p)*0.33+0.05`，`major = 35*f(p)+20`，`minor` 联动 | 同上，拟真度差异 |
| 9 | **DOWN 前清场** | **无** | `injectVerifierCleanupBeforeDownIfNeeded()`：SDK ≥ 36 时先注入一个 ACTION_CANCEL | ★ Android 16 起注入流首帧 DOWN 的残留状态问题 |
| 10 | **返回值判断** | `accepted != true` 即视为拒绝 | `Boolean`→booleanValue；`Number`→`!=0`；**其他一律当成功** | 本项目对非 Boolean 返回会误判 |
| 11 | **pointer id** | 从**活列表**推导最小空闲，取不到返回 -1（放弃本帧） | `stableMotionIdMap` 持久映射 + 显式 release；满 16 时**回退 0**（会撞） | 本项目更严谨；影破 id 跨帧更稳定 |
| 12 | **驱动方式** | native `upload()` 内同步调 `pushFrame` | 独立分发线程 + dirty 标记 | 本项目与 reader/render 线程同频 |
| 13 | **前置依赖** | **必须先 `readerInit` 成功**（`ShellServerEntry.kt:947` 的 `return` 早于 `:977`） | `setInjectOnlyMode(true)` → 停掉所有 physicalReaders，纯程序注入 | ★★ 当前最致命 |
| 14 | **可观测性** | `lastError` 一句 + 3s 限流 log | `dumpTouchRuntimeStatus()` / `runtimeTimeline` / 候选方法矩阵 / 注入延迟 | 影破故障能自证，本项目只能猜 |

---

## 二、第 3、4 项：方法解析（决定"能不能启用"）

### 本项目 —— `InputManagerInjector.kt:162-172`

```kotlin
private fun findInjectMethod(cls: Class<*>): Method? {
    for (m in cls.methods) {
        if (m.name != "injectInputEvent") continue
        val p = m.parameterTypes
        if (p.size == 2 &&
            InputEvent::class.java.isAssignableFrom(p[0]) &&
            p[1] == Int::class.javaPrimitiveType
        ) return m
    }
    return null          // ← 找不到就整个后端死
}
```

`init()` 里对应 `fail("${service.javaClass.name} has no injectInputEvent(InputEvent, int)")`（`:116`）。

注释里写着"参数扫描是为了兼容只暴露三参形态的构建"，**但代码里那条扫描并不存在** —— 注释与实现不符。

### 影破 —— `TouchInjectionServer.java:4868-4936`

```java
// resolveInputBinderInjectHandle()
Object object0 = Class.forName("android.os.ServiceManager")
        .getMethod("getService", String.class).invoke(null, "input");
IBinder iBinder0 = (IBinder) object0;
iBinder0.isBinderAlive();
Object object1 = Class.forName("android.hardware.input.IInputManager$Stub")
        .getMethod("asInterface", IBinder.class).invoke(null, iBinder0);
Class class0 = Class.forName("android.hardware.input.IInputManager");
Method[] arr_method = class0.getMethods();
ArrayList arrayList0 = new ArrayList();
for (int v = 0; v < arr_method.length; ++v) {
    if (yp.g1(arr_method[v].getName(), "inject", true)) {   // 名字含 "inject"
        arrayList0.add(arr_method[v]);
    }
}
Method method1 = resolveLinkStyleInjectMethod(class0, arrayList0);
```

```java
// resolveLinkStyleInjectMethod()
try {
    method0 = class0.getMethod("injectInputEvent", InputEvent.class, Integer.TYPE);  // 二参优先
} catch (Throwable t) { method0 = null; }
if (method0 != null) return method0;
for (Object o : list0) {
    if (supportsInjectMethod((Method) o)) return (Method) o;   // 任意形态回退
}
return null;
```

```java
// supportsInjectMethod() —— 回退的筛选条件
arr_class.length != 0 && InputEvent.class.isAssignableFrom(arr_class[0])
    && 其余参数全部 isSupportedInjectParameterType()   // int / boolean / long
```

```java
// buildInjectArguments() + defaultInjectArgumentFor()
arr_object[0] = inputEvent0;
for (int v = 1; v < arr_class.length; ++v) {
    arr_object[v] = defaultInjectArgumentFor(method0, v, arr_class[v]);
}
// defaultInjectArgumentFor: int → (名含 "ToTarget" && v == 2) ? -1 : 0
//                           boolean → false
//                           long    → 0L
```

**要点**：`-1` 是 `INVALID_UID`，传给 `injectInputEventToTarget` 表示"不限定目标窗口"。

**后果**：AOSP 在 `IInputManager.aidl` 里给二参版注了
"exists only for compatibility purposes and may be removed in a future release"。
那一天到来时，影破自动切到 `ToTarget` 形态继续跑，本项目**初始化即失败、后端全灭**。

---

## 三、第 5-9 项：事件内容（决定"启用后会不会被过滤"）

### 3.1 事件构造 —— 两边几乎是同一行

本项目 `:353-368`：

```kotlin
val event = MotionEvent.obtain(
    downTime, now, fullAction, count, props, coords,
    0, 0, 1f, 1f,
    0,                              // deviceId — always the main display
    0,                              // edgeFlags
    InputDevice.SOURCE_TOUCHSCREEN,
    0                               // flags
)
```

影破 `:2246-2247`：

```java
MotionEvent motionEvent0 = MotionEvent.obtain(
        this.mergedDownTime, v2, v, list0.size(),
        this.mergedPointerProperties, this.mergedPointerCoords,
        0, 0, 1.0f, 1.0f,
        v3,          // ← deviceId：真触摸设备 id
        0, 0x1002, 0);
motionEvent0.setSource(0x1002);
```

**唯一实质差异就是第 11 个参数。** 影破的 `v3` 来自 `activeInjectedTouchDeviceIdLocked()`（`:408-413`），
优先 `physicalRealTouchDeviceId`，而它是这样算出来的（`:1675-1699`）：

```java
private final int detectPhysicalRealTouchDeviceId() {
    int[] arr_v = InputDevice.getDeviceIds();
    ... for each device:
        if ((device.getSources() & 0x1002) != 0) { found = device; break; }   // 0x1002 = SOURCE_TOUCHSCREEN
    return found == null ? 0 : found.getId();
}
```

### 3.2 PointerCoords —— 本项目 2 个字段，影破 6 个

本项目 `:337-345`：

```kotlin
for (i in 0 until count) {
    val f = fingers[i]
    props[i].id = f.localId
    props[i].toolType = MotionEvent.TOOL_TYPE_FINGER
    coords[i].x = f.x
    coords[i].y = f.y
    coords[i].pressure = 1f      // ← 满压
    coords[i].size = 1f          // ← 满面积；touchMajor/touchMinor 不设 = 0
}
```

影破 `:516-527` + `:3109-3140`：

```java
private final void applyLinkTouchShapeToPointerCoords(PointerCoords c, float x, float y, float p) {
    c.clear();
    c.x = x;
    c.y = y;
    c.pressure = p;
    float f3 = clamp(p, 0.1f, 1.0f);
    float f4 = 35.0f * f3 + 20.0f;
    c.touchMajor = f4;
    c.touchMinor = (0.13f * f3 + 0.72f) * f4;
    c.size = f3 * 0.33f + 0.05f;
}
```

而 `p` 不是常数，是**仿真的人类压力**：

```java
private final float humanizedProgramPressureLocked(int pointerId, float x, float y, float requested, ...) {
    float f3 = normalizedRequestedProgramPressure(requested);
    float f4 = (float)((Math.sin(0.0011f * y + (0.0017f * x + 0.731f * pointerId)) + 1.0) * 0.5);
    // 再按 位移速度 / 距上次采样时间 / 压力包络 做低通
    ... clamp(..., 0.52f * f3, 0.92f * f3)
}
```

**取值区间是 0.52~0.92 倍请求压力** —— 刻意**不落在 1.0**。

> 判读：AOSP 原生 InputDispatcher 不校验这些字段（scrcpy 长期用裸注入就是证明），
> 所以这不是"能不能通过平台"的问题，而是**"OEM 定制层 / 游戏端做输入合理性校验时，谁的特征更显眼"**的问题。
> 本项目用的是最机械的一档（满压、满面积、面积三轴为 0、来源设备为 0），需要替换成拟真值。

### 3.3 DOWN 前清场 —— 本项目完全缺

影破 `:3145-3162`：

```java
private final void injectVerifierCleanupBeforeDownIfNeeded(
        int action, long eventTime, int pointerCount,
        PointerProperties[] props, PointerCoords[] coords, int deviceId, String reason) {
    if ((action & 0xFF) == 0 && Build.VERSION.SDK_INT >= 36 && pointerCount > 0) {   // 0 = ACTION_DOWN
        MotionEvent cancel = MotionEvent.obtain(eventTime, eventTime, 3, pointerCount,
                props, coords, 0, 0, 1.0f, 1.0f, deviceId, 0, 0x1002, 0);           // 3 = ACTION_CANCEL
        cancel.setSource(0x1002);
        try {
            if (!invokeInjectMotionEvent(cancel)) {
                Log.w("ShizukuUserSvc", "cleanup cancel rejected deviceId=" + deviceId + " reason=" + reason);
            } else if (!physicalInjectVerifierCleared) {
                physicalInjectVerifierCleared = true;
            }
        } finally { cancel.recycle(); }
    }
}
```

调用点在 `emitMergedMotion()` 里，紧挨着真正的 DOWN（`:2245`）：

```java
this.injectVerifierCleanupBeforeDownIfNeeded(v, v2, list0.size(), ...);
MotionEvent motionEvent0 = MotionEvent.obtain(this.mergedDownTime, v2, v, ...);
```

另有一个全零的清理 CANCEL（`sendLinkStyleCleanupCancelLocked`，`:4987-5030`）：
`pressure = touchMajor = touchMinor = size = 0`，用于切换/复位。

**影破的门槛是 `SDK_INT >= 36`**（Android 16），说明作者在 Android 16 上实测撞到了"首帧 DOWN 被拒"。

### 3.4 返回值判断 —— 本项目更严格，也更脆

本项目 `:392-396`：

```kotlin
val accepted = method.invoke(target, event, INJECT_INPUT_EVENT_MODE_ASYNC)
if (accepted != true) { reportRefusal(...); return false }
```

影破 `:3163-3184`：

```java
Object object0 = qr0.a1.invoke(qr0.a0, arr_object1);
boolean0 = Boolean.valueOf(
    (object0 instanceof Boolean ? ((Boolean) object0).booleanValue()
     : !(object0 instanceof Number) || ((Number) object0).intValue() != 0));   // 非 Boolean/Number ⇒ 当成功
```

差异点：反射调用 `InputManager.injectInputEvent` 时返回类型必然是 `boolean`，
所以本项目当前不受影响；但一旦按影破那样改用 `IInputManager` binder 形态，
代理返回的可能是别的东西 —— 影破那三档判断就是为这个准备的。

---

## 四、第 13 项：接入层（当前最致命，和上一轮的 `readerInit` 硬门同一处）

### 本项目 —— `ShellServerEntry.kt:946-995`

```kotlin
if (!ShellNative.readerIsReady()) {
    if (!ShellNative.readerInit(screenW, screenH, rotation)) {
        Log.e(TAG, "OPEN: readerInit FAILED at ${screenW}x$screenH rot=$rotation")
        replyErr("readerInit failed (no touch panel?)")
        return                                   // ← 门在这里
    }
}
...
val useInputManager = ShellNative.injectGetBackend() == ShellNative.INJECT_BACKEND_INPUT_MANAGER
if (useInputManager) {
    val ok = ShellNative.injectSetBackend(ShellNative.INJECT_BACKEND_INPUT_MANAGER)   // ← IM 在这里才初始化
    ...
}
```

而 `inject_set_backend()` 的 IM 分支（`uinput_inject.cpp:1035-1053`）会调
`imBridgeInit()` → 反射拿 InputManager → 解析方法。

**IM 从原理上不需要面板**（它绕开 EventHub/InputReader），却排在一道面板门之后。
面板读不到（权限被拒 / 协议 A）的设备，连改走 IM 的机会都没有。

### 影破 —— `setInjectOnlyMode()`（`:5046-5075`）

```java
public final boolean setInjectOnlyMode(boolean z) {
    if (this.injectOnlyMode != z) {
        this.injectOnlyMode = z;
        this.recordTimelineLocked("inject_only:" + (z ? 1 : 0));
        if (z) {
            this.physicalTriggerEnabled = false;
            this.clearPhysicalTriggerActivationLocked();
            this.physicalTriggerHolding = false;
            this.physicalStatus = "OK: inject-only active";
            list0 = this.stopPhysicalReadersLocked();      // ← 停掉全部面板 reader
            this.signalMergedInjectLocked("injectOnlyEnabled");
        }
        ...
    }
}
```

`isTouchMergerActiveLocked()`（`:3260-3264`）也把 inject-only 排除在抓取之外：

```java
return this.injectOnlyMode ? false : !this.physicalReaders.isEmpty() && this.physicalGrabCompleted;
```

**这是本项目缺的一档工作模式**：影破可以在完全不碰 `/dev/input` 的前提下做注入，
所以它的适用面天然比本项目宽 —— 包括那些 shell 读不到面板的设备。

### 物理手指的处理也不同

| | 本项目 | 影破 |
|---|---|---|
| 结构 | 一套 `g_fingers[]`，镜像手指与自瞄手指共用槽位记账 + takeover 融合 | 两张表 `mergedPhysicalPointers` / `mergedProgramPointers`，`doMergeAndInject()` 里合并 |
| 分发 | native 每次 `upload()` 同步 `pushFrame` | `ensureMergedDispatchThreadStartedLocked()` 独立线程 + dirty 标记 |
| 抓取 | `exclusive` 默认 true，恒 grab | `isLinkStyleGrabModeLocked()` = 有 reader 才算抓取模式 |

---

## 五、第 14 项：可观测性

影破有完整的运行时自证系统：

| 机制 | 位置 | 作用 |
|---|---|---|
| `dumpTouchRuntimeStatus()` | `:1896-2170` | 一次性导出全部运行时状态（大量字段） |
| `runtimeTimeline`（环形缓冲） | `recordTimelineLocked(...)` | 记录每一次注入成败与原因 |
| `describeParameters(method)` | `:1134-1145` | 记录**实际解析到的方法签名** |
| `logLinkStyleInjectCandidates` | `:3528-3534` | 记录所有候选 inject 方法 |
| `lastPhysicalCandidateMatrix` / `lastPhysicalOpenMatrix` | 字段 | 记录设备探测矩阵 |
| `recordInjectLatencyLocked()` | 多处 | 注入延迟 |
| `lastInjectPath` / `lastInjectError` | 字段 | 当前走的哪条路、最后错什么 |

本项目对应物只有 `InputManagerInjector.lastError()`（一句字符串）
+ `reportRefusal()` 的 3 秒限流 log。

**这就是为什么影破的用户报障能直接定位，而本项目的用户只能说"用不了"。**

---

## 六、优先级建议

### P0 —— IM 不该被面板门挡住（约 10 行）

`ShellServerEntry.kt:947-951` 的 `readerInit` 失败 `return`，改为**降级但不中止**：
面板读不到时把 `useInputManager` 置为 true 并继续，让 IM 后端接管。
IM 本来就不需要面板，这是用最小的改动打开最大的适用面。

配套（同一处）：`readerInit` 失败时若 IM 也不可用，才 `replyErr` —— 不要两个都坏还报"面板读不到"。

### P1 —— 方法解析补回退（对齐影破，约 25 行）

`findInjectMethod()` 增加：
1. 二参 `injectInputEvent(InputEvent, int)` 优先（现状）；
2. 找名字含 `inject` 且 `param[0]` 是 `InputEvent`、其余全为 `int/boolean/long` 的方法；
3. 参数填充：int → 名含 `ToTarget` 且位置 2 时 `-1`，否则 `0`；boolean → `false`；long → `0L`。

顺便把 `:156-160` 那段与代码不符的注释一起修掉。

### P2 —— 事件拟真（约 20 行，建议做成开关并默认开启）

- `deviceId`：按影破方式取真触摸屏 id（`InputDevice.getDeviceIds()` 里 `sources & 0x1002` 的第一个），取不到回退 0；
- `pressure`：换成 0.5~0.95 区间的温和波动，**不要恒 1.0**；
- `size` / `touchMajor` / `touchMinor`：按影破公式给值，不要留 0；
- 保留 `setSource(SOURCE_TOUCHSCREEN)`（与 obtain 第 13 参一致，冗余但无害）。

### P3 —— SDK ≥ 36 的 DOWN 前 CANCEL（约 15 行）

照 `injectVerifierCleanupBeforeDownIfNeeded()`：`Build.VERSION.SDK_INT >= 36` 且 action 为 DOWN 时，
先用同 deviceId 注入一个 1 指针 `ACTION_CANCEL`，检查返回值并 log。

### P4 —— inject-only 模式（结构性，成本最高）

在菜单里加"注入模式：仅注入（不读面板）"，对齐影破 `setInjectOnlyMode`：
跳过 `readerInit`/grab，直接用 IM 注入程序指针。这一档是**给读不到面板的设备兜底的最终手段**。

### P5 —— 自检与自证

至少在 IM 初始化成功时把以下内容写进 `aimbot_daemon.log` 并回报给 App：
- 解析到的类名 + 方法签名 + 参数类型列表（对齐 `describeParameters`）；
- `deviceId` 取到了哪个值（真实 id 还是 0）；
- 第一次注入的返回值 / 被拒原因；
- 累计成功/失败计数（对齐 `totalInjectFailureCount`）。

---

## 七、待实测确认的点（不写成结论）

1. `deviceId = 0` 在目标 ROM 的 `InputDispatcher` 里是否进入与真实设备不同的分支 —— 需在失败设备上对比
   `deviceId = 真 id` 前后的 `dumpsys input` 与游戏内表现。
2. `pressure = 1.0` + `size = 1.0` + `touchMajor = 0` 是否被某些 ROM 的注入校验丢弃 ——
   需要通过 P2 的开关做 A/B 验证，而不是断言。
3. 本项目 IM 后端目前**只在用户手动切到 INPUT_MANAGER 时才会初始化**；
   失败设备上用户是否切得过去，取决于 P0 是否先修。

---

## 证据索引

| 主题 | 本项目 | 影破（`TouchInjectionServer.java`） |
|---|---|---|
| 方法解析 | `InputManagerInjector.kt:162-172` | `:4868-4936`（`resolveInputBinderInjectHandle` / `resolveLinkStyleInjectMethod`） |
| 参数筛选 | —（无） | `:3218-3230`（`supportsInjectMethod` / `isSupportedInjectParameterType`） |
| 参数填充 | `:391`（写死 mode=0） | `:773-785` + `:1075-1096`（`buildInjectArguments` / `defaultInjectArgumentFor`） |
| 事件构造 | `:353-368` | `:2246-2247`、`:3147-3148`、`:5002-5003` |
| PointerCoords | `:337-345` | `:516-527`（`applyLinkTouchShapeToPointerCoords`） |
| 压力拟真 | —（恒 1f） | `:3109-3140`（`humanizedProgramPressureLocked`） |
| deviceId | `:364`（恒 0） | `:408-413` + `:1675-1699`（`detectPhysicalRealTouchDeviceId`） |
| DOWN 前 CANCEL | —（无） | `:3145-3162` + `:4987-5030` |
| 返回/异常 | `:387-410` | `:3163-3184`（`invokeInjectMotionEvent`） |
| id 分配 | `:473-485` | `:429-443`（`allocateMotionIdLocked`） |
| 状态机 | `:229-303`（`applyDesired`） | `:1727-1895`（`doMergeAndInject`） |
| 分发线程 | native `upload()` 同步 | `:2307-2320`（`ensureMergedDispatchThreadStartedLocked`） |
| inject-only | —（无） | `:5046-5075`（`setInjectOnlyMode`） |
| 自检/状态 | `lastError` | `:1896-2170`（`dumpTouchRuntimeStatus`） |
| 接入点 | `ShellServerEntry.kt:946-995` | — |
| native 驱动 | `uinput_inject.cpp:376-389`、`:995-1053` | — |

---

## 八、本次已实现的改动（2026-09-16）

改动文件：**只有一个** —— `app/src/main/java/io/github/xiangsu1145/aimbotnextgen/inject/InputManagerInjector.kt`。
JNI 桥接契约（`init` / `pushFrame` / `releaseAll` / `isReady` / `lastError` 五个静态方法）未变，
native 侧（`input_jni.cpp`、`uinput_inject.cpp`、`inject_backend.h`）一行未动。
新增一个 `stats(): String` 静态方法，native 不需要调用它。

### 8.1 句柄：直连 IInputManager AIDL（新，首选路线）

`obtainService()` 现在是三级：

1. `ServiceManager.getService("input")` → `IBinder` → `IInputManager$Stub.asInterface()` → **直接用 AIDL 接口**；
2. 退路 `InputManager.getInstance()`（Java 包装类）；
3. 退路 `Context.getSystemService("input")`。

哪条生效会记在 `route` 里并写进日志。

### 8.2 方法解析：二参优先 + 任意签名回退（新）

`findInjectMethod()` 先试 `injectInputEvent(InputEvent, int)`；拿不到就扫整张方法表，
接受「名字含 `inject`」+「`param[0]` 是 `InputEvent`」+「其余全是 int/boolean/long（含装箱）」的方法，
并 `setAccessible(true)`。这覆盖 `injectInputEventToTarget(event, mode, targetUid)` 形态。

`buildInjectArguments()` / `defaultArgumentFor()`：int → （`index == 2` 且名字含 `ToTarget`）时 `-1`，
否则 `0`；boolean → `false`；long → `0L`。`-1` 即 `INVALID_UID`（不限定目标窗口）。

### 8.3 事件内容：拟真（新）

- **`deviceId`**：`detectTouchDevice()` 在 `init()` 时扫描 `InputDevice.getDeviceIds()`，
  取第一个 `sources` 含 `SOURCE_TOUCHSCREEN` 的 `device.id` 并缓存；扫不到或枚举抛异常则回退 `0`
  （旧行为，仍然可用）。日志会写明取到了哪个设备。
- **`pressure`**：`humanizedPressure(f)` —— 两个慢速正弦（一个跟位置、一个跟时间）
  合成 0.60~0.90 的取值，再以 0.25 的系数低通缓动到目标，整体 clamp 在 0.55~0.92。
  **不再恒为 1.0**。状态存在 `Finger.pressure` 上，跨帧连续。
- **`size` / `touchMajor` / `touchMinor`**：`applyTouchShape()` 按压力算 ——
  `major = 35*clamp(p,0.1,1)+20`、`minor = (0.13*c+0.72)*major`、`size = c*0.33+0.05`。不再留 0。
- `event.source = SOURCE_TOUCHSCREEN` 在 `obtain()` 之后**再设一次**（对齐影破，冗余无害）。

⚠️ 实现时踩到并已修掉的一个自伤：`applyTouchShape()` 最初照影破的写法先调了 `coords.clear()`，
而本项目是先设 `x/y` 再调它 —— `clear()` 会把刚写的坐标清零。
现已把它合并成一次调用 `applyTouchShape(coords, x, y, pressure)`，与影破「一个函数设全部」的写法对齐。

### 8.4 起手清场：DOWN 前的 ACTION_CANCEL（新）

`cancelBeforeDown()` —— 当 `SDK_INT >= 36` 且动作为 `ACTION_DOWN` 时，
先用**同一批 pointers、同一坐标、同一 `deviceId`** 注入一个 `ACTION_CANCEL`，再发真正的 DOWN。
被拒只记日志（`cancel_fail++` + 一行 `Log.w`），**不阻止** DOWN —— 这是清理，不是可验证的前置条件。

常量 `SDK_ANDROID_16 = 36` 独立命名，将来 Google 调整门槛只需改一处。

### 8.5 日志（新，这条你最需要）

| 时机 | 内容 |
|---|---|
| `init()` 成功 | 一段结构化日志：`route` / `service` 类名 / `method` 签名+参数类型 / `deviceId`+设备名 / SDK 版本 + 是否启用预取消 |
| `init()` 失败 | 具体原因；方法解析失败时还会**列出该类所有 `inject*` 方法的签名**，这样"到底有没有可用的注入方法"一眼可见 |
| 首次注入成功 | 记一次 `action` / `pointers` / `deviceId` / 走的方法签名 |
| 每次注入失败 | `failCount` 累计 + action/pointers/deviceId + 提示去 system log 查 `Injection failed` / `Inconsistent event`；**3 秒限流** |
| 被拒是 `SecurityException` + `INJECT_EVENTS` | 单独一条：提示开「USB 调试（安全设置）」并重启 |
| 预取消被拒 | `pointers` / `deviceId` + 说明可能导致随后的 DOWN 不被接受 |
| 每 1000 帧 | `pushFrame` 里输出一行 `im[route | method | dev | ok/fail | cancel_ok/cancel_fail]`（约 10 秒一次） |

最后一条是给**现场排障**用的：以后用户反馈"用不了"，日志里就能看到
「是没调用（ok=0 说明没进 emit）」「是调用了被拒（fail 在涨）」「还是 deviceId 没探到」。

### 8.6 本次**没有**动的（需要你决定）

1. **`readerInit` 硬门（P0）** —— `ShellServerEntry.kt:947-951` 的 `return` 仍在。
   影响的是「读不到面板」的设备：它们连 IM 都初始化不到。
   你这次反馈的失败设备是「ui 可以点击」（= 面板读得到），所以这一条不是当前障碍，
   但它仍是另一类设备（权限被拒 / 协议 A）的拦路石。改它要动 OPEN 的失败语义 + 「菜单触摸一并失效」的产品取舍，未擅自改。
2. **inject-only 模式（P4）** —— 仍然没有。IM 依旧与镜像/融合共用一套 `g_fingers`，
   做不到影破那种「完全不碰 `/dev/input`」的纯注入形态。
3. 返回值判断已放宽（`Boolean` → 值；`Number` → `!= 0`；其他一律视为成功），对齐影破。
4. pointer id 分配**保持本项目的活列表推导**，没有改成影破的 `stableMotionIdMap`
   —— 影破那版在满 16 时会回退到 0 并撞车，本项目的写法更安全。

### 8.7 怎么验证

```bash
# 1. 装上后，切到 InputManager（菜单 → 触摸 → 触摸方式）
# 2. 看 daemon 日志里的初始化段
adb logcat -s AimbotInject:V AimbotInput:V AimbotShell:V | grep -iE "injector ready|route|method|deviceId"
#    期望看到类似：
#    InputManager injector ready
#      route    : IInputManager binder (ServiceManager.getService)
#      method   : injectInputEvent(InputEvent, int)
#      deviceId : 5 (focaltech_ts)
# 3. 摸屏幕，看注入计数在涨
adb logcat -s AimbotInject:V | grep "im\["
# 4. 如果 fail 在涨，去 system 侧找原因
adb logcat -d | grep -iE "InputDispatcher|Injection failed|Inconsistent event"
```

判定：
- `dev=0` ⇒ 设备枚举没成功，退回旧的匿名注入（不致命，但拟真没生效）；
- `ok=0 fail=0` 且 `im[...]` 在滚动 ⇒ 帧进来了但**没有手指落下**（native 侧没有 `isDown` 的手指），
  问题在 uinput/融合那侧，不在 IM；
- `fail` 持续增长 ⇒ 注入被拒，看下一步的 system log；
- 只出现「USB 调试（安全设置）」那条 ⇒ 权限问题，和实现无关。
