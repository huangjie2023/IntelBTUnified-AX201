//
//  IntelBTUnified.cpp
//
//  Plugin de Lilu que junta en un solo kext:
//    - IntelBTPatcher (zxystd, OpenIntelWireless; fixes by lshbluesky v2.5.2):
//        direccion aleatoria BLE + reescritura de eventos LE Read Remote
//        Features Complete (fake PHY Update Complete + fake LE Data Length
//        Change, #514) para desbloquear LE_READY_WAIT_FOR_LENGTH.
//    - BlueToolFixup (Dhinak G / acidanthera): parches de BlueTool y
//      bluetoothd en memoria. A diferencia de BlueToolFixup original, los
//      parches son PROCESS-SCOPED: solo se aplican cuando el propio proceso
//      bluetoothd / BlueTool valida sus paginas, de modo que softwareupdated
//      ve los binarios originales y la actualizacion OTA de macOS no falla.
//  Desde 1.7.0 estos parches estan ACTIVOS (sin ellos el controlador no se enciende en
//  macOS 26) y siguen siendo process-scoped para no romper la actualizacion OTA.
//  -ibtota los desactiva; -ibtplegacy restaura el comportamiento incondicional.
//  El driver que carga el firmware (IntelBluetoothFirmware) vive en el mismo
//  bundle. Solo se incluye firmware AX201 (8087:0026, gen2, ibt-19-32-*).
//
//  1.8.0: la reescritura de eventos LE Read Remote Features lleva estado por conexion
//  (connection handle) en vez de un contador global. Con varios dispositivos
//  reconectando a la vez (tipico del arranque en frio) la secuencia global se
//  desalineaba: un dispositivo perdia su features complete y no conectaba, sin ningun
//  crash ni log. -ibtleoff desactiva esa reescritura para aislar el problema.
//
//  1.9.1: 根治「唤醒/内存压力后 bluetoothd SIGBUS（APFS compressed data hash mismatch）」。
//  机制（acidanthera/bugtracker#2228，jaymatich 2026-08-28 受控复现）：
//    1. 本插件经 cs_validate_page 在内存里改写 bluetoothd/BlueTool 的某个 4K 代码页；
//    2. 该页所属的 APFS 16KiB 压缩验证组在内存压力下被“部分”逐出（补丁页驻留、
//       相邻页被回收）；
//    3. 回填缺页时 decmpfs_pagein_compressed 解压缺页后对整个 16KiB 组调 VNOP_VERIFY；
//    4. “驻留补丁页 + 回填原始页”的混合内容与盘上认证哈希不符 → EBADMSG → SIGBUS。
//  这解释了延迟出现、负载相关、purge/重启暂时修复、盘上数据校验全部通过的现象。
//  修复：路由 _VNOP_VERIFY，仅当“本 boot 我们确实补丁过的那两个文件
//  (/usr/sbin/bluetoothd、/usr/sbin/BlueTool)”返回 EBADMSG 时吞掉。回填的页内容本身
//  正确（解压的原始字节），校验失败纯粹因为内存里的补丁页混进了验证组；吞掉后
//  decmpfs 正常 commit，故障页路径与崩溃一并消失。系统其余文件的完整性校验完全
//  不受影响（按 vnode 路径过滤）。-ibtnovb 关闭此行为。
//
//  Solo macOS 12 (Monterey) o posterior: los parches de macOS 11 y anteriores se han eliminado.
//
//  1.9.2: 修掉 1.9.1 吞错的实际失效原因（顺序依赖 bug）。
//  1.9.1 要求“本 boot 真的补丁过该文件”才允许吞错，而该标志只在某页命中 pattern 时
//  才置位。真实顺序是「先缺页 → 先做 16KiB 组校验 → 才轮到后面含 pattern 的页」，
//  崩溃发生时标志必然还是 false，条件永远不成立——本机实测：连续崩 6 次、同一
//  offset、日志里零条 swallowed。1.9.2 把门控改成“钩子已 armed + vnode 路径精确匹配
//  这两个文件 + 错误码 EBADMSG”，并补上成功路径的日志（armed / swallowed #n /
//  vn_getpath 失败 / 非 EBADMSG），下次崩溃后日志本身就能给出结论。
//
//  1.9.3: 把吞错门控改成“失败区间里真的含我们改写过的 4K 页”。
//  1.9.2 用“路径命中就吞”虽然能生效，但会连带吞掉与补丁无关的真数据损坏，也让
//  “到底是补丁还是盘”无法判定。1.9.3 记录每页被改写的文件偏移，VNOP_VERIFY 失败时
//  比对区间：命中 → 吞并打 `our patched page 0x... is inside this verify range`；
//  不命中 → 原样返回并打 `contains NO page we patched ... real data corruption`。
//  本机 2026-11-06 15:17 崩溃（offset 0x339B50，与前一天完全相同）就是这条判据的
//  第一个待判对象。
//
//  SPDX-License-Identifier: GPL-3.0-only
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_version.hpp>
#include <Headers/kern_devinfo.hpp>

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <sys/proc.h>
#include <sys/random.h>
#include <sys/errno.h>

#include "Devices.h"

#define MODULE_IBTP  "ibtp"

// Lilu 的 DBGLOG 在非 DEBUG 构建里被编译成空语句（字符串根本不进二进制），所以
// "-ibtpdbg 能打开日志"是假的。这里用运行时开关代替：默认不打印，加 -ibtpdbg 才输出。
// 判断放在 pluginStart 里做一次，热路径上只有一个 bool 比较。
static bool diagEnabled = false;
#define DIAGLOG(module, str, ...) do { if (diagEnabled) SYSLOG(module, str, ## __VA_ARGS__); } while (0)
#define MODULE_BTLFX "btlfx"

#pragma mark - Intel HCI: direccion aleatoria + eventos LE (antes IntelBTPatcher)

#define INTEL_VENDOR_ID             0x8087
#define HCI_OP_RESET                0x0c03
#define HCI_OP_LE_SET_SCAN_PARAM    0x200B
#define HCI_OP_LE_SET_RANDOM_ADDR   0x2005
#define HCI_OP_LE_READ_REMOTE_FEATURES 0x2016
#define HCI_CMD_HDR_SIZE            3
#define MAX_HCI_BUF_LEN             255

typedef struct __attribute__((packed)) {
    uint16_t opcode;
    uint8_t  len;
    uint8_t  data[];
} HciCommandHdr;

typedef struct __attribute__((packed)) {
    uint8_t  evt;
    uint8_t  len;
    uint8_t  data[];
} HciEventHdr;

typedef struct {
    void *owner;
    IOMemoryDescriptor *dataBuffer;
    IOUSBHostCompletionAction action;
} AsyncOwnerData;

static mach_vm_address_t oldHostDeviceRequest {};
static mach_vm_address_t oldAsyncIO {};
static mach_vm_address_t oldInitPipe {};
static mach_vm_address_t oldInitPipeConfig {};

// El original usaba una direccion constante igual para todo el mundo. Ahora se genera
// una por arranque. Una direccion estatica aleatoria BLE exige los dos bits altos a 1.
static uint8_t randomAddrCmd[HCI_CMD_HDR_SIZE + 6];
// 1 = ya enviada tras el ultimo HCI_Reset. Se toca desde un hook que puede ejecutarse en paralelo.
static int randomAddrSent = 0;

