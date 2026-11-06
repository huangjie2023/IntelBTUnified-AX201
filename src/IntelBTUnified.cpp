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
//  1.9.4: 撤回 1.9.3 的“按记录页偏移门控吞错”，回到“路径 + EBADMSG 必定吞”。
//  1.9.3 把“失败区间里含我们改写过的 4K 页”当成吞错前提，等于又埋回 1.9.1 的坑：
//  cs_validate_page 只在页首次从盘 pagein 时被调用，页被改写并驻留进 file cache 之后
//  就不再经过它，所以记录集合天然不完整；一旦比对不中，吞错又变成“一次都不吞”。
//  页偏移比对降级为日志附注（inside / no recorded page），只用于根因归类，不阻断吞错。
//  注意：本机与两份独立案例的对照组均已排除介质/盘上数据——不命中只能说明“我们的
//  采样没覆盖到”，不能反推盘坏。
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
#include "driver/IbtDiag.h"

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
// 1.9.5: cs_validate 路由是否成功。以前只在日志里体现，而本机日志通道不可用
// （见 IbtDiag.h 开头），所以把它变成可被 ioreg 读到的状态位。
static bool csHookArmed = false;
// -ibtnovb: 关闭 VNOP_VERIFY 吞错（诊断开关）。
static bool disableVerifyBypass = false;
// 计数：命中（吞掉）次数、路径解析失败次数、非 EBADMSG 错误次数、“不是我们的页”次数。
static uint32_t verifySwallowed = 0;
static uint32_t verifyPathFailures = 0;
static uint32_t verifyOtherErrors = 0;
// 1.9.5: 最后一次被吞的区间，供 ioreg 读取。纯内存写，无分配、无 I/O。
static uint64_t lastVerifyOffset = 0;
static uint64_t lastVerifySize = 0;
static uint64_t lastVerifyPage = 0;

// 1.9.6: 「钩子到底在不在调用路径上」——这是不用等故障 A 复现就能判定的唯一办法。
//
// 问题：1.9.1~1.9.5 的吞错全都建立在「decmpfs 回填缺页后会调 VNOP_VERIFY」这个前提上，
// 但从来没有证据证明这句话在 Tahoe 上成立。armed=Yes 只说明符号路由成功，不说明它被调用。
// 若前提不成立，那么写多少判据都是空转，而结论只能靠"下次崩溃再看"来反证，成本极高。
//
// 做法：cs_validate 钩子里已经解析出了路径，判定 vp 属于 bluetoothd/BlueTool 时把 vnode
// 指针记进 btVnodes[]（≤16 个，普通指针，不做 retain，只在进程生命周期内有效）；
// VNOP_VERIFY 钩子每次进来先做 ≤16 次指针比较，命中就计数（**不论返回值**）。
// 于是：bluetoothd 启动并完成缺页之后
//   IBTVnopCalls > 0  → 前提成立，钩子确实被调用过，之后的 swallowed=0 是"没出错"；
//   IBTVnopCalls == 0 → 前提不成立，拦截面选错了，必须换钩点（decmpfs/APFS 一侧）。
// 这是"一次开机即可定性"的判据，不再需要撞运气。
static constexpr uint32_t kMaxBtVnodes = 16;
static vnode_t btVnodes[kMaxBtVnodes] {};
static uint32_t btVnodeCount = 0;
static uint32_t verifyCallsOnBt = 0;

