# InputManager 后端 — 现场测试日志采集

给测试用户的操作说明。目的：让每份反馈都带上**能定位问题的数字**，而不是"用不了"三个字。

---

## 0. 前置条件（缺一不可）

1. **必须是新 APK**（含重写后的 InputManager 后端）。
2. **菜单里「触摸方式」必须选 `InputManager`。**
   ⚠️ 默认值是 `uinput`（`uinput_inject.cpp:126`）。不切的话走的还是老路，抓出来的 log 对新后端没有任何参考价值。
   切换后**要停一次服务再启动** —— 后端在 `OPEN` 时决定，热切不生效。

> 顺便确认一下：菜单里「触摸方式」旁边如果没有 `(注入不可用)` 字样，说明后端自检通过；有这行字说明注入本身就没起来，可以直接抓 log 了。

---

## ⚠️ 采集的头号陷阱：tag 名称差一个字母

第一次实际采集就栽在这里，务必看清。日志分布在**五个不同的 tag** 上：

| tag | 来源 | 里面有什么 |
|---|---|---|
| `AimbotInject` | Kotlin 侧 IM 后端 | init（路线/方法/deviceId）、首注、周期统计 `im[...]` |
| **`AimbotInput`** | **native 注入层** | **`health …frames= down= mirror= aim=`**、`uinput ready/created/teardown`、所有注入失败 |
| **`AimbotReader`** | **native 读取层** | 面板发现、`EVIOCGRAB`、`no touch panel found` |
| `aimbot_shell` | daemon 主流程 | `MILESTONE …`（OPEN 全流程，E 级不会被刷掉）、`inject tick` |
| **`InputDispatcher`** | **系统** | **`Untrusted touch due to occlusion by …`** ← 一票否决级判据，见 §3.0 |

**`AimbotInject` ≠ `AimbotInput`** —— 只抓前者等于丢掉了**最能定位问题的那一半**（`health` 行）。别再按 tag 列了，见下面的命令。

---

## 1. 采集（三步）

### 第 1 步 — 清空日志缓冲

```bash
adb logcat -c
```

### 第 2 步 — 开始抓取（命令会一直挂着，先别动）

```bash
adb logcat | grep -iE "aimbot|ShellManager|InputDispatcher" > im.log
```

> 用 `grep -i "<关键字>"` 而不是 `-s TAG:TAG`：一次覆盖上面五个 tag（大小写不敏感），不会再漏。
> 精确等价的 tag 写法是 `-s AimbotInject:V AimbotInput:V AimbotReader:V aimbot_shell:V ShellManager:V InputDispatcher:V`。

### 第 3 步 — 在手机上依次做完这几件事

1. App 里**停止**服务，再**启动**服务（必须做，init 日志只在这时打一次）
2. 打开菜单，确认「触摸方式」= `InputManager`，**菜单保持打开**
3. **先测"手点有没有反应"**：手指直接点桌面 / 应用列表，滑一滑 —— 这一步决定问题在镜像还是在自瞄
4. 进游戏，**用手指正常滑动、点按 30 秒**
5. 如果自瞄有效果，让它跑十几秒（产生注入帧 + 周期统计行）

然后回到电脑按 **Ctrl + C** 结束抓取。`im.log` 就是反馈文件。

> **手机终端用户请直接看 §5** —— 那边只有一条命令，不用电脑、不用 adb。

---

## 2. 附一条环境信息（很重要，一条命令）

权限/组/SELinux 域的问题只有这条能看出来：

```bash
adb shell "id; P=\$(pidof aimbot_shell); echo \"pid=\$P\"; cat /proc/\$P/attr/current; grep -E '^(Uid|Gid|Groups)' /proc/\$P/status; ls -lZ /dev/uinput /dev/input/uinput 2>&1; getprop ro.build.version.sdk" > env.txt
```

正常的输出长这样（uid/gid 都应是 `2000`，域是 `u:r:shell:s0`）：

```
uid=2000(shell) gid=2000(shell) groups=2000(shell),1004(input),3003(inet),...
pid=12345
u:r:shell:s0
Uid:	2000	2000	2000	2000
Gid:	2000	2000	2000	2000
Groups:	2000 1004 3003 ...
crw-rw---- 1 root uhid u:object_r:uhid_device:s0 10, 223 /dev/uinput
```

