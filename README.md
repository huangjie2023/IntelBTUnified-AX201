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

## 1.8.0：冷启动偶发「蓝牙正常但连不上设备」

1.7.0 实测：OTA 正常、日常可用，但**有时候冷启动**后蓝牙能开、能看到设备，却连不上，且没有任何崩溃日志。

根因在 LE 事件改写：上游（lshbluesky v2.5.2）用一个**全局** `leMetaStage` 静态变量，按「第 1 个事件放行 / 第 2 个改写为假 PHY / 第 3 个改写为假 Data Length」推进。这只有在**连接一个一个来**时才成立。冷启动时系统会把多个已配对设备同时重连，事件在管道上交错：

```
A1 B1 A2 B2 A3 B3      <- 三个事件一组，每组带同一个 connection handle
```

全局计数器就会错位：A 的真实 features complete 被当成「第 1 个」放行过（或 B 的会被改写成假 PHY），于是某个设备**永远收不到自己的 features complete**，卡在连接建立阶段。表现就是「驱动正常、界面正常、就是连不上、无崩溃」——因为协议层失败，不涉及崩溃。

1.8.0 把状态改成**按 connection handle 索引的小表**（4 个 slot）：

| 改动 | 作用 |
|---|---|
| 每个 handle 独立 stage 0→1→2 | 多设备同时重连不再互相踩 |
| 只在命令里的 handle 上注册，未注册的 features 事件**原样放行** | 不再误改别人的事件 |
| `status != 0` 时直接丢弃 slot | 失败事件不该被改写 |
| handle 空闲 256 个事件后自动过期；`HCI_Reset` 清空全表 | 防止 handle 复用继承错位状态 |
| 假事件写入改为**局部拷贝** | 两条连接并发完成时不共享可变缓冲 |
| 新增 `le h=0xNNN ...` 日志（每 boot 前 16 条，1.9.0 起需 `-ibtpdbg`） | 需要时能看到某个 handle 是否走完三段 |
| 新增 `[PATCH] event pipe captured / mismatch` 日志 | 若捕获到的 pipe 不是实际收发事件的那个，此前会静默失效 |
| 新增 `-ibtleoff` | 关掉 LE 改写做隔离验证 |

排障脚本：`sh tools/bt-diag.sh`（只读，收集 kext 版本、补丁命中、固件日志、LE 改写、崩溃报告、控制器信息）。

### 1.9.0：唤醒后 bluetoothd 断言（EXC_GUARD）的两处修正

现象：合盖睡眠唤醒后 bluetoothd 在约 16 秒时 `EXC_GUARD / GUARD_TYPE_USER / namespc 18 / reason 2` 主动终止，随后 launchd 拉起、蓝牙显示关闭，手动开启即恢复。四次样本中三次发生、一次未发生，且 `Time Since Wake` 恒为 18 秒、崩溃栈 6 帧偏移完全一致——固定超时后主动断言，不是随机内存故障。

1. **假 PHY Update 谎报 2M**。`fakePhyUpdateCompleteEvent` 沿用了上游的 `tx=LE 2M, rx=LE 2M`，但驱动从未下发 `HCI_LE_Set_PHY`，控制器实际仍在 LE 1M。bluetoothd 认为链路已升级、芯片仍在 1M，这条不一致在唤醒重连时最容易暴露。1.9.0 改为上报 `LE 1M`（真实值）。`-ibtphy2m` 可恢复旧行为用于对照。
2. **槽位表满时原样放行**。1.8.0 的做法是表满就不注册，该 handle 的三个 `features complete` 会全部交给 bluetoothd（它只请求了一次）。1.9.0 改为淘汰 `idle` 最大的槽位、保留新连接。

对照方法：默认（1M）跑几轮睡眠；若仍崩，加 `-ibtphy2m` 再跑，两者都崩则加 `-ibtleoff` 判断是否与 LE 改写有关。

> **1.9.0 实测结果：假说证伪。** 默认 1M 连跑 5 次合盖睡眠，崩 4 次、未崩 1 次，比例与 1.8.x 没有区别。报 2M 不是诱因，槽位表也不是——因为唤醒流程根本不建立新连接。两个修正保留（1M 是真实值，LRU 比原样放行合理），但它们不解决唤醒断言。

### 已排除：唤醒时屏幕中央的圆形模糊（与蓝牙无关）

合盖唤醒后屏幕中央出现约 2 秒的模糊，形状是圆形而非矩形，1.8.0 / 1.8.1 / 1.9.0 三个版本表现完全一致。

三个版本在 LE 状态机（全局 → 4 slot）、日志机制（SYSLOG → 删掉 → DIAGLOG）、PHY 上报（2M → 1M）上差异很大，现象却一模一样，因此与 kext 逻辑无关。圆形也对不上任何「某块区域重绘失败」的故障形态，那是矩形。

结论：这是 WindowServer 唤醒后重建渲染管线时的径向模糊过渡动画。这台机器的 Ice Lake G4 核显在 Tahoe 下频率被锁在 300MHz（WhateverGreen 目前不做 GPU DVFS），GPU 密集的 backdrop blur 就拖到了 2 秒。

