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
