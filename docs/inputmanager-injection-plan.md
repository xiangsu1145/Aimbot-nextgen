# 触摸注入：改用 InputManager.injectInputEvent 的实施方案

> 状态：**规划完成，未写代码**（用户裁决「先规划不写代码」）
> 背景工程：Aimbot Nextgen（`io.github.xiangsu1145.aimbotnextgen`）
> 相关文档：`docs/screen-capture-research.md`、`docs/shell-daemon-crash-report.md`

---

## 0. 一句话

把「回注触摸」这一步从 `/dev/uinput` 换成 **`InputManager.injectInputEvent(MotionEvent)`**，
从而**完全绕过输入设备层** —— 厂商对「虚拟触摸设备」做的任何事（HyperOS 在 InputReader 之后丢事件）
都碰不到这条路径。

---

## 1. 为什么必须换（结论先行）

### 1.1 已经证伪的路线

小米（MTK / HyperOS，面板 `focaltech_ts`，100 倍标度）上的现象：**悬浮层 UI 可交互，系统层触摸零反应。**

调查结论（全部有 log/dumpsys 证据）：

| 环节 | 事实 |
| --- | --- |
| grab | 成功（`grab: 1/1 device(s)`） |
| 帧写入 | 成功（`frames` 在涨、`write_failures=0`、无短写） |
| 设备被系统接纳 | **完全接纳** —— `dumpsys input` 里 `Device 11: focaltech_ts`、`Classes: KEYBOARD\|TOUCH\|TOUCH_MT`、`Sources: KEYBOARD\|TOUCHSCREEN`、`DeviceType: TOUCH_SCREEN`、`AssociatedDisplay: hasAssociatedDisplay=true`、`Enabled: true`，与真面板 `Device 6` **逐项相同** |
| 加载顺序 | 已改成 1.2.1 同款（先 `EVIOCGRAB` 后建 uinput 设备）—— **无变化** |
| 与 aimbot 1.2.1 的代码差异 | `touch_core.cpp` 全部 842 行逐段对完，**除加载顺序外无第二处差异**（**2026-09-16 更正：代码确实逐行相同，但喂给它的入参不同 —— 老项目 `cloneFd` 恒为 -1，新项目为真面板 fd，因此产出的虚拟设备根本不是一个东西。见 `docs/uinput-field-failure-diagnosis.md` §2**） |

**⇒ uinput 这条路已经走到尽头。** 故障点在「设备被接纳」之后，我们这侧的代码够不到。

> ⚠️ **2026-09-16 补充（现场数据推翻了上面这个结论）：** 老项目（aimbot 1.2.1 + 无线调试 Shizuku，**uid 2000，不是 root**，log 确认走 uinput 且无静默回退）
> 在大量用户机上 uinput 是**能用**的。所以"uinput 走到尽头"至少不成立为通例 —— 新项目产出的虚拟设备与那个"生产验证过的版本"不是同一个
> （身份/轴集合/是否发压力三轴全部不同），且新项目额外把「能读到物理面板」变成了硬前提。
> 完整定位见 **`docs/uinput-field-failure-diagnosis.md`**。别再按"uinput 没救"来做取舍。

### 1.2 为什么 InputManager 能绕开

`injectInputEvent` 的事件由 `InputDispatcher` 直接受理，**不经过 `EventHub` / `InputReader`**，
因此不依赖任何输入设备节点，也就不存在「设备被接纳但不投递」这个中间层。

参考：**scrcpy（14 万★）的 server 就是 uid 2000 的 `app_process`**，用反射调
`android.hardware.input.InputManager#injectInputEvent`，在包括小米在内的大量真机上工作。

### 1.3 隐藏 API 风险 —— 已排除

`InputManager` / `injectInputEvent` 都是 @hide。**但本项目 daemon 已经在用隐藏 API 反射且工作正常**：

`shell/ShellLayerHost.kt` 里已反射 `android.view.SurfaceControl$Builder`、`android.view.SurfaceSession`、
`android.view.SurfaceControl$Transaction`（悬浮层就是靠它建的）。
scrcpy 的 server 同样是 shell 域 `app_process`，反射隐藏 API，**源码里没有任何 hidden-api 豁免代码**。

**⇒ 我们 daemon 里 `Class.forName("android.hardware.input.InputManager")` + `getMethod("injectInputEvent", …)`
一定能调通。** 无需引入 HiddenApiBypass。

---

## 2. 目标 / 非目标

**目标**
- 新增一个与 uinput 并列的触摸回注后端 `INPUT_MANAGER`，可运行时切换、可持久化。
- **多指必须正确**：不得出现两根虚拟手指互相"跳屏 / 闪烁"（老项目的老毛病）。
- 悬浮层菜单、grab、自瞄、吞触摸语义**全部保持不变**。