// 1.9.7: 1.9.6 实机读数 IBTBtVnodes=2 / IBTPatchedPages=4 / IBTVnopCalls=0 ——
// cs_validate 认出了这两个文件、页也改进去了，但 _VNOP_VERIFY 一次都没被它们的校验
// 调用过。这说明「decmpfs 回填缺页后调 _VNOP_VERIFY」这个前提在 macOS 26 上不成立，
// 1.9.1~1.9.5 的吞错是死代码。为免再靠猜测换钩点，1.9.7 把候选入口全部"探针化"：
// 逐个尝试路由，成功的按位记进 symMask，并各自统计总调用数与命中 bt vnode 的次数。
// 所有新钩子都是**原样转发 + 计数**，不改变任何行为，一次开机就能看出真正的入口是哪个。
//   bit0 = _VNOP_VERIFY          （1.9.1~1.9.6 押的那个，实测 bt 命中 0）
//   bit1 = _vnode_verify         （VNOP_VERIFY 的内层实现，若调用者直接内联调用它）
//   bit2 = _decmpfs_pagein_compressed（缺页解压入口，理论上校验就在它内部/之后）
//   bit3 = _decmpfs_validate_compressed_file（整文件校验，唤醒时可能走这条）
static uint32_t symMask = 0;
static uint32_t verifyAllCalls = 0;
static uint32_t vnodeVerifyAll = 0;
static uint32_t vnodeVerifyOnBt = 0;
static uint32_t decmpfsAll = 0;
static uint32_t decmpfsOnBt = 0;
static uint32_t validateAll = 0;
static uint32_t validateOnBt = 0;

// 1.9.8: 1.9.7 读数（全 0 + SymMask=13）说明前面四个候选没有一个是入口，于是回头去内核
// 缓存里枚举真实符号名，挖到 APFS 自己那一层：
//   _apfs_vnop_verify               ← vnode 操作表里 vop_verify 表项的实现本体。表项里存的是
//                                     它的地址，所以改它的函数头，走表项的调用同样会落到我们
//                                     这里——这正是 _VNOP_VERIFY 那个导出包装接不住的原因。
//   _apfs_pagein_with_verification  ← 名字直说"带校验的 pagein"，压缩页缺页最可能的入口
//   _apfs_verify_uncompressed_data  ← 明文数据校验
//   _apfs_verify_rangelist          ← 区间校验
// 第一个带吞错（这才是可能生效的修复），后三个只计数（定位用）。
//   bit4 = _apfs_vnop_verify | bit5 = _apfs_pagein_with_verification
//   bit6 = _apfs_verify_uncompressed_data | bit7 = _apfs_verify_rangelist
static uint32_t apfsVerAll = 0;
static uint32_t apfsVerOnBt = 0;
static uint32_t apfsPiAll = 0;
static uint32_t apfsPiOnBt = 0;
static uint32_t apfsUncAll = 0;
static uint32_t apfsUncOnBt = 0;
static uint32_t apfsRlAll = 0;
static uint32_t apfsRlOnBt = 0;
static bool apfsVerifyArmed = false;
// 1.9.9: 1.9.8 实机 SymMask=13（只有 bit0/2/3），四条 APFS 路由全部失败、计数器全 0。
// 原因不是符号不存在（字符串枚举确认过），而是**查错了符号表**：
// APFS 是 kext，它的内部函数是局部符号（只被同文件的 vnode 操作表引用，不进内核导出表），
// 而 KernelPatcher::KernelID 走的是内核导出符号表。Lilu 的正规做法是先用 bundle id 取到
// 该 kext 的 kinfo（loadKinfo → prelink/KC 里按 id 找），再用这个 id 解析符号与路由。
// 记录两件事：bit8 = APFS kinfo 是否拿到；apfsSolveMask = 四个符号各自是否解析成功
// （这样才能区分“kext 没找到”“符号没找到”“符号找到但路由失败”三种失败）。
static uint32_t apfsSolveMask = 0;

static inline bool isBtVnode(vnode_t vp) {
    uint32_t n = __atomic_load_n(&btVnodeCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < kMaxBtVnodes; i++)
        if (__atomic_load_n(&btVnodes[i], __ATOMIC_RELAXED) == vp)
            return true;
    return false;
}

