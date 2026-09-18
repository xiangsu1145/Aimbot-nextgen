# 注入被系统丢掉的真因：Untrusted touch due to occlusion

> 依据：`G:\qq\qq1\aShellYou__20260916133226.txt`（8541 行 / 2.6 MB，用户 13:32 采集）
> 设备：小米 HyperOS + MTK，display 1280x2772，面板 `/dev/input/event5` ABS 127999x277199，`deviceId 6 (focaltech_ts)`，SDK 36

## 1. 结论

**注入链路本身完全正常 —— 触摸是被系统在 `InputDispatcher` 里丢掉的，丢的原因是我们自己的悬浮图层。**

```
09-16 13:30:51.484  1981  2685 W InputDispatcher: Untrusted touch due to occlusion by /2000/[BBQ] aimbot-ui#31822#31823
                                                                                                        ↑ 我们的 SF 图层（uid 2000）
```

这一行在这个 log 里出现 **810 次**（13:30:51.484 → 13:31:48.720），精确覆盖图层存活的全时段。

## 2. 证据链：同一台设备、同一会话内的 A/B

| 时段 | 守护进程 | 我们的图层 | 结果 |
|---|---|---|---|
| 13:30:47.98 – 13:30:50.27 | 运行中，面板已 grab，IM 注入中 | **未创建** | **4 个手势全部送达** MainActivity（`MIUIInput: ViewRootImpl ... ACTION_DOWN/UP`，`moveCount:30/5/5`） |
| 13:30:50.30 – 13:31:18.3 | 同上 | 存在（13:30:50.301 `BLASTBufferQueue: [[BBQ] aimbot-ui#31822#0] constructor()`） | **0 个事件送达**；810 条 `Untrusted touch due to occlusion by /2000/[BBQ] aimbot-ui` |
| 13:31:18.3 – 13:31:23.8 | 已退出，面板已释放 | 无 | 物理触摸（`deviceId=6`）正常送达 |
| 13:31:23.8 – 13:31:48 | 第二次会话，面板已 grab | 存在（`aimbot-ui#31868`） | 同样 0 送达，继续被拦 |

同时可见注入侧一切正常：

```
AimbotInput: inject: backend = InputManager (uinput device destroyed)
AimbotInject: first DOWN accepted: 1 pointer(s) at (896, 2059) device=6
AimbotInject: im[... dev=6 | frames=800 ok=925 fail=0 | down=146 up=144 move=528 | cancel_ok=107 cancel_fail=0]
AimbotReader: gesture at 896,2059 -> passthrough          ← 207 条
```

`ok=925 fail=0` + 坐标在屏幕空间内（0..1280 / 0..2772）+ `fail=0`：**我们这侧没有任何问题，事件确实进了 `InputDispatcher` 并被派发给 app 的窗口**，只是随后被判为"不可信触摸"丢掉。810 次拦截 ≈ 207 个手势 ×（DOWN + N×MOVE + UP + 预清场 CANCEL）。

## 3. 机制（Android 官方行为变更原文）

Android 12 起（**与 targetSdk 无关**）：

> To preserve system security and a good user experience, Android 12 prevents apps from consuming touch events where an overlay obscures the app in an unsafe way… the system blocks touches that pass through certain windows.
> Logcat: `Untrusted touch due to occlusion by PACKAGE_NAME`
> 例外（允许穿透）只有：**可信窗口**（无障碍 / IME / 助手）、**不可见窗口**、**alpha = 0.0 的全透明窗口**、**组合不透明度 ≤ 系统最大遮掩不透明度（默认 0.8）的系统警报窗口**。注意：`TYPE_APPLICATION_OVERLAY` 类型的窗口**不受信任**。

我们的图层不满足任何一条例外，且是最坏组合：

```
InputDispatcher:  0: name=[BBQ] aimbot-ui#31822#31823, displayId=0, inputConfig=NO_INPUT_CHANNEL,
                     alpha=1, frame=[0,0][1280,2772], globalScale=1,
                     touchableRegion=<empty>, ownerPid=21090, ownerUid=2000,
                     touchOcclusionMode=BLOCK_UNTRUSTED
```

- **全屏**（`frame=[0,0][1280,2772]`）⇒ 屏幕上任意一点都被它遮住
- `touchableRegion=<empty>` ⇒ 它自己不吃触摸，触摸全部**穿透**下去
- `alpha=1` / `BLOCK_UNTRUSTED` ⇒ 判定为"不可信遮挡者"
- 非可信窗口（不是无障碍/IME/助手）

⇒ **穿过它的触摸（= 我们在该点上注入的每一次触摸）全部被 `InputDispatcher` 丢弃。**

我们的图层是 `ShellLayerHost` 反射 SF 建的裸 `SurfaceControl`（`setLayerStack 0` / `setLayer Int.MAX_VALUE` / `setGeometry` 全屏），**全工程从未设置过 `InputWindowInfo`**（grep `InputWindowInfo|setInputWindowInfo|trustedOverlay` 命中 0），所以这些值是 SF 对无主的 SurfaceControl 的默认值 —— 正好落在这条规则的枪口上。

