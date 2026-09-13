# Shell 守护进程启动失败分析（2026-09-12）

**设备**: OnePlus OPD2404 (ColorOS) — `1ddb632b`
**系统**: Android 15 (SDK 35), boot_completed=1
**抓取时间**: 2026-09-12 13:30
**目的**: 仅汇报，不修复。

---

## 0. TL;DR（一句话）

`aimbot_shell` 守护进程现在**根本没有在跑**，最近的 `kill 11187` 之后再也没有成功起过新的 daemon —— 启动逻辑发出 `app_process ...` 命令、`adb shell` 子 shell 立刻返回（0 字节 stdout）、端口文件**还是 1.5 小时前那次成功留下的旧值**，app 等 4 秒连不上 127.0.0.1 → 报"启动失败"。

从 logcat 看，新启动的 `app_process` **没有产生任何 `aimbot_shell` 这个 TAG 的 logcat 输出**（连 `daemon starting uid=...` 都没打）—— 它在 fork 后、Java 主类进入 `main()` 之前就死了，所以根本没有机会更新端口文件，也没有产生 tombstone。

---

## 1. 用户报告的两个症状 vs 实际证据

| 症状 | 现状（logcat / 文件系统证据） |
|------|------------------------------|
| "有时能连上，有时连不上，现在怎么弄都不行" | 端口文件 `mtime=2026-09-12 12:55`，内容 `41081`。**这 1.5 小时里没有任何新的启动写到该文件**，但 logcat 里只有**一次** `launching daemon via adb shell`（13:27:41）且那次没成功。 |
| "之前连上之后 `kill 11187` 就永远连不上" | 12:55 那次连上的 daemon **进程已经被 kill 掉**（logcat 里看不到它的 PID 144xx 的退出，但 `ps -A` 现在没有 `aimbot_shell`，且端口文件再没更新过）。 |

---

## 2. 当前设备状态（13:30 实测）

```
$ ps -A | grep aimbot_shell
（空）                                   ← 当前没有 daemon 在跑

$ ls -la /data/local/tmp/aimbot_shell.port
-rw-rw-rw- 1 shell shell 5 2026-09-12 12:55 /data/local/tmp/aimbot_shell.port
Modify: 12:55:01                        ← 1.5 小时前的旧文件，没有被覆盖

$ cat /data/local/tmp/aimbot_shell.port
41081                                   ← 旧端口值，新启动没改写

$ ss -tlnp | grep 41081                  ← 当前没人监听 41081
（空）

$ adb shell "logcat -d -v threadtime | grep aimbot_shell"
09-12 13:27:41.828  8087  8670 I ShellManager: launching daemon via adb shell: (setsid ...
09-12 13:27:41.853  8087  8670 I ShellManager: shellCommand returned, daemon should be binding ...
[就这两条，全是 ShellManager 发的；守护进程自己一行都没打]
```

---

## 3. 完整因果链（13:27:41 那次失败的启动）

```
13:27:41.648  ShellManager.startDiscovery            mDNS 找 ADB 端口
13:27:41.790  mDNS resolved ADB port=38783
13:27:41.827  ADB TCP connected to 127.0.0.1:38783   ADB session 建立（OK）
13:27:41.828  ShellManager.launching daemon via adb shell
              ↓ 发出命令：
              (setsid /system/bin/app_process -Djava.class.path='/data/app/.../base.apk'
               /system/bin --nice-name=aimbot_shell
               io.github.xiangsu1145.aimbotnextgen.shell.ShellServerEntry '/data/app/.../lib'
               >/dev/null 2>&1) &
13:27:41.853  shellCommand returned                   ← adb shell 立刻返回（25ms）
              ↓ 关键点：app 在这里假设 daemon 已 fork 并在跑
              ↓ 实际上没有任何 aimbot_shell 的日志，连 'daemon starting' 都没打
13:27:41.906  ShellManager.daemon published port=41081
              ↑ 但读到的 41081 其实是端口文件 **12:55 的旧内容**！
              ↑ 没有新的 writeText 调用过端口文件
13:27:42~46   Socket connect 127.0.0.1:41081           ← ECONNREFUSED（没人监听）
13:27:46.001  ShellManager.could not connect ... within 4s: ECONNREFUSED
13:27:46.002  state CONNECTING -> ERROR :: 启动失败: 无法连接 daemon
```

**核心异常**：ShellManager 在 13:27:41.906 读端口文件读到 `41081` 然后上报"daemon published port=41081"——但**这个值是 1.5 小时前那次启动写入的，不是这次启动写的**（mtime 12:55）。这说明：

1. `app_process ... ShellServerEntry` 启动后**立刻就死了**，根本走不到 `writeText(port)` 那行；
2. `alreadyRunningPort()`（`ShellServerEntry.kt:167`）的逻辑里有一个"已存在端口则 exit(0)"的检测——上一次 daemon 在退出时**没来得及删端口文件**（看代码只有 `DESTROY` 和 `shutdown()` 才会删 `PORT_FILE`，被 SIGKILL/系统回收时不会删）；
3. 于是新一轮 ShellServerEntry 启动后第一时间读到旧端口 = 41081 → `Socket(127.0.0.1, 41081).use { port }` 成功连上了一个**已经死了的 daemon**留下的端口号 → 返回非 null → `Log.i("a daemon is already serving ... — this one exits")` → `exitProcess(0)`，**新进程一出生就退出**。