static inline void rememberBtVnode(vnode_t vp) {
    if (vp == nullptr || isBtVnode(vp))
        return;
    uint32_t slot = __atomic_fetch_add(&btVnodeCount, 1, __ATOMIC_ACQ_REL);
    if (slot < kMaxBtVnodes)
        __atomic_store_n(&btVnodes[slot], vp, __ATOMIC_RELEASE);
}
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
            rememberBtVnode(vp);
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
            rememberBtVnode(vp);
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
// 1.9.4 的判据（按顺序短路，正常情况下每条热路径只多一次返回值比较）：
//   1. 返回非 0 且 buf != NULL（是对实际数据的校验，不是块大小查询）；
//   2. 钩子确实 armed（-ibtota 下钩子根本不会安装，这道只是兜底）；
//   3. vn_getpath 精确匹配 /usr/sbin/bluetoothd 或 /usr/sbin/BlueTool
//      ——系统其余文件的校验一丝不减，其它错误也照原样返回；
//   4. 错误恰为 EBADMSG。
// 页偏移比对（patchedPages[] 是否落在 [offset, offset+bufsize)）**只写日志**，
// 不参与判定。原因：cs_validate_page 只在页首次从盘 pagein 时调用，页被改写并驻留
// 进 file cache 之后不再经过它，记录集合必然不完整；拿它当门控就会重演 1.9.1/1.9.3
// “一次都不吞”的结局。
//
// 关于“会不会误吞真损坏”：本机与两份独立案例的对照组（APFS 卷校验、SMART、全量
// pread、冷扫描 os.dmg、故障后立刻 mmap 复扫）全部通过，说明盘上字节是好的；
// 系统卷又有 SSV 每次开机加密校验兜底。所以这条路径上把 EBADMSG 当作“内存态混合
// 内容”处理是站得住的，日志里的 inside/no-recorded-page 只是用来做根因归类。
//
// 吞掉后 decmpfs 走正常 commit 路径，回填的页内容是解压出的原始字节，正确。
//
// 1.9.6 只做两件事，都不改上面的判定语义：
//   1. 加 verifyCallsOnBt 计数：每进钩子就按 vnode 身份统计，**不看返回值**。它的作用是
//      把"钩子在不在调用路径上"从猜测变成一次开机可读的数字（判据见 btVnodes 处的注释）。
//   2. 定域顺序改为「路径优先、身份兜底」：路径解析失败时退回 vnode 身份，避免
//      1.9.4 里"vn_getpath 失败 → 不吞 → 照样崩"的死角；路径成功但不像目标文件的，
//      仍然放行（防止 vnode 地址被回收后误吞别的文件的真损坏）。
// 1.9.8: 把"这次 EBADMSG 到底吞不吞"从具体钩子里抽出来，_VNOP_VERIFY（历史钩子，实测
// 是死符号）和 _apfs_vnop_verify（表项实现）共用同一份判定，避免两处逻辑漂移。
// 判定仍是确定性的三条：钩子已装（由调用方检查）+ 定域命中 + err == EBADMSG。
// offset/bufsize 只作日志与快照附注，不参与决定——这是 1.9.3 用页偏移当门控失败的教训。
static int handleBtVerifyError(vnode_t vp, off_t offset, size_t bufsize, int err, bool onBt) {
    // 定域：路径优先（能挡住 vnode 地址被回收后误判），路径解析失败时退回身份判定。
    // 1.9.4 在这里是"路径失败 → 不吞 → 照样崩"，等于给修复留了一个必然失效的角落。
    char path[PATH_MAX] = {};
    int pathlen = PATH_MAX;
    bool scoped = false;
    if (vn_getpath(vp, path, &pathlen) == 0)
        scoped = strncmp(path, "/usr/sbin/", kSbinDirLength) == 0 &&
                 (strcmp(path + kSbinDirLength, "bluetoothd") == 0 || strcmp(path + kSbinDirLength, "BlueTool") == 0);
    else
        scoped = onBt;   // 解析不出来但身份确认过，仍然定域

    if (!scoped) {
        if (path[0] == 0)
            __atomic_fetch_add(&verifyPathFailures, 1, __ATOMIC_RELAXED);
        return err;
    }

    if (err != EBADMSG) {
        if (__atomic_fetch_add(&verifyOtherErrors, 1, __ATOMIC_RELAXED) == 0)
            SYSLOG(MODULE_BTLFX, "verify returned %d on %s (not EBADMSG, propagated)", err, path[0] ? path : "<bt vnode>");
        return err;
    }

    // 页偏移比对只作为**日志附注**，不再参与“吞不吞”的决定。
    // 1.9.3 把它当门控，结果又退回到 1.9.1 那类“我们记的页不全 → 一次都不吞”的坑：
    // cs_validate_page 只在页首次从盘 pagein 时被调用，页一旦被改写并驻留进 file cache，
    // 后续都不会再走这条路径，patchedPages[] 完全可能不包含真正被污染的那一页。
    uint64_t oursAt = 0;
    uint32_t recorded = __atomic_load_n(&patchedPageCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < recorded && i < kMaxPatchedPages; i++) {
        uint64_t p = __atomic_load_n(&patchedPages[i], __ATOMIC_ACQUIRE);
        if (p != 0 && p >= (uint64_t)offset && p < (uint64_t)offset + bufsize) {
            oursAt = p;
            break;
        }
    }

    uint32_t n = __atomic_add_fetch(&verifySwallowed, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&lastVerifyOffset, (uint64_t)offset, __ATOMIC_RELAXED);
    __atomic_store_n(&lastVerifySize, (uint64_t)bufsize, __ATOMIC_RELAXED);
    __atomic_store_n(&lastVerifyPage, oursAt, __ATOMIC_RELAXED);
    // 日志拿不到，改把计数推给 IORegistry。每 boot 最多 8 次，代价可忽略。
    if (n <= 8) {
        ibtDiagPublishLive();
        const char *who = path[0] ? path : "<bt vnode>";
        if (oursAt)
            SYSLOG(MODULE_BTLFX, "swallowed verify mismatch #%u on %s @ 0x%llx (size %zu): patched page 0x%llx inside this group (%u pages recorded)",
                   n, who, (unsigned long long)offset, bufsize, (unsigned long long)oursAt, recorded);
        else
            SYSLOG(MODULE_BTLFX, "swallowed verify mismatch #%u on %s @ 0x%llx (size %zu): no recorded patched page in this group (%u recorded) - diagnostic only",
                   n, who, (unsigned long long)offset, bufsize, recorded);
    }
    return 0;
}