## 4. 顺带解决掉的两个旧疑问

**① 为什么 uinput 也"设备被接纳、系统层零反应"。**
同一条规则作用于"合成触摸"这个类别，与后端无关。老架构（aimbot 1.2.1 / 影破）用的是 `TYPE_APPLICATION_OVERLAY` 窗口，而且它们的触摸注入走的是另一条"窗口可见性"路径 —— 只要图层条件不同，命运就不同。**换后端（uinput ↔ InputManager）永远修不好这件事**，这是这轮最重要的认知修正。

**② 为什么"我和一两个人能用"，大多数人不能。**
这条规则的执行强度取决于 ROM：ColorOS 对自建 SF 图层不拦，HyperOS 严格拦。同一份代码、同样的 uid 2000 + `u:r:shell:s0`，结果一个能用一个不能用 —— 与上下文无关，与**图层是否触发遮挡判定**有关。

## 5. 修法

### P0 — 立刻验证（一条命令，终端以 shell 身份跑）

```bash
settings put global block_untrusted_touches 1
```

- `1` = 只在 logcat 里警告、**不拦截**（保留证据，便于对比）
- `0` = 完全不拦截（文档里"允许不受信任触摸"的取值）
- `2` = 默认：拦截

然后**停服务 → 启服务 → 打开菜单 → 点桌面**。若触摸立刻恢复、且 `Untrusted touch` 变成纯告警，本诊断定案。

> `settings put global` 需要 `WRITE_SECURE_SETTINGS` —— uid 2000(shell) 持有，正是 `adb shell settings put` 一直能用的原因，我们的守护进程同 uid，所以也能执行。

### P0.5 — 若上一条无效（说明该 ROM 不认这个开关）

```bash
settings put global maximum_obscuring_opacity_for_touch 1.0   # 默认 0.8
```

把阈值抬到 1.0 让 `alpha=1` 的图层不再算遮挡（不改全局拦截策略，比 P0 更克制）。若也不生效，说明该 ROM 走的不是 opacity 路径，直接跳 P2。

### P1 — 内置进守护进程

启动时读一次当前值写进日志（`block_untrusted_touches` / `maximum_obscuring_opacity_for_touch`），失败设备上自动 P0；退出时按用户选择恢复。要动 `ShellServerEntry` 的启动/退出流程。

### P2 — 结构性修复（不碰系统设置，最干净）

在 `ShellLayerHost.applyGeometry()` 那个 `Transaction` 里补一次 `setInputWindowInfo`：

- ① 把图层的输入窗口标成**可信覆盖层**（`InputWindowInfo.trustedOverlay = true`）——直接在例外名单里；
- ② 或者把输入窗口的 **frame 缩到菜单矩形**（`frame=[menu rect]`）。只有菜单区会被判"遮挡"，而菜单区的触摸**本来就被我们吞掉**（`regions`），等于零代价。

②更保险也更有表达力，但需要确认 `InputWindowInfo.frame` 可由该路径设置。两者都走隐藏 API（`SurfaceControl.Transaction#setInputWindowInfo` + 反射 `android.view.InputWindowInfo`），必须真机验证是否被 ROM 采纳 —— 建议做成开关，失败自动回退。

### P3 — 复测 uinput

同一台设备把「触摸方式」切回 `uinput`，确认它是否同样被这条规则拦（预期：是）。结论决定 P1/P2 是否都不必做 —— 因为 **P2 一修，两个后端一起好**。

## 6. 采集要求变更（重要）

上一轮的采集命令 `logcat -d | grep -iE "aimbot|ShellManager"` 这次**碰巧**捞到了 `InputDispatcher`（因为那行里含 `aimbot-ui`／`aimbotnextgen`），但这是运气。以后固定写成：

```bash
logcat -d | grep -iE "aimbot|ShellManager|InputDispatcher|MIUIInput" > /sdcard/im.log
```

`InputDispatcher` 这行是**一票否决级判据**，优先级高于所有 IM 侧判据：

| 看到 | 结论 |
|---|---|
| `Untrusted touch due to occlusion by /2000/[BBQ] aimbot-ui` | **注入被系统丢** —— 与 IM/uinput 实现无关，走本文件 §5 |
| 有 `first DOWN accepted` + `ok` 在涨，但**没有**上述告警且屏幕仍不动 | 才轮到查厂商过滤 / 坐标空间 |
| `MIUIInput: ViewRootImpl ... ACTION_DOWN` 在图层存在期间一条都没有 | 同第一条，交叉印证 |

## 7. 待确认

1. 失败设备上跑 P0 后触摸是否恢复（**这是唯一能一锤定音的实验**）。
2. 该 ROM 是否认 `maximum_obscuring_opacity_for_touch`。
3. uinput 后端是否同样被拦（P3）。
4. 你本机（ColorOS）为何不拦：是 ROM 不执行该规则，还是本机 `block_untrusted_touches`/阈值被设过非默认值 —— 有条件时 `settings get global block_untrusted_touches` 与 `maximum_obscuring_opacity_for_touch` 各读一次即可对比。
