//
//  IntelBluetoothFirmware.cpp
//  IntelBluetoothFirmware
//
//  Created by zxystd on 2019/11/17.
//  Copyright © 2019 zxystd. All rights reserved.
//

#include "BtIntel.h"

#include "IntelBluetoothFirmware.hpp"
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>
#include <libkern/version.h>
#include <libkern/OSTypes.h>
#include <kern/clock.h>
#include <IOKit/usb/StandardUSB.h>
#include "Hci.h"
#include "linux.h"
#include "Log.h"
#include "Devices.h"
#include "IbtDiag.h"

#include "IntelBluetoothOpsGen2.hpp"

#define super IOService
OSDefineMetaClassAndStructors(IntelBluetoothFirmware, IOService)

// 1.9.5: 诊断出口。本机 macOS 26 上 IOLog 不落统一日志、msgbuf 又被 socket 噪声
// 刷掉，日志一条都读不到，前三轮迭代全靠猜。这里把状态挂到 service 属性上，
// 用 ioreg 读，不需要 root、不写盘、不依赖日志子系统。
static IOService *gDiagSvc = nullptr;
static unsigned int gPmEvents = 0;
// 1.9.20: stop() 次数。USB 驱动在睡眠时会被系统 stop（唤醒再 start），
// 这个数 + IBTWireTeardown 一起判定 teardown 是否还跟着睡眠走。
static unsigned int gServiceStopCount = 0;
// 1.9.10：唤醒竞态计时槽位，见 IbtDiag.h
static unsigned int gIbtTiming[kIbtT_Slots] = {};

unsigned int ibtNowMs()
{
    AbsoluteTime now = 0;
    clock_get_uptime(&now);
    // macOS 26 的 MacKernelSDK 把这个函数改成了两参数（结果走出参）
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(now, &ns);
    return (unsigned int)(ns / 1000000ULL);
}

void ibtTimingSet(unsigned int slot, unsigned int value)
{
    if (slot < kIbtT_Slots)
        gIbtTiming[slot] = value;
}

void ibtTimingAdd(unsigned int slot, unsigned int delta)
{
    if (slot < kIbtT_Slots)
        gIbtTiming[slot] += delta;
}

// 1.9.12：诊断快照刷新周期。bluetoothd 的补丁页在 start() 之后才出现，
// 30 秒足够覆盖一次冷启动后的进程加载，又不会给 workloop 造成可见负担。
// 1.9.17：缩短到 15 秒——这个定时器同时负责处理钉页队列，缩短间隔能把
// vnode 的有效窗口压得更紧。
static constexpr unsigned int kDiagRefreshMs = 15000;

