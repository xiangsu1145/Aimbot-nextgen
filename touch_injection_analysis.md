# 触摸注入失败分析报告

**项目**: AimbotNextgen
**日期**: 2026-09-16
**状态**: 已确认根因（最终版）

---

## 1. 问题描述

用户的 app 在部分设备上触摸服务无法工作（游戏收不到注入的触摸），而参考 APK（New World 1.0.2）能在这些设备上正常工作。

| 现象 | 说明 |
|------|------|
| Grab 成功 | EVIOCGRAB 调用正常，设备独占成功 |
| UI 可用 | ImGui 菜单渲染正常 |
| 部分用户 InputManager 也无法触摸 | 不是 uinput backend 单独的问题 |
| 用户的 OPPO OPD2404 设备正常 | 特定设备/ROM 版本问题 |

---

## 2. 关键发现

### 2.1 参考 APK 的触摸注入方式（已确认）

参考 APK（New World 1.0.2）**不使用 `injectInputEvent`**，也**不使用 EVIOCGRAB**。

通过以下方式确认：
- Native library `libQnncpu.so` 中**没有**任何 `injectInputEvent`、`InputManager`、`InputDispatcher` 字符串
- Native library 中有明确的 uinput 相关字符串：`uinput`、`aim_inject`、`ujmrfv_aim_inject`、`ujmrfv_aim_touch_down_bridge` 等
- Native library 中有 `aim_inject::ConcurrentQueue`（多线程安全注入队列）
- APK binary 中**没有任何** `EVIOCGRAB`、`/dev/uinput`、物理设备读取相关字符串

注入方式：**通过 ParcelFileDescriptor + binder IPC 将 `/dev/uinput` fd 从 Shizuku UserService 传到 App 进程，直接写 uinput 设备**

### 2.2 参考 APK 的完整架构（源码确认）

```
App 进程侧 (shell UID):
┌─────────────────────────────────────────────────────────┐
│  ShizukuInputEngine                                     │
│    injectDown/Move/Up() → ShizukuTouchServiceClient   │
└──────────────────────────┬──────────────────────────────┘
                           │ AIDL / binder call
┌──────────────────────────▼──────────────────────────────┐
│  Shizuku UserService (system_server 上下文)            │
│    ShizukuTouchUserService.onTransact()                  │
│    → NativeUinputBridge.nativeInit()                    │
│    → open("/dev/uinput")  ← 有 system 权限！            │
│    → 创建 uinput 虚拟设备 + nativeExportFds()           │
│    → mapAimInjectChannel() → 返回 ParcelFileDescriptor[]│
└─────────────────────────────────────────────────────────┘
                           │ fd 通过 binder 传回 App
┌──────────────────────────▼──────────────────────────────┐
│  App native 代码 (libQnncpu.so)                        │
│    TouchInjectChannel.nativeImportFds() 导入 uinput fd │
│    aim_inject 模块: 直接 write(uinput_fd, ...) 写 MT 帧 │
│    不经过 InputReader / InputDispatcher                │
└─────────────────────────────────────────────────────────┘
```

### 2.3 关键证据：native library 字符串

`libQnncpu.so` (5839KB) 中的关键符号：
```
aim_inject                          ← 核心注入模块
ujmrfv_aim_inject                   ← aim 注入入口
ujmrfv_aim_touch_down_bridge       ← 下沉触摸桥接
ujmrfv_aim_touch_move_bridge       ← 移动触摸桥接
ujmrfv_aim_touch_up_bridge          ← 抬起触摸桥接
uinput                              ← 直接使用 uinput
N10moodycamel15ConcurrentQueueIN10aim_inject12_GLOBAL__N_112P  ← 多线程安全队列
```

完全**没有**：`injectInputEvent`、`InputManager`、`InputDispatcher`、`INJECT_EVENTS`

### 2.4 为什么参考 APK 能工作

| 因素 | 参考 APK | 你的 App |
|------|---------|---------|
| 进程上下文 | Shizuku UserService (system_server) | adb shell (shell UID 2000) |
| `/dev/uinput` 访问 | system 权限，完全可写 | shell UID 受 SELinux 限制 |
| EVIOCGRAB | **不需要** | 需要（设计如此） |
| InputManager API | 不使用 | 使用（受权限限制） |
| InputDispatcher | 不经过 | 经过（可能被拦截） |

### 2.5 参考 APK 的 HiddenApiBypass 用途

虽然 native 层不使用 `injectInputEvent`，但 `UjmrfvCoreService` 导入了 `org.lsposed.hiddenapibypass.ah`：

可能用于绕过其他隐藏 API（如 `WindowManager` 的一些方法），而非触摸注入。

### 2.6 为什么部分设备失败