---

## 3. 判读表（收到 log 后怎么一眼定位）

### 3.0 🚨 先看这条 —— 一票否决级判据

```
W InputDispatcher: Untrusted touch due to occlusion by /2000/[BBQ] aimbot-ui#…
```

**只要出现这一行，其它判据全部作废** —— 触摸是被**系统**丢的，与 IM/uinput 的实现、坐标系、拟真、权限都无关。

- 含义：Android 12+ 阻止"穿过窗口"的触摸。我们的悬浮图层是全屏、`alpha=1`、非可信窗口 ⇒ 穿过它的触摸全被判为"不可信触摸"丢弃。
- 特征：**悬浮菜单开着时全屏点不动**（菜单本身能点，因为菜单走 reader→ImGui，不经过系统派发）。
- 决定因素：图层是否存在。图层没建时（服务没起 / 菜单已关）触摸正常。
- 处置：见 `docs/untrusted-touch-occlusion.md`（`settings put global block_untrusted_touches 1` 可即时验证）。

---

### 3.1 `AimbotInject` — IM 后端自身

| 日志 | 含义 | 结论 |
|---|---|---|
| `InputManager injector ready` + `route: IInputManager binder` + `method: injectInputEvent(InputEvent, int)` | 后台就绪，AIDL 直连成功 | ✅ 正常 |
| `no two-argument injectInputEvent on ...` → `falling back to ...` | 二参不见了，回退到别的签名 | ✅ 回退生效（正常） |
| `... exposes no usable inject method (...)` | 该类**没有任何**可用注入方法 | ❌ 此 ROM 封了这条路，括号里是它有哪些方法 |
| **`first injection accepted (action=3, …)`** | 系统接受了第一个事件（通常是 SDK≥36 的预取消 CANCEL） | ✅ 注入链路通 |
| **`first DOWN accepted: N pointer(s) at (x, y) device=…`** | **第一次真正的按下被接受，且带坐标** | ✅ 坐标应是**屏幕像素**（1280×2772 上是三位数）。出现五位数 ⇒ 坐标系错了，手指落在屏幕外 |
| `im[frames=F ok=N fail=M down=D up=U move=V …]` | 周期统计行（约每 200 帧 / 2 秒一行） | 见下 |
| `InputManager refused the event: ...` + `Enable "USB debugging (Security settings)"` | **权限被拒** | ❌ 让用户开「USB 调试（安全设置）」并重启 |
| `injectInputEvent threw ... SecurityException ... INJECT_EVENTS` | 同上，权限问题 | ❌ 同上 |
| `no device in N reports SOURCE_TOUCHSCREEN` | 没枚举到触摸屏，`dev=0` | ⚠️ 拟真降级，不致命（AOSP 不校验 deviceId） |
| `no input service — neither the IInputManager binder nor ...` | 连输入服务的两条路都断了 | ❌ 环境异常（多半不是 App 的问题） |

`im[...]` 怎么读：

- `frames` 不动 ⇒ `pushFrame` 没被调 ⇒ native 侧根本没有手指状态变化。
- `down` 有、`move=0` ⇒ 手指按下了但再没动过（自瞄锁住不动 / 镜像卡住）。
- `fail` 在涨 ⇒ 注入被拒，去看 §3.1 的拒绝原因行。
- `down / up` 不配对 ⇒ 手势流断在中间。

### 3.2 ⭐ native `AimbotInput` — 这一行最能定位

```
health 10000ms: frames=+N/M restates=R failures=F force_restate=0 down=D mirror=X aim=Y backend=inputmgr fd=-1 ready=1 take=-1 our=(none)
```

`mirror` = 当前按下的手指里**来自物理面板镜像**的个数，`aim` = **程序请求**（自瞄/App 命令）的个数。这两个数字把"注入没反应"一刀切成两种完全不同的故障：