void ibtDiagPublishLive()
{
    IOService *svc = gDiagSvc;
    if (!svc)
        return;

    IbtDiagSnapshot s {};
    ibtDiagFill(&s);
    s.pmEvents = gPmEvents;
    s.stopCount = gServiceStopCount;

    // 前三个是“修复到底生效没有”的判据：csHookArmed 决定补丁页写得进去，
    // vnopArmed 决定 EBADMSG 吞不吞得掉。任何一个为 0，1.9.x 的修复都是空转。
    svc->setProperty("IBTArmed", (bool)(s.csHookArmed != 0));
    svc->setProperty("IBTVnopArmed", (bool)(s.vnopArmed != 0));
    svc->setProperty("IBTMode", (unsigned long long)s.mode, 32);
    svc->setProperty("IBTNovb", (bool)(s.novb != 0));
    svc->setProperty("IBTSwallowed", (unsigned long long)s.swallowed, 32);
    svc->setProperty("IBTPathFail", (unsigned long long)s.pathFailures, 32);
    svc->setProperty("IBTOtherErr", (unsigned long long)s.otherErrors, 32);
    svc->setProperty("IBTVnopCalls", (unsigned long long)s.callsOnBt, 32);
    svc->setProperty("IBTBtVnodes", (unsigned long long)s.btVnodes, 32);
    // 1.9.7 探针读数
    svc->setProperty("IBTSymMask", (unsigned long long)s.symMask, 32);
    svc->setProperty("IBTVnopAll", (unsigned long long)s.vnopAll, 32);
    svc->setProperty("IBTKvAll", (unsigned long long)s.kvAll, 32);
    svc->setProperty("IBTKvOnBt", (unsigned long long)s.kvOnBt, 32);
    svc->setProperty("IBTDecAll", (unsigned long long)s.decAll, 32);
    svc->setProperty("IBTDecOnBt", (unsigned long long)s.decOnBt, 32);
    svc->setProperty("IBTValAll", (unsigned long long)s.valAll, 32);
    svc->setProperty("IBTValOnBt", (unsigned long long)s.valOnBt, 32);
    svc->setProperty("IBTDecOk", (unsigned long long)s.decOk, 32);
    svc->setProperty("IBTDecBad", (unsigned long long)s.decBad, 32);
    svc->setProperty("IBTDecOther", (unsigned long long)s.decOther, 32);
    svc->setProperty("IBTApfsVerAll", (unsigned long long)s.apfsVerAll, 32);
    svc->setProperty("IBTApfsVerOnBt", (unsigned long long)s.apfsVerOnBt, 32);
    svc->setProperty("IBTApfsPiAll", (unsigned long long)s.apfsPiAll, 32);
    svc->setProperty("IBTApfsPiOnBt", (unsigned long long)s.apfsPiOnBt, 32);
    svc->setProperty("IBTApfsUncAll", (unsigned long long)s.apfsUncAll, 32);
    svc->setProperty("IBTApfsUncOnBt", (unsigned long long)s.apfsUncOnBt, 32);
    svc->setProperty("IBTApfsRlAll", (unsigned long long)s.apfsRlAll, 32);
    svc->setProperty("IBTApfsRlOnBt", (unsigned long long)s.apfsRlOnBt, 32);
    svc->setProperty("IBTApfsSolve", (unsigned long long)s.apfsSolve, 32);
    svc->setProperty("IBTPmEvents", (unsigned long long)s.pmEvents, 32);
    svc->setProperty("IBTPatchedPages", (unsigned long long)s.patchedPages, 32);
    svc->setProperty("IBTLastOffset", s.lastOffset, 64);
    svc->setProperty("IBTLastSize", s.lastSize, 32);
    svc->setProperty("IBTLastPage", s.lastPage, 64);
    // 1.9.14：补丁位图 + board-id 前置条件。判读见 IbtDiag.h。
    svc->setProperty("IBTPatchMask", (unsigned long long)s.patchMask, 32);
    svc->setProperty("IBTBidArmed", (bool)(s.bidArmed != 0));
    svc->setProperty("IBTBidMiss", (unsigned long long)s.bidMiss, 32);
    svc->setProperty("IBTBoardId", (const char *)s.boardId);
    svc->setProperty("IBTWireSolve", (unsigned long long)s.wireSolve, 32);
    // 1.9.18: UPL 钉页执行状态。判读见 IbtDiag.h。
    svc->setProperty("IBTWireArmed", (bool)(s.wireArmed != 0));
    svc->setProperty("IBTWireQueued", (unsigned long long)s.wireQueued, 32);
    svc->setProperty("IBTWireOk", (unsigned long long)s.wireOk, 32);
    svc->setProperty("IBTWirePages", (unsigned long long)s.wirePages, 32);
    svc->setProperty("IBTWireCreateFail", (unsigned long long)s.wireCreateFail, 32);
    svc->setProperty("IBTWireAbortFail", (unsigned long long)s.wireAbortFail, 32);
    svc->setProperty("IBTWireActive", (unsigned long long)s.wireActive, 32);
    svc->setProperty("IBTWireLastErr", (unsigned long long)s.wireLastErr, 32);
    // 1.9.20: stop/teardown 计数——实证「睡眠时 USB 服务被 stop → teardown 拆掉
    // A 类保护」。判读：睡眠后 StopCount 增长而 TeardownCount 不动 = 修复生效。
    svc->setProperty("IBTStopCount", (unsigned long long)s.stopCount, 32);
    svc->setProperty("IBTWireTeardown", (unsigned long long)s.wireTeardown, 32);
    svc->setProperty("IBTWireTeardownAborted", (unsigned long long)s.wireTeardownAborted, 32);
    // 1.9.25: LE 改写状态机可观测性（修 BLE 鼠标配对）
    svc->setProperty("IBTLeHookCalls", (unsigned long long)s.leHookCalls, 32);
    svc->setProperty("IBTLeRealPass", (unsigned long long)s.leRealPass, 32);
    svc->setProperty("IBTLeFakePhy", (unsigned long long)s.leFakePhy, 32);
    svc->setProperty("IBTLeFakeDlc", (unsigned long long)s.leFakeDlc, 32);
    svc->setProperty("IBTLeSlotsReg", (unsigned long long)s.leSlotsReg, 32);
    svc->setProperty("IBTLeConnReg", (unsigned long long)s.leConnReg, 32);
    svc->setProperty("IBTIosrcMask", (unsigned long long)s.iosrcMask, 32);
    // 1.9.27: 深漏斗（判读见 IbtDiag.h）
    svc->setProperty("IBTLeHookPipe", (unsigned long long)s.leHookPipe, 32);
    svc->setProperty("IBTLeHookAsync", (unsigned long long)s.leHookAsync, 32);
    svc->setProperty("IBTLeHookRjj", (unsigned long long)s.leHookRjj, 32);
    svc->setProperty("IBTLeHookBundled", (unsigned long long)s.leHookBundled, 32);
    svc->setProperty("IBTLeParsed", (unsigned long long)s.leParsed, 32);
    svc->setProperty("IBTLeEvt3E", (unsigned long long)s.leEvt3E, 32);
    svc->setProperty("IBTLeCcSeen", (unsigned long long)s.leCcSeen, 32);
    svc->setProperty("IBTLeAsyncMatch", (unsigned long long)s.leAsyncMatch, 32);
    svc->setProperty("IBTLeRemoteReads", (unsigned long long)(int)s.leRemoteReads, 32);
    svc->setProperty("IBTLeCmd2016", (unsigned long long)s.leCmd2016, 32);
    svc->setProperty("IBTCmdAll", (unsigned long long)s.leCmdAll, 32);
    svc->setProperty("IBTLeCmd2016Async", (unsigned long long)s.leCmd2016Async, 32);
    svc->setProperty("IBTLeCmd2016Rjj", (unsigned long long)s.leCmd2016Rjj, 32);
    svc->setProperty("IBTCmdHci", (unsigned long long)s.leCmdHci, 32);
    svc->setProperty("IBTCmdDev", (unsigned long long)s.leCmdDev, 32);
    svc->setProperty("IBTPipeCap", (unsigned long long)s.lePipeCap, 32);
    // 1.9.11：cs_validate_page 入口漏斗。判读见 IbtDiag.h。
    svc->setProperty("IBTCsAll", (unsigned long long)s.csAll, 32);
    svc->setProperty("IBTCsPathFail", (unsigned long long)s.csPathFail, 32);
    svc->setProperty("IBTCsSbin", (unsigned long long)s.csSbin, 32);
    svc->setProperty("IBTCsMatch", (unsigned long long)s.csMatch, 32);
    // 1.9.10：唤醒竞态计时。判读方式：
    //   IBTSetupMs ≈ 27000 且 IBTFwMs ≈ 26000 → 27 秒全在固件下载
    //   IBTFwRetry = 1                        → 走的是失败重试路径，重点查第一次为什么失败
    //   IBTFwMs 很小而 IBTSetupMs 很大        → 不在下载，在 readVersion / 管道建立
    svc->setProperty("IBTSetupMs", (unsigned long long)gIbtTiming[kIbtT_SetupMs], 32);
    svc->setProperty("IBTFwMs", (unsigned long long)gIbtTiming[kIbtT_FwMs], 32);
    svc->setProperty("IBTFwRetry", (unsigned long long)gIbtTiming[kIbtT_FwRetry], 32);
    svc->setProperty("IBTFwSkip", (unsigned long long)gIbtTiming[kIbtT_FwSkip], 32);
    svc->setProperty("IBTStartCnt", (unsigned long long)gIbtTiming[kIbtT_SetupCnt], 32);
}