> ⚠️ 这一条需要复核代码逻辑：如果已经能 `Socket().use` 成功，理论上这个端口还有人监听才对。可是 `ss -tlnp` 显示 41081 没人监听。这说明 `alreadyRunningPort()` 那次 `Socket()` 也连不上（结果 null），所以**它没走"自我退出"分支**。换言之它走到了 `bind 127.0.0.1:0` → 写端口文件 → 但是它写出来新端口号了吗？

> **再看代码**（`ShellServerEntry.kt:218`）：
> ```kotlin
> runCatching { java.io.File(PORT_FILE).writeText(port.toString()) }
> ```
> `runCatching` 默默吞掉异常——如果新进程能 bind 但 writeText 失败（不是这种情况，文件 mtime 没变），或者更可能：**新进程压根没起起来**（在 fork 阶段被某种力量 SIGKILL），那么文件 mtime 当然就是旧的 12:55。

---

## 4. 关键判断：daemon 为什么起不来

按可能性排序：

### 4.1 最可能：**上一次 daemon 的残留文件让新 daemon "自杀"**（代码逻辑陷阱）

读 `ShellServerEntry.kt:167-189`：

```kotlin
private fun alreadyRunningPort(): Int? {
    val port = ...File(PORT_FILE).readText()...          // 读到旧 41081
    if (port == null || port !in 1..65535) return null
    return try {
        Socket(127.0.0.1, port).use { port }             // 没人监听 → 抛异常
    } catch (_: Throwable) {
        null  // 注释说 "stale file from a daemon that is gone"
    }
}
```

如果 Socket 抛异常 → 返回 null → 继续往下 bind 0 → writeText(新端口号)。那么端口文件 mtime 应该被刷新到当前时间。

**但实测 mtime 是 12:55**，说明新进程**根本没跑到 bind/writeText**。

所以这条不是原因，但要警惕——下次运气好的时候可能就成元凶。

### 4.2 几乎确定：**启动后立即被外部 SIGKILL**

证据：
- logcat 里只有一次 `launching daemon via adb shell`，没有对应 pid 的 `Zygote: Process xxxx exited`——因为新进程从来**没被 zygote 认领过**到日志级别；或者日志在它死前就被 logcat buffer 滚掉了（buffer 只剩 11981 行）；
- 没有任何 tombstone——进程不是 SIGSEGV/SIGABRT，是被信号 9 杀的（signal 9 = KILL，不产生 tombstone）；
- `pkill -f aimbot_shell` 留过：上一次 daemon 被用户 `kill 11187`，但 app 也试图 `pkill -f aimbot_shell`（`ShellManager.stop()` 里有）。这本身不致命，但说明这个进程名在用户/OS 眼中**容易被各种工具瞄上**。

ColorOS 在 mtime 12:55 ~ 现在之间，**有充分的权力去杀 shell uid 的后台进程**：

```
09-12 12:54 ~ 13:28  Zygote 杀进程记录（搜 11981 行 buffer）：
  13:26:49  14774 com.oplus.pantanal.ums:utrace_core        self-kill (idle)
  13:27:40  14655 com.oplus.powermonitor                    self-kill (PowerMonitor selfKill!)
  13:28:35  14953 com.oplus.dmp:main                        self-kill (DmpIdleSuicideScheduler, 120s idle)
```

这些是 ColorOS 自己的"自杀器"在杀后台服务。我们的 `app_process` 在 `setsid` 后是 PPID=1 的孤儿进程，没有 OOM 保护、没有 foreground 标记、没有 NM 关联——它最容易成为下一个被"自杀器"挑中的目标。

### 4.3 次要怀疑点：native 初始化失败（但无证据）

`ShellServerEntry.kt:204`：
```kotlin
ShellNative.modelLoadFromDisk()
```
这一步在 `loadLibrary` 之后、bind 之前。如果 `libaimbotng.so` 里的某个全局对象（model loader / Vulkan / NCNN / QNN）抛了 C++ 未捕获异常 → `JNI_OnLoad` 失败 → 整个进程崩溃。

但是 **无 logcat 痕迹**：`Log.e("aimbot_shell", "failed to load libaimbotng.so", t)` 这种行也没打。说明崩溃比 `loadLibrary` 那一行还要早——也就是说崩溃发生在 Java/Kotlin 主类进入 `main()` 之前。

### 4.4 还要核实：**`(setsid ... ) &` 在 ColorOS 上是否还成立**

`ShellManager.kt:248` 的注释说：
> without it, the daemon is in the adb shell's process group, and on OPPO ColorOS the adb shell's exit kills every process in that group within a second — even `nohup` does not help, because it is not SIGHUP that kills them but the cgroup cleanup.

