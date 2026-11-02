# IntelBTUnified AX201

单 kext Intel 蓝牙驱动（Lilu 插件 + 固件加载驱动 + bluetoothd 补丁合一），**仅支持 AX201（USB 8087:0026）**，面向 macOS Tahoe 26.x 黑苹果 + OpenCore。

替代 `IntelBluetoothFirmware.kext` + `IntelBTPatcher.kext` + `BlueToolFixup.kext` 三个驱动（以及各类 1.0.12 老合体包）。

## 基线与改动

- 架构基于 [laguillo/IntelBTUnified](https://github.com/laguillo/IntelBTUnified)
- 补丁代码对齐 [lshbluesky/IntelBluetoothFirmware](https://github.com/lshbluesky/IntelBluetoothFirmware) **v2.5.2**（2026-10-18）：
  - LE Read Remote Features Complete 事件三段改写：假 PHY Update Complete + 假 LE Data Length Change，解开 bluetoothd 的 `LE_READY_WAIT_FOR_LENGTH` 卡死（#514）
  - 钩住 `IOUSBHostPipe::io` 与 `initWithDescriptorsAndOwners`（含 macOS 26.5+ 签名变化）
- **OTA 修复（process-scoped 补丁）**：bluetoothd/BlueTool 的内存补丁只在**该进程自身加载页面时**才应用。`softwareupdated` 等更新进程校验的是原始二进制，OTA 增量升级不再报 `bluetoothd does not verify / FullReplacementRequired`
- 固件取自 lshbluesky v2.5.2 的 `ibt-19-*` 全系列
- 保留上游改进：随机 BLE 地址每次开机生成、长度校验、原子状态、补丁应用日志

> 上游 FAQ 明确：**macOS Monterey 及以后不要使用 `IntelBluetoothInjector.kext`**（"Do NOT use this Kext on macOS Monterey and newer"）。Monterey+ 靠的是驱动把 USB 设备改名为 `Bluetooth USB Host Controller` 来绕过 bluetoothd/BlueTool 的 board-id 检查，所以单 kext 里**不该**有那条 Broadcom transport 注入 personality。本包结构对应全量包的 `IntelBluetoothFirmware` + `IntelBTPatcher` + `BlueToolFixup`，不含 Injector。

## 1.5.0 瘦身（只针对这一台 AX201）

目标机器固定为内置 AX201（USB 8087:0026，hw_variant 0x13，revision 32），不做通用化：

| 项 | 1.4.x | 1.5.0 |
|---|---|---|
| 固件 | `ibt-19-*` 全 20 个文件 | `ibt-19-32-{0,1,4}.sfi` + `.ddc` 共 6 个（内容去重后仅 2 个 blob） |
| 固件压缩体积 | 1,539,896 B | **482,220 B** |
| 驱动代码 | 含 Gen1/Gen3 死代码 | 只编 Gen2；`IntelBluetoothOpsGen1/Gen3` 已删除 |
| kext 二进制 | 1,636,568 B | 约 578,000 B |

`start()` 不再判断代际，直接实例化 `IntelBluetoothOpsGen2`，日志固定打印 `Selected ops: Gen2 (AX201 only)`。留 `ibt-19-32-0/1` 两个旧 build 只是为了让 `findFwBestMatch` 在芯片报 0 或 1 时仍能命中（它们与 `-4` 逐字节相同，不占额外空间）。

## 1.4.0 关键修复：AX201 用的是 `ibt-19-*`，不是 `ibt-17/18`

1.3.0 冷启动直接进不了系统，内核日志给出了完整答案：

```
IntelFirmware: Driver Start()
IntelFirmware: Selected ops: Gen2
IntelFirmware: Bootloader revision 0.4 build 0 week 11 2017
IntelFirmware: Device revision is 2
IntelFirmware: Firmware: ibt-19-32-4.sfi NOT PRESENT in kext
IntelFirmware: ibt-19-2.sfi not found, using fallback ibt-17-16-1.sfi
IntelFirmware: Intel boot failed
```

- `getFirmware()` 按 `ibt-<hw_variant>-<hw_revision>-<fw_revision>.sfi` 拼名。AX201 的 `hw_variant = 0x13 = 19`（源码注释里的 **HrP**，Harrion Peak），因此要的是 **`ibt-19-32-4.sfi`**。
- `0x11` = JfP（AX200，`ibt-17-*`）、`0x12` = ThP（`ibt-18-*`）、`0x14` = CcP。**17/18 从来不是 AX201 的固件**。laguillo 仓库默认 `FW="ibt-17-* ibt-18-*"` 这个假设是错的，热重启时芯片残留固件走 `-EALREADY` 跳过下载，所以一直没暴露。
- `ibt-19-2.sfi` 是"老命名格式"（`ibt-<variant>-<dev_revid>`），19 系列根本没有这个文件；而当时实现的 `findFwBestMatch()` 会跨变体做前缀回退，把 **AX200 的 `ibt-17-16-1.sfi` 灌给了 AX201** → RSA 校验/启动失败 → `Intel boot failed` → probe 失败 → IOKit 反复重试，于是开机疯狂刷日志、进不了系统。

1.4.0 的修法：

| 项 | 处理 |
|---|---|
| 固件集 | 只打包 `ibt-19-*`（20 个文件，`ibt-19-32-4.sfi` = 729372 B），`ibt-17/18-*` 全部移除 |
| 跨变体错配 | `findFwBestMatch()` 重写为**只在同 `hw_variant` 内回退**：解析 `ibt-<variant>-<rev>[-<build>]`，variant 不同直接跳过；同 variant 内优先同 revision，其次 build 高者 |
| 无效回退 | 删除 `getFirmwareLegacy()`（生成的 `ibt-19-2.sfi` 必然不存在，只是白跑一次） |
| 构建脚本 | CI 的 `make FW=` 参数与 Makefile `dist` 目标同步改为 `ibt-19-*`（改 Makefile 但漏改 workflow 是 1.3.0 仍打包 17/18 的原因之一） |
| 失败即死循环 | 固件不符时 IOKit 无限重试 probe，屏幕刷满日志、系统起不来 | 连续失败 3 次后停止尝试并打一行 `giving up (firmware/chip mismatch)`：蓝牙不可用，但系统能进 |

## 1.3.0 / 1.2.0 的历史修复（保留记录）

| 版本 | 问题 | 修复 |
|---|---|---|
| 1.3.0 | 冷启动首次 `downloadFirmwareData` 失败后直接放弃 | 回 bootloader → 等待 1 s → 重试一次 |
| 1.2.0 | AX201 被强制走 Gen3 路径（`start()` 只区分 Gen1 / 其它） | `kTypeGen2` 直接实例化 `IntelBluetoothOpsGen2` |
| 1.2.0 | 固件查找失败时在 `firmwareMode` 下静默 `return true`，驱动显示"已加载"但芯片仍在 bootloader | 区分 `NOT PRESENT in kext` 与真正加载成功，补 `fw_name` 属性 |
| 1.2.0 | 缺少定位日志 | 增加 `Selected ops: Gen2`、`Firmware file ... unavailable` 等 |

> 冷启动失败的典型链条：冷启动 → 芯片无正确固件 → 芯片停在 bootloader → bluetoothd 的 HCI 命令全部失败 → `EXC_GUARD` 自杀 / 驱动反复 probe。热重启时芯片仍带着上一次的固件，看起来"正常"，会掩盖问题。

## 安装（OpenCore）

1. 取 Actions 构建产物 `IntelBTUnified-1.5.0-AX201.zip`
2. 删除 EFI 里的：`IntelBluetoothFirmware.kext`、`IntelBTPatcher.kext`、`BlueToolFixup.kext`、`IntelBluetoothInjector.kext`、`IntelBluetoothBundle.kext`（以及任何合体版）
3. 把 `IntelBTUnified.kext` 放入 `EFI/OC/Kexts/`，在 `config.plist → Kernel → Add` 中**放在 Lilu 之后**
4. NVRAM 建议注入（Tahoe 蓝牙重新初始化）：
   - `bluetoothInternalControllerInfo` = 28 字节 0
   - `bluetoothExternalDongleFailed` = `00`
5. 冷启动验证：
   - `sudo dmesg | grep IntelFirmware` → 应看到
     `Found device firmware ibt-19-32-4.sfi` → `send firmware payload done` → `Notify: Firmware download done`
   - `ioreg -l -rc IntelBluetoothFirmware | grep fw_name` → `ibt-19-32-4.sfi`（热重启后显示 `(already running)` 属正常）
   - `system_profiler SPBluetoothDataType` → State: On
   - OTA 升级：系统设置里直接检查软件更新应能正常验证

若日志出现 `Intel boot failed` 且刷屏，说明灌进去的固件与芯片不符：立刻移除本 kext，把 `dmesg` 里 `Bootloader revision` / `Firmware:` 那几行贴出来。

## Boot-args

| 参数 | 作用 |
|---|---|
| `-ibtoff` / `-ibtcompatoff` / `-btlfxoff` | 禁用本插件 |
| `-ibtdbg` / `-ibtcompatdbg` / `-btlfxdbg` | 调试日志 |
| `-ibtbeta` | 允许比声明上限更新的 macOS |
| `-ibtota` | 完全关闭 bluetoothd/BlueTool 内存补丁（OTA 排障用，蓝牙会失效） |
| `-ibtplegacy` | 恢复老式无条件补丁（等价 BlueToolFixup，会破坏 OTA 校验） |
| `-btlfxboardid` / `-btlfxallowanyaddr` / `-btlfxnvramcheck` | 同 BlueToolFixup 原有开关 |

内核日志只出现在 `sudo dmesg`（`log show` 看不到），可加 `msgbuf=1048576` 扩大缓冲区。

## 构建

GitHub Actions 自动构建（macos-15-intel）。本地构建需要 macOS + Command Line Tools：

```
make deps                          # 克隆 MacKernelSDK + Lilu（版本固定）
make clean && make FW="ibt-19-32-*"
```

## 许可

GPL-3.0-only（继承上游）。固件遵循 `licenses/iwlwifi-firmware.txt`。

致谢：zxystd / OpenIntelWireless、lshbluesky、laguillo、acidanthera。
