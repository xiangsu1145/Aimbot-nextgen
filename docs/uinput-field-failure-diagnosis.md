# uinput 大面积失效：老项目行 / 新项目不行 的代码级定位

> 日期：2026-09-16
> 现象：老项目（aimbot 1.2.1，Shizuku + 无线调试，uid 2000，**uinput**，log 确认无静默回退）别人都能用；
> 新项目（Aimbot Nextgen，`app_process` daemon，**同样 uid 2000**，同样 uinput）只有作者和少数人能用。
> 前置排除：不是 root vs ADB（老项目用户没有 root）。

---

## 0. 一句话结论

**同一个 uid 2000、同一份 uinput 代码，但"喂给这份代码的入参"完全不同 ——**
新项目把「能读到物理面板」变成了 uinput 的**硬前提**，老项目把它**当成可选装饰**。

- 读不到面板的设备：老项目照常建虚拟设备（随机身份、4 个轴）；**新项目直接中止，连设备都不建。**
- 读到面板的设备：老项目仍然走「无 clone」退化分支；**新项目改走「克隆真面板」分支** —— 于是产出一个和 1.2.1 完全不同的设备。

**你感觉"上下文不同"是对的，但上下文的具体内容是「uid 2000 能不能读 `/dev/input`」，不是 system_server。**
而"少数人能用的那几台"，正是两件事都满足的设备。

---

## 1. 硬伤一：新项目里 uinput 被面板绑成了硬门（老项目从来没这个门）

### 1.1 证据链（新项目）

`app/src/main/java/.../shell/ShellServerEntry.kt:946-951`：

```kotlin
if (!ShellNative.readerIsReady()) {
    if (!ShellNative.readerInit(screenW, screenH, rotation)) {
        Log.e(TAG, "OPEN: readerInit FAILED at ${screenW}x$screenH rot=$rotation")
        replyErr("readerInit failed (no touch panel?)")
        return                      // ← 整个 OPEN 在这里中止
    }
}
...
} else if (!ShellNative.uinputIsReady()) {
    if (!ShellNative.uinputInit(screenW, screenH)) { ... }   // 第 996-1009 行，永远执行不到
}
```

`return` 之后的两件事同时没了：

| 丢的东西 | 后果 |
| --- | --- |
| `uinputInit()`（第 997 行） | **虚拟触摸设备根本不存在 → uinput 注入 100% 死** |
| `startReaderThread()`（第 1019 行） | 菜单触摸也没了（菜单靠 `readerPoll` + `readerReadPointers` 喂 ImGui） |

所以失败用户的真实体感是：**悬浮菜单点不动 + 触摸完全没反应**，而不是"菜单能点但自瞄不跟枪"。

### 1.2 `readerInit` 会在哪些设备上失败

`app/src/main/cpp/input/touch_reader.cpp`：

| 位置 | 条件 | 说明 |
| --- | --- | --- |
| `:428-432` | `opendir("/dev/input")` 失败 | 直接返回空表 |
| `:462-463` | `open("/dev/input/eventN")` 失败 | **静默 `continue`**，该设备被当成"不存在" |
| `:369-395` | `probeTouchDevice()` 要求 **ABS_MT_SLOT + POSITION_X + POSITION_Y 三者齐全** | `if (!hasX \|\| !hasY \|\| !hasSlot) return false;` |
| `:757-760` | 枚举结果为空 | `LOGE("no touch panel found under /dev/input")` → false |
| `:784-795` | `open` / `probe` 失败 | → false |

两条独立的失败面：

1. **权限面**：uid 2000 打开 `/dev/input/eventN` 被拒（`input` 组缺失 / SELinux `u:r:shell:s0 → input_device` 不放行）。
   → `open` 失败被静默跳过 → 表空 → `readerInit` false。
2. **协议面**：面板是 **Protocol A（无 ABS_MT_SLOT）** → `probeTouchDevice` 一律拒绝。
   → 表空 → `readerInit` false。

> 附带的死代码：`probeTouchDevice` 强制要求 `hasSlot` 为真，所以 `reader_init_with_path` 里
> `hasSlot ? "B(slot)" : "A(stream)"` 的 `A(stream)` 分支**永远不可能被打出来**，
> `decodeBatch(..., useSlots=false)` 的协议 A 解码器在当前版本里是够不着的。
> `docs/inputmanager-injection-plan.md` 里"不改 reader 的解码（协议 A/B 原样）"这句话与实现不符 —— 协议 A 设备连门都进不来。

### 1.3 老项目在同一位置的写法（从不设门）

`G:/ai/Aimbot-ai/android-client/app/src/main/cpp/src/injection/touch_core.cpp`：

