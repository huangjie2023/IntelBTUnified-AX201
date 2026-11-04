# bluetoothd 崩溃循环：系统卷修复手册

> 适用症状：bluetoothd 每 10 秒崩一次，最后 `Too many corpses being created`，蓝牙完全不可用。
> 本文件与 IntelBTUnified 无关，**不要试图改 kext 解决**。

---

## 一、症状指纹（用来确认是不是同一件事）

`sudo dmesg` 或 `log show --last boot` 里出现：

```
kernel: (apfs) apfs_pagein:15350: disk1s4 ino 1152921500312575936/... f_offset 3399680 size 4096 ... error 94 (bluetoothd)
kernel: (AppleMobileFileIntegrity) AMFI: Denying core dump for pid 169 (bluetoothd)
kernel: bluetoothd[169] Corpse allowed 1 of 5
...
kernel: Process[1408] crashed: bluetoothd. Too many corpses being created.
```

崩溃报告（Crash Reporter）里：

```
Exception Type:  EXC_BAD_ACCESS (SIGBUS)
Exception Subtype: KERN_MEMORY_ERROR at 0x...  (落在 /usr/sbin/bluetoothd 的 __TEXT 区)
Kernel Triage:
  APFS - Encountered a compressed data hash mismatch
  APFS - Pagein of compressed inode failed
  VM   - Filesystem pagein returned an error in vnode_pagein
```

注意：这份 Triage 与 2026-11 之前那种 `EXC_GUARD / namespc 18 / reason 2 / SIMULATED` 是**两件完全不同的事**。后者是 bluetoothd 自己 `os_fault` 退出（几秒自愈）；前者是磁盘上的代码页读不出来，进程根本起不来。

---

## 二、为什么可以 100% 排除 IntelBTUnified

两条，任何一条单独成立就够：

1. **时序上不可能。** decmpfs 的哈希校验发生在 `vnode_pagein` 里、页数据解压完成的那一刻。
   `_cs_validate_page` 是 pagein **成功之后**才被调用的。pagein 失败时，我们的钩子根本没被执行，也就没有任何机会去写这页。
   → 报错的是“数据从哪来”的环节，不是“拿到之后谁改了它”。

2. **代码上写不回去。** `patched_cs_validate_page` 里只做 `KernelPatcher::findAndReplace(const_cast<void*>(data), ...)`，直接在页的内核映射上 memcpy。
   全程没有 `ubc_upl_commit_range()`、没有 `UPL_COMMIT_SET_DIRTY`，页不会被标脏，VM 不会写回。
   而且 bluetoothd 在密封只读系统卷的快照里，物理上也写不进去。

所以：`-ibtota` 这个开关在当前状态下**没有任何作用**，不必为了排查去加它。

---

## 三、坏在哪

| 现场 | 缺失的 64K 压缩块 |
|---|---|
| 11-03 21:59 崩在 `0x37AA1C` | `0x370000 – 0x37FFFF` |
| 11-04 10:33 崩在 `0x33A39E`，pagein 报 `f_offset 0x33E000` | `0x330000 – 0x33FFFF` |

两次分属**不同的 boot session**（`2D44FF2E…` / `3EA11526…`），坏块位置一致，说明是**落盘的数据**坏了，不是内存。
两个不同区块先后坏，也不是“某一次写坏了”，而是持续在丢。

成因链（按证据强度排序）：

1. 开机日志每次都出现 `nx_mount: disk1s4 reloading after unclean unmount, checkpoint xid 1306505, superblock xid 1302679` —— 卷没有被干净卸载。
2. 同期有 `AppleIntelICLLPGraphicsFramebuffer` 的 `DC6 is already enabled` kernel panic。panic = 非正常关机 = 掉电断在半路。
3. 硬盘是 Kingston RBUSNS8154P3512GJ1（Phison E13T，**无 DRAM 缓存，无掉电保护**）。这类盘在异常掉电时最容易丢 FTL 映射表 / 写缓存，表现正好是：读回来的是“合法但错误”的数据 → 解压哈希对不上 → EBADMSG(94)。
4. 同一份日志里 `spaceman_scan_free_blocks: trims took 17.255171 s`（16.7 万段），trim 慢到 17 秒，也是这块盘在高负载下吃力的旁证。
5. **这不是第一次，而且坏在同一个地方。** 1.5.0 时期的同款故障，崩溃偏移恒定 `0x339B50`，页 `0x339000`；今天这次是 `0x33A39E` / `0x33E000`。
   `0x339000`、`0x33A000`、`0x33E000` **全部落在同一个 64K 压缩块 `0x330000–0x33FFFF` 里**。
   相隔几个月、跨多个系统版本、跨多次断电重启，反复击中同一段 16 万字节——这不可能是巧合，也不是"某一次写坏了"，而是这块介质上有一片区域持续不可靠。

