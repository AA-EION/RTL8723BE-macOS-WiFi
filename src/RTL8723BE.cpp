// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Issen Software Group
// Ported from Linux drivers/net/wireless/realtek/rtlwifi (rtl8723be, rtl8723com, efuse.c),
// Copyright (c) 2009-2014 Realtek Corporation (licensed GPL-2.0).

/*-
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002, 2003 Sam Leffler, Errno Consulting
 * Copyright (c) 2007-2009 Damien Bergamini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "RTL8723BE.hpp"
#include <string.h>
#include <sys/random.h>
#include <pexpert/pexpert.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(RTL8723BE, IOEthernetController);

// Hardware-init stages reported through the diagnostics interface and kext log.
enum {
    kStageNone = 0,
    kStageMapped,
    kStageEfuse,
    kStageRings,
    kStagePowerOn,
    kStageMac,
    kStageFirmware,
    kStageMacTable,
    kStageBB,
    kStageRF,
    kStageConfigured,
    kStageReady
};

// Gated operations dispatched through gatedCall().
enum {
    kOpScanResults = 1,
    kOpConnect,
    kOpDisconnect,
    kOpStatus,
    kOpAntenna,
    kOpReadReg,
    kOpWriteReg,
    kOpReadRF,
    kOpWriteRF,
    kOpDiag,
    kOpIQK,
    kOpLinkInfo,
    kOpOptions
};

static inline uint32_t bitShift(uint32_t mask) {
    return mask ? (uint32_t)__builtin_ctz(mask) : 0;
}

static void rtlDelayMs(uint32_t ms) {
    // All callers run in thread context (start(), gated user-client calls).
    IOSleep(ms);
}

// ============================================================================
// Linux rtl8723be power sequences (pwrseq.h), rows that apply to PCIe only.
// ============================================================================

enum { kPwrWrite = 1, kPwrPoll = 2, kPwrDelay = 3 };

// RTL8723B_TRANS_CARDDIS_TO_CARDEMU + RTL8723B_TRANS_CARDEMU_TO_ACT
static const RTLPwrCmd kCardEnableFlow[] = {
    {0x0005, kPwrWrite, 0x88, 0x00},  // clear SUS_EN / APDM_EN
    {0x0005, kPwrWrite, 0x18, 0x00},  // 0x04[12:11] = 0, disable WL suspend
    {0x0301, kPwrWrite, 0xFF, 0x00},  // PCIe DMA start
    {0x0005, kPwrWrite, 0x1C, 0x00},  // disable SW LPS and WLSUS_EN
    {0x0075, kPwrWrite, 0x01, 0x01},  // disable USB suspend
    {0x0006, kPwrPoll,  0x02, 0x02},  // wait 0x04[17] power ready
    {0x0075, kPwrWrite, 0x01, 0x00},  // enable USB suspend
    {0x0006, kPwrWrite, 0x01, 0x01},  // release WLON reset
    {0x0005, kPwrWrite, 0x80, 0x00},  // disable HWPDN
    {0x0005, kPwrWrite, 0x18, 0x00},  // disable WL suspend
    {0x0005, kPwrWrite, 0x01, 0x01},  // APFM_ONMAC
    {0x0005, kPwrPoll,  0x01, 0x00},  // wait for MAC power-on state machine
    {0x0010, kPwrWrite, 0x40, 0x40},  // WL controls XTAL setting
    {0x0049, kPwrWrite, 0x02, 0x02},  // falling edge interrupt
    {0x0063, kPwrWrite, 0x02, 0x02},  // GPIO9 interrupt mode
    {0x0062, kPwrWrite, 0x02, 0x00},  // GPIO9 input mode
    {0x0058, kPwrWrite, 0x01, 0x01},  // HSISR GPIO[C:0] interrupt
    {0x005A, kPwrWrite, 0x02, 0x02},  // HSISR GPIO9 interrupt
    {0x0068, kPwrWrite, 0x08, 0x08},  // GPIO9 pull-high (test chip row, harmless)
    {0x0069, kPwrWrite, 0x40, 0x40},  // GPIO9 pull-high
};

// RTL8723B_TRANS_ACT_TO_LPS
static const RTLPwrCmd kLpsEnterFlow[] = {
    {0x0301, kPwrWrite, 0xFF, 0xFF},  // PCIe DMA stop
    {0x0522, kPwrWrite, 0xFF, 0xFF},  // TX pause
    {0x05F8, kPwrPoll,  0xFF, 0x00},
    {0x05F9, kPwrPoll,  0xFF, 0x00},
    {0x05FA, kPwrPoll,  0xFF, 0x00},
    {0x05FB, kPwrPoll,  0xFF, 0x00},
    {0x0002, kPwrWrite, 0x01, 0x00},  // CCK/OFDM off
    {0x0002, kPwrDelay, 0x00, 0x00},
    {0x0002, kPwrWrite, 0x02, 0x00},  // BB reset
    {0x0100, kPwrWrite, 0xFF, 0x03},  // reset MAC TRX
    {0x0101, kPwrWrite, 0x02, 0x00},
    {0x0553, kPwrWrite, 0x20, 0x20},
};

// RTL8723B_TRANS_ACT_TO_CARDEMU + RTL8723B_TRANS_CARDEMU_TO_PDN
static const RTLPwrCmd kCardDisableFlow[] = {
    {0x001F, kPwrWrite, 0xFF, 0x00},  // RF off
    {0x004F, kPwrWrite, 0x01, 0x00},
    {0x0049, kPwrWrite, 0x02, 0x00},
    {0x0005, kPwrWrite, 0x02, 0x02},  // turn off MAC by HW state machine
    {0x0005, kPwrPoll,  0x02, 0x00},
    {0x0010, kPwrWrite, 0x40, 0x00},  // BT controls XTAL
    {0x0006, kPwrWrite, 0x01, 0x00},
    {0x0005, kPwrWrite, 0x80, 0x80},
};

// ============================================================================
// DMA buffers
// ============================================================================

bool RTL8723BE::allocDMA(RTLDmaBuf &buf, size_t size, size_t alignment) {
    memset(&buf, 0, sizeof(buf));
    buf.mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        size, 0xFFFFFFFFULL & ~(uint64_t(alignment) - 1));
    if (!buf.mem) return false;

    buf.cmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, 32, size,
                                              IODMACommand::kMapped, size, (UInt32)alignment);
    if (!buf.cmd || buf.cmd->setMemoryDescriptor(buf.mem, true) != kIOReturnSuccess) {
        freeDMA(buf);
        return false;
    }

    IODMACommand::Segment64 seg;
    UInt32 numSeg = 1;
    UInt64 offset = 0;
    if (buf.cmd->gen64IOVMSegments(&offset, &seg, &numSeg) != kIOReturnSuccess ||
        numSeg != 1 || seg.fLength < size ||
        !rtlDmaRangeValid(seg.fIOVMAddr, size, alignment)) {
        freeDMA(buf);
        return false;
    }
    buf.ioaddr = seg.fIOVMAddr;
    buf.virt = buf.mem->getBytesNoCopy();
    buf.size = size;
    if (!buf.virt) {
        freeDMA(buf);
        return false;
    }
    memset(buf.virt, 0, size);
    return true;
}

void RTL8723BE::freeDMA(RTLDmaBuf &buf) {
    if (buf.cmd) {
        buf.cmd->clearMemoryDescriptor();
        buf.cmd->release();
    }
    if (buf.mem) {
        buf.mem->release();
    }
    memset(&buf, 0, sizeof(buf));
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
    fCommandGate = nullptr;
    fInterruptSource = nullptr;
    fScanTimer = nullptr;
    fLinkTimer = nullptr;
    fLinkTicks = fLastBSSActivity = fHandshakeDeadline = 0;
    fLinkRSSI = -100;
    fNetif = nullptr;
    fMediumDict = nullptr;
    fRFLock = nullptr;
    fTxLock = nullptr;

    fPowerState = POWER_CARDDIS;
    fHIMRMask = 0;
    fInterruptEnabled = false;
    fHwReady = false;
    fPMRegistered = fSuspended = fInterfaceEnabled = false;
    fFwReady = false;
    memset(&fCalib, 0, sizeof(fCalib));
    fInitStage = kStageNone;
    fRFChnlVal = 0;
    fFwVersion = 0;
    fFwSubVersion = 0;
    fCutVersion = 0;
    fCurrentChannel = 1;
    fAntNum = 2;
    fActiveAntenna = 1;
    fAntennaForced = false;
    fOemHP = false;

    for (int i = 0; i < 8; ++i) {
        memset(&fTxRing[i], 0, sizeof(fTxRing[i]));
        fTxRingDescVirt[i] = nullptr;
        fTxHostIdx[i] = 0;
        for (int j = 0; j < RTL8723BE_TX_DESC_COUNT; ++j) {
            memset(&fTxBuf[i][j], 0, sizeof(fTxBuf[i][j]));
        }
    }
    memset(&fRxRing, 0, sizeof(fRxRing));
    fRxRingDescVirt = nullptr;
    fRxHostIdx = 0;
    for (int j = 0; j < RTL8723BE_RX_DESC_COUNT; ++j) {
        memset(&fRxBuf[j], 0, sizeof(fRxBuf[j]));
    }

    fOptions = kRTLOptionHT;
    int bluetooth = 0;
    if (PE_parse_boot_argn("rtl8723be_bluetooth", &bluetooth, sizeof(bluetooth)) && bluetooth == 1)
        fOptions |= kRTLOptionBluetoothCoexist;
    fTxCrypto = false;
    fState = kRTLStateDisconnected;
    memset(fMyMAC, 0, 6);
    memset(fConnectedBSSID, 0, 6);
    memset(fConnectedSSID, 0, sizeof(fConnectedSSID));
    fTxSeq = 0;
    fAid = 0;
    fNetStats = nullptr;

    memset(fPMK, 0, sizeof(fPMK));
    memset(fPTK, 0, sizeof(fPTK));
    fWPA.reset();
    memset(&fPeer, 0, sizeof(fPeer));
    fDataRate = DESC_RATE1M;
    fRateID = 8;
    fFirmwareRA = false;

    fKeyInstalled = false;
    fTxPN = 1;
    for (int i = 0; i < 16; ++i) fRxPtkPN[i] = 0;
    memset(fRxGtkPN, 0, sizeof(fRxGtkPN));

    memset(&fScanResults, 0, sizeof(fScanResults));
    memset(fScanPeers, 0, sizeof(fScanPeers));
    fScanActive = false;
    fScanRetriedOtherAntenna = false;
    fAntennaCalibrated = false;
    fPassMgmtStart = 0;
    fPass1Score = 0;
    fScanCurrentChannel = 1;
    fHomeChannel = 1;

    fTxPackets = 0;
    fRxPackets = 0;
    fTxErrors = 0;
    fRxErrors = 0;
    fIntCount = 0;
    fRxDescDone = 0;
    fRxMgmt = fRxBeacons = fRxData = fRxCtrl = fRxDropped = 0;
    fTxRingFull = 0;
    memset(fRxOk, 0, sizeof(fRxOk));
    memset(fRxCrc, 0, sizeof(fRxCrc));
    fUcDataRx = fUcDelivered = fUcReplay = fUcMicFail = fBcDelivered = fBcMicFail = 0;
    fLastUcFcPn = fLastUcRateLen = 0;
    fRxBytes = fTxBytes = 0;
    fCurRxRate = 0;
    fLastHmeBox = 0;

    return true;
}

void RTL8723BE::cleanupResources() {
    if (fPMRegistered) { fPMRegistered = false; PMstop(); }
    rtl_wifi::erase(fPMK, sizeof(fPMK));
    rtl_wifi::erase(fPTK, sizeof(fPTK));
    fWPA.reset();
    rtl_wifi::erase(fTxPlain, sizeof(fTxPlain));
    rtl_wifi::erase(fRxPlain, sizeof(fRxPlain));
    fScanActive = false;
    if (fCommandGate) {
        fCommandGate->commandWakeup(&fScanActive);
    }

    if (fScanTimer) {
        fScanTimer->cancelTimeout();
        if (fWorkLoop) fWorkLoop->removeEventSource(fScanTimer);
        fScanTimer->release();
        fScanTimer = nullptr;
    }
    if (fLinkTimer) {
        fLinkTimer->cancelTimeout();
        if (fWorkLoop) fWorkLoop->removeEventSource(fLinkTimer);
        fLinkTimer->release();
        fLinkTimer = nullptr;
    }

    disableInterrupts();
    if (fInterruptSource) {
        fInterruptSource->disable();
        if (fWorkLoop) fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = nullptr;
    }

    if (fMMIOBase && fInitStage >= kStagePowerOn) {
        hwDisable();
    }
    if (fPCIDevice) {
        // Disabling MAC alone does not revoke PCI DMA access to host memory.
        fPCIDevice->setBusMasterEnable(false);
        (void)fPCIDevice->configRead16(kIOPCIConfigCommand);
        IOSleep(1);
    }
    freeDMARings();

    fNetStats = nullptr;
    if (fNetif) {
        // attachInterface() hands back a retained object; without this release the
        // interface keeps the controller alive and the kext can never unload.
        detachInterface(fNetif);
        fNetif->release();
        fNetif = nullptr;
    }

    if (fMediumDict) {
        fMediumDict->release();
        fMediumDict = nullptr;
    }

    if (fCommandGate) {
        if (fWorkLoop) fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = nullptr;
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
    if (fTxLock) {
        IOLockFree(fTxLock);
        fTxLock = nullptr;
    }
    fHwReady = false;
    fInitStage = kStageNone;
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

    // Bring the function to D0 and keep ASPM off (Linux disables ASPM for hw_init).
    UInt8 pmCap = 0;
    fPCIDevice->findPCICapability(kIOPCIPowerManagementCapability, &pmCap);
    if (pmCap) {
        uint16_t pmcsr = fPCIDevice->configRead16(pmCap + 4);
        if ((pmcsr & 3) != 0) {
            fPCIDevice->configWrite16(pmCap + 4, pmcsr & ~0x8003U);
            IOSleep(15);
        }
    }
    UInt8 pcieCap = 0;
    fPCIDevice->findPCICapability(kIOPCIPCIExpressCapability, &pcieCap);
    if (pcieCap) {
        uint16_t linkCtrl = fPCIDevice->configRead16(pcieCap + 0x10);
        fPCIDevice->configWrite16(pcieCap + 0x10, linkCtrl & ~3U);
    }

    fPCIDevice->setIOEnable(true);
    fPCIDevice->setMemoryEnable(true);
    IOSleep(2);

    // Map BAR2 (16 KB memory aperture; BAR0 is I/O space on this part).
    fMMIOMap = fPCIDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!fMMIOMap || fMMIOMap->getLength() < 0x4000 || !fMMIOMap->getVirtualAddress()) {
        IOLog("RTL8723BE: Failed to map BAR2 MMIO region\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }
    fMMIOBase = (volatile uint8_t*)fMMIOMap->getVirtualAddress();
    if (mmio_read32(REG_SYS_ISO_CTRL) == 0xFFFFFFFFU) {
        IOLog("RTL8723BE: BAR2 reads 0xFFFFFFFF; device not responding\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }
    fInitStage = kStageMapped;

    // Mask all interrupts and clear sticky status before anything else.
    mmio_write32(REG_HIMR, 0);
    mmio_write32(REG_HIMRE, 0);
    mmio_write32(REG_HISR, 0xFFFFFFFFU);
    mmio_write32(REG_HISRE, 0xFFFFFFFFU);

    fRFLock = IOLockAlloc();
    fTxLock = IOLockAlloc();
    fWorkLoop = getWorkLoop();
    if (!fRFLock || !fTxLock || !fWorkLoop) {
        cleanupResources();
        super::stop(provider);
        return false;
    }
    fWorkLoop->retain();

    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate || fWorkLoop->addEventSource(fCommandGate) != kIOReturnSuccess) {
        cleanupResources();
        super::stop(provider);
        return false;
    }

    // A previous driver instance may have locked ISO/CLK/power registers (RSV_CTRL = 0x0E).
    mmio_write8(REG_RSV_CTRL, 0x00);

    // Chip cut version ([15:12] of SYS_CFG) is used by table conditions.
    fCutVersion = (uint8_t)((mmio_read32(REG_SYS_CFG) >> 12) & 0x0F);

    readEfuse(fCalib);
    memcpy(fMyMAC, fCalib.mac_addr, 6);
    fInitStage = kStageEfuse;
    IOLog("RTL8723BE: eFuse %s (used %u bytes) MAC %02x:%02x:%02x:%02x:%02x:%02x xtal=0x%02x "
          "svid=%04x smid=%04x bt=0x%02x pkg=%u cut=%u\n",
          fCalib.autoload_ok ? "OK" : "INVALID", fCalib.efuse_used,
          fMyMAC[0], fMyMAC[1], fMyMAC[2], fMyMAC[3], fMyMAC[4], fMyMAC[5],
          fCalib.crystal_cap, fCalib.svid, fCalib.smid, fCalib.bt_setting,
          fCalib.package_type, fCutVersion);

    // Antenna: eFuse 0xC3 (bit0 = two antennas, bit6 = single-antenna path is AUX),
    // overridable like Linux's ant_sel module parameter: rtl8723be_ant_sel=1|2.
    fAntNum = (fCalib.autoload_ok && (fCalib.bt_setting & 0x01) == 0) ? 1 : 2;
    fActiveAntenna = (fAntNum == 1 && fCalib.autoload_ok && (fCalib.bt_setting & 0x40)) ? 2 : 1;
    int antSel = 0;
    if (PE_parse_boot_argn("rtl8723be_ant_sel", &antSel, sizeof(antSel)) && (antSel == 1 || antSel == 2)) {
        fAntNum = (antSel == 1) ? 1 : 2;
        fActiveAntenna = (antSel == 1) ? 2 : 1;
        fAntennaForced = true;
    }
    fOemHP = fCalib.autoload_ok && fCalib.customer_id == 0 && fCalib.did == 0x8176 &&
             fCalib.svid == 0x103C && fCalib.smid == 0x1629;

    if (!initDMARings()) {
        IOLog("RTL8723BE: DMA ring allocation failed\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }
    fInitStage = kStageRings;

    // Rings are valid; the NIC may now master the bus (Linux: pci_set_master before hw_init).
    fPCIDevice->setBusMasterEnable(true);

    if (!hwInit()) {
        IOLog("RTL8723BE: Hardware initialization failed at stage %u; aborting\n", fInitStage);
        cleanupResources();
        super::stop(provider);
        return false;
    }

    fInterruptSource = nullptr;
    int irqType = 0;
    for (int index = 0; fPCIDevice->getInterruptType(index, &irqType) == kIOReturnSuccess; ++index) {
        if (irqType & kIOInterruptTypePCIMessaged) {
            fInterruptSource = IOFilterInterruptEventSource::filterInterruptEventSource(
                this, &RTL8723BE::interruptAction, &RTL8723BE::interruptFilter, fPCIDevice, index);
            if (fInterruptSource) break;
        }
    }
    if (!fInterruptSource) {
        fInterruptSource = IOFilterInterruptEventSource::filterInterruptEventSource(
            this, &RTL8723BE::interruptAction, &RTL8723BE::interruptFilter, fPCIDevice, 0);
    }
    if (!fInterruptSource || fWorkLoop->addEventSource(fInterruptSource) != kIOReturnSuccess) {
        IOLog("RTL8723BE: Failed to register interrupt event source\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    fScanTimer = IOTimerEventSource::timerEventSource(this, &RTL8723BE::scanTimerAction);
    if (!fScanTimer || fWorkLoop->addEventSource(fScanTimer) != kIOReturnSuccess) {
        cleanupResources();
        super::stop(provider);
        return false;
    }
    fLinkTimer = IOTimerEventSource::timerEventSource(this, &RTL8723BE::linkTimerAction);
    if (!fLinkTimer || fWorkLoop->addEventSource(fLinkTimer) != kIOReturnSuccess) {
        cleanupResources();
        super::stop(provider);
        return false;
    }

    fMediumDict = OSDictionary::withCapacity(1);
    if (!fMediumDict) {
        cleanupResources();
        super::stop(provider);
        return false;
    }
    IONetworkMedium *medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 54 * 1000000);
    if (medium) {
        IONetworkMedium::addMedium(fMediumDict, medium);
        publishMediumDictionary(fMediumDict);
        setSelectedMedium(medium);
        medium->release();
    }

    if (!attachInterface((IONetworkInterface**)&fNetif, false)) {
        IOLog("RTL8723BE: Failed to attach network interface\n");
        cleanupResources();
        super::stop(provider);
        return false;
    }

    // Radio is up and receiving from here on; the stack only gates the data path.
    fInterruptSource->enable();
    enableInterrupts();
    refreshLED(true);
    fLinkTimer->setTimeoutMS(1000);

    setLinkStatus(kIONetworkLinkValid, nullptr);
    fNetif->registerService();
    static IOPMPowerState powerStates[2] = {
        {1,0,0,0,0,0,0,0,0,0,0,0},
        {1,IOPMDeviceUsable | IOPMPowerOn,IOPMPowerOn,IOPMPowerOn,0,0,0,0,0,0,0,0}
    };
    PMinit();
    fPMRegistered = true;
    registerPowerDriver(this, powerStates, 2);
    provider->joinPMtree(this);
    makeUsable();
    registerService();
    IOLog("RTL8723BE: Driver v1.4.0 ready (fw v%u.%u, %u-ant, path %s%s, RCR=0x%08x, RF18=0x%05x)\n",
          fFwVersion, fFwSubVersion, fAntNum, fActiveAntenna == 2 ? "AUX/S0" : "MAIN/S1",
          fAntennaForced ? " forced" : "", mmio_read32(REG_RCR), fRFChnlVal);
    return true;
}

void RTL8723BE::stop(IOService *provider) {
    if (fPMRegistered) { fPMRegistered = false; PMstop(); }
    // Serialize with any in-flight user-client call before tearing down event sources.
    if (fCommandGate) {
        fCommandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &RTL8723BE::quiesce));
    }
    cleanupResources();
    super::stop(provider);
}

IOReturn RTL8723BE::quiesce(void *, void *, void *, void *) {
    fHwReady = false;
    disableInterrupts();
    if (fScanTimer) fScanTimer->cancelTimeout();
    if (fLinkTimer) fLinkTimer->cancelTimeout();
    fScanActive = false;
    fCommandGate->commandWakeup(&fScanActive);
    return kIOReturnSuccess;
}

void RTL8723BE::free() {
    cleanupResources();
    super::free();
}

IOReturn RTL8723BE::setPowerState(unsigned long state, IOService *device) {
    (void)device;
    if (fPMRegistered && fCommandGate) fCommandGate->runAction(&RTL8723BE::gatedPower, (void *)(uintptr_t)state);
    return IOPMAckImplied;
}

IOReturn RTL8723BE::gatedPower(OSObject *owner, void *state, void *, void *, void *) {
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (!me || !me->fPMRegistered || !me->fPCIDevice || !me->fMMIOBase) return kIOReturnNotReady;
    bool awake = (uintptr_t)state != 0;
    if (!awake && !me->fSuspended) {
        if (IOOutputQueue *q = me->getOutputQueue()) { q->stop(); q->flush(); }
        me->fScanTimer->cancelTimeout();
        me->fLinkTimer->cancelTimeout();
        me->fScanActive = false;
        me->fCommandGate->commandWakeup(&me->fScanActive);
        if (me->fHwReady) me->deauthTeardown();
        // Linux rtl_op_stop()/rtl_pci_stop() shuts the MAC down before PCI suspend.
        me->hwDisable();
        me->fInterruptSource->disable();
        me->fPCIDevice->setBusMasterEnable(false);
        (void)me->fPCIDevice->configRead16(kIOPCIConfigCommand);
        me->fSuspended = true;
        IOLog("RTL8723BE: radio suspended; keys discarded\n");
    } else if (awake && me->fSuspended) {
        me->fPCIDevice->setMemoryEnable(true);
        UInt8 capability = 0;
        if (me->fPCIDevice->findPCICapability(kIOPCIPowerManagementCapability, &capability)) {
            uint16_t control = me->fPCIDevice->configRead16(capability + 4);
            me->fPCIDevice->configWrite16(capability + 4, control & ~0x8003U);
            IOSleep(15);
        }
        if (me->fPCIDevice->findPCICapability(kIOPCIPCIExpressCapability, &capability)) {
            uint16_t control = me->fPCIDevice->configRead16(capability + 0x10);
            me->fPCIDevice->configWrite16(capability + 0x10, control & ~3U);
        }
        if (me->mmio_read32(REG_SYS_ISO_CTRL) == 0xffffffffU) return kIOReturnNotReady;
        me->resetRings();
        me->fPCIDevice->setBusMasterEnable(true);
        // Linux's resume path restarts the adapter; never reuse pre-sleep DMA/keys.
        if (!me->hwInit()) {
            me->hwDisable();
            me->fPCIDevice->setBusMasterEnable(false);
            IOLog("RTL8723BE: wake initialization failed; radio remains unavailable\n");
            return kIOReturnNotReady;
        }
        me->fSuspended = false;
        me->fLastBSSActivity = me->fLinkTicks;
        me->fInterruptSource->enable();
        me->enableInterrupts();
        me->fLinkTimer->setTimeoutMS(1000);
        if (me->fInterfaceEnabled) if (IOOutputQueue *q = me->getOutputQueue()) q->start();
        IOLog("RTL8723BE: radio reinitialized after wake; ready for application auto-join\n");
    }
    return kIOReturnSuccess;
}

// ============================================================================
// IOEthernetController Interface Implementation
// ============================================================================

// Serialize outputPacket() on the work loop (TX scratch buffers, CCMP PN, TX ring).
IOOutputQueue *RTL8723BE::createOutputQueue() {
    return IOGatedOutputQueue::withTarget(this, getWorkLoop(), 256);
}

bool RTL8723BE::configureInterface(IONetworkInterface *netif) {
    if (!super::configureInterface(netif)) return false;
    IONetworkData *data = netif->getParameter(kIONetworkStatsKey);
    fNetStats = data ? (IONetworkStats *)data->getBuffer() : nullptr;
    return true;
}

IOReturn RTL8723BE::enable(IONetworkInterface *netif) {
    (void)netif;
    if (!fHwReady) return kIOReturnNotReady;
    fInterfaceEnabled = true;
    if (IOOutputQueue *q = getOutputQueue()) {
        q->setCapacity(256);
        q->start();
    }
    // Data path only; the radio stays up for scanning regardless of interface state.
    setLinkStatus(fState == kRTLStateConnected ? (kIONetworkLinkValid | kIONetworkLinkActive)
                                               : kIONetworkLinkValid, nullptr);
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::disable(IONetworkInterface *netif) {
    (void)netif;
    fInterfaceEnabled = false;
    if (IOOutputQueue *q = getOutputQueue()) {
        q->stop();
        q->flush();
    }
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::getHardwareAddress(IOEthernetAddress *addrP) {
    if (!addrP) return kIOReturnBadArgument;
    memcpy(addrP->bytes, fMyMAC, 6);
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::setPromiscuousMode(bool active) {
    (void)active;
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::setMulticastMode(bool active) {
    (void)active;
    return kIOReturnSuccess;
}

IOReturn RTL8723BE::setMulticastList(IOEthernetAddress *addrs, UInt32 count) {
    (void)addrs;
    (void)count;
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

uint32_t RTL8723BE::getBBReg(uint32_t addr, uint32_t mask) {
    return (mmio_read32(addr) & mask) >> bitShift(mask);
}

void RTL8723BE::setBBReg(uint32_t addr, uint32_t mask, uint32_t data) {
    if (mask != 0xFFFFFFFFU) {
        uint32_t orig = mmio_read32(addr);
        data = (orig & ~mask) | ((data << bitShift(mask)) & mask);
    }
    mmio_write32(addr, data);
}

// ============================================================================
// RF register access (Linux rtl8723_phy_rf_serial_read/write, path A)
// ============================================================================

uint32_t RTL8723BE::getRFReg(uint8_t offset, uint32_t mask) {
    if (fRFLock) IOLockLock(fRFLock);
    uint32_t hssi = mmio_read32(RFPGA0_XA_HSSIPARAMETER2);
    uint32_t hssi2 = (hssi & ~BLSSIREADADDRESS) | ((uint32_t)offset << 23) | BLSSIREADEDGE;
    mmio_write32(RFPGA0_XA_HSSIPARAMETER2, hssi & ~BLSSIREADEDGE);
    mmio_write32(RFPGA0_XA_HSSIPARAMETER2, hssi2);
    mmio_write32(RFPGA0_XA_HSSIPARAMETER2, hssi | BLSSIREADEDGE);
    IODelay(120);
    bool pi = (mmio_read32(RFPGA0_XA_HSSIPARAMETER1) & (1U << 8)) != 0;
    uint32_t value = mmio_read32(pi ? TRANSCEIVEA_HSPI_READBACK : RFPGA0_XA_LSSIREADBACK) & BLSSIREADBACKDATA;
    if (fRFLock) IOLockUnlock(fRFLock);
    return (value & mask) >> bitShift(mask);
}

void RTL8723BE::setRFReg(uint8_t offset, uint32_t mask, uint32_t data) {
    if (mask != 0xFFFFFU) {
        uint32_t orig = getRFReg(offset, 0xFFFFF);
        data = (orig & ~mask) | (data << bitShift(mask));
    }
    if (fRFLock) IOLockLock(fRFLock);
    uint32_t dataAndAddr = (((uint32_t)offset << 20) | (data & 0x000FFFFFU)) & 0x0FFFFFFFU;
    mmio_write32(REG_RFPGA0_XA_LSSI, dataAndAddr);
    IODelay(1);
    if (fRFLock) IOLockUnlock(fRFLock);
}

// ============================================================================
// Power sequencing
// ============================================================================

bool RTL8723BE::runPowerSequence(const RTLPwrCmd *seq, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const RTLPwrCmd &c = seq[i];
        switch (c.cmd) {
        case kPwrWrite: {
            uint8_t v = mmio_read8(c.offset);
            mmio_write8(c.offset, (uint8_t)((v & ~c.mask) | (c.value & c.mask)));
            break;
        }
        case kPwrPoll: {
            bool ok = false;
            for (int n = 0; n < 5000; ++n) {
                if ((mmio_read8(c.offset) & c.mask) == (c.value & c.mask)) { ok = true; break; }
                if (mmio_read32(0) == 0xFFFFFFFFU) break; // device fell off the bus
                IODelay(10);
            }
            if (!ok) {
                IOLog("RTL8723BE: pwrseq poll 0x%04x mask 0x%02x timed out (0x%02x)\n",
                      c.offset, c.mask, mmio_read8(c.offset));
                return false;
            }
            break;
        }
        case kPwrDelay:
            IODelay(2);
            break;
        }
    }
    return true;
}

bool RTL8723BE::powerOn() {
    mmio_write8(REG_RSV_CTRL, 0x00);
    // Auto power down to chip-off state disabled (APS_FSMCO[15] = 0).
    mmio_write8(REG_APS_FSMCO + 1, mmio_read8(REG_APS_FSMCO + 1) & ~0x80U);
    if (!runPowerSequence(kCardEnableFlow, sizeof(kCardEnableFlow) / sizeof(kCardEnableFlow[0]))) {
        return false;
    }
    fPowerState = POWER_ACT;
    return true;
}

// Linux _rtl8723be_poweroff_adapter()
void RTL8723BE::powerOff() {
    if (!fMMIOBase || mmio_read32(0) == 0xFFFFFFFFU) return;
    (void)runPowerSequence(kLpsEnterFlow, sizeof(kLpsEnterFlow) / sizeof(kLpsEnterFlow[0]));
    if ((mmio_read8(REG_MCUFWDL) & 0x80) && fFwReady) {
        firmwareSelfReset();
    }
    mmio_write8(REG_SYS_FUNC_EN + 1, mmio_read8(REG_SYS_FUNC_EN + 1) & ~0x04U); // reset MCU
    mmio_write8(REG_MCUFWDL, 0x00);
    (void)runPowerSequence(kCardDisableFlow, sizeof(kCardDisableFlow) / sizeof(kCardDisableFlow[0]));
    mmio_write8(REG_RSV_CTRL + 1, mmio_read8(REG_RSV_CTRL + 1) & ~0x01U);
    mmio_write8(REG_RSV_CTRL + 1, mmio_read8(REG_RSV_CTRL + 1) | 0x01U);
    mmio_write8(REG_RSV_CTRL, 0x0E);
    fPowerState = POWER_CARDDIS;
    fFwReady = false;
}

void RTL8723BE::hwDisable() {
    disableInterrupts();
    fHwReady = false;
    fState = kRTLStateDisconnected;
    if (!fMMIOBase || mmio_read32(0) == 0xFFFFFFFFU) return;
    mmio_write8(REG_MSR, 0x00);            // no link
    refreshLED(false);
    powerOff();
}

// ============================================================================
// MAC init (Linux _rtl8723be_init_mac / _rtl8723be_llt_table_init)
// ============================================================================

bool RTL8723BE::initLLT() {
    const uint32_t txpktbufBndy = 245;
    const uint32_t maxPage = 255;

    mmio_write32(REG_TRXFF_BNDY, 0x27FF0000U | txpktbufBndy);
    mmio_write8(REG_TDECTRL + 1, (uint8_t)txpktbufBndy);
    mmio_write8(REG_TXPKTBUF_BCNQ_BDNY, (uint8_t)txpktbufBndy);
    mmio_write8(REG_TXPKTBUF_MGQ_BDNY, (uint8_t)txpktbufBndy);
    mmio_write8(0x045D, (uint8_t)txpktbufBndy);
    mmio_write8(REG_PBP, 0x31);
    mmio_write8(REG_RX_DRVINFO_SZ, 0x4);

    auto writeLLT = [this](uint32_t address, uint32_t next) -> bool {
        mmio_write32(REG_LLT_INIT, (1U << 30) | ((address & 0xFF) << 8) | (next & 0xFF));
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            uint32_t status = mmio_read32(REG_LLT_INIT);
            if (status == 0xFFFFFFFFU) return false;
            if (((status >> 30) & 0x3) == 0) return true;
            IODelay(2);
        }
        return false;
    };
    for (uint32_t i = 0; i < txpktbufBndy - 1; ++i) {
        if (!writeLLT(i, i + 1)) return false;
    }
    if (!writeLLT(txpktbufBndy - 1, 0xFF)) return false;
    for (uint32_t i = txpktbufBndy; i < maxPage; ++i) {
        if (!writeLLT(i, i + 1)) return false;
    }
    if (!writeLLT(maxPage, txpktbufBndy)) return false;

    mmio_write32(REG_RQPN, 0x80E40808U);
    mmio_write8(REG_RQPN_NPQ, 0x00);
    return true;
}

bool RTL8723BE::initMac() {
    if (!powerOn()) {
        IOLog("RTL8723BE: power-on sequence failed\n");
        return false;
    }
    fInitStage = kStagePowerOn;

    mmio_write8(REG_MULTI_FUNC_CTRL, mmio_read8(REG_MULTI_FUNC_CTRL) | 0x08);
    mmio_write8(REG_APS_FSMCO, mmio_read8(REG_APS_FSMCO) | 0x10);
    mmio_write8(REG_CR, 0xFF);
    IOSleep(2);
    mmio_write8(REG_HWSEQ_CTRL, mmio_read8(REG_HWSEQ_CTRL) | 0x7F);
    IOSleep(2);
    if (mmio_read8(REG_SYS_CFG + 3) & 0x01) {
        mmio_write8(0x007C, mmio_read8(0x007C) | 0x40);
    }
    mmio_write8(REG_SYS_CLKR, mmio_read8(REG_SYS_CLKR) | 0x08);
    mmio_write8(REG_GPIO_MUXCFG + 1, mmio_read8(REG_GPIO_MUXCFG + 1) & ~0x10);
    mmio_write16(REG_CR, 0x02FF);

    if (!initLLT()) {
        IOLog("RTL8723BE: LLT table init failed\n");
        return false;
    }

    mmio_write32(REG_HISR, 0xFFFFFFFFU);
    mmio_write32(REG_HISRE, 0xFFFFFFFFU);
    mmio_write8(REG_FWIMR + 3, mmio_read8(REG_FWIMR + 3) | 0x40);

    uint16_t trx = mmio_read16(REG_TRXDMA_CTRL);
    mmio_write16(REG_TRXDMA_CTRL, (uint16_t)((trx & 0xF) | 0xF5B1));
    mmio_write8(REG_FWHW_TXQ_CTRL + 1, 0x1F);
    mmio_write32(REG_RCR, RTL8723BE_RCR_DEFAULT | RCR_AICV);
    mmio_write16(REG_RXFLTMAP2, 0xFFFF);
    mmio_write32(REG_TCR, RTL8723BE_TCR_DEFAULT);

    mmio_write32(REG_BCNQ_DESA, (uint32_t)fTxRing[Q_BCN].ioaddr);
    mmio_write32(REG_MGQ_DESA, (uint32_t)fTxRing[Q_MGNT].ioaddr);
    mmio_write32(REG_VOQ_DESA, (uint32_t)fTxRing[Q_VO].ioaddr);
    mmio_write32(REG_VIQ_DESA, (uint32_t)fTxRing[Q_VI].ioaddr);
    mmio_write32(REG_BEQ_DESA, (uint32_t)fTxRing[Q_BE].ioaddr);
    mmio_write32(REG_BKQ_DESA, (uint32_t)fTxRing[Q_BK].ioaddr);
    mmio_write32(REG_HQ_DESA, (uint32_t)fTxRing[Q_HIGH].ioaddr);
    mmio_write32(REG_RX_DESA, (uint32_t)fRxRing.ioaddr);

    mmio_write8(REG_PCIE_CTRL_REG + 3, mmio_read8(REG_PCIE_CTRL_REG + 3) | 0x77);
    mmio_write32(REG_INT_MIG, 0);
    mmio_write32(REG_MCUTST_1, 0);
    mmio_write8(REG_SECONDARY_CCA_CTRL, 0x3);

    // DPDT / fixed board antenna wiring (Kordan, 2013-01-14)
    setBBReg(0x64, 1U << 20, 0);
    setBBReg(0x64, 1U << 24, 0);
    setBBReg(0x40, 1U << 4, 0);
    setBBReg(0x40, 1U << 3, 1);
    setBBReg(0x4C, (1U << 24) | (1U << 23), 0x2);
    setBBReg(0x944, 0x3, 0x3);
    setBBReg(0x930, 0xFF, 0x77);
    setBBReg(0x38, 1U << 11, 1);

    mmio_write8(REG_RXDMA_CONTROL, mmio_read8(REG_RXDMA_CONTROL) & ~0x04);
    return true;
}

// ============================================================================
// eFuse (Linux efuse.c read_efuse_byte / read_efuse / efuse_power_switch)
// ============================================================================

uint8_t RTL8723BE::readEfuseByte(uint16_t addr) {
    mmio_write8(REG_EFUSE_CTRL + 1, (uint8_t)(addr & 0xFF));
    uint8_t b2 = mmio_read8(REG_EFUSE_CTRL + 2);
    mmio_write8(REG_EFUSE_CTRL + 2, (uint8_t)(((addr >> 8) & 0x03) | (b2 & 0xFC)));
    uint8_t b3 = mmio_read8(REG_EFUSE_CTRL + 3);
    mmio_write8(REG_EFUSE_CTRL + 3, b3 & 0x7F);
    for (int retry = 0; retry < 1000; ++retry) {
        if (mmio_read8(REG_EFUSE_CTRL + 3) & 0x80) break;
        IODelay(1);
    }
    IODelay(50);
    return (uint8_t)(mmio_read32(REG_EFUSE_CTRL) & 0xFF);
}

bool RTL8723BE::readEfuse(CalibData &out) {
    uint8_t phys[RTL8723BE_EFUSE_REAL_LEN];
    uint8_t map[RTL8723BE_EFUSE_MAP_LEN];

    memset(&out, 0, sizeof(out));

    // efuse_power_switch(hw, false, true)
    mmio_write8(REG_EFUSE_ACCESS, 0x69);
    uint16_t fen = mmio_read16(REG_SYS_FUNC_EN);
    if (!(fen & (1U << 12))) mmio_write16(REG_SYS_FUNC_EN, fen | (1U << 12));      // FEN_ELDR
    uint16_t clk = mmio_read16(REG_SYS_CLKR);
    if ((clk & 0x22) != 0x22) mmio_write16(REG_SYS_CLKR, clk | 0x22);             // LOADER_CLK_EN | ANA8M

    size_t n = 0;
    for (; n < sizeof(phys); ++n) {
        phys[n] = readEfuseByte((uint16_t)n);
    }
    uint8_t pkg = readEfuseByte(0x1FB);
    mmio_write8(REG_EFUSE_ACCESS, 0x00);

    out.efuse_used = (uint16_t)rtlEfuseDecode(phys, sizeof(phys), map, sizeof(map));

    switch (pkg & 0x7) {
    case 0x4: out.package_type = 4; break; // TFBGA79
    case 0x5: out.package_type = 2; break; // TFBGA90
    case 0x6: out.package_type = 1; break; // QFN68
    case 0x7: out.package_type = 3; break; // TFBGA80
    default:  out.package_type = 0; break;
    }

    uint16_t eepromId = (uint16_t)(map[0] | (map[1] << 8));
    out.autoload_ok = (eepromId == RTL8723BE_EEPROM_ID);

    memcpy(out.mac_addr, &map[EEPROM_MAC_ADDR], 6);
    out.crystal_cap = map[EEPROM_XTAL_8723BE];
    out.thermal_meter = map[EEPROM_THERMAL_METER];
    out.channel_plan = map[EEPROM_CHANNELPLAN];
    out.customer_id = map[EEPROM_CUSTOMER_ID];
    out.bt_setting = map[EEPROM_RF_BT_SETTING_8723B];
    out.vid = (uint16_t)(map[EEPROM_VID] | (map[EEPROM_VID + 1] << 8));
    out.did = (uint16_t)(map[EEPROM_DID] | (map[EEPROM_DID + 1] << 8));
    out.svid = (uint16_t)(map[EEPROM_SVID] | (map[EEPROM_SVID + 1] << 8));
    out.smid = (uint16_t)(map[EEPROM_SMID] | (map[EEPROM_SMID + 1] << 8));
    // 2.4G CCK / HT40 base power indexes for path A group 0..5 (Linux _rtl8723be_read_power_value_fromprom)
    memcpy(out.tx_pwr_cck, &map[EEPROM_TX_PWR_INX], 6);
    memcpy(out.tx_pwr_ht40, &map[EEPROM_TX_PWR_INX + 6], 5);
    out.tx_pwr_ht20_diff = map[EEPROM_TX_PWR_INX + 11];

    bool macValid = out.autoload_ok &&
                    !((out.mac_addr[0] & 0x01) != 0) &&
                    !(out.mac_addr[0] == 0xFF && out.mac_addr[1] == 0xFF && out.mac_addr[2] == 0xFF) &&
                    !(out.mac_addr[0] == 0 && out.mac_addr[1] == 0 && out.mac_addr[2] == 0);
    if (!macValid) {
        // Locally administered fallback that is still unique per boot.
        uint8_t rnd[3];
        read_random(rnd, sizeof(rnd));
        const uint8_t fallback[6] = {0x02, 0xE0, 0x4C, rnd[0], rnd[1], rnd[2]};
        memcpy(out.mac_addr, fallback, 6);
    }
    if (out.crystal_cap == 0xFF) out.crystal_cap = 0x20;
    if (out.thermal_meter == 0xFF) out.thermal_meter = 0x18;
    for (int i = 0; i < 6; ++i) if (out.tx_pwr_cck[i] == 0xFF || out.tx_pwr_cck[i] == 0) out.tx_pwr_cck[i] = 0x2D;
    for (int i = 0; i < 5; ++i) if (out.tx_pwr_ht40[i] == 0xFF || out.tx_pwr_ht40[i] == 0) out.tx_pwr_ht40[i] = 0x2D;
    return out.autoload_ok;
}

// ============================================================================
// 8051 firmware download (Linux rtl8723_download_fw)
// ============================================================================

void RTL8723BE::firmwareSelfReset() {
    mmio_write8(REG_RSV_CTRL + 1, mmio_read8(REG_RSV_CTRL + 1) & ~0x01U);
    mmio_write8(REG_SYS_FUNC_EN + 1, mmio_read8(REG_SYS_FUNC_EN + 1) & ~0x04U);
    IODelay(50);
    mmio_write8(REG_RSV_CTRL + 1, mmio_read8(REG_RSV_CTRL + 1) | 0x01U);
    mmio_write8(REG_SYS_FUNC_EN + 1, mmio_read8(REG_SYS_FUNC_EN + 1) | 0x04U);
}

bool RTL8723BE::downloadFirmware(const uint8_t *fwBuf, size_t fwLen) {
    if (!fwBuf || fwLen < sizeof(RTLFirmwareHeader)) return false;

    const RTLFirmwareHeader *hdr = (const RTLFirmwareHeader*)fwBuf;
    const uint8_t *code = fwBuf;
    size_t codeLen = fwLen;
    if ((OSSwapLittleToHostInt16(hdr->signature) & 0xFFF0) == 0x5300) {
        fFwVersion = OSSwapLittleToHostInt16(hdr->version);
        fFwSubVersion = hdr->subversion;
        code += sizeof(RTLFirmwareHeader);
        codeLen -= sizeof(RTLFirmwareHeader);
    }
    if (codeLen > 8 * 4096) return false;

    if (mmio_read8(REG_MCUFWDL) & 0x80) {
        firmwareSelfReset();
        mmio_write8(REG_MCUFWDL, 0x00);
    }

    // rtl8723_enable_fw_download(hw, true)
    mmio_write8(REG_SYS_FUNC_EN + 1, mmio_read8(REG_SYS_FUNC_EN + 1) | 0x04);
    mmio_write8(REG_MCUFWDL, mmio_read8(REG_MCUFWDL) | 0x01);
    mmio_write8(REG_MCUFWDL + 2, mmio_read8(REG_MCUFWDL + 2) & 0xF7);

    // rtl8723_write_fw: 4 KB pages, byte writes, image padded to a 4-byte multiple.
    size_t padded = (codeLen + 3) & ~(size_t)3;
    for (size_t page = 0; page * 4096 < padded; ++page) {
        mmio_write8(REG_MCUFWDL + 2, (uint8_t)((mmio_read8(REG_MCUFWDL + 2) & 0xF8) | (page & 0x07)));
        size_t base = page * 4096;
        size_t chunk = (padded - base) < 4096 ? (padded - base) : 4096;
        for (size_t i = 0; i < chunk; ++i) {
            uint8_t b = (base + i < codeLen) ? code[base + i] : 0;
            mmio_write8(REG_FW_START_ADDR + (uint32_t)i, b);
        }
    }

    // rtl8723_enable_fw_download(hw, false)
    mmio_write8(REG_MCUFWDL, mmio_read8(REG_MCUFWDL) & 0xFE);
    mmio_write8(REG_MCUFWDL + 1, 0x00);

    // rtl8723_fw_free_to_go
    uint32_t v = 0;
    bool chk = false;
    for (int i = 0; i < 6000; ++i) {
        v = mmio_read32(REG_MCUFWDL);
        if (v & MCUFWDL_CHKSUM_RPT) { chk = true; break; }
    }
    if (!chk) {
        IOLog("RTL8723BE: firmware checksum report failed (MCUFWDL=0x%08x)\n", v);
        return false;
    }
    v = (mmio_read32(REG_MCUFWDL) | MCUFWDL_RDY) & ~MCUFWDL_WINTINI_RDY;
    mmio_write32(REG_MCUFWDL, v);
    firmwareSelfReset();

    for (int i = 0; i < 600; ++i) {  // 600 x 5 ms = 3 s
        v = mmio_read32(REG_MCUFWDL);
        if (v & MCUFWDL_WINTINI_RDY) {
            fFwReady = true;
            return true;
        }
        IOSleep(5);
    }
    IOLog("RTL8723BE: firmware did not report WINTINI_RDY (MCUFWDL=0x%08x)\n", v);
    return false;
}

// ============================================================================
// PHY / RF table programming (Linux rtl8723be_phy_config_with_headerfile)
// ============================================================================

enum { kTableMac = 0, kTableBB, kTableAGC, kTableRF };

// Linux _rtl8723be_check_positive(): evaluate an IF/ELSE-IF condition header.
bool RTL8723BE::checkCondition(uint32_t cond1, uint32_t cond2) {
    const uint32_t intf = 1U << 0; // PCIe
    const uint8_t boardType = 0;   // no external LNA/PA on RTL8723BE modules
    uint32_t driver1 = ((uint32_t)fCutVersion << 24) | (0x04U << 16) |
                       ((uint32_t)fCalib.package_type << 12) | (intf << 8) | boardType;
    uint32_t driver2 = 0;

    if ((cond1 & 0x0000F000U) != 0 && (cond1 & 0x0000F000U) != (driver1 & 0x0000F000U)) return false;
    if ((cond1 & 0x0F000000U) != 0 && (cond1 & 0x0F000000U) != (driver1 & 0x0F000000U)) return false;

    cond1 &= 0x00FF0FFFU;
    driver1 &= 0x00FF0FFFU;
    if ((cond1 & driver1) != cond1) return false;
    if ((cond1 & 0x0F) == 0) return true;

    uint32_t mask = 0;
    if (cond1 & (1U << 0)) mask |= 0x000000FFU;
    if (cond1 & (1U << 1)) mask |= 0x0000FF00U;
    if (cond1 & (1U << 2)) mask |= 0x00FF0000U;
    if (cond1 & (1U << 3)) mask |= 0xFF000000U;
    return (cond2 & mask) == (driver2 & mask);
}

bool RTL8723BE::configTable(const uint32_t *table, size_t len, int kind) {
    bool matched = true, skipped = false;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t v1 = table[i];
        uint32_t v2 = table[i + 1];

        if (v1 & 0xC0000000U) {
            if (v1 & 0x80000000U) {
                uint8_t cond = (uint8_t)((v1 >> 28) & 0x3);
                if (cond == 3) {            // ENDIF
                    matched = true;
                    skipped = false;
                } else if (cond == 2) {     // ELSE
                    matched = !skipped;
                } else if (skipped) {       // IF / ELSE IF after a taken branch
                    matched = false;
                } else if (checkCondition(v1, v2)) {
                    matched = true;
                    skipped = true;
                } else {
                    matched = false;
                    skipped = false;
                }
            }
            continue;
        }
        if (!matched) continue;

        switch (kind) {
        case kTableMac:
            if (v1 < 0x4000U) mmio_write8(v1, (uint8_t)v2);
            break;
        case kTableBB:
            if (v1 == 0xFE) rtlDelayMs(50);
            else if (v1 == 0xFD) rtlDelayMs(5);
            else if (v1 == 0xFC) rtlDelayMs(1);
            else if (v1 == 0xFB) IODelay(50);
            else if (v1 == 0xFA) IODelay(5);
            else if (v1 == 0xF9) IODelay(1);
            else if (v1 < 0x4000U) { mmio_write32(v1, v2); IODelay(1); }
            break;
        case kTableAGC:
            if (v1 < 0x4000U) { mmio_write32(v1, v2); IODelay(1); }
            break;
        case kTableRF:
            if (v1 == 0xFE || v1 == 0xFFE) rtlDelayMs(50);
            else if (v1 <= 0xFF) { setRFReg((uint8_t)v1, 0xFFFFF, v2); IODelay(1); }
            break;
        }
    }
    return true;
}

bool RTL8723BE::phyMacConfig() {
    configTable(RTL8723BEMAC_1T_ARRAY, RTL8723BEMAC_1T_ARRAYLEN, kTableMac);
    mmio_write8(REG_MAX_AGGR_NUM, 0x0B);
    return true;
}

// Linux rtl8723be_phy_bb_config()
bool RTL8723BE::phyBBConfig() {
    uint16_t fen = mmio_read16(REG_SYS_FUNC_EN);
    mmio_write16(REG_SYS_FUNC_EN, fen | (1U << 13) | (1U << 0) | (1U << 1));
    mmio_write8(REG_RF_CTRL, 0x07);                    // RF_EN | RF_RSTB | RF_SDMRSTB
    mmio_write8(REG_SYS_FUNC_EN, 0xE3);                // PPLL | PCIEA | DIO_PCIE | BB_GLB_RSTN | BBRSTB
    mmio_write32(0x4C, mmio_read32(0x4C) | (1U << 23));
    mmio_write8(REG_AFE_XTAL_CTRL + 1, 0x80);

    // Switch the shared antenna to BT while the BB is programmed (as Linux does).
    mmio_write32(0x948, fActiveAntenna == 2 ? 0x280 : 0x0);

    configTable(RTL8723BEPHY_REG_1TARRAY, RTL8723BEPHY_REG_1TARRAYLEN, kTableBB);
    // RTL8723BEPHY_REG_ARRAY_PG is a TX-power-by-rate table, not register writes.
    configTable(RTL8723BEAGCTAB_1TARRAY, RTL8723BEAGCTAB_1TARRAYLEN, kTableAGC);

    uint32_t xtal = fCalib.crystal_cap & 0x3F;
    setBBReg(REG_MAC_PHY_CTRL, 0xFFF000, xtal | (xtal << 6));
    return true;
}

// Linux _rtl8723be_phy_rf6052_config_parafile() for path A
bool RTL8723BE::phyRFConfig() {
    uint32_t savedEnv = getBBReg(RFPGA0_XAB_RFINTERFACESW, BRFSI_RFENV);
    setBBReg(RFPGA0_XA_RFINTERFACEOE, BRFSI_RFENV << 16, 0x1);
    IODelay(1);
    setBBReg(RFPGA0_XA_RFINTERFACEOE, BRFSI_RFENV, 0x1);
    IODelay(1);
    setBBReg(RFPGA0_XA_HSSIPARAMETER2, B3WIREADDRESSLENGTH, 0x0);
    IODelay(1);
    setBBReg(RFPGA0_XA_HSSIPARAMETER2, B3WIREDATALENGTH, 0x0);
    IODelay(1);

    configTable(RTL8723BE_RADIOA_1TARRAY, RTL8723BE_RADIOA_1TARRAYLEN, kTableRF);
    if (fOemHP) {
        setRFReg(0x52, 0xFFFFF, 0x7E4BD);
    }

    setBBReg(RFPGA0_XAB_RFINTERFACESW, BRFSI_RFENV, savedEnv);
    return true;
}

// Linux _rtl8723be_hw_configure()
void RTL8723BE::hwConfigure() {
    mmio_write32(REG_RRSR, 0x00000FFFU);   // RATE_ALL_CCK | RATE_ALL_OFDM_AG
    mmio_write32(REG_ARFR0 + 4, 0xFFFFF000U);
    mmio_write32(REG_ARFR1 + 4, 0x003FF000U);
    mmio_write16(REG_FWHW_TXQ_CTRL, 0x1F00);
    mmio_write8(REG_AMPDU_MAX_TIME, 0x70);
    mmio_write16(REG_RL, 0x0707);
    mmio_write32(REG_DARFRC, 0x01000000U);
    mmio_write32(REG_DARFRC + 4, 0x07060504U);
    mmio_write32(REG_RARFRC, 0x01000000U);
    mmio_write32(REG_RARFRC + 4, 0x07060504U);
    mmio_write8(REG_BCN_CTRL, 0x1D);
    mmio_write8(REG_TBTT_PROHIBIT + 1, 0xFF);
    mmio_write16(REG_NAV_PROT_LEN, 0x0040);
    mmio_write32(REG_FAST_EDCA_CTRL, 0x03086666U);
    mmio_write8(REG_HT_SINGLE_AMPDU, 0x80);
    mmio_write8(REG_RX_PKT_LIMIT, 0x20);
    mmio_write8(REG_MAX_AGGR_NUM, 0x1F);
}

// WLAN-only BT-coexistence antenna setup (Linux halbtc8723b1ant/2ant init_hw_config with
// firmware < v24 and an internal switch). Grants the shared RF front end to Wi-Fi.
void RTL8723BE::btcoexInit() {
    mmio_write8(0x0790, (uint8_t)((mmio_read8(0x0790) & 0xC0) | 0x05));

    mmio_write8(0x0039, mmio_read8(0x0039) | 0x08);
    mmio_write8(0x0974, 0xFF);
    mmio_write8(0x0944, mmio_read8(0x0944) | 0x03);
    mmio_write8(0x0930, 0x77);

    // Internal switch: 0x4C[23] = 1, 0x4C[24] = 0; fixed external switch S1->Main, S0->Aux.
    uint32_t r4c = mmio_read32(0x4C);
    r4c |= (1U << 23);
    r4c &= ~(1U << 24);
    mmio_write32(0x4C, r4c);
    mmio_write8(0x0064, mmio_read8(0x0064) & ~0x01);

    // PTA: grant to PTA, WLAN_ACT controlled by PTA, S0/S1 selected by Wi-Fi.
    mmio_write8(0x0765, 0x00);
    mmio_write8(0x076E, 0x0C);
    mmio_write8(0x0067, mmio_read8(0x0067) | 0x20);
    mmio_write8(0x0040, mmio_read8(0x0040) | 0x20);

    // Coex table type 0: Wi-Fi owns every slot while BT is idle.
    mmio_write32(0x06C0, 0x55555555U);
    mmio_write32(0x06C4, 0x55555555U);
    mmio_write32(0x06C8, 0x00FFFFFFU);
    mmio_write8(0x06CC, 0x03);

    // Wi-Fi on S1 (main) -> 0x948 = 0x000, on S0 (aux) -> 0x948 = 0x280.
    mmio_write32(0x0948, fActiveAntenna == 2 ? 0x280 : 0x0);
    // rtl8723be_phy_set_rfpath_switch(hw, 1)
    mmio_write32(REG_BB_PAD_CTRL, 0x1);
    applyCoexProfile();
}

// Linux _rtl8723be_fill_h2c_command(): three main and four extension bytes.
bool RTL8723BE::sendH2C(uint8_t id, const uint8_t *params, uint8_t len) {
    if (!fFwReady || len > 7 || (len && !params)) return false;
    uint8_t box = fLastHmeBox;
    bool free = false;
    for (int i = 0; i < 100; ++i) {
        if (((mmio_read8(REG_HMETFR) >> box) & 0x01) == 0) { free = true; break; }
        IODelay(10);
    }
    if (!free) {
        IOLog("RTL8723BE: H2C 0x%02x: firmware did not drain mailbox %u\n", id, box);
        return false;
    }
    uint8_t content[4] = {id, 0, 0, 0};
    // Linux fw.c: extension must be committed before the main mailbox triggers the MCU.
    if (len > 3) {
        uint8_t extension[4] = {};
        memcpy(extension, params + 3, len - 3);
        for (unsigned i = 0; i < 4; ++i) mmio_write8(0x01F0 + box * 4U + i, extension[i]);
    }
    for (uint8_t i = 0; i < len && i < 3; ++i) content[1 + i] = params[i];
    for (uint8_t i = 0; i < 4; ++i) mmio_write8(REG_HMEBOX_0 + box * 4U + i, content[i]);
    fLastHmeBox = (uint8_t)((box + 1) & 0x3);
    return true;
}

// Settings that differ between Linux's 1-antenna (halbtc8723b1ant, what ant_sel=1 selects)
// and 2-antenna (halbtc8723b2ant) coexistence code for a Wi-Fi-only link.
void RTL8723BE::applyCoexProfile() {
    setRFReg(0x01, 0xFFFFF, fAntNum == 1 ? 0x780 : 0x0);   // Wi-Fi TRx mask
    mmio_write8(0x0778, fAntNum == 1 ? 0x01 : 0x03);        // coex counter statistics

    // Tell the firmware where the Wi-Fi antenna is (H2C 0x65: inverse, internal switch) and,
    // for firmware >= v24, release GNT_BT through the firmware (H2C 0x6E) instead of 0x765.
    if (fFwReady) {
        const uint8_t antInfo[2] = {(uint8_t)(fActiveAntenna == 2 ? 1 : 0), 0};
        sendH2C(0x65, antInfo, 2);
        if (fFwVersion >= 0x18) {
            const uint8_t gntBtLow = 0;
            sendH2C(0x6E, &gntBtLow, 1);
        }
    }
    if (fAntNum == 1) {
        const bool shared = (fOptions & kRTLOptionBluetoothCoexist) != 0;
        // Linux halbtc8723b1ant_ps_tdma(false, 8) and internal BTC_ANT_PATH_PTA.
        // This is its PTA baseline, not the dynamic SCO/HID/A2DP policy engine.
        const uint8_t tdma[5] = {uint8_t(shared ? 8 : 0),0,0,0,0};
        sendH2C(0x60, tdma, sizeof(tdma));
        mmio_write32(0x0948, shared ? (fActiveAntenna == 2 ? 0x80 : 0x200) : (fActiveAntenna == 2 ? 0x280 : 0));
    }
    IOLog("RTL8723BE: BT-coex profile: %u-antenna\n", fAntNum);
}

// Linux _rtl8723be_phy_lc_calibrate(hw, false)
void RTL8723BE::lcCalibrate() {
    uint8_t tmp = mmio_read8(0x0D03);
    uint32_t rfMode = 0;
    if (tmp & 0x70) {
        mmio_write8(0x0D03, tmp & 0x8F);
        rfMode = getRFReg(0x00, 0xFFF);
        setRFReg(0x00, 0xFFF, (rfMode & 0x8FFFF) | 0x10000);
    } else {
        mmio_write8(REG_TXPAUSE, 0xFF);
    }
    setRFReg(0xB0, 0xFFFFF, 0xDFBE0);
    setRFReg(RF_CHNLBW, 0xFFF, 0x8C0A);
    rtlDelayMs(50);
    setRFReg(0xB0, 0xFFFFF, 0xDFFE0);
    if (tmp & 0x70) {
        mmio_write8(0x0D03, tmp);
        setRFReg(0x00, 0xFFF, rfMode);
    } else {
        mmio_write8(REG_TXPAUSE, 0x00);
    }
}

// ============================================================================
// IQ calibration (Linux rtl8723be_phy_iq_calibrate, both internal RF paths)
// ============================================================================

enum {
    RFPGA0_IQK = 0xE28, RTX_IQK_TONE_A = 0xE30, RRX_IQK_TONE_A = 0xE34, RTX_IQK_PI_A = 0xE38,
    RRX_IQK_PI_A = 0xE3C, RTX_IQK = 0xE40, RRX_IQK = 0xE44, RIQK_AGC_PTS = 0xE48, RIQK_AGC_RSP = 0xE4C,
    RTX_IQK_TONE_B = 0xE50, RRX_IQK_TONE_B = 0xE54, RTX_IQK_PI_B = 0xE58, RRX_IQK_PI_B = 0xE5C,
    ROFDM0_XARXIQIMBALANCE = 0xC14, ROFDM0_XBRXIQIMBALANCE = 0xC1C, ROFDM0_ECCATHRESHOLD = 0xC4C,
    ROFDM0_XATXIQIMBALANCE = 0xC80, ROFDM0_XBTXIQIMBALANCE = 0xC88, ROFDM0_XCTXAFE = 0xC94,
    ROFDM0_XDTXAFE = 0xC9C, RF_WE_LUT = 0xEF, RF_RCK_OS = 0x30, RF_TXPA_G1 = 0x31, RF_TXPA_G2 = 0x32
};

static bool iqkTxOk(uint32_t eac, uint32_t e94, uint32_t e9c) {
    if ((eac & (1U << 28)) || ((e94 & 0x03FF0000) >> 16) == 0x142 || ((e9c & 0x03FF0000) >> 16) == 0x42)
        return false;
    uint32_t tmp = (e9c & 0x03FF0000) >> 16;
    if (tmp & 0x200) tmp = 0x400 - tmp;
    uint32_t x = (e94 & 0x03FF0000) >> 16;
    return x < 0x110 && x > 0xF0 && tmp < 0xF;
}

static bool iqkRxOk(uint32_t eac, uint32_t ea4) {
    uint32_t tmp = (eac & 0x03FF0000) >> 16;
    if (tmp & 0x200) tmp = 0x400 - tmp;
    if (!(eac & (1U << 27)) && ((ea4 & 0x03FF0000) >> 16) != 0x132 && ((eac & 0x03FF0000) >> 16) != 0x36)
        return true;
    uint32_t x = (ea4 & 0x03FF0000) >> 16;
    return !(eac & (1U << 27)) && x < 0x110 && x > 0xF0 && tmp < 0xF;
}

void RTL8723BE::iqkOneShot(uint32_t agcRsp) {
    setBBReg(RIQK_AGC_RSP, 0xFFFFFFFF, agcRsp);
    setBBReg(RFPGA0_IQK, 0xFFFFFFFF, 0x80800000);
    setBBReg(RIQK_AGC_PTS, 0xFFFFFFFF, 0xF9000000);
    setBBReg(RIQK_AGC_PTS, 0xFFFFFFFF, 0xF8000000);
    IOSleep(10);
    setBBReg(RFPGA0_IQK, 0xFFFFFFFF, 0x00000000);
}

void RTL8723BE::iqkTones(uint32_t txToneA, uint32_t rxToneA, uint32_t txPiA, uint32_t rxPiA) {
    setBBReg(RTX_IQK_TONE_A, 0xFFFFFFFF, txToneA);
    setBBReg(RRX_IQK_TONE_A, 0xFFFFFFFF, rxToneA);
    setBBReg(RTX_IQK_TONE_B, 0xFFFFFFFF, 0x38008C1C);
    setBBReg(RRX_IQK_TONE_B, 0xFFFFFFFF, 0x38008C1C);
    setBBReg(RTX_IQK_PI_A, 0xFFFFFFFF, txPiA);
    setBBReg(RRX_IQK_PI_A, 0xFFFFFFFF, rxPiA);
    setBBReg(RTX_IQK_PI_B, 0xFFFFFFFF, 0x82110000);
    setBBReg(RRX_IQK_PI_B, 0xFFFFFFFF, 0x28110000);
}

// Path A = S1 (0x948 = 0x000), path B = S0 (0x948 = 0x280). Returns bit0 TX ok, bit1 RX ok.
uint8_t RTL8723BE::iqkTx(bool pathB) {
    setBBReg(RFPGA0_IQK, 0xFFFFFFFF, 0);
    mmio_write32(0x948, pathB ? 0x280 : 0x0);
    if (pathB) {
        setRFReg(0xED, 0xFFFFF, 0x00020);
        setRFReg(0x43, 0xFFFFF, 0x40FC1);
    } else {
        setRFReg(RF_WE_LUT, 0xFFFFF, 0x800A0);
        setRFReg(RF_RCK_OS, 0xFFFFF, 0x20000);
        setRFReg(RF_TXPA_G1, 0xFFFFF, 0x0003F);
        setRFReg(RF_TXPA_G2, 0xFFFFF, 0xC7F87);
    }
    setBBReg(RTX_IQK, 0xFFFFFFFF, 0x01007C00);
    setBBReg(RRX_IQK, 0xFFFFFFFF, 0x01004800);
    iqkTones(0x18008C1C, 0x38008C1C, 0x821403EA, pathB ? 0x28110000 : 0x28160000);
    iqkOneShot(0x00462911);
    uint32_t eac = mmio_read32(0xEAC), e94 = mmio_read32(0xE94), e9c = mmio_read32(0xE9C);
    return iqkTxOk(eac, e94, e9c) ? 0x01 : 0x00;
}

uint8_t RTL8723BE::iqkRx(bool pathB) {
    setBBReg(RFPGA0_IQK, 0xFFFFFFFF, 0);
    mmio_write32(0x948, pathB ? 0x280 : 0x0);

    // 1. TX IQK to obtain the TXIMR setting used as the RX IQK stimulus
    if (pathB) {
        setRFReg(RF_WE_LUT, 0xFFFFF, 0x800A0);
        setRFReg(RF_RCK_OS, 0xFFFFF, 0x30000);
        setRFReg(RF_TXPA_G1, 0xFFFFF, 0x0001F);
        setRFReg(RF_TXPA_G2, 0xFFFFF, 0xF7FF7);
        setRFReg(0xED, 0xFFFFF, 0x00020);
        setRFReg(0x43, 0xFFFFF, 0x60FED);
    } else {
        setRFReg(RF_WE_LUT, 0x80000, 0x1);
        setRFReg(RF_RCK_OS, 0xFFFFF, 0x30000);
        setRFReg(RF_TXPA_G1, 0xFFFFF, 0x0001F);
        setRFReg(RF_TXPA_G2, 0xFFFFF, 0xF7FB7);
        setBBReg(RFPGA0_IQK, 0xFFFFFFFF, 0x80800000);
    }
    setBBReg(RTX_IQK, 0xFFFFFFFF, 0x01007C00);
    setBBReg(RRX_IQK, 0xFFFFFFFF, 0x01004800);
    iqkTones(0x18008C1C, 0x38008C1C, 0x82160FF0, 0x28110000);
    iqkOneShot(0x0046A911);
    uint32_t eac = mmio_read32(0xEAC), e94 = mmio_read32(0xE94), e9c = mmio_read32(0xE9C);
    if (!iqkTxOk(eac, e94, e9c)) return 0x00;
    setBBReg(RTX_IQK, 0xFFFFFFFF, 0x80007C00 | (e94 & 0x3FF0000) | ((e9c & 0x3FF0000) >> 16));

    // 2. RX IQK
    setBBReg(RFPGA0_IQK, 0xFFFFFFFF, 0);
    setRFReg(RF_WE_LUT, 0x80000, 0x1);
    setRFReg(RF_RCK_OS, 0xFFFFF, 0x30000);
    setRFReg(RF_TXPA_G1, 0xFFFFF, 0x0001F);
    setRFReg(RF_TXPA_G2, 0xFFFFF, 0xF7D77);
    if (pathB) {
        setRFReg(RF_WE_LUT, 0x80000, 0x0);
        setRFReg(0xED, 0xFFFFF, 0x00020);
        setRFReg(0x43, 0xFFFFF, 0x60FBD);
    } else {
        setRFReg(0xDF, 0xFFFFF, 0xF80);
        setRFReg(0x55, 0xFFFFF, 0x4021F);
    }
    setBBReg(RRX_IQK, 0xFFFFFFFF, 0x01004800);
    iqkTones(0x38008C1C, 0x18008C1C, 0x82110000, 0x2816001F);
    iqkOneShot(0x0046A8D1);
    eac = mmio_read32(0xEAC);
    uint32_t ea4 = mmio_read32(0xEA4);
    if (!pathB) {
        setBBReg(RFPGA0_IQK, 0xFFFFFFFF, 0);
        setRFReg(0xDF, 0xFFFFF, 0x780);
    }
    return iqkRxOk(eac, ea4) ? 0x03 : 0x01;
}

void RTL8723BE::iqkRun(long result[][8], uint8_t t) {
    static const uint32_t addaReg[16] = {0x85C, 0xE6C, 0xE70, 0xE74, 0xE78, 0xE7C, 0xE80, 0xE84,
                                         0xE88, 0xE8C, 0xED0, 0xED4, 0xED8, 0xEDC, 0xEE0, 0xEEC};
    static const uint32_t macReg[4] = {0x522, 0x550, 0x551, 0x040};
    static const uint32_t bbReg[9] = {0xC04, 0xC08, 0x874, 0xB68, 0xB6C, 0x870, 0x860, 0x864, 0xA04};

    uint8_t c50 = mmio_read8(0xC50);
    uint8_t c58 = mmio_read8(0xC58);
    if (t == 0) {
        for (int i = 0; i < 16; ++i) fIqkAdda[i] = mmio_read32(addaReg[i]);
        for (int i = 0; i < 3; ++i) fIqkMac[i] = mmio_read8(macReg[i]);
        fIqkMac[3] = mmio_read32(macReg[3]);
        for (int i = 0; i < 9; ++i) fIqkBB[i] = mmio_read32(bbReg[i]);
    }
    for (int i = 0; i < 16; ++i) mmio_write32(addaReg[i], 0x01C00014);   // path ADDA on
    uint32_t pathSel = mmio_read32(0x948);

    // MAC setting for calibration: pause TX, clear BCN_CTRL bit 3, 0x40 bit 5
    mmio_write8(macReg[0], 0x3F);
    for (int i = 1; i < 3; ++i) mmio_write8(macReg[i], (uint8_t)(fIqkMac[i] & ~0x08));
    mmio_write8(macReg[3], (uint8_t)(fIqkMac[3] & ~0x20));

    setBBReg(0xA04, 0x0F000000, 0xF);
    setBBReg(0xC04, 0xFFFFFFFF, 0x03A05600);
    setBBReg(0xC08, 0xFFFFFFFF, 0x000800E4);
    setBBReg(0x874, 0xFFFFFFFF, 0x22204000);

    for (int i = 0; i < 2; ++i) {
        if (iqkTx(false) == 0x01) {
            result[t][0] = (mmio_read32(0xE94) & 0x3FF0000) >> 16;
            result[t][1] = (mmio_read32(0xE9C) & 0x3FF0000) >> 16;
            break;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (iqkRx(false) == 0x03) {
            result[t][2] = (mmio_read32(0xEA4) & 0x3FF0000) >> 16;
            result[t][3] = (mmio_read32(0xEAC) & 0x3FF0000) >> 16;
            break;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (iqkTx(true) == 0x01) {
            result[t][4] = (mmio_read32(0xE94) & 0x3FF0000) >> 16;
            result[t][5] = (mmio_read32(0xE9C) & 0x3FF0000) >> 16;
            break;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (iqkRx(true) == 0x03) {
            result[t][6] = (mmio_read32(0xEA4) & 0x3FF0000) >> 16;
            result[t][7] = (mmio_read32(0xEAC) & 0x3FF0000) >> 16;
            break;
        }
    }

    setBBReg(RFPGA0_IQK, 0xFFFFFFFF, 0);
    if (t != 0) {
        for (int i = 0; i < 16; ++i) mmio_write32(addaReg[i], fIqkAdda[i]);
        for (int i = 0; i < 3; ++i) mmio_write8(macReg[i], (uint8_t)fIqkMac[i]);
        mmio_write32(macReg[3], fIqkMac[3]);
        for (int i = 0; i < 9; ++i) mmio_write32(bbReg[i], fIqkBB[i]);
        mmio_write32(0x948, pathSel);
        mmio_write8(0xC50, 0x50);
        mmio_write8(0xC50, c50);
        mmio_write8(0xC58, 0x50);
        mmio_write8(0xC58, c58);
        setBBReg(0xE30, 0xFFFFFFFF, 0x01008C00);
        setBBReg(0xE34, 0xFFFFFFFF, 0x01008C00);
    }
}

static bool iqkSimilar(long result[][8], uint8_t c1, uint8_t c2) {
    uint32_t bitmap = 0;
    uint8_t finalCand[2] = {0xFF, 0xFF};
    bool bresult = true;
    for (uint32_t i = 0; i < 8; ++i) {
        long t1 = result[c1][i], t2 = result[c2][i];
        if (i & 1) {
            if (t1 & 0x200) t1 |= (long)(int32_t)0xFFFFFC00;
            if (t2 & 0x200) t2 |= (long)(int32_t)0xFFFFFC00;
        }
        uint32_t diff = (uint32_t)(t1 > t2 ? t1 - t2 : t2 - t1);
        if (diff > 5) {
            if ((i == 2 || i == 6) && !bitmap) {
                if (result[c1][i] + result[c1][i + 1] == 0) finalCand[i / 4] = c2;
                else if (result[c2][i] + result[c2][i + 1] == 0) finalCand[i / 4] = c1;
                else bitmap |= (1U << i);
            } else {
                bitmap |= (1U << i);
            }
        }
    }
    if (bitmap == 0) {
        for (uint32_t i = 0; i < 2; ++i) {
            if (finalCand[i] != 0xFF) {
                for (uint32_t j = i * 4; j < (i + 1) * 4 - 2; ++j) result[3][j] = result[finalCand[i]][j];
                bresult = false;
            }
        }
        return bresult;
    }
    if (!(bitmap & 0x03)) for (int i = 0; i < 2; ++i) result[3][i] = result[c1][i];
    if (!(bitmap & 0x0C)) for (int i = 2; i < 4; ++i) result[3][i] = result[c1][i];
    if (!(bitmap & 0x30)) for (int i = 4; i < 6; ++i) result[3][i] = result[c1][i];
    if (!(bitmap & 0xC0)) for (int i = 6; i < 8; ++i) result[3][i] = result[c1][i];
    return false;
}

// Fill the TX/RX IQ imbalance registers of one BB path from result[cand][base..base+3].
void RTL8723BE::iqkFill(long result[][8], uint8_t cand, bool pathB, bool txOnly) {
    const uint32_t txImb = pathB ? ROFDM0_XBTXIQIMBALANCE : ROFDM0_XATXIQIMBALANCE;
    const uint32_t rxImb = pathB ? ROFDM0_XBRXIQIMBALANCE : ROFDM0_XARXIQIMBALANCE;
    const uint32_t txAfe = pathB ? ROFDM0_XDTXAFE : ROFDM0_XCTXAFE;
    const int base = pathB ? 4 : 0;
    uint32_t old = (mmio_read32(txImb) >> 22) & 0x3FF;
    uint32_t x = (uint32_t)result[cand][base];
    if (x & 0x200) x |= 0xFFFFFC00;
    uint32_t txA = (x * old) >> 8;
    setBBReg(txImb, 0x3FF, txA);
    setBBReg(ROFDM0_ECCATHRESHOLD, pathB ? (1U << 27) : (1U << 31), ((x * old) >> 7) & 1);
    int64_t y = result[cand][base + 1];
    if (y & 0x200) y |= (int64_t)(int32_t)0xFFFFFC00;
    int64_t txC = (y * (int64_t)old) >> 8;
    setBBReg(txAfe, 0xF0000000, (uint32_t)((txC & 0x3C0) >> 6));
    setBBReg(txImb, 0x003F0000, (uint32_t)(txC & 0x3F));
    setBBReg(ROFDM0_ECCATHRESHOLD, pathB ? (1U << 25) : (1U << 29), (uint32_t)(((y * (int64_t)old) >> 7) & 1));
    if (txOnly) return;
    setBBReg(rxImb, 0x3FF, (uint32_t)result[cand][base + 2]);
    setBBReg(rxImb, 0xFC00, (uint32_t)(result[cand][base + 3] & 0x3F));
    if (!pathB) setBBReg(0xCA0, 0xF0000000, (uint32_t)((result[cand][3] >> 6) & 0xF));
}

// Returns a bitmask: bit0 path A (S1) calibrated, bit1 path B (S0) calibrated.
uint32_t RTL8723BE::iqCalibrate() {
    long result[4][8];
    memset(result, 0, sizeof(result));
    uint32_t pathSel = mmio_read32(0x948);
    uint8_t finalCand = 0xFF;

    for (uint8_t i = 0; i < 3; ++i) {
        iqkRun(result, i);
        if (i == 1 && iqkSimilar(result, 0, 1)) { finalCand = 0; break; }
        if (i == 2) {
            if (iqkSimilar(result, 0, 2)) { finalCand = 0; break; }
            if (iqkSimilar(result, 1, 2)) {
                finalCand = 1;
            } else {
                long sum = 0;
                for (int k = 0; k < 8; ++k) sum += result[3][k];
                finalCand = sum ? 3 : 0xFF;
            }
        }
    }

    uint32_t ok = 0;
    if (finalCand != 0xFF) {
        if (result[finalCand][0]) { iqkFill(result, finalCand, false, result[finalCand][2] == 0); ok |= 1; }
        if (result[finalCand][4]) { iqkFill(result, finalCand, true, result[finalCand][6] == 0); ok |= 2; }
    }
    mmio_write32(0x948, pathSel);
    IOLog("RTL8723BE: IQK candidate %u: A tx(%03lx,%03lx) rx(%03lx,%03lx) B tx(%03lx,%03lx) rx(%03lx,%03lx)\n",
          finalCand,
          finalCand < 4 ? result[finalCand][0] : 0, finalCand < 4 ? result[finalCand][1] : 0,
          finalCand < 4 ? result[finalCand][2] : 0, finalCand < 4 ? result[finalCand][3] : 0,
          finalCand < 4 ? result[finalCand][4] : 0, finalCand < 4 ? result[finalCand][5] : 0,
          finalCand < 4 ? result[finalCand][6] : 0, finalCand < 4 ? result[finalCand][7] : 0);
    return ok;
}

// Linux rtl8723be_phy_set_bw_mode_callback() for HT20
void RTL8723BE::setBandwidth20() {
    mmio_write8(REG_BWOPMODE, mmio_read8(REG_BWOPMODE) | 0x04);
    setBBReg(RFPGA0_RFMOD, 0x1, 0x0);
    setBBReg(RFPGA1_RFMOD, 0x1, 0x0);
    fRFChnlVal = (fRFChnlVal & 0xFFFFF3FFU) | (1U << 10) | (1U << 11);
    setRFReg(RF_CHNLBW, 0xFFFFF, fRFChnlVal);
}

void RTL8723BE::setTxPowerIndex(uint8_t cck, uint8_t ofdm) {
    if (cck > 0x3F) cck = 0x3F;
    if (ofdm > 0x3F) ofdm = 0x3F;
    // CCK 1M / 2M / 5.5M / 11M
    setBBReg(RTXAGC_A_CCK1_MCS32, 0x0000FF00U, cck);
    setBBReg(RTXAGC_B_CCK11_A_CCK2_11, 0xFFFFFF00U, (uint32_t)cck | ((uint32_t)cck << 8) | ((uint32_t)cck << 16));
    // OFDM 6..54M and HT MCS0..7
    uint32_t o = (uint32_t)ofdm * 0x01010101U;
    mmio_write32(RTXAGC_A_RATE18_06, o);
    mmio_write32(RTXAGC_A_RATE54_24, o);
    mmio_write32(RTXAGC_A_MCS03_MCS00, o);
    mmio_write32(RTXAGC_A_MCS07_MCS04, o);
}

bool RTL8723BE::setChannel(uint8_t channel) {
    if (channel < 1 || channel > 14) return false;

    // eFuse channel groups (Linux _rtl8723be_get_chnl_group)
    uint8_t group = channel <= 2 ? 0 : channel <= 5 ? 1 : channel <= 8 ? 2 : channel <= 11 ? 3 : 4;
    uint8_t cck = fCalib.tx_pwr_cck[channel == 14 ? 5 : group];
    int ofdmDiff = (int8_t)((fCalib.tx_pwr_ht20_diff & 0x0F) << 4) >> 4;
    int ofdm = (int)fCalib.tx_pwr_ht40[group] + (fCalib.tx_pwr_ht20_diff == 0xFF ? 0 : ofdmDiff);
    if (ofdm < 0) ofdm = 0;
    setTxPowerIndex(cck, (uint8_t)ofdm);

    fRFChnlVal = (fRFChnlVal & 0xFFFFFC00U) | channel;
    setRFReg(RF_CHNLBW, 0xFFFFF, fRFChnlVal);
    IODelay(10);
    fCurrentChannel = channel;
    return true;
}

void RTL8723BE::refreshLED(bool on) {
    // LED0 software control (Linux rtl8723be_sw_led_on/off); LED1 kept in sync for HP boards.
    uint8_t led = mmio_read8(REG_LEDCFG2);
    if (on) {
        mmio_write8(REG_LEDCFG2, (uint8_t)(((led & ~0x40) & 0xF0) | 0x20));
        mmio_write8(REG_LEDCFG1, mmio_read8(REG_LEDCFG1) & 0x10);
        mmio_write8(REG_LEDCFG0, mmio_read8(REG_LEDCFG0) & 0x70); // verified to light the HP white LED
    } else {
        mmio_write8(REG_LEDCFG2, (uint8_t)(((led & 0xF0) & ~0x40) | 0x08 | 0x20));
        mmio_write8(REG_LEDCFG1, (uint8_t)((mmio_read8(REG_LEDCFG1) & 0x10) | 0x08));
    }
}

// Linux _rtl8723be_check_pcie_dma_hang(): 0x350[26] enables the debug port,
// 0x350[25]/[24] then report an RX/TX DMA hang.
static bool pcieDmaHung(RTL8723BE *me) {
    uint8_t tmp = me->mmio_read8(0x0350 + 3);
    if (!(tmp & 0x04)) {
        me->mmio_write8(0x0350 + 3, tmp | 0x04);
        IOSleep(100);
    }
    tmp = me->mmio_read8(0x0350 + 3);
    return (tmp & 0x03) != 0;
}

// Linux _rtl8723be_reset_pcie_interface_dma()
static void resetPcieDma(RTL8723BE *me, bool macPowerOn) {
    me->mmio_write8(REG_RSV_CTRL, me->mmio_read8(REG_RSV_CTRL) & ~0x03);
    me->mmio_write8(0x00CC, me->mmio_read8(0x00CC) | 0x04);          // PMC_DBG_CTRL2 unlock

    uint8_t rxdma = me->mmio_read8(REG_RXDMA_CONTROL);
    bool releaseRxPause = !(rxdma & 0x04);
    if (releaseRxPause) me->mmio_write8(REG_RXDMA_CONTROL, rxdma | 0x04);
    uint8_t dmaPause = me->mmio_read8(REG_PCIE_CTRL_REG + 1);
    if (dmaPause != 0xFF) me->mmio_write8(REG_PCIE_CTRL_REG + 1, 0xFF);

    if (macPowerOn) me->mmio_write8(REG_CR, 0);
    me->mmio_write8(REG_SYS_FUNC_EN + 1, me->mmio_read8(REG_SYS_FUNC_EN + 1) & ~0x01);
    me->mmio_write8(REG_SYS_FUNC_EN + 1, me->mmio_read8(REG_SYS_FUNC_EN + 1) | 0x01);
    if (macPowerOn) me->mmio_write8(REG_CR, 0xFF);

    me->mmio_write8(0x00F8 + 2, me->mmio_read8(0x00F8 + 2) | 0x02); // MAC_PHY_CTRL_NORMAL autoload-down
    if (!macPowerOn) {
        if (releaseRxPause) me->mmio_write8(REG_RXDMA_CONTROL, me->mmio_read8(REG_RXDMA_CONTROL) & ~0x04);
        me->mmio_write8(REG_PCIE_CTRL_REG + 1, dmaPause);
    }
    me->mmio_write8(0x00CC, me->mmio_read8(0x00CC) & ~0x04);
}

// Linux rtl8723be_hw_init()
bool RTL8723BE::hwInit() {
    uint8_t cr = mmio_read8(REG_CR);
    bool macOn = (cr != 0 && cr != 0xEA);
    if (pcieDmaHung(this)) {
        IOLog("RTL8723BE: PCIe DMA hang detected on entry; resetting DMA engine\n");
        resetPcieDma(this, macOn);
        macOn = false;
    }
    if (macOn) {
        // MAC was left running (previous driver instance or firmware); start from a clean state.
        IOLog("RTL8723BE: MAC active on entry (CR=0x%02x); powering off first\n", cr);
        powerOff();
    }

    if (!initMac()) return false;
    fInitStage = kStageMac;

    if (!fCalib.autoload_ok) {
        // The chip may have been in power-down when start() first read the eFuse.
        if (readEfuse(fCalib)) {
            memcpy(fMyMAC, fCalib.mac_addr, 6);
            if (!fAntennaForced) {
                fAntNum = (fCalib.bt_setting & 0x01) ? 2 : 1;
                fActiveAntenna = (fAntNum == 1 && (fCalib.bt_setting & 0x40)) ? 2 : 1;
            }
            IOLog("RTL8723BE: eFuse re-read after power-on: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                  fMyMAC[0], fMyMAC[1], fMyMAC[2], fMyMAC[3], fMyMAC[4], fMyMAC[5]);
        }
    }

    mmio_write8(REG_SYS_CFG, mmio_read8(REG_SYS_CFG) & 0x7F);

    // Default to v36 like Linux; rtl8723be_fw=15 selects the older embedded v15 image.
    int fwSel = 36;
    PE_parse_boot_argn("rtl8723be_fw", &fwSel, sizeof(fwSel));
    const uint8_t *fw = (fwSel == 15) ? rtl8723befw_bin : rtl8723befw36_bin;
    size_t fwLen = (fwSel == 15) ? rtl8723befw_bin_len : rtl8723befw36_bin_len;
    fLastHmeBox = 0;
    if (downloadFirmware(fw, fwLen)) {
        IOLog("RTL8723BE: firmware v%u.%u running\n", fFwVersion, fFwSubVersion);
    } else {
        // Linux continues without firmware; RX/scan do not depend on it.
        IOLog("RTL8723BE: firmware not ready; continuing without it\n");
    }
    fInitStage = kStageFirmware;

    phyMacConfig();
    mmio_write32(REG_RCR, RTL8723BE_RCR_DEFAULT);
    fInitStage = kStageMacTable;

    phyBBConfig();
    fInitStage = kStageBB;

    phyRFConfig();
    fRFChnlVal = getRFReg(RF_CHNLBW, 0xFFFFF);
    fRFChnlVal = (fRFChnlVal & 0xFFF03FFU) | (1U << 10) | (1U << 11);
    fInitStage = kStageRF;

    hwConfigure();
    mmio_write32(REG_CAMCMD, 0xC0000000U);   // invalidate + clear all CAM entries
    mmio_write8(REG_SECCFG, 0x0000);        // software crypto only
    for (int i = 0; i < 6; ++i) mmio_write8(REG_MACID + i, fMyMAC[i]);

    btcoexInit();
    lcCalibrate();
    mmio_write8(REG_NAV_UPPER, (uint8_t)((30000 + 127) / 128));

    // Release RX DMA and TX/RX PCIe DMA.
    uint8_t rxdma = mmio_read8(REG_RXDMA_CONTROL);
    if (rxdma & 0x04) mmio_write8(REG_RXDMA_CONTROL, rxdma & ~0x04);
    mmio_write8(REG_PCIE_CTRL_REG + 1, 0x00);

    // Station, no link: accept beacons from every BSS.
    mmio_write8(REG_MSR, (uint8_t)(mmio_read8(REG_MSR) & 0xFC));
    for (int i = 0; i < 6; ++i) mmio_write8(REG_BSSID + i, 0);
    mmio_write32(REG_RCR, mmio_read32(REG_RCR) & ~(RCR_CBSSID_BCN | RCR_CBSSID_DATA));
    mmio_write16(REG_RXFLTMAP0, 0xFFFF);
    mmio_write16(REG_RXFLTMAP2, 0xFFFF);

    setBandwidth20();
    setChannel(1);
    fInitStage = kStageConfigured;

    fHwReady = true;
    fInitStage = kStageReady;
    return true;
}

// ============================================================================
// Multi-Queue DMA Ring Architecture
// ============================================================================

bool RTL8723BE::initDMARings() {
    const RTLQueueId queues[] = { Q_BK, Q_BE, Q_VI, Q_VO, Q_BCN, Q_MGNT, Q_HIGH };

    for (RTLQueueId q : queues) {
        size_t idx = (size_t)q;
        if (!allocDMA(fTxRing[idx], RTL8723BE_TX_DESC_COUNT * sizeof(TxDescPci), 256)) return false;
        fTxRingDescVirt[idx] = (TxDescPci*)fTxRing[idx].virt;
        for (int i = 0; i < RTL8723BE_TX_DESC_COUNT; ++i) {
            if (!allocDMA(fTxBuf[idx][i], RTL8723BE_TX_BUF_SIZE, 4)) return false;
        }
    }
    if (!allocDMA(fRxRing, RTL8723BE_RX_DESC_COUNT * sizeof(RxDesc32), 256)) return false;
    fRxRingDescVirt = (RxDesc32*)fRxRing.virt;
    for (int i = 0; i < RTL8723BE_RX_DESC_COUNT; ++i) {
        if (!allocDMA(fRxBuf[i], RTL8723BE_RX_BUF_SIZE, 8)) return false;
    }
    resetRings();
    return true;
}

// Linux _rtl_pci_init_trx_ring / rtl_pci_reset_trx_ring equivalent.
void RTL8723BE::resetRings() {
    for (int q = 0; q < 8; ++q) {
        fTxHostIdx[q] = 0;
        if (!fTxRingDescVirt[q]) continue;
        memset(fTxRingDescVirt[q], 0, RTL8723BE_TX_DESC_COUNT * sizeof(TxDescPci));
        for (int i = 0; i < RTL8723BE_TX_DESC_COUNT; ++i) {
            fTxRingDescVirt[q][i].set_next_addr(fTxRing[q].ioaddr +
                (size_t(i + 1) % RTL8723BE_TX_DESC_COUNT) * sizeof(TxDescPci));
        }
    }
    fRxHostIdx = 0;
    if (fRxRingDescVirt) {
        memset(fRxRingDescVirt, 0, RTL8723BE_RX_DESC_COUNT * sizeof(RxDesc32));
        for (int i = 0; i < RTL8723BE_RX_DESC_COUNT; ++i) {
            RxDesc32 &d = fRxRingDescVirt[i];
            d.set_buffer_addr(fRxBuf[i].ioaddr);
            d.set_length(RTL8723BE_RX_BUF_SIZE);
            if (i == RTL8723BE_RX_DESC_COUNT - 1) d.set_eor(true);
            OSSynchronizeIO();
            d.set_own(true);
        }
    }
    OSSynchronizeIO();
}

void RTL8723BE::freeDMARings() {
    for (int q = 0; q < 8; ++q) {
        for (int i = 0; i < RTL8723BE_TX_DESC_COUNT; ++i) freeDMA(fTxBuf[q][i]);
        freeDMA(fTxRing[q]);
        fTxRingDescVirt[q] = nullptr;
    }
    for (int i = 0; i < RTL8723BE_RX_DESC_COUNT; ++i) freeDMA(fRxBuf[i]);
    freeDMA(fRxRing);
    fRxRingDescVirt = nullptr;
}

// Fill a TX descriptor the way Linux rtl8723be_tx_fill_desc() does for a single-segment frame.
// `frame` must not include the FCS; the MAC appends it.
bool RTL8723BE::transmitRawFrame(RTLQueueId qId, const uint8_t *frame, size_t len, uint8_t rate) {
    size_t qIdx = (size_t)qId;
    if (!frame || len < 10 || len > RTL8723BE_TX_BUF_SIZE || qIdx >= 8 ||
        !fTxRingDescVirt[qIdx] || !fHwReady) {
        return false;
    }

    IOLockLock(fTxLock);
    uint16_t slot = fTxHostIdx[qIdx];
    TxDescPci *desc = &fTxRingDescVirt[qIdx][slot];
    if (desc->get_own() || !fTxBuf[qIdx][slot].virt) {
        fTxRingFull++;
        IOLockUnlock(fTxLock);
        return false;
    }

    memcpy(fTxBuf[qIdx][slot].virt, frame, len);

    const uint16_t fc = (uint16_t)(frame[0] | (frame[1] << 8));
    const bool isData = ((fc >> 2) & 0x3) == 2;
    const bool isQoS = isData && (fc & 0x0080);
    const bool multicast = (frame[4] & 0x01) != 0;

    uint64_t nextAddr = desc->get_next_addr();
    memset(desc, 0, sizeof(*desc));
    desc->set_next_addr(nextAddr);
    desc->set_offset(RTL_TX_HEADER_SIZE);
    desc->set_pktsize((uint16_t)len);
    desc->set_firstseg(true);
    desc->set_lastseg(true);
    desc->set_bmc(multicast);
    desc->set_queuesel(rtlTxQueueSelect(qId));
    desc->set_macid(0);
    const bool peerData = isData && qId != Q_MGNT && fState == kRTLStateConnected;
    desc->set_use_rate(!peerData || !fFirmwareRA);
    desc->set_shortgi(peerData && fPeer.ht && fPeer.shortGI);
    if (peerData && fTxCrypto && (fc & 0x4000)) desc->set_security(3);
    if (peerData && fPeer.protection) {
        desc->set_cts_to_self(true);
        desc->set_rts_rate(DESC_RATE11M);
    }
    desc->set_tx_rate(rate);
    desc->set_data_rate_fb_limit(0x1F);
    desc->set_rts_rate_fb_limit(0xF);
    if (!isData) {
        desc->set_rate_id(7);           // RATR_INX_WIRELESS_MC
        desc->set_disable_fb(true);
    } else {
        desc->set_rate_id(peerData ? fRateID : 7); // Linux ARFR ID, not RATR index
    }
    if (!isQoS) {
        desc->set_hwseq_en(true);       // hardware assigns the sequence number
    } else {
        desc->set_seq(fTxSeq++);
    }
    desc->set_buffer_size((uint16_t)len);
    desc->set_buffer_addr(fTxBuf[qIdx][slot].ioaddr);
    OSSynchronizeIO();
    desc->set_own(true);
    OSSynchronizeIO();

    // Doorbell (Linux rtl8723be_tx_polling)
    mmio_write16(REG_PCIE_CTRL_REG, (uint16_t)(qId == Q_BCN ? (1U << 4) : (1U << (uint8_t)qId)));

    fTxHostIdx[qIdx] = (uint16_t)((slot + 1) % RTL8723BE_TX_DESC_COUNT);
    fTxPackets++;
    IOLockUnlock(fTxLock);
    return true;
}

// ============================================================================
// Interrupt Handling
// ============================================================================

void RTL8723BE::enableInterrupts() {
    if (!fMMIOBase) return;
    mmio_write32(REG_HISR, 0xFFFFFFFFU);
    mmio_write32(REG_HISRE, 0xFFFFFFFFU);
    // Only the sources this driver services; HSISR/C2H/PS timeout stay masked.
    fHIMRMask = IMR_ROK | IMR_RDU | IMR_VODOK | IMR_VIDOK | IMR_BEDOK | IMR_BKDOK |
                IMR_MGNTDOK | IMR_HIGHDOK;
    fInterruptEnabled = true;
    mmio_write32(REG_HIMR, fHIMRMask);
    mmio_write32(REG_HIMRE, RTL8723BE_IMRE_DEFAULT);
}

void RTL8723BE::disableInterrupts() {
    fInterruptEnabled = false;
    fHIMRMask = 0;
    if (!fMMIOBase) return;
    mmio_write32(REG_HIMR, 0);
    mmio_write32(REG_HIMRE, 0);
}

bool RTL8723BE::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src) {
    (void)src;
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (!me || !me->fMMIOBase || !me->fInterruptEnabled || me->fHIMRMask == 0) {
        return false;
    }

    uint32_t hisr = me->mmio_read32(REG_HISR);
    if (hisr == 0xFFFFFFFFU) {
        return false;
    }
    uint32_t active = hisr & me->fHIMRMask;
    uint32_t hisre = me->mmio_read32(REG_HISRE) & RTL8723BE_IMRE_DEFAULT;
    if (active == 0 && hisre == 0) {
        return false;
    }

    // Mask until the work loop has drained the rings, then acknowledge (W1C).
    me->mmio_write32(REG_HIMR, 0);
    me->mmio_write32(REG_HISR, active);
    if (hisre) me->mmio_write32(REG_HISRE, hisre);
    me->fIntCount++;
    return true;
}

void RTL8723BE::interruptAction(OSObject *owner, IOInterruptEventSource *src, int count) {
    (void)src;
    (void)count;
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (!me || !me->fMMIOBase) return;

    me->handleRxInterrupt();
    me->handleTxInterrupt();

    if (me->fInterruptEnabled && me->fHIMRMask != 0) {
        me->mmio_write32(REG_HIMR, me->fHIMRMask);
    }
}

// Signal strength in dBm from the PHY status report (Linux _rtl8723be_query_rxphystatus).
int8_t RTL8723BE::rxSignalDbm(const RxDesc32 *desc, const uint8_t *phy) {
    if (!desc->get_physt() || desc->get_drv_infosize() < 1) return -100;
    if (desc->get_rxmcs() < DESC_RATE6M) {
        uint8_t agc = phy[5];
        uint8_t lna = (agc & 0xE0) >> 5;
        uint8_t vga = agc & 0x1F;
        int pwr;
        switch (lna) {
        case 6: pwr = -34 - 2 * vga; break;
        case 4: pwr = -14 - 2 * vga; break;
        case 1: pwr = 6 - 2 * vga; break;
        case 0: pwr = 16 - 2 * vga; break;
        default: pwr = -100; break;
        }
        return (int8_t)(pwr < -100 ? -100 : pwr > 0 ? 0 : pwr);
    }
    int pwr = ((phy[4] >> 1) & 0x7F) - 110;
    return (int8_t)(pwr < -100 ? -100 : pwr > 0 ? 0 : pwr);
}

void RTL8723BE::handleRxInterrupt() {
    if (!fRxRingDescVirt || !fHwReady) return;

    for (int budget = RTL8723BE_RX_DESC_COUNT; budget > 0; --budget) {
        RxDesc32 *desc = &fRxRingDescVirt[fRxHostIdx];
        if (desc->get_own()) {
            break; // still owned by the NIC
        }
        OSSynchronizeIO();
        fRxDescDone++;

        // Buffer layout (rtl_pci_rx_interrupt): [drv_info * 8][shift][802.11 frame incl. FCS]
        const uint8_t *buf = (const uint8_t*)fRxBuf[fRxHostIdx].virt;
        size_t hdrOff = (size_t)desc->get_drv_infosize() * 8 + desc->get_shift();
        size_t len = desc->get_length();

        const uint8_t rate = desc->get_rxmcs();
        const int mod = rate < DESC_RATE6M ? 0 : (rate <= DESC_RATE54M ? 1 : 2);
        fCurRxRate = rate;
        if (desc->get_c2h_report()) {
            // Linux rtl8723be_rx_query_desc(): C2H has no 802.11 FCS to strip.
            // Dynamic coexistence/rate report consumers are not yet ported.
        } else if (desc->get_crc32_err()) {
            fRxCrc[mod]++;
            fRxErrors++;
        } else if (desc->get_icv_err()) {
            fRxDropped++;
        } else if (buf && len >= 14 && hdrOff + len <= RTL8723BE_RX_BUF_SIZE) {
            fRxOk[mod]++;
            int8_t rssi = rxSignalDbm(desc, buf);
            processRxFrame(buf + hdrOff, len - 4, rssi);   // RCR_APPFCS: drop the FCS
            fRxPackets++;
        } else {
            fRxDropped++;
        }

        bool isLast = (fRxHostIdx == RTL8723BE_RX_DESC_COUNT - 1);
        desc->dw0 = 0;
        desc->set_length(RTL8723BE_RX_BUF_SIZE);
        desc->set_buffer_addr(fRxBuf[fRxHostIdx].ioaddr);
        if (isLast) desc->set_eor(true);
        OSSynchronizeIO();
        desc->set_own(true);
        OSSynchronizeIO();
        fRxHostIdx = isLast ? 0 : (uint16_t)(fRxHostIdx + 1);
    }
}

void RTL8723BE::handleTxInterrupt() {
    // Resume queued mbufs after hardware releases a descriptor (no packet drop on ring pressure).
    if (fState == kRTLStateConnected) if (IOOutputQueue *q = getOutputQueue()) q->service();
}

// ============================================================================
// Scanner (runs on the work loop; the user-client thread sleeps on the gate)
// ============================================================================

void RTL8723BE::sendProbeRequest() {
    static const uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
    static const uint8_t extRates[] = {0x32, 0x04, 0x30, 0x48, 0x60, 0x6C};
    uint8_t *f = fTxFrame;
    size_t pos = 0;
    f[pos++] = 0x40; f[pos++] = 0x00;           // Probe Request
    f[pos++] = 0x00; f[pos++] = 0x00;           // duration
    memset(f + pos, 0xFF, 6); pos += 6;         // DA broadcast
    memcpy(f + pos, fMyMAC, 6); pos += 6;       // SA
    memset(f + pos, 0xFF, 6); pos += 6;         // BSSID wildcard
    f[pos++] = 0x00; f[pos++] = 0x00;           // sequence (hardware)
    f[pos++] = 0x00; f[pos++] = 0x00;           // wildcard SSID
    memcpy(f + pos, rates, sizeof(rates)); pos += sizeof(rates);
    memcpy(f + pos, extRates, sizeof(extRates)); pos += sizeof(extRates);
    f[pos++] = 0x03; f[pos++] = 0x01; f[pos++] = fScanCurrentChannel; // DS parameter set
    transmitRawFrame(Q_MGNT, f, pos, DESC_RATE1M);
}

void RTL8723BE::scanStep() {
    handleRxInterrupt();   // also poll, in case MSI delivery is delayed

    if (!fScanActive) return;
    if (fScanCurrentChannel >= 13) {
        const uint32_t passScore = fRxMgmt - fPassMgmtStart;
        const bool canSwitch = !fAntennaForced && fState != kRTLStateConnected;
        if (canSwitch && !fScanRetriedOtherAntenna && (!fAntennaCalibrated || fScanResults.count == 0)) {
            // The eFuse antenna info on many single-antenna HP boards does not match the wiring
            // (Linux needs ant_sel= there). Sweep the other internal path too and keep the better one.
            fScanRetriedOtherAntenna = true;
            fPass1Score = passScore;
            fActiveAntenna = (fActiveAntenna == 2) ? 1 : 2;
            mmio_write32(0x0948, fActiveAntenna == 2 ? 0x280 : 0x0);
            fPassMgmtStart = fRxMgmt;
            fScanCurrentChannel = 0;
        } else {
            if (canSwitch && fScanRetriedOtherAntenna) {
                if (fPass1Score > passScore) {
                    fActiveAntenna = (fActiveAntenna == 2) ? 1 : 2;
                    mmio_write32(0x0948, fActiveAntenna == 2 ? 0x280 : 0x0);
                }
                if (!fAntennaCalibrated) {
                    IOLog("RTL8723BE: antenna calibration: %s selected (%u vs %u frames)\n",
                          fActiveAntenna == 2 ? "AUX/S0" : "MAIN/S1",
                          fPass1Score > passScore ? fPass1Score : passScore,
                          fPass1Score > passScore ? passScore : fPass1Score);
                }
                fAntennaCalibrated = true;
                if (fActiveAntenna == 2 && fAntNum == 2) {
                    // The wiring contradicts the eFuse (single antenna on S0): behave like ant_sel=1.
                    fAntNum = 1;
                    applyCoexProfile();
                }
            }
            finishScan();
            return;
        }
    }
    fScanCurrentChannel++;
    setChannel(fScanCurrentChannel);
    sendProbeRequest();
    fScanTimer->setTimeoutMS(RTL8723BE_SCAN_DWELL_MS);
}

void RTL8723BE::finishScan() {
    setChannel(fHomeChannel);
    applyCoexProfile(); // restore PTA after scan's temporary Wi-Fi antenna selection
    if (fState == kRTLStateConnected) {
        sendNullDataFrame(false);
    }
    fScanActive = false;
    IOLog("RTL8723BE: Scan completed, %u access points discovered (rx desc %u, beacons %u, ints %u)\n",
          fScanResults.count, fRxDescDone, fRxBeacons, fIntCount);
    fCommandGate->commandWakeup(&fScanActive);
}

void RTL8723BE::scanTimerAction(OSObject *owner, IOTimerEventSource *timer) {
    (void)timer;
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (me && me->fHwReady) me->scanStep();
}

void RTL8723BE::linkTimerAction(OSObject *owner, IOTimerEventSource *timer) {
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (!me || !me->fHwReady) return;
    ++me->fLinkTicks;
    if (!me->fScanActive) {
        // Host policy: bounded authentication/association and handshake waits, like BSD's
        // management watchdog. These are deadlines, not retransmission-triggered extensions.
        bool pending = me->fState >= kRTLStateAuthenticating && me->fState <= kRTLStateHandshake;
        bool lost = me->fState == kRTLStateConnected && me->fLinkTicks - me->fLastBSSActivity >= 30;
        if ((pending && int32_t(me->fLinkTicks - me->fHandshakeDeadline) >= 0) || lost) {
            IOLog("RTL8723BE: %s; disconnecting for application auto-join\n", lost ? "BSS inactivity timeout" : "connection exchange timeout");
            me->deauthTeardown();
        }
    } else me->fLastBSSActivity = me->fLinkTicks;
    if (!me->fScanActive && me->fState == kRTLStateConnected && me->fFirmwareRA && me->fLinkRSSI > -100) {
        // Linux rtl8723be_dm_check_rssi_monitor(), H2C_RSSIBE_REPORT.
        const uint8_t report[3] = {0,0x20,uint8_t(me->fLinkRSSI + 100)};
        me->sendH2C(0x42, report, sizeof(report));
    }
    timer->setTimeoutMS(1000);
}

IOReturn RTL8723BE::gatedScan(OSObject *owner, void *, void *, void *, void *) {
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (!me || !me->fHwReady) return kIOReturnNotReady;
    if (me->fScanActive || (me->fState != kRTLStateDisconnected && me->fState != kRTLStateConnected)) return kIOReturnBusy;

    me->fScanActive = true;
    me->fScanRetriedOtherAntenna = false;
    me->fPassMgmtStart = me->fRxMgmt;
    me->fScanResults.count = 0;
    me->fHomeChannel = me->fCurrentChannel;
    me->fScanCurrentChannel = 0;
    if (me->fState == kRTLStateConnected) {
        me->sendNullDataFrame(true);
    }
    me->scanStep();

    AbsoluteTime deadline;
    clock_interval_to_deadline(RTL8723BE_SCAN_TIMEOUT_MS * 2, kMillisecondScale, &deadline);
    while (me->fScanActive) {
        if (me->fCommandGate->commandSleep(&me->fScanActive, deadline, THREAD_ABORTSAFE) != THREAD_AWAKENED) {
            break;
        }
    }
    if (me->fScanActive && me->fHwReady) {
        me->fScanTimer->cancelTimeout();
        me->finishScan();
    }
    return me->fHwReady ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn RTL8723BE::startScan() {
    if (!fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedScan);
}

// ============================================================================
// User-client operations (serialized on the work loop)
// ============================================================================

IOReturn RTL8723BE::gatedCall(OSObject *owner, void *op, void *a1, void *a2, void *a3) {
    RTL8723BE *me = OSDynamicCast(RTL8723BE, owner);
    if (!me) return kIOReturnNotAttached;
    uintptr_t code = (uintptr_t)op;

    switch (code) {
    case kOpLinkInfo: {
        RTL8723BELinkInfo *info = (RTL8723BELinkInfo *)a1;
        memset(info, 0, sizeof(*info));
        info->version = 1; info->options = me->fOptions;
        info->rate_mask = me->fPeer.rates | (me->fPeer.ht ? uint32_t(me->fPeer.mcs) << 12 : 0);
        info->ht20 = me->fPeer.ht; info->short_gi = me->fPeer.ht && me->fPeer.shortGI;
        info->wmm = me->fPeer.wmm; info->firmware_rate_control = me->fFirmwareRA;
        info->tx_crypto = me->fTxCrypto; info->max_rate = me->fDataRate;
        return kIOReturnSuccess;
    }
    case kOpOptions: {
        uint32_t options = uint32_t((uintptr_t)a1);
        if (!me->fHwReady) return kIOReturnNotReady;
        if (options & ~(kRTLOptionHT | kRTLOptionTxCrypto | kRTLOptionBluetoothCoexist)) return kIOReturnBadArgument;
        if (me->fState != kRTLStateDisconnected || me->fScanActive) return kIOReturnBusy;
        me->fOptions = options;
        me->applyCoexProfile();
        return kIOReturnSuccess;
    }
    case kOpConnect: {
        struct Args { const RTL8723BEConnectParams *p; const char *ssid; const char *pass; uint8_t *pmk; };
        Args *a = (Args *)a1;
        if (!me->fHwReady) return kIOReturnNotReady;
        if (me->fScanActive) return kIOReturnBusy;
        bool found = false;
        for (uint32_t i = 0; i < me->fScanResults.count; ++i) {
            if (memcmp(me->fScanResults.aps[i].bssid, a->p->bssid, 6) == 0 &&
                strcmp(me->fScanResults.aps[i].ssid, a->ssid) == 0 &&
                me->fScanResults.aps[i].is_wpa2 && me->fScanPeers[i].rates) found = true;
        }
        if (!found) return kIOReturnNotFound;
        if (me->fState != kRTLStateDisconnected) me->deauthTeardown();
        strlcpy(me->fConnectedSSID, a->ssid, sizeof(me->fConnectedSSID));
        memcpy(me->fConnectedBSSID, a->p->bssid, 6);
        memcpy(me->fPMK, a->pmk, sizeof(me->fPMK));
        return me->beginConnect();
    }
    case kOpDisconnect:
        if (!me->fHwReady) return kIOReturnNotReady;
        me->deauthTeardown();
        return kIOReturnSuccess;
    case kOpIQK: {
        if (!me->fHwReady) return kIOReturnNotReady;
        if (me->fScanActive) return kIOReturnBusy;
        uint32_t ok = me->iqCalibrate();
        *(uint32_t *)a1 = ok;
        return kIOReturnSuccess;
    }
    case kOpScanResults:
        memcpy(a1, &me->fScanResults, sizeof(RTL8723BEScanResults));
        return kIOReturnSuccess;
    case kOpStatus: {
        RTL8723BEStatus *st = (RTL8723BEStatus *)a1;
        memset(st, 0, sizeof(*st));
        st->state = (uint32_t)me->fState;
        memcpy(st->mac, me->fMyMAC, 6);
        memcpy(st->bssid, me->fConnectedBSSID, 6);
        strlcpy(st->ssid, me->fConnectedSSID, sizeof(st->ssid));
        st->channel = me->fCurrentChannel;
        st->rssi = -100;
        for (uint32_t i = 0; i < me->fScanResults.count; ++i) {
            if (memcmp(me->fScanResults.aps[i].bssid, me->fConnectedBSSID, 6) == 0) {
                st->rssi = me->fScanResults.aps[i].rssi;
            }
        }
        if (me->fState == kRTLStateConnected) st->rssi = me->fLinkRSSI;
        st->antenna = me->fActiveAntenna;
        st->tx_packets = me->fTxPackets;
        st->rx_packets = me->fRxPackets;
        st->tx_errors = me->fTxErrors;
        st->rx_errors = me->fRxErrors;
        return kIOReturnSuccess;
    }
    case kOpAntenna: {
        uint8_t ant = (uint8_t)(uintptr_t)a1;
        if (ant != 1 && ant != 2) return kIOReturnBadArgument;
        if (!me->fHwReady) return kIOReturnNotReady;
        me->fActiveAntenna = ant;
        me->fAntennaForced = true;
        me->mmio_write32(0x0948, ant == 2 ? 0x280 : 0x0);
        if (ant == 2 && me->fAntNum == 2) me->fAntNum = 1;   // single antenna on S0 (ant_sel=1)
        me->applyCoexProfile();
        IOLog("RTL8723BE: Wi-Fi RF path set to %s\n", ant == 2 ? "AUX/S0" : "MAIN/S1");
        return kIOReturnSuccess;
    }
    case kOpReadReg: {
        if (!me->fHwReady) return kIOReturnNotReady;
        uint32_t addr = (uint32_t)(uintptr_t)a1, width = (uint32_t)(uintptr_t)a2;
        uint32_t *out = (uint32_t *)a3;
        if (addr >= 0x4000 || (addr & (width - 1)) != 0) return kIOReturnBadArgument;
        if (width == 1) *out = me->mmio_read8(addr);
        else if (width == 2) *out = me->mmio_read16(addr);
        else if (width == 4) *out = me->mmio_read32(addr);
        else return kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    case kOpWriteReg: {
        if (!me->fHwReady) return kIOReturnNotReady;
        uint32_t addr = (uint32_t)(uintptr_t)a1, width = (uint32_t)(uintptr_t)a2;
        uint32_t value = (uint32_t)(uintptr_t)a3;
        if (addr >= 0x4000 || (addr & (width - 1)) != 0) return kIOReturnBadArgument;
        if (width == 1) me->mmio_write8(addr, (uint8_t)value);
        else if (width == 2) me->mmio_write16(addr, (uint16_t)value);
        else if (width == 4) me->mmio_write32(addr, value);
        else return kIOReturnBadArgument;
        IOLog("RTL8723BE: debug write%u 0x%04x = 0x%08x\n", width * 8, addr, value);
        return kIOReturnSuccess;
    }
    case kOpReadRF: {
        uint32_t off = (uint32_t)(uintptr_t)a1;
        if (off > 0xFF || !me->fHwReady) return off > 0xFF ? kIOReturnBadArgument : kIOReturnNotReady;
        *(uint32_t *)a2 = me->getRFReg((uint8_t)off, 0xFFFFF);
        return kIOReturnSuccess;
    }
    case kOpWriteRF: {
        uint32_t off = (uint32_t)(uintptr_t)a1;
        if (off > 0xFF || !me->fHwReady) return off > 0xFF ? kIOReturnBadArgument : kIOReturnNotReady;
        me->setRFReg((uint8_t)off, 0xFFFFF, (uint32_t)(uintptr_t)a2);
        if (off == RF_CHNLBW) me->fRFChnlVal = (uint32_t)(uintptr_t)a2 & 0xFFFFF;
        IOLog("RTL8723BE: debug RF write 0x%02x = 0x%05x\n", off, (uint32_t)(uintptr_t)a2);
        return kIOReturnSuccess;
    }
    case kOpDiag: {
        RTL8723BEDiag *d = (RTL8723BEDiag *)a1;
        memset(d, 0, sizeof(*d));
        d->version = RTL8723BE_DIAG_VERSION;
        d->init_stage = me->fInitStage;
        d->fw_version = ((uint32_t)me->fFwVersion << 16) | me->fFwSubVersion;
        if (me->fMMIOBase && me->fHwReady) {
            d->mcufwdl = me->mmio_read32(REG_MCUFWDL);
            d->cr = me->mmio_read16(REG_CR);
            d->rcr = me->mmio_read32(REG_RCR);
            d->himr = me->mmio_read32(REG_HIMR);
            d->hisr = me->mmio_read32(REG_HISR);
            d->rxdma_ctrl = me->mmio_read8(REG_RXDMA_CONTROL) | ((uint32_t)me->mmio_read8(REG_RXPKT_NUM) << 8);
            // Latch the BB statistics counters (Linux rtl8723be_dm_false_alarm_counter_statistics).
            me->setBBReg(0x0C00, 1U << 31, 1);
            me->setBBReg(0x0D00, 1U << 31, 1);
            d->bb_ofdm_cca = me->mmio_read32(0x0DA0) & 0xFFFF;
            uint32_t cck = me->mmio_read32(0x0A60);
            d->bb_cck_cca = ((cck & 0xFF) << 8) | ((cck & 0xFF00) >> 8);
            me->setBBReg(0x0C00, 1U << 31, 0);
            me->setBBReg(0x0D00, 1U << 31, 0);
            d->reg_948 = me->mmio_read32(0x0948);
            d->reg_92c = me->mmio_read32(0x092C);
            d->reg_4c = me->mmio_read32(0x004C);
            d->reg_765 = me->mmio_read8(0x0765);
            d->reg_76e = me->mmio_read8(0x076E);
            d->reg_67 = me->mmio_read8(0x0067);
            if (me->fHwReady) {
                d->rf_chnlbw = me->getRFReg(RF_CHNLBW, 0xFFFFF);
                d->rf_mode = me->getRFReg(0x00, 0xFFFFF);
            }
        }
        d->interrupts = me->fIntCount;
        d->rx_desc_done = me->fRxDescDone;
        d->rx_mgmt = me->fRxMgmt;
        d->rx_beacons = me->fRxBeacons;
        d->rx_data = me->fRxData;
        d->rx_ctrl = me->fRxCtrl;
        d->rx_crc_err = (uint32_t)me->fRxErrors;
        d->rx_dropped = me->fRxDropped;
        d->tx_submitted = (uint32_t)me->fTxPackets;
        d->tx_ring_full = me->fTxRingFull;
        d->rx_ok_cck = me->fRxOk[0]; d->rx_ok_ofdm = me->fRxOk[1]; d->rx_ok_ht = me->fRxOk[2];
        d->rx_crc_cck = me->fRxCrc[0]; d->rx_crc_ofdm = me->fRxCrc[1]; d->rx_crc_ht = me->fRxCrc[2];
        d->uc_data_rx = me->fUcDataRx; d->uc_delivered = me->fUcDelivered;
        d->uc_replay = me->fUcReplay; d->uc_mic_fail = me->fUcMicFail;
        d->bc_delivered = me->fBcDelivered; d->bc_mic_fail = me->fBcMicFail;
        d->last_uc_fc_pn_lo = me->fLastUcFcPn; d->last_uc_rate_len = me->fLastUcRateLen;
        d->rx_bytes = me->fRxBytes; d->tx_bytes = me->fTxBytes;
        d->efuse_used = me->fCalib.efuse_used;
        d->efuse_autoload_ok = me->fCalib.autoload_ok ? 1 : 0;
        d->package_type = me->fCalib.package_type;
        d->ant_num = me->fAntNum;
        d->ant_path = me->fActiveAntenna;
        d->channel = me->fCurrentChannel;
        d->svid = me->fCalib.svid;
        d->smid = me->fCalib.smid;
        return kIOReturnSuccess;
    }
    }
    return kIOReturnUnsupported;
}

IOReturn RTL8723BE::getScanResults(RTL8723BEScanResults *outResults) {
    if (!outResults) return kIOReturnBadArgument;
    if (!fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpScanResults, outResults);
}

IOReturn RTL8723BE::getStatus(RTL8723BEStatus *outStatus) {
    if (!outStatus) return kIOReturnBadArgument;
    if (!fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpStatus, outStatus);
}

IOReturn RTL8723BE::setAntennaPath(uint8_t ant) {
    if (!fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpAntenna,
                                   (void *)(uintptr_t)ant);
}

IOReturn RTL8723BE::debugReadReg(uint32_t addr, uint32_t width, uint32_t *outValue) {
    if (!outValue || !fCommandGate) return kIOReturnBadArgument;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpReadReg,
                                   (void *)(uintptr_t)addr, (void *)(uintptr_t)width, outValue);
}

IOReturn RTL8723BE::debugWriteReg(uint32_t addr, uint32_t width, uint32_t value) {
    if (!fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpWriteReg,
                                   (void *)(uintptr_t)addr, (void *)(uintptr_t)width,
                                   (void *)(uintptr_t)value);
}

IOReturn RTL8723BE::debugReadRF(uint32_t offset, uint32_t *outValue) {
    if (!outValue || !fCommandGate) return kIOReturnBadArgument;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpReadRF,
                                   (void *)(uintptr_t)offset, outValue);
}

IOReturn RTL8723BE::debugWriteRF(uint32_t offset, uint32_t value) {
    if (!fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpWriteRF,
                                   (void *)(uintptr_t)offset, (void *)(uintptr_t)value);
}

IOReturn RTL8723BE::runIQK(uint32_t *outMask) {
    if (!outMask || !fCommandGate) return kIOReturnBadArgument;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpIQK, outMask);
}

IOReturn RTL8723BE::getDiag(RTL8723BEDiag *outDiag) {
    if (!outDiag || !fCommandGate) return kIOReturnBadArgument;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpDiag, outDiag);
}

// ============================================================================
// 802.11 frame parsing
// ============================================================================

// `len` excludes the FCS.
bool RTL8723BE::parseBeaconOrProbe(const uint8_t *frame, size_t len, RTL8723BEDiscoveredAP &outAP) {
    if (!frame || len < 36) return false;

    memset(&outAP, 0, sizeof(outAP));
    memcpy(outAP.bssid, frame + 16, 6);
    outAP.beacon_interval = (uint16_t)(frame[32] | (((uint16_t)frame[33]) << 8));
    outAP.capabilities = (uint16_t)(frame[34] | (((uint16_t)frame[35]) << 8));
    rtl_wifi::Peer peer;
    if (!rtl_wifi::parseIEs(frame + 36, len - 36, peer)) return false;
    outAP.is_wpa2 = peer.wpa2 && (outAP.capabilities & 0x10) ? 1 : 0;
    outAP.channel = fCurrentChannel;

    size_t offset = 36;
    while (offset + 2 <= len) {
        uint8_t tag = frame[offset];
        uint8_t tagLen = frame[offset + 1];
        offset += 2;
        if (offset + tagLen > len) break;

        if (tag == 0 && tagLen <= RTL8723BE_MAX_SSID_LEN) {
            memcpy(outAP.ssid, frame + offset, tagLen);
            outAP.ssid[tagLen] = '\0';
        } else if (tag == 3 && tagLen >= 1) {
            outAP.channel = frame[offset];
        }
        offset += tagLen;
    }
    return true;
}

// `len` excludes the FCS.
void RTL8723BE::processRxFrame(const uint8_t *frame, size_t len, int8_t rssi) {
    if (!frame || len < 10) return;

    const uint8_t type = (frame[0] >> 2) & 0x03;
    const uint8_t subtype = (frame[0] >> 4) & 0x0F;

    if (type == 1) {
        fRxCtrl++;
        return;
    }
    if (len < 24) return;

    if (type == 0) {
        fRxMgmt++;
        if (subtype == 8 || subtype == 5) { // Beacon or Probe Response
            if (subtype == 8) fRxBeacons++;
            RTL8723BEDiscoveredAP ap;
            if (!parseBeaconOrProbe(frame, len, ap)) return;
            rtl_wifi::Peer peer;
            if (!rtl_wifi::parseIEs(frame + 36, len - 36, peer)) return;
            if (memcmp(frame + 10, fConnectedBSSID, 6) == 0 && memcmp(frame + 16, fConnectedBSSID, 6) == 0) {
                fLastBSSActivity = fLinkTicks;
                if (rssi > -100) fLinkRSSI = rssi;
            }
            ap.rssi = rssi;
            if (ap.channel < 1 || ap.channel > 14) ap.channel = fCurrentChannel;
            for (uint32_t i = 0; i < fScanResults.count; ++i) {
                if (memcmp(fScanResults.aps[i].bssid, ap.bssid, 6) == 0) {
                    // Keep the strongest reading and any SSID a probe response revealed.
                    if (ap.ssid[0] == '\0') memcpy(ap.ssid, fScanResults.aps[i].ssid, sizeof(ap.ssid));
                    if (fScanResults.aps[i].rssi > ap.rssi) ap.rssi = fScanResults.aps[i].rssi;
                    fScanResults.aps[i] = ap;
                    fScanPeers[i] = peer;
                    return;
                }
            }
            if (fScanResults.count < RTL8723BE_MAX_SCAN_APS) {
                fScanPeers[fScanResults.count] = peer;
                fScanResults.aps[fScanResults.count++] = ap;
            }
            return;
        }

        // Everything below must come from the AP we are talking to, addressed to us.
        if (memcmp(frame + 10, fConnectedBSSID, 6) != 0) return;
        if (memcmp(frame + 16, fConnectedBSSID, 6) != 0) return;
        const bool toUs = memcmp(frame + 4, fMyMAC, 6) == 0;
        const bool bcast = (frame[4] & 0x01) != 0;
        if (subtype == 11 && toUs) {
            handleAuthResponse(frame, len);
        } else if ((subtype == 1 || subtype == 3) && toUs) {
            handleAssocResponse(frame, len);
        } else if ((subtype == 12 || subtype == 10) && (toUs || bcast) && fState != kRTLStateDisconnected) {
            handleDeauthFrame(frame, len);
        } else if (subtype == 13 && toUs) {
            handleActionFrame(frame, len);
        }
        return;
    }

    if (type != 2) return;
    fRxData++;

    // Only frames From-DS of our BSS addressed to us or group-addressed.
    if ((frame[1] & 0x03) != 0x02 || memcmp(frame + 10, fConnectedBSSID, 6) != 0) return;
    if (!(frame[4] & 1) && memcmp(frame + 4, fMyMAC, 6) != 0) return;
    if ((frame[1] & 4) || (frame[22] & 15)) return; // fragmentation requires a separate authenticated reassembly path
    if (subtype & 0x04) return; // null / no-data frames

    size_t hdrLen = (subtype & 0x08) ? 26 : 24;
    if ((subtype & 0x08) && (frame[1] & 0x80)) hdrLen += 4; // HT control present
    if (len < hdrLen + 8) return;
    const bool isProtected = (frame[1] & 0x40) != 0;

    if (!isProtected) {
        const uint8_t *llc = frame + hdrLen;
        if (rtl_wifi::snapHeader(llc, len - hdrLen) && llc[6] == 0x88 && llc[7] == 0x8E &&
            memcmp(frame + 4, fMyMAC, 6) == 0 && !((subtype & 8) && (frame[24] & 0x80))) {
            handleEAPOLFrame(frame, len, hdrLen);
        }
        return; // never deliver unprotected data to the stack
    }
    if ((fState != kRTLStateConnected && fState != kRTLStateHandshake) || !fKeyInstalled) return;
    if (len < hdrLen + 8 + 8 + 8) return; // MAC + CCMP hdr + LLC + MIC

    const uint8_t *ccmp = frame + hdrLen;
    if ((ccmp[3] & 0x20) == 0) return; // ExtIV must be set for CCMP
    uint64_t pn = ((uint64_t)ccmp[0]) | (((uint64_t)ccmp[1]) << 8) |
                  (((uint64_t)ccmp[4]) << 16) | (((uint64_t)ccmp[5]) << 24) |
                  (((uint64_t)ccmp[6]) << 32) | (((uint64_t)ccmp[7]) << 40);

    const bool isMulticast = (frame[4] & 0x01) != 0;
    const uint8_t tid = (subtype & 0x08) ? (frame[24] & 0x0F) : 0;
    if (!isMulticast) {
        fUcDataRx++;
        fLastUcFcPn = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16) | (uint32_t)(pn & 0xFFFF);
        fLastUcRateLen = ((uint32_t)fCurRxRate << 16) | (uint32_t)(len & 0xFFFF);
    }
    const uint8_t keyId = (ccmp[3] >> 6) & 0x03;

    if (ccmp[2] != 0 || (ccmp[3] & 0x3f) != 0x20 || pn == 0) return;
    if (isMulticast) {
        if (pn <= fRxGtkPN[keyId][tid]) return;
    } else {
        if (pn <= fRxPtkPN[tid]) { fUcReplay++; return; }
    }

    uint8_t nonce[13];
    uint8_t aad[30];
    rtl_crypto::ccmp_build_nonce(frame, hdrLen, pn, nonce);
    size_t aadLen = rtl_crypto::ccmp_build_aad(frame, hdrLen, aad);
    if (aadLen == 0) return;

    size_t cipherLen = len - hdrLen - 8 - 8;
    const uint8_t *ciphertext = frame + hdrLen + 8;
    const uint8_t *mic = ciphertext + cipherLen;
    if (cipherLen > sizeof(fRxPlain)) return;

    if ((isMulticast && !fWPA.hasGTK(keyId)) || (!isMulticast && keyId != 0)) return;
    const uint8_t *key = isMulticast ? fWPA.gtk(keyId) : (fPTK + 32);
    if (!rtl_crypto::ccmp_decrypt(key, nonce, aad, aadLen, ciphertext, cipherLen, mic, fRxPlain)) {
        fRxDropped++;
        if (isMulticast) fBcMicFail++; else fUcMicFail++;
        return;
    }
    if (isMulticast) fRxGtkPN[keyId][tid] = pn; else fRxPtkPN[tid] = pn;
    fLastBSSActivity = fLinkTicks;
    if (rssi > -100) fLinkRSSI = rssi;
    if ((subtype & 8) && (frame[24] & 0x80)) {
        if (isMulticast || !rtl_wifi::validateAMSDU(fRxPlain, cipherLen, fMyMAC)) { fRxDropped++; return; }
        size_t offset = 0;
        while (offset < cipherLen) {
            const uint8_t *sub = fRxPlain + offset;
            size_t amount = (size_t(sub[12]) << 8) | sub[13];
            deliverMSDU(sub + 14, amount, sub, sub + 6, false);
            offset += 14 + amount;
            if (offset < cipherLen) offset = (offset + 3) & ~size_t(3);
        }
    } else deliverMSDU(fRxPlain, cipherLen, frame + 4, frame + 16, isMulticast);
}

void RTL8723BE::deliverMSDU(const uint8_t *data, size_t len, const uint8_t *da, const uint8_t *sa, bool multicast) {
    if (!len || len > 1508 || (sa[0] & 1)) { fRxDropped++; return; }
    // OpenBSD ieee80211_decap(): remove RFC1042 SNAP, otherwise preserve 802.3 LLC.
    uint16_t ethertype;
    if (rtl_wifi::snapHeader(data, len)) {
        ethertype = uint16_t((data[6] << 8) | data[7]);
        data += 8; len -= 8;
        if (ethertype == 0x888e) {
            if (!multicast && memcmp(sa, fConnectedBSSID, 6) == 0) handleEAPOLKey(data, len);
            return;
        }
    } else {
        if (len > 1500) { fRxDropped++; return; }
        ethertype = uint16_t(len);
    }
    if (fState != kRTLStateConnected) return;
    size_t ethLen = 14 + len;
    mbuf_t m = allocatePacket((UInt32)ethLen);
    if (m && fNetif) {
        uint8_t *dst = (uint8_t*)mbuf_data(m);
        memcpy(dst, da, 6);
        memcpy(dst + 6, sa, 6);
        dst[12] = (uint8_t)(ethertype >> 8);
        dst[13] = (uint8_t)(ethertype & 0xFF);
        memcpy(dst + 14, data, len);
        fNetif->inputPacket(m, (UInt32)ethLen);
        if (fNetStats) fNetStats->inputPackets++;
        if (multicast) fBcDelivered++; else fUcDelivered++;
        fRxBytes += ethLen;
    } else if (m) {
        freePacket(m);
    }
}

// ============================================================================
// Open System authentication, association and WPA2-PSK 4-way handshake
// ============================================================================

// RSN IE: WPA2, CCMP group + pairwise, PSK AKM.
size_t RTL8723BE::buildRsnIE(uint8_t *out) {
    static const uint8_t rsn[] = {
        0x30, 0x14, 0x01, 0x00,
        0x00, 0x0F, 0xAC, 0x04,
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,
        0x00, 0x00
    };
    memcpy(out, rsn, sizeof(rsn));
    return sizeof(rsn);
}

IOReturn RTL8723BE::connect(const RTL8723BEConnectParams *params) {
    if (!params) return kIOReturnBadArgument;
    if (!fCommandGate || !fHwReady) return kIOReturnNotReady;

    char ssid[RTL8723BE_MAX_SSID_LEN + 1];
    char pass[RTL8723BE_MAX_PASSPHRASE_LEN + 1];
    if (!memchr(params->ssid, 0, sizeof(params->ssid)) ||
        !memchr(params->passphrase, 0, sizeof(params->passphrase))) return kIOReturnBadArgument;
    strlcpy(ssid, params->ssid, sizeof(ssid));
    strlcpy(pass, params->passphrase, sizeof(pass));
    size_t passLen = strlen(pass);
    if (ssid[0] == '\0' || passLen < 8 || passLen > 64) {
        rtl_wifi::erase(pass, sizeof(pass));
        return kIOReturnBadArgument;
    }

    // PBKDF2 (4096 iterations) outside the work-loop gate.
    uint8_t pmk[32];
    if (!rtl_wifi::derivePMK(pass, passLen, (const uint8_t*)ssid, strlen(ssid), pmk)) {
        rtl_wifi::erase(pass, sizeof(pass));
        return kIOReturnBadArgument;
    }

    struct Args { const RTL8723BEConnectParams *p; const char *ssid; const char *pass; uint8_t *pmk; } a =
        { params, ssid, pass, pmk };
    IOReturn ret = fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpConnect, &a);
    rtl_wifi::erase(pmk, sizeof(pmk));
    rtl_wifi::erase(pass, sizeof(pass));
    return ret;
}

IOReturn RTL8723BE::beginConnect() {
    // Tune to the AP's channel from the last scan.
    for (uint32_t i = 0; i < fScanResults.count; ++i) {
        if (memcmp(fScanResults.aps[i].bssid, fConnectedBSSID, 6) == 0) {
            if (!setChannel(fScanResults.aps[i].channel)) return kIOReturnBadArgument;
            fPeer = fScanPeers[i];
            fPeer.capabilities = fScanResults.aps[i].capabilities;
            if (!(fOptions & kRTLOptionHT)) fPeer.ht = false;
            break;
        }
    }
    for (int i = 0; i < 6; ++i) mmio_write8(REG_BSSID + i, fConnectedBSSID[i]);

    fWPA.begin(fPMK, fMyMAC, fConnectedBSSID, fPeer.rsn, fPeer.rsnLength);
    fState = kRTLStateAuthenticating;
    fHandshakeDeadline = fLinkTicks + 5;
    fKeyInstalled = false;

    IOLog("RTL8723BE: Connecting to %s (BSSID %02x:%02x:%02x:%02x:%02x:%02x) on channel %u\n",
          fConnectedSSID, fConnectedBSSID[0], fConnectedBSSID[1], fConnectedBSSID[2],
          fConnectedBSSID[3], fConnectedBSSID[4], fConnectedBSSID[5], fCurrentChannel);

    uint8_t *f = fTxFrame;
    memset(f, 0, 30);
    f[0] = 0xB0;
    memcpy(f + 4, fConnectedBSSID, 6);
    memcpy(f + 10, fMyMAC, 6);
    memcpy(f + 16, fConnectedBSSID, 6);
    f[24] = 0x00; f[25] = 0x00; // Open System
    f[26] = 0x01; f[27] = 0x00; // transaction 1
    return transmitRawFrame(Q_MGNT, f, 30, DESC_RATE1M) ? kIOReturnSuccess : kIOReturnNoResources;
}

IOReturn RTL8723BE::disconnect() {
    if (!fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpDisconnect);
}

void RTL8723BE::handleAuthResponse(const uint8_t *frame, size_t len) {
    if (len < 30 || fState != kRTLStateAuthenticating) return;
    uint16_t seq = (uint16_t)(frame[26] | (frame[27] << 8));
    uint16_t status = (uint16_t)(frame[28] | (frame[29] << 8));
    if (seq != 2) return;
    if (status != 0) {
        IOLog("RTL8723BE: Authentication rejected (status %u)\n", status);
        deauthTeardown();
        return;
    }
    fState = kRTLStateAssociating;
    fHandshakeDeadline = fLinkTicks + 5;
    IOLog("RTL8723BE: Authenticated. Sending Association Request...\n");

    static const uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
    static const uint8_t extRates[] = {0x32, 0x04, 0x30, 0x48, 0x60, 0x6C};
    uint8_t *f = fTxFrame;
    size_t pos = 0;
    f[pos++] = 0x00; f[pos++] = 0x00; f[pos++] = 0x00; f[pos++] = 0x00;
    memcpy(f + pos, fConnectedBSSID, 6); pos += 6;
    memcpy(f + pos, fMyMAC, 6); pos += 6;
    memcpy(f + pos, fConnectedBSSID, 6); pos += 6;
    f[pos++] = 0x00; f[pos++] = 0x00;
    uint16_t capabilities = 0x0011 | (fPeer.capabilities & 0x0420);
    f[pos++] = uint8_t(capabilities); f[pos++] = uint8_t(capabilities >> 8);
    f[pos++] = 0x0A; f[pos++] = 0x00;           // Listen interval
    size_t ssidLen = strlen(fConnectedSSID);
    f[pos++] = 0x00; f[pos++] = (uint8_t)ssidLen;
    memcpy(f + pos, fConnectedSSID, ssidLen); pos += ssidLen;
    memcpy(f + pos, rates, sizeof(rates)); pos += sizeof(rates);
    memcpy(f + pos, extRates, sizeof(extRates)); pos += sizeof(extRates);
    pos += buildRsnIE(f + pos);
    pos += rtl_wifi::associationIEs(fPeer, f + pos, sizeof(fTxFrame) - pos);
    transmitRawFrame(Q_MGNT, f, pos, DESC_RATE1M);
}

void RTL8723BE::handleAssocResponse(const uint8_t *frame, size_t len) {
    if (len < 30 || fState != kRTLStateAssociating) return;
    uint16_t status = (uint16_t)(frame[26] | (frame[27] << 8));
    if (status != 0) {
        IOLog("RTL8723BE: Association rejected (status %u)\n", status);
        deauthTeardown();
        return;
    }
    fAid = (uint16_t)((frame[28] | (frame[29] << 8)) & 0x3FFF);
    rtl_wifi::Peer response;
    if (!rtl_wifi::parseIEs(frame + 30, len - 30, response) || !fAid || fAid > 2007 ||
        !(fPeer.rates & response.rates)) {
        deauthTeardown(); return;
    }
    // The association response, rather than the beacon alone, confirms HT and WMM.
    if (response.rates) fPeer.rates &= response.rates;
    fPeer.ht = fPeer.ht && response.ht;
    fPeer.wmm = fPeer.wmm && response.wmm;
    fPeer.shortGI = fPeer.shortGI && response.shortGI;
    fPeer.mcs &= response.mcs;
    fPeer.capabilities &= uint16_t(frame[24] | (frame[25] << 8));
    if (!fPeer.mcs) fPeer.ht = false;
    if (response.wmm) memcpy(fPeer.edca, response.edca, sizeof(fPeer.edca));
    configurePeer();
    fState = kRTLStateHandshake;
    fHandshakeDeadline = fLinkTicks + 15;
    mmio_write8(REG_MSR, (uint8_t)((mmio_read8(REG_MSR) & 0xFC) | 0x02)); // station, linked
    IOLog("RTL8723BE: Associated (AID %u). Awaiting WPA2 4-Way Handshake...\n", fAid);
}

// Linux rtlwifi/cam.c: pairwise entry 4, AES cipher 4, valid written last.
// RX decryption stays disabled: software authenticates MICs and enforces per-TID replay.
bool RTL8723BE::installTxKey() {
    uint32_t words[8] = {};
    rtlCCMPPairwiseCAM(fConnectedBSSID, fPTK + 32, words);
    bool ok = true;
    for (int i = 7; i >= 0; --i) {
        mmio_write32(REG_CAMWRITE, words[i]);
        mmio_write32(REG_CAMCMD, 0x80010000U | uint32_t(4 * 8 + i));
        unsigned polls = 0;
        while (mmio_read32(REG_CAMCMD) & (1U << 31)) {
            if (++polls == 100) { ok = false; break; }
            IODelay(10);
        }
        if (!ok) break;
    }
    rtl_wifi::erase(words, sizeof(words));
    if (ok) mmio_write8(REG_SECCFG, 1U << 2); // SCR_TXENCENABLE only
    return ok;
}

IOReturn RTL8723BE::getLinkInfo(RTL8723BELinkInfo *out) {
    if (!out || !fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpLinkInfo, out);
}
IOReturn RTL8723BE::setOptions(uint32_t options) {
    if (!fCommandGate) return kIOReturnNotReady;
    return fCommandGate->runAction(&RTL8723BE::gatedCall, (void *)(uintptr_t)kOpOptions, (void *)(uintptr_t)options);
}

// Linux rtl8723be_update_hal_rate_mask(): one-stream HT20, or negotiated b/g.
void RTL8723BE::configurePeer() {
    setBandwidth20();
    fDataRate = rtl_wifi::highestRate(fPeer);
    fRateID = fPeer.ht ? 1 : ((fPeer.rates & 0xff0) ? 6 : 8);
    uint32_t bitmap = fPeer.rates | (fPeer.ht ? uint32_t(fPeer.mcs) << 12 : 0);
    uint8_t mask[7] = {0, uint8_t(fRateID | (fPeer.shortGI && fPeer.ht ? 0x80 : 0)), 0,
                      uint8_t(bitmap), uint8_t(bitmap >> 8), uint8_t(bitmap >> 16), uint8_t(bitmap >> 24)};
    fFirmwareRA = sendH2C(0x40, mask, sizeof(mask));
    const uint8_t media[3] = {1,0,0};
    sendH2C(0x01, media, sizeof(media)); // MEDIA_STATUS_RPT, MACID 0 connected
    mmio_write32(REG_RRSR, (mmio_read32(REG_RRSR) & ~0xfffU) | fPeer.basicRates);
    // Linux rtl8723be_update_channel_access_setting()/HW_VAR_ACK_PREAMBLE.
    const uint8_t slot = fPeer.capabilities & 0x0400 ? 9 : 20;
    const uint8_t sifs = fPeer.ht ? 14 : 10;
    mmio_write8(0x051b, slot);
    mmio_write8(0x0515, sifs); mmio_write8(0x0517, sifs);
    mmio_write8(0x0429, sifs); mmio_write8(0x063b, sifs);
    mmio_write16(0x063e, 0x0e0e);
    const uint8_t preamble = mmio_read8(REG_TRXPTCL_CTL + 2);
    mmio_write8(REG_TRXPTCL_CTL + 2, (preamble & ~2U) | (fPeer.capabilities & 0x0020 ? 2 : 0));
    // Defaults from Linux; overwrite only complete, validated WMM parameter sets.
    static const uint16_t regs[] = {0x508,0x50c,0x504,0x500};
    static const uint32_t defaults[] = {0x005ea42b,0x0000a44f,0x005e4322,0x002f3222};
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t value = fPeer.edca[i] ? fPeer.edca[i] : defaults[i];
        if (fPeer.edca[i]) value = (value & ~255U) | (10 + ((value & 255U) - 10) / 9 * slot);
        mmio_write32(regs[i], value);
    }
    IOLog("RTL8723BE: peer %s rates=0x%03x MCS=0x%02x SGI=%u WMM=%u\n",
          fPeer.ht ? "HT20" : "b/g", fPeer.rates, fPeer.mcs, fPeer.shortGI, fPeer.wmm);
}

void RTL8723BE::handleEAPOLFrame(const uint8_t *frame, size_t len, size_t hdrLen) {
    const size_t offset = hdrLen + 8;
    if (len >= offset) handleEAPOLKey(frame + offset, len - offset);
}

void RTL8723BE::handleEAPOLKey(const uint8_t *e, size_t len) {
    if (fState != kRTLStateHandshake && fState != kRTLStateConnected) return;
    // EAPOL can carry Ethernet padding, which is outside its authenticated length.
    if (len < 4) return;
    const size_t packetLen = 4 + size_t((e[2] << 8) | e[3]);
    if (packetLen > len) return;
    uint8_t randomNonce[32];
    read_random(randomNonce, sizeof(randomNonce));
    uint8_t *reply = fTxPlain + 8;
    rtl_wifi::Supplicant::Result result;
    bool valid = fWPA.receive(e, packetLen, randomNonce, reply, sizeof(fTxPlain) - 8, result);
    rtl_wifi::erase(randomNonce, sizeof(randomNonce));
    if (!valid) return;
    if (result.pairwiseChanged) {
        memcpy(fPTK, fWPA.ptk(), sizeof(fPTK));
        fTxPN = 1;
        memset(fRxPtkPN, 0, sizeof(fRxPtkPN));
        fKeyInstalled = true;
        fTxCrypto = false;
        mmio_write8(REG_SECCFG, 0);
        if (fOptions & kRTLOptionTxCrypto) {
            fTxCrypto = installTxKey();
            if (!fTxCrypto) IOLog("RTL8723BE: CAM programming failed; using software CCMP\n");
        }
    }
    for (unsigned i = 0; i < 4; ++i) if (result.groupChanged & (1U << i))
        for (unsigned tid = 0; tid < 16; ++tid) fRxGtkPN[i][tid] = fWPA.gtkRSC(i);
    memset(fTxFrame, 0, 32);
    fTxFrame[0] = 0x08; fTxFrame[1] = 0x01;
    memcpy(fTxFrame + 4, fConnectedBSSID, 6);
    memcpy(fTxFrame + 10, fMyMAC, 6);
    memcpy(fTxFrame + 16, fConnectedBSSID, 6);
    static const uint8_t llc[] = {0xAA,0xAA,0x03,0,0,0,0x88,0x8E};
    size_t txLength = 32 + result.replyLength;
    if ((e[6] & 8) == 0 && fKeyInstalled) {
        // Group-key message 2 uses the installed pairwise protection (OpenBSD PAE output).
        if (fTxPN >= (1ULL << 48)) { deauthTeardown(); return; }
        uint64_t pn = fTxPN++;
        fTxFrame[1] = 0x41;
        uint8_t *iv = fTxFrame + 24;
        iv[0] = uint8_t(pn); iv[1] = uint8_t(pn >> 8); iv[2] = 0; iv[3] = 0x20;
        for (unsigned i = 0; i < 4; ++i) iv[4 + i] = uint8_t(pn >> (16 + i * 8));
        memcpy(fTxPlain, llc, sizeof(llc));
        uint8_t nonce[13], aad[30], mic[8];
        rtl_crypto::ccmp_build_nonce(fTxFrame, 24, pn, nonce);
        size_t aadLength = rtl_crypto::ccmp_build_aad(fTxFrame, 24, aad);
        if (!rtl_crypto::ccmp_encrypt(fPTK + 32, nonce, aad, aadLength, fTxPlain,
                                     8 + result.replyLength, fTxFrame + 32, mic)) return;
        txLength = 40 + result.replyLength + 8;
        memcpy(fTxFrame + txLength - 8, mic, sizeof(mic));
    } else {
        memcpy(fTxFrame + 24, llc, sizeof(llc));
        memcpy(fTxFrame + 32, reply, result.replyLength);
    }
    if (!transmitRawFrame(Q_MGNT, fTxFrame, txLength, DESC_RATE1M)) return;
    if (fWPA.connected() && fState != kRTLStateConnected) {
        fState = kRTLStateConnected;
        fLastBSSActivity = fLinkTicks;
        setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid, getSelectedMedium());
        refreshLED(true);
        IOLog("RTL8723BE: WPA2 connected to %s (%s, firmware RA %s)\n", fConnectedSSID,
              fPeer.ht ? "HT20" : "legacy", fFirmwareRA ? "on" : "off");
    }
}

void RTL8723BE::handleDeauthFrame(const uint8_t *frame, size_t len) {
    uint16_t reason = len >= 26 ? (uint16_t)(frame[24] | (frame[25] << 8)) : 0;
    IOLog("RTL8723BE: Received %s from AP (reason %u). Tearing down link.\n",
          ((frame[0] >> 4) & 0xF) == 12 ? "Deauthentication" : "Disassociation", reason);
    deauthTeardown();
}

void RTL8723BE::handleActionFrame(const uint8_t *frame, size_t len) {
    if (fState != kRTLStateConnected || !fPeer.ht || len < 33 || frame[24] != 3 || frame[25] != 0) return;
    // OpenBSD ieee80211_get_addba_resp(), refusal branch. Never accept an agreement
    // before a replay-safe reorder queue and hardware BA context exist.
    uint16_t params = uint16_t(frame[27] | (frame[28] << 8));
    uint16_t tidOnly = params & 0x003c;
    uint8_t token = frame[26];
    uint8_t *reply = fTxFrame;
    memset(reply, 0, 33);
    reply[0] = 0xd0;
    memcpy(reply + 4, fConnectedBSSID, 6);
    memcpy(reply + 10, fMyMAC, 6);
    memcpy(reply + 16, fConnectedBSSID, 6);
    reply[24] = 3; reply[25] = 1; reply[26] = token;
    reply[27] = 37; // IEEE80211_STATUS_REFUSED
    reply[29] = uint8_t(tidOnly); reply[30] = uint8_t(tidOnly >> 8);
    transmitRawFrame(Q_MGNT, reply, 33, DESC_RATE1M);
}

void RTL8723BE::deauthTeardown() {
    if (fState == kRTLStateConnected || fState == kRTLStateHandshake) {
        uint8_t *f = fTxFrame;
        memset(f, 0, 26);
        f[0] = 0xC0;                           // Deauthentication
        memcpy(f + 4, fConnectedBSSID, 6);
        memcpy(f + 10, fMyMAC, 6);
        memcpy(f + 16, fConnectedBSSID, 6);
        f[24] = 0x03;                          // reason: leaving
        transmitRawFrame(Q_MGNT, f, 26, DESC_RATE1M);
    }

    setLinkStatus(kIONetworkLinkValid, nullptr);
    mmio_write8(REG_MSR, (uint8_t)(mmio_read8(REG_MSR) & 0xFC));
    for (int i = 0; i < 6; ++i) mmio_write8(REG_BSSID + i, 0);

    fTxCrypto = false;
    mmio_write8(REG_SECCFG, 0);
    mmio_write32(REG_CAMWRITE, 0);
    mmio_write32(REG_CAMCMD, 0x80010020U); // invalidate pairwise CAM entry 4
    const uint8_t media[3] = {0,0,0};
    sendH2C(0x01, media, sizeof(media));
    rtl_wifi::erase(fPTK, sizeof(fPTK));
    fWPA.reset();
    memset(&fPeer, 0, sizeof(fPeer));
    fDataRate = DESC_RATE1M;
    fRateID = 8;
    fFirmwareRA = false;
    rtl_wifi::erase(fPMK, sizeof(fPMK));
    fKeyInstalled = false;
    fTxPN = 1;
    for (int i = 0; i < 16; ++i) fRxPtkPN[i] = 0;
    memset(fRxGtkPN, 0, sizeof(fRxGtkPN));
    fLinkRSSI = -100;
    fState = kRTLStateDisconnected;
}

void RTL8723BE::sendNullDataFrame(bool powerManagement) {
    uint8_t *f = fTxFrame;
    memset(f, 0, 24);
    f[0] = 0x48;                                   // Null function (no QoS)
    f[1] = powerManagement ? 0x11 : 0x01;          // To-DS (| PM)
    memcpy(f + 4, fConnectedBSSID, 6);
    memcpy(f + 10, fMyMAC, 6);
    memcpy(f + 16, fConnectedBSSID, 6);
    transmitRawFrame(Q_MGNT, f, 24, DESC_RATE1M);
}

// ============================================================================
// Outbound Packet Processing (outputPacket)
// ============================================================================

UInt32 RTL8723BE::outputPacket(mbuf_t m, void *param) {
    (void)param;
    if (!m) return kIOReturnOutputSuccess;

    if (!fHwReady || fState != kRTLStateConnected || !fKeyInstalled) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }

    size_t pktLen = mbuf_pkthdr_len(m);
    if (pktLen < 14 || pktLen > sizeof(fOutEth)) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (fTxRingDescVirt[Q_BE][fTxHostIdx[Q_BE]].get_own()) return kIOReturnOutputStall;
    if (mbuf_copydata(m, 0, pktLen, fOutEth) != 0) { freePacket(m); return kIOReturnOutputDropped; }

    uint16_t etherType = (uint16_t)((fOutEth[12] << 8) | fOutEth[13]);
    if (etherType == 0x888E) {
        freePacket(m);
        return kIOReturnOutputDropped; // user space must not inject EAPOL
    }
    const uint8_t *da = fOutEth;
    const uint8_t *payload = fOutEth + 14;
    size_t payloadLen = pktLen - 14;

    uint8_t *frame = fOutFrame;
    frame[0] = fPeer.wmm ? 0x88 : 0x08;
    frame[1] = 0x41;                 // To-DS | Protected
    frame[2] = 0x00; frame[3] = 0x00;
    memcpy(frame + 4, fConnectedBSSID, 6);
    memcpy(frame + 10, fMyMAC, 6);
    memcpy(frame + 16, da, 6);
    frame[22] = 0x00; frame[23] = 0x00;
    frame[24] = 0x00; frame[25] = 0x00;
    const size_t headerLength = fPeer.wmm ? 26 : 24;
    size_t pos = headerLength;

    if (fTxPN >= (1ULL << 48)) { deauthTeardown(); freePacket(m); return kIOReturnOutputDropped; }
    uint64_t pn = fTxPN++;
    frame[pos + 0] = (uint8_t)(pn & 0xFF);
    frame[pos + 1] = (uint8_t)((pn >> 8) & 0xFF);
    frame[pos + 2] = 0x00;
    frame[pos + 3] = 0x20;           // ExtIV, key id 0
    frame[pos + 4] = (uint8_t)((pn >> 16) & 0xFF);
    frame[pos + 5] = (uint8_t)((pn >> 24) & 0xFF);
    frame[pos + 6] = (uint8_t)((pn >> 32) & 0xFF);
    frame[pos + 7] = (uint8_t)((pn >> 40) & 0xFF);
    pos += 8;

    uint8_t nonce[13];
    uint8_t aad[30];
    rtl_crypto::ccmp_build_nonce(frame, headerLength, pn, nonce);
    size_t aadLen = rtl_crypto::ccmp_build_aad(frame, headerLength, aad);

    size_t plainLen = 8 + payloadLen;
    if (plainLen > sizeof(fTxPlain) || pos + plainLen + 8 > sizeof(fOutFrame)) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    fTxPlain[0] = 0xAA; fTxPlain[1] = 0xAA; fTxPlain[2] = 0x03;
    fTxPlain[3] = 0x00; fTxPlain[4] = 0x00; fTxPlain[5] = 0x00;
    fTxPlain[6] = (uint8_t)(etherType >> 8);
    fTxPlain[7] = (uint8_t)(etherType & 0xFF);
    memcpy(fTxPlain + 8, payload, payloadLen);

    if (fTxCrypto) {
        // mac80211/wpa.c: hardware receives the software IV and plaintext, without MIC.
        memcpy(frame + pos, fTxPlain, plainLen);
        pos += plainLen;
    } else {
        uint8_t mic[8];
        if (!rtl_crypto::ccmp_encrypt(fPTK + 32, nonce, aad, aadLen, fTxPlain, plainLen, frame + pos, mic)) {
            freePacket(m); return kIOReturnOutputDropped;
        }
        pos += plainLen;
        memcpy(frame + pos, mic, 8); pos += 8;
    }

    // The MAC appends the FCS itself.
    if (!transmitRawFrame(Q_BE, frame, pos, fDataRate)) {
        fTxErrors++;
        if (fNetStats) fNetStats->outputErrors++;
        return kIOReturnOutputStall;
    }
    freePacket(m);
    if (fNetStats) fNetStats->outputPackets++;
    fTxBytes += pktLen;
    return kIOReturnOutputSuccess;
}