/* Si el firmware no encaja con el chip, IOKit reintenta el probe sin parar
 * (log infinito en pantalla y arranque eterno). Tras 3 fallos seguidos se
 * deja de intentar: se pierde el Bluetooth, pero el sistema arranca.
 */
static int gStartFailures = 0;
static const int kMaxStartFailures = 3;

//com.apple.iokit.IOBluetoothHostControllerUSBTransport

enum { kMyOffPowerState = 0, kMyOnPowerState = 1 };

#define kIOPMPowerOff 0

static IOPMPowerState myTwoStates[2] =
{
    {1, kIOPMPowerOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

bool IntelBluetoothFirmware::init(OSDictionary *dictionary)
{
    XYLog("Driver init()\n");
    return super::init(dictionary);
}

void IntelBluetoothFirmware::free() {
    XYLog("Driver free()\n");
    // 1.9.20: UPL/vnode 引用的回收点从 stop() 挪到这里——free() 只在实例真正
    // 销毁（kext 卸载）时调用，睡眠触发的 USB 服务 stop/start 不再拆 A 类保护。
    // 见 stop() 里的分析。
    ibtWireTeardown();
    // stop() 正常路径已经摘掉；这里只是兜底，防止 start() 失败时残留的定时器
    // 在实例释放后仍被 workloop 回访。
    if (m_pDiagTimer) {
        m_pDiagTimer->cancelTimeout();
        OSSafeReleaseNULL(m_pDiagTimer);
    }
    super::free();
}

// Camino de error comun de start(): IOKit solo llama a stop() si start() tuvo exito,
// asi que aqui hay que deshacer PMinit y cerrar el dispositivo nosotros.
bool IntelBluetoothFirmware::fail()
{
    cleanUp();
    if (m_pmStarted) {
        PMstop();
        m_pmStarted = false;
    }
    return false;
}

bool IntelBluetoothFirmware::start(IOService *provider)
{
    XYLog("Driver Start()\n");
    if (gStartFailures >= kMaxStartFailures) {
        XYLog("Setup failed %d times in a row, giving up (firmware/chip mismatch)\n",
              gStartFailures);
        return false;
    }
    char fwName[64];
    // 1.9.10：从进 start() 到 publishReg() 的墙钟时长。设备在我们 open 期间
    // Apple 的 IOBluetoothHostControllerUSBTransport 装不上，所以这段时间
    // 直接等于"内核迟迟不建 /dev/cu.BLTH"的时长。
    const unsigned int tStart = ibtNowMs();
    ibtTimingAdd(kIbtT_SetupCnt, 1);
    m_pDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (m_pDevice == NULL) {
        XYLog("Driver Start fail, not usb device\n");
        return false;
    }
    PMinit();
    m_pmStarted = true;
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);
    makeUsable();
    
    if (!super::start(provider))
        return fail();
    
    if (!m_pDevice->open(this)) {
        XYLog("start fail, can not open device\n");
        return fail();
    }
    // Esta build solo soporta AX201 (0x0026), que es gen2. Se instancia Gen2
    // de forma fija: las rutas Gen1/Gen3 no se compilan, no hay nada que elegir.
    XYLog("Selected ops: Gen2 (AX201 only)\n");
    m_pBTIntel = new IntelBluetoothOpsGen2();
    if (!m_pBTIntel || !m_pBTIntel->initWithDevice(this, m_pDevice)) {
        XYLog("start fail, can not init device\n");
        return fail();
    }
    XYLog("BT init succeed\n");
    if (!m_pBTIntel->setup()) {
        gStartFailures++;
        m_pDevice->setProperty("FirmwareLoaded", false);
        return fail();
    }
    gStartFailures = 0;
    m_pBTIntel->getFirmwareName(fwName, sizeof(fwName));
    publishReg(true, fwName);
    ibtTimingSet(kIbtT_SetupMs, ibtNowMs() - tStart);
    // 1.9.5: 从这里开始 ioreg 才读得到诊断状态。processPatcher 走的是 Lilu 的
    // onPatcherLoad 回调，早于本函数的 start()，所以此刻 armed 标志已是最终值。
    gDiagSvc = this;
    ibtDiagPublishLive();
    // 1.9.12：一次性快照不够用。bluetoothd 的页补丁发生在 start() 之后几十秒，
    // 只靠 start/pm/stop 发布时，ioreg 里那组计数永远是“进程还没起来”的 0——
    // 1.9.6~1.9.11 的读数全被这一点污染过。这里挂一个 30 秒的轮询，
    // 只做 setProperty，不触碰任何 IO 路径。
    if (getWorkLoop()) {
        m_pDiagTimer = IOTimerEventSource::timerEventSource(this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this, &IntelBluetoothFirmware::diagTimerFired));
        if (m_pDiagTimer) {
            if (getWorkLoop()->addEventSource(m_pDiagTimer) == kIOReturnSuccess) {
                m_pDiagTimer->setTimeoutMS(kDiagRefreshMs);
            } else {
                OSSafeReleaseNULL(m_pDiagTimer);
            }
        }
    }
    if (!m_pDiagTimer)
        XYLog("diag timer unavailable, snapshot stays at start-time values\n");
    cleanUp();
    // 1.9.21：救砖巡检需要长期持有 provider 指针。cleanUp 只是清掉 open 状态；
    // provider 在我们 stop() 之前必然有效（IOService 客户端关系保证），定时器
    // 在 stop() 里被摘除，指针不会在设备销毁后被回访。
    m_pDevice = OSDynamicCast(IOUSBHostDevice, provider);
    m_rescueEnabled = !gIbtRescueDisabled;
    // USB stop/start 循环（睡眠）后状态重置：唤醒后 Apple 重新 open 之前
    // 存在无人持有的空窗，闩锁必须重新学习。
    m_appleHeld = false;
    m_rescueCandidate = 0;
    return true;
}

