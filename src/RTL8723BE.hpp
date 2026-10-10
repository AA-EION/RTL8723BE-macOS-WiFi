#ifndef _RTL8723BE_HPP_
#define _RTL8723BE_HPP_

#include <IOKit/IOLib.h>
#include <sys/kpi_mbuf.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IONetworkStats.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

#include "RTL8723BE_hw.hpp"
#include "RTL8723BE_ipc.h"
#include "RTL8723BE_crypto.hpp"
#include "RTL8723BE_tables.hpp"
#include "RTL8723BE_firmware.hpp"

#define RTL8723BE_TX_DESC_COUNT  16
#define RTL8723BE_RX_DESC_COUNT  32
#define RTL8723BE_RX_BUF_SIZE    4096
#define RTL8723BE_TX_BUF_SIZE    4096

// A beacon interval is 102.4 ms; dwell long enough to catch one per channel.
#define RTL8723BE_SCAN_DWELL_MS  120
#define RTL8723BE_SCAN_TIMEOUT_MS 5000

// A physically contiguous DMA buffer mapped for the device through IODMACommand,
// so the address handed to the NIC is correct with or without an IOMMU (VT-d).
struct RTLDmaBuf {
    IOBufferMemoryDescriptor *mem;
    IODMACommand             *cmd;
    void                     *virt;
    uint64_t                  ioaddr;
    size_t                    size;
};

// One entry of a Linux-style power sequence table (pwrseq.h), PCIe rows only.
struct RTLPwrCmd {
    uint16_t offset;
    uint8_t  cmd;   // 1 = write, 2 = poll, 3 = delay (value: 0 = us, 1 = ms)
    uint8_t  mask;
    uint8_t  value;
};

class RTL8723BE : public IOEthernetController {
    OSDeclareDefaultStructors(RTL8723BE);

    typedef IOEthernetController super;

public:
    virtual bool init(OSDictionary *properties = nullptr) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;

    // IOEthernetController Interface
    virtual IOReturn enable(IONetworkInterface *netif) override;
    virtual IOReturn disable(IONetworkInterface *netif) override;
    virtual UInt32 outputPacket(mbuf_t m, void *param) override;
    virtual IOReturn getHardwareAddress(IOEthernetAddress *addrP) override;
    virtual IOReturn setPromiscuousMode(bool active) override;
    virtual IOReturn setMulticastMode(bool active) override;
    virtual IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;
    virtual IOOutputQueue *createOutputQueue() override;
    virtual bool configureInterface(IONetworkInterface *netif) override;

    // UserClient Control APIs (all serialized through fCommandGate)
    IOReturn startScan();
    IOReturn getScanResults(RTL8723BEScanResults *outResults);
    IOReturn connect(const RTL8723BEConnectParams *params);
    IOReturn disconnect();
    IOReturn getStatus(RTL8723BEStatus *outStatus);
    IOReturn setAntennaPath(uint8_t ant);
    IOReturn debugReadReg(uint32_t addr, uint32_t width, uint32_t *outValue);
    IOReturn debugWriteReg(uint32_t addr, uint32_t width, uint32_t value);
    IOReturn debugReadRF(uint32_t offset, uint32_t *outValue);
    IOReturn debugWriteRF(uint32_t offset, uint32_t value);
    IOReturn getDiag(RTL8723BEDiag *outDiag);
    IOReturn runIQK(uint32_t *outMask);

    // Hardware Register Accessors
    uint8_t  mmio_read8(uint32_t offset);
    uint16_t mmio_read16(uint32_t offset);
    uint32_t mmio_read32(uint32_t offset);
    void     mmio_write8(uint32_t offset, uint8_t val);
    void     mmio_write16(uint32_t offset, uint16_t val);
    void     mmio_write32(uint32_t offset, uint32_t val);

    // Masked 32-bit access (Linux rtl_get_bbreg / rtl_set_bbreg semantics)
    uint32_t getBBReg(uint32_t addr, uint32_t mask);
    void     setBBReg(uint32_t addr, uint32_t mask, uint32_t data);