**非目标**
- 不改 reader 的解码（协议 A/B 原样）。
- 不改悬浮层 / 截图 / 推理。
- 不删 uinput 后端 —— 一加等设备上 uinput 工作正常，不能回归。

---

## 3. 方案总览

```
物理面板 (/dev/input/eventN)
      │  O_RDONLY + EVIOCGRAB（不变）
      ▼
touch_reader.cpp   解码协议A/B → 面板 raw 坐标
      │            ├─ rebuildPointersLocked() → 同时产出
      │            │     ①raw（uinput 用）②屏幕像素（InputManager 用）
      │            ▼
      │      publishPointers()
      │            │
      │      ┌─────┴─────┐  后端分支（config: "inject"）
      │      ▼           ▼
      │  uinput 后端   InputManager 后端
      │  uinput_mirror_physical()   im_emit()  ──反向 JNI──►  InputManagerInjector.kt
      │  （现状，不动）                                        │
      │                                                        ▼
      │                                        PointersState（唯一状态机）
      │                                                        │
      └── 自瞄 / 扳机（native, ImGui 线程）                      ▼
          也要走同一个状态机  ────────────────►  MotionEvent → injectInputEvent
```

**关键设计：** 无论 mirror 还是自瞄，都只做一件事 ——
**声明"现在应该有哪些手指"（绝对状态）**，由 Kotlin 侧唯一的状态机去和上一次状态做差分、
自己算出 `ACTION_DOWN / POINTER_DOWN / MOVE / POINTER_UP / UP`。
**没有任何调用方自己去拼 MotionEvent。**

---

## 4. 核心：多指正确性契约（不跳屏的规则）

> 这 10 条是整份文档的重点。违反任意一条都可能重现"多指跳屏"。

1. **唯一状态机、唯一写者。** 所有产生触摸的来源（mirror、自瞄、扳机、自动化点击）都通过同一个
   `PointersState`，提交前先合并成一份完整的"期望指针集"。**永远不允许用部分指针列表构造事件。**
2. **每一个 MotionEvent 都必须携带当前所有已按下指针**，顺序固定（插入序），不得只带变化的那个。
3. **`PointerProperties.id` 是本地下标（0..9），随指针一生不变。**
   指针还按着时**绝不复用它的 id**（取 `nextUnusedLocalId()`）。外部 id（面板 tracking id / 我们的
   `10+slot`）只用来做映射，不进 `MotionEvent`。
4. **action 由差分推出，一次提交只出一个 action：**
   | 变化 | action |
   | --- | --- |
   | 0 → 1 | `ACTION_DOWN`，同时 `downTime = now` |
   | n → n+1 | `ACTION_POINTER_DOWN \| (index << ACTION_POINTER_INDEX_SHIFT)` |
   | n → n-1 | `ACTION_POINTER_UP \| (index << ACTION_POINTER_INDEX_SHIFT)`；若 n-1 == 0 再补 `ACTION_UP` |
   | n → n（仅坐标变） | `ACTION_MOVE` |
   | 0 → 0 | 什么都不发 |
5. **`downTime` 从第一次 `ACTION_DOWN` 起，整个手势期间恒定**（跨指针增减也不变）；
   **`eventTime = SystemClock.uptimeMillis()`**，**绝不能是未来时间**。
6. **不要"背景手指"（background finger）。** 流以一个真实的 `ACTION_DOWN` 开始。
   （老项目用一个常驻 `BG_ID` 在 (5,5) 撑着手势 —— 见 §5.2，那是多余的幽灵指针。）
7. **固定字段：** `source = SOURCE_TOUCHSCREEN`、`toolType = TOOL_TYPE_FINGER`、`buttonState = 0`、
   `metaState = 0`、`edgeFlags = 0`、`deviceId = 0`、`flags = 0`、`xPrecision = yPrecision = 1f`。
8. **`ACTION_POINTER_UP` 的 index 是指针在"本事件数组里"的位置** —— 该数组**仍包含正在抬起的那个指针**。
   所以：先算 index → 生成事件 → 再把它从状态里移除。顺序反了就会指错手指。
   （scrcpy 的 `PointerState.getPointerIndex()` → `update()` 填 props/coords → `cleanUp()` 在最后移除，就是这个顺序。）
9. **上限 10 指**（`MAX_POINTERS = 10`），超出的直接丢弃并只记一次日志。
10. **`MotionEvent.obtain(...)` 之后必须 `recycle()`**（这是池化对象，不回收会造成 native 泄漏）。

