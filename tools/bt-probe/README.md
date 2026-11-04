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
| `btd=1 → btd=0`，`usb` 仍是 `1` | **设备还在总线上**，只是 bluetoothd 死了 | 不用重启。`sudo launchctl kickstart -k system/com.apple.bluetoothd` 大概率能拉回来 |
| `usb=1 → usb=0` | 设备掉出了 USB 总线 | 需要端口复位，通常只能重启。这是最坏的一种 |
| `pw=1 → pw=0` | 用户/系统把蓝牙关了 | 正常行为，不是故障 |

**这张表是装探针的全部意义。** 上一轮我判断「设备掉出了总线」，依据只有一条 `system_profiler | sed`——那条命令如果把错误吞进 stderr，就什么也不打印，和「设备不存在」看起来一模一样。所以那个结论**还没有站住**。探针的第一份 `TRANSITION` 才能定性。

---

## 关于 `autorecover`（实验性）

默认关闭。开启后，只在同时满足三个条件时才动手：

```
usb=1（设备在总线上） && pw=1（用户希望开着） && btd=0（进程没了）
```

然后执行一次 `launchctl kickstart -k system/com.apple.bluetoothd`，3 秒后复查，结果写进日志。

- 如果上一节的表格落到第一行，它能把「只能重启」变成「一分钟内自己回来」。
- 如果设备真的掉出了总线，它什么也做不了，只会在日志里留下 `AUTORECOVER: failed`——这也是有用的信息。

不要一上来就开。先跑纯观测，拿到第一份 `TRANSITION` 再决定。

---

## 注意

- 探针不解决 APFS 那一层。它只是让下一次故障**有据可查**。
- 日志 2MB 自动轮转，只保留 `.log` 和 `.log.1`。
- 如果 `launchctl print system/local.bt-probe` 里 `last exit code` 非 0，看 `/var/log/bt-probe.err`。
