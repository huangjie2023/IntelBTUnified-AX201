//
//  IbtDiag.h
//  IntelBTUnified
//
//  1.9.5：进程内诊断快照。
//
//  为什么需要这个：本机 macOS 26 上两条日志通道都拿不到数据——
//    1) IOLog()/lilu_os_log() 不落统一日志（实测 boot.log 里 99 万行，
//       "IntelFirmware"（驱动自己 XYLog 的前缀）出现 0 次，而蓝牙功能正常，
//       证明那段代码执行过、日志就是没进库）；
//    2) msgbuf/dmesg 只有 558 行且全是 socket 调试噪声，开机横幅早被冲掉。
//  所以 1.9.1~1.9.4 全部"日志为空"是通道问题，不是代码问题。
//
//  这个头文件给出一个纯内存快照，由驱动层发布到 IORegistry，用
//      ioreg -r -c IntelBluetoothFirmware -l -w0 | grep IBT
//  就能读到，不需要 root、不写盘、不依赖任何日志子系统。
//
#pragma once

struct IbtDiagSnapshot {
    unsigned int csHookArmed;   // 1 = _cs_validate_page 路由成功
    unsigned int vnopArmed;     // 1 = _VNOP_VERIFY 路由成功（吞错修复生效的前提）
    unsigned int mode;          // 0 = process-scoped, 1 = legacy, 2 = OFF (-ibtota)
    unsigned int novb;          // 1 = 吞错被 -ibtnovb 关掉
    unsigned int swallowed;     // 累计吞掉的 EBADMSG 次数
    unsigned int pathFailures;  // vn_getpath 失败次数（无法定域，未吞）
    unsigned int otherErrors;   // bluetoothd/BlueTool 上非 EBADMSG 的错误次数
    unsigned int callsOnBt;     // 1.9.6: _VNOP_VERIFY 被 bluetoothd/BlueTool vnode 调用过的次数（不看返回值）
    unsigned int btVnodes;      // 1.9.6: 已记录的 bluetoothd/BlueTool vnode 数（>0 说明 cs_validate 确实见过它们）
    unsigned int pmEvents;      // 睡眠/唤醒回调次数，用来判断快照新鲜度
    // 1.9.7 探针：候选入口是否装上了（按位）、各自总调用数、命中 bt vnode 的次数
    unsigned int symMask;       // bit0 _VNOP_VERIFY, bit1 _vnode_verify, bit2 _decmpfs_pagein_compressed, bit3 _decmpfs_validate_compressed_file
                                // 1.9.8: bit4 _apfs_vnop_verify, bit5 _apfs_pagein_with_verification, bit6 _apfs_verify_uncompressed_data, bit7 _apfs_verify_rangelist
    unsigned int vnopAll;       // _VNOP_VERIFY 总调用
    unsigned int kvAll;         // _vnode_verify 总调用
    unsigned int kvOnBt;        // _vnode_verify 命中 bt vnode
    unsigned int decAll;        // _decmpfs_pagein_compressed 总调用
    unsigned int decOnBt;       // 同上，命中 bt vnode
    unsigned int valAll;        // _decmpfs_validate_compressed_file 总调用
    unsigned int valOnBt;       // 同上，命中 bt vnode
    // 1.9.8 APFS 侧：第一个是带吞错的真正钩点，后三个只计数
    unsigned int apfsVerAll;    // _apfs_vnop_verify 总调用（表项实现）
    unsigned int apfsVerOnBt;   // 同上，命中 bt vnode —— >0 说明这就是入口
    unsigned int apfsPiAll;     // _apfs_pagein_with_verification 总调用
    unsigned int apfsPiOnBt;
    unsigned int apfsUncAll;    // _apfs_verify_uncompressed_data 总调用
    unsigned int apfsUncOnBt;
    unsigned int apfsRlAll;     // _apfs_verify_rangelist 总调用
    unsigned int apfsRlOnBt;
    unsigned int apfsSolve;     // 1.9.9: 四个 APFS 符号各自是否解析成功（0..3 位），区分"kext 没找到/符号没找到"
    unsigned int patchedPages;  // 本 boot 记录到的补丁页数（诊断附注，不作判定）
    unsigned long long lastOffset;  // 最后一次被吞的校验起点
    unsigned long long lastSize;    // 最后一次被吞的校验长度
    unsigned long long lastPage;    // 该区间内命中的补丁页（0 = 没记录）

