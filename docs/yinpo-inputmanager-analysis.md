# 影安（YingPo / com.yingP.runtime.m625）触摸注入实现分析

> 样本：`D:\破解\YingPo_RES.apk (5)`（JEB 反编译，`classes1/` 为 java 输出）
> 目的：确认「内置 ADB 调试 + InputManager 注入」这条路在 **uid 2000（shell）** 下如何落地，
> 并回答本项目能否照此实现。
> 日期：2026-09-16

---

## 0. 结论速览

| 问题 | 答案 |
|---|---|
| `D:\AndroidHiddenApiBypass` 里有三参数的 `InputManager.injectInputEvent(InputEvent,int,int)` 吗？ | **没有。** 全仓库 5 个类，`InputManager` / `injectInputEvent` / `INJECT_EVENTS` 命中数 **0**。它是隐藏 API *反射工具*，不是 API 包装库。 |
| 那三参数版本真实存在吗？ | **存在，但有两个不同的东西**：`android.hardware.input.InputManager.injectInputEvent(InputEvent,int,int)`（Java 包装类，Android 14 已有）和 `android.hardware.input.IInputManager.injectInputEventToTarget(InputEvent,int,int)`（AIDL，Binder 接口）。影安用的是**后者**。 |
| 我的项目在 ADB（shell，uid 2000）下能用它吗？ | **能。** 影安就是活证据：它的 IM 注入跑在一个会执行 `dumpsys input` 的进程里 —— 那是只有 shell/root 才能做的事。所需权限 `INJECT_EVENTS` 由 `com.android.shell` 持有（`adb shell input` 一直依赖这一点）。 |
| `getevent` 看不到它的触摸 | **必然。** IM 注入在 `InputDispatcher` 内部完成，不经过 EventHub/InputReader，**不产生任何 evdev 设备**，所以 `getevent` 永远看不到。uinput 才会造出可见的 evdev 节点。 |

---

## 1. 它的触摸是「双后端」

这个 App **不是**只有 InputManager，它两条路都有，而且分得很干净：

| 后端 | 载体 | 说明 |
|---|---|---|
| **uinput** | `lib/arm64-v8a/libtim9.so` | 独立**原生可执行文件**，`strings` 自述是 **TouchMerger**：`Virtual touchscreen created (slots: %d, MT axes: %d, prop: DIRECT)` / `/dev/uinput` / `/dev/input/event%d` / `TouchMerger initialized in INJECT-ONLY mode` |
| **InputManager** | `TouchInjectionServer.java`（6123 行 Java，反混淆后） | 反射 `IInputManager` binder，直接调 AIDL 的 `injectInputEvent` / `injectInputEventToTarget` |

启动方式（`ShellServiceMain.handleStartInject`，实测代码）：

```java
File file0 = new File(s, "libtim9.so");              // s = nativeLibraryDir
if(!file0.isFile()) return "ERROR libtim9.so not found: " + file0.getAbsolutePath();
Process process0 = new ProcessBuilder(new String[]{
        file0.getAbsolutePath(), "--inject-only"}).redirectErrorStream(false).start();
ShellServiceMain.injectProcess = process0;
// 之后读 stderr，等 5000ms 内的就绪信号；失败则 stopInjectProcess()
```

> 注意：它把 **lib 目录下的 `.so` 直接当可执行文件跑**。所以 uinput 那条路的进程是 shell 服务的子进程，继承 uid 2000。

---

## 2. 它跑在什么进程里（这是全部问题的关键）

三条独立的证据，都指向同一个答案：**uid 2000 的 shell 服务进程，不是 App 进程（10xxx），也不是 system_server。**

### 证据 A — 它在自己的进程里跑只有 shell 能跑的命令

```java
// TouchInjectionServer.java:1251 / 1467 / 1553 / 2487
String s = this.runCommandCapture(new String[]{"dumpsys", "input"});
String s = this.runCommandCapture(new String[]{"dumpsys", "window", "displays"});
```

`dumpsys` 需要 `android.permission.DUMP`，只有 shell/root/system 持有。App 进程做不了这件事。

### 证据 B — 它的日志 tag 是 Shizuku UserService 的约定

整个文件里所有日志都写 `Log.w("ShizukuUserSvc", ...)`（全包仅此文件使用该 tag）。
配合 `classes1/` 里打包了完整的 `rikka.shizuku.*`（`Shizuku.bindUserService` / `UserServiceArgs` / `ShizukuServiceConnection`）与 `moe.shizuku.manager.adb.*`（Shizuku Manager 的**无线调试 AdbClient**，即"内置 ADB 调试"的来源），架构与 Shizuku UserService 完全同构。

