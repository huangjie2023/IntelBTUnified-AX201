# bt-wake-autofix

给「合盖唤醒后蓝牙掉一次」的兜底自愈。不修 bluetoothd，也不碰 kext。

## 为什么需要

`bluetoothd` 在唤醒后会自己调 `os_fault_with_payload(18, 2)` 主动退出（报告里是
`EXC_GUARD / SIMULATED / Application Triggered Fault`）。这是它做完自检后主动放弃，
不是内存被写坏：

- 崩溃帧 6 层全在 `bluetoothd` 自身，没有一帧是 kext，也没有 `IOUSBHost` / `IOKit` 帧
- `procLaunch` 恒定落在唤醒后 1.85 秒，存活 16.1 秒后退出 —— 固定超时，不是随机故障
- `-ibtleoff` / `-ibtphy2m` / 槽位表改动都不改变崩溃栈，逐帧一致

驱动侧已经无可改。剩下的只能是在它退出之后，把控制器重新上电、把设备连回去。

## 它做什么

LaunchDaemon 每 30 秒看一眼 `bluetoothd` 的 PID。**只有 PID 变了**（说明被 launchd 重新拉起过）
才动作，平时零开销：

1. 等 20 秒，让系统从唤醒里安静下来
2. `IOBluetoothPreferenceSetControllerPowerState(1)` 确保控制器上电
3. 遍历已配对设备，对 **trusted 且未连接** 的调 `openConnection`
4. 结果追加到 `/var/log/bt-autofix.log`（只追加，不会自己增长失控，可随时删）

## 安装

```sh
cd tools/bt-wake-autofix
sudo sh install.sh
```

安装脚本第 3 步会先自检 `IOBluetooth` 私有接口，**接口不存在就中止，不会留下半装状态**。

## 验证

```sh
sudo sh /usr/local/lib/bt-autofix/bt-wake-autofix.sh --check   # 只看不动
sudo sh /usr/local/lib/bt-autofix/bt-wake-autofix.sh --now     # 真的连一次
tail -20 /var/log/bt-autofix.log
```

`--check` 正常时应该看到 `controllerPower=1` 和每台设备一行 `connected` / `disconnected trusted=...`。

## 卸载

```sh
sudo sh install.sh uninstall
```

## 局限

- 依赖 `IOBluetooth` 的私有 C 接口。macOS 哪天把它删了，`--check` 会直接报 `ERROR`，不会静默失效。
- `openConnection` 只对 trusted 设备下手；如果设备本身关机或不在范围内，会失败，属正常。
- 这是兜底，不是修复。bluetoothd 该退还是会退，只是你不用再手动点连接。