    // 1.9.11：cs_validate_page 入口分级计数。
    //
    // 1.9.10 的读数是 Armed=Yes / BtVnodes=0 / PatchedPages=0，但这一组数字
    // 分不清三种截然不同的情况，进一步的分析全部无从下手：
    //   a) 钩子根本没被调用（csAll=0）      → 这条路在 macOS 26 上就是死的
    //   b) 被调用了但 vn_getpath 拿不到路径 → pagein 上下文不能解析路径
    //   c) 路径拿到了但不是 /usr/sbin 下的 → 校验入口换了（例如走 SSV/cryptex）
    // 下面四个数是严格递进的漏斗，一次开机就能定位断点在哪一级。
    unsigned int csAll;      // 进入钩子的总次数（任何判断之前）
    unsigned int csPathFail; // vn_getpath 返回非 0
    unsigned int csSbin;     // 路径解析成功但不在 /usr/sbin/ 下（未计入 /usr/sbin 内的）
    unsigned int csMatch;    // 命中 bluetoothd 或 BlueTool 的次数（≥ btVnodes，后者去重）

    // 1.9.13：_decmpfs_validate_compressed_file 对 bluetoothd/BlueTool 的返回值分布。
    //
    // 1.9.12 定时刷新后的实测读数：BtVnodes=2 / PatchedPages=4 / ValOnBt=64 /
    // VnopAll=0。补丁确实在打，而整条占位最久的 _VNOP_VERIFY 全系统 0 次调用
    // （死符号），真正处理压缩页校验的是 _decmpfs_validate_compressed_file。
    // 这三个数用来确认它就是崩溃出口：如果 decBad 随崩溃增长，吞错就落在这里；
    // 如果 decBad 恒 0，说明错误发生在更下层（APFS 内部），还得继续往下挖。
    unsigned int decOk;      // 返回 0（校验通过）
    unsigned int decBad;     // 返回 EBADMSG（压缩块哈希不匹配）
    unsigned int decOther;   // 返回其它非 0

    // 1.9.14：按补丁位图，以及 board-id patch 的前置条件。
    //
    // 起因：bluetoothd 用 board-id 查表决定 transport——
    //   hostControllerTransportForBoardID(bid) →
    //     命中 UART 列表 → 等 /dev/cu.BLTH；命中 USB 列表 → 用 USB transport；
    //     都不命中 → 默认 UART。
    // MacBookPro16,2 的真机蓝牙是 T2 管理的 Broadcom，board-id 落在 UART 列表，
    // 于是 bluetoothd 每轮唤醒都去等那个串口，等不到 15 秒就 os_crash()。
    // 本仓库 1.7.0 起就有对应的替换补丁（把 boardIdsWithUSBBluetooth[0] 换成
    // 本机 board-id），但 26.7.3 上日志里始终是 "FindTransportType -- UART
    // Transport"，说明补丁大概率没命中，而此前没有任何读数能证实这一点：
    // PatchedPages 只按页去重，同一页里 7 个补丁只算 1 页，无法反推。
    //
    // 这三个字段是唯一能定性的判据：
    //   bidArmed = 0                → 前置条件没过，补丁根本没尝试
    //   bidArmed = 1 且 patchMask 无 bit1/bit7 → 尝试了但字符串没匹配上
    //   bidArmed = 1 且 patchMask 有 bit1/bit7 → 补丁打上了，问题在别处
    unsigned int patchMask;  // appliedPatches 原样：bit0 skip-update, bit1 BlueTool board-id,
                             // bit2 vendor, bit3 chipset, bit4 chipset13, bit5 chipset15,
                             // bit6 nvram, bit7 bluetoothd board-id, bit8 address
    unsigned int bidArmed;   // 1 = shouldPatchBoardId，board-id 补丁本次是否启用
    unsigned int bidMiss;    // board-id 补丁执行了但没找到 needle 的次数（>0 且 patchMask 无 bit1/7 = needle 过期）
    char boardId[24];        // 实际 boardIdentifier（null 结尾），用来核对长度判定是否成立

