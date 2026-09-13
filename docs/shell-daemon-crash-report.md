# Shell 守护进程崩溃分析报告

**日期**: 2026-09-11  
**设备**: OnePlus OPD2404 (ColorOS)  
**系统**: Android 15 (SDK 35), Build `OPD2404_15.0.0.820(CN01)`  
**调查方法**: 实时 logcat 监控 + tombstone 分析 + 内存分析

---

## 1. 核心发现：ImGui 启动后数分钟崩溃的真实原因

用户报告的症状是：**启动 ImGui 后运行几分钟，Shell 守护进程才断开**（不是启动时立即崩溃）。

### 实时 logcat 证据（2026-09-11 12:15 ~ 12:21）

```
12:15:12  ADB WIFI TLS 连接建立（aimbot）
12:16:11  aimbot-capture 虚拟显示器 #9 创建（3000x2120）
12:17:55  VirtualDisplay #9 释放（存活 104 秒）
12:17:57  VirtualDisplay #10 创建
12:17:58  VirtualDisplay #10 释放（仅存活 1 秒！）
12:18:00  VirtualDisplay #11 创建
12:20:40  ⚠️ ADB WIFI TLS 断开
12:20:41  ⚠️ "Virtual display released because application token died: com.android.shell"
```

**关键行**：
```
VirtualDisplayAdapter: Virtual display device released because application token died: com.android.shell
```

这说明 **shell 进程（aimbot_shell，UID 2000）被系统杀死**，导致虚拟显示器被释放。

### 崩溃时间线分析

| 时间 | 事件 | 距 ImGui 启动 |
|------|------|-------------|
| 12:15:12 | ADB 连接建立 | 0s |
| 12:16:11 | Capture 虚拟显示器创建（ImGui 启动） | ~59s |
| 12:17:55 | 显示器 #9 释放 → 立即重建 #10 | ~163s |
| 12:17:58 | 显示器 #10 释放 → 立即重建 #11 | ~166s |
| 12:20:40 | **ADB 会话断开** | ~328s (~5.5min) |
| 12:20:41 | **shell 进程死亡，显示器释放** | ~329s |

---

## 2. 根本原因：虚拟显示器反复重建 + 内存压力

### 问题一：Capture 虚拟显示器在反复创建/销毁

logcat 显示虚拟显示器被**反复创建和销毁**：
- Display #9 存活 104 秒后正常释放
- Display #10 仅存活 **1 秒**就被释放
- Display #11 存活 161 秒后因进程死亡被释放

这说明 `ScreenCapture` 的 capture supervisor 在反复调用 `start()` → `stop()` → `start()`。

**代码路径**（`ShellServerEntry.kt:327-368`）：
```kotlin
// capture supervisor 每 400ms 轮询一次
while (running && !captureStop) {
    val wanted = ShellNative.captureWanted()
    val size = ShellNative.captureWantedSize()
    if (wanted) {
        if (ScreenCapture.needsRestart()) {  // ← 这里反复返回 true
            ScreenCapture.start(size)         // ← 每次都销毁旧的，创建新的
        }
    }
}
```

`needsRestart()` 返回 `true` 的条件（`ScreenCapture.kt:104-108`）：
```kotlin
fun needsRestart(): Boolean {
    if (!running) return true
    val now = liveGeometry() ?: return false
    return now != geometry  // ← 屏幕几何变化时返回 true
}
```

**每次 `start()` 都会**：
1. 调用 `stop()` 释放旧的虚拟显示器和 ImageReader
2. 创建新的 ImageReader（3000×2120 RGBA = **~25MB/缓冲 × 3 = ~75MB**）
3. 创建新的虚拟显示器
4. 启动新的帧泵线程

### 问题二：内存消耗巨大

| 组件 | 内存估算 |
|------|---------|
| ImageReader 3 缓冲 (3000×2120 RGBA) | ~75 MB |
| Vulkan 渲染器 (swapchain + 帧缓冲) | ~30-50 MB |
| 原始触摸读取器 + uinput | ~5 MB |
| ImGui + geometry watcher + region pump 线程 | ~10 MB |
| **总计** | **~120-140 MB** |

ColorOS 的 OOM 机制会杀死内存消耗过高的非前台进程。shell 进程虽然是 `app_process`，但它不是系统注册的"应用"，可能不在 OOM 保护范围内。

### 问题三：ColorOS 对 WIFI ADB 的不稳定性

tombstone 证据（4 次 adbd 崩溃）：
```
adbd_auth: failed to read from framework fd
```
- tombstone_06: 16:40
- tombstone_08: 17:04
- tombstone_12: 21:08
- tombstone_16: 21:40

adbd 崩溃会**杀死所有 TCP 会话**，包括 shell 守护进程的连接。

---

## 3. 崩溃机制总结