// Pipe de interrupcion (eventos HCI) del controlador Intel, capturado en initWithDescriptorsAndOwners.
static void *hookPipeInstance = nullptr;

// #514: estado por conexion (1.8.0).
//
// 1.7.0 y anteriores usaban un unico contador global (leMetaStage) para reescribir los
// eventos LE Read Remote Features Complete. Eso solo funciona si las conexiones llegan de
// una en una. Al arrancar en frio el Mac reconecta varios dispositivos a la vez y los eventos
// de conexiones distintas se intercalan (A1 B1 A2 B2 A3 B3), con lo que la maquina de
// estados global se desplaza: el evento real de un dispositivo se convierte en "fake PHY
// Update" y el otro pierde su features complete. Resultado: el controlador esta perfecto,
// no hay ningun crash, y el dispositivo simplemente no conecta hasta el siguiente reinicio.
//
// Ahora el estado va indexado por connection handle (los tres eventos de una misma lectura
// llevan el mismo handle) y se libera solo cuando se han reescrito los tres.
#define LE_SLOT_COUNT       4       // conexiones simultaneas soportadas
#define LE_SLOT_IDLE_LIMIT  256     // eventos sin usar el slot -> se descarta

struct LeSlot {
    uint16_t handle;
    uint8_t  stage;      // 0 = real (pasa), 1 -> fake PHY Update, 2 -> fake Data Length
    uint16_t idle;       // eventos vistos desde el ultimo uso (para caducar)
    bool     used;
};

static LeSlot leSlots[LE_SLOT_COUNT];
static int  leActiveSlots = 0;
// -ibtleoff: desactiva por completo la reescritura (palanca de diagnostico).
static bool leRewriteDisabled = false;
// PHY que se reporta en el fake PHY Update Complete (0x01 = LE 1M, 0x02 = LE 2M).
static uint8_t fakePhyValue = 0x01;
// Presupuesto de lineas de log para no inundar el arranque.
static int  leLogBudget = 16;

static inline uint16_t leHandleOfEvent(const HciEventHdr *hdr) {
    // Read Remote Features Complete: [0]=subevent 0x04, [1]=status, [2..3]=handle
    return (uint16_t)((hdr->data[2] | (hdr->data[3] << 8)) & 0x0FFF);
}

static LeSlot *leFindSlot(uint16_t handle) {
    for (int i = 0; i < LE_SLOT_COUNT; i++)
        if (leSlots[i].used && leSlots[i].handle == handle)
            return &leSlots[i];
    return nullptr;
}

static void leClearAllSlots(const char *why) {
    bool any = false;
    for (int i = 0; i < LE_SLOT_COUNT; i++) {
        if (leSlots[i].used) {
            any = true;
            leSlots[i].used = false;
        }
    }
    if (__atomic_exchange_n(&leActiveSlots, 0, __ATOMIC_SEQ_CST) && any)
        DIAGLOG(MODULE_IBTP, "le slots cleared (%s)", why);
}

static void leRegisterSlot(uint16_t handle) {
    LeSlot *slot = leFindSlot(handle);
    if (slot) {     // reintento sobre la misma conexion: reiniciar la secuencia
        slot->stage = 0;
        slot->idle = 0;
        return;
    }
    for (int i = 0; i < LE_SLOT_COUNT; i++) {
        if (!leSlots[i].used) {
            leSlots[i].used = true;
            leSlots[i].handle = handle;
            leSlots[i].stage = 0;
            leSlots[i].idle = 0;
            __atomic_add_fetch(&leActiveSlots, 1, __ATOMIC_SEQ_CST);
            DIAGLOG(MODULE_IBTP, "le slot +h=0x%03x (active %d)", handle, leActiveSlots);
            return;
        }
    }
    // Tabla llena: desalojar el slot mas inactivo y quedarse con la conexion nueva.
    // Dejar la nueva sin registrar es peor: sus dos eventos extra llegarian intactos y
    // bluetoothd veria tres "features complete" para una sola peticion, que es el tipo
    // de incoherencia que dispara el aborto en la reconexion tras el reposo.
    int victim = 0;
    for (int i = 1; i < LE_SLOT_COUNT; i++)
        if (leSlots[i].idle > leSlots[victim].idle)
            victim = i;
    if (leLogBudget-- > 0)
        DIAGLOG(MODULE_IBTP, "le slot evict h=0x%03x (idle %u) -> h=0x%03x", leSlots[victim].handle, leSlots[victim].idle, handle);
    leSlots[victim].handle = handle;
    leSlots[victim].stage = 0;
    leSlots[victim].idle = 0;
}

static void makeRandomAddressCommand() {
    uint8_t addr[6];
    bool allZero, allOne;
    do {
        read_random(addr, sizeof(addr));
        addr[5] |= 0xC0;
        // La parte aleatoria (46 bits) no puede ser todo ceros ni todo unos.
        allZero = (addr[0] | addr[1] | addr[2] | addr[3] | addr[4]) == 0 && (addr[5] & 0x3F) == 0;
        allOne  = (addr[0] & addr[1] & addr[2] & addr[3] & addr[4]) == 0xFF && (addr[5] & 0x3F) == 0x3F;
    } while (allZero || allOne);
    randomAddrCmd[0] = HCI_OP_LE_SET_RANDOM_ADDR & 0xFF;
    randomAddrCmd[1] = HCI_OP_LE_SET_RANDOM_ADDR >> 8;
    randomAddrCmd[2] = 6;
    memcpy(&randomAddrCmd[HCI_CMD_HDR_SIZE], addr, sizeof(addr));
}

static bool isIntelBluetooth(IOUSBHostDevice *dev) {
    auto desc = dev ? dev->getDeviceDescriptor() : nullptr;
    if (!desc || USBToHost16(desc->idVendor) != INTEL_VENDOR_ID)
        return false;
    uint16_t pid = USBToHost16(desc->idProduct);
    for (const IbtDevice &d : kIbtDevices)
        if (d.pid == pid)
            return true;
    return false;
}

static IOReturn newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length, IOUSBHostCompletion *completion, unsigned int timeout);

static void sendRandomAddress(void *that, IOService *provider, unsigned int timeout) {
    StandardUSB::DeviceRequest req {};
    req.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeClass, kRequestRecipientInterface);
    req.bRequest = 0xE0;
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = sizeof(randomAddrCmd);
    unsigned int len = sizeof(randomAddrCmd);

    IOReturn ret = kIOReturnNoMemory;
    auto desc = IOBufferMemoryDescriptor::withBytes(randomAddrCmd, sizeof(randomAddrCmd), kIODirectionOut);
    if (desc) {
        ret = desc->prepare(kIODirectionOut);
        if (ret == kIOReturnSuccess) {
            ret = FunctionCast(newHostDeviceRequest, oldHostDeviceRequest)(that, provider, req, nullptr, desc, len, nullptr, timeout);
            desc->complete(kIODirectionOut);
        }
        desc->release();
    }
    if (ret == kIOReturnSuccess) {
        DIAGLOG(MODULE_IBTP, "random address HCI sent");
    } else {
        SYSLOG(MODULE_IBTP, "[PATCH] random address HCI failed: 0x%x (will retry on next scan)", ret);
    }
    if (ret != kIOReturnSuccess)
        __atomic_store_n(&randomAddrSent, 0, __ATOMIC_SEQ_CST);  // reintentar en el siguiente scan
}