    // 1.9.16：wire 路线可行性探测。
    //
    // A 类崩溃（APFS compressed data hash mismatch → SIGBUS）的机制已实测闭环：
    // 补丁页驻留 + 同 16KB 组相邻页被逐出后重读 → 组校验把「内存里改过的页」和
    // 「盘上原始页」混在一起算哈希 → 必然不匹配。唯一正解是把被补丁的 16KB 组
    // 整组 wire 住（acidanthera/bugtracker#2228 复现者给出的实验方向）。
    // ubc_range_wire 不在公开 SDK 头文件里，但它在不在内核符号表里只能实测：
    // _VNOP_VERIFY 同样不是公开 KPI，1.9.7 照样从 KernelID 解析到了。
    // 这 5 位就是逐个试出来的结果：
    //   bit0 _ubc_range_wire      有 → wire 路线可写（1.9.16 实测 26.x 无此符号）
    //   bit1 _ubc_range_unwire    有 → 才能配对释放（同上）
    //   bit2 _vm_page_wire        备选：逐页 wire（1.9.16 实测有）
    //   bit3 _vm_page_unwire      （1.9.16 实测有）
    //   bit4 _ubc_msync           对照组，公开 KPI，理应恒为 1；若它是 0 说明探测本身有问题
    // 1.9.17: bit0/1 实测不存在，改走 kernel_map 映射 wire，追加 5 位：
    //   bit5 _kernel_map（全局变量）bit6 _vm_map_enter_mem_object bit7 _vm_map_wire
    //   bit8 _vm_map_unwire        bit9 _vm_map_remove
    unsigned int wireSolve;

    // 1.9.17: kernel_map 映射 wire 的执行状态。判读：
    //   wireArmed=0 且 wireSolve 缺 bit5~7 之一 → 符号缺失，本路线不可用（行为同 1.9.16）
    //   wireArmed=1 且 wireEnterFail 增长       → pager port 不能这样映射，路线失败
    //   wireOk 增长且 wireActive 稳定          → 组已钉住，观察 A 类（SIGBUS + ktriage
    //                                             hash mismatch）是否绝迹
    // wireActive 应恒等于 wireOk - teardown 次数；stop 后归 0。
    unsigned int wireArmed;
    unsigned int wireQueued;    // 累计入队的 16KB 组数（含表满溢出，>32 时多的进不来）
    unsigned int wireOk;        // 成功 wire 的组数
    unsigned int wireEnterFail; // vm_map_enter_mem_object 失败次数
    unsigned int wireWireFail;  // vm_map_wire 失败次数
    unsigned int wireActive;    // 当前保持 wire 的组数
    unsigned int wireLastErr;   // 最后一次非 0 的 kern_return_t
};

// ---------------------------------------------------------------------------
// 1.9.10：唤醒竞态计时。
//
// 已确认的故障链（2026-11-06 18:42 实例，全部来自统一日志实测）：
//   [唤醒 +0.00s] USB 端口 power-gate 后重新枚举
//   [+0.41s]      IntelBluetoothFirmware selected configuration 1  ← 本驱动接住设备
//   [+0.28s]      bluetoothd 启动，传输层切 UART/H4BC，开始等 /dev/cu.BLTH
//   [+16.15s]     bluetoothd 等满 15 秒放弃 → Transport layer initialization
//                 failed → os_crash()（ExcUserFault, reason=1201）
//   [+27.0s]      内核才 CreateSerialDevice —— 已经晚了 11 秒
//   [+27.3s]      launchd 拉起的第二个 bluetoothd 一次成功
//
// 即：这 27 秒里只有本驱动在跑（Apple 的 HCI 传输层装不上，因为设备被
// 我们 open 着），所以 16s/27s 的差值必须在本驱动内部解释。下面四个槽位
// 就是把这 27 秒拆开：固件下载本身慢，还是走了"失败→回 bootloader→
// IOSleep(1000)→整包重下"的重试路径。
// ---------------------------------------------------------------------------
enum {
    kIbtT_SetupMs  = 0,  // start() 进入 → publishReg() 完成，毫秒
    kIbtT_FwMs     = 1,  // downloadFirmwareData() 全流程耗时（含重试），毫秒
    kIbtT_FwRetry  = 2,  // 1 = 走过 resetToBootloader + IOSleep(1000) 重试
    kIbtT_FwSkip   = 3,  // 1 = 固件已是最新，返回 -EALREADY 跳过下载
    kIbtT_SetupCnt = 4,  // 本 boot 内 start() 被调用次数
    kIbtT_Slots    = 5
};

unsigned int ibtNowMs();                                  // 单调毫秒
void ibtTimingSet(unsigned int slot, unsigned int value);
void ibtTimingAdd(unsigned int slot, unsigned int delta);

// 由 IntelBTUnified.cpp 实现：读当前全局状态填快照
void ibtDiagFill(IbtDiagSnapshot *out);

// 由驱动层实现：把快照写进已注册的 service 属性。任何上下文都可调用，
// 未注册前调用是空操作。
void ibtDiagPublishLive();

// 1.9.17: 由 IntelBTUnified.cpp 实现。process() 在 workloop 上下文调用
// （cs_validate_page 持有 pager/对象锁，不能在钩子里直接 vm_map_wire，会死锁）；
// teardown 在 stop() 里调用，unwire 并移除全部 kernel_map 映射。
void ibtWireProcess();
void ibtWireTeardown();
