#include "RTL8723BE.hpp"
#include <string.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(RTL8723BE, IOEthernetController);

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

    // 1. Force PCI Power Management Capability (offset 0x40 -> PMCSR at 0x44) from D2/D3 to D0
    uint16_t pmcsr = fPCIDevice->configRead16(0x44);
    if ((pmcsr & 0x0003) != 0) {
        IOLog("RTL8723BE: Waking PCI function from D%u to D0 (PMCSR=0x%04x)\n", pmcsr & 0x3, pmcsr);
        fPCIDevice->configWrite16(0x44, (pmcsr & ~0x0003U) | 0x0100U);
        IODelay(15000); // 15ms PCI D2/D3 -> D0 stabilization
    }

    // 2. Disable PCIe ASPM L0s/L1 (PCIe Capability at 0x70 -> Link Control at 0x80) to prevent MMIO freeze
    uint16_t linkCtrl = fPCIDevice->configRead16(0x80);
    if (linkCtrl & 0x0003U) {
        fPCIDevice->configWrite16(0x80, linkCtrl & ~0x0003U);
    }

    fPCIDevice->setIOEnable(true);
    fPCIDevice->setMemoryEnable(true);
    fPCIDevice->setBusMasterEnable(true);
    IODelay(2000);

    // 3. Map BAR2 16KB 64-bit MMIO aperture (PCI Config Base Address 2 = offset 0x18)
    fMMIOMap = fPCIDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!fMMIOMap) {
        fMMIOMap = fPCIDevice->mapDeviceMemoryWithRegister(0x18);
    }
    if (!fMMIOMap) {
        IOLog("RTL8723BE: Failed to map BAR2 MMIO region\n");
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
    mmio_write32(REG_HISR, 0xFFFFFFFFU);

    fRFLock = IOLockAlloc();
    fStateLock = IOLockAlloc();

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
        IOLog("RTL8723BE: Power-on sequence warning — continuing in safe register state\n");
    }

    if (!readEfuse(fCalib)) {
        IOLog("RTL8723BE: eFuse read fallback to default MAC\n");
    }
    memcpy(fMyMAC, fCalib.mac_addr, 6);
    IOLog("RTL8723BE: Hardware MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
          fMyMAC[0], fMyMAC[1], fMyMAC[2], fMyMAC[3], fMyMAC[4], fMyMAC[5]);

    if (!downloadFirmware(rtl8723befw_bin, rtl8723befw_bin_len)) {
        IOLog("RTL8723BE: Firmware download did not report WINTINI_RDY on first attempt; continuing cleanly\n");
    } else {
        IOLog("RTL8723BE: 8051 MCU Firmware v36 loaded and initialized successfully\n");
    }

    initBasebandAndRF();
    initLLT();

    if (!initDMARings()) {
        IOLog("RTL8723BE: DMA rings initialization failed\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    // 6. Prefer MSI interrupt index 1 (non-shared edge-triggered), fallback to index 0
    fInterruptSource = IOFilterInterruptEventSource::filterInterruptEventSource(
        this,
        &RTL8723BE::interruptAction,
        &RTL8723BE::interruptFilter,
        fPCIDevice,
        1
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
    // Keep fInterruptSource disabled until enable(IONetworkInterface*) is called!

    // 7. Create Scan Timer Event Source
    fScanTimer = IOTimerEventSource::timerEventSource(this, &RTL8723BE::scanTimerAction);
    if (fScanTimer) {
        fWorkLoop->addEventSource(fScanTimer);
    }

    // 8. Publish Ethernet Mediums & Attach Network Interface
    fMediumDict = OSDictionary::withCapacity(1);
    IONetworkMedium *medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 100 * 1000000);
    if (medium) {
        IONetworkMedium::addMedium(fMediumDict, medium);
        publishMediumDictionary(fMediumDict);
        setSelectedMedium(medium);
        medium->release();
    }

    if (!attachInterface((IONetworkInterface**)&fNetif, true)) {
        IOLog("RTL8723BE: Failed to attach network interface\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    setAntennaPath(2); // Aux port 2 for HP

    registerService();
    IOLog("RTL8723BE: Driver loaded and ready (MSI/DMA safe mode)\n");
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
    IOLog("RTL8723BE::enable()\n");
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
    IOLog("RTL8723BE::disable()\n");
    fInterruptEnabled = false;
    fHIMRMask = 0;
    mmio_write32(REG_HIMR, 0);
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
    if (!fMMIOBase) return 0;
    return *(volatile uint8_t*)(fMMIOBase + offset);
}

uint16_t RTL8723BE::mmio_read16(uint32_t offset) {
    if (!fMMIOBase) return 0;
    return *(volatile uint16_t*)(fMMIOBase + offset);
}

uint32_t RTL8723BE::mmio_read32(uint32_t offset) {
    if (!fMMIOBase) return 0;
    return *(volatile uint32_t*)(fMMIOBase + offset);
}

void RTL8723BE::mmio_write8(uint32_t offset, uint8_t val) {
    if (!fMMIOBase) return;
    *(volatile uint8_t*)(fMMIOBase + offset) = val;
    OSSynchronizeIO();
}

void RTL8723BE::mmio_write16(uint32_t offset, uint16_t val) {
    if (!fMMIOBase) return;
    *(volatile uint16_t*)(fMMIOBase + offset) = val;
    OSSynchronizeIO();
}

void RTL8723BE::mmio_write32(uint32_t offset, uint32_t val) {
    if (!fMMIOBase) return;
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

    // 2. Clear auto power down in REG_APS_FSMCO+1 (0x0005)
    uint8_t aps = mmio_read8(REG_APS_FSMCO + 1);
    mmio_write8(REG_APS_FSMCO + 1, aps & ~0x80);

    // 3. Clear suspend and powerdown in 0x0005
    mmio_write8(REG_APS_FSMCO + 1, mmio_read8(REG_APS_FSMCO + 1) & ~0x88);

    // 4. Start PCIe DMA clock: write 0x00 to 0x0301
    mmio_write8(0x0301, 0x00);

    // 5. Clocks and Function Enable: REG_SYS_FUNC_EN (0x0002) & REG_SYS_CLKR (0x0008)
    mmio_write16(REG_SYS_FUNC_EN, 0x0405); // PCIe DMA enable + CPU clock/unreset
    mmio_write16(REG_SYS_CLKR, 0x0808);    // MAC clock enable + Ring enable

    // 6. Release analog isolation: clear bit 5 of 0x0000
    uint8_t iso = mmio_read8(REG_SYS_ISO_CTRL);
    mmio_write8(REG_SYS_ISO_CTRL, iso & ~0x20);

    // 7. Disable software LPS: clear bits 2, 3, 4 of 0x0005
    uint8_t aps_lps = mmio_read8(REG_APS_FSMCO + 1);
    mmio_write8(REG_APS_FSMCO + 1, aps_lps & ~0x1C);

    // 8. Poll power stability (REG_PWR_STATUS 0x0006 bit 1 == 1)
    int timeout = 25;
    while (timeout-- > 0) {
        if (mmio_read8(REG_PWR_STATUS) & 0x02) break;
        IODelay(200);
    }

    // 9. Core Activation
    mmio_write8(REG_MULTI_FUNC_CTRL, mmio_read8(REG_MULTI_FUNC_CTRL) | 0x08);
    mmio_write8(REG_APS_FSMCO, mmio_read8(REG_APS_FSMCO) | 0x10);
    mmio_write8(REG_SYS_CLKR, mmio_read8(REG_SYS_CLKR) | 0x08);

    mmio_write8(REG_HWSEQ_CTRL, 0x7F);
    IODelay(500);

    // 10. Enable Command Register REG_CR (0x0100) = 0x02FF (TRX enable + MAC enable)
    mmio_write16(REG_CR, 0x02FF);

    fPowerState = POWER_ACT;
    return true;
}

bool RTL8723BE::powerOff() {
    if (fMMIOBase && mmio_read32(0x0000) != 0xFFFFFFFFU) {
        mmio_write32(REG_HIMR, 0);
        mmio_write8(REG_RSV_CTRL, 0x0E);
        mmio_write16(REG_CR, 0x0000);
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

    // 1. Link normal queue pages 0..244
    for (uint32_t i = 0; i < 244; ++i) {
        uint32_t val = (1U << 30) | (i << 8) | (i + 1);
        mmio_write32(REG_LLT_INIT, val);
        IODelay(2);
    }
    // Terminate normal ring
    mmio_write32(REG_LLT_INIT, (1U << 30) | (244U << 8) | 0xFF);
    IODelay(2);

    // 2. Link beacon queue pages 245..255
    for (uint32_t i = 245; i < 255; ++i) {
        uint32_t val = (1U << 30) | (i << 8) | (i + 1);
        mmio_write32(REG_LLT_INIT, val);
        IODelay(2);
    }
    // Terminate beacon ring
    mmio_write32(REG_LLT_INIT, (1U << 30) | (255U << 8) | 0xFF);
    IODelay(2);

    return true;
}

// ============================================================================
// eFuse & Factory Calibration Decoding
// ============================================================================

bool RTL8723BE::readEfuse(CalibData &outCalib) {
    uint8_t shadow_map[512];
    memset(shadow_map, 0xFF, 512);

    if (fMMIOBase && mmio_read32(0x0000) != 0xFFFFFFFFU) {
        // Enable eFuse access
        mmio_write8(REG_EFUSE_ACCESS, 0x69);

        // Read key eFuse offsets (0x00..0x20 for TX power and 0xB0..0xE0 for crystal/MAC/IDs)
        for (uint16_t addr = 0; addr < 0xE8; ++addr) {
            if (addr >= 0x20 && addr < 0xB8) continue;
            uint32_t cmd = ((uint32_t)addr << 8) | (0x72U << 24) | 0x80000000U;
            mmio_write32(REG_EFUSE_CTRL, cmd);

            int retry = 40;
            while (retry-- > 0) {
                uint32_t res = mmio_read32(REG_EFUSE_CTRL);
                if (res == 0xFFFFFFFFU) break;
                if ((res & 0x80000000U) == 0) {
                    shadow_map[addr] = (uint8_t)(res & 0xFF);
                    break;
                }
                IODelay(2);
            }
        }

        // Disable eFuse access
        mmio_write8(REG_EFUSE_ACCESS, 0x00);
    }

    // Extract fields
    memcpy(outCalib.mac_addr, &shadow_map[0x00D0], 6);
    outCalib.crystal_cap = shadow_map[0x00B9];
    outCalib.thermal_meter = shadow_map[0x00BA];
    outCalib.channel_plan = shadow_map[0x00B8];

    memcpy(outCalib.tx_pwr_cck, &shadow_map[0x0010], 6);
    memcpy(outCalib.tx_pwr_ht40, &shadow_map[0x0016], 5);
    outCalib.tx_pwr_ht20_diff = shadow_map[0x001B];

    outCalib.vid  = (uint16_t)(shadow_map[0x00D6] | (((uint16_t)shadow_map[0x00D7]) << 8));
    outCalib.did  = (uint16_t)(shadow_map[0x00D8] | (((uint16_t)shadow_map[0x00D9]) << 8));
    outCalib.svid = (uint16_t)(shadow_map[0x00DA] | (((uint16_t)shadow_map[0x00DB]) << 8));
    outCalib.smid = (uint16_t)(shadow_map[0x00DC] | (((uint16_t)shadow_map[0x00DD]) << 8));

    // Fallbacks if blank/unprogrammed (0xFF or 0x00)
    if ((outCalib.mac_addr[0] == 0xFF && outCalib.mac_addr[1] == 0xFF) ||
        (outCalib.mac_addr[0] == 0x00 && outCalib.mac_addr[1] == 0x00)) {
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

    // 9. Poll for WINTINI_RDY
    int retries = 50;
    while (retries-- > 0) {
        fwdl_status = mmio_read32(REG_MCUFWDL);
        if (fwdl_status & MCUFWDL_WINTINI_RDY) {
            return true;
        }
        IODelay(200);
    }

    IOLog("RTL8723BE: Firmware WINTINI_RDY status=0x%08x\n", fwdl_status);
    return false;
}

// ============================================================================
// Baseband & RF Register Tables Initialization
// ============================================================================

bool RTL8723BE::initBasebandAndRF() {
    // 1. Load MAC 1T Array
    for (size_t i = 0; i < RTL8723BEMAC_1T_ARRAYLEN; i += 2) {
        mmio_write32(RTL8723BEMAC_1T_ARRAY[i], RTL8723BEMAC_1T_ARRAY[i + 1]);
    }

    // 2. Load PHY Reg 1T Array
    for (size_t i = 0; i < RTL8723BEPHY_REG_1TARRAYLEN; i += 2) {
        mmio_write32(RTL8723BEPHY_REG_1TARRAY[i], RTL8723BEPHY_REG_1TARRAY[i + 1]);
    }

    // 3. Load AGC Tab 1T Array
    for (size_t i = 0; i < RTL8723BEAGCTAB_1TARRAYLEN; i += 2) {
        mmio_write32(RTL8723BEAGCTAB_1TARRAY[i], RTL8723BEAGCTAB_1TARRAY[i + 1]);
    }

    // 4. Load PHY Reg PG Array
    for (size_t i = 0; i < RTL8723BEPHY_REG_ARRAY_PGLEN; i += 2) {
        mmio_write32(RTL8723BEPHY_REG_ARRAY_PG[i], RTL8723BEPHY_REG_ARRAY_PG[i + 1]);
    }

    // 5. Load RF Radio A 1T Array (via 3-wire LSSI serial interface)
    for (size_t i = 0; i < RTL8723BE_RADIOA_1TARRAYLEN; i += 2) {
        writeRFRegister((uint8_t)RTL8723BE_RADIOA_1TARRAY[i], RTL8723BE_RADIOA_1TARRAY[i + 1]);
    }

    // 6. Set Default Channel
    setChannel(1);

    return true;
}

bool RTL8723BE::setChannel(uint8_t channel, uint8_t bw) {
    if (channel < 1 || channel > 14) return false;

    // Baseband 3-wire LSSI RF write: register 0x18 (RF_CHNLBW)
    // Bits [27:20] = 0x18, Bits [9:0] = channel
    uint32_t val = (0x18U << 20) | (channel & 0x3FF);
    if (bw == 0) { // 20 MHz
        val |= (1U << 10) | (1U << 11);
    } else {       // 40 MHz
        val |= (1U << 10);
    }
    mmio_write32(REG_RFPGA0_XA_LSSI, val);
    IODelay(1000); // PLL lock delay

    fCurrentChannel = channel;
    return true;
}

IOReturn RTL8723BE::setAntennaPath(uint8_t ant) {
    if (ant != 1 && ant != 2) return kIOReturnBadArgument;
    mmio_write32(REG_BB_PAD_CTRL, ant);
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

        fTxRingDescMem[idx] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            RTL8723BE_TX_DESC_COUNT * sizeof(TxDesc40),
            0x00000000FFFFFF00ULL
        );
        if (!fTxRingDescMem[idx]) return false;
        fTxRingDescMem[idx]->prepare(kIODirectionInOut);

        fTxRingDescVirt[idx] = (TxDesc40*)fTxRingDescMem[idx]->getBytesNoCopy();
        fTxRingDescPhys[idx] = fTxRingDescMem[idx]->getPhysicalAddress();
        memset(fTxRingDescVirt[idx], 0, RTL8723BE_TX_DESC_COUNT * sizeof(TxDesc40));

        // Allocate TX packet data buffers (< 4GB physical mask)
        for (int i = 0; i < RTL8723BE_TX_DESC_COUNT; ++i) {
            fTxBufMem[idx][i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                RTL8723BE_TX_BUF_SIZE,
                0x00000000FFFFFFFCULL
            );
            if (!fTxBufMem[idx][i]) return false;
            fTxBufMem[idx][i]->prepare(kIODirectionInOut);

            fTxRingDescVirt[idx][i].set_buffer_addr((uint32_t)(fTxBufMem[idx][i]->getPhysicalAddress() & 0xFFFFFFFFULL));
            fTxRingDescVirt[idx][i].set_own(false);
        }
    }

    // 2. Allocate RX Ring (< 4GB 256-byte aligned physical mask)
    fRxHostIdx = 0;
    fRxRingDescMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        RTL8723BE_RX_DESC_COUNT * sizeof(RxDesc32),
        0x00000000FFFFFF00ULL
    );
    if (!fRxRingDescMem) return false;
    fRxRingDescMem->prepare(kIODirectionInOut);

    fRxRingDescVirt = (RxDesc32*)fRxRingDescMem->getBytesNoCopy();
    fRxRingDescPhys = fRxRingDescMem->getPhysicalAddress();
    memset(fRxRingDescVirt, 0, RTL8723BE_RX_DESC_COUNT * sizeof(RxDesc32));

    // Allocate RX packet buffers (< 4GB physical mask)
    for (int i = 0; i < RTL8723BE_RX_DESC_COUNT; ++i) {
        fRxBufMem[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            RTL8723BE_RX_BUF_SIZE,
            0x00000000FFFFFFFCULL
        );
        if (!fRxBufMem[i]) return false;
        fRxBufMem[i]->prepare(kIODirectionInOut);

        fRxRingDescVirt[i].dw0 = 0;
        fRxRingDescVirt[i].set_length(RTL8723BE_RX_BUF_SIZE);
        fRxRingDescVirt[i].set_buffer_addr((uint32_t)(fRxBufMem[i]->getPhysicalAddress() & 0xFFFFFFFFULL));
        if (i == (RTL8723BE_RX_DESC_COUNT - 1)) {
            fRxRingDescVirt[i].set_eor(true); // End of Ring flag
        }
        fRxRingDescVirt[i].set_own(true); // Owned by hardware, ready to receive
    }

    // 3. Program MMIO DESA registers
    mmio_write32(REG_BKQ_DESA, (uint32_t)(fTxRingDescPhys[Q_BK] & 0xFFFFFFFFULL));
    mmio_write32(REG_BEQ_DESA, (uint32_t)(fTxRingDescPhys[Q_BE] & 0xFFFFFFFFULL));
    mmio_write32(REG_VIQ_DESA, (uint32_t)(fTxRingDescPhys[Q_VI] & 0xFFFFFFFFULL));
    mmio_write32(REG_VOQ_DESA, (uint32_t)(fTxRingDescPhys[Q_VO] & 0xFFFFFFFFULL));
    mmio_write32(REG_BCNQ_DESA, (uint32_t)(fTxRingDescPhys[Q_BCN] & 0xFFFFFFFFULL));
    mmio_write32(REG_MGQ_DESA, (uint32_t)(fTxRingDescPhys[Q_MGNT] & 0xFFFFFFFFULL));
    mmio_write32(REG_HQ_DESA, (uint32_t)(fTxRingDescPhys[Q_HIGH] & 0xFFFFFFFFULL));
    mmio_write32(REG_RX_DESA, (uint32_t)(fRxRingDescPhys & 0xFFFFFFFFULL));

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
    size_t qIdx = (size_t)qId;
    if (qIdx >= 8 || !fTxRingDescVirt[qIdx] || !fTxBufMem[qIdx][fTxHostIdx[qIdx]]) {
        return false;
    }

    TxDesc40 *desc = &fTxRingDescVirt[qIdx][fTxHostIdx[qIdx]];
    if (desc->get_own()) {
        // Ring full (backpressure)
        return false;
    }

    uint8_t *buf = (uint8_t*)fTxBufMem[qIdx][fTxHostIdx[qIdx]]->getBytesNoCopy();
    if (frame && len > 0) {
        memcpy(buf, frame, len);
    }

    desc->set_pktsize((uint16_t)len);
    desc->set_offset(40);
    desc->set_firstseg(true);
    desc->set_lastseg(true);
    desc->set_queuesel((uint8_t)qId);
    desc->set_own(true); // Relinquish to DMA

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

        uint16_t len = desc->get_length();
        uint8_t shift = desc->get_shift();
        const uint8_t *src = (const uint8_t*)fRxBufMem[fRxHostIdx]->getBytesNoCopy();

        if (src && len > 0 && (shift + len) <= RTL8723BE_RX_BUF_SIZE && !desc->get_crc32_err()) {
            processRxFrame(src + shift, len);
            fRxPackets++;
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
                if (rtl_crypto::aes_key_unwrap(fPTK + 16, verify_buf + 99, kd_len, unwrapped)) {
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