```cpp
// :675-679  探测不到就用屏幕尺寸兜底，不失败
if (detectTouchDeviceViaGetevent(touchPath, sizeof(touchPath), touchMaxX, touchMaxY)) {
    LOGD("Using touch device ABS: %dx%d", touchMaxX, touchMaxY);
} else {
    LOGD("No touch device detected, using screen size as ABS: %dx%d", touchMaxX, touchMaxY);
}
// :684-700  fd=-1 只是"没有真 fd 可读"，设备条目照插
// :694-698  grab 失败 → LOGE("grab %s failed, physical touches not suppressed")，仅此而已
// :712      createUinputDevice(touchMaxX, touchMaxY, cloneFd) ← 无论如何都会建
```

而且它把"读不到面板"这件事**写进了设计注释**（`:249-252`）：

```cpp
// Detect physical touch device via `getevent -p` text output.
// Avoids direct /dev/input/eventX open — Shizuku UserService process has no
// SELinux permission for that, but can execute `getevent` which reads kernel
// info on its own.
```

**老项目作者是明确撞过这堵墙的，并且把"不读面板"当成必须遵守的约束来做设计。新项目把这个约束整个丢掉了。**

---

## 2. 硬伤二：reader 成功时，新项目造的虚拟设备 ≠ 1.2.1 造的虚拟设备

`docs/inputmanager-injection-plan.md` §1.1 写「`touch_core.cpp` 全部 842 行逐段对完，除加载顺序外**无第二处差异**」。

**代码确实逐段一样，但结论是错的** —— 因为决定行为的不是代码，是 `createUinputDevice(..., sourceFd)` 的**第 3 个参数**。

| | 老项目（Shizuku / ADB） | 新项目（能读面板时） |
| --- | --- | --- |
| `cloneFd` / `sourceFd` | **恒为 -1**（UserService 读不到 `/dev/input`，grab 与临时 `open` 双双失败） | **真面板 fd**（能读面板是它启动的前提） |
| 走的分支 | 无 clone 退化分支 | clone 分支 |
| `name` | 15 位随机串 | **真面板名**（如 `focaltech_ts`） |
| `id` (bustype/vendor/product/version) | `BUS_I2C(0x18)` + 随机 5..14 | **真面板的 EVIOCGID** |
| `INPUT_PROP_*` | 只 `INPUT_PROP_DIRECT` | 真面板全部 prop |
| `EV_ABS` 声明轴 | 只有 `ABS_MT_SLOT` / `POSITION_X` / `POSITION_Y` / `TRACKING_ID` | **真面板全部 ABS 轴** |
| `EV_KEY` 声明 | 只有 `BTN_TOUCH` + `BTN_TOOL_FINGER` | **真面板全部 KEY 位** |
| `g_pressure_max` / `g_touch_major_max` / `g_width_major_max` | **全部 0** | 真面板的 max |
| 每帧实际发出的事件 | `SLOT, [TRACKING_ID], POS_X, POS_Y` + 尾 `BTN_TOUCH, 5×BTN_TOOL_*, SYN` | 上面这些 **+ `ABS_MT_PRESSURE` + `ABS_MT_TOUCH_MAJOR` + `ABS_MT_WIDTH_MAJOR`** |

即：**"老项目大家都在用、生产验证过的那个虚拟设备"，是随机名 + BUS_I2C + 4 个轴 + 不发压力/接触面积的版本。
新项目默认产出的是它的反面（真面板克隆 + 多发三轴）。**

老项目的 clone 分支在 ADB 模式下**从未在生产环境被执行过** —— 也就是说"克隆出来的设备在真机上到底能不能被投递"这件事，
老项目从来没验证过（root 模式才可能走到）。新项目却把它变成了默认路径。

### 2.1 其中一处具体可查的坑：压力值为 0

`uinput_inject.cpp:420-422`：

```cpp
if (g_pressureMax > 0)
    count = pushEvent(count, EV_ABS, ABS_MT_PRESSURE,
                      randInRange(g_pressureMax / 333, g_pressureMax / 40));
```

`randInRange(lo,hi)` 在 `hi <= lo` 时返回 `lo`。所以：

| 面板 `ABS_MT_PRESSURE.max` | 实际发出范围 | 结果 |
| --- | --- | --- |
| 1 | `randInRange(0, 0)` | **恒为 0** |
| 255 | `randInRange(0, 6)` | 经常 0 |
| 1023 | `randInRange(3, 25)` | 正常 |
| 4096 | `randInRange(12, 102)` | 正常 |

而 pressure / contact-area 为 0 在输入栈里的语义是"没有接触"。**老项目因为 `g_pressure_max` 恒为 0，这三轴一个都不发，天然绕开了这个坑。**
（对照：`docs/inputmanager-injection-plan.md` §1.1 小米那台"`dumpsys input` 逐项与真面板相同、设备 `Enabled: true`、帧在写、`write_failures=0`，系统层零反应"—— 与"设备被接纳但事件语义不被接受"完全吻合。）

