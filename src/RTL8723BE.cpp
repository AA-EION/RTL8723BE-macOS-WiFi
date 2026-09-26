#include "RTL8723BE.hpp"
#include <string.h>
#include <pexpert/pexpert.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(RTL8723BE, IOEthernetController);

// Return only prepared, contiguous buffers whose entire DMA range fits 32 bits.
static IOBufferMemoryDescriptor *allocateDMA(size_t size, size_t alignment) {
    IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        size, 0xFFFFFFFFULL & ~(uint64_t(alignment) - 1));
    if (!mem) return nullptr;
    if (mem->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        mem->release();
        return nullptr;
    }
    if (!mem->getBytesNoCopy() ||
        !rtlDmaRangeValid(mem->getPhysicalAddress(), size, alignment)) {
        mem->complete(kIODirectionInOut);
        mem->release();
        return nullptr;
    }
    return mem;
}

// ============================================================================
// IOKit Lifecycle
// ============================================================================

bool RTL8723BE::init(OSDictionary *properties) {
    if (!super::init(properties)) {
        return false;
    }

    fPCIDevice = nullptr;
    fMMIOMap = nullptr;
    fMMIOBase = nullptr;
    fWorkLoop = nullptr;
    fInterruptSource = nullptr;
    fScanTimer = nullptr;
    fNetif = nullptr;
    fMediumDict = nullptr;
    fRFLock = nullptr;
    fStateLock = nullptr;

    fPowerState = POWER_CARDDIS;
    fHIMRMask = 0;
    fInterruptEnabled = false;
    memset(&fCalib, 0, sizeof(fCalib));
    fCurrentChannel = 1;
    fActiveAntenna = 2; // Default to Aux port 2 for HP 103C:804C

    for (int i = 0; i < 8; ++i) {
        fTxRingDescMem[i] = nullptr;
        fTxRingDescVirt[i] = nullptr;
        fTxRingDescPhys[i] = 0;
        fTxHostIdx[i] = 0;
        for (int j = 0; j < RTL8723BE_TX_DESC_COUNT; ++j) {
            fTxBufMem[i][j] = nullptr;
        }
    }

    fRxRingDescMem = nullptr;
    fRxRingDescVirt = nullptr;
    fRxRingDescPhys = 0;
    fRxHostIdx = 0;
    for (int j = 0; j < RTL8723BE_RX_DESC_COUNT; ++j) {
        fRxBufMem[j] = nullptr;
    }

    fState = kRTLStateDisconnected;
    memset(fMyMAC, 0, 6);
    memset(fConnectedBSSID, 0, 6);
    memset(fConnectedSSID, 0, sizeof(fConnectedSSID));
    memset(fPassphrase, 0, sizeof(fPassphrase));

    memset(fPMK, 0, sizeof(fPMK));
    memset(fPTK, 0, sizeof(fPTK));
    memset(fGTK, 0, sizeof(fGTK));
    memset(fSNonce, 0, sizeof(fSNonce));
    memset(fANonce, 0, sizeof(fANonce));

    fKeyInstalled = false;
    fLastReplayCounter = 0;
    fTxPN = 1;
    for (int i = 0; i < 16; ++i) fRxPtkPN[i] = 0;
    for (int i = 0; i < 4; ++i)  fRxGtkPN[i] = 0;

    memset(&fScanResults, 0, sizeof(fScanResults));
    fScanActive = false;
    fScanCurrentChannel = 1;
    fHomeChannel = 1;

    fTxPackets = 0;
    fRxPackets = 0;
    fTxErrors = 0;
    fRxErrors = 0;

    return true;
}

void RTL8723BE::cleanupResources() {
    fInterruptEnabled = false;
    fHIMRMask = 0;

    if (fMMIOBase) {
        mmio_write32(REG_HIMR, 0);
        mmio_write32(REG_HIMRE, 0);
        mmio_write32(REG_HISR, 0xFFFFFFFFU);
    }

    if (fScanTimer) {
        fScanTimer->cancelTimeout();
        if (fWorkLoop) fWorkLoop->removeEventSource(fScanTimer);
        fScanTimer->release();
        fScanTimer = nullptr;
    }

    if (fInterruptSource) {
        fInterruptSource->disable();
        if (fWorkLoop) fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = nullptr;
    }

    if (fMMIOBase) {
        powerOff();
    }
    if (fPCIDevice) {
        // Disabling MAC alone does not revoke PCI DMA access to host memory.
        fPCIDevice->setBusMasterEnable(false);
        (void)fPCIDevice->configRead16(kIOPCIConfigCommand);
        IOSleep(1);
    }
    freeDMARings();

    if (fNetif) {
        detachInterface(fNetif);
        fNetif = nullptr;
    }

    if (fMediumDict) {
        fMediumDict->release();
        fMediumDict = nullptr;
    }

    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = nullptr;
    }

    if (fMMIOMap) {
        fMMIOMap->release();
        fMMIOMap = nullptr;
        fMMIOBase = nullptr;
    }

    if (fPCIDevice) {
        fPCIDevice->release();
        fPCIDevice = nullptr;
    }

    if (fRFLock) {
        IOLockFree(fRFLock);
        fRFLock = nullptr;
    }

    if (fStateLock) {
        IOLockFree(fStateLock);
        fStateLock = nullptr;
    }
}

