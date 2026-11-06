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

void ibtDiagPublishLive()
{
    IOService *svc = gDiagSvc;
    if (!svc)
        return;

    IbtDiagSnapshot s {};
    ibtDiagFill(&s);
    s.pmEvents = gPmEvents;

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
    svc->setProperty("IBTApfsVerAll", (unsigned long long)s.apfsVerAll, 32);
    svc->setProperty("IBTApfsVerOnBt", (unsigned long long)s.apfsVerOnBt, 32);
    svc->setProperty("IBTApfsPiAll", (unsigned long long)s.apfsPiAll, 32);
    svc->setProperty("IBTApfsPiOnBt", (unsigned long long)s.apfsPiOnBt, 32);
    svc->setProperty("IBTApfsUncAll", (unsigned long long)s.apfsUncAll, 32);
    svc->setProperty("IBTApfsUncOnBt", (unsigned long long)s.apfsUncOnBt, 32);
    svc->setProperty("IBTApfsRlAll", (unsigned long long)s.apfsRlAll, 32);
    svc->setProperty("IBTApfsRlOnBt", (unsigned long long)s.apfsRlOnBt, 32);
    svc->setProperty("IBTPmEvents", (unsigned long long)s.pmEvents, 32);
    svc->setProperty("IBTPatchedPages", (unsigned long long)s.patchedPages, 32);
    svc->setProperty("IBTLastOffset", s.lastOffset, 64);
    svc->setProperty("IBTLastSize", s.lastSize, 32);
    svc->setProperty("IBTLastPage", s.lastPage, 64);
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
    // 1.9.5: 从这里开始 ioreg 才读得到诊断状态。processPatcher 走的是 Lilu 的
    // onPatcherLoad 回调，早于本函数的 start()，所以此刻 armed 标志已是最终值。
    gDiagSvc = this;
    ibtDiagPublishLive();
    cleanUp();
    return true;
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
    ibtDiagPublishLive();
    return IOPMAckImplied;
}

void IntelBluetoothFirmware::stop(IOService *provider)
{
    XYLog("Driver Stop()\n");
    ibtDiagPublishLive();
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