    // RF register access (path A, 3-wire LSSI write / HSSI read)
    uint32_t getRFReg(uint8_t offset, uint32_t mask = 0xFFFFF);
    void     setRFReg(uint8_t offset, uint32_t mask, uint32_t data);

    // Interrupt Callbacks
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    static void scanTimerAction(OSObject *owner, IOTimerEventSource *timer);

private:
    // Gated entry points (run on the work loop)
    static IOReturn gatedScan(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn gatedCall(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    IOReturn quiesce(void *, void *, void *, void *);

    // Hardware initialization (ported from Linux rtl8723be_hw_init)
    bool hwInit();
    void hwDisable();
    bool runPowerSequence(const RTLPwrCmd *seq, size_t count);
    bool powerOn();
    void powerOff();
    bool initMac();
    bool initLLT();
    bool readEfuse(CalibData &outCalib);
    uint8_t readEfuseByte(uint16_t addr);
    bool downloadFirmware(const uint8_t *fwBuf, size_t fwLen);
    void firmwareSelfReset();
    bool configTable(const uint32_t *table, size_t len, int kind);
    bool checkCondition(uint32_t cond1, uint32_t cond2);
    bool phyMacConfig();
    bool phyBBConfig();
    bool phyRFConfig();
    void hwConfigure();
    void btcoexInit();
    void lcCalibrate();
    uint32_t iqCalibrate();
    void iqkRun(long result[][8], uint8_t t);
    uint8_t iqkTx(bool pathB);
    uint8_t iqkRx(bool pathB);
    void iqkOneShot(uint32_t agcRsp);
    void iqkTones(uint32_t txToneA, uint32_t rxToneA, uint32_t txPiA, uint32_t rxPiA);
    void iqkFill(long result[][8], uint8_t cand, bool pathB, bool txOnly);
    void applyCoexProfile();
    bool sendH2C(uint8_t id, const uint8_t *params, uint8_t len);
    void setBandwidth20();
    void setTxPowerIndex(uint8_t cckIndex, uint8_t ofdmIndex);
    bool setChannel(uint8_t channel);
    void refreshLED(bool on);
    bool allocDMA(RTLDmaBuf &buf, size_t size, size_t alignment);
    void freeDMA(RTLDmaBuf &buf);
    bool initDMARings();
    void resetRings();
    void freeDMARings();
    void cleanupResources();
    void enableInterrupts();
    void disableInterrupts();

    // DMA Ring Operations
    bool transmitRawFrame(RTLQueueId qId, const uint8_t *frame, size_t len, uint8_t rate);
    void handleRxInterrupt();
    void handleTxInterrupt();

    // Scan state machine
    void scanStep();
    void sendProbeRequest();
    void finishScan();

    // 802.11 Protocol & Frame Handlers
    void processRxFrame(const uint8_t *frame, size_t len, int8_t rssi);
    bool parseBeaconOrProbe(const uint8_t *frame, size_t len, RTL8723BEDiscoveredAP &outAP);
    int8_t rxSignalDbm(const RxDesc32 *desc, const uint8_t *phyStatus);
    void handleAuthResponse(const uint8_t *frame, size_t len);
    void handleAssocResponse(const uint8_t *frame, size_t len);
    void handleEAPOLFrame(const uint8_t *frame, size_t len, size_t hdrLen);
    IOReturn beginConnect();
    size_t buildRsnIE(uint8_t *out);
    void handleDeauthFrame(const uint8_t *frame, size_t len);

    // Challenger Fixes Helpers
    void deauthTeardown();
    void sendNullDataFrame(bool powerManagement);

    // IOKit Objects
    IOPCIDevice                   *fPCIDevice;
    IOMemoryMap                   *fMMIOMap;
    volatile uint8_t              *fMMIOBase;
    IOWorkLoop                    *fWorkLoop;
    IOCommandGate                 *fCommandGate;
    IOFilterInterruptEventSource  *fInterruptSource;
    IOTimerEventSource            *fScanTimer;
    IOEthernetInterface           *fNetif;
    OSDictionary                  *fMediumDict;
    IOLock                        *fRFLock;

    // Hardware State
    RTLPowerState                  fPowerState;
    volatile uint32_t              fHIMRMask;
    volatile bool                  fInterruptEnabled;
    bool                           fHwReady;
    bool                           fFwReady;
    CalibData                      fCalib;
    uint32_t                       fInitStage;
    uint32_t                       fRFChnlVal;   // cached RF 0x18 (Linux rfreg_chnlval[0])
    uint16_t                       fFwVersion;
    uint8_t                        fFwSubVersion;
    uint8_t                        fCutVersion;
    uint8_t                        fCurrentChannel;
    uint8_t                        fAntNum;        // 1 or 2
    uint8_t                        fActiveAntenna; // 1 = Main (S1), 2 = Aux (S0)
    bool                           fAntennaForced; // boot-arg or CLI choice
    bool                           fOemHP;

    // DMA Descriptors & Buffers
    RTLDmaBuf                      fTxRing[8];
    TxDescPci                      *fTxRingDescVirt[8];
    RTLDmaBuf                      fTxBuf[8][RTL8723BE_TX_DESC_COUNT];
    uint16_t                       fTxHostIdx[8];
    IOLock                        *fTxLock;

    RTLDmaBuf                      fRxRing;
    RxDesc32                      *fRxRingDescVirt;
    RTLDmaBuf                      fRxBuf[RTL8723BE_RX_DESC_COUNT];
    uint16_t                       fRxHostIdx;

    // 802.11 Protocol State
    RTL8723BEWifiState             fState;
    uint8_t                        fMyMAC[6];
    uint8_t                        fConnectedBSSID[6];
    char                           fConnectedSSID[RTL8723BE_MAX_SSID_LEN + 1];
    uint16_t                       fTxSeq;

    // Cryptographic Keys & Nonces
    uint8_t                        fPMK[32];
    uint8_t                        fPTK[64]; // KCK(16), KEK(16), TK(16), etc.
    uint8_t                        fGTK[16];
    uint8_t                        fSNonce[32];
    uint8_t                        fANonce[32];

    // Challenger Fixes State
    bool                           fKeyInstalled;        // KRACK mitigation
    uint64_t                       fLastReplayCounter;   // EAPOL replay counter
    uint64_t                       fTxPN;                // Transmit CCMP PN
    uint64_t                       fRxPtkPN[16];         // Per-TID Unicast PN
    uint64_t                       fRxGtkPN[4];          // Per-KeyID Multicast/Broadcast PN

    // Scan State & Cache
    RTL8723BEScanResults           fScanResults;
    bool                           fScanActive;
    bool                           fScanRetriedOtherAntenna;
    bool                           fAntennaCalibrated;   // both RF paths compared once
    uint32_t                       fPassMgmtStart;       // fRxMgmt at start of the current pass
    uint32_t                       fPass1Score;          // mgmt frames heard on the first path
    uint8_t                        fScanCurrentChannel;
    uint8_t                        fHomeChannel;

    // Scratch buffers (kept off the 16 KB kernel stack)
    uint8_t                        fTxFrame[2048];
    uint8_t                        fTxPlain[1600];
    uint8_t                        fRxPlain[2048];
    uint8_t                        fOutEth[1514];   // outputPacket() only
    uint8_t                        fOutFrame[2048]; // outputPacket() only
    uint16_t                       fAid;
    IONetworkStats                *fNetStats;

    // Statistics
    uint64_t                       fTxPackets;
    uint64_t                       fRxPackets;
    uint64_t                       fTxErrors;
    uint64_t                       fRxErrors;
    volatile uint32_t              fIntCount;
    uint32_t                       fRxDescDone;
    uint32_t                       fRxMgmt, fRxBeacons, fRxData, fRxCtrl, fRxDropped;
    uint32_t                       fTxRingFull;
    uint32_t                       fRxOk[3], fRxCrc[3];   // CCK / OFDM / HT
    uint32_t                       fUcDataRx, fUcDelivered, fUcReplay, fUcMicFail, fBcDelivered, fBcMicFail;
    uint32_t                       fLastUcFcPn, fLastUcRateLen;
    uint8_t                        fCurRxRate;
    uint32_t                       fIqkAdda[16], fIqkMac[4], fIqkBB[9];
    uint8_t                        fLastHmeBox;
};

#endif // _RTL8723BE_HPP_