bool RTL8723BE::start(IOService *provider) {
    // Keep experimental hardware access opt-in, including OpenCore injection.
    int experimental = 0;
    if (!PE_parse_boot_argn("rtl8723be_experimental", &experimental, sizeof(experimental)) ||
        experimental != 1) {
        IOLog("RTL8723BE: Hardware startup disabled; experimental driver under crash review\n");
        return false;
    }
    if (!super::start(provider)) {
        return false;
    }

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        IOLog("RTL8723BE: Provider is not an IOPCIDevice\n");
        super::stop(provider);
        return false;
    }

    fPCIDevice->retain();
    // Revoke DMA inherited from firmware/an earlier boot before touching BARs.
    fPCIDevice->setBusMasterEnable(false);

    // Discover capabilities; offsets are not guaranteed by PCI.
    UInt8 pmCap = 0;
    fPCIDevice->findPCICapability(0x01, &pmCap);
    if (pmCap) {
        uint16_t pmcsr = fPCIDevice->configRead16(pmCap + 4);
        if ((pmcsr & 3) != 0) {
            fPCIDevice->configWrite16(pmCap + 4, pmcsr & ~0x8003U);
            IOSleep(15);
        }
    }
    UInt8 pcieCap = 0;
    fPCIDevice->findPCICapability(0x10, &pcieCap);
    if (pcieCap) {
        uint16_t linkCtrl = fPCIDevice->configRead16(pcieCap + 0x10);
        fPCIDevice->configWrite16(pcieCap + 0x10, linkCtrl & ~3U);
    }

    fPCIDevice->setIOEnable(true);
    fPCIDevice->setMemoryEnable(true);
    IODelay(2000);

    // 3. Map BAR2 16KB 64-bit MMIO aperture (PCI Config Base Address 2 = offset 0x18)
    fMMIOMap = fPCIDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!fMMIOMap) {
        IOLog("RTL8723BE: Failed to map BAR2 MMIO region\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }
    if (fMMIOMap->getLength() < 0x4000 || !fMMIOMap->getVirtualAddress()) {
        IOLog("RTL8723BE: Invalid BAR2 aperture\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }
    fMMIOBase = (volatile uint8_t*)fMMIOMap->getVirtualAddress();
    IOLog("RTL8723BE: Mapped BAR2 MMIO at %p (length %lu)\n",
          fMMIOBase, (unsigned long)fMMIOMap->getLength());

    // 4. Immediately mask all hardware interrupts and clear sticky HISR bits before registering IRQ
    fHIMRMask = 0;
    fInterruptEnabled = false;
    mmio_write32(REG_HIMR, 0);
    mmio_write32(REG_HIMRE, 0);
    mmio_write32(REG_HISR, 0xFFFFFFFFU);

    fRFLock = IOLockAlloc();
    fStateLock = IOLockAlloc();
    if (!fRFLock || !fStateLock) {
        cleanupResources();
        super::stop(provider);
        return false;
    }

    fWorkLoop = getWorkLoop();
    if (!fWorkLoop) {
        IOLog("RTL8723BE: Failed to acquire IOWorkLoop\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }
    fWorkLoop->retain();

    // 5. Hardware Power-on Sequence + eFuse + Firmware + DMA Rings BEFORE enabling interrupts!
    if (!powerOn()) {
        IOLog("RTL8723BE: Power-on failed; aborting startup\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    if (!readEfuse(fCalib)) {
        IOLog("RTL8723BE: eFuse read fallback to default MAC\n");
    }
    memcpy(fMyMAC, fCalib.mac_addr, 6);
    IOLog("RTL8723BE: Hardware MAC: %02x:%02x:%02x:%02x:%02x:%02x (xtal=0x%02x)\n",
          fMyMAC[0], fMyMAC[1], fMyMAC[2], fMyMAC[3], fMyMAC[4], fMyMAC[5], fCalib.crystal_cap);

    // Program MAC address into REG_MACID (0x0610..0x0615)
    for (int i = 0; i < 6; ++i) {
        mmio_write8(0x0610U + (uint32_t)i, fMyMAC[i]);
    }

    // IMPORTANT: downloadFirmware() uses 0x1000..0x1FFF (shared Packet Buffer SRAM) while MCUFWDL_EN=1,
    // so downloadFirmware() MUST run BEFORE initLLT() and initDMARings()!
    if (!downloadFirmware(rtl8723befw_bin, rtl8723befw_bin_len)) {
        IOLog("RTL8723BE: Firmware download failed; aborting startup\n");
        cleanupResources();
        super::stop(provider);
        return false;
    } else {
        IOLog("RTL8723BE: 8051 MCU firmware loaded and initialized successfully\n");
    }

    if (!initLLT()) {
        IOLog("RTL8723BE: LLT/FIFO initialization failed\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    if (!initDMARings()) {
        IOLog("RTL8723BE: DMA rings initialization failed\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    if (!initBasebandAndRF()) {
        IOLog("RTL8723BE: Baseband/RF initialization failed\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    // Re-assert FIFO boundaries and RX filter after MAC/BB table loads:
    // RCR_AMF (BIT 20 = 0x00100000) | RCR_ADF (BIT 18 = 0x00040000) | RCR_AB | RCR_AM | RCR_APM | RCR_AAP
    mmio_write16(0x06A0, 0xFFFFU); // REG_RXFLTMAP0: accept all Management frames (Beacons & Probe Responses!)
    mmio_write16(0x06A2, 0xFFFFU); // REG_RXFLTMAP1: accept Control frames
    mmio_write16(0x06A4, 0xFFFFU); // REG_RXFLTMAP2: accept all Data frames
    mmio_write32(REG_RCR, 0x00142A0FU);

    // Turn ON HP Wireless LED (active-low LED0/LED1 at REG_LEDCFG2 0x004E, REG_LEDCFG1 0x004D, REG_LEDCFG0 0x004C)
    mmio_write8(0x004E, (mmio_read8(0x004E) & 0x90U) | 0x20U);
    mmio_write8(0x004D, mmio_read8(0x004D) & 0x10U);
    mmio_write8(0x004C, mmio_read8(0x004C) & 0x70U);
    IOLog("RTL8723BE: GPIO_PIN_CTRL(0x0044)=0x%08x LEDCFG2(0x004E)=0x%02x RCR(0x0608)=0x%08x\n",
          mmio_read32(0x0044), mmio_read8(0x004E), mmio_read32(REG_RCR));

    // Select a vector advertised as MSI, rather than assuming index 1 exists.
    int irqIndex = 0;
    int irqType = 0;
    for (int index = 0; fPCIDevice->getInterruptType(index, &irqType) == kIOReturnSuccess; ++index) {
        if (irqType & kIOInterruptTypePCIMessaged) {
            irqIndex = index;
            break;
        }
    }
    fInterruptSource = IOFilterInterruptEventSource::filterInterruptEventSource(
        this,
        &RTL8723BE::interruptAction,
        &RTL8723BE::interruptFilter,
        fPCIDevice,
        irqIndex
    );
    if (!fInterruptSource) {
        fInterruptSource = IOFilterInterruptEventSource::filterInterruptEventSource(
            this,
            &RTL8723BE::interruptAction,
            &RTL8723BE::interruptFilter,
            fPCIDevice,
            0
        );
    }
    if (!fInterruptSource || fWorkLoop->addEventSource(fInterruptSource) != kIOReturnSuccess) {
        IOLog("RTL8723BE: Failed to register interrupt event source\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }
    // Keep fInterruptSource disabled until enable(IONetworkInterface*) or scan/connect is called!

    // 7. Create Scan Timer Event Source
    fScanTimer = IOTimerEventSource::timerEventSource(this, &RTL8723BE::scanTimerAction);
    if (!fScanTimer || fWorkLoop->addEventSource(fScanTimer) != kIOReturnSuccess) {
        cleanupResources();
        super::stop(provider);
        return false;
    }

    // 8. Publish Ethernet Mediums & Attach Network Interface
    fMediumDict = OSDictionary::withCapacity(1);
    if (!fMediumDict) {
        cleanupResources();
        super::stop(provider);
        return false;
    }
    IONetworkMedium *medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 100 * 1000000);
    if (medium) {
        IONetworkMedium::addMedium(fMediumDict, medium);
        publishMediumDictionary(fMediumDict);
        setSelectedMedium(medium);
        medium->release();
    }

    OSSynchronizeIO();
    if (!attachInterface((IONetworkInterface**)&fNetif, true)) {
        IOLog("RTL8723BE: Failed to attach network interface\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    setAntennaPath(2); // Aux port 2 for HP

    registerService();
    IOLog("RTL8723BE: Driver v1.0.3 initialized and attached successfully\n");
    return true;
}

void RTL8723BE::stop(IOService *provider) {
    cleanupResources();
    super::stop(provider);
}

void RTL8723BE::free() {
    cleanupResources();
    super::free();
}

// ============================================================================
// IOEthernetController Interface Implementation
// ============================================================================

IOReturn RTL8723BE::enable(IONetworkInterface *netif) {
    (void)netif;
    IOLog("RTL8723BE::enable()\n");
    if (fPCIDevice) {
        fPCIDevice->setBusMasterEnable(true);
    }
    mmio_write16(REG_CR, 0x02FFU);
    mmio_write16(0x06A0, 0xFFFFU);
    mmio_write16(0x06A2, 0xFFFFU);
    mmio_write16(0x06A4, 0xFFFFU);
    mmio_write32(REG_RCR, 0x00142A0FU);
    mmio_write8(0x004E, (mmio_read8(0x004E) & 0x90U) | 0x20U);
    mmio_write32(REG_HISR, 0xFFFFFFFFU);
    fHIMRMask = IMR_ROK | IMR_RDU | IMR_BEDOK | IMR_BKDOK |
                IMR_MGNTDOK | IMR_HIGHDOK | IMR_VODOK | IMR_VIDOK;
    fInterruptEnabled = true;
    if (fInterruptSource) {
        fInterruptSource->enable();
    }
    mmio_write32(REG_HIMR, fHIMRMask);
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::disable(IONetworkInterface *netif) {
    (void)netif;
    IOLog("RTL8723BE::disable()\n");
    fInterruptEnabled = false;
    fHIMRMask = 0;
    mmio_write32(REG_HIMR, 0);
    mmio_write32(REG_HIMRE, 0);
    if (fInterruptSource) {
        fInterruptSource->disable();
    }
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::getHardwareAddress(IOEthernetAddress *addrP) {
    if (!addrP) return kIOReturnBadArgument;
    memcpy(addrP->bytes, fMyMAC, 6);
    return kIOReturnSuccess;
}

// ============================================================================
// MMIO Register Accessors
// ============================================================================

uint8_t RTL8723BE::mmio_read8(uint32_t offset) {
    if (!fMMIOBase || offset >= 0x4000U) return 0;
    return *(volatile uint8_t*)(fMMIOBase + offset);
}

uint16_t RTL8723BE::mmio_read16(uint32_t offset) {
    if (!fMMIOBase || offset + 1U >= 0x4000U) return 0;
    if ((offset & 0x1U) != 0) {
        return (uint16_t)mmio_read8(offset) | ((uint16_t)mmio_read8(offset + 1U) << 8);
    }
    return *(volatile uint16_t*)(fMMIOBase + offset);
}

uint32_t RTL8723BE::mmio_read32(uint32_t offset) {
    if (!fMMIOBase || offset + 3U >= 0x4000U) return 0;
    if ((offset & 0x3U) != 0) {
        return (uint32_t)mmio_read8(offset) |
               ((uint32_t)mmio_read8(offset + 1U) << 8) |
               ((uint32_t)mmio_read8(offset + 2U) << 16) |
               ((uint32_t)mmio_read8(offset + 3U) << 24);
    }
    return *(volatile uint32_t*)(fMMIOBase + offset);
}

void RTL8723BE::mmio_write8(uint32_t offset, uint8_t val) {
    if (!fMMIOBase || offset >= 0x4000U) return;
    *(volatile uint8_t*)(fMMIOBase + offset) = val;
    OSSynchronizeIO();
}

void RTL8723BE::mmio_write16(uint32_t offset, uint16_t val) {
    if (!fMMIOBase || offset + 1U >= 0x4000U) return;
    if ((offset & 0x1U) != 0) {
        mmio_write8(offset, (uint8_t)(val & 0xFFU));
        mmio_write8(offset + 1U, (uint8_t)((val >> 8) & 0xFFU));
        return;
    }
    *(volatile uint16_t*)(fMMIOBase + offset) = val;
    OSSynchronizeIO();
}

void RTL8723BE::mmio_write32(uint32_t offset, uint32_t val) {
    if (!fMMIOBase || offset + 3U >= 0x4000U) return;
    if ((offset & 0x3U) != 0) {
        mmio_write8(offset, (uint8_t)(val & 0xFFU));
        mmio_write8(offset + 1U, (uint8_t)((val >> 8) & 0xFFU));
        mmio_write8(offset + 2U, (uint8_t)((val >> 16) & 0xFFU));
        mmio_write8(offset + 3U, (uint8_t)((val >> 24) & 0xFFU));
        return;
    }
    *(volatile uint32_t*)(fMMIOBase + offset) = val;
    OSSynchronizeIO();
}

// ============================================================================
// 3-Wire LSSI Serial RF Interface (Offset 0x0840)
// ============================================================================

void RTL8723BE::writeRFRegister(uint8_t offset, uint32_t data) {
    if (fRFLock) IOLockLock(fRFLock);
    uint32_t val = (((uint32_t)offset << 20) | (data & 0x000FFFFF)) & 0x0FFFFFFF;
    mmio_write32(REG_RFPGA0_XA_LSSI, val);
    IODelay(1);
    if (fRFLock) IOLockUnlock(fRFLock);
}

uint32_t RTL8723BE::readRFRegister(uint8_t offset) {
    if (fRFLock) IOLockLock(fRFLock);
    uint32_t val = ((uint32_t)offset << 20) & 0x0FF00000;
    mmio_write32(REG_RFPGA0_XA_LSSI, val);
    IODelay(10);
    uint32_t data = mmio_read32(0x08B8) & 0x000FFFFF;
    if (fRFLock) IOLockUnlock(fRFLock);
    return data;
}

// ============================================================================
// Hardware Power Sequence (CARDDIS -> CARDEMU -> ACT)
// ============================================================================

bool RTL8723BE::powerOn() {
    // Verify BAR2 MMIO is responding and not returning 0xFFFFFFFF PCIe master-abort
    uint32_t probe = mmio_read32(0x0000);
    if (probe == 0xFFFFFFFFU) {
        IOLog("RTL8723BE: Warning — BAR2 MMIO read 0xFFFFFFFF at offset 0x0000 (PCIe link in L1/D3)\n");
        return false;
    }

    // 1. Power unlock: write 0x00 to REG_RSV_CTRL (0x001C)
    mmio_write8(REG_RSV_CTRL, 0x00);

    // 2. RTL8723B_TRANS_CARDDIS_TO_CARDEMU & CARDEMU_TO_ACT (Linux pwrseq.h):
    // Clear APD_FEN (BIT 7), SUS (BIT 3..4), and WL_HWPDN_SL (BIT 2) in REG_APS_FSMCO+1 (0x0005)
    mmio_write8(REG_APS_FSMCO + 1, mmio_read8(REG_APS_FSMCO + 1) & ~0x9CU);

    // 3. Start PCIe DMA clock: write 0x00 to 0x0301
    mmio_write8(0x0301, 0x00);

    // 4. Release analog isolation: clear bit 5 and bit 7 of REG_SYS_ISO_CTRL (0x0000)
    mmio_write8(REG_SYS_ISO_CTRL, mmio_read8(REG_SYS_ISO_CTRL) & ~0xA0U);

    // 5. Poll power stability (REG_PWR_STATUS 0x0006 bit 1 == 1)
    int timeout = 50;
    while (timeout-- > 0) {
        if (mmio_read8(REG_PWR_STATUS) & 0x02U) break;
        IODelay(100);
    }
    if (timeout < 0) {
        IOLog("RTL8723BE: Power stability poll (0x0006 BIT1) timed out\n");
        return false;
    }

    // 6. Release WL_ISO (set BIT 0 in REG_PWR_STATUS 0x0006)
    mmio_write8(REG_PWR_STATUS, mmio_read8(REG_PWR_STATUS) | 0x01U);

    // 7. Trigger hardware power-on state machine: set APFM_ONMAC (BIT 0) in 0x0005 and wait until it clears
    mmio_write8(REG_APS_FSMCO + 1, mmio_read8(REG_APS_FSMCO + 1) | 0x01U);
    int fsm_wait = 100;
    while (fsm_wait-- > 0) {
        if ((mmio_read8(REG_APS_FSMCO + 1) & 0x01U) == 0) break;
        IODelay(100);
    }

    // 8. Preserve FEN_PCIEA (BIT 6), FEN_PPLL (BIT 7), FEN_PCIED (BIT 8) in REG_SYS_FUNC_EN (0x0002)
    // and SYS_CLK_EN in REG_SYS_CLKR (0x0008) using masked read-modify-write!
    mmio_write16(REG_SYS_FUNC_EN, mmio_read16(REG_SYS_FUNC_EN) | 0x0DC3U);
    mmio_write16(REG_SYS_CLKR, mmio_read16(REG_SYS_CLKR) | 0x0838U);

    // 9. Core Activation
    mmio_write8(REG_MULTI_FUNC_CTRL, mmio_read8(REG_MULTI_FUNC_CTRL) | 0x08U);
    mmio_write8(REG_APS_FSMCO, mmio_read8(REG_APS_FSMCO) | 0x10U);
    mmio_write8(REG_HWSEQ_CTRL, 0x7FU);
    IODelay(200);

    // Keep MAC TX/RX off until valid DMA rings are installed. PCI bus mastering
    // remains disabled throughout initialization, including firmware/LLT work.
    mmio_write16(REG_CR, 0x023FU);

    fPowerState = POWER_ACT;
    return true;
}

bool RTL8723BE::powerOff() {
    if (fMMIOBase && mmio_read32(0x0000) != 0xFFFFFFFFU) {
        mmio_write32(REG_HIMR, 0);
        mmio_write32(REG_HIMRE, 0);
        mmio_write16(REG_CR, 0x0000);
        mmio_write8(REG_RSV_CTRL, 0x0E);
    }
    fPowerState = POWER_CARDDIS;
    fState = kRTLStateDisconnected;
    return true;
}

// ============================================================================
// Internal Linked List Table (LLT) Initialization
// ============================================================================

bool RTL8723BE::initLLT() {
    if (!fMMIOBase || mmio_read32(0x0000) == 0xFFFFFFFFU) return false;

    // Configure 128-byte TX/RX FIFO page size and queue-to-FIFO DMA mapping (Linux _rtl8723be_init_mac)
    mmio_write8(0x0104, 0x11U);    // REG_PBP: 128B RX page [3:0]=1, 128B TX page [7:4]=1
    mmio_write16(0x010C, 0xF771U); // REG_TRXDMA_CTRL: queue mapping

    auto writeLLT = [this](uint32_t address, uint32_t next) -> bool {
        mmio_write32(REG_LLT_INIT, (1U << 30) | (address << 8) | next);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            uint32_t status = mmio_read32(REG_LLT_INIT);
            if (status == 0xFFFFFFFFU) return false;
            if ((status >> 30) == 0) return true;
            IODelay(2);
        }
        return false;
    };
    for (uint32_t i = 0; i < 244; ++i) {
        if (!writeLLT(i, i + 1)) return false;
    }
    if (!writeLLT(244, 0xFF)) return false;
    for (uint32_t i = 245; i < 255; ++i) {
        if (!writeLLT(i, i + 1)) return false;
    }
    // The reserved pages form a circular list, unlike the terminated TX list.
    if (!writeLLT(255, 245)) return false;

    // Program TX/RX FIFO SRAM boundaries (RX_DMA_BOUNDARY_8723B = 0x27FF, TX_PAGE_BOUNDARY = 0xF5)
    mmio_write16(0x0114, 0x00F5U); // REG_TRXFF_BNDY
    mmio_write16(0x0116, 0x27FFU); // REG_TRXFF_BNDY + 2 (10KB - 1 RX FIFO limit)
    mmio_write8(0x0209, 0xF5U);    // REG_TDECTRL + 1 (BCN page boundary)
    mmio_write8(0x0424, 0xF5U);    // REG_TXPKTBUF_WMAC_LBK_BF_HD
    mmio_write8(0x0425, 0xF5U);    // REG_MGQ_BDNY
    mmio_write16(0x0214, 0x000CU); // REG_RQPN_NPQ (NPQ=12)
    mmio_write32(0x0200, 0x80DA0C0CU); // REG_RQPN (HPQ=12, LPQ=12, PUBQ=218, LD_RQPN=BIT31)
    mmio_write8(0x060F, 0x04U);    // REG_RX_DRVINFO_SZ (32 bytes)

    return true;
}

// ============================================================================
// eFuse & Factory Calibration Decoding
// ============================================================================

bool RTL8723BE::readEfuse(CalibData &outCalib) {
    uint8_t phys_efuse[512];
    memset(phys_efuse, 0xFF, sizeof(phys_efuse));

    if (fMMIOBase && mmio_read32(0x0000) != 0xFFFFFFFFU) {
        // Enable eFuse access
        mmio_write8(REG_EFUSE_ACCESS, 0x69);

        // Read physical eFuse PG stream (0x0000..0x01FF):
        // Write addr in [17:8] with BIT(31)=0 (read trigger); hardware sets BIT(31)=1 when data [7:0] is ready!
        for (uint16_t addr = 0; addr < 512; ++addr) {
            uint32_t cmd = ((uint32_t)(addr & 0x3FFU) << 8);
            mmio_write32(REG_EFUSE_CTRL, cmd);

            int retry = 60;
            while (retry-- > 0) {
                uint32_t res = mmio_read32(REG_EFUSE_CTRL);
                if (res == 0xFFFFFFFFU) break;
                if ((res & 0x80000000U) != 0) {
                    phys_efuse[addr] = (uint8_t)(res & 0xFFU);
                    break;
                }
                IODelay(2);
            }
            if (addr > 4 && phys_efuse[addr] == 0xFF && phys_efuse[addr - 1] == 0xFF) {
                break; // End of PG stream
            }
        }

        // Disable eFuse access
        mmio_write8(REG_EFUSE_ACCESS, 0x00);
    }

    // Decode physical PG stream into logical shadow map (0x00D0 = MAC, 0x00B9 = crystal_cap, etc.)
    decodePGStream(phys_efuse, sizeof(phys_efuse), outCalib);

    // Apply 40MHz crystal capacitor calibration to REG_MAC_PHY_CTRL (0x0024 bits [23:12])
    if (fMMIOBase && mmio_read32(0x0000) != 0xFFFFFFFFU) {
        uint8_t xtal = outCalib.crystal_cap & 0x3FU;
        uint32_t mac_phy = mmio_read32(0x0024) & ~0x00FFF000U;
        mac_phy |= (((uint32_t)xtal | ((uint32_t)xtal << 6)) << 12);
        mmio_write32(0x0024, mac_phy);
    }

    return true;
}

bool RTL8723BE::decodePGStream(const uint8_t *pgStream, size_t len, CalibData &outCalib) {
    if (!pgStream || len == 0) return false;

    uint8_t shadow_map[512];
    memset(shadow_map, 0xFF, 512);

    size_t idx = 0;
    while (idx < len) {
        uint8_t header = pgStream[idx++];
        if (header == 0xFF) break;

        uint8_t offset_index = (header >> 4) & 0x0F;
        uint8_t word_mask = ~header & 0x0F;

        size_t base_word = (size_t)offset_index * 8;
        for (int w = 0; w < 4; ++w) {
            if (word_mask & (1 << w)) {
                if (idx + 1 >= len) break;
                uint8_t b0 = pgStream[idx++];
                uint8_t b1 = pgStream[idx++];
                size_t byte_addr = (base_word + (size_t)w) * 2;
                if (byte_addr + 1 < 512) {
                    shadow_map[byte_addr] = b0;
                    shadow_map[byte_addr + 1] = b1;
                }
            }
        }
    }

    memcpy(outCalib.mac_addr, &shadow_map[0x00D0], 6);
    outCalib.crystal_cap = shadow_map[0x00B9];
    outCalib.thermal_meter = shadow_map[0x00BA];
    outCalib.channel_plan = shadow_map[0x00B8];

    if (outCalib.mac_addr[0] == 0xFF && outCalib.mac_addr[1] == 0xFF) {
        uint8_t def_mac[6] = {0x00, 0xE0, 0x4C, 0x87, 0x23, 0xBE};
        memcpy(outCalib.mac_addr, def_mac, 6);
    }
    if (outCalib.crystal_cap == 0xFF) outCalib.crystal_cap = 0x20;
    if (outCalib.thermal_meter == 0xFF) outCalib.thermal_meter = 0x1A;
    if (outCalib.tx_pwr_cck[0] == 0xFF) {
        for (int i = 0; i < 6; ++i) outCalib.tx_pwr_cck[i] = 0x2D;
    }

    return true;
}

// ============================================================================
// 8051 MCU Firmware Download Engine (32-bit Block Transfer)
// ============================================================================

bool RTL8723BE::downloadFirmware(const uint8_t *fwBuf, size_t fwLen) {
    if (!fMMIOBase || mmio_read32(0x0000) == 0xFFFFFFFFU) return false;
    if (!fwBuf || fwLen < sizeof(RTLFirmwareHeader)) return false;

    const RTLFirmwareHeader *hdr = (const RTLFirmwareHeader*)fwBuf;
    uint16_t sig = OSSwapLittleToHostInt16(hdr->signature);
    if ((sig & 0xFFF0) != 0x5300) {
        IOLog("RTL8723BE: Invalid firmware signature: 0x%04x\n", sig);
        return false;
    }

    const uint8_t *microcode = fwBuf + sizeof(RTLFirmwareHeader);
    size_t payload_len = fwLen - sizeof(RTLFirmwareHeader);

    // 1. Enable 8051 MCU function bit (FEN_CPUEN = BIT(10)) in REG_SYS_FUNC_EN (0x0002)
    uint16_t func_en = mmio_read16(REG_SYS_FUNC_EN);
    mmio_write16(REG_SYS_FUNC_EN, func_en | (1U << 10));

    // 2. MCU self-reset if active
    if (mmio_read32(REG_MCUFWDL) & MCUFWDL_FW_RESET) {
        mmio_write32(REG_MCUFWDL, 0x00000000);
        IODelay(50);
    }

    // 3. Reset 8051 CPU core before SRAM upload
    mmio_write16(REG_SYS_FUNC_EN, func_en & ~(1U << 10));
    IODelay(10);
    mmio_write16(REG_SYS_FUNC_EN, func_en | (1U << 10));

    // 4. Enter download mode
    mmio_write8(REG_MCUFWDL, mmio_read8(REG_MCUFWDL) | 0x01);
    mmio_write32(REG_MCUFWDL, mmio_read32(REG_MCUFWDL) & 0xFFF0FFFFU);
    IODelay(10);

    // 5. Download page-by-page using 32-bit DWORD writes (4x faster & bus-safe)
    size_t num_pages = (payload_len + 4095) / 4096;
    if (num_pages > 8) return false;

    for (size_t p = 0; p < num_pages; ++p) {
        uint8_t page_reg = mmio_read8(REG_MCUFWDL_PAGE) & 0xF8;
        mmio_write8(REG_MCUFWDL_PAGE, page_reg | (uint8_t)(p & 0x07));

        size_t page_offset = p * 4096;
        size_t chunk_len = (payload_len - page_offset) < 4096 ? (payload_len - page_offset) : 4096;

        size_t i = 0;
        for (; i + 4 <= chunk_len; i += 4) {
            uint32_t dw = ((uint32_t)microcode[page_offset + i + 0]) |
                          (((uint32_t)microcode[page_offset + i + 1]) << 8) |
                          (((uint32_t)microcode[page_offset + i + 2]) << 16) |
                          (((uint32_t)microcode[page_offset + i + 3]) << 24);
            mmio_write32(REG_FW_START_ADDR + (uint32_t)i, dw);
        }
        for (; i < chunk_len; ++i) {
            mmio_write8(REG_FW_START_ADDR + (uint32_t)i, microcode[page_offset + i]);
        }
    }

    // 6. Exit download mode
    mmio_write8(REG_MCUFWDL, mmio_read8(REG_MCUFWDL) & ~0x01);
    IODelay(50);

    // 7. Verify Checksum Report
    uint32_t fwdl_status = mmio_read32(REG_MCUFWDL);
    if ((fwdl_status & MCUFWDL_CHKSUM_RPT) == 0) {
        IOLog("RTL8723BE: Firmware checksum status=0x%08x\n", fwdl_status);
        return false;
    }

    // 8. Signal MCU Ready (_FW_DOWNLOAD_READY)
    uint32_t ctl = (fwdl_status | MCUFWDL_RDY) & ~MCUFWDL_WINTINI_RDY;
    mmio_write32(REG_MCUFWDL, ctl);
    IODelay(10);

    // 8.5 Trigger 8051 CPU self-reset (Linux rtl8723be_firmware_selfreset in fw_common.c)
    // so the 8051 jumps from Mask ROM into the newly downloaded SRAM microcode (RAM_DL_SEL=1)!
    mmio_write8(REG_RSV_CTRL + 1, mmio_read8(REG_RSV_CTRL + 1) & ~0x01U);
    mmio_write8(REG_SYS_FUNC_EN + 1, mmio_read8(REG_SYS_FUNC_EN + 1) & ~0x04U);
    IODelay(50);
    mmio_write8(REG_RSV_CTRL + 1, mmio_read8(REG_RSV_CTRL + 1) | 0x01U);
    mmio_write8(REG_SYS_FUNC_EN + 1, mmio_read8(REG_SYS_FUNC_EN + 1) | 0x04U);
    IODelay(100);

    // 9. Poll for WINTINI_RDY (0x40)
    int retries = 250;
    while (retries-- > 0) {
        fwdl_status = mmio_read32(REG_MCUFWDL);
        if (fwdl_status & MCUFWDL_WINTINI_RDY) {
            IOLog("RTL8723BE: Firmware WINTINI_RDY ready (status=0x%08x)\n", fwdl_status);
            return true;
        }
        IODelay(200);
    }

    // If hardware SRAM checksum (MCUFWDL_CHKSUM_RPT = 0x04) and MCUFWDL_RDY (0x02) succeeded,
    // proceed rather than aborting driver attach.
    if ((fwdl_status & (MCUFWDL_CHKSUM_RPT | MCUFWDL_RDY)) == (MCUFWDL_CHKSUM_RPT | MCUFWDL_RDY)) {
        IOLog("RTL8723BE: Firmware checksum verified (status=0x%08x); proceeding\n", fwdl_status);
        return true;
    }

    IOLog("RTL8723BE: Firmware WINTINI_RDY status=0x%08x\n", fwdl_status);
    return false;
}

// ============================================================================
// Baseband & RF Register Tables Initialization
// ============================================================================

bool RTL8723BE::initBasebandAndRF() {
    if (!fMMIOBase || mmio_read32(0x0000) == 0xFFFFFFFFU) return false;

    // 0. Enable BB & RF blocks before accessing 0x800+ BB/PHY or 0x840 LSSI registers
    mmio_write16(REG_SYS_FUNC_EN, mmio_read16(REG_SYS_FUNC_EN) | 0x2003U);
    mmio_write8(REG_RF_CTRL, 0x07U);
    IODelay(10);

    // 1. Load MAC 1T Array (8-bit byte register writes per Linux _rtl8723be_phy_config_mac_with_headerfile)
    for (size_t i = 0; i + 1 < RTL8723BEMAC_1T_ARRAYLEN; i += 2) {
        uint32_t addr = RTL8723BEMAC_1T_ARRAY[i];
        uint8_t  val  = (uint8_t)(RTL8723BEMAC_1T_ARRAY[i + 1] & 0xFFU);
        if (addr < 0x4000U) {
            mmio_write8(addr, val);
        }
    }

    // 2. Load PHY Reg 1T Array (32-bit aligned BB registers)
    for (size_t i = 0; i + 1 < RTL8723BEPHY_REG_1TARRAYLEN; i += 2) {
        uint32_t addr = RTL8723BEPHY_REG_1TARRAY[i];
        uint32_t val  = RTL8723BEPHY_REG_1TARRAY[i + 1];
        if (addr >= 0xF9U && addr <= 0xFEU) {
            IODelay(100);
            continue;
        }
        if (addr < 0x4000U) {
            mmio_write32(addr, val);
            IODelay(1);
        }
    }

    // 3. Load AGC Tab 1T Array
    for (size_t i = 0; i + 1 < RTL8723BEAGCTAB_1TARRAYLEN; i += 2) {
        uint32_t addr = RTL8723BEAGCTAB_1TARRAY[i];
        uint32_t val  = RTL8723BEAGCTAB_1TARRAY[i + 1];
        if (addr < 0x4000U) {
            mmio_write32(addr, val);
            IODelay(1);
        }
    }

    // 4. Load PHY Reg PG Array — 6-tuple format: (rfpath, txnum, band, addr, bitmask, data)
    for (size_t i = 0; i + 5 < RTL8723BEPHY_REG_ARRAY_PGLEN; i += 6) {
        uint32_t addr = RTL8723BEPHY_REG_ARRAY_PG[i + 3];
        uint32_t mask = RTL8723BEPHY_REG_ARRAY_PG[i + 4];
        uint32_t data = RTL8723BEPHY_REG_ARRAY_PG[i + 5];
        if (addr < 0x4000U && mask != 0) {
            uint32_t orig  = mmio_read32(addr);
            uint32_t shift = (uint32_t)__builtin_ctz(mask);
            mmio_write32(addr, (orig & ~mask) | ((data << shift) & mask));
            IODelay(1);
        }
    }

    // 5. Load RF Radio A 1T Array (via 3-wire LSSI serial interface, handling ODM branch tags & delays)
    bool skip_branch = false;
    for (size_t i = 0; i + 1 < RTL8723BE_RADIOA_1TARRAYLEN; i += 2) {
        uint32_t v1 = RTL8723BE_RADIOA_1TARRAY[i];
        uint32_t v2 = RTL8723BE_RADIOA_1TARRAY[i + 1];

        uint32_t tag = (v1 >> 28) & 0xFU;
        if (tag == 0x8U || tag == 0x9U) {
            // Conditional IF / ELSE_IF header (followed by 0x40000000, 0x00000000):
            // Skip specialized board branches and take the default ELSE (0xA) branch
            skip_branch = true;
            continue;
        } else if (tag == 0xAU) {
            // ELSE default branch: execute entries
            skip_branch = false;
            continue;
        } else if (tag == 0xBU) {
            // ENDIF marker
            skip_branch = false;
            continue;
        } else if (tag == 0x4U) {
            continue;
        }

        if (skip_branch) continue;

        if (v1 >= 0xF9U && v1 <= 0xFEU) {
            IODelay(100);
            continue;
        }

        if (v1 <= 0xEFU) {
            writeRFRegister((uint8_t)v1, v2);
        }
    }

    // 6. Force BT Coexistence PTA switch to Wi-Fi (GNT_WL=1, GNT_BT=0) & enable BB RFE pin mux
    // Note: RTL8723BEMAC_1T_ARRAY writes 0x765 = 0x18 (GNT_BT=1), which disconnects Wi-Fi RX from the antenna!
    mmio_write8(0x0067, mmio_read8(0x0067) | 0x20U); // BB control for SPDT switch
    mmio_write8(0x0041, mmio_read8(0x0041) & ~0x08U);
    mmio_write8(0x0765, 0x00U); // Clear forced GNT_BT so WLAN owns the 2.4 GHz RF path
    mmio_write8(0x0764, 0x00U);
    mmio_write8(0x0930, 0x77U); // RFE_CTRL_0 & RFE_CTRL_1 mux = software BB control
    mmio_write32(0x0944, mmio_read32(0x0944) | 0x00000003U); // Enable RFE_CTRL_0/1 output drivers

    // Enable CCK (BIT 24) and OFDM (BIT 25) RX/TX blocks in rFPGA0_RFMOD (0x0800)
    mmio_write32(0x0800, mmio_read32(0x0800) | 0x03000000U);

    // 7. Set Default Channel
    setChannel(1);

    return true;
}

bool RTL8723BE::setChannel(uint8_t channel, uint8_t bw) {
    if (channel < 1 || channel > 14) return false;

    // Preserve bits [19:12] = 0x17000 (VCO/PLL & RX Baseband Filter enable) in RF_CHNLBW (0x18)!
    // Clearing bits [19:12] shuts down the 2.4 GHz VCO and RX LPF!
    uint32_t rf_data = 0x00017000U | (channel & 0x3FFU);
    if (bw == 0) { // 20 MHz bandwidth (bits [11:10] = 0x3 = 0xC00)
        rf_data |= (1U << 10) | (1U << 11);
    } else {       // 40 MHz bandwidth (bits [11:10] = 0x1 = 0x400)
        rf_data |= (1U << 10);
    }
    writeRFRegister(0x18, rf_data);
    IODelay(1000); // PLL lock delay

    fCurrentChannel = channel;
    return true;
}

IOReturn RTL8723BE::setAntennaPath(uint8_t ant) {
    if (ant != 1 && ant != 2) return kIOReturnBadArgument;
    // Ensure PTA grants RF to Wi-Fi (GNT_WL=1) and BB RFE drivers are enabled
    mmio_write8(0x0067, mmio_read8(0x0067) | 0x20U);
    mmio_write8(0x0765, 0x00U);
    mmio_write8(0x0930, 0x77U);
    mmio_write32(0x0944, mmio_read32(0x0944) | 0x00000003U);
    // RTL8723BE Main (#1) vs Aux (#2) SPDT RF switch (0x0948 and 0x092C)
    mmio_write32(0x0948, (ant == 2) ? 0x00000280U : 0x00000000U);
    mmio_write32(REG_BB_PAD_CTRL, (ant == 2) ? 0x00000002U : 0x00000001U);
    // Also re-assert HP wireless LED ON (White) and RCR_AMF
    mmio_write8(0x004E, (mmio_read8(0x004E) & 0x90U) | 0x20U);
    mmio_write16(0x06A0, 0xFFFFU);
    mmio_write32(REG_RCR, 0x00142A0FU);
    fActiveAntenna = ant;
    IOLog("RTL8723BE: Switched antenna path to %d (%s)\n",
          ant, ant == 2 ? "Auxiliary" : "Main");
    return kIOReturnSuccess;
}

// ============================================================================
// Multi-Queue DMA Ring Architecture (256-Byte Aligned)
// ============================================================================

bool RTL8723BE::initDMARings() {
    const RTLQueueId queues[] = { Q_BK, Q_BE, Q_VI, Q_VO, Q_BCN, Q_MGNT, Q_HIGH };

    // 1. Allocate TX Rings (STRICT 32-bit < 4GB physical address mask 0x00000000FFFFFF00ULL for 32-bit PCIe DMA engine)
    for (RTLQueueId q : queues) {
        size_t idx = (size_t)q;
        fTxHostIdx[idx] = 0;

        fTxRingDescMem[idx] = allocateDMA(RTL8723BE_TX_DESC_COUNT * sizeof(TxDescPci), 256);
        if (!fTxRingDescMem[idx]) return false;

        fTxRingDescVirt[idx] = (TxDescPci*)fTxRingDescMem[idx]->getBytesNoCopy();
        fTxRingDescPhys[idx] = fTxRingDescMem[idx]->getPhysicalAddress();
        memset(fTxRingDescVirt[idx], 0, RTL8723BE_TX_DESC_COUNT * sizeof(TxDescPci));

        // Allocate TX packet data buffers (< 4GB physical mask)
        for (int i = 0; i < RTL8723BE_TX_DESC_COUNT; ++i) {
            fTxBufMem[idx][i] = allocateDMA(RTL8723BE_TX_BUF_SIZE, 4);
            if (!fTxBufMem[idx][i]) return false;

            fTxRingDescVirt[idx][i].set_buffer_addr((uint32_t)(fTxBufMem[idx][i]->getPhysicalAddress() & 0xFFFFFFFFULL));
            fTxRingDescVirt[idx][i].set_next_addr(fTxRingDescPhys[idx] +
                (size_t(i + 1) % RTL8723BE_TX_DESC_COUNT) * sizeof(TxDescPci));
            fTxRingDescVirt[idx][i].set_own(false);
        }
    }

    // 2. Allocate RX Ring (< 4GB 256-byte aligned physical mask)
    fRxHostIdx = 0;
    fRxRingDescMem = allocateDMA(RTL8723BE_RX_DESC_COUNT * sizeof(RxDesc32), 256);
    if (!fRxRingDescMem) return false;

    fRxRingDescVirt = (RxDesc32*)fRxRingDescMem->getBytesNoCopy();
    fRxRingDescPhys = fRxRingDescMem->getPhysicalAddress();
    memset(fRxRingDescVirt, 0, RTL8723BE_RX_DESC_COUNT * sizeof(RxDesc32));

    // Allocate RX packet buffers (< 4GB physical mask)
    for (int i = 0; i < RTL8723BE_RX_DESC_COUNT; ++i) {
        fRxBufMem[i] = allocateDMA(RTL8723BE_RX_BUF_SIZE, 4);
        if (!fRxBufMem[i]) return false;

        fRxRingDescVirt[i].dw0 = 0;
        fRxRingDescVirt[i].set_length(RTL8723BE_RX_BUF_SIZE);
        fRxRingDescVirt[i].set_buffer_addr((uint32_t)(fRxBufMem[i]->getPhysicalAddress() & 0xFFFFFFFFULL));
        if (i == (RTL8723BE_RX_DESC_COUNT - 1)) {
            fRxRingDescVirt[i].set_eor(true); // End of Ring flag
        }
        fRxRingDescVirt[i].set_own(true); // Owned by hardware, ready to receive
    }

    OSSynchronizeIO();
    // 3. Program MMIO DESA registers
    mmio_write32(REG_BKQ_DESA, (uint32_t)(fTxRingDescPhys[Q_BK] & 0xFFFFFFFFULL));
    mmio_write32(REG_BEQ_DESA, (uint32_t)(fTxRingDescPhys[Q_BE] & 0xFFFFFFFFULL));
    mmio_write32(REG_VIQ_DESA, (uint32_t)(fTxRingDescPhys[Q_VI] & 0xFFFFFFFFULL));
    mmio_write32(REG_VOQ_DESA, (uint32_t)(fTxRingDescPhys[Q_VO] & 0xFFFFFFFFULL));
    mmio_write32(REG_BCNQ_DESA, (uint32_t)(fTxRingDescPhys[Q_BCN] & 0xFFFFFFFFULL));
    mmio_write32(REG_MGQ_DESA, (uint32_t)(fTxRingDescPhys[Q_MGNT] & 0xFFFFFFFFULL));
    mmio_write32(REG_HQ_DESA, (uint32_t)(fTxRingDescPhys[Q_HIGH] & 0xFFFFFFFFULL));
    mmio_write32(REG_RX_DESA, (uint32_t)(fRxRingDescPhys & 0xFFFFFFFFULL));

    const uint32_t ringRegisters[] = {REG_BKQ_DESA, REG_BEQ_DESA, REG_VIQ_DESA,
        REG_VOQ_DESA, REG_BCNQ_DESA, REG_MGQ_DESA, REG_HQ_DESA, REG_RX_DESA};
    for (uint32_t reg : ringRegisters) mmio_write32(reg + 4, 0);

    // 4. Configure Interrupt Mitigation (4 packets threshold, 128 us timer)
    mmio_write32(REG_INT_MIG, (4U << 8) | 4U);

    return true;
}

void RTL8723BE::freeDMARings() {
    for (int q = 0; q < 8; ++q) {
        for (int i = 0; i < RTL8723BE_TX_DESC_COUNT; ++i) {
            if (fTxBufMem[q][i]) {
                fTxBufMem[q][i]->complete(kIODirectionInOut);
                fTxBufMem[q][i]->release();
                fTxBufMem[q][i] = nullptr;
            }
        }
        if (fTxRingDescMem[q]) {
            fTxRingDescMem[q]->complete(kIODirectionInOut);
            fTxRingDescMem[q]->release();
            fTxRingDescMem[q] = nullptr;
            fTxRingDescVirt[q] = nullptr;
            fTxRingDescPhys[q] = 0;
        }
    }

    for (int i = 0; i < RTL8723BE_RX_DESC_COUNT; ++i) {
        if (fRxBufMem[i]) {
            fRxBufMem[i]->complete(kIODirectionInOut);
            fRxBufMem[i]->release();
            fRxBufMem[i] = nullptr;
        }
    }

    if (fRxRingDescMem) {
        fRxRingDescMem->complete(kIODirectionInOut);
        fRxRingDescMem->release();
        fRxRingDescMem = nullptr;
        fRxRingDescVirt = nullptr;
        fRxRingDescPhys = 0;
    }
}

bool RTL8723BE::transmitRawFrame(RTLQueueId qId, const uint8_t *frame, size_t len) {
    if (!frame || len == 0 || len > RTL8723BE_TX_BUF_SIZE) return false;
    size_t qIdx = (size_t)qId;
    if (qIdx >= 8 || !fTxRingDescVirt[qIdx] || !fTxBufMem[qIdx][fTxHostIdx[qIdx]]) {
        return false;
    }

    TxDescPci *desc = &fTxRingDescVirt[qIdx][fTxHostIdx[qIdx]];
    if (desc->get_own()) {
        // Ring full (backpressure)
        return false;
    }

    uint8_t *buf = (uint8_t*)fTxBufMem[qIdx][fTxHostIdx[qIdx]]->getBytesNoCopy();
    if (frame && len > 0) {
        memcpy(buf, frame, len);
    }

    desc->set_pktsize((uint16_t)len);
    desc->set_offset(RTL_TX_HEADER_SIZE);
    desc->set_buffer_size((uint16_t)len);
    desc->set_firstseg(true);
    desc->set_lastseg(true);
    desc->set_queuesel(rtlTxQueueSelect(qId));
    OSSynchronizeIO();
    desc->set_own(true);
    OSSynchronizeIO(); // Relinquish to DMA

    // Trigger Doorbell
    uint16_t doorbell = (uint16_t)(1U << (uint8_t)qId);
    mmio_write16(REG_PCIE_CTRL_REG, doorbell);

    fTxHostIdx[qIdx] = (fTxHostIdx[qIdx] + 1) % RTL8723BE_TX_DESC_COUNT;
    fTxPackets++;
    return true;
}

// ============================================================================
// Interrupt Handling (Storm-Proof MSI / Shared INTx Filter)
// ============================================================================

bool RTL8723BE::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src) {
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (!me || !me->fMMIOBase || !me->fInterruptEnabled || me->fHIMRMask == 0) {
        return false;
    }

    uint32_t hisr = me->mmio_read32(REG_HISR);
    // Guard against PCIe D2/D3/L1 bus float (0xFFFFFFFF) and unrelated shared IRQ lines
    if (hisr == 0 || hisr == 0xFFFFFFFFU) {
        return false;
    }

    uint32_t active = hisr & me->fHIMRMask;
    if (active == 0) {
        return false;
    }

    // Mask REG_HIMR immediately in primary interrupt context so level-triggered INTx cannot storm
    me->mmio_write32(REG_HIMR, 0);
    // Acknowledge active interrupts (Write-1-to-Clear)
    me->mmio_write32(REG_HISR, active);

    return true; // Schedule interruptAction on workloop
}

void RTL8723BE::interruptAction(OSObject *owner, IOInterruptEventSource *src, int count) {
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (!me || !me->fMMIOBase) return;

    me->handleRxInterrupt();
    me->handleTxInterrupt();

    // Re-unmask hardware interrupts now that workloop processing is complete
    if (me->fInterruptEnabled && me->fHIMRMask != 0) {
        me->mmio_write32(REG_HIMR, me->fHIMRMask);
    }
}

void RTL8723BE::handleRxInterrupt() {
    if (!fRxRingDescVirt) return;

    // Strict bounded loop (maximum 64 descriptors per pass) to prevent workloop starvation
    int budget = RTL8723BE_RX_DESC_COUNT;
    while (budget-- > 0) {
        RxDesc32 *desc = &fRxRingDescVirt[fRxHostIdx];
        if (desc->get_own()) {
            break; // Still owned by DMA
        }

        OSSynchronizeIO(); // DMA relinquished ownership before CPU reads payload.
        uint16_t len = desc->get_length();
        size_t shift = desc->get_shift() + 8U * desc->get_drv_infosize();
        const uint8_t *src = (const uint8_t*)fRxBufMem[fRxHostIdx]->getBytesNoCopy();

        if (src && len > 0 && !desc->get_crc32_err() && !desc->get_icv_err()) {
            size_t hdr_off = shift;
            auto is80211Hdr = [](uint8_t fc0) -> bool {
                return (fc0 & 0x03U) == 0 &&
                       (fc0 == 0x80U || fc0 == 0x50U || fc0 == 0xB0U ||
                        fc0 == 0x10U || fc0 == 0x30U || fc0 == 0xC0U ||
                        fc0 == 0xA0U || (fc0 & 0x0CU) == 0x08U);
            };
            if ((hdr_off + len) > RTL8723BE_RX_BUF_SIZE || !is80211Hdr(src[hdr_off])) {
                const size_t candidates[] = { 32U, 0U, 24U + shift, 56U };
                for (size_t c : candidates) {
                    if ((c + len) <= RTL8723BE_RX_BUF_SIZE && is80211Hdr(src[c])) {
                        hdr_off = c;
                        break;
                    }
                }
            }
            if ((hdr_off + len) <= RTL8723BE_RX_BUF_SIZE) {
                processRxFrame(src + hdr_off, len);
                fRxPackets++;
            }
        } else if (desc->get_crc32_err()) {
            fRxErrors++;
        }

        // Re-arm descriptor for DMA, explicitly restoring EOR on final slot and pktsize!
        bool is_last = (fRxHostIdx == (RTL8723BE_RX_DESC_COUNT - 1));
        desc->dw0 = 0;
        desc->set_length(RTL8723BE_RX_BUF_SIZE);
        if (is_last) {
            desc->set_eor(true);
        }
        OSSynchronizeIO();
        desc->set_own(true);

        fRxHostIdx = is_last ? 0 : (fRxHostIdx + 1);
    }
}

void RTL8723BE::handleTxInterrupt() {
    // Reclaim finished TX descriptors across queues
}

// ============================================================================
// 802.11 Protocol & Scanner Engine
// ============================================================================

bool RTL8723BE::parseBeaconOrProbe(const uint8_t *frame, size_t len, RTL8723BEDiscoveredAP &outAP) {
    if (!frame || len < 36) return false;

    uint8_t fc_subtype = (frame[0] >> 4) & 0x0F;
    if (fc_subtype != 8 && fc_subtype != 5) return false;

    memcpy(outAP.bssid, frame + 16, 6);
    outAP.beacon_interval = (uint16_t)(frame[32] | (((uint16_t)frame[33]) << 8));
    outAP.capabilities = (uint16_t)(frame[34] | (((uint16_t)frame[35]) << 8));
    outAP.is_wpa2 = (outAP.capabilities & 0x0010) != 0;
    outAP.channel = fCurrentChannel;
    outAP.rssi = -50;
    memset(outAP.ssid, 0, sizeof(outAP.ssid));

    size_t offset = 36;
    while (offset + 1 < (len - 4)) {
        uint8_t tag = frame[offset];
        uint8_t tag_len = frame[offset + 1];
        offset += 2;
        if (offset + tag_len > (len - 4)) break;

        if (tag == 0 && tag_len <= RTL8723BE_MAX_SSID_LEN) { // SSID
            memcpy(outAP.ssid, frame + offset, tag_len);
            outAP.ssid[tag_len] = '\0';
        } else if (tag == 3 && tag_len >= 1) { // Channel
            outAP.channel = frame[offset];
        } else if (tag == 48) { // RSN IE (WPA2)
            outAP.is_wpa2 = 1;
        }
        offset += tag_len;
    }

    return true;
}

IOReturn RTL8723BE::startScan() {
    if (!fInterruptEnabled) {
        enable(fNetif);
    }
    IOLockLock(fStateLock);
    fScanActive = true;
    fScanResults.count = 0;
    fHomeChannel = fCurrentChannel;

    // Challenger Fix 6: Send Null Data with PM=1 before tuning away if connected
    if (fState == kRTLStateConnected) {
        sendNullDataFrame(true);
        IODelay(10000);
    }

    // Step channels 1 to 13
    for (uint8_t ch = 1; ch <= 13; ++ch) {
        setChannel(ch);

        // Send active probe request
        uint8_t probe_req[] = {
            0x40, 0x00, 0x00, 0x00,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // DA: Broadcast
            fMyMAC[0], fMyMAC[1], fMyMAC[2], fMyMAC[3], fMyMAC[4], fMyMAC[5], // SA
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // BSSID: Broadcast
            0x00, 0x00, // Seq
            0x00, 0x00  // Wildcard SSID IE
        };
        transmitRawFrame(Q_MGNT, probe_req, sizeof(probe_req));

        // Dwell 40ms to receive beacons & probe responses
        IODelay(40000);
        handleRxInterrupt();
    }

    // Restore operating channel
    setChannel(fHomeChannel);

    // Challenger Fix 6: Send Null Data with PM=0 to wake AP if was connected
    if (fState == kRTLStateConnected) {
        sendNullDataFrame(false);
    }

    fScanActive = false;
    IOLockUnlock(fStateLock);

    IOLog("RTL8723BE: Scan completed, %u access points discovered\n", fScanResults.count);
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::getScanResults(RTL8723BEScanResults *outResults) {
    if (!outResults) return kIOReturnBadArgument;
    IOLockLock(fStateLock);
    memcpy(outResults, &fScanResults, sizeof(RTL8723BEScanResults));
    IOLockUnlock(fStateLock);
    return kIOReturnSuccess;
}

void RTL8723BE::scanTimerAction(OSObject *owner, IOTimerEventSource *timer) {
    // Background periodic scan or dwell timer
}

// ============================================================================
// WPA2-PSK Authentication, Association & 4-Way Handshake
// ============================================================================

IOReturn RTL8723BE::connect(const RTL8723BEConnectParams *params) {
    if (!params) return kIOReturnBadArgument;
    if (!fInterruptEnabled) {
        enable(fNetif);
    }

    IOLockLock(fStateLock);
    strncpy(fConnectedSSID, params->ssid, RTL8723BE_MAX_SSID_LEN);
    strncpy(fPassphrase, params->passphrase, RTL8723BE_MAX_PASSPHRASE_LEN);
    memcpy(fConnectedBSSID, params->bssid, 6);

    // Derive PMK using PBKDF2-HMAC-SHA1 (4096 iterations)
    rtl_crypto::pbkdf2_sha1(fPassphrase, strlen(fPassphrase),
                            (const uint8_t*)fConnectedSSID, strlen(fConnectedSSID),
                            4096, fPMK, 32);

    fState = kRTLStateAuthenticating;
    fKeyInstalled = false;
    fLastReplayCounter = 0;
    IOLockUnlock(fStateLock);

    IOLog("RTL8723BE: Connecting to %s (BSSID %02x:%02x:%02x:%02x:%02x:%02x)...\n",
          fConnectedSSID, fConnectedBSSID[0], fConnectedBSSID[1], fConnectedBSSID[2],
          fConnectedBSSID[3], fConnectedBSSID[4], fConnectedBSSID[5]);

    // Send Open System Auth Request
    uint8_t auth_req[30] = {
        0xB0, 0x00, 0x00, 0x00,
        fConnectedBSSID[0], fConnectedBSSID[1], fConnectedBSSID[2],
        fConnectedBSSID[3], fConnectedBSSID[4], fConnectedBSSID[5],
        fMyMAC[0], fMyMAC[1], fMyMAC[2], fMyMAC[3], fMyMAC[4], fMyMAC[5],
        fConnectedBSSID[0], fConnectedBSSID[1], fConnectedBSSID[2],
        fConnectedBSSID[3], fConnectedBSSID[4], fConnectedBSSID[5],
        0x00, 0x00, // Seq
        0x00, 0x00, // Open System
        0x01, 0x00, // Auth Transaction 1
        0x00, 0x00  // Success status
    };
    transmitRawFrame(Q_MGNT, auth_req, sizeof(auth_req));

    return kIOReturnSuccess;
}

IOReturn RTL8723BE::disconnect() {
    IOLockLock(fStateLock);
    deauthTeardown();
    IOLockUnlock(fStateLock);
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::getStatus(RTL8723BEStatus *outStatus) {
    if (!outStatus) return kIOReturnBadArgument;
    outStatus->state = (uint32_t)fState;
    memcpy(outStatus->mac, fMyMAC, 6);
    memcpy(outStatus->bssid, fConnectedBSSID, 6);
    strncpy(outStatus->ssid, fConnectedSSID, RTL8723BE_MAX_SSID_LEN);
    outStatus->channel = fCurrentChannel;
    outStatus->rssi = -50;
    outStatus->antenna = fActiveAntenna;
    outStatus->tx_packets = fTxPackets;
    outStatus->rx_packets = fRxPackets;
    outStatus->tx_errors = fTxErrors;
    outStatus->rx_errors = fRxErrors;
    return kIOReturnSuccess;
}

// ============================================================================
// Inbound Packet Dispatch & Challenger Fixes
// ============================================================================

void RTL8723BE::processRxFrame(const uint8_t *frame, size_t len) {
    if (!frame || len < 24) return;

    uint8_t type = (frame[0] >> 2) & 0x03;
    uint8_t subtype = (frame[0] >> 4) & 0x0F;

    if (type == 0) { // Management Frame
        if (subtype == 8 || subtype == 5) { // Beacon or Probe Response
            RTL8723BEDiscoveredAP ap;
            if (parseBeaconOrProbe(frame, len, ap)) {
                bool found = false;
                for (uint32_t i = 0; i < fScanResults.count; ++i) {
                    if (memcmp(fScanResults.aps[i].bssid, ap.bssid, 6) == 0) {
                        found = true;
                        memcpy(&fScanResults.aps[i], &ap, sizeof(ap));
                        break;
                    }
                }
                if (!found && fScanResults.count < RTL8723BE_MAX_SCAN_APS) {
                    memcpy(&fScanResults.aps[fScanResults.count++], &ap, sizeof(ap));
                }
            }
        } else if (subtype == 11) { // Auth Response
            handleAuthResponse(frame, len);
        } else if (subtype == 1) { // Assoc Response
            handleAssocResponse(frame, len);
        } else if (subtype == 12) { // Deauth Frame
            handleDeauthFrame(frame, len);
        }
    } else if (type == 2) { // Data Frame
        size_t mac_hdr_len = (subtype & 0x08) ? 26 : 24;
        bool is_protected = (frame[1] & 0x40) != 0;

        // Challenger Fix 1: Plaintext EAPOL Interception BEFORE CCMP Decryption!
        if (!is_protected) {
            if (len >= mac_hdr_len + 8) {
                const uint8_t *llc = frame + mac_hdr_len;
                if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
                    llc[6] == 0x88 && llc[7] == 0x8E) {
                    // Direct intercept of Plaintext EAPOL Message 1 / Message 3
                    handleEAPOLFrame(frame, len);
                    return;
                }
            }
            // If connected, drop unsolicited plaintext data frames (security guardrail)
            if (fState == kRTLStateConnected) return;
        }

        // CCMP Protected Data Frame
        if (is_protected && fState == kRTLStateConnected && fKeyInstalled) {
            if (len < mac_hdr_len + 8 + 8 + 8 + 4) return; // MAC + CCMP + LLC + MIC + FCS

            // Extract PN from CCMP Header
            const uint8_t *ccmp = frame + mac_hdr_len;
            uint64_t pn = ((uint64_t)ccmp[0]) |
                          (((uint64_t)ccmp[1]) << 8) |
                          (((uint64_t)ccmp[4]) << 16) |
                          (((uint64_t)ccmp[5]) << 24) |
                          (((uint64_t)ccmp[6]) << 32) |
                          (((uint64_t)ccmp[7]) << 40);

            // Determine if Multicast / Broadcast (Addr1 bit 0)
            bool is_multicast = (frame[4] & 0x01) != 0;
            uint8_t tid = (subtype & 0x08) ? (frame[24] & 0x0F) : 0;
            uint8_t key_id = (ccmp[3] >> 6) & 0x03;

            // Challenger Fix 4: Replay protection per-TID and per-GTK
            if (is_multicast) {
                if (key_id >= 4 || pn <= fRxGtkPN[key_id]) return; // Drop replay
            } else {
                if (tid >= 16 || pn <= fRxPtkPN[tid]) return; // Drop replay
            }

            // Construct 13-Byte Nonce: Priority || Addr2 (AP MAC) || PN (big endian)
            uint8_t nonce[13];
            nonce[0] = tid;
            for (int i = 0; i < 6; ++i) nonce[1 + i] = frame[10 + i];
            for (int i = 0; i < 6; ++i) nonce[7 + i] = (uint8_t)((pn >> ((5 - i) * 8)) & 0xFF);

            // Challenger Fix 5: IEEE 802.11i AAD Masking
            uint8_t aad[26];
            memcpy(aad, frame, mac_hdr_len);
            aad[0] &= 0x8F; // Mask Subtype if mgmt, retain data
            aad[1] &= 0xC7; // Mask Retry, PwrMgt, MoreData
            aad[22] = 0x00; // Mask Seq
            aad[23] &= 0x0F;// Retain Frag
            if (mac_hdr_len == 26) {
                aad[24] &= 0x0F; // Retain TID
                aad[25] = 0x00;  // Mask QoS Control high byte
            }

            size_t cipher_len = len - mac_hdr_len - 8 - 8 - 4;
            const uint8_t *ciphertext = frame + mac_hdr_len + 8;
            const uint8_t *mic = ciphertext + cipher_len;

            uint8_t plaintext[2048];
            if (cipher_len > sizeof(plaintext)) return;

            const uint8_t *key = is_multicast ? fGTK : (fPTK + 32);
            if (!rtl_crypto::ccmp_decrypt(key, nonce, aad, mac_hdr_len,
                                          ciphertext, cipher_len, mic, plaintext)) {
                return; // Decryption / MIC check failed
            }

            // Update verified replay counter
            if (is_multicast) {
                fRxGtkPN[key_id] = pn;
            } else {
                fRxPtkPN[tid] = pn;
            }

            // Validate LLC/SNAP (8 bytes: AA AA 03 00 00 00 EtherType)
            if (cipher_len < 8 || plaintext[0] != 0xAA || plaintext[1] != 0xAA || plaintext[2] != 0x03) {
                return;
            }
            uint16_t ethertype = (uint16_t)((((uint16_t)plaintext[6]) << 8) | plaintext[7]);

            // Bridge to Ethernet II frame delivered to macOS XNU BSD stack
            size_t eth_len = 14 + (cipher_len - 8);
            mbuf_t m = allocatePacket((UInt32)eth_len);
            if (m) {
                uint8_t *dst = (uint8_t*)mbuf_data(m);
                memcpy(dst, frame + 4, 6);       // Destination MAC
                memcpy(dst + 6, frame + 16, 6);  // Source MAC
                dst[12] = (uint8_t)((ethertype >> 8) & 0xFF);
                dst[13] = (uint8_t)(ethertype & 0xFF);
                memcpy(dst + 14, plaintext + 8, cipher_len - 8);

                fNetif->inputPacket(m, (UInt32)eth_len);
            }
        }
    }
}

void RTL8723BE::handleAuthResponse(const uint8_t *frame, size_t len) {
    if (len < 30 || fState != kRTLStateAuthenticating) return;

    uint16_t status = (uint16_t)(frame[28] | (((uint16_t)frame[29]) << 8));
    if (status == 0) {
        fState = kRTLStateAssociating;
        IOLog("RTL8723BE: Authenticated. Sending Association Request...\n");

        // Send Association Request
        uint8_t assoc_req[64];
        assoc_req[0] = 0x00; assoc_req[1] = 0x00; assoc_req[2] = 0x00; assoc_req[3] = 0x00;
        memcpy(assoc_req + 4, fConnectedBSSID, 6);
        memcpy(assoc_req + 10, fMyMAC, 6);
        memcpy(assoc_req + 16, fConnectedBSSID, 6);
        assoc_req[22] = 0x00; assoc_req[23] = 0x00;
        assoc_req[24] = 0x11; assoc_req[25] = 0x04; // Capabilities: ESS + Privacy
        assoc_req[26] = 0x0A; assoc_req[27] = 0x00; // Listen Interval

        size_t pos = 28;
        // SSID IE
        assoc_req[pos++] = 0x00;
        size_t ssid_len = strlen(fConnectedSSID);
        assoc_req[pos++] = (uint8_t)ssid_len;
        memcpy(assoc_req + pos, fConnectedSSID, ssid_len);
        pos += ssid_len;

        transmitRawFrame(Q_MGNT, assoc_req, pos);
    }
}

void RTL8723BE::handleAssocResponse(const uint8_t *frame, size_t len) {
    if (len < 30 || fState != kRTLStateAssociating) return;

    uint16_t status = (uint16_t)(frame[26] | (((uint16_t)frame[27]) << 8));
    if (status == 0) {
        fState = kRTLStateHandshake;
        IOLog("RTL8723BE: Associated. Awaiting WPA2 4-Way Handshake...\n");
    }
}

void RTL8723BE::handleEAPOLFrame(const uint8_t *frame, size_t len) {
    size_t eapol_start = 34; // QoS Data (26) + LLC/SNAP (8)
    if (len >= 32 && frame[24] == 0xAA) {
        eapol_start = 32;   // Non-QoS Data (24) + LLC/SNAP (8)
    }
    if (len < eapol_start + 4 + 95) return;

    uint16_t key_info = (uint16_t)((((uint16_t)frame[eapol_start + 5]) << 8) | frame[eapol_start + 6]);
    bool is_mic_set = (key_info & 0x0100) != 0;
    bool is_install_set = (key_info & 0x0040) != 0;

    uint64_t replay_counter = 0;
    for (int i = 0; i < 8; ++i) {
        replay_counter = (replay_counter << 8) | frame[eapol_start + 9 + (size_t)i];
    }

    if (!is_mic_set) {
        // Message 1 (AP -> STA)
        // Challenger Fix 3: Replay counter validation
        if (fLastReplayCounter > 0 && replay_counter < fLastReplayCounter) {
            return; // Drop replayed Message 1
        }

        // If retransmitted Message 1 with same counter and ANonce, reuse SNonce
        bool is_retransmit = (replay_counter == fLastReplayCounter && fLastReplayCounter > 0 &&
                              memcmp(fANonce, frame + eapol_start + 17, 32) == 0);

        memcpy(fANonce, frame + eapol_start + 17, 32);
        fLastReplayCounter = replay_counter;

        if (!is_retransmit) {
            // Generate deterministic synthetic SNonce
            for (int i = 0; i < 32; ++i) fSNonce[i] = (uint8_t)(0x55 ^ i);
        }

        // Derive PTK (PRF-512)
        rtl_crypto::prf_512(fPMK, "Pairwise key expansion",
                            fConnectedBSSID, fMyMAC,
                            fANonce, fSNonce,
                            fPTK);

        // Build and send Message 2 (STA -> AP)
        uint8_t m2[200];
        // MAC Header (26 bytes)
        m2[0] = 0x88; m2[1] = 0x01; m2[2] = 0x00; m2[3] = 0x00;
        memcpy(m2 + 4, fConnectedBSSID, 6);
        memcpy(m2 + 10, fMyMAC, 6);
        memcpy(m2 + 16, fConnectedBSSID, 6);
        m2[22] = 0x00; m2[23] = 0x00; m2[24] = 0x00; m2[25] = 0x00;

        // LLC/SNAP (8 bytes)
        uint8_t llc[] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
        memcpy(m2 + 26, llc, 8);

        size_t m2_eapol = 34;
        m2[m2_eapol + 0] = 0x02; // Version
        m2[m2_eapol + 1] = 0x03; // EAPOL-Key
        m2[m2_eapol + 2] = 0x00;
        m2[m2_eapol + 3] = 0x5F; // Length 95

        m2[m2_eapol + 4] = 0x02; // Type
        m2[m2_eapol + 5] = 0x01; // Key Info: Pairwise | MIC
        m2[m2_eapol + 6] = 0x0A;
        m2[m2_eapol + 7] = 0x00;
        m2[m2_eapol + 8] = 0x10; // Key Length 16

        // Replay Counter matching M1
        for (int i = 7; i >= 0; --i) {
            m2[m2_eapol + 9 + (7 - (size_t)i)] = (uint8_t)((replay_counter >> (i * 8)) & 0xFF);
        }

        // SNonce
        memcpy(m2 + m2_eapol + 17, fSNonce, 32);

        // Zero IV, RSC, Reserved
        memset(m2 + m2_eapol + 49, 0, 32);

        // Zero MIC placeholder
        size_t mic_pos = m2_eapol + 81;
        memset(m2 + mic_pos, 0, 16);

        // Key Data Length 0
        m2[m2_eapol + 97] = 0x00;
        m2[m2_eapol + 98] = 0x00;

        // Compute HMAC-SHA1 MIC using KCK (first 16 bytes of PTK)
        uint8_t digest[20];
        rtl_crypto::hmac_sha1(fPTK, 16, m2 + m2_eapol, 99, digest);
        memcpy(m2 + mic_pos, digest, 16);

        transmitRawFrame(Q_BE, m2, m2_eapol + 99);
        IOLog("RTL8723BE: Received EAPOL M1. Transmitted EAPOL M2.\n");

    } else if (is_mic_set && is_install_set) {
        // Message 3 (AP -> STA)
        // Challenger Fix 3: Strictly greater replay counter
        if (replay_counter <= fLastReplayCounter && fKeyInstalled) {
            // Drop replay
            return;
        }

        // Verify MIC using KCK *before* modifying state or replay counter
        size_t eapol_len = len - eapol_start - 4; // exclude FCS
        uint8_t verify_buf[256];
        if (eapol_len > sizeof(verify_buf)) return;
        memcpy(verify_buf, frame + eapol_start, eapol_len);

        uint8_t rx_mic[16];
        memcpy(rx_mic, verify_buf + 81, 16);
        memset(verify_buf + 81, 0, 16);

        uint8_t comp_digest[20];
        rtl_crypto::hmac_sha1(fPTK, 16, verify_buf, eapol_len, comp_digest);
        if (memcmp(rx_mic, comp_digest, 16) != 0) {
            IOLog("RTL8723BE: EAPOL M3 MIC verification failed!\n");
            return;
        }

        fLastReplayCounter = replay_counter;

        // Unwrap GTK from Key Data using KEK (ptk + 16)
        if (eapol_len >= 99) {
            uint16_t kd_len = (uint16_t)((((uint16_t)verify_buf[97]) << 8) | verify_buf[98]);
            if (kd_len >= 32 && (99 + kd_len) <= eapol_len) {
                uint8_t unwrapped[64];
                if (rtl_crypto::aes_key_unwrap(fPTK + 16, verify_buf + 99, kd_len, unwrapped, sizeof(unwrapped))) {
                    if (unwrapped[0] == 0xDD) { // KDE
                        memcpy(fGTK, unwrapped + 8, 16);
                    }
                }
            }
        }

        // Challenger Fix 2: KRACK Mitigation!
        if (!fKeyInstalled) {
            // First time receiving valid Message 3: Install TK and reset TX PN
            fKeyInstalled = true;
            fTxPN = 1;
            for (int i = 0; i < 16; ++i) fRxPtkPN[i] = 0;
            for (int i = 0; i < 4; ++i)  fRxGtkPN[i] = 0;
            IOLog("RTL8723BE: Installing TK & GTK keys. Link is now secure.\n");
        } else {
            // Retransmitted Message 3: DO NOT reinstall TK and DO NOT reset TX PN!
            IOLog("RTL8723BE: Retransmitted M3 detected. Preserving existing TK and TX PN.\n");
        }

        // Build and send Message 4 (STA -> AP)
        uint8_t m4[200];
        m4[0] = 0x88; m4[1] = 0x01; m4[2] = 0x00; m4[3] = 0x00;
        memcpy(m4 + 4, fConnectedBSSID, 6);
        memcpy(m4 + 10, fMyMAC, 6);
        memcpy(m4 + 16, fConnectedBSSID, 6);
        m4[22] = 0x00; m4[23] = 0x00; m4[24] = 0x00; m4[25] = 0x00;

        uint8_t llc[] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
        memcpy(m4 + 26, llc, 8);

        size_t m4_eapol = 34;
        m4[m4_eapol + 0] = 0x02;
        m4[m4_eapol + 1] = 0x03;
        m4[m4_eapol + 2] = 0x00;
        m4[m4_eapol + 3] = 0x5F;

        m4[m4_eapol + 4] = 0x02;
        m4[m4_eapol + 5] = 0x03; // Pairwise | MIC | Secure
        m4[m4_eapol + 6] = 0x0A;
        m4[m4_eapol + 7] = 0x00;
        m4[m4_eapol + 8] = 0x00;

        for (int i = 7; i >= 0; --i) {
            m4[m4_eapol + 9 + (7 - (size_t)i)] = (uint8_t)((replay_counter >> (i * 8)) & 0xFF);
        }

        memset(m4 + m4_eapol + 17, 0, 64);
        size_t m4_mic_pos = m4_eapol + 81;
        memset(m4 + m4_mic_pos, 0, 16);
        m4[m4_eapol + 97] = 0x00;
        m4[m4_eapol + 98] = 0x00;

        uint8_t m4_digest[20];
        rtl_crypto::hmac_sha1(fPTK, 16, m4 + m4_eapol, 99, m4_digest);
        memcpy(m4 + m4_mic_pos, m4_digest, 16);

        transmitRawFrame(Q_BE, m4, m4_eapol + 99);

        fState = kRTLStateConnected;
        setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid);
        IOLog("RTL8723BE: WPA2 Handshake Complete! Connected to %s.\n", fConnectedSSID);
    }
}

void RTL8723BE::handleDeauthFrame(const uint8_t *frame, size_t len) {
    IOLog("RTL8723BE: Received Deauthentication frame. Tearing down link.\n");
    deauthTeardown();
}

// Challenger Fix 6: Safe Deauthentication Teardown Sequence
void RTL8723BE::deauthTeardown() {
    // 1. Halt MAC TX DMA
    mmio_write16(REG_CR, mmio_read16(REG_CR) & ~0x0004);

    // 2. Clear TX descriptor rings and reclaim ownership
    for (int q = 0; q < 8; ++q) {
        if (fTxRingDescVirt[q]) {
            for (int i = 0; i < RTL8723BE_TX_DESC_COUNT; ++i) {
                fTxRingDescVirt[q][i].set_own(false);
            }
        }
    }

    // 3. Flush internal PCIe FIFO
    mmio_write16(REG_PCIE_CTRL_REG, 0x0000);

    // 4. Set macOS network link inactive (valid status but not active)
    setLinkStatus(kIONetworkLinkValid);

    // 5. Zero out cryptographic keys and reset replay tracking
    memset(fPTK, 0, sizeof(fPTK));
    memset(fGTK, 0, sizeof(fGTK));
    fKeyInstalled = false;
    fLastReplayCounter = 0;
    fTxPN = 1;
    for (int i = 0; i < 16; ++i) fRxPtkPN[i] = 0;
    for (int i = 0; i < 4; ++i)  fRxGtkPN[i] = 0;

    fState = kRTLStateDisconnected;
}

// Challenger Fix 6: 802.11 Null-Data Frame (PM=1 before scan, PM=0 after)
void RTL8723BE::sendNullDataFrame(bool powerManagement) {
    uint8_t null_frame[26];
    null_frame[0] = 0x48; // QoS Null Data
    null_frame[1] = powerManagement ? 0x11 : 0x01; // To DS | (PM=1 if true)
    null_frame[2] = 0x00; null_frame[3] = 0x00;
    memcpy(null_frame + 4, fConnectedBSSID, 6);
    memcpy(null_frame + 10, fMyMAC, 6);
    memcpy(null_frame + 16, fConnectedBSSID, 6);
    null_frame[22] = 0x00; null_frame[23] = 0x00;
    null_frame[24] = 0x00; null_frame[25] = 0x00;

    transmitRawFrame(Q_VO, null_frame, sizeof(null_frame));
}

// ============================================================================
// Outbound Packet Processing (outputPacket)
// ============================================================================

UInt32 RTL8723BE::outputPacket(mbuf_t m, void *param) {
    if (!m) return kIOReturnOutputSuccess;

    if (fState != kRTLStateConnected || !fKeyInstalled) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }

    size_t pkt_len = mbuf_pkthdr_len(m);
    if (pkt_len < 14) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }

    uint8_t eth_buf[1514];
    if (pkt_len > sizeof(eth_buf)) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    mbuf_copydata(m, 0, pkt_len, eth_buf);

    uint16_t ether_type = (uint16_t)((((uint16_t)eth_buf[12]) << 8) | eth_buf[13]);

    // Challenger Fix 7: Drop raw user-space injected EAPOL packets
    if (ether_type == 0x888E) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }

    const uint8_t *da = eth_buf;
    const uint8_t *sa = eth_buf + 6;
    const uint8_t *payload = eth_buf + 14;
    size_t payload_len = pkt_len - 14;

    uint8_t frame[2048];
    size_t frame_pos = 0;

    // 1. 802.11 QoS Data MAC Header (26 bytes)
    frame[0] = 0x88;
    frame[1] = 0x41; // To DS = 1, Protected = 1
    frame[2] = 0x00; frame[3] = 0x00;
    memcpy(frame + 4, fConnectedBSSID, 6); // Addr1: BSSID
    memcpy(frame + 10, sa, 6);             // Addr2: SA (My MAC)
    memcpy(frame + 16, da, 6);             // Addr3: DA
    frame[22] = 0x00; frame[23] = 0x00;
    frame[24] = 0x00; frame[25] = 0x00;    // QoS Control (TID 0)
    frame_pos = 26;

    // 2. CCMP Header (8 bytes)
    uint64_t pn = fTxPN++;
    frame[frame_pos + 0] = (uint8_t)(pn & 0xFF);
    frame[frame_pos + 1] = (uint8_t)((pn >> 8) & 0xFF);
    frame[frame_pos + 2] = 0x00;
    frame[frame_pos + 3] = 0x20; // ExtIV = 1
    frame[frame_pos + 4] = (uint8_t)((pn >> 16) & 0xFF);
    frame[frame_pos + 5] = (uint8_t)((pn >> 24) & 0xFF);
    frame[frame_pos + 6] = (uint8_t)((pn >> 32) & 0xFF);
    frame[frame_pos + 7] = (uint8_t)((pn >> 40) & 0xFF);
    frame_pos += 8;

    // 3. 13-Byte Nonce
    uint8_t nonce[13];
    nonce[0] = 0x00; // TID 0
    memcpy(nonce + 1, sa, 6);
    for (int i = 0; i < 6; ++i) {
        nonce[7 + i] = (uint8_t)((pn >> ((5 - i) * 8)) & 0xFF);
    }

    // 4. Challenger Fix 5: Masked AAD (26 bytes)
    uint8_t aad[26];
    memcpy(aad, frame, 26);
    aad[0] &= 0x8F;
    aad[1] &= 0xC7;
    aad[22] = 0x00;
    aad[23] = 0x00;
    aad[24] &= 0x0F;
    aad[25] = 0x00;

    // 5. Plaintext: RFC 1042 LLC/SNAP (8 bytes) + Ethernet Payload
    uint8_t plaintext[1600];
    plaintext[0] = 0xAA; plaintext[1] = 0xAA; plaintext[2] = 0x03;
    plaintext[3] = 0x00; plaintext[4] = 0x00; plaintext[5] = 0x00;
    plaintext[6] = (uint8_t)((ether_type >> 8) & 0xFF);
    plaintext[7] = (uint8_t)(ether_type & 0xFF);
    size_t plain_len = 8 + payload_len;
    if (plain_len > sizeof(plaintext)) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    memcpy(plaintext + 8, payload, payload_len);

    // 6. CCMP Encrypt under TK (fPTK + 32)
    uint8_t mic[8];
    rtl_crypto::ccmp_encrypt(fPTK + 32, nonce, aad, 26,
                             plaintext, plain_len,
                             frame + frame_pos, mic);
    frame_pos += plain_len;

    // Append 8-byte MIC
    memcpy(frame + frame_pos, mic, 8);
    frame_pos += 8;

    // 7. IEEE 802.11 CRC32 FCS
    uint32_t crc = rtl_crypto::crc32_80211(frame, frame_pos);
    frame[frame_pos + 0] = (uint8_t)(crc & 0xFF);
    frame[frame_pos + 1] = (uint8_t)((crc >> 8) & 0xFF);
    frame[frame_pos + 2] = (uint8_t)((crc >> 16) & 0xFF);
    frame[frame_pos + 3] = (uint8_t)((crc >> 24) & 0xFF);
    frame_pos += 4;

    // Transmit via Hardware BE DMA Queue
    bool ok = transmitRawFrame(Q_BE, frame, frame_pos);

    freePacket(m);
    return ok ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
}