// 1.9.12：定时把内核侧的计数刷进 ioreg。跑在 workloop 线程，不涉及 page fault，
// 也不改任何状态，只是把已有的原子变量读出来写进属性表。
void IntelBluetoothFirmware::diagTimerFired()
{
    // 1.9.17→1.9.18: 钉页队列必须在 workloop 上下文处理（cs_validate_page 钩子
    // 持有 pager/对象锁，在钩子里建 UPL/fault 页会死锁）。处理完再刷新 ioreg
    // 快照，这样 WireOk/WireActive 与属性表始终同步。
    ibtWireProcess();
    // 1.9.21：救砖巡检与诊断同节奏（15s 一拍）。探测本身只是 open/close 的
    // 独占性试探，不发起任何 USB 传输，对死控制器也安全。
    if (m_rescueEnabled)
        diagRescueTick();
    ibtDiagPublishLive();
    setProperty("IBTRescueArmed", (bool)m_rescueEnabled);
    setProperty("IBTRescueHeld", (bool)m_appleHeld);
    setProperty("IBTRescueCandidate", (unsigned long long)m_rescueCandidate, 32);
    setProperty("IBTRescueCount", (unsigned long long)m_rescueCount, 32);
    if (m_pDiagTimer)
        m_pDiagTimer->setTimeoutMS(kDiagRefreshMs);
}