---

## 5. 老项目（aimbot 1.2.1）为什么会"多指跳屏"

逐行读 `injector/TouchInjector.kt` + `service/RemoteInjectorService.java` 后的结论：

### 5.1 根因：**没有 id 注册表，随机 id 会撞车**（最主要）

```kotlin
val tapId = { var n: Int; do { n = 7 + (Math.random()*3).toInt() } while (n == lastTapId); lastTapId = n; n }()
```

`tap()` 和 `swipe()` 都从 `{7,8,9}` 里随机取 id，**唯一的约束只是"不等于上一次用过的 id"**。
而 `drawingPointerId`（正在按住的那根）只在 `moveTo/lift` 里被判空，**从不参与这项约束**。

⇒ 结果：一次 `swipe` 还按着（`drawingPointerId = 8`）时来了一个 `tap`，它可能同样取到 8 ——
**同一帧里两个逻辑手指共用一个 `PointerProperties.id`**，系统认为它们是同一根手指，
于是在两个坐标之间来回跳。**这就是"两根虚拟手指跳屏"。**

### 5.2 幽灵指针：常驻背景手指

`init()` 里发一个 `ACTION_DOWN`（id=10）在 `(5,5)`，之后**永不抬起**，所有后续事件都是
`ACTION_POINTER_DOWN/UP`。后果：**每一个事件都带 2 个指针**，其中一个永远贴在屏幕左上角。
任何做多指几何计算的应用（捏合、双指旋转、手势识别）都会看到这根多余的"手指"。

### 5.3 `eventTime` 用了未来时间

```kotlin
MotionEvent.obtain(bgDownTime, now + 8, …)      // tap 的 UP
MotionEvent.obtain(bgDownTime, now + durationMs, …) // swipe 的 MOVE
```

`InputDispatcher` 对 `eventTime` 有单调性要求，发未来时间戳属于未定义行为（可能被丢或乱序）。

### 5.4 `ACTION_POINTER_*` 的 index 写死成 1

```kotlin
val shift = 1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT
…  ACTION_POINTER_DOWN or shift      // 永远是 index=1
```

只在"背景手指永远排第 0 位、新指针永远排第 1 位"这个前提下成立。**一旦同时有 3 根手指就不对了。**

### 5.5 缺少 DOWN/UP 配对纪律

因为背景手指永不抬起，代码里**没有**"最后一根手指抬起时补 `ACTION_UP`"的逻辑；
一旦背景手指那条路出问题（比如 `destroy()` 没被调到），整个触摸会话就悬在那儿。

> **本方案 §4 的 10 条规则，逐条对应上面 5 个缺陷。** 实现时把这张表当 checklist。

---

## 6. scrcpy 的做法（照抄的部分）

`G:\scrcpy`（本地源码），关键文件：

| 文件 | 作用 |
| --- | --- |
| `server/src/main/java/com/genymobile/scrcpy/wrappers/InputManager.java` | 反射封装：`android.hardware.input.InputManager.class.getMethod("injectInputEvent", InputEvent.class, int.class)`；通过 `FakeContext.get().getSystemService("input")` 拿实例 |
| `server/src/main/java/com/genymobile/scrcpy/control/PointersState.java` | **指针状态机**：`getPointerIndex(id)` 分配 `localId`、`update(props, coords)` 填充、`cleanUp()` 移除已抬起的 |
| `server/src/main/java/com/genymobile/scrcpy/control/Controller.java` `injectTouch()` | **action 推导**：`pointerCount == 1 ? ACTION_DOWN : (POINTER_DOWN\|index<<SHIFT)`；`lastTouchDown` 管 `downTime` |
| `server/src/main/java/com/genymobile/scrcpy/device/Device.java` `injectEvent()` | 非 0 displayId 才调 `setDisplayId` |

**可直接借用的三点：**
1. `nextUnusedLocalId()` 式的外部 id → 本地 id 映射（解决 §5.1）。
2. `PointersState.update()` 里"先填 props/coords、最后 cleanUp"的顺序（解决 §4.8）。
3. `lastTouchDown` 作为整个手势的 `downTime`（解决 §4.5）。

**不需要的：** 背景手指、mouse/button 分支、`setActionButton`、多 display 的 `setDisplayId`
（我们只注入主屏，displayId 恒为 0，连 `setDisplayId` 都不用调）。

---

## 7. 代码落点（文件级计划）

### 7.1 新增