> ⚠️ 这一条是**待验证的差异点**，不是已定罪。定性方法是把它当 A/B 变量（见 §4），
> 因为克隆身份（name/id）本身也可能是被过滤的原因。两者的共同点是：**都是新项目独有、老项目在生产里从未出现过的差异。**

---

## 3. 为什么"少数人能用的那几台"恰好能用

新项目要跑通 uinput，必须同时满足：

1. uid 2000 能 `open("/dev/input/eventN")`（否则卡在第 1 步 `readerInit`）
2. 面板是 Protocol B（否则 `probeTouchDevice` 拒绝）
3. 该 ROM 对"克隆成真面板的虚拟触摸设备"不做事后过滤（否则 §2 现象）

你那台一加（ColorOS，面板 21199×29999）三条都满足 → 能用。
老项目只需要第 3 条里"随机名设备不被过滤"这一条（而且连读面板都不需要）→ 所以它几乎无往不利。

**这就是"老项目人人能用、新项目少数人能用"的分布来源。**

---

## 4. 一步定性（下次用户反馈只问这两件事）

### 4.1 问用户：**悬浮菜单能不能点？**

| 现象 | 定性 | 位置 |
| --- | --- | --- |
| 菜单**点不动**，触摸全无 | `readerInit` 失败，OPEN 中止，虚拟设备没建 | §1 |
| 菜单**能点**，但游戏/桌面收不到触摸 | 虚拟设备建了且被接纳，事件没落地 | §2 |

### 4.2 让他贴这几行 log

```
# /data/local/tmp/aimbot_daemon.log   （或 logcat -s aimbot_shell:V）
OPEN: readerInit FAILED              → §1，权限面或协议面
no touch panel found under /dev/input → §1
uinput ready: ... clone='...' abs=[...]   → §2：看 clone 指向谁、声明了哪些轴
health ...ms: frames=+N/M failures=0 ...  → 帧在涨、无失败 ⇒ 故障在设备之外（§2）
uinputInit failed (cannot open /dev/uinput?)  → /dev/uinput 本身不可写
```

### 4.3 让他跑（一次性、只读）

```bash
P=$(pidof aimbot_shell)
cat /proc/$P/status | grep -E "^(Uid|Gid|Groups)"   # 有没有 input(1004)
cat /proc/$P/attr/current                            # u:r:shell:s0 ?
ls -l /dev/input/                                    # 权限
ls /dev/input/event* | while read e; do echo -n "$e "; getevent -p "$e" 2>/dev/null | grep -c ABS_MT_SLOT; done
getevent -p | grep -A2 -iE "PRESSURE|TOUCH_MAJOR"    # 压力 max 是多少（验证 §2.1）
```

---

## 5. 修法（按你的习惯：先方案，确认后再改）

### P0 — 让 uinput 不再依赖 reader（恢复 1.2.1 的容错）

`ShellServerEntry.kt:946-951`：`readerInit` 失败**不要 return**，降级继续。

- `uinput_init()` 本身已经自带三级退化：`g_sourcePanel` → `getevent -p` → 屏幕尺寸；`cloneFd < 0` 时也照常
  `createUinputDevice()`（`:836-878`）。**退化路径是现成的，只是被上层的 `return` 拦死了。**
- 代价：reader 挂了 ⇒ 镜像/吞触摸/菜单触摸都不可用，但**注入本身活着**（= 老项目的行为）。
- 建议给一个显式开关（如 `injectRequiresPanel`，默认 off = 容错），而不是无条件二选一。

### P1 — 让虚拟设备身份可切换（对齐生产验证过的那个版本）

把「克隆真面板」与「随机身份」做成配置项，默认走**随机身份 + 只发 4 个核心轴**（= 老项目生产验证版本）：

- `name`/`id`/`props` 克隆开关；
- `ABS_MT_PRESSURE` / `TOUCH_MAJOR` / `WIDTH_MAJOR` 的发送开关，默认**关**；
- 若保留发送，必须给下限：`max/333` 换成 `max(1, max/333)`，禁掉 `pressure = 0`（§2.1）。

### P2 — 把协议 A 放进来（当前是死码）

`probeTouchDevice` 目前要求 `hasSlot`。若要真支持协议 A：
- 主屏判定改为"有 XY 且 `EVIOCGABS` 有效"，并用**排除法**剔除屏下指纹辅助设备（而非靠"必须有 SLOT"一刀切）；
- 否则就把 `reader_init_with_path` 里 `A(stream)` 的分支与 `decodeBatch(useSlots=false)` 的注释改成"当前不可达"，避免下一个人误以为它受支持。

---

## 6. 待办

- [ ] 拿一个失败用户的 §4.1 答案 + §4.2 log，确认是 §1 还是 §2（**这是唯一还没落地的证据**）
- [ ] P0 改动（约 10 行）
- [ ] §2 的 A/B 验证：同一台设备上，关掉身份克隆 / 关掉三轴，看是否恢复