```
你的 App:
  adb shell → shell UID (2000)
  → /dev/uinput 可能可写（部分设备）
  → injectInputEvent(2-param)  → 需要 INJECT_EVENTS
  → 部分 ColorOS/MIUI 对 shell UID 的 INJECT_EVENTS 做了限制
  → 失败

参考 APK:
  Shizuku UserService → system_server 上下文
  → /dev/uinput 完全可写
  → 直接 write(uinput_fd, ...) 绕过 Android InputDispatcher
  → 不需要 INJECT_EVENTS，不需要任何 Android 权限检查
  → 成功
```

你的设备（ColorOS OPD2404）恰好对 shell UID 的 `INJECT_EVENTS` 放行了，所以 InputManager 路径可用。部分用户的设备（不同 ColorOS 版本、MIUI、Flyme 等）没有放行。

### 2.7 无障碍服务的真正用途

参考 APK 的无障碍服务配置：

```xml
<accessibility-service
    android:accessibilityEventTypes="0x00000020"   <!-- TYPE_NOTIFICATION_STATE_CHANGED -->
    android:accessibilityFeedbackType="0x00000010" <!-- FEEDBACK_GENERIC -->
    android:canRetrieveWindowContent="false"/>
```

它**只监听通知变化**，并不直接处理触摸。存在的意义是：
- 作为保活机制（无障碍服务有高优先级，系统不会轻易杀掉）
- 监听通知来检测游戏状态

### 2.8 其他差异

| 权限 | 参考 APK | 你的 APK |
|------|---------|----------|
| `WRITE_SECURE_SETTINGS` | ✅ | ✅ |
| `BIND_ACCESSIBILITY_SERVICE` | ✅ | ❌ 没有 |
| `ACCESS_SUPERUSER` | ✅ (Shizuku 需要) | ❌ 没有 |
| `CAPTURE_VIDEO_OUTPUT` | ✅ | ❌ 没有 |
| `CAPTURE_SECURE_VIDEO_OUTPUT` | ✅ | ❌ 没有 |
| `FOREGROUND_SERVICE_DATA_SYNC` | ✅ | ❌ (用 SPECIAL_USE) |
| `QUERY_ALL_PACKAGES` | ✅ | ❌ 没有 |

---

## 3. 验证方法

让失败的用户执行以下命令，查日志确认失败类型：

```bash
adb logcat | grep -i "inject\|AimbotInject\|INJECT_EVENTS\|SecurityException\|InputManager"
```

### 失败类型判断

| 日志 | 含义 |
|------|------|
| `"InputManager injector unavailable: no InputManager"` | InputManager 获取失败 |
| `"Enable USB debugging (Security settings)"` | `INJECT_EVENTS` 权限被拒 |
| `"injectInputEvent failed: SecurityException"` | 权限问题 |
| `"InputManager refused the event"` | 方法存在但事件被拒绝 |

---

## 4. 解决方案

### 方案 A：集成 Shizuku SDK（推荐，完整复现参考 APK）

将 Shizuku SDK 集成到你的 App，复现参考 APK 的架构：
1. 嵌入 `libshizuku.so`
2. 创建一个 Shizuku UserService（注册 `IShizukuTouchService` AIDL）
3. 在 UserService 中执行 `open("/dev/uinput")` 和 `nativeInit()`（Shizuku 服务运行在 system_server 上下文，有权限）
4. UserService 通过 `mapAimInjectChannel()` 返回 ParcelFileDescriptor[]
5. App 端通过 `TouchInjectChannel.nativeImportFds()` 导入 fd
6. Native 代码 `write(uinput_fd, ...)` 直接写触摸帧

**优点**：和参考 APK 完全一致，绕过所有权限限制
**缺点**：需要重构 daemon 架构，Shizuku 集成复杂度高

### 方案 B：HiddenApiBypass + InputManager 3参数版本（中等难度）

如果 InputManager 路径在部分设备上可用，可以尝试：
1. 集成 LSPosed HiddenApiBypass
2. 用 HiddenApiBypass 获取 `InputManager.injectInputEvent(InputEvent, int, int)` 的 3 参数隐藏版本
3. 调用时传入 `uid = -1`（`InputManager.TARGET_UID`）

但注意：native library 确认参考 APK **不使用** InputManager，完全使用 uinput。如果 HiddenApiBypass + InputManager 方案在部分设备上仍失败（因为 InputDispatcher 可能仍然拦截），建议还是走方案 A。

**优点**：改动较小，保留现有 InputManager 路径
**缺点**：和参考 APK 实现不一致，可能仍有兼容性问题

### 方案 C：改进现有 uinput backend