它和 bluetoothd 的 16 秒断言**没有因果关系**，但两者共享同一个背景：唤醒后用户态恢复整体偏慢（8GB 内存 + 无 DRAM 的 Phison E13T，page in 慢）。所以这条现象不能当作「USB / IO 阻塞」的证据，不要再顺着它查。

### 定性完成：唤醒断言与 kext 无关（三个假说全部证伪）

`-ibtleoff` 连跑两次合盖睡眠，**两次都崩**，崩溃栈与 1.9.0 默认配置**逐帧一致**（1414646 / 951136 / 5312934 / 5319185 / 5175992 / 5174993），`r14` 恒 1201，`Time Since Wake` 恒 18。至此三条线全部关闭：

| 假说 | 版本 | 结果 |
|---|---|---|
| 假 PHY Update 谎报 2M | 1.9.0 | 5 崩 4，与 1.8.1 的 4 崩 3 无差异 → 证伪 |
| 槽位表满原样放行 | 1.9.0 | 同上，且唤醒流程根本不建立新连接 → 证伪 |
| LE 事件改写是诱因 | `-ibtleoff` | 2 崩 2，栈完全不变 → 证伪 |

重新定性这份报告，结论和之前不一样：

- `is_simulated: 1`、`Exception Note: SIMULATED (this is NOT a crash)`、`Termination Reason: Application Triggered Fault` —— **它不是崩溃，是 bluetoothd 自己调 `os_fault_with_payload(18, 2)` 主动退出**，macOS 把这套动作记成"模拟故障"并生成报告。
- 崩溃纹 6 帧**全部落在 bluetoothd 自身**，没有一帧是 kext 代码，也没有任何 `IOUSBHost` / `IOKit` 帧。驱动没参与这个调用。
- `procLaunch` 两次都落在唤醒后 **1.85 秒**，存活 **16.1 秒** 后退出（两次 16.164 / 16.111 秒）。不是"等不到设备所以慢"，是**固定超时到点后主动放弃**。
- `codeSigningValidationCategory: 1`（PLATFORM 二进制）、`SIP: enabled`。

合起来：bluetoothd 在"非 Apple 硬件 + 自身代码页被内存改写"的环境里跑到某个唤醒重连路径，自检不过就主动结束自己。**这不是内存被写坏，也不是驱动时序问题，kext 侧没有可改的东西了。**

剩下的空间只有两条，都不属于驱动：`pmset` 层面降低唤醒恢复压力，以及让 bluetoothd 退出后自愈得更彻底。

### 2026-11-04：bluetoothd 变成崩溃循环，根因在系统卷而不是驱动

新现象与上面那份 `EXC_GUARD` **不是一回事**：`EXC_BAD_ACCESS (SIGBUS)` + `KERN_MEMORY_ERROR`，内核 triage 是 `APFS - Encountered a compressed data hash mismatch` / `Pagein of compressed inode failed` / `VM - Filesystem pagein returned an error in vnode_pagein`，落点仍在 bluetoothd 的 `__TEXT`。

关键一条是内核日志：

```
(apfs) apfs_pagein:15350: disk1s4 ino 1152921500312575936/... f_offset 3399680 size 4096 ... error 94 (bluetoothd)
```

`error 94` = `EBADMSG`，每 10 秒一条，最后 `Too many corpses being created` —— bluetoothd 在 page fault 阶段就死了，进程根本没起来。

**可以确定与 kext 无关**，两条独立理由：

1. decmpfs 的哈希校验发生在 `vnode_pagein` 解压完成的那一刻，`_cs_validate_page` 是 pagein **成功之后**才被调用；pagein 失败时钩子不执行。报错的是「数据从哪来」的环节。
2. `patched_cs_validate_page` 只做 `KernelPatcher::findAndReplace(const_cast<void*>(data), ...)` 原地 memcpy，没有 `ubc_upl_commit_range` / `UPL_COMMIT_SET_DIRTY`，页不会被标脏，也不会被写回；何况文件在密封只读快照里。

因此 `-ibtota` 在此状态下无作用，不要用它做判定。

坏块位置：`0x370000–0x37FFFF`（11-03 21:59）与 `0x330000–0x33FFFF`（11-04 10:33），分属两个 boot session，说明是落盘数据。结合作业日志里每次开机的 `reloading after unclean unmount` 与并存的 DC6 kernel panic，成因指向「反复非正常关机 + 无 DRAM 无掉电保护的 Phison E13T」。

修复只能动系统卷：**见 [`REPAIR-SYSTEM-VOLUME.md`](REPAIR-SYSTEM-VOLUME.md)**。要点：先备份 → 看重装/快照 → 做完立刻加 `-nodisplaysleepDC6` 止住 panic → 几天内若在新文件新偏移上复发即换 SSD。

### 1.8.1：把诊断日志移出 USB 热路径