// 1.9.21：探测设备当前是否无人持有。正常态 Apple 的传输驱动独占持有设备，
// open() 返回 false；栈拆台后设备被释放，open() 成功——探测后立刻 close 归还，
// 全程零 USB 传输。
bool IntelBluetoothFirmware::probeAppleGone()
{
    if (!m_pDevice)
        return false;
    if (!m_pDevice->open(this))
        return false;   // 有人持有（正常态 = Apple 传输驱动）
    m_pDevice->close(this);
    return true;        // 无人持有 = Apple 已释放
}

// 1.9.21：救砖状态机，两拍确认后触发 IOUSBHostDevice::reset()（官方语义 =
// terminate 并重枚举，等效拔插；不可在 USB 端口 workloop 调用——本函数跑在
// 我们自己的 workloop 上，满足要求）。reset 后设备重枚举、本驱动重新 start
// 并重载固件，bluetoothd 重新拿到传输层，全程无需重启。
void IntelBluetoothFirmware::diagRescueTick()
{
    const bool appleGone = probeAppleGone();
    if (!appleGone) {
        if (m_rescueCandidate)
            XYLog("rescue: Apple stack back, cancel candidate (%u)\n", m_rescueCandidate);
        m_appleHeld = true;
        m_rescueCandidate = 0;
        return;
    }
    if (!m_appleHeld) {
        // start() 后 Apple 尚未接入的空窗（或上一轮救砖后的重枚举窗口），
        // 闩锁未建立，不计入候选。
        return;
    }
    ++m_rescueCandidate;
    XYLog("rescue: Apple stack released device, candidate %u/2\n", m_rescueCandidate);
    if (m_rescueCandidate < 2)
        return; // 两拍（约 30s）确认，滤掉 Apple 自身短时的 close/reopen 竞态
    m_rescueCandidate = 0;
    m_appleHeld = false;
    XYLog("rescue: transport dead, resetting USB device (attempt #%u)\n", ++m_rescueCount);
    if (!m_pDevice->open(this)) {
        // 竞态：确认期间 Apple 又把设备抢回去了，放弃本轮、重新闩锁。
        XYLog("rescue: device re-held by Apple, abort reset\n");
        m_appleHeld = true;
        return;
    }
    IOReturn rc = m_pDevice->reset();
    XYLog("rescue: reset()=0x%x, device reenumerating, firmware will reload\n", rc);
    m_pDevice->close(this);
    // reset 触发 terminate → 我们的 stop()（定时器摘除）→ 设备重枚举 →
    // start() 重新武装。若 port reset 救不回来，下一轮枚举失败，维持原状。
}