static mach_vm_address_t orig_vnop_verify {};

static int patched_vnop_verify(vnode_t vp, off_t offset, uint8_t *buf, size_t bufsize, size_t *verifyblksize, void **verifyctxp, uint32_t flags, void *ctx, void *verifykind) {
    int err = FunctionCast(patched_vnop_verify, orig_vnop_verify)(vp, offset, buf, bufsize, verifyblksize, verifyctxp, flags, ctx, verifykind);
    // 1.9.7: 总调用数（不看 vnode）。用来区分两种 0：
    //   总数为 0 → 这个符号在本内核里已经没人调用（死符号），钩它毫无意义；
    //   总数很大而 bt 命中为 0 → 符号活着，但 bluetoothd 的校验不走它。
    __atomic_fetch_add(&verifyAllCalls, 1, __ATOMIC_RELAXED);
    if (vp == nullptr || disableVerifyBypass || !verifyBypassArmed)
        return err;

    // 1.9.6: 先做身份判定（≤16 次指针比较，无分配、无 I/O、可在缺页上下文安全调用）。
    // 无论返回值都要计数——IBTVnopCalls 就是"钩子是否真的被调用"的唯一证据。
    bool onBt = isBtVnode(vp);
    if (onBt)
        __atomic_fetch_add(&verifyCallsOnBt, 1, __ATOMIC_RELAXED);

    if (err == 0 || buf == nullptr)
        return err;

    // 1.9.8: 判定逻辑抽到 handleBtVerifyError，与 _apfs_vnop_verify 共用同一份。
    return handleBtVerifyError(vp, offset, bufsize, err, onBt);
}

// ---------------------------------------------------------------------------
// 1.9.7 探针钩子：原样转发 + 计数，**不改任何行为**。
//
// 统一用 9 个参数槽位声明：x86_64 SysV 下前 6 个走寄存器、后 3 个走栈，只要"接收时
// 用同一套 9 槽、转发时也用同一套 9 槽"，无论真实函数实际收几个参数，字节都是原样
// 传下去的（多出来的寄存器/栈槽被被调用者忽略）。因此不需要精确知道这些函数原型，
// 也能安全地做 pass-through —— 前提只有一个：第 1 个参数是 vnode（点或空指针都安全，
// 因为我们只做指针比较，不解引用）。
static mach_vm_address_t orig_vnode_verify {};
static mach_vm_address_t orig_decmpfs_pagein {};
static mach_vm_address_t orig_decmpfs_validate {};