功能与 1.8.0 完全相同。1.8.0 为了排查，把 LE 三段的日志放在 `IOUSBHostPipe::io` 完成回调里，而 Lilu 的 `SYSLOG` 是同步 `IOLog`，放在 USB 事件回调用属于不必要的开销。1.8.1 把这些打印从 `SYSLOG` 撤下。**注意：当时的做法是改 `DBGLOG`，而 Lilu 的 `DBGLOG` 在非 DEBUG 构建里会被编译成空语句，字符串根本不进二进制，也就是说那批日志实际是被删掉了，加 `-ibtpdbg` 也看不到。** 1.9.0 改为自建 `DIAGLOG`：运行时判断 `diagEnabled`，默认不打印，加 `-ibtpdbg` 才输出，热路径上只有一个 bool 比较。

## 1.6.0 / 1.7.0：睡眠唤醒 SIGBUS 的排查（补丁是必需的）

现象：1.5.0 冷启动、连接都正常，**合盖唤醒后 bluetoothd 崩溃**，崩溃点落在 `/usr/sbin/bluetoothd` 的 `__TEXT`，内核 triage 是：

```
APFS - Encountered a compressed data hash mismatch
APFS - Pagein of compressed inode failed
VM   - Filesystem pagein returned an error in vnode_pagein
```

两次崩溃的故障地址换算成文件偏移**完全相同**（`0x339B50`，页 `0x339000`），bluetoothd 进入 `consecutiveCrashCount` 递增的重启循环。

怀疑对象是 `cs_validate_page` 钩子：系统卷上的 `bluetoothd` 是 decmpfs 压缩文件，钩子拿到的 `data` 是「已解压的页」，`findAndReplace()` 直接原地改字节，页与压缩数据缓存不再一致。

**1.6.0 做了实验**：默认不挂 `_cs_validate_page` 钩子，任何页面都不碰。结果——**蓝牙完全不启动，也不崩溃**。说明这几处 bluetoothd 补丁在 macOS 26.7.2 + AX201 上是必需的，不能省。

**1.7.0 恢复补丁，并把每次补丁的文件偏移打进日志**，用来验证崩溃页是否就是我们改过的那一页：

```
btlfx: patched /usr/sbin/bluetoothd: chipset range check (15.4) (page 0x........, file offset 0x........)
```

| boot-arg | 行为 |
|---|---|
| 无 | 按进程打补丁（默认，OTA 友好） |
| `-ibtplegacy` | 老式无条件打补丁（等价 BlueToolFixup） |
| `-ibtota` | 完全关闭补丁（蓝牙会不启动，仅排障用） |

log: `sudo dmesg | grep btlfx`

**现状（1.9.0 之后）**：这条 `APFS compressed data hash mismatch` 自 1.7.0 起未再复现。它和唤醒后的 `EXC_GUARD / Application Triggered Fault`（bluetoothd 主动退出，见 1.9.0 节）是**两条完全不同的路径**，不要混在一起看——前者是存储层 pagein 失败，后者是进程自己结束。

机制上的隐患仍在：钩子改的是 decmpfs 压缩文件的页缓存，改完之后这一页的内容与磁盘上的压缩块不再一致。1.7.0 加 offset 日志本来就是为了验证「崩的那页是不是我们改过的」，但 1.7.0 之后没再复现，所以这个问题至今没有定论。主要变量更可能是无 DRAM 的 Phison E13T 本身——压缩数据损坏是它的典型故障形态。

要从机制上彻底消除，只能不再碰页：把 `cs_validate_page` 换成 Lilu `UserPatcher`，改进程内存而不是页缓存。代价是要降 SIP（`csr-active-config`）、重做 patch 注入，且 1.6.0 已证明「不 patch 蓝牙就不启动」，没有回退余地。

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

1. 取 Actions 构建产物 `IntelBTUnified-1.9.0-AX201.zip`
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
   - 冷启动连接异常时跑 `sh tools/bt-diag.sh`，把输出留档

若日志出现 `Intel boot failed` 且刷屏，说明灌进去的固件与芯片不符：立刻移除本 kext，把 `dmesg` 里 `Bootloader revision` / `Firmware:` 那几行贴出来。

## Boot-args

| 参数 | 作用 |
|---|---|
| `-ibtoff` / `-ibtcompatoff` / `-btlfxoff` | 禁用本插件 |
| `-ibtdbg` / `-ibtcompatdbg` / `-btlfxdbg` | 调试日志 |
| `-ibtbeta` | 允许比声明上限更新的 macOS |
| `-ibtota` | 完全关闭 bluetoothd/BlueTool 补丁（关闭后蓝牙不启动，仅排障） |
| `-ibtplegacy` | 恢复老式无条件补丁（等价 BlueToolFixup，会破坏 OTA 校验） |
| `-ibtleoff` | 关闭 LE Read Remote Features 事件改写（隔离「连不上又没崩溃」用） |
| `-ibtphy2m` | 假 PHY Update 恢复上报 LE 2M（1.9.0 之前的行为，仅用于对照） |
| `-ibtpdbg` | 打开 LE 槽位/事件改写诊断日志（默认关闭，热路径零输出） |
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