// #514 (lshbluesky v2.5.2): reescritura en 3 etapas de los eventos LE Read Remote
// Features Complete (0x04) repetidos:
//   etapa 0 = el real (pasa tal cual), 1 = reescrito como fake PHY Update Complete,
//   2 = reescrito como fake LE Data Length Change (desbloquea el
//   LE_READY_WAIT_FOR_LENGTH de bluetoothd).
// Desde 1.8.0 la etapa es POR CONEXION (connection handle), no global: con varios
// dispositivos reconectando a la vez (habitual en arranque en frio) la secuencia global
// se desalineaba y las conexiones se quedaban colgadas sin ningun crash.
#define HCI_EVT_LE_META                               0x3E
#define HCI_EVT_LE_META_READ_REMOTE_FEATURES_COMPLETE 0x04

// Plantillas; cada reescritura usa una copia local para no compartir bytes mutables
// entre conexiones que se completan a la vez.
// El controlador nunca ha recibido HCI_LE_Set_PHY, asi que sigue en LE 1M. El upstream
// reportaba 2M/2M: bluetoothd creia la conexion ya subida a 2M mientras el chip seguia
// en 1M, y esa incoherencia es la sospechosa del aborto a los ~16 s tras despertar.
// 1.9.0 informa 1M/1M (estado real). -ibtphy2m recupera el comportamiento antiguo.
static const uint8_t fakePhyUpdateCompleteEvent[8] = {0x3E, 0x06, 0x0C, 0x00, 0x00, 0x00, 0x01, 0x01};
// LE Data Length Change (LE Meta subevent 0x07, sin byte de estado -> handle en [3][4]).
// bluetoothd se queda en LE_READY_WAIT_FOR_LENGTH tras el PHY hasta ver esto.
// 27 octets / 328us tx+rx.
static const uint8_t fakeDataLengthChangeEvent[13] = {0x3E, 0x0B, 0x07, 0x00, 0x00, 0x1B, 0x00, 0x48, 0x01, 0x1B, 0x00, 0x48, 0x01};

static void asyncIOCompletion(void* owner, void* parameter, IOReturn status, uint32_t bytesTransferred)
{
    AsyncOwnerData *asyncOwner = (AsyncOwnerData *)owner;
    IOMemoryDescriptor* dataBuffer = asyncOwner->dataBuffer;
    uint32_t allocLen = bytesTransferred;

    if (dataBuffer && bytesTransferred) {
        void *buffer = IOMalloc(allocLen);
        if (buffer) {
            if (dataBuffer->getLength() > 0 && (getKernelVersion() < KernelVersion::Sequoia || !dataBuffer->prepare(kIODirectionInOut))) {
                dataBuffer->readBytes(0, buffer, allocLen);
                HciEventHdr *hdr = (HciEventHdr *)buffer;
                const bool isLeFeatures = hdr->evt == HCI_EVT_LE_META &&
                                          hdr->data[0] == HCI_EVT_LE_META_READ_REMOTE_FEATURES_COMPLETE &&
                                          hdr->len >= 12;
                const uint16_t curHandle = isLeFeatures ? leHandleOfEvent(hdr) : 0xFFFF;
                if (isLeFeatures) {
                    uint16_t handle = curHandle;
                    uint8_t eventStatus = hdr->data[1];
                    LeSlot *slot = leFindSlot(handle);
                    if (slot && eventStatus == 0) {
                        // El primer evento de cada conexion pasa intacto (es el features
                        // complete real); los dos siguientes se reescriben.
                        if (slot->stage == 0) {
                            slot->stage = 1;
                            slot->idle = 0;
                            if (leLogBudget-- > 0)
                                DIAGLOG(MODULE_IBTP, "le h=0x%03x features complete (real, pass)", handle);
                        } else if (slot->stage == 1) {
                            uint8_t phy[sizeof(fakePhyUpdateCompleteEvent)];
                            memcpy(phy, fakePhyUpdateCompleteEvent, sizeof(phy));
                            phy[4] = hdr->data[2];
                            phy[5] = hdr->data[3];
                            phy[6] = fakePhyValue;
                            phy[7] = fakePhyValue;
                            dataBuffer->writeBytes(0, phy, sizeof(phy));
                            bytesTransferred = 8;
                            slot->stage = 2;
                            slot->idle = 0;
                            if (leLogBudget-- > 0)
                                DIAGLOG(MODULE_IBTP, "le h=0x%03x -> fake PHY Update", handle);
                        } else {
                            uint8_t dlc[sizeof(fakeDataLengthChangeEvent)];
                            memcpy(dlc, fakeDataLengthChangeEvent, sizeof(dlc));
                            dlc[3] = hdr->data[2];
                            dlc[4] = hdr->data[3];
                            dataBuffer->writeBytes(0, dlc, sizeof(dlc));
                            bytesTransferred = 13;
                            if (leLogBudget-- > 0)
                                DIAGLOG(MODULE_IBTP, "le h=0x%03x -> fake Data Length Change (done)", handle);
                            slot->used = false;
                            __atomic_sub_fetch(&leActiveSlots, 1, __ATOMIC_SEQ_CST);
                        }
                    } else if (slot) {
                        // Status != 0: la lectura fallo, no hay features que reescribir.
                        slot->used = false;
                        __atomic_sub_fetch(&leActiveSlots, 1, __ATOMIC_SEQ_CST);
                        DIAGLOG(MODULE_IBTP, "le h=0x%03x features failed (status %u), slot dropped", handle, eventStatus);
                    } else {
                        // Evento sin slot registrado: llega de una conexion para la que no
                        // se pidieron lecturas extra. Se deja pasar sin tocar.
                        DIAGLOG(MODULE_IBTP, "le h=0x%03x features event without slot, untouched", handle);
                    }
                }
                // Caducar slots que dejaron de recibir eventos (p.ej. conexion abortada o
                // lecturas extra rechazadas por el controlador). Se cuenta con CUALQUIER
                // evento, no solo con features complete: si no llega ninguno mas, el slot
                // quedaria armado para siempre y un handle reutilizado heredaria la etapa.
                if (leActiveSlots > 0) {
                    for (int i = 0; i < LE_SLOT_COUNT; i++) {
                        if (!leSlots[i].used || leSlots[i].handle == curHandle)
                            continue;
                        if (++leSlots[i].idle > LE_SLOT_IDLE_LIMIT) {
                            DIAGLOG(MODULE_IBTP, "le slot h=0x%03x expired (idle)", leSlots[i].handle);
                            leSlots[i].used = false;
                            __atomic_sub_fetch(&leActiveSlots, 1, __ATOMIC_SEQ_CST);
                        }
                    }
                }
                if (getKernelVersion() >= KernelVersion::Sequoia) dataBuffer->complete(kIODirectionInOut);
            }
            IOFree(buffer, allocLen);
        }
    }
    if (asyncOwner->action)
        asyncOwner->action(asyncOwner->owner, parameter, status, bytesTransferred);
    delete asyncOwner;
}