static int patched_vnode_verify(vnode_t vp, off_t offset, uint8_t *buf, size_t bufsize, size_t *verifyblksize, void **verifyctxp, uint32_t flags, void *ctx, void *verifykind) {
    __atomic_fetch_add(&vnodeVerifyAll, 1, __ATOMIC_RELAXED);
    if (vp != nullptr && isBtVnode(vp))
        __atomic_fetch_add(&vnodeVerifyOnBt, 1, __ATOMIC_RELAXED);
    return FunctionCast(patched_vnode_verify, orig_vnode_verify)(vp, offset, buf, bufsize, verifyblksize, verifyctxp, flags, ctx, verifykind);
}

static int patched_decmpfs_pagein(vnode_t vp, off_t offset, uint8_t *buf, size_t bufsize, size_t *a, void **b, uint32_t c, void *d, void *e) {
    __atomic_fetch_add(&decmpfsAll, 1, __ATOMIC_RELAXED);
    if (vp != nullptr && isBtVnode(vp))
        __atomic_fetch_add(&decmpfsOnBt, 1, __ATOMIC_RELAXED);
    return FunctionCast(patched_decmpfs_pagein, orig_decmpfs_pagein)(vp, offset, buf, bufsize, a, b, c, d, e);
}

// 第三个候选独立一份 orig/计数：绝不能和上面共用 orig 指针——第二次路由会把它覆盖成
// 自己的原函数地址，先装的钩子就会转发到错误的函数里。
static int patched_decmpfs_validate(vnode_t vp, off_t offset, uint8_t *buf, size_t bufsize, size_t *a, void **b, uint32_t c, void *d, void *e) {
    __atomic_fetch_add(&validateAll, 1, __ATOMIC_RELAXED);
    if (vp != nullptr && isBtVnode(vp))
        __atomic_fetch_add(&validateOnBt, 1, __ATOMIC_RELAXED);
    return FunctionCast(patched_decmpfs_validate, orig_decmpfs_validate)(vp, offset, buf, bufsize, a, b, c, d, e);
}

// ---------------------------------------------------------------------------
// 1.9.8 APFS 侧钩子。
//
// _apfs_vnop_verify 是**唯一带吞错**的新钩子。它和前面那些"导出包装"的区别在于：
// 调用方读 vnode 操作表拿到的是这个函数的地址，所以改它的函数头就等于接住了所有
// 走表项的校验调用——_VNOP_VERIFY 那个导出包装之所以 0 次调用，正是因为没人经过它。
// 第二个参数按 off_t 收、原样传回；3~9 槽用 void* 只做字节透传，不解引用。
// bufsize 未知时传 0，此时页偏移比对退化成"只记 offset"，不影响吞不吞的决定。
static mach_vm_address_t orig_apfs_vnop_verify {};

static int patched_apfs_vnop_verify(vnode_t vp, off_t offset, void *a3, void *a4, void *a5, void *a6, void *a7, void *a8, void *a9) {
    int err = FunctionCast(patched_apfs_vnop_verify, orig_apfs_vnop_verify)(vp, offset, a3, a4, a5, a6, a7, a8, a9);
    __atomic_fetch_add(&apfsVerAll, 1, __ATOMIC_RELAXED);
    if (vp == nullptr || disableVerifyBypass || !apfsVerifyArmed)
        return err;

    bool onBt = isBtVnode(vp);
    if (onBt)
        __atomic_fetch_add(&apfsVerOnBt, 1, __ATOMIC_RELAXED);
    if (err == 0)
        return err;

    return handleBtVerifyError(vp, offset, 0, err, onBt);
}