```
┌─────────────────────────────────────────────────────────┐
│  用户点击"UI_ON"                                         │
│       ↓                                                  │
│  ShellLayerHost.start() → Vulkan 渲染器启动               │
│       ↓                                                  │
│  ScreenCapture.start() → 虚拟显示器创建 (75MB)            │
│       ↓                                                  │
│  capture supervisor 每 400ms 检查                        │
│       ↓                                                  │
│  needsRestart() 返回 true（几何变化/重建）                 │
│       ↓                                                  │
│  ScreenCapture.start() → stop() + 重新创建               │
│       ↓                                                  │
│  重复 N 次 → 内存累积                                    │
│       ↓                                                  │
│  ┌───────────────────────────────────┐                   │
│  │ 触发路径 A: OOM/LMK 杀死进程      │                   │
│  │ → shell 进程死亡                  │                   │
│  │ → "application token died"        │                   │
│  │ → TCP 会话关闭                    │                   │
│  │ → "Shell 守护进程已断开"          │                   │
│  └───────────────────────────────────┘                   │
│       或                                                  │
│  ┌───────────────────────────────────┐                   │
│  │ 触发路径 B: adbd 崩溃             │                   │
│  │ → "failed to read framework fd"   │                   │
│  │ → TCP 连接断开                    │                   │
│  │ → "Shell 守护进程已断开"          │                   │
│  └───────────────────────────────────┘                   │
└─────────────────────────────────────────────────────────┘
```

---

## 4. 修复建议

### P0 — 修复虚拟显示器反复重建

**问题**：`needsRestart()` 在几何变化时返回 `true`，导致整个 capture 链路重建。

**修复**：在 `ScreenCapture.start()` 中，如果尺寸没变就不要重建：

```kotlin
@Synchronized
fun start(side: Int): Boolean {
    // 如果已经在运行且尺寸没变，只更新 crop 即可
    if (running && reader != null && virtualDisplay != null) {
        val nextCrop = centredCrop(side, geometry?.width ?: 0, geometry?.height ?: 0)
        if (nextCrop.side == crop.side) return true
        crop = nextCrop
        return true
    }
    stop()
    // ... 原有创建逻辑
}
```

或者在 capture supervisor 中避免不必要的 `start()` 调用：

```kotlin
if (wanted) {
    if (ScreenCapture.needsRestart()) {
        // 只有在真正需要时才重建
        if (ScreenCapture.activeSize() == 0 || needsGeometryRebuild()) {
            ScreenCapture.start(size)
        }
    } else {
        ScreenCapture.setCrop(size)
    }
}
```

### P1 — 降低内存占用

1. **减小 ImageReader 缓冲区**：`kMaxImages = 2`（从 3 改为 2），节省 ~25MB
2. **降低虚拟显示器分辨率**：如果不影响检测精度，可以用半分辨率
3. **在不需要 capture 时立即释放**：确保 `UI_OFF` 时 `ScreenCapture.stop()` 被调用

### P2 — 增强 adbd 崩溃恢复

当前的 `scheduleReconnect()` 已有指数退避，但建议：
- 在 `onClosed` 中区分 adbd 崩溃和正常断开
- adbd 崩溃后等待更长时间再重连（adbd 重启需要时间）
- 考虑监听 `Intent.ACTION_MY_PACKAGE_RESTARTED` 广播

### P3 — 用户侧缓解

- 在系统设置中将 AimbotNextgen 加入**电池优化白名单**
- 关闭 ColorOS 的"智能省电"或"深度省电"模式
- 如果可能，卸载 `com.luckyzyx.luckytool`（它注入系统进程导致不稳定）

---

## 5. 附录：tombstone 统计

### aimbot_shell 崩溃（ClassNotFoundException — 早期调试阶段）

| Tombstone | 时间 | Abort Message |
|-----------|------|--------------|
| 20-25, 27-31, 00 | 10:02~11:47 | ClassNotFoundException: moe.shizuku.starter.ServiceStarter |
| 01-05 | 14:29~14:31 | ClassNotFoundException: ...ShellServerEntry |

### adbd 崩溃（系统级 bug）

| Tombstone | 时间 | Abort Message |
|-----------|------|--------------|
| 06 | 16:40 | adbd_auth: failed to read from framework fd |
| 08 | 17:04 | adbd_auth: failed to read from framework fd |
| 12 | 21:08 | adbd_auth: failed to read from framework fd |
| 16 | 21:40 | adbd_auth: failed to read from framework fd |

### 其他崩溃（第三方 app）

| Tombstone | 时间 | 进程 | 原因 |
|-----------|------|------|------|
| 07, 09, 10, 13-15, 17-19, 24 | 16:41~21:55 | com.oplus.battery | LuckyTool libdexkit.so 注入崩溃 |
| 11 | 19:52 | probe_dlopen | SIGSEGV（调试工具） |