static IOReturn newAsyncIO(void *that, IOMemoryDescriptor* dataBuffer, uint32_t bytesTransferred, IOUSBHostCompletion* completion, uint32_t completionTimeoutMs)
{
    if (!leRewriteDisabled && leActiveSlots > 0 && completion) {
        if (that == hookPipeInstance) {
            AsyncOwnerData *ownerData = new AsyncOwnerData;
            ownerData->action = completion->action;
            ownerData->owner = completion->owner;
            ownerData->dataBuffer = dataBuffer;
            completion->action = asyncIOCompletion;
            completion->owner = ownerData;
        } else if (leLogBudget-- > 0) {
            // Diagnostico de arranque en frio: si el pipe capturado no es el que usa el
            // controlador, la reescritura LE no se aplica a nada y el sintoma es
            // exactamente "bluetooth encendido pero no conecta, sin crash".
            DIAGLOG(MODULE_IBTP, "event pipe mismatch: captured %p, io on %p (le fix inactive)", hookPipeInstance, that);
        }
    }
    return FunctionCast(newAsyncIO, oldAsyncIO)(that, dataBuffer, bytesTransferred, completion, completionTimeoutMs);
}

static void captureIntelInterruptPipe(void *that, IOUSBHostDevice *device, StandardUSB::EndpointDescriptor const *descriptor) {
    if (!device || !descriptor)
        return;
    const StandardUSB::DeviceDescriptor *deviceDescriptor = device->getDeviceDescriptor();
    if (deviceDescriptor && USBToHost16(deviceDescriptor->idVendor) == INTEL_VENDOR_ID &&
        isIntelBluetooth(device)) {
        uint8_t epType = StandardUSB::getEndpointType(descriptor);
        if (epType == kIOUSBEndpointTypeInterrupt && hookPipeInstance != that) {
            // Cambia en cada re-enumeracion (tras la descarga de firmware), por eso se
            // registra: una captura fallida explica los fallos de conexion en frio.
            SYSLOG(MODULE_IBTP, "[PATCH] event pipe captured %p (was %p)", that, hookPipeInstance);
            hookPipeInstance = that;
        }
    }
}

static int newInitPipe(void *that, StandardUSB::EndpointDescriptor const *descriptor, StandardUSB::SuperSpeedEndpointCompanionDescriptor const *superDescriptor, AppleUSBHostController *controller, IOUSBHostDevice *device, IOUSBHostInterface *interface, unsigned char a7, unsigned short a8)
{
    int ret = FunctionCast(newInitPipe, oldInitPipe)(that, descriptor, superDescriptor, controller, device, interface, a7, a8);
    captureIntelInterruptPipe(that, device, descriptor);
    return ret;
}

// macOS 26.5+: el tercer parametro paso a ser ConfigurationDescriptor (era SuperSpeedEndpointCompanionDescriptor)
static int newInitPipeConfig(void *that, StandardUSB::EndpointDescriptor const *descriptor, StandardUSB::ConfigurationDescriptor const *configDescriptor, AppleUSBHostController *controller, IOUSBHostDevice *device, IOUSBHostInterface *interface, unsigned char a7, unsigned short a8)
{
    int ret = FunctionCast(newInitPipeConfig, oldInitPipeConfig)(that, descriptor, configDescriptor, controller, device, interface, a7, a8);
    captureIntelInterruptPipe(that, device, descriptor);
    return ret;
}

static IOReturn newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length, IOUSBHostCompletion *completion, unsigned int timeout) {
    // Solo comandos HCI (clase, host->dispositivo, bRequest 0) de un controlador
    // Bluetooth Intel conocido (antes se inspeccionaba cualquier dispositivo USB).
    auto dev = OSDynamicCast(IOUSBHostDevice, reinterpret_cast<OSObject *>(that));
    if (dev && isIntelBluetooth(dev) && request.wLength >= HCI_CMD_HDR_SIZE && request.bRequest == 0 &&
        (request.bmRequestType & kDeviceRequestDirectionMask) == (kRequestDirectionOut << kDeviceRequestDirectionPhase) &&
        (request.bmRequestType & kDeviceRequestTypeMask) == (kRequestTypeClass << kDeviceRequestTypePhase)) {
        uint8_t hciBuf[MAX_HCI_BUF_LEN] = {};
        const HciCommandHdr *hdr = nullptr;
        if (data != nullptr) {
            hdr = static_cast<const HciCommandHdr *>(data);
        } else if (descriptor != nullptr && descriptor->getLength() >= HCI_CMD_HDR_SIZE) {
            // En Sequoia+ el descriptor llega sin preparar y no se puede leer directamente.
            bool prepared = getKernelVersion() < KernelVersion::Sequoia || descriptor->prepare(kIODirectionOut) == kIOReturnSuccess;
            if (prepared) {
                descriptor->readBytes(0, hciBuf, min(descriptor->getLength(), (IOByteCount)MAX_HCI_BUF_LEN));
                if (getKernelVersion() >= KernelVersion::Sequoia)
                    descriptor->complete(kIODirectionOut);
                hdr = reinterpret_cast<const HciCommandHdr *>(hciBuf);
            }
        }
        if (hdr) {
            if (hdr->opcode == HCI_OP_RESET) {
                // Tras un reset hay que volver a enviar la direccion y olvidar el estado
                // de las conexiones anteriores (el controlador las ha perdido).
                __atomic_store_n(&randomAddrSent, 0, __ATOMIC_SEQ_CST);
                leClearAllSlots("hci reset");
            } else if (hdr->opcode == HCI_OP_LE_SET_SCAN_PARAM) {
                if (__atomic_exchange_n(&randomAddrSent, 1, __ATOMIC_SEQ_CST) == 0)
                    sendRandomAddress(that, provider, timeout);
            } else if (hdr->opcode == HCI_OP_LE_READ_REMOTE_FEATURES && data == nullptr) {
                // #514: enviar DOS lecturas extra -> dos eventos 0x04 duplicados que se
                // reescriben (1er dup -> fake PHY Update Complete, 2o dup -> fake LE Data
                // Length Change). La peticion real (con completion) se ejecuta abajo.
                // El estado se lleva por connection handle: los tres eventos de esta
                // lectura lo comparten, y asi dos dispositivos reconectando a la vez no
                // se pisan (causa de "conecta a veces" en arranque en frio).
                if (!leRewriteDisabled && hdr->len >= 2) {
                    uint16_t handle = (uint16_t)((hdr->data[0] | (hdr->data[1] << 8)) & 0x0FFF);
                    leRegisterSlot(handle);
                }
                FunctionCast(newHostDeviceRequest, oldHostDeviceRequest)(that, provider, request, nullptr, descriptor, length, nullptr, timeout);
                FunctionCast(newHostDeviceRequest, oldHostDeviceRequest)(that, provider, request, nullptr, descriptor, length, nullptr, timeout);
            }
            DIAGLOG(MODULE_IBTP, "opcode: 0x%04x len: %u wLength: %u async: %d", hdr->opcode, hdr->len, request.wLength, completion != nullptr);
        }
    }
    return FunctionCast(newHostDeviceRequest, oldHostDeviceRequest)(that, provider, request, data, descriptor, length, completion, timeout);
}