### 证据 C — 它自己的 shell 服务是 `app_process` 拉起的

```java
// TouchInjectionServer.java:3351（清理旧进程时的识别条件）
if(yp.g1(s, "ShellServiceMain", false) ||
   yp.g1(s, "app_process", false) && yp.g1(s, "libshellservice_dex", false)) { ... kill ... }
```

`ShellServiceMain.main(String[])` 就是一个 `app_process` 入口（stdin 行协议：先写 `READY`，再逐行读命令，结束 `stopServer()`）。
载荷藏在 `lib/arm64-v8a/libcom_kwai_kling_shellservice_dex.so`（**把 dex 塞进 .so**），配合 `libcom_kwai_kling_touch_probe_jni.so`。
这套「dex 藏 .so + app_process 起 + 自己带 AdbClient 做无线配对」= 影安版的内置 ADB 调试。

**⇒ 结论：`TouchInjectionServer` 的 IM 注入 = uid 2000 + shell 域。和本项目 daemon 同档同域。**

---

## 3. IM 注入的完整调用链（逐字引用）

### 3.1 解析句柄：直接拿 binder，不用 `InputManager` 包装类

```java
private final qr resolveInputBinderInjectHandle() {
    Object object0 = Class.forName("android.os.ServiceManager")
            .getMethod("getService", String.class).invoke(null, "input");
    IBinder iBinder0 = object0 instanceof IBinder ? ((IBinder)object0) : null;
    if(iBinder0 == null) throw new IllegalStateException("input binder is null");
    iBinder0.isBinderAlive();

    Object object1 = Class.forName("android.hardware.input.IInputManager$Stub")
            .getMethod("asInterface", IBinder.class).invoke(null, iBinder0);
    if(object1 == null) throw new IllegalStateException("proxy is null");

    Class class0 = Class.forName("android.hardware.input.IInputManager");
    Method[] arr_method = class0.getMethods();          // 攒出所有名字含 "inject" 的方法
    ...
    Method method1 = this.resolveLinkStyleInjectMethod(class0, arrayList0);
    if(method1 == null) throw new NoSuchMethodException("no injectInputEvent method");
    method1.setAccessible(true);
    ln0 = new qr(object1, method1, "IInputManager." + method1.getName()
            + this.describeParameters(method1));        // handle = (目标对象, Method, 描述)
}
```

**它反射的是 `IInputManager`（AIDL 接口），不是 `android.hardware.input.InputManager`。**
理由：AIDL 接口的方法名就是 binder 上的真实交易名，绕开了 `InputManager` → `InputManagerGlobal` 两层包装，也不依赖 `InputManager.getInstance()` 这类 `@UnsupportedAppUsage` 入口。

### 3.2 选方法：二参数优先，缺失则回退到任意签名兼容的 inject*

```java
private final Method resolveLinkStyleInjectMethod(Class class0, List list0) {
    Method method0;
    try { method0 = class0.getMethod("injectInputEvent", InputEvent.class, Integer.TYPE); }
    catch(Throwable throwable0) { method0 = new ln(throwable0); }
    if(method0 instanceof ln) method0 = null;
    if(method0 != null) return method0;                 // ← 首选：injectInputEvent(InputEvent,int)
    for(Object object0: list0) {
        if(this.supportsInjectMethod(((Method)object0))) return (Method)object0;
    }
    return null;                                        // ← 回退：含 injectInputEventToTarget
}
```

`supportsInjectMethod` 的条件：名字含 `inject`、`param[0]` 是 `InputEvent` 子类、**其余参数全是 int/Integer/boolean/Boolean/long/Long**。所以它天然兼容 2/3/4 参数的各版本写法。

### 3.3 填参数：mode=0(ASYNC)，targetUid=-1(INVALID_UID)

