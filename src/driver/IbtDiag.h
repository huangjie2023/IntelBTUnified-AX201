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
                             // bit6 nvram, bit7 bluetoothd board-id, bit8 address,
                             // bit9/bit10 保留历史位：1.9.19 的 transport wait 补丁，
                             // 1.9.23 起整段移除，这两位恒为 0（不再代表 needle 过期）
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
    // 1.9.17→1.9.18: wireSolve 是符号探测的存档（bit5~8 有、bit9 缺）。
    // kernel_map 映射路线已废弃：vm_map_enter_mem_object 对 8/8 组返回
    // KERN_INVALID_ADDRESS——新 XNU 拒绝把外部对象映射进 kernel_map。
    // 1.9.18 改用公开 KPI ubc_create_upl（UPL_FILE_IO|COPYOUT_FROM|NO_SYNC|
    // SET_LITE|SET_IO_WIRE，IOMemoryDescriptor::prepare 同款组合），无需符号。
    unsigned int wireSolve;

    // 1.9.18: UPL 钉页执行状态。判读：
    //   wireArmed=0                          → 只可能被 -ibtnowire 关掉
    //   wireOk=0 且 wireCreateFail 增长       → ubc_create_upl 失败，看 wireLastErr
    //   wireOk>0 且 wirePages≈4×wireOk        → 组已钉住（每 4K 页计 1）
    //   wireActive 稳定、不再增长             → 正常，teardown 前应等于 wireOk
    // 之后观察 A 类（SIGBUS + ktriage hash mismatch）是否绝迹。
    unsigned int wireArmed;
    unsigned int wireQueued;    // 累计入队的 16KB 组数（表满 >32 溢出）
    unsigned int wireOk;        // 成功创建并持有的 UPL 数
    unsigned int wirePages;     // 实际钉住的 4K 页数（page list present 统计）
    unsigned int wireCreateFail;// ubc_create_upl 失败次数
    unsigned int wireAbortFail; // stop 时 ubc_upl_abort 失败次数
    unsigned int wireActive;    // 当前持有中的 UPL 数
    unsigned int wireLastErr;   // 最后一次非 0 的 kern_return_t

    // 1.9.20: teardown 独立性判据。背景：本驱动是 USB 服务，睡眠时系统会
    // stop()、唤醒再 start()；1.9.18/19 把 UPL 回收放在 stop() 里，等于每次
    // 睡眠都拆掉 A 类保护（实测：第三次唤醒 SIGBUS 复发，WireActive 9→0）。
    // 1.9.20 起回收点移到 free()（仅 kext 卸载）。判读：
    //   睡眠若干次后 IBTStopCount 增长、IBTWireTeardown 仍为 0、wireActive
    //   保持 = 修复生效；Teardown>0 且 Active 掉到 0 = 又有卸载路径在拆。
    unsigned int wireTeardown;        // teardown 执行次数（正常 boot 应恒为 0）
    unsigned int wireTeardownAborted; // teardown 累计回收的 UPL 数
    unsigned int stopCount;           // 服务 stop() 次数（睡眠会 +1，正常）

    // 1.9.25: LE 改写状态机可观测性（本机日志通道死，全部走 ioreg）。
    // 判读漏斗（BLE 鼠标/键盘"连上但配对挂起到超时 708"专用）：
    //   IbtLeHookCalls=0   → 事件没流经我们的钩子（IOSource 三条路由失败，看 iosrcMask）
    //   IbtIosrcMask       → bit0 Completion / bit1 Rjj / bit2 Bundled 各自路由成功
    //   IbtLeConnReg>0     → Connection Complete 兜底注册生效（Tahoe 命令路径哑掉的备胎）
    //   IbtLeSlotsReg>0    → 命令路径（0x2016 双发）注册正常
    //   IbtLeRealPass>0 且 LeFakePhy=0 → stage1 没触发：第二个 features 事件没来
    //   IbtLeFakeDlc>0 但仍配不上     → 改写已发出，问题在 bluetoothd 消费侧/鼠标侧
    unsigned int leHookCalls;  // leProcessBuffer 进入次数（全部路径合计）
    unsigned int leRealPass;   // stage0：真实 features complete 放行
    unsigned int leFakePhy;    // stage1：fake PHY Update Complete 发出
    unsigned int leFakeDlc;    // stage2：fake LE Data Length Change 发出
    unsigned int leSlotsReg;   // 命令路径注册 slot 数
    unsigned int leConnReg;    // 事件路径（Connection Complete）兜底注册数
    unsigned int iosrcMask;    // 三条 IOSource::io 重载路由成功位图
    // 1.9.27: 深漏斗——每路径入口计数 + 解析计数。判读：
    //   HookAsync=0 且 HookRjj 巨大 → 异步路径没进（门槛/路由问题）
    //   Parsed=0 → readBytes 都没成功（描述符问题）
    //   Evt3E>0 而 CcSeen=0 → 看到 LE 元事件但没有 Connection Complete（事件被截断/分帧不同）
    //   CcSeen>0 而 ConnReg=0 → 有 CC 但 status!=0（连接失败事件）
    unsigned int leHookPipe;   // IOUSBHostPipe::io 包装入口
    unsigned int leHookAsync;  // IOSource Completion 重载包装入口
    unsigned int leHookRjj;    // IOSource Rjj 同步重载入口
    unsigned int leHookBundled;// IOSource Bundled 重载包装入口
    unsigned int leParsed;     // 成功读到 2 字节事件头
    unsigned int leEvt3E;      // LE 元事件（evt==0x3E）计数
    unsigned int leCcSeen;     // 元子事件 0x01 计数（含 status!=0）
    unsigned int leAsyncMatch; // 异步入口命中捕获 pipe 的次数（1.9.29 起恒 0 属预期，历史对照）
    int          leRemoteReads;// 0x2016 开窗计数当前值（>0 = 窗口开着）
    unsigned int leCmd2016;    // 命令钩子捕获到的 0x2016 命令数
    unsigned int leCmd2016Async; // 1.9.31: 异步 Completion 入口识别到的 0x2016（判定命令走同步还是异步）
    unsigned int leCmd2016Rjj;   // 1.9.31: 同步 Rjj 入口识别到的 0x2016（已含开窗+补发）
    unsigned int leCmdAll;     // deviceRequest 钩子入口总数（路由是否安装）
    unsigned int leCmdHci;     // 成功读到 opcode 的请求数（读路径是否工作）
    unsigned int leCmdDev;     // 通过 isIntelBluetooth 的请求数（过滤器是否失配）
    unsigned int lePipeCap;    // Intel 中断管道捕获成功次数（init 钩子是否触发）
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