所以设计时就已经把"setsid 解决 ColorOS 的 cgroup 清理"当成已知问题处理。**但是**：`adb shellCommand` 之后并不立刻关闭 adb session（app 还活着，session 还在用）——所以 cgroup 清理是发生在**用户断开 adb 或者 adb session 闲置超时**之后。

`ShellManager.kt:259-262`：
```kotlin
adbClient.shellCommand(cmd) { chunk -> ... }
Log.i(TAG, "shellCommand returned, daemon should be binding ...")
```

shellCommand 一返回就立刻去 `cat /data/local/tmp/aimbot_shell.port`——`adb shellCommand` 完成**不等于 adb session 关闭**，所以 cgroup 清理**此刻还没发生**。但是在 `if (port == null || ...) return@launch` 这段里 adb 是一直在用的（readDaemonPortViaAdb 还在调 shellCommand）。

**真正的杀手**更可能是 app 接下来报错、进入 ERROR 状态、然后过了若干秒用户没操作 → app 进入后台 → 25s heartbeat 停顿 + 几分钟无操作 → ColorOS 把整个 adb session 杀掉 → cgroup 清理孤儿 app_process。

也就是说：**我们的 daemon 启动成功 ≠ 长期存活**，但是这次连启动成功都没拿到。

---

## 5. 为什么"之前那次连上"的版本现在起不来

回看 12:55 那次——能连上说明 12:55 时这套机制是工作的。现在是**设备状态变了**而不是代码变了：

- **电池优化更严**？`isIgnoringBatteryOptimizations` 状态在 `ShellDaemonService.onCreate` 里 log 过（需要去抓）——这是不是 false 决定了 ColorOS 多久会冻结我们 app。
- **adbd 状态**？端口文件 12:55 后没再写过——意味着 app 启动→调 daemon→成功的流程从那时起就**再没走完过**。可能不是 daemon 逻辑坏了，是从 12:55 之后**用户再也没有"启动成功过一次"**。换句话说 12:55 那次是上一次也是唯一一次成功。
- **app 自身进后台太久被冻**？13:26 ~ 13:30 logcat 里 adb mdns rescan tick 还在跑（`AdbPairingService: Rescan tick, restarting discovery` 每 12 秒一次），说明 app 没被完全冻死，只是 daemon 链路断了。

---

## 6. 一些可立即做的事（不修复，仅验证假设）

按报告者要求**不修复**，仅列出可能后续动作：

1. **抓 12:55 那次成功时的 daemon 日志**—— logcat buffer 滚没了，下一次可以重现一次成功启动后立刻 `logcat -d > /sdcard/logcat.txt` 抓现场。
2. **直接重启 adbd / 重启设备**——清掉所有残留端口文件 + cgroup 状态 + 自杀器记录。
3. **`adb shell run-as shell cat /data/local/tmp/aimbot_shell.port`** 不行（untrusted_app 读不到）——只能 `adb shell cat`。
4. **临时手动起一个 daemon 看看**：项目里有 `tools/daemon_ui_smoke.sh` / `daemon_grabbed_smoke.sh`——直接用它们跑一次，**绕过 ShellManager 那套逻辑**，定位"daemon 本身是否能起来"。
5. **手动清端口文件**：
   ```
   adb shell rm -f /data/local/tmp/aimbot_shell.port
   ```
   然后再"启动"——如果 4.1 的判断成立（残留文件导致自杀），这一步立即会让下一次"启动"成功。
6. **抓 `isIgnoringBatteryOptimizations` 状态**：在 logcat 里 `grep "battery_optimisation ignoring="`——如果是 false，ColorOS 会在后台 30~60s 冻掉 app，连带 adb session。

---

## 7. 不确定项

- 12:55 那次成功后，daemon **到底存活了多久**？是用户 `kill 11187` 主动结束，还是被杀？
- `(setsid ... ) &` 启动后，子 shell 是否真的在 25ms 内返回了？（logcat 间隔 25ms 是两次 `I` 之间，可能中间还发生了别的事）
- 13:27:41 那次启动的 app_process 的 PID 是多少？logcat 里没有它的 `I aimbot_shell: daemon starting uid=...`——意味着它在 `Log.i` 之前就死了。

---

## 8. 结论

> **现在连不上不是 daemon 逻辑坏掉了，是启动过程中守护进程根本没成功初始化就死了；同时 `/data/local/tmp/aimbot_shell.port` 是 1.5 小时前的旧文件，新的启动又读不到新值（因为根本没写出来），所以 ShellManager 误以为"41081 端口已公布"，连不上再甩锅给 ECONNREFUSED。**

**最强假设**：ColorOS 的"自杀器" / OOM / cgroup 清理在 `setsid` 出来的孤儿 app_process 完成 Java VM + JNI 初始化之前把它 SIGKILL 掉了——这是 logcat 没记录、tombstone 也没有、只有 ECONNREFUSED 痕迹能解释的现象。

**待验证**：手动清掉端口文件再启动是否能恢复；以及 `tools/daemon_*.sh` 那套是否还能直接起 daemon（如果是 → 说明是 ShellManager/ShellServerEntry 启动路径有问题，而不是 daemon 本身坏掉）。