// 下面三个只计数，不改变行为——它们是"如果不是 vop_verify，那到底是哪个"的定位探针。
// 每个都必须有自己独立的 orig 指针，共用会被后一次路由覆盖，先装的钩子转发进错函数。
static mach_vm_address_t orig_apfs_pagein_verif {};

static int patched_apfs_pagein_verif(vnode_t vp, off_t offset, void *a3, void *a4, void *a5, void *a6, void *a7, void *a8, void *a9) {
    __atomic_fetch_add(&apfsPiAll, 1, __ATOMIC_RELAXED);
    if (vp != nullptr && isBtVnode(vp))
        __atomic_fetch_add(&apfsPiOnBt, 1, __ATOMIC_RELAXED);
    return FunctionCast(patched_apfs_pagein_verif, orig_apfs_pagein_verif)(vp, offset, a3, a4, a5, a6, a7, a8, a9);
}

static mach_vm_address_t orig_apfs_verify_unc {};

static int patched_apfs_verify_unc(vnode_t vp, off_t offset, void *a3, void *a4, void *a5, void *a6, void *a7, void *a8, void *a9) {
    __atomic_fetch_add(&apfsUncAll, 1, __ATOMIC_RELAXED);
    if (vp != nullptr && isBtVnode(vp))
        __atomic_fetch_add(&apfsUncOnBt, 1, __ATOMIC_RELAXED);
    return FunctionCast(patched_apfs_verify_unc, orig_apfs_verify_unc)(vp, offset, a3, a4, a5, a6, a7, a8, a9);
}

static mach_vm_address_t orig_apfs_verify_rl {};

static int patched_apfs_verify_rl(vnode_t vp, off_t offset, void *a3, void *a4, void *a5, void *a6, void *a7, void *a8, void *a9) {
    __atomic_fetch_add(&apfsRlAll, 1, __ATOMIC_RELAXED);
    if (vp != nullptr && isBtVnode(vp))
        __atomic_fetch_add(&apfsRlOnBt, 1, __ATOMIC_RELAXED);
    return FunctionCast(patched_apfs_verify_rl, orig_apfs_verify_rl)(vp, offset, a3, a4, a5, a6, a7, a8, a9);
}