---

## 三点五、"我查了硬盘，寿命还有 90%，没问题啊"

这条**不能作为反证**。要把三个不同层次分清：

| 你在工具里看到的 | 它实际测的是什么 | 能不能发现本问题 |
|---|---|---|
| `Percentage Used: 10%` / "寿命 90%" | **写入磨损**：已消耗的 TBW 占设计总写入量的比例 | ❌ 完全无关 |
| `SMART Status: Verified` / 自检 passed | 控制器能应答、介质能完成一次内部读写自检 | ❌ 读回路自检不比对文件层哈希 |
| 剩余寿命、温度、通电次数 | 健康度**估算** | ❌ 估算，不是校验 |

具体为什么漏：

1. **这个文件从来不写。** `/usr/sbin/bluetoothd` 在密封只读系统卷快照里，装完系统后一个字节都没被写过。写磨损指标对它是空的——磨损 0%，照样可能读不出来。
2. **坏的是"读回来的内容不对"，不是"读失败"。** 无 DRAM 的盘丢了 FTL 映射表之后，同一个 LBA 会映射到别的物理页，控制器读到的是**另一段合法数据**，ECC 校验通过、自检通过、SMART 一个计数都不涨，只有上层比对哈希才发现不对。SMART 里唯一可能沾边的是 `Media and Data Integrity Errors`，而它只统计 **ECC 失败**，静默错数据不计入。所以它显示 0 也不代表清白。
3. **decmpfs 的哈希比对发生在文件系统之上。** 硬盘固件根本不知道"这个 64K 压缩块解压后应该是什么"，它没有能力发现。

### 可以直接验证的判别实验（不用装任何东西）

读那个文件本身，看是不是真的读不出来：

```sh
sudo dd if=/usr/sbin/bluetoothd of=/dev/null bs=4096 2>&1 | tail -3
```

- **报 `Input/output error`**，且 `records in` 停在 826 / 830 / 891 附近 → 文件数据确实读不出来，介质/映射层问题，SMART 说什么都不重要。
- **完整读完全部页、一个错都没有** → 那硬盘是清白的，我的判断错了，问题在 decmpfs 解压路径，届时再换方向查。

想看得更细，逐页打点：

```sh
for p in $(seq 700 1000); do
  dd if=/usr/sbin/bluetoothd of=/dev/null bs=4096 skip=$p count=1 2>/dev/null \
    || echo "BAD page $p  offset $((p*4096))  0x$(printf %x $((p*4096)))"
done
```

顺带核对一件事：同一次抓取里，`log show --last boot | grep -i "compressed data hash mismatch"` 有没有 `(bluetoothd)` **以外**的进程名。有 → 扩散了；只有 bluetoothd → 局部。

### 实测结果（2026-11-04 12:38，`seq 700 1000` 逐页）

**702 个页（覆盖 `0x2BC000–0x3E8000`，含报错的 `0x33E000` / 页 830 和 `0x370000` / 页 880），一个 `BAD page` 都没有。**

按上面自己写的判定标准，这一条读下来是"盘清白、我判断错了"。但要老实补三句：

1. **这个实验比我当初说的弱。** `dd` 走的是 `read(2)` → `apfs_vnop_read` → decmpfs 解压；崩溃走的是 vnode pager → `apfs_pagein` → 解压 + 哈希比对。两条路复用同一套解压代码，所以**大概率**同样会撞哈希；但我没有在本机验证过 read 路径一定跑那一步比对。所以"全过"只能说明"数据现在读得出来、解得出"，不能 100% 反证 pagein 一定没问题。
2. **有个前置项必须先排掉**：如果 macOS 在这两次崩溃之后换过 `/usr/sbin/bluetoothd`（系统更新/快照重建），dd 读的就是另一个文件，实验作废。先看 `stat` 的 inode 与 mtime。
3. **一旦确认是同一个文件**，那么"落盘数据坏了"这条就该降级：坏的不是存着的字节（现在读得出来），而是那两次 boot session 里**解压出来的结果**。方向要转到：内存/页缓存、解压路径、或那一瞬间的读回路，而不是"介质上有一片死区"。