```java
private final Object[] buildInjectArguments(Method method0, InputEvent inputEvent0) {
    Class[] arr_class = method0.getParameterTypes();
    Object[] arr_object = new Object[arr_class.length];
    arr_object[0] = inputEvent0;
    for(int v = 1; v < arr_class.length; ++v)
        arr_object[v] = this.defaultInjectArgumentFor(method0, v, arr_class[v]);
    return arr_object;
}

private final Object defaultInjectArgumentFor(Method method0, int v, Class class0) {
    int v1 = 0;
    // ... int/Integer → 继续；boolean/Boolean → return false；long/Long → return 0L
    if(v != 1) {                                        // v == 1 就是 mode，保持 0
        String s = method0.getName();
        if(yp.g1(s, "ToTarget", true) && v == 2) v1 = -1;   // v == 2 → targetUid = -1
    }
    return v1;
}
```

⇒ 它对三参数版本 `injectInputEventToTarget(event, mode, targetUid)` 填的是 **`mode = 0`（`INJECT_INPUT_EVENT_MODE_ASYNC`，不等待）、`targetUid = -1`（`Process.INVALID_UID`，即"不限定窗口"）** —— 语义上等价于二参数版。**它没有真的用 targetUid 去指定某个 App。**

### 3.4 调用与判定

```java
private final boolean invokeInjectMotionEvent(MotionEvent motionEvent0) {
    qr qr0 = this.getInjectHandle();
    Object[] arr_object = this.buildInjectArguments(qr0.a1, motionEvent0);
    try {
        Object[] arr_object1 = Arrays.copyOf(arr_object, arr_object.length);
        Object object0 = qr0.a1.invoke(qr0.a0, arr_object1);
        boolean0 = Boolean.valueOf(object0 instanceof Boolean ? ((Boolean)object0).booleanValue()
                : !(object0 instanceof Number) || ((Number)object0).intValue() != 0);
    } catch(Throwable throwable0) { boolean0 = new ln(throwable0); }
    Throwable throwable1 = mn.a0(boolean0);
    if(throwable1 != null)
        Log.w("ShizukuUserSvc", "invokeInjectMotionEvent failed handle=" + qr0.a2, throwable1);
    ...
    return boolean0.booleanValue();          // 返回 false 时调用方会回滚状态
}
```

它**检查返回值**（`InputManager` 被拒时返回 false），失败会把 handle 描述一并打进日志 —— 这正是本项目 `reportRefusal()` 在做的事。

### 3.5 一个真实世界的坑：SDK ≥ 36 的预取消

```java
private final void injectVerifierCleanupBeforeDownIfNeeded(int v, long v1, int v2, ...) {
    if((v & 0xFF) == 0 && Build.VERSION.SDK_INT >= 36 && v2 > 0) {   // v 是 action，0 = ACTION_DOWN
        MotionEvent motionEvent0 = MotionEvent.obtain(v1, v1, 3 /*ACTION_CANCEL*/, v2,
                arr_motionEvent$PointerProperties, arr_motionEvent$PointerCoords,
                0, 0, 1.0f, 1.0f, v3, 0, 0x1002 /*SOURCE_TOUCHSCREEN*/, 0);
        motionEvent0.setSource(0x1002);
        if(!this.invokeInjectMotionEvent(motionEvent0))
            Log.w("ShizukuUserSvc", "cleanup cancel rejected deviceId=" + v3 + " reason=" + s);
        ...
    }
}
```

**Android 16 (SDK 36) 上，在 ACTION_DOWN 之前先注入一个 ACTION_CANCEL**，清掉可能残留的手势状态。字段名 `physicalInjectVerifierCleared` 说明这是踩过坑之后的补偿。本项目 IM 路径目前没有这一步 —— 值得记一笔。

### 3.6 MotionEvent 构造参数（与本项目一致）

`deviceId = v3`、`edgeFlags = 0`、`source = 0x1002`（`SOURCE_TOUCHSCREEN`）、`flags = 0`、`pressure/size` 有效值。和本项目 `InputManagerInjector.emit()` 的 `MotionEvent.obtain(...)` 完全同构，没有额外魔法。

---

## 4. AOSP 侧的权威事实（对齐用）

### 4.1 AIDL（`IInputManager.aidl`，master，逐字）

```aidl
    // Injects an input event into the system. The caller must have the INJECT_EVENTS permssion.
    // This method exists only for compatibility purposes and may be removed in a future release.
    @UnsupportedAppUsage
    boolean injectInputEvent(in InputEvent ev, int mode);

    // Injects an input event into the system. The caller must have the INJECT_EVENTS permission.
    // The caller can target windows owned by a certain UID by providing a valid UID, or by
    // providing {@link android.os.Process#INVALID_UID} to target all windows.
    boolean injectInputEventToTarget(in InputEvent ev, int mode, int targetUid);
```