// 1.9.17→1.9.18: 由 IntelBTUnified.cpp 实现。process() 在 workloop 上下文调用
// （cs_validate_page 持有 pager/对象锁，不能在钩子里直接建 UPL/fault 页，会死锁）；
// teardown 在 stop() 里调用，abort 全部 UPL 并释放 vnode 引用。
void ibtWireProcess();
void ibtWireTeardown();
// 1.9.24: 唤醒重钉请求。驱动层在 setPowerState(唤醒) 里调用，下一次
// ibtWireProcess()（diag timer，同时被收紧到 1s）会 abort 旧 UPL 并重建，
// 兜底"睡眠路径让 IO_WIRE 失效但引用还在"的场景。
void ibtWireRewireRequest();

// ---------------------------------------------------------------------------
// 1.9.21：传输死亡自动救砖（wedge 自愈）。
//
// 故障形态（2026-11-07 21:03 实测闭环）：AX201 蓝牙固件在控制器内部跑死
// （HCI 永久无响应，bluetoothd 看门狗 "Stack Mutex try lock failed" 自杀），
// Apple 栈拆台（DetachUserClients / terminating pseudo nub）并释放设备，
// 但设备没有被 terminate（StopCount=0）→ 蓝牙从系统里消失，只能重启。
// 且关机时系统对死控制器 teardown 干等约 2 分钟 → watchdogd 137s 无签到
// → 内核 panic（非正常关机）。
//
// 检测信号：正常态 Apple 的传输驱动独占持有 USB 设备（open 必失败）；
// 栈拆台后设备被释放，open 会成功。连续两拍（~30s）探测到"Apple 已释放"
// 即判定传输死亡，调用 IOUSBHostDevice::reset()（官方语义 = terminate 并
// 重枚举，等效拔插）→ 设备重枚举 → 本驱动重新 start → 固件重载 → 自愈。
//
// 关闭开关：boot-arg -ibt-norescue。
// 读数：IBTRescueArmed / IBTRescueHeld / IBTRescueCandidate / IBTRescueCount。
// ---------------------------------------------------------------------------
extern bool gIbtRescueDisabled; // true = -ibt-norescue，救砖关闭