void IntelBluetoothFirmware::publishReg(bool isSucceed, const char *fwName)
{
    m_pDevice->setProperty("FirmwareLoaded", isSucceed);
    // Tras un reinicio sin cortar la alimentacion el chip conserva el firmware y no se descarga
    // nada: el nombre queda vacio.
    if (isSucceed)
        setProperty("fw_name", fwName[0] ? fwName : "(already running)");
    // Monterey+
    if (version_major >= 21)
        m_pDevice->setName("Bluetooth USB Host Controller");
}

void IntelBluetoothFirmware::cleanUp()
{
    XYLog("Clean up...\n");
    OSSafeReleaseNULL(m_pBTIntel);
    if (m_pDevice) {
        if (m_pDevice->isOpen(this)) {
            m_pDevice->close(this);
        }
        m_pDevice = NULL;
    }
}

IOReturn IntelBluetoothFirmware::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    // 1.9.5: 每次睡眠/唤醒都刷新一次快照，休眠前的那一份正好覆盖上一轮唤醒
    // 期间累计的 swallowed 计数（唤醒瞬间的快照早于 bluetoothd 缺页）。
    ++gPmEvents;
    // 1.9.24: 唤醒时请求重钉补丁页组（powerStateOrdinal 0 = 睡眠，1 = 唤醒）。
    // 实测 2026-11-08：唤醒后 2s BlueTool SIGBUS（APFS 组校验混合失败），睡眠路径
    // 上 UPL 的 IO_WIRE 可能失效。这里只置请求标志（PM 上下文不能建 UPL/fault 页），
    // 真正的重建在下一次 diag timer 的 workloop 上下文里做。同时把定时器收紧到
    // 1s，把"唤醒 → 重钉完成"的无保护窗口从最长 15s 压到 ~1s。
    if (powerStateOrdinal != 0) {
        ibtWireRewireRequest();
        if (m_pDiagTimer)
            m_pDiagTimer->setTimeoutMS(1000);
    }
    ibtDiagPublishLive();
    return IOPMAckImplied;
}