static void processPatcher(KernelPatcher &patcher) {    const bool argOta    = checkKernelArgument("-ibtota");
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
    if (patcher.routeMultipleLong(KernelPatcher::KernelID, &csRoute, 1)) {
        csHookArmed = true;
        SYSLOG(MODULE_BTLFX, "cs_validate page hook armed");
    } else
        SYSLOG(MODULE_BTLFX, "failed to route cs validation pages");
    // 两条路由分开提交：符号缺失只影响自己，不影响另一条（Tahoe 上 _VNOP_VERIFY
    // 解析失败 = 修复不生效，退回 1.9.0 行为，不会更糟）。
    KernelPatcher::RouteRequest verifyRoute("_VNOP_VERIFY", patched_vnop_verify, orig_vnop_verify);
    if (patcher.routeMultipleLong(KernelPatcher::KernelID, &verifyRoute, 1)) {
        verifyBypassArmed = true;
        __atomic_fetch_or(&symMask, 1u, __ATOMIC_RELAXED);
        SYSLOG(MODULE_BTLFX, "VNOP_VERIFY bypass armed (bluetoothd/BlueTool only, any EBADMSG will be swallowed)");
    } else {
        SYSLOG(MODULE_BTLFX, "failed to route VNOP_VERIFY (crash fix INACTIVE - symbol missing)");
    }

    // 1.9.7 探针：只计数、不改行为。成功与否按位记进 symMask，读数是
    //   IBTSymMask & 1 / 2 / 4 / 8 分别对应下面四条 route 是否成功。
    // 逐个单独提交，任何一个失败都不影响其它（符号可能被内联或根本不存在）。
    KernelPatcher::RouteRequest vnodeVerifyRoute("_vnode_verify", patched_vnode_verify, orig_vnode_verify);
    if (patcher.routeMultipleLong(KernelPatcher::KernelID, &vnodeVerifyRoute, 1))
        __atomic_fetch_or(&symMask, 2u, __ATOMIC_RELAXED);

    KernelPatcher::RouteRequest decmpfsRoute("_decmpfs_pagein_compressed", patched_decmpfs_pagein, orig_decmpfs_pagein);
    if (patcher.routeMultipleLong(KernelPatcher::KernelID, &decmpfsRoute, 1))
        __atomic_fetch_or(&symMask, 4u, __ATOMIC_RELAXED);

    KernelPatcher::RouteRequest decmpfsValidateRoute("_decmpfs_validate_compressed_file", patched_decmpfs_validate, orig_decmpfs_validate);
    if (patcher.routeMultipleLong(KernelPatcher::KernelID, &decmpfsValidateRoute, 1))
        __atomic_fetch_or(&symMask, 8u, __ATOMIC_RELAXED);

    // 1.9.9: 改用 APFS 自己的 kinfo id 解析符号。1.9.8 用 KernelID 试这四条全部失败
    // （SymMask 没有 bit4~7），而符号确实存在于内核缓存里——差别只在"哪个符号表"。
    // bit8 记录是否拿到 APFS kinfo；IBTApfsSolve 的 0..3 位记录四个符号各自是否解析成功。
    static const char *apfsPaths[] { "/System/Library/Extensions/AppleAPFS.kext" };
    patcher.clearError();
    size_t apfsId = patcher.loadKinfo("com.apple.filesystems.apfs", apfsPaths, arrsize(apfsPaths), false);
    bool apfsKext = apfsId < 32;   // INVALID 是 SIZE_MAX，任何真实下标都远小于 32
    if (apfsKext)
        __atomic_fetch_or(&symMask, 0x100u, __ATOMIC_RELAXED);
    size_t apfsSymId = apfsKext ? apfsId : KernelPatcher::KernelID;

    const char *apfsSyms[4] {
        "_apfs_vnop_verify",
        "_apfs_pagein_with_verification",
        "_apfs_verify_uncompressed_data",
        "_apfs_verify_rangelist",
    };
    for (size_t i = 0; i < arrsize(apfsSyms); i++)
        if (patcher.solveSymbol(apfsSymId, apfsSyms[i]))
            __atomic_fetch_or(&apfsSolveMask, 1u << i, __ATOMIC_RELAXED);

    // 路由统一走一个小包装：先按 APFS kinfo 的 id 试，失败再退回 KernelID（1.9.8 的路径）。
    // 两条都失败才算真失败，这样 bit4~7 的含义保持"这条钩子到底装上没有"。
    auto routeApfs = [&](KernelPatcher::RouteRequest &req) -> bool {
        if (patcher.routeMultipleLong(apfsSymId, &req, 1))
            return true;
        if (apfsKext && apfsSymId != KernelPatcher::KernelID)
            return patcher.routeMultipleLong(KernelPatcher::KernelID, &req, 1);
        return false;
    };

    KernelPatcher::RouteRequest apfsVerifyRoute("_apfs_vnop_verify", patched_apfs_vnop_verify, orig_apfs_vnop_verify);
    if (routeApfs(apfsVerifyRoute)) {
        apfsVerifyArmed = true;
        __atomic_fetch_or(&symMask, 0x10u, __ATOMIC_RELAXED);
        SYSLOG(MODULE_BTLFX, "apfs_vnop_verify bypass armed (vnode op table entry, bluetoothd/BlueTool only)");
    } else {
        SYSLOG(MODULE_BTLFX, "failed to route apfs_vnop_verify (fallback to _VNOP_VERIFY only)");
    }

    KernelPatcher::RouteRequest apfsPageinRoute("_apfs_pagein_with_verification", patched_apfs_pagein_verif, orig_apfs_pagein_verif);
    if (routeApfs(apfsPageinRoute))
        __atomic_fetch_or(&symMask, 0x20u, __ATOMIC_RELAXED);

    KernelPatcher::RouteRequest apfsUncRoute("_apfs_verify_uncompressed_data", patched_apfs_verify_unc, orig_apfs_verify_unc);
    if (routeApfs(apfsUncRoute))
        __atomic_fetch_or(&symMask, 0x40u, __ATOMIC_RELAXED);

    KernelPatcher::RouteRequest apfsRlRoute("_apfs_verify_rangelist", patched_apfs_verify_rl, orig_apfs_verify_rl);
    if (routeApfs(apfsRlRoute))
        __atomic_fetch_or(&symMask, 0x80u, __ATOMIC_RELAXED);
}