⚠️ **注释明确写了二参数版"只为兼容存在，未来可能删除"。** 影安之所以写"二参数优先 + 任意签名回退"，就是为了防这一天。本项目目前只认二参数（见 §5）。

### 4.2 Java 包装类（`android.hardware.input.InputManager`）

- Android 14 (`android-14.0.0_r1`) 与 master **两个重载都有**：
  - `public boolean injectInputEvent(InputEvent event, int mode)` — `@RequiresPermission(INJECT_EVENTS)` `@UnsupportedAppUsage` `@hide`
  - `public boolean injectInputEvent(InputEvent event, int mode, int targetUid)` — `@RequiresPermission(INJECT_EVENTS)` `@hide`
- 三参数版的 javadoc：*"If a valid targetUid is provided, the system will only consider injecting the input event into windows owned by the provided uid. If the input event is targeted at a window that is not owned by the provided uid, input injection will fail and a RemoteException will be thrown."*
- 三参数版**不委托**二参数版，直接把 `targetUid` 交给 `mGlobal.injectInputEvent(event, mode, targetUid)`。

### 4.3 `INJECT_EVENTS` 谁有

- 权限本体：`signature` 级。
- `com.android.shell` 是 platform 签名的特权应用 → 持有 `INJECT_EVENTS` → **任何以 uid 2000 运行的进程都能通过检查**（`adb shell input` 就是靠这个）。
- 特例：**MIUI/HyperOS** 把 shell 的 `INJECT_EVENTS` 挂在"USB 调试（安全设置）"后面，不开就抛 `SecurityException: ... INJECT_EVENTS`，且需重启。本项目 `reportPermissionProblem()` 已经覆盖了这条提示。

---

## 5. 与本项目的逐项对照

| 项 | 影安 | 本项目现状 | 差异影响 |
|---|---|---|---|
| 进程 / uid | `app_process` 起的 shell 服务，uid 2000 | `app_process` 起的 `ShellServerEntry`，uid 2000 | **相同** |
| 反射目标 | `ServiceManager.getService("input")` → `IInputManager$Stub.asInterface` → `IInputManager` | `android.hardware.input.InputManager.getInstance()`（失败才退 `Context.getSystemService`） | 影安的更抗版本变化；本项目多依赖 `InputManagerGlobal` 一层 |
| 方法选择 | 二参数优先，**回退到任意 inject* 签名** | `findInjectMethod()` **只匹配二参数** | 见下方 P1 |
| mode | `0`（ASYNC） | `INJECT_INPUT_EVENT_MODE_ASYNC` | 相同 |
| targetUid | 3 参数时填 `-1` | 无三参数支持 | 本项目少一条未来容错 |
| MotionEvent | source `0x1002`、flags 0、`deviceId` 可指定 | source `SOURCE_TOUCHSCREEN`、flags 0、`deviceId = 0` | 基本相同 |
| 预取消 | SDK ≥ 36 时 DOWN 前先发 CANCEL | 无 | 见 P2 |
| 失败处理 | 检查返回值 + 打 handle 描述 | `reportRefusal()` 限频 + `reportPermissionProblem()` | 本项目做得更好 |
| 物理面板依赖 | IM 路径**不需要**（`touchDown/touchMove/touchUp` 纯坐标 API） | IM 仍排在 `readerInit` 硬门**之后** | 见 P0 |
| 后端切换 | uinput 走独立子进程，IM 走本进程 | 同进程互斥切换（`inject_set_backend`） | 设计取舍，非缺陷 |

---

## 6. 给本项目的行动建议（**代码未改，等你确认**）

### P0 — `readerInit` 硬门要挪到 IM 初始化之后（最关键）

`ShellServerEntry.kt:946-952`：

```kotlin
if (!ShellNative.readerIsReady()) {
    if (!ShellNative.readerInit(screenW, screenH, rotation)) {
        replyErr("readerInit failed (no touch panel?)")
        return                       // ← 整个 OPEN 中止
    }
}
```

IM 的初始化在 `:975-995`，**在这个 return 之后**。但 IM 注入根本不需要物理面板（它把事件直接交给 `InputDispatcher`，`inject_backend.h` 的文件头注释已经写明了这一点）。

⇒ 任何 `readerInit` 失败的设备，**连 IM 都拿不到**。建议：IM 后端在 reader 之前独立完成 `injectSetBackend`，reader 失败只降级掉「镜像 / 吞触摸 / 菜单触摸」，不影响注入。