| 现象 | 含义 | 下一步 |
|---|---|---|
| `frames=+0` 连续出现 | **没有任何手指状态变化**，upload 没被驱动 | 不是注入的问题：reader 没喂 / 自瞄没请求 |
| `down=0 mirror=0 aim=0` | 此刻没有任何手指 | 正常（空闲） |
| `mirror>0` 但屏幕没反应 | 物理手指在注入，系统不采信 | 查 grab/mirror 链、坐标、厂商过滤 |
| `aim>0` 但屏幕没反应 | 自瞄在注入，系统不采信 | 查自瞄目标坐标是否落在屏幕内 |
| `frames` 在涨但 `down` 一直是 0 | 调用了 upload 但没有手指 | 上层状态机把手指都抬了 |

> 这张表存在的原因：只用 IM 侧日志，`ok=1` 既可能是"镜像的一个手指"，也可能是"自瞄的一个手指"，两者故障方向相反。`mirror`/`aim` 分开计数就是为了不再靠猜。

### 3.3 `aimbot_shell` — daemon 主流程（MILESTONE 是 E 级，不会被刷掉）

| 日志 | 含义 | 结论 |
|---|---|---|
| `MILESTONE inject backend at startup: InputManager configLoaded=true` | 后端选对了 | ✅ |
| `MILESTONE inject backend at startup: uinput configLoaded=false (config.json unreadable …)` | **配置没保存过 ⇒ 走的是默认 uinput 后端，用户没测到 IM** | ❌ 让他按 §0 切后端 + 停/启服务，重抓 |
| `MILESTONE inject backend at startup: InputManager configLoaded=false` | 后端是 IM，但配置文件读不到 | ⚠️ 能用（默认值兜底），只是上次的选择可能没落盘 |
| ⭐ `inject tick: backend=… ready=… grabbed=… sink=… regions=… ui=… uinputFail=…` | **每 5 秒无条件一条**，不依赖有没有手指 | 见下 |
| `inject tick(im): im[route \| method \| dev \| frames=F ok=N fail=M \| down=D up=U move=V \| cancel_ok=…]` | IM 后端的累计计数，同样每 5 秒一条 | 见下 |
| `MILESTONE OPEN: inject=InputManager ready, no virtual touchscreen` | OPEN 成功，IM 就位 | ✅ |
| `MILESTONE OPEN: inject=InputManager NOT usable (原因)` | IM 初始化失败，**原因在括号里** | ❌ 括号内容直接对应 §3.1 |
| `MILESTONE OPEN: panel=… grabbed=true` | 物理面板**被独占** | ⚠️ IM 后端下同样会 grab：物理手指从此只能靠 mirror 经 IM 送回去 |
| `OPEN: readerInit FAILED at ...` | 读不到物理面板 → **整个 OPEN 中止**（IM 也拿不到） | ❌ 这一类设备连 IM 都到不了，是另一条待修的门 |
| `MILESTONE OPEN: uinputInit FAILED ...` | uinput 建设备失败 | ❌ 走的是 uinput 老路 |
| 完全没有 `MILESTONE OPEN:` | OPEN 命令根本没跑到 | ❌ App 侧没发命令 / 连接断了 |

> **为什么要有 `inject tick`**：其它每一条注入日志都是"计数器"，只在真的发生事情时才输出。
> 用户什么都没碰的会话，会**一行注入日志都没有** —— 看上去和"采集漏抓了"完全一样，
> 这个歧义已经浪费过两轮现场测试。`inject tick` 每 5 秒无条件打一条，
> `frames=F` 一行就能把这两者分开；顺带它把三个"物理手指还准不准则放行"的状态摆在明面上：
> `grabbed`（我们占着面板）、`sink`（镜像开没开）、`regions`（当前有几个菜单矩形在吞手势）。
>
> **`regions` 的判读**：0 = 全透传；菜单打开时是个位数（板上的控件数）；
> **如果它是"一个覆盖整屏的矩形"或数字异常大，那本身就是 bug** ——
> 在 grab 被独占的前提下，全屏吞手势 = 整块屏幕点不动，只剩菜单能用。
> 这正是"菜单能点、别的都不行"这类反馈的头号嫌疑。

### 3.4 一句话结论

