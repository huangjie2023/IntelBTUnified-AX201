# 案例研究：bluetoothd 唤醒后 SIGBUS（APFS 压缩数据哈希不匹配）

> 这份文档回答一个问题：**为什么能确定根因在 kext 的内存补丁，而不是硬盘、内存条或系统卷。**
> 结论建立在两个**与本机无关**的独立案例上，外加本机一条闭环实验。

---

## 案例 A：acidanthera/bugtracker#2228（决定性证据）

**案例**：[acidanthera/bugtracker#2228](https://github.com/acidanthera/bugtracker/issues/2228)
**报告人**：jaymatich，2026-08-28（第 5 条评论）
**环境**：MacBookPro12,1 / OCLP 2.4.1 / **FeatureUnlock 1.1.7** / Sequoia 15.7.8 与 15.7.9 / 两块不同 SSD
**规模**：32 次签名一致的崩溃，涉及 Control Center、Finder、Freeform、FaceTime、App Store、Safari

FeatureUnlock 和我们的 IntelBTUnified 是同一类东西：**Lilu 插件，在 `cs_validate_page` 钩子里原地改写目标进程的代码页**。区别只是目标文件不同（AirPlaySupport.framework vs bluetoothd）。

### 受控复现（这是最关键的部分）

对活跃的 `dyld_shared_cache_x86_64h.01` 做**只读全量 mmap 扫描**，在 AirPlaySupport 的 cache offset `0x111f9000` 处稳定复现。内核日志：

```text
decmpfs_pagein_compressed: Verification failed with error 94,
uplPos = 287277056, uplSize = 16384, did_read = 16384,
valid_pages = 3, invalid_pages = 1, tail_page_valid = 1
```

- `287277056 == 0x111f8000`，`16384 == 16 KiB` → 验证组是 `0x111f8000–0x111fbfff`
- `valid_pages = 3, invalid_pages = 1` → **组里 4 页，3 页驻留、1 页正在回填**
- errno 94 = `EBADMSG`

然后他把 FeatureUnlock 1.1.7 源码里全部 40 个补丁字节串拿去匹配这个组：

| 组内页 | 命中情况 |
|---|---|
| `0x111f8000` | **13 个 FeatureUnlock 补丁数组**（被改写的页） |
| `0x111f9000` | **出错的那页，零命中**（邻居页） |
| 另外两页 | 零命中 |

Sidecar 那组崩溃同样：出错页 `0x09d15000` 零命中，相邻页 `0x09d16000` 命中 14 个补丁数组。

**这一条推翻了“补丁页自己坏了”的直觉判断**：崩溃点永远落在**没被改过的邻居页**上，因为邻居页被回收后回填时，要与“已改写的邻居”一起过组校验。

### 排除硬件/盘上数据的对照组

| 对照组 | 结果 |
|---|---|
| APFS 卷完整性校验（两台安装、两块 SSD） | 通过 |
| NVMe 内部 SMART | 正常 |
| 普通全量 `pread` 扫描 | 通过 |
| 对签名 Cryptex `os.dmg` / `os.clone.dmg` 冷扫描 | 通过，且逻辑 cache 的 SHA-256 与在线 cache 相同 |
| 故障发生后**立刻**重跑两次 mmap 扫描 | 通过 |

结论原话（大意）：**这不是盘上永久损坏的字节。**

### 机制（5 步）

1. 认证的 APFS 加载并校验一个**原始的 16 KiB Cryptex 组**；
2. Lilu 插件在内存中改写该组里的**一个 4 KiB 页**；
3. 内存压力下，**相邻的另一页被逐出**，被改写的页仍驻留；
4. `decmpfs` 只回填缺掉的那页，然后 `VNOP_VERIFY` 校验**整个 16 KiB 组**；
5. “被改写页 + 原始回填页”的混合内容与认证哈希不符 → `EBADMSG` → 应用侧 `SIGBUS`。

报告人指出这个模型同时解释了：延迟出现（不是一打补丁就崩）、与负载/内存压力相关、`valid_pages = 3 / invalid_pages = 1` 这个比例、以及**为什么 `purge` 或重启能暂时修复**。

### 他提出的两个修复方向

1. 离线在 Cryptex 路径里改文件并重建认证 graft 元数据（等于“重新签一个系统”，我们做不了也做不到 OTA 安全）；
2. **把受影响的 16 KiB 组整组保持常驻**，使邻居页无法被单独回填到被改写页旁边。

我们的 1.9.1 走的是第三条等价路线：**在 `VNOP_VERIFY` 这一层，只对这两个文件的 `EBADMSG` 放过**。理由见下文“为什么这么选”。

---

## 案例 B：OpenIntelWireless/IntelBluetoothFirmware#493（同类驱动、同签名）

**案例**：[IntelBluetoothFirmware#493](https://github.com/OpenIntelWireless/IntelBluetoothFirmware/issues/493)
**标题**：*Bluetooth randomness crashes / The probability of crashing during sleep awakening is relatively high*
**环境**：AX210 / macOS 14.4.1 / 上游 kext 2.4.0 / SIP disabled
**状态**：closed，**零评论**——没人诊断出来

报告里的崩溃报告与本机逐项相同：

```text
Process:              bluetoothd [39512]
Path:                 /usr/sbin/bluetoothd
Exception Type:       EXC_BAD_ACCESS (SIGBUS)
Exception Codes:      KERN_MEMORY_ERROR at 0x000000010f3761df
Termination Reason:   Namespace SIGNAL, Code 10 Bus error: 10
Time Since Wake:      29 seconds

Kernel Triage:
APFS - Encountered a compressed data hash mismatch
VM   - Page has restart bit set
APFS - Pagein of compressed inode failed
VM   - Filesystem pagein returned an error in vnode_pagein

__TEXT  10f0e1000-10f9c0000 [9084K] r-x/r-x SM=COW  /usr/sbin/bluetoothd
```

**意义**：这不是 FeatureUnlock 的孤例，也不依赖某一块 SSD、某个 macOS 版本或某张网卡——Intel 蓝牙 kext 生态里同样的“改 bluetoothd 代码页”做法会打出同样的崩溃，而且同样没人能解释。上游 kext 也走 `cs_validate_page`，机制完全一致。

---

## 与本机签名的逐项比对

| 特征 | 案例 A（FeatureUnlock） | 案例 B（上游 IntelBT） | 本机 |
|---|---|---|---|
| 崩溃类型 | SIGBUS / KERN_MEMORY_ERROR | SIGBUS / KERN_MEMORY_ERROR | **相同** |
| 内核 triage | APFS compressed data hash mismatch | 同 | **同** |
| 落点 | AirPlaySupport `__TEXT` | bluetoothd `__TEXT` | **bluetoothd `__TEXT`** |
| 触发时机 | 长期使用 / 内存压力 | 唤醒后 29 s | **唤醒后 18–30 s** |
| 崩溃偏移 | 每次不同 | — | **不同（0x339B50 / 0x1E27A7 / 0x33A39E）** |
| 只有被改写的进程崩 | 是（Control Center 等） | 是（bluetoothd） | **是（只有 bluetoothd）** |
| APFS 卷校验 / SMART / 冷读 | 全过 | — | **全过** |
| `purge` / 重启 | 暂时修复 | — | **暂时修复（11-05 实测）** |

最后一行是闭环实验：2026-11-05 手动 `sudo purge` + `sudo launchctl kickstart -k system/com.apple.bluetoothd`，把“蓝牙在但连不上、一连就掉”救回来了。**盘上数据损坏不可能被 `purge` 修好**——`purge` 只是丢弃内存页缓存。这一条单独就足以否掉“硬盘坏块”。

---

## 之前误判为硬件的原因（复盘）

| 当时的判断 | 为什么错 |
|---|---|
| “两个 boot session 崩在不同偏移 → 两块不同的坏块” | 偏移由“当次补丁命中哪一页 + 哪一页被回收”决定，**每 boot 重新生成**，本来就是随机的 |
| “盘上有坏块，所以重装系统才治本” | 对照组显示盘上数据完好；且 `purge` 能修好，说明问题在内存 |
| “SSV 启动校验能过所以盘没问题” → 反过来推“那就是 kext 之外的东西” | 方向对了，但结论反了：SSV 过恰恰证明盘上没错，错的是内存里的混合页 |
| “SMART 90% 不能排除介质问题，建议换 SSD” | 忽略了更便宜的决定性实验：`purge`。它一次就能区分内存态与盘上态 |

用户当时坚持“硬盘没问题、内存也检测了没问题”，是对的。

### 红线（写在前面，防止再犯）

本故障的**所有**后续诊断都不得把结论推向介质。已经确认的三条事实：

1. 崩溃页 `0x339B50` 在 2026-11-05 21:29 与 2026-11-06 15:17 两个不同 boot、不同 ASLR 基址下**完全相同**——是确定性的代码路径，不是随机坏块；
2. `/usr/sbin/bluetoothd` 的 `mtime = Oct 22 18:38:16`（系统安装时间）、`shasum` 每次一致，盘上原件干净；
3. 两份独立案例（本机 + #2228 + #493）的对照组全部通过，其中一份还是在两块不同 SSD、两台不同安装上复现的。

所以当某条日志看起来“像是盘坏了”（比如 1.9.3 曾打的 `no recorded page in this group`），正确的解读只有两种：**我们的采样没覆盖到那一页**，或者**那一页已经被逐出后重新落位**。两者都是内存态。要真的指控介质，必须先有「同一只读文件、跨重启 `shasum` 不一致」这种直接证据。

---

## 1.9.1 具体做了什么

路由内核符号 `_VNOP_VERIFY`（与已有的 `_cs_validate_page` 同一套 Lilu `routeMultipleLong` 机制），条件全满足才把 `EBADMSG` 吞成 0：

1. 错误恰为 `EBADMSG`（真实 I/O 错误原样上抛）；
2. `buf != NULL`（是对数据的校验，不是块大小查询）；
3. `vn_getpath` 精确等于 `/usr/sbin/bluetoothd` 或 `/usr/sbin/BlueTool`。

> 1.9.1 原本还有第 4 条“本 boot 确实补写过这两个文件的页”，1.9.2 已删除；1.9.3 换成“失败区间含我们记录过的页”，1.9.4 又撤回。原因是 `cs_validate_page` 只在页首次 pagein 时调用，页驻留 file cache 后不再经过，任何基于“记录是否命中”的门控都会变成“一次都不吞”。**最终判据就是上面三条。**

系统其余所有文件的完整性校验不受影响。吞掉后 `decmpfs_pagein_compressed` 走正常 commit 分支（源码 `bsd/kern/decmpfs.c:1615` `if (!err)`），回填页内容是解压出的原始字节，本来就正确——校验失败纯粹来自内存里的混合页。

### 为什么不用“wire 住 16 KiB 组”

jaymatich 的方向 2 需要操作 `vm_page` / `vm_object` 私有结构与锁纪律（`vm_page_lookup`、`vm_page_wire` 都不是 KPI），在 Tahoe 上出错就是 kernel panic。而吞 `VNOP_VERIFY` 是**确定性**的：不管页何时被淘汰、何时回填，都不会再产生这个错误。两者都断链，选风险低的。

### 已知残余风险（诚实清单）

| 风险 | 影响 | 怎么发现 |
|---|---|---|
| macOS 26.2 内核里解析不到 `_VNOP_VERIFY` | 修复不生效，退回 1.9.0 行为（仍会崩） | `sudo dmesg \| grep "failed to route VNOP_VERIFY"` |
| 其他文件也出现 `EBADMSG` | 完全不受影响（按 vnode 路径过滤） | — |
| 真实的介质损坏恰好发生在 bluetoothd | 会被吞掉，不报警 | 该概率极低；SSV 每次开机加密校验系统卷，且本机对照组已排除介质。真出事的前置信号是跨重启 `shasum` 不一致 |
| 唤醒后的 `EXC_GUARD` 断言（另一条线） | 与本修复无关，可能仍出现 | 崩溃报告里 `Namespace SIMULATED` / `os_fault_with_payload(18,2)` |

### 验证方法

```sh
sudo dmesg | grep -E "VNOP_VERIFY|swallowed|failed to route"
kmutil showloaded | grep -i intelbt      # 应为 1.9.1
ls -lt ~/Library/Logs/DiagnosticReports/bluetoothd* 2>/dev/null | head
```

- 出现 `swallowed verify mismatch on /usr/sbin/bluetoothd @ 0x...` = 修复**确实命中**了以前会崩的那次（最重要的一行）
- 出现 `failed to route VNOP_VERIFY` = 没生效，需要换 hook 点
- 全新的 bluetoothd SIGBUS 崩溃报告 = 修复未覆盖到，需要复查

---

## 参考

- acidanthera/bugtracker#2228 — https://github.com/acidanthera/bugtracker/issues/2228
- OpenIntelWireless/IntelBluetoothFirmware#493 — https://github.com/OpenIntelWireless/IntelBluetoothFirmware/issues/493
- XNU `bsd/kern/decmpfs.c`（`decmpfs_pagein_compressed` 的 `VNOP_VERIFY` 调用在 1600 行）
- XNU `bsd/sys/vnode_if.h`（`VNOP_VERIFY` 原型，9 参数）