真正决定性的动作不是 dd，而是**让 bluetoothd 再 exec 一次**——exec 才会走 pagein：

```sh
stat -f "inode=%i size=%z mtime=%Sm" /usr/sbin/bluetoothd
pgrep -lf bluetoothd
sudo launchctl kickstart -k system/com.apple.bluetoothd && sleep 3
pgrep -lf bluetoothd
log show --last 1m --predicate 'eventMessage CONTAINS "apfs_pagein" OR eventMessage CONTAINS "compressed data hash mismatch"' --info | tail -20
```

- kickstart 后进程又没了 / 又出 `apfs_pagein ... error 94` → 现在就复现，问题还在，走第四节重装。
- kickstart 后正常起来、没有 APFS 报错 → 是间歇性，不是常驻损坏。这时不必急着换盘，但要看它下次睡眠唤醒后还犯不犯。

---

## 四、修复步骤

### 0. 先备份（不要跳过）

坏块会继续增加。先保住数据：

```sh
# 确认外置盘挂载点
ls /Volumes
# 用 Time Machine 或直接 rsync 用户数据
sudo rsync -aH --info=progress2 /Users/caomuzi/ /Volumes/<外置盘>/backup-home/
```

### 1. 看看有没有今天之前的本地快照

```sh
tmutil listlocalsnapshots /
```

如果列表里有 11-03 之前、且当时蓝牙还正常的快照，可以先尝试回滚（比重装快得多，但 macOS 26 上成功率不保证，失败就跳到第 2 步）：

```sh
sudo tmutil restore ...   # 或用“迁移助理/恢复”图形界面选择该快照
```

没有可用快照，或者回滚后仍然崩 → 第 2 步。

### 2. Recovery 里重装 macOS（保留数据）

这是唯一能重建密封系统快照、把坏掉的 `bluetoothd` 换成好副本的可靠方法。

1. 重启，OpenCore 引导菜单里选 **Recovery**（不是 Installer 分区）。
2. 进“磁盘工具” → 选中 Macintosh HD → **急救/First Aid**。先让它把能修的文件系统结构修一遍。
3. 退出磁盘工具 → **重新安装 macOS**。
4. 目标盘选现有的 Macintosh HD（**不要抹掉**）。装完数据和 App 都在。
5. OpenCore 的 EFI 分区、`Kexts` 目录、`config.plist` 都不会被动。

> 注意：Tahoe 上这一步要联网下载（十几 GB）。慢是正常的，别在中途断电 —— 这块盘最怕的就是这个。

### 3. 装完先做的事

```sh
# 立刻把 DC6 panic 关掉，否则下一次 panic 又会制造新的坏块
sudo nvram boot-args="-v -noDC9 -nodisplaysleepDC6 keepsyms=1"
```

然后重启，确认两件事：

```sh
# 1) 不再有 APFS 报错（应该为空）
log show --last boot | grep -i "hash mismatch"

# 2) bluetoothd 只起一次，PID 不再每分钟变
pgrep -l bluetoothd
```

蓝牙回来 = 修好了。

---

## 五、怎么判断是不是盘快死了

重装之后：

- **几天内没有复发** → 大概率是那几次 panic 造成的局部损坏，重装即愈。继续用，但把 DC6 参数留着。
- **几天内又在新的文件/新的偏移上出现 `compressed data hash mismatch`** → 换盘。这颗 Phison E13T 无 DRAM 已经不只是一次性损坏，而是开始持续丢数据。$200 级别的有 DRAM 盘（铠侠 RC20 一类）就能解决，比继续和它耗时间划算。
- 判断方法：`log show --last boot | grep -i "compressed data hash mismatch"`，看 `(bluetoothd)` 以外的进程名有没有出现。

---

## 六、别做的事

- 别为了这个改 kext、换 `-ibtota`、降 SIP —— 因果链对不上。
- 别用 `fsck_apfs` 指望修好：APFS 只校验元数据树，不校验文件数据的 decmpfs 哈希，它根本检测不到这个问题。
- 别在同一块盘上反复 panic-重启：每一次都在增加坏块概率。