// 1.9.22：关机/重启时立即解除救砖武装。背景：关机 teardown 流程里 Apple 的
// 传输驱动会主动 close 设备，救砖巡检会把"无人持有"误判为栈拆台，两拍后
// 在关机路径里 reset() 重枚举——把干净关机拖成分钟级死等（最坏 watchdog
// panic）。本回调由电源管理层在 shutdown/restart 时投递（本驱动注册过
// registerPowerDriver，必然收到），睡眠走的是 setPowerState，不受影响。
void IntelBluetoothFirmware::systemWillShutdown(IOOptionBits specifier)
{
    if (m_rescueEnabled) {
        m_rescueEnabled = false;
        m_rescueCandidate = 0;
        XYLog("rescue: disarmed for shutdown/restart (specifier=0x%lx)\n",
              (unsigned long)specifier);
    }
    super::systemWillShutdown(specifier);
}

void IntelBluetoothFirmware::stop(IOService *provider)
{
    XYLog("Driver Stop()\n");
    ++gServiceStopCount;
    ibtDiagPublishLive();
    // 1.9.20: 这里不再 teardown！实测（19:07 第三次唤醒崩溃）发现本驱动是 USB
    // 服务，睡眠时系统会 stop()、唤醒再 start()——旧代码在 stop 里回收全部
    // UPL，等于每次睡眠都亲手拆掉 A 类保护，唤醒后补丁页被逐出重读即
    // SIGBUS（WireActive 9→0 就是证据）。UPL 持有 object+vnode 引用，跨
    // 服务 stop/start 持有完全安全，teardown 改到 free()（仅 kext 卸载时）。
    // 先停定时器再摘 gDiagSvc，避免回调在 service 释放之后仍去 setProperty。
    if (m_pDiagTimer) {
        m_pDiagTimer->cancelTimeout();
        if (getWorkLoop())
            getWorkLoop()->removeEventSource(m_pDiagTimer);
        OSSafeReleaseNULL(m_pDiagTimer);
    }
    gDiagSvc = nullptr;
    if (m_pmStarted) {
        PMstop();
        m_pmStarted = false;
    }
    super::stop(provider);
}

IOService * IntelBluetoothFirmware::probe(IOService *provider, SInt32 *score)
{
    XYLog("Driver Probe()\n");
    if (!super::probe(provider, score)) {
        XYLog("super probe failed\n");
        return NULL;
    }
    IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev) {
        XYLog("is not usb device\n");
        return NULL;
    }
    UInt16 vendorID = USBToHost16(dev->getDeviceDescriptor()->idVendor);
    UInt16 productID = USBToHost16(dev->getDeviceDescriptor()->idProduct);
    XYLog("name=%s, class=%s, vendorID=0x%04X, productID=0x%04X\n", dev->getName(), provider->metaClass->getClassName(), vendorID, productID);
    // La generacion sale de devices.txt (la misma tabla que genera las personalidades
    // del Info.plist). Un ID desconocido ya no cae en Gen2 en silencio: se rechaza.
    for (const IbtDevice &d : kIbtDevices) {
        if (d.pid == productID && vendorID == 0x8087) {
            currentType = (BTType)d.gen;
            return this;
        }
    }
    XYLog("productID 0x%04X not in the supported device table\n", productID);
    return NULL;
}