- **`mirror>0` 且注入被接受，屏幕还是不动** ⇒ 不是 IM 的问题（事件已进 InputDispatcher），转查厂商过滤 / 坐标空间。
- **`frames=+0`、`down=0` 一直不变** ⇒ 问题在更上游（reader 或自瞄），跟注入无关。
- **`inject tick` 在滚但 `frames` 一直是 0** ⇒ 注入器活着，但**没有任何手指被推给它** ⇒ 要么没碰屏幕，要么 reader 没解出指头。
- **`regions` 是个覆盖整屏的数** ⇒ 手势全被菜单吞了（grab 下 = 整屏死），先查这个再查注入。
- 完全没有 `MILESTONE OPEN:` ⇒ 问题在连接层。
- `readerInit FAILED` ⇒ 面板门，IM 还没轮到上场。
- `NOT usable` / `refused` ⇒ IM 本身没起来，括号/后文就是原因。

### 3.5 别忘了问的那句话

**菜单能点 ≠ 镜像正常。** 菜单走的是 reader 直连 ImGui，与注入无关。所以在 IM 后端下必须单独问一句：

> **「手指直接点桌面 / 应用列表，有没有反应？」**

- **没反应** ⇒ grab 吞掉了物理触摸，而 mirror 没把它送回去（IM 后端下 uinput 设备已被销毁）⇒ 查 mirror 链。
- **有反应** ⇒ 镜像链正常，问题在自瞄那条链。

---

## 4. 收齐哪些文件

| 文件 | 必需 | 说明 |
|---|---|---|
| `im.log` | ✅ | 主证据（终端用户就是这一份） |
| 机型 + ROM 版本 | ✅ | 相机型差异很大，让对方顺手报一下 |
| `env.txt` / 环境那条输出 | ⬜ | 电脑用户跑 §2；终端用户跑 §5 末尾那条。有它判断权限/组/域最快 |
| `daemon.log` | ⬜ | `adb shell cat /data/local/tmp/aimbot_daemon.log > daemon.log`，内容与 logcat 基本重合，logcat 丢了才需要 |

---

## 5. 手机终端用户专用：就这一条命令

不用电脑、不用 adb。在手机终端里跑：

```bash
logcat -d | grep -iE "aimbot|ShellManager|InputDispatcher" > /sdcard/im.log
```

### 什么时候跑（顺序别反）

`-d` 的意思是「把此刻缓冲区里已有的日志导出来，然后立刻退出」—— 它**不会等你**。所以先做完手机上该做的，再跑命令：

1. 停服务 → 启服务
2. 菜单「触摸方式」选成 `InputManager`
3. **手指点一下桌面 / 应用列表，看有没有反应**（这一步最关键：菜单能点不代表触摸正常，菜单走的是另一条路）
4. 滑动、点按 30 秒
5. 进游戏，开自瞄跑十几秒

**做完马上跑上面那条。** 越晚跑，前面的 init 日志越容易被新日志挤掉。

跑完文件在**手机存储根目录**，名字 `im.log` —— 直接发送即可，不用做别的。

### 两个小情况

- **提示写不进去 / 没有权限** ⇒ 把路径换成 `/data/local/tmp/im.log`：
  ```bash
  logcat -d | grep -iE "aimbot|ShellManager|InputDispatcher" > /data/local/tmp/im.log
  ```
  然后用 `cat /data/local/tmp/im.log` 看内容、复制出来。
- **想边跑边看有没有抓到**（不写文件）⇒ 去掉 `>` 那截：
  ```bash
  logcat -d | grep -iE "aimbot|ShellManager|InputDispatcher"
  ```

> 注意：终端 App 如果拿不到 shell/root 身份，读不到别的进程日志（Android 10+ 的限制），命令会输出空行。那种情况只能走 adb 或 OTG。

### §2 的环境信息也能在终端里跑

把 `adb shell` 和外面的引号去掉，直接粘这一条：

```bash
id; P=$(pidof aimbot_shell); cat /proc/$P/attr/current; grep -E '^(Uid|Gid|Groups)' /proc/$P/status; ls -lZ /dev/uinput; getprop ro.build.version.sdk
```

### 后续可以做的（零门槛终极方案）

daemon 本身是 shell uid、有 `DUMP` 权限，可以用现成的命令通道执行
`logcat -d -s AimbotInject:V aimbot_shell:V > /data/local/tmp/im.log` 再 `cat` 回来显示 ——
用户在 App 里点一下就能复制，连终端都不需要。尚未实现，需要改代码。