```
app/src/main/java/io/github/xiangsu1145/aimbotnextgen/inject/
    TouchBackend.kt            枚举 + 从 config 选择/持久化
    PointersState.kt           §4 的状态机（照 scrcpy 重写，去掉 mouse/BG）
    InputManagerInjector.kt    反射封装 + 状态机 + pushFrame() 入口
app/src/main/cpp/input/
    im_bridge.h / im_bridge.cpp  反向 JNI：native → InputManagerInjector.pushFrame()
```

### 7.2 修改

| 文件 | 改动 |
| --- | --- |
| `cpp/input/touch_reader.cpp` | `publishPointers()` 里按后端分支：uinput 走 `uinput_mirror_physical()`；InputManager 走 `im_bridge_emit(...)`，**传屏幕像素 `xs/ys`（不是 raw）** |
| `cpp/input/input_jni.cpp` | `JNI_OnLoad` 里缓存 `InputManagerInjector` 的 jclass/jmethodID（沿用 `skip_screenshot.h` / `exit_request.h` 那套现成机制） |
| `java/.../shell/ShellServerEntry.kt` | OPEN 时按 config 初始化后端；InputManager 后端失败要能回退（见 §9.3）；shutdown 时销毁 |
| `cpp/ui/gui/sections/settings_section.cpp` | 新增一行下拉/开关「触摸注入方式: uinput / InputManager」 |
| `cpp/config/config_manager.cpp` | 持久化 `"inject": "uinput" \| "inputmgr"` |
| `java/.../shell/ShellNative.kt` | 新增 `imSetBackend` 等 JNI 声明（如需） |

### 7.3 反向 JNI 的调用形态（避免每帧分配 jintArray）

native 侧持有 3 个**已创建的 `jintArray`（长度 10）global ref**，每帧只做
`SetIntArrayRegion` + `CallStaticVoidMethod`，**零分配**：

```kotlin
// InputManagerInjector.kt
@JvmStatic
fun pushFrame(ids: IntArray, xs: IntArray, ys: IntArray, n: Int) { … }
```

Kotlin 侧：`ids` 是外部 id（面板 tracking id / `10+slot`），`xs/ys` 是**屏幕像素**。

### 7.4 自瞄 / 融合语义（比 uinput 更简单）

uinput 后端里「touch fusion」要靠 `uinput_takeover_physical_id()` 预占 slot +
`g_panelIds[]` 按身份排除，因为 mirror 是"写 slot"的、两个写者会抢同一个 slot。

**在"绝对状态"模型下这件事自然消失**：自瞄只需**覆盖期望指针集里某一根手指的 x/y**，
再提交一次。没有第二个写者、没有 slot 分配、也就没有 `takeover` / `release_takeover` 这套东西。
`INJECT_SLOT` / `UINPUT_ID_PRIMARY` 等常量在 InputManager 后端下完全不用。

---

## 8. 与 uinput 后端的共存

- `config.json` 新增 `"inject": "uinput" | "inputmgr"`，**默认 `"uinput"`**（一加等设备上工作正常，不能回归）。
- 设置页新增一行可切换；**切换立即生效**（和「独占触摸面板」「允许触摸穿透」同一节奏，
  在 `syncSettingsPage()` 里按过渡下发）。
- 两个后端**共享** reader / grab / 吞触摸 / 菜单矩形 —— 只有"最后一跳"不同。
- **grab 保留**：既要读菜单，也要压掉物理手指，否则会和注入的点叠成两个。

---

## 9. 风险与应对

### 9.1 `INJECT_EVENTS` 权限（**最需要先验证的一条**）

`injectInputEvent` 要求调用者 uid 持有 `android.permission.INJECT_EVENTS`。
AOSP 里 `com.android.shell` 有这个签名权限，**但 MIUI/HyperOS 把它藏在
「USB 调试（安全设置）」开关后面** —— scrcpy 源码里专门为这个写了提示：

```java
Ln.e("Make sure you have enabled \"USB debugging (Security Settings)\" and then rebooted your device.");
```

**应对：**
- 调用时捕获 `SecurityException`，若 message 含 `INJECT_EVENTS permission`，
  **打一条 `MILESTONE` 级日志**（E 级，任何过滤都抓得到）并附上上面那句可操作提示；
- 在设置页那一行旁边用文字显示当前状态（"注入失败：需要打开 USB 调试(安全设置)"）；
- 这是**极可能的一次性开关问题**，值得在给测试者的说明里单独写一行。

### 9.2 延迟

多一次 JNI 跳（native → Kotlin）+ 一次 `MotionEvent.obtain`。
`obtain` 是池化的，scrcpy 在 60fps 下毫无压力；我们按 100+ 帧/秒估算，
额外开销是每帧一次对象获取/回收。**除非实测到掉帧，不预先优化**；
真要优化就把"每帧都发 MOVE"改成"坐标没变就不发"。