#pragma mark - Lilu

// 1.9.5: 诊断快照。唯一的目的是让“钩子到底挂上没有、吞了几次”变成
// ioreg 能读到的数字——本机日志通道不可用（IOLog 不落统一日志、msgbuf 被
// socket 噪声刷掉），1.9.1~1.9.4 全靠猜，白跑了四轮。
void ibtDiagFill(IbtDiagSnapshot *out) {
    if (!out)
        return;
    out->csHookArmed  = csHookArmed ? 1 : 0;
    out->vnopArmed    = verifyBypassArmed ? 1 : 0;
    out->mode         = disableCsPatching ? 2u : (legacyCsPatching ? 1u : 0u);
    out->novb         = disableVerifyBypass ? 1 : 0;
    out->swallowed    = __atomic_load_n(&verifySwallowed, __ATOMIC_RELAXED);
    out->pathFailures = __atomic_load_n(&verifyPathFailures, __ATOMIC_RELAXED);
    out->otherErrors  = __atomic_load_n(&verifyOtherErrors, __ATOMIC_RELAXED);
    out->callsOnBt    = __atomic_load_n(&verifyCallsOnBt, __ATOMIC_RELAXED);
    out->btVnodes     = __atomic_load_n(&btVnodeCount, __ATOMIC_RELAXED);
    out->symMask      = __atomic_load_n(&symMask, __ATOMIC_RELAXED);
    out->vnopAll      = __atomic_load_n(&verifyAllCalls, __ATOMIC_RELAXED);
    out->kvAll        = __atomic_load_n(&vnodeVerifyAll, __ATOMIC_RELAXED);
    out->kvOnBt       = __atomic_load_n(&vnodeVerifyOnBt, __ATOMIC_RELAXED);
    out->decAll       = __atomic_load_n(&decmpfsAll, __ATOMIC_RELAXED);
    out->decOnBt      = __atomic_load_n(&decmpfsOnBt, __ATOMIC_RELAXED);
    out->valAll       = __atomic_load_n(&validateAll, __ATOMIC_RELAXED);
    out->valOnBt      = __atomic_load_n(&validateOnBt, __ATOMIC_RELAXED);
    out->apfsVerAll   = __atomic_load_n(&apfsVerAll, __ATOMIC_RELAXED);
    out->apfsVerOnBt  = __atomic_load_n(&apfsVerOnBt, __ATOMIC_RELAXED);
    out->apfsPiAll    = __atomic_load_n(&apfsPiAll, __ATOMIC_RELAXED);
    out->apfsPiOnBt   = __atomic_load_n(&apfsPiOnBt, __ATOMIC_RELAXED);
    out->apfsUncAll   = __atomic_load_n(&apfsUncAll, __ATOMIC_RELAXED);
    out->apfsUncOnBt  = __atomic_load_n(&apfsUncOnBt, __ATOMIC_RELAXED);
    out->apfsRlAll    = __atomic_load_n(&apfsRlAll, __ATOMIC_RELAXED);
    out->apfsRlOnBt   = __atomic_load_n(&apfsRlOnBt, __ATOMIC_RELAXED);
    out->apfsSolve    = __atomic_load_n(&apfsSolveMask, __ATOMIC_RELAXED);
    out->pmEvents     = 0; // 由驱动层覆盖
    out->patchedPages = __atomic_load_n(&patchedPageCount, __ATOMIC_RELAXED);
    out->lastOffset   = __atomic_load_n(&lastVerifyOffset, __ATOMIC_RELAXED);
    out->lastSize     = __atomic_load_n(&lastVerifySize, __ATOMIC_RELAXED);
    out->lastPage     = __atomic_load_n(&lastVerifyPage, __ATOMIC_RELAXED);
}

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