static const char *kextIOUSBHostPath[] { "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily" };

static KernelPatcher::KextInfo kextIOUSBHost {
    "com.apple.iokit.IOUSBHostFamily",
    kextIOUSBHostPath,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

static void processUSBHost(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
    if (kextIOUSBHost.loadIndex != index)
        return;
    makeRandomAddressCommand();
    KernelPatcher::RouteRequest requests[] {
        {"__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
            newHostDeviceRequest, oldHostDeviceRequest},
        {"__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
            newAsyncIO, oldAsyncIO},
        // macOS 26.5+: el tercer parametro paso a ser ConfigurationDescriptor
        {"__ZN13IOUSBHostPipe28initWithDescriptorsAndOwnersEPKN11StandardUSB18EndpointDescriptorEPKNS0_23ConfigurationDescriptorEP22AppleUSBHostControllerP15IOUSBHostDeviceP18IOUSBHostInterfaceht",
            newInitPipeConfig, oldInitPipeConfig},
        {"__ZN13IOUSBHostPipe28initWithDescriptorsAndOwnersEPKN11StandardUSB18EndpointDescriptorEPKNS0_37SuperSpeedEndpointCompanionDescriptorEP22AppleUSBHostControllerP15IOUSBHostDeviceP18IOUSBHostInterfaceht",
            newInitPipe, oldInitPipe},
    };
    patcher.routeMultiple(index, requests, arrsize(requests), address, size);
    if (patcher.getError() == KernelPatcher::Error::NoError) {
        SYSLOG(MODULE_IBTP, "routed %zu symbols", arrsize(requests));
    } else {
        SYSLOG(MODULE_IBTP, "failed to resolve symbols, error = %d", patcher.getError());
        patcher.clearError();
    }
}

#pragma mark - BlueTool / bluetoothd (antes BlueToolFixup), process-scoped para OTA

static const uint8_t kSkipUpdateFilePathOriginal[] = "/etc/bluetool/SkipBluetoothAutomaticFirmwareUpdate";
static const uint8_t kSkipUpdateFilePathPatched[]  = "/System/Library/CoreServices/boot.efi";

// Workaround 12.4 Beta 3+: macOS puede detectar el chipset dos veces (interno y dongle externo):
// 'ERROR -- Third Party Dongle has the same address as the internal module'
static const uint8_t kSkipAddressCheckOriginal[] = {
    0x48, 0x89, 0xF3,             // mov    rbx, rsi
    0xE8, 0x00, 0x00, 0x00, 0x00, // call   <somewhere>
    0x85, 0xC0,                   // test   eax, eax
    0x74, 0x1D,                   // je
};
static const uint8_t kSkipAddressCheckPatched[] = {
    0x48, 0x89, 0xF3,
    0xE8, 0x00, 0x00, 0x00, 0x00,
    0x85, 0xC0,
    0x72, 0x1D,                   // jb short
};
static const uint8_t kSkipAddressCheckMask[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF,
    0xFF, 0xFF,
};

static const uint8_t kVendorCheckOriginal[] = { 0x81, 0xFA, 0x5C, 0x0A, 0x00, 0x00, 0x74 };  // cmp edx, BRCM; je
static const uint8_t kVendorCheckPatched[]  = { 0x81, 0xFA, 0x5C, 0x0A, 0x00, 0x00, 0xEB };  // jmp

// El chequeo de rango de chipset no considera valido el 0 "THIRD_PARTY_DONGLE": sin esto el
// Bluetooth no vuelve a encenderse tras el primer ciclo de energia (acidanthera/BrcmPatchRAM#18).
static const uint8_t kBadChipsetCheckOriginal[] = { 0x81, 0xF9, 0xCF, 0x07, 0x00, 0x00, 0x72 };  // jb
static const uint8_t kBadChipsetCheckPatched[]  = { 0x81, 0xF9, 0xCF, 0x07, 0x00, 0x00, 0xEB };  // jmp

static const uint8_t kBadChipsetCheckOriginal13_3[] = { 0x81, 0xF9, 0x9E, 0x0F, 0x00, 0x00, 0x77, 0x1A };
static const uint8_t kBadChipsetCheckPatched13_3[]  = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

static const uint8_t kBadChipsetCheckOriginal15_4[] = { 0x81, 0xF9, 0x6F, 0x17, 0x00, 0x00, 0x77, 0x30 };
static const uint8_t kBadChipsetCheckPatched15_4[]  = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

static const uint8_t kSkipInternalControllerNVRAMCheck13_3[] = {
    0x41, 0x80, 0x00, 0x01, // xor     r15b, 1
    0x75, 0x00,             // jnz     short
    0x84, 0xDB,             // test    bl, bl
    0x75, 0x00              // jnz     short
};
static const uint8_t kSkipInternalControllerNVRAMCheckMask13_3[] = {
    0xFF, 0xFF, 0x00, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF,
    0xFF, 0x00
};
static const uint8_t kSkipInternalControllerNVRAMCheckPatched13_3[] = {
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static bool shouldPatchBoardId = false;
static bool shouldPatchAddress = false;
static bool shouldPatchNvramCheck = false;
// 1.6.0 desactivo estos parches por defecto; en 1.7.0 vuelven a estar activos
// porque sin ellos el controlador no se enciende (macOS 26.7.2). El offset de
// cada parche se registra para poder comprobar si la pagina que falla al
// reanudar es una de las que modificamos.
static bool disableCsPatching = false;
// Modo legado: parchea siempre (comportamiento BlueToolFixup original, rompe OTA).
static bool legacyCsPatching = false;

static constexpr size_t kBoardIdSize = sizeof("Mac-F60DEB81FF30ACF6");
static constexpr size_t kBoardIdSizeLegacy = sizeof("Mac-F22586C8");

static const char boardIdsWithUSBBluetooth[][kBoardIdSize] = {
    "Mac-F60DEB81FF30ACF6", "Mac-9F18E312C5C2BF0B", "Mac-937CB26E2E02BB01", "Mac-E43C1C25D4880AD6",
    "Mac-06F11FD93F0323C5", "Mac-06F11F11946D27C5", "Mac-A369DDC4E67F1C45", "Mac-FFE5EF870D7BA81A",
    "Mac-DB15BD556843C820", "Mac-B809C3757DA9BB8D", "Mac-65CE76090165799A", "Mac-4B682C642B45593E",
    "Mac-77F17D7DA9285301", "Mac-BE088AF8C5EB4FA2"
};

static mach_vm_address_t orig_cs_validate {};

// Cada parche deja constancia la primera vez que se aplica. Si tras una actualizacion de macOS
// un patron deja de coincidir, el parche desaparece del log en vez de fallar en silencio.
enum PatchId : uint32_t {
    PatchSkipUpdate  = 1u << 0,
    PatchBlueToolBid = 1u << 1,
    PatchVendor      = 1u << 2,
    PatchChipset     = 1u << 3,
    PatchChipset13   = 1u << 4,
    PatchChipset15   = 1u << 5,
    PatchNvram       = 1u << 6,
    PatchBtdBid      = 1u << 7,
    PatchAddress     = 1u << 8,
};
static uint32_t appliedPatches = 0;

// 1.7.0: se registra el offset de fichero de cada parche aplicado. Sirve para
// saber si la pagina que falla al reanudar (APFS compressed data hash mismatch)
// es justamente una de las que tocamos nosotros o no.
static uint64_t currentPageOffset = 0;

// 1.9.3: 精确门控——只有「本次校验的区间里确实包含我们改写过的 4K 页」才吞。
//
// 走过来的两步教训：
//   1.9.1 用“本 boot 补丁过该文件”做门控，那是顺序依赖的死锁：该标志只在某页命中
//   pattern 时才置位，而失败页通常先被访问，标志恒 false → 一次都不吞（实测连崩 6 次、
//   同一 offset、零条 swallowed）。
//   1.9.2 改成“路径命中就吞”，能吞了，但会连带吞掉**与补丁无关**的真数据损坏
//   （那等于把盘上读错的代码放行，比崩溃更危险），而且丢掉了一个关键结论：
//   到底是不是我们改的页导致的。
//
// 1.9.3 直接按事实判定：cs_validate 每次真正改写一页，就把该页的 4K 文件偏移记进
// patchedPages[]；VNOP_VERIFY 失败时，看失败区间 [offset, offset+bufsize) 里有没有
// 记录过的页。有 → 混合内容，吞（正当）；没有 → 跟补丁无关，原样返回并记一行
// “real data corruption”，崩溃照旧，但结论从此不用猜。
// 这个判定天然没有顺序问题：能造成组哈希不符的页，必然已经被我们改过并记录过。
static bool verifyBypassArmed = false;
// -ibtnovb: 关闭 VNOP_VERIFY 吞错（诊断开关）。
static bool disableVerifyBypass = false;
// 计数：命中（吞掉）次数、路径解析失败次数、非 EBADMSG 错误次数、“不是我们的页”次数。
static uint32_t verifySwallowed = 0;
static uint32_t verifyPathFailures = 0;
static uint32_t verifyOtherErrors = 0;
static uint32_t verifyNotOurs = 0;
// 被我们改写过的 4K 页（文件偏移）。写入只发生在 cs_validate 上下文，读取在任意进程
// 上下文的 VNOP_VERIFY 里，所以走原子。0 表示空槽（offset 0 是 Mach-O 头所在页，
// 永远不含我们要匹配的指令序列）。
static constexpr uint32_t kMaxPatchedPages = 64;
static uint64_t patchedPages[kMaxPatchedPages] {};
static uint32_t patchedPageCount = 0;
static constexpr size_t kSbinDirLength = sizeof("/usr/sbin/") - 1;

static inline void recordPatchedPage(uint64_t pageOffset) {
    uint32_t n = __atomic_load_n(&patchedPageCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < kMaxPatchedPages; i++)
        if (__atomic_load_n(&patchedPages[i], __ATOMIC_RELAXED) == pageOffset)
            return;
    uint32_t slot = __atomic_fetch_add(&patchedPageCount, 1, __ATOMIC_ACQ_REL);
    if (slot < kMaxPatchedPages)
        __atomic_store_n(&patchedPages[slot], pageOffset, __ATOMIC_RELEASE);
}

static inline void notePatch(PatchId id, const char *name, const char *path, size_t off) {
    if ((__atomic_fetch_or(&appliedPatches, (uint32_t)id, __ATOMIC_RELAXED) & id) == 0) {
        recordPatchedPage(currentPageOffset);
        SYSLOG(MODULE_BTLFX, "patched %s: %s (page 0x%llx, file offset 0x%llx)", path, name,
               (unsigned long long)currentPageOffset, (unsigned long long)(currentPageOffset + off));
    }
}

static inline size_t findPatternOffset(const void *haystack, size_t haystackSize, const void *needle, size_t findSize) {
    if (!findSize || haystackSize < findSize)
        return SIZE_MAX;
    const uint8_t *h = static_cast<const uint8_t *>(haystack);
    const uint8_t *n = static_cast<const uint8_t *>(needle);
    for (size_t i = 0; i + findSize <= haystackSize; i++)
        if (h[i] == n[0] && memcmp(h + i, n, findSize) == 0)
            return i;
    return SIZE_MAX;
}

// 1.9.1: 返回是否真的改到了字节（findAndReplace 找到并替换才为 true）。
// 返回值只用来表达“本页命中并替换成功”，吞错判定改由 notePatch 记录的页偏移完成。
static inline bool searchAndPatch(PatchId id, const char *name, const void *haystack, size_t haystackSize, const char *path, const void *needle, size_t findSize, const void *patch, size_t replaceSize) {
    size_t off = findPatternOffset(haystack, haystackSize, needle, findSize);
    if (KernelPatcher::findAndReplace(const_cast<void *>(haystack), haystackSize, needle, findSize, patch, replaceSize)) {
        notePatch(id, name, path, off);
        return true;
    }
    return false;
}

template <size_t findSize, size_t replaceSize>
static inline bool searchAndPatch(PatchId id, const char *name, const void *haystack, size_t haystackSize, const char *path, const uint8_t (&needle)[findSize], const uint8_t (&patch)[replaceSize]) {
    return searchAndPatch(id, name, haystack, haystackSize, path, needle, findSize, patch, replaceSize);
}

static inline bool searchAndPatchWithMask(PatchId id, const char *name, const void *haystack, size_t haystackSize, const char *path, const void *needle, size_t findSize, const void *findMask, size_t findMaskSize, const void *patch, size_t replaceSize) {
    size_t off = findPatternOffset(haystack, haystackSize, needle, findSize);
    if (KernelPatcher::findAndReplaceWithMask(const_cast<void *>(haystack), haystackSize, needle, findSize, findMask, findMaskSize, patch, replaceSize, nullptr, 0)) {
        notePatch(id, name, path, off);
        return true;
    }
    return false;
}

// PROCESS-SCOPED (OTA-safe): la pagina solo se parchea cuando el propio proceso
// bluetoothd / BlueTool esta cargando sus paginas (contexto de page fault del
// proceso en ejecucion). softwareupdated y el resto del sistema validan el
// binario original, por lo que la verificacion de ficheros del actualizador OTA
// no falla ("bluetoothd does not verify / FullReplacementRequired").
static bool currentProcIs(const char *name) {
    char buf[MAXCOMLEN + 1] = {};
    proc_selfname(buf, sizeof(buf));
    return buf[0] != 0 && strcmp(buf, name) == 0;
}

static void patched_cs_validate_page(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data, int *validated_p, int *tainted_p, int *nx_p) {
    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    FunctionCast(patched_cs_validate_page, orig_cs_validate)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
    if (disableCsPatching)
        return;
    currentPageOffset = page_offset;
    if (vn_getpath(vp, path, &pathlen) == 0 && UNLIKELY(strncmp(path, "/usr/sbin/", kSbinDirLength) == 0)) {
        if (strcmp(path + kSbinDirLength, "BlueTool") == 0) {
            if (!legacyCsPatching && !currentProcIs("BlueTool"))
                return;
            bool any = false;
            any |= searchAndPatch(PatchSkipUpdate, "skip firmware auto-update", data, PAGE_SIZE, path, kSkipUpdateFilePathOriginal, kSkipUpdateFilePathPatched);
            if (shouldPatchBoardId)
                any |= searchAndPatch(PatchBlueToolBid, "board-id", data, PAGE_SIZE, path, boardIdsWithUSBBluetooth[0], kBoardIdSize, BaseDeviceInfo::get().boardIdentifier, kBoardIdSize);
            // 页已被改写时 notePatch 已把页偏移记进 patchedPages[]（吞错判定用）；
            // any 保留只为表达“本页至少命中一处”，不参与门控。
            (void)any;
        }
        else if (strcmp(path + kSbinDirLength, "bluetoothd") == 0) {
            if (!legacyCsPatching && !currentProcIs("bluetoothd"))
                return;
            bool any = false;
            any |= searchAndPatch(PatchVendor, "vendor check", data, PAGE_SIZE, path, kVendorCheckOriginal, kVendorCheckPatched);
            any |= searchAndPatch(PatchChipset, "chipset range check", data, PAGE_SIZE, path, kBadChipsetCheckOriginal, kBadChipsetCheckPatched);
            if (getKernelVersion() >= KernelVersion::Ventura && getKernelVersion() < KernelVersion::Sequoia)
                any |= searchAndPatch(PatchChipset13, "chipset range check (13.3)", data, PAGE_SIZE, path, kBadChipsetCheckOriginal13_3, kBadChipsetCheckPatched13_3);
            if (getKernelVersion() >= KernelVersion::Sequoia)
                any |= searchAndPatch(PatchChipset15, "chipset range check (15.4)", data, PAGE_SIZE, path, kBadChipsetCheckOriginal15_4, kBadChipsetCheckPatched15_4);
            if (shouldPatchNvramCheck)
                any |= searchAndPatchWithMask(PatchNvram, "internal controller NVRAM check", data, PAGE_SIZE, path, kSkipInternalControllerNVRAMCheck13_3, sizeof(kSkipInternalControllerNVRAMCheck13_3), kSkipInternalControllerNVRAMCheckMask13_3, sizeof(kSkipInternalControllerNVRAMCheckMask13_3), kSkipInternalControllerNVRAMCheckPatched13_3, sizeof(kSkipInternalControllerNVRAMCheckPatched13_3));
            if (shouldPatchBoardId)
                any |= searchAndPatch(PatchBtdBid, "board-id", data, PAGE_SIZE, path, boardIdsWithUSBBluetooth[0], kBoardIdSize, BaseDeviceInfo::get().boardIdentifier, kBoardIdSize);
            if (shouldPatchAddress)
                any |= searchAndPatchWithMask(PatchAddress, "same-address check", data, PAGE_SIZE, path, kSkipAddressCheckOriginal, sizeof(kSkipAddressCheckOriginal), kSkipAddressCheckMask, sizeof(kSkipAddressCheckMask), kSkipAddressCheckPatched, sizeof(kSkipAddressCheckPatched));
            // 页已被改写时 notePatch 已把页偏移记进 patchedPages[]（吞错判定用）；
            // any 保留只为表达“本页至少命中一处”，不参与门控。
            (void)any;
        }
    }
}

// 1.9.1 起：吞掉“补丁页驻留 + 相邻页回填”造成的 16KiB 验证组校验失败。
//
// 原型（XNU bsd/sys/vnode_if.h）：
//   extern errno_t VNOP_VERIFY(vnode_t, off_t, uint8_t *, size_t, size_t *,
//                              void **, vnode_verify_flags_t, vfs_context_t,
//                              vnode_verify_kind_t *);
// 9 个参数；x86_64 SysV 下前 6 个走寄存器、后 3 个走栈，声明必须精确 9 参才能
// 正确转发。枚举/上下文/类型指针在这里用等价的 ABI 表示（int/void *）。
// 钩子只用到第 1 参（vnode）和返回值，其余原样转发，即使不同 macOS 小版本的
// 中间参数语义有出入也不会误读。
//
// 触发链（acidanthera/bugtracker#2228）：decmpfs_pagein_compressed 回填缺页后
// VNOP_VERIFY 整组校验 → 混合内容与盘上哈希不符 → err 94 (EBADMSG) → UPL 带
// 错误 abort → vm_fault 失败 → bluetoothd SIGBUS（Time Since Wake ~20-30s 的
// 固定签名）。
//
// 1.9.3 的判据（按顺序短路，正常情况下每条热路径只多一次返回值比较）：
//   1. 返回非 0 且 buf != NULL（是对实际数据的校验，不是块大小查询）；
//   2. 钩子确实 armed（-ibtota 下钩子根本不会安装，这道只是兜底）；
//   3. vn_getpath 精确匹配 /usr/sbin/bluetoothd 或 /usr/sbin/BlueTool
//      ——系统其余文件的校验一丝不减，其它错误也照原样返回；
//   4. 错误恰为 EBADMSG；
//   5. **失败区间 [offset, offset+bufsize) 里含至少一个我们改写过的 4K 页**。
// 第 5 条是 1.9.3 新增的：1.9.1 的门控因顺序问题恒不成立（一次不吞），1.9.2 改成
// “路径命中就吞”又会连带吞掉与补丁无关的真数据损坏。按页偏移判定两者都解决，
// 且能直接给出结论：不吞时说明这个 16K 组跟补丁无关。
//
// 吞掉后 decmpfs 走正常 commit 路径，回填的页内容是解压出的原始字节，正确。
static mach_vm_address_t orig_vnop_verify {};

static int patched_vnop_verify(vnode_t vp, off_t offset, uint8_t *buf, size_t bufsize, size_t *verifyblksize, void **verifyctxp, uint32_t flags, void *ctx, void *verifykind) {
    int err = FunctionCast(patched_vnop_verify, orig_vnop_verify)(vp, offset, buf, bufsize, verifyblksize, verifyctxp, flags, ctx, verifykind);
    if (err == 0 || buf == nullptr || vp == nullptr || disableVerifyBypass || !verifyBypassArmed)
        return err;

    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    if (vn_getpath(vp, path, &pathlen) != 0) {
        // 只有真出错时才会走到这里（err != 0），量很小。
        if (__atomic_fetch_add(&verifyPathFailures, 1, __ATOMIC_RELAXED) == 0)
            SYSLOG(MODULE_BTLFX, "verify error %d but vn_getpath failed: cannot scope, not swallowing (further ones silent)", err);
        return err;
    }
    if (strncmp(path, "/usr/sbin/", kSbinDirLength) != 0 ||
        (strcmp(path + kSbinDirLength, "bluetoothd") != 0 && strcmp(path + kSbinDirLength, "BlueTool") != 0))
        return err;

    if (err != EBADMSG) {
        if (__atomic_fetch_add(&verifyOtherErrors, 1, __ATOMIC_RELAXED) == 0)
            SYSLOG(MODULE_BTLFX, "verify returned %d on %s (not EBADMSG, propagated)", err, path);
        return err;
    }

    // 1.9.3: 只有失败区间里真的含我们改写过的页，才认定是“混合内容”并吞。
    uint64_t oursAt = 0;
    uint32_t recorded = __atomic_load_n(&patchedPageCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < recorded && i < kMaxPatchedPages; i++) {
        uint64_t p = __atomic_load_n(&patchedPages[i], __ATOMIC_ACQUIRE);
        if (p != 0 && p >= (uint64_t)offset && p < (uint64_t)offset + bufsize) {
            oursAt = p;
            break;
        }
    }
    if (oursAt == 0) {
        // 和补丁无关：原样返回，让调用方照常失败，但把结论写清楚。
        if (__atomic_fetch_add(&verifyNotOurs, 1, __ATOMIC_RELAXED) == 0)
            SYSLOG(MODULE_BTLFX, "verify mismatch on %s @ 0x%llx (size %zu) contains NO page we patched (%u pages recorded) - not swallowing: this is real data corruption",
                   path, (unsigned long long)offset, bufsize, recorded);
        return err;
    }

    uint32_t n = __atomic_add_fetch(&verifySwallowed, 1, __ATOMIC_RELAXED);
    if (n <= 8)
        SYSLOG(MODULE_BTLFX, "swallowed verify mismatch #%u on %s @ 0x%llx (size %zu): our patched page 0x%llx is inside this verify range",
               n, path, (unsigned long long)offset, bufsize, (unsigned long long)oursAt);
    return 0;
}

static void processPatcher(KernelPatcher &patcher) {
    const bool argOta    = checkKernelArgument("-ibtota");
    legacyCsPatching     = checkKernelArgument("-ibtplegacy");
    // Por defecto SI se parchea (sin los parches el controlador no se enciende);
    // -ibtota desactiva todo, -ibtplegacy usa el modo antiguo incondicional.
    disableCsPatching    = argOta;
    SYSLOG(MODULE_BTLFX, "cs_validate patching: %s", disableCsPatching ? "OFF (-ibtota)" : (legacyCsPatching ? "legacy (unconditional)" : "process-scoped"));
    if (getKernelVersion() >= KernelVersion::Sonoma) {
        shouldPatchBoardId = checkKernelArgument("-btlfxboardid");
    } else {
        auto boardId = BaseDeviceInfo::get().boardIdentifier;
        auto boardIdSize = strlen(boardId) + 1;
        shouldPatchBoardId = boardIdSize == kBoardIdSize || boardIdSize == kBoardIdSizeLegacy;
        if (shouldPatchBoardId)
            for (size_t i = 0; i < arrsize(boardIdsWithUSBBluetooth); i++)
                if (strcmp(boardIdsWithUSBBluetooth[i], boardId) == 0) {
                    shouldPatchBoardId = false;
                    break;
                }
    }
    // 12.4 Beta 3+, XNU 21.5
    if ((getKernelVersion() == KernelVersion::Monterey && getKernelMinorVersion() >= 5) || getKernelVersion() > KernelVersion::Monterey)
        shouldPatchAddress = checkKernelArgument("-btlfxallowanyaddr");
    if (getKernelVersion() <= KernelVersion::Sonoma)
        shouldPatchNvramCheck = checkKernelArgument("-btlfxnvramcheck");
    if (disableCsPatching) {
        SYSLOG(MODULE_BTLFX, "bluetoothd/BlueTool pages left untouched (add -ibtpatch to enable)");
        return;
    }
    // 1.9.1: -ibtnovb 关闭 VNOP_VERIFY 吞错（诊断开关；默认开启）。
    disableVerifyBypass = checkKernelArgument("-ibtnovb");
    if (disableVerifyBypass)
        SYSLOG(MODULE_BTLFX, "VNOP_VERIFY EBADMSG bypass OFF (-ibtnovb)");
    KernelPatcher::RouteRequest csRoute("_cs_validate_page", patched_cs_validate_page, orig_cs_validate);
    // 1.9.2: 成功也打一行。1.9.1 只在失败时打，导致“到底挂上没有”只能靠推断，
    // 这次实测就是被这一点拖住的——日志里没有行，无法区分“没挂上”和“没触发”。
    if (patcher.routeMultipleLong(KernelPatcher::KernelID, &csRoute, 1))
        SYSLOG(MODULE_BTLFX, "cs_validate page hook armed");
    else
        SYSLOG(MODULE_BTLFX, "failed to route cs validation pages");
    // 两条路由分开提交：符号缺失只影响自己，不影响另一条（Tahoe 上 _VNOP_VERIFY
    // 解析失败 = 修复不生效，退回 1.9.0 行为，不会更糟）。
    KernelPatcher::RouteRequest verifyRoute("_VNOP_VERIFY", patched_vnop_verify, orig_vnop_verify);
    if (patcher.routeMultipleLong(KernelPatcher::KernelID, &verifyRoute, 1)) {
        verifyBypassArmed = true;
        SYSLOG(MODULE_BTLFX, "VNOP_VERIFY bypass armed (bluetoothd/BlueTool only, swallows only when our patched page is inside the failing range)");
    } else {
        SYSLOG(MODULE_BTLFX, "failed to route VNOP_VERIFY (crash fix INACTIVE - symbol missing)");
    }
}

#pragma mark - Lilu

static void pluginStart() {
    SYSLOG("ibt", "start");
    diagEnabled = checkKernelArgument("-ibtpdbg");
    if (diagEnabled)
        SYSLOG(MODULE_IBTP, "diagnostics ON (-ibtpdbg)");
    // 1.8.0: palanca de diagnostico. Si con -ibtleoff las conexiones dejan de fallar en
    // arranque en frio, el problema esta en la reescritura de eventos LE, no en el firmware.
    leRewriteDisabled = checkKernelArgument("-ibtleoff");
    if (leRewriteDisabled)
        SYSLOG(MODULE_IBTP, "[PATCH] LE event rewriting OFF (-ibtleoff)");
    // 1.9.0: por defecto se informa LE 1M, que es el PHY real del controlador; -ibtphy2m
    // recupera el 2M/2M del upstream para poder comparar.
    if (checkKernelArgument("-ibtphy2m")) {
        fakePhyValue = 0x02;
        SYSLOG(MODULE_IBTP, "fake PHY Update reports LE 2M (-ibtphy2m, legacy behaviour)");
    }
    lilu.onKextLoadForce(&kextIOUSBHost, 1,
        [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            processUSBHost(patcher, index, address, size);
        });
    lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
        processPatcher(patcher);
    });
}

// Argumentos de arranque. Se aceptan tambien los de los kexts originales.
static const char *bootargOff[] { "-ibtoff", "-ibtcompatoff", "-btlfxoff" };
static const char *bootargDebug[] { "-ibtdbg", "-ibtcompatdbg", "-btlfxdbg" };
static const char *bootargBeta[] { "-ibtbeta", "-ibtcompatbeta", "-btlfxbeta" };

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey,
    KernelVersion::Tahoe,
    pluginStart
};