### 9.3 失败回退

InputManager 后端初始化失败（拿不到 binder / 反射失败 / 权限异常）时：
- **不静默降级**（这是本工程的既有原则：不掩盖故障）；
- 打 `MILESTONE` 错误日志，UI 上明确显示"注入不可用"；
- 是否自动切回 uinput **由用户裁决**（默认不自动切，避免症状消失后无人知道原因）。

### 9.4 坐标系

`MotionEvent` 要的是**屏幕像素（display 逻辑坐标系、已含旋转）**。
好消息：reader 里 `panelToScreen()` 产出的就是它，`rebuildPointersLocked()` 已经同时算了
`sx/sy`（屏幕）和 `rawX/rawY`（面板）。**InputManager 用 `sx/sy`，uinput 用 raw** —— 两套并存，不要混。

### 9.5 与「独占触摸面板」开关的关系

`exclusive = false` 时我们既不 grab 也不回注（`publishPointers(g_sink && g_exclusive)`）。
InputManager 后端下这个开关语义不变：**仍然只在 grab 生效时注入**，否则原生触摸 + 注入 = 双点。

---

## 10. 实施步骤（建议顺序）

1. **权限探针（最小验证，先做）**
   在 daemon 里加一段一次性探针：反射拿 `InputManager`，`injectInputEvent` 一个
   `ACTION_DOWN/UP` 在 (2,2)，把结果 + 异常原文打成 `MILESTONE`。
   **先在一加上跑通，再发给测试者。** 这一步就能确认 §9.1。
2. `PointersState.kt`（纯逻辑，可单测）。
3. `InputManagerInjector.kt`（反射 + 状态机 + `pushFrame`）。
4. `im_bridge.cpp`（反向 JNI，零分配）。
5. `publishPointers()` 分支 + config + 设置页开关。
6. 自瞄接进同一个状态机（替掉 uinput 路径的 takeover）。
7. 一加回归（两种后端都测）→ 再给测试者。

---

## 11. 验收判据

| 项 | 判据 |
| --- | --- |
| 单指 | 桌面滑动 / 打开 App / 长按，全部正常 |
| **多指（重点）** | **两根手指同时按住并各自拖动，两个点各自跟随、互不干扰、不跳屏**；再试三指 |
| 菜单 | 悬浮层菜单仍可点击；菜单外手势仍原样透传 |
| 自瞄 | 自瞄移动与真实手指不冲突（不会出现两个点抢位） |
| 日志 | `MILESTONE` 里出现注入成功；无 `SecurityException`；无 `Too many pointers` |
| 回归 | 一加设备上 uinput 后端行为与改动前一致 |

---

## 12. 待决问题（需要用户裁决）

1. **默认后端**：建议保持 `uinput`（不动一加），测试者手动切到 InputManager。
2. **切换入口形态**：「独占触摸面板」是开关，「注入方式」是二选一 —— 用开关（关=uinput）还是下拉？
3. 若 InputManager 在所有设备上都不比 uinput 差，后期是否干脆只留一条路？（本方案按"两条并存"规划。）
4. `exclusive = false` 且需要注入时（不 grab 但想注入），要不要允许？现在的语义是"不 grab 就不注入"。

---

## 附录 A：关键常量

```
INJECT_INPUT_EVENT_MODE_ASYNC = 0            // scrcpy 用的就是它；不要用 WAIT_FOR_FINISH
MAX_POINTERS = 10
ACTION_POINTER_INDEX_SHIFT = 8
SOURCE_TOUCHSCREEN / TOOL_TYPE_FINGER
INJECT_EVENTS = android.permission.INJECT_EVENTS   // uid 2000 需要它
```

## 附录 B：参考文件清单

```
G:\scrcpy\server\src\main\java\com\genymobile\scrcpy\
    wrappers\InputManager.java        反射封装 + 权限错误提示
    control\PointersState.java        指针状态机（localId 分配 / cleanUp 顺序）
    control\Controller.java           injectTouch()：action 推导 + downTime
    device\Device.java                injectEvent()：setDisplayId + 调用
    FakeContext.java                  getSystemService("input") 的来源

G:\ai\Aimbot-ai\android-client\app\src\main\java\team\maodie\aimbot\
    injector\TouchInjector.kt                老实现（§5 的 5 个缺陷全在这里）
    service\RemoteInjectorService.java        另一份 InputManager 分支（BG_ID = 10）
G:\ai\Aimbot_Nextgen\app\src\main\java\io\github\xiangsu1145\aimbotnextgen\
    shell\ShellLayerHost.kt                   证明 daemon 已能用隐藏 API 反射
```
