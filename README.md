# IntelBTUnified AX201

单 kext Intel 蓝牙驱动（Lilu 插件 + 固件加载驱动合一），**仅支持 AX201（USB 8087:0026）**，面向 macOS Tahoe 26.x 黑苹果 + OpenCore。

替代 `IntelBluetoothFirmware.kext` + `IntelBTPatcher.kext` + `BlueToolFixup.kext` 三个驱动（以及各类 1.0.12 老合体包）。

## 基线与改动

- 架构基于 [laguillo/IntelBTUnified](https://github.com/laguillo/IntelBTUnified)
- 补丁代码对齐 [lshbluesky/IntelBluetoothFirmware](https://github.com/lshbluesky/IntelBluetoothFirmware) **v2.5.2**（2026-10-18）：
  - LE Read Remote Features Complete 事件三段改写：假 PHY Update Complete + 假 LE Data Length Change，解开 bluetoothd 的 `LE_READY_WAIT_FOR_LENGTH` 卡死（#514）
  - 钩住 `IOUSBHostPipe::io` 与 `initWithDescriptorsAndOwners`（含 macOS 26.5+ 签名变化）
- **OTA 修复（process-scoped 补丁）**：bluetoothd/BlueTool 的内存补丁只在**该进程自身加载页面时**才应用。`softwareupdated` 等更新进程校验的是原始二进制，OTA 增量升级不再报 `bluetoothd does not verify / FullReplacementRequired`
- 固件仅打包 AX201 所需的 `ibt-17-*` / `ibt-18-*`（**laguillo 原版固件**，与驱动下载逻辑配套）
- 保留上游改进：随机 BLE 地址每次开机生成、长度校验、原子状态、补丁应用日志

## 1.2.0 修复（冷启动后蓝牙打不开）

| 问题 | 说明 | 修复 |
|---|---|---|
| AX201 被强制走 Gen3 路径 | `start()` 只区分 Gen1 / 其它，gen2 芯片（AX201）落入 Gen3 的 legacy 兼容分支 | `kTypeGen2` 现在直接实例化 `IntelBluetoothOpsGen2`，与上游 IntelBluetoothFirmware 的 AX201 路径一致 |
| 固件被替换成 lshbluesky 新版 | `ibt-17-16-1.sfi` 由 664592 B 换成 675500 B，与本驱动的下载/bootloader 流程不配套；热重启时芯片里残留旧固件掩盖了问题，**冷启动必须重灌时才暴露** | 恢复 laguillo 原版 `ibt-17-*` / `ibt-18-*` |
| 固件名不匹配时静默成功 | 精确查找失败后，`firmwareMode` 为真会直接 `return true`，驱动显示"已加载"但芯片仍在 bootloader | 新增 `findFwBestMatch()` 前缀回退（`ibt-17-16-0` → `ibt-17-*` 最新版），失败时打印 `NOT PRESENT in kext` |
| 无固件来源日志 | 出错时只有一行模糊日志 | 增加 `Selected ops: Gen2`、`Firmware file ... unavailable (firmwareMode=x)` 等日志 |

> 冷启动失败的典型链条：冷启动 → 芯片无固件 → 驱动找不到/灌不进对应固件 → 芯片停在 bootloader → bluetoothd 的 HCI 命令全部失败 → `EXC_GUARD` 自杀循环、蓝牙开关打不开。热重启时芯片仍带着上一次的固件，所以看起来"正常"。

## 安装（OpenCore）

1. 从 Actions 构建产物下载 `IntelBTUnified-1.2.0-AX201.zip`
2. 删除 EFI 里的：`IntelBluetoothFirmware.kext`、`IntelBTPatcher.kext`、`BlueToolFixup.kext`、`IntelBluetoothInjector.kext`、`IntelBluetoothBundle.kext`（以及任何合体版）
3. 把 `IntelBTUnified.kext` 放入 `EFI/OC/Kexts/`，在 `config.plist → Kernel → Add` 中**放在 Lilu 之后**
4. NVRAM 建议注入（Tahoe 蓝牙重新初始化）：
   - `bluetoothInternalControllerInfo` = 28 字节 0
   - `bluetoothExternalDongleFailed` = `00`
5. 重启验证：
   - `ioreg -rc IntelBluetoothFirmware | grep fw_name` → 应显示 ibt-17-* 或 ibt-18-*（热重启后显示 `(already running)` 属正常）
   - `system_profiler SPBluetoothDataType` → State: On
   - OTA 升级：系统设置里直接检查软件更新应能正常验证

## Boot-args

| 参数 | 作用 |
|---|---|
| `-ibtoff` / `-ibtcompatoff` / `-btlfxoff` | 禁用本插件 |
| `-ibtdbg` / `-ibtcompatdbg` / `-btlfxdbg` | 调试日志 |
| `-ibtbeta` | 允许比声明上限更新的 macOS |
| `-ibtota` | 完全关闭 bluetoothd/BlueTool 内存补丁（OTA 排障用，蓝牙会失效） |
| `-ibtplegacy` | 恢复老式无条件补丁（等价 BlueToolFixup，会破坏 OTA 校验） |
| `-btlfxboardid` / `-btlfxallowanyaddr` / `-btlfxnvramcheck` | 同 BlueToolFixup 原有开关 |

内核日志只出现在 `sudo dmesg`（`log show` 看不到），可加 `msgbuf=1048576` 扩大缓冲区。关注 `ibtp`、`btlfx` 前缀。

## 构建

GitHub Actions 自动构建（macos-15-intel）。本地构建需要 macOS + Command Line Tools：

```
make deps                          # 克隆 MacKernelSDK + Lilu（版本固定）
make clean && make FW="ibt-17-* ibt-18-*"
```

## 许可

GPL-3.0-only（继承上游）。固件遵循 `licenses/iwlwifi-firmware.txt`。

致谢：zxystd / OpenIntelWireless、lshbluesky、laguillo、acidanthera。
