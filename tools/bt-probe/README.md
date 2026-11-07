# bt-probe —— 蓝牙状态探针

抓「APFS 哈希不匹配 → bluetoothd 崩 → 蓝牙从系统里彻底消失 → 只能重启」这条链的现场。

**默认只读**：不 kickstart、不重启、不改任何系统设置。唯一副作用是每分钟跑一次 `ioreg` / `pgrep` / `defaults read`，外加一个日志文件。

---

## 为什么需要它

现在的问题是**每次故障都在重启之后才被发现**，证据全被重启清掉了。这个探针解决的就是这一点：状态一变就落盘，把当时的内核日志一起抓下来。

它盯三个量：

| 量 | 含义 | 采集方式 |
|---|---|---|
| `usb` | USB 总线上还有没有 `Bluetooth USB Host Controller` | `ioreg -p IOUSB -w0` |
| `btd` | `bluetoothd` 进程在不在 | `pgrep -x bluetoothd` |
| `pw` | 用户希望蓝牙开着的状态 | `defaults read /Library/Preferences/com.apple.Bluetooth ControllerPowerState` |

---

## 装

```sh
cd tools/bt-probe
sudo sh install.sh               # 纯观测，推荐先用这个
sudo sh install.sh autorecover   # 观测 + 实验性自愈
```

卸载（保留日志）：

```sh
sudo sh install.sh uninstall
```

---

## 日志怎么读

```sh
sudo tail -50 /var/log/bt-probe.log
```

正常时只有心跳，每 30 分钟一行：

```
2026-11-04 20:00:12 heartbeat | usb=1 btd=1 pw=1 | 1:20 up 2 users, load averages: 0.42 0.55 0.61
```

出问题时是一段 `TRANSITION` + 上下文：

```
2026-11-04 23:41:08 TRANSITION | usb=1 btd=1 pw=1 -> usb=1 btd=0 pw=1
2026-11-04 23:41:08 ---- context begin ----
...
2026-11-04 23:41:08 ---- context end ----
```

### `TRANSITION` 的三种组合，对应完全不同的处置

| 变化 | 含义 | 处置 |
|---|---|---|
| `btd=1 → btd=0`，`usb` 仍是 `1` | **设备还在总线上**，只是 bluetoothd 死了 | 不用重启。手动恢复见下面「验证有效的手动自救」 |
| `usb=1 → usb=0` | 设备掉出了 USB 总线 | 需要端口复位，通常只能重启。这是最坏的一种 |
| `pw=1 → pw=0` | 用户/系统把蓝牙关了 | 正常行为，不是故障 |

**这张表是装探针的全部意义。** 上一轮我判断「设备掉出了总线」，依据只有一条 `system_profiler | sed`——那条命令如果把错误吞进 stderr，就什么也不打印，和「设备不存在」看起来一模一样。所以那个结论**还没有站住**。探针的第一份 `TRANSITION` 才能定性。

---

## 验证有效的手动自救（2026-11-05 实测）

蓝牙「在但连不上、一连就掉」时，两条命令恢复（已实测成功）：

```sh
sudo purge
sudo launchctl kickstart -k system/com.apple.bluetoothd
```

**为什么有效**：IntelBTUnified 的 `patched_cs_validate_page` 钩子会在内存里修改 bluetoothd 的代码页（vendor check / chipset range / board-id）。这页属于盘上某个 APFS 16KB 压缩验证组；内存压力下该组内页面被逐出，之后 pagein 时组哈希校验失败（EBADMSG → SIGBUS），或者进程加载到「半旧半新」的坏页——表现就是 bluetoothd 活着但行为异常。

- `purge` 把整个统一 page cache 冲掉，坏页被丢弃；
- `kickstart` 让 bluetoothd 重新拉起，从盘上加载干净的原始页，再由钩子重新打补丁——这次是完整的。

单独 `kickstart` 有时不够：如果坏页还留在 UBC（统一缓冲缓存）里，新进程重新 mmap 时拿到的还是那页坏数据。**先 purge 再 kickstart 才是完整序列。**

---

## 关于 `autorecover`（实验性）

默认关闭。开启后，**两个触发源任一满足**就动手，且都要求 `usb=1`（设备在总线上）`pw=1`（用户希望开着）：

```
A. btd=0                                  进程没了（老判据）
B. 出现新的崩溃报告                        1.9.11 起新增，主力判据
```

B 的来源是 `/Library/Logs/DiagnosticReports/` 下 `bluetoothd*.ips` 与 `ExcUserFault*.ips` 的最新 mtime 变化。首次运行只建基线，不会立刻触发；同一份报告只触发一次；两次恢复之间至少隔 120 秒。

**为什么必须加 B**：崩溃后 launchd 会在 10 秒 throttle 后自己把 bluetoothd 拉起来，探针 60 秒一轮，看到的永远是 `usb=1 btd=1 pw=1` 这个「正常」三元组。老判据在这个窗口里一次都不会触发，于是「蓝牙能开、连不上设备、一连就自动开关」这种带坏页的活体状态永远是手动处理。B 把「进程在不在」换成「有没有崩过」，才真正盖住这一段。

动作序列（2026-11-05 起为 purge + kickstart，2026-11-07 加崩溃触发）：

1. `purge` 冲掉 page cache（清掉可能存在的坏页），并记录耗时
2. `launchctl kickstart -k system/com.apple.bluetoothd`
3. 4 秒后复查；若失败，隔 10 秒再完整来一轮，仍失败才记 `AUTORECOVER: failed`

- 如果设备真的掉出了总线（`usb=0`），它什么也不做，只在日志里留下原因——这也是有用的信息。
- 冷却是必需的：连续崩溃会连着产生多份报告，purge 本身很重，不能每 60 秒来一次。

现在可以开了：purge + kickstart 序列已经实测验证。

---

## 注意

- 探针不解决 APFS 那一层。它只是让下一次故障**有据可查**。
- 日志 2MB 自动轮转，只保留 `.log` 和 `.log.1`。
- 如果 `launchctl print system/local.bt-probe` 里 `last exit code` 非 0，看 `/var/log/bt-probe.err`。