### P1 — `findInjectMethod` 的注释与代码不一致

`InputManagerInjector.kt:155-172` 的注释说*"The parameter scan exists for builds that expose only the three-argument form (event, mode, uid)"*，但代码只 `return` 二参数，三参数**不会被选中**，会走到 `fail("... has no injectInputEvent(InputEvent, int)")`。

建议照影安的做法补：`null` 之后扫 `cls.methods`，接受「`param[0]` 是 `InputEvent` + 其余全是 int/boolean/long」，参数用 `mode = 0` / `targetUid = -1` 填充。这样 AOSP 真删掉二参数版时不会突然全线失效。

### P2 — SDK ≥ 36 的 DOWN 前预取消

在 `applyDesired()` 里「第一个手指加入」那一刻，若 `Build.VERSION.SDK_INT >= 36`，先注入一个 `ACTION_CANCEL`（单指针、source TOUCHSCREEN）再发 DOWN。影安为此专门留了字段和分支，说明 Android 16 上确有残留状态问题。

### P3 — 可选：改用 `IInputManager` binder 直连

把 `obtainInputManager()` 的首选路径换成 `ServiceManager.getService("input")` + `IInputManager$Stub.asInterface`，可以少依赖 `InputManager` / `InputManagerGlobal` 两层 @hide 内部实现。收益是跨版本更稳；成本是丢掉 `InputManager.getInstance()` 自己帮你处理的那点初始化。**非必要，视 P1 的结果再定。**

### P4 — 让 IM 成为失败用户的默认后端

`getevent` 看不到 = 没有 evdev 设备 = **ROM 层没有可过滤的对象**。这正好绕开 `docs/uinput-field-failure-diagnosis.md` 里「硬伤 2：克隆设备被厂商事后过滤」那一条。配合 P0 一起做，才是完整解法。

---

## 7. 验证清单（在你的失败设备上）

```bash
# 1. IM 后端是否真的活着（daemon 日志里应出现 inject=inputmgr / inject=InputManager ready）
grep -nE "inject=|InputManager|INJECT_EVENTS" /data/local/tmp/aimbot_daemon.log | tail -20

# 2. shell uid 到底有没有 INJECT_EVENTS（有输出 = 有权限）
adb shell dumpsys package com.android.shell | grep -i inject_events

# 3. 注入后系统侧怎么判的（IM 被拒时原因只在这里）
adb logcat -d | grep -iE "InputDispatcher|Injection failed|Inconsistent event"

# 4. 反向确认 IM 生效特征：getevent 里【不该】出现新的触摸设备
adb shell getevent -pl | grep -i -E "virtual|TouchMerger|aimbot"
#   （用 uinput 后端时这里会出现设备；用 IM 后端时这里始终干净 —— 这就是"看不到触摸"的原因）
```

判读：
- `aimbot_daemon.log` 有 `inject=InputManager NOT usable` → 权限/版本问题，看第 2、3 步
- 日志显示 IM ready，但游戏仍收不到 → 看第 3 步的 `InputDispatcher` 侧拒绝原因
- 日志里连 `inject=` 这行都没有 → **P0 那个 `return` 把 IM 一起掐死了**

---

## 附：本文件的一手证据来源

| 内容 | 位置 |
|---|---|
| IM 反射与调用 | `classes1/com/yingP/runtime/m625/service/TouchInjectionServer.java` :773-788, :1075-1096, :3164-3182, :4864-4950, :5708+ |
| shell 命令 | 同文件 :1251, :1467, :1553, :2487 |
| shell 进程识别 | 同文件 :3351 |
| libtim9.so 启动 | `classes1/com/yingP/runtime/m625/service/ShellServiceMain.java` :485-528 |
| `app_process` 入口 | 同文件 :938 `public static final void main(String[] arr_s)` |
| Binder 服务 | `classes1/com/yingP/runtime/m625/service/ShizukuInputBinderService.java` |
| TouchMerger 字符串 | `lib/arm64-v8a/libtim9.so` |
| AHB 无 InputManager | `D:\AndroidHiddenApiBypass`（5 个 java 文件，命中 0） |
| AIDL 定义 | AOSP `core/java/android/hardware/input/IInputManager.aidl` |
| Java 包装类 | AOSP `core/java/android/hardware/input/InputManager.java`（master / android-14.0.0_r1） |