确保 uinput backend 在更多设备上可靠工作：
1. 检查 `/dev/uinput` 对 shell UID 的可写性
2. 如果 `/dev/uinput` 不可写，尝试 `adb remount` 模式或 root 模式
3. 在 `InputManager` 失败时自动回退到 uinput

---

**推荐路线**：方案 A（集成 Shizuku）是真正解决所有设备兼容性问题的方案，但实现复杂度最高。方案 B 可以作为快速修复。

---

## 6. 参考资料

- [LSPosed/HiddenApiBypass](https://github.com/LSPosed/HiddenApiBypass) — 官方库（参考 APK 用于其他隐藏 API，非触摸注入）
- [Shizuku](https://github.com/RikkaApps/Shizuku) — 嵌入式 Shizuku 架构，UserService 运行在 system_server 上下文
- [scrcpy InputManager 注入](https://github.com/Genymobile/scrcpy/blob/master/app/src/input/layer_input_manager.cpp) — 类似实现
- `app/src/main/java/io/github/xiangsu1145/aimbotnextgen/inject/InputManagerInjector.kt` — 当前 InputManager 实现
- `app/src/main/cpp/input/uinput_inject.cpp` — uinput backend 实现

---

## 7. 复核与更正（2026-09-16 第二轮，一手源码核实）

### 7.1 更正：Shizuku UserService **不是** system_server 上下文

第 2.2 / 2.4 / 2.6 节把 Shizuku UserService 描述为 "system_server 上下文、有 system 权限"，
**这是错的**。基于它得出的"方案 A 能拿到 system 权限"也是错的，必须先纠正，否则后续排查继续跑偏。

证据（一手）：

| 来源 | 内容 |
|------|------|
| `Shizuku-API/README.md`（官方，UserService 一节） | "the service runs in a different process and **as the identity (Linux UID) of root (UID 0) or shell (UID 2000**, if the backend is Shizuku and user starts Shizuku with adb)" |
| `Shizuku-API/server-shared/.../UserService.java` | `create(String[] args)` 只把 `--uid=` 用于算 `userId = uid / 100000`，**从不调用 setuid/setgid**；进程 uid/SELinux 域完整继承自 Shizuku server |
| `Shizuku/server/.../ShizukuUserServiceManager.java` + `starter/.../ServiceStarter.java:44-59` | 用户服务命令 = `app_process ... moe.shizuku.starter.ServiceStarter --uid=<callingUid>`，由 server 进程 fork/exec |
| `Shizuku/manager/src/main/jni/starter.cpp:192-196` | Shizuku server 自身只接受 uid 0 或 2000 启动 |

**误判来源**：`UserService.create()` 里调了 `ActivityThread.systemMain()`。那只是在**本进程内**造一个
system 模样的 ActivityThread / Context，**不改 uid、不改 SELinux 域、更不会进入 system_server 进程**。
看到这个名字就推断"运行在 system_server 上下文"是这次误判的根因。

⇒ 结论：**老项目（aimbot 1.2.1）走 Shizuku 时，打开 `/dev/uinput` 的进程是 uid 2000 + `u:r:shell:s0`
（ADB 模式），或 uid 0（root / Sui 模式）。**
本项目 daemon 是 `adb shell → setsid /system/bin/app_process`，同样是 **uid 2000 + `u:r:shell:s0`**。
**所以在 ADB 模式下，老项目与新项目的进程上下文是相同的** —— "上下文不同"不能单独解释大面积失败。

### 7.2 那"老项目人人可用 / 新项目少数可用"只剩这几种可能

1. **root 与 ADB 的差别（最可能）**：老项目用户在 Shizuku 里选 root/Sui → uid 0 → `/dev/uinput` 必然可写；
   新项目**只有无线调试一条路**（全工程 grep 无 `su` 启动路径），人人都被压在 uid 2000 上。
   若失败用户多数非 root，则"老项目人人可用"很可能来自 root，而不是来自"UserService 有 system 权限"。
2. **组 / DAC 差别**：AOSP `rootdir/ueventd.rc` 一手数据 ——
   `/dev/uinput 0660 uhid uhid`、`/dev/uhid 0660 uhid uhid`、`/dev/input/* 0660 root input`。
   开 uinput 需 **root 或 `uhid`(3011) 组**；读面板需 **root 或 `input`(1004) 组**。
   这组补充组由 adbd（或 su）在启动时设定，是"每个进程身份"的一部分，**必须实测**。
3. **SELinux 依 ROM 而异**：`u:r:shell:s0 → uhid_device` 是否放行，AOSP 与各 OEM ROM 不一致，
   只能在失败设备上用 `dmesg | grep avc` 实证。
4. **节点形态**：`/dev/uinput` 可能不存在（uinput 为模块未加载，或厂商改用别的节点名）。
   本项目与老项目都**硬编码 `/dev/uinput`**，都没有变体探测。
5. **Grab + 物理手指镜像 / takeover 融合子系统（新项目独有）**：
   老项目 uinput 路径只有 `grab + 注入虚拟手指`，**不镜像物理手指**（`RemoteInjectorService.init()` 的
   uinput 分支只有 `openUinputNative()`，`nativeInputmgrGrab` 只属于 InputManager 分支）；
   新项目在其上叠了 `uinput_mirror_physical` + `uinput_takeover_physical_id` 融合。
   面板协议 A/B、槽位数、tracking id 语义因设备而异，融合逻辑一旦把**注入手指**误判成物理手指而抢占/清掉它的槽，
   现象同样是"注入没反应"——**且与设备强相关**。

### 7.3 判定顺序（在失败用户的设备上跑，从上往下，先便宜后贵）

```bash
# ① 守护进程真实身份：uid / gid / 补充组 / SELinux 域
P=$(pidof aimbot_shell)
cat /proc/$P/status | grep -E "^(Uid|Gid|Groups)"; id
cat /proc/$P/attr/current

# ② uinput 节点形态与访问权
ls -lZ /dev/uinput* /dev/uhid 2>&1
ls -lZ /dev/input/ | head

# ③ 内核层是否真的拒绝（SELinux 实锤）
dmesg | grep -i avc | grep -iE "uinput|uhid|input_device" | tail -20

# ④ daemon 自己的日志：open 的 errno
grep -nE "uinput|errno|createUinput" /data/local/tmp/aimbot_daemon.log | tail -30

# ⑤ 虚拟设备是否被 InputReader 接纳
dumpsys input | grep -iE -B2 -A8 "aimbot|touchpanel|<真面板名>"
```

判读表：

| 观测 | 结论 | 方向 |
|------|------|------|
| `open failed errno=13` (EACCES) + `avc ... uhid_device` | 该设备上 shell 域/组不够 | 必须换 root/uid 0 域，或让用户开 root；不是坐标问题 |
| `open failed errno=2` (ENOENT) | 该设备没有 `/dev/uinput` | 加节点变体探测；必要时 `modprobe uinput`（需 root） |
| 组里**没有** `uhid`/`input` | 启动路径没带上 adbd 的补充组 | 检查是谁拉起的 daemon（必须 `adb shell`，不能由 App fork） |
| open 成功、④ 无 errno、⑤ 里**看不到**虚拟设备 | 设备没被 InputReader 接纳 | 检查声明（ABS 范围/PROP/名冲突） |
| ⑤ 里看得到、但坐标全错/注入手指被吃 | 声明或镜像融合逻辑 | 关 `exclusive` 复测，再查融合槽位归属 |
| `dumpsys` 里注入手指出现后立即消失 | fusion 把注入手指当物理手指抢占了 | 查 `uinput_takeover_physical_id` 的排除条件 |

**最快的一次分叉实验**：让失败用户把 `exclusive`（抓物理面板）关掉，只保留 uinput 注入 ——
关掉后能动 ⇒ 问题是 grab/镜像/融合（代码+设备差异）；关掉后仍然不能动 ⇒ 问题是 uinput 本身（权限/节点/声明），
再按上表 ①②③④ 定位。这是一次开关就能把两大方向劈开的实验，比继续读代码快得多。

### 7.4 应当马上补的自检（让下一轮用户反馈自带答案）

daemon 启动时把身份与结果打进 `aimbot_daemon.log`，并在失败时通过现有 `replyErr` 机制回给 App：

- `Os.getuid()/getgid()` + `/proc/self/status` 的 `Groups` + SELinux context（照 `Shizuku/common/util/OsUtils.java` 的写法）
- `open("/dev/uinput")` 的 `errno`
- 创建后校验：`/sys/class/input/*/name` 是否出现自己的设备名（以及 `dumpsys input` 是否登记）
- 记住口径：**`inject_is_ready()` 而非 `uinput_is_ready()`**（见项目 MEMORY）

- **已确认参考 APK 源码**：
  - `D:/破解/New World_1.0.2_at_decrypted/classes2/sources/com/newshijie/ujmrfv/input/ShizukuInputEngine.java` — App 侧入口
  - `D:/破解/New World_1.0.2_at_decrypted/classes2/sources/com/newshijie/ujmrfv/input/ShizukuTouchUserService.java` — UserService 实现
  - `D:/破解/New World_1.0.2_at_decrypted/classes2/sources/com/newshijie/ujmrfv/input/NativeUinputBridge.java` — JNI bridge
  - `D:/破解/New World_1.0.2_at_decrypted/lib/arm64-v8a/libQnncpu.so` — native 注入实现（含 `aim_inject`、`uinput` 等符号）
