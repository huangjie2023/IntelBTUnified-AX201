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
