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
    unsigned int patchedPages;  // 本 boot 记录到的补丁页数（诊断附注，不作判定）
    unsigned long long lastOffset;  // 最后一次被吞的校验起点
    unsigned long long lastSize;    // 最后一次被吞的校验长度
    unsigned long long lastPage;    // 该区间内命中的补丁页（0 = 没记录）
};

// 由 IntelBTUnified.cpp 实现：读当前全局状态填快照
void ibtDiagFill(IbtDiagSnapshot *out);

// 由驱动层实现：把快照写进已注册的 service 属性。任何上下文都可调用，
// 未注册前调用是空操作。
void ibtDiagPublishLive();
