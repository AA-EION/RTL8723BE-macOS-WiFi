#ifndef _RTL8723BE_HPP_
#define _RTL8723BE_HPP_

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#include "RTL8723BE_hw.hpp"
#include "RTL8723BE_ipc.h"
#include "RTL8723BE_crypto.hpp"
#include "RTL8723BE_tables.hpp"
#include "RTL8723BE_firmware.hpp"

#define RTL8723BE_TX_DESC_COUNT  64
#define RTL8723BE_RX_DESC_COUNT  64
#define RTL8723BE_RX_BUF_SIZE    2048
#define RTL8723BE_TX_BUF_SIZE    4096

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

    // UserClient Control APIs
    IOReturn startScan();
    IOReturn getScanResults(RTL8723BEScanResults *outResults);
    IOReturn connect(const RTL8723BEConnectParams *params);
    IOReturn disconnect();
    IOReturn getStatus(RTL8723BEStatus *outStatus);
    IOReturn setAntennaPath(uint8_t ant);

    // Hardware Register Accessors
    uint8_t  mmio_read8(uint32_t offset);
    uint16_t mmio_read16(uint32_t offset);
    uint32_t mmio_read32(uint32_t offset);
    void     mmio_write8(uint32_t offset, uint8_t val);
    void     mmio_write16(uint32_t offset, uint16_t val);
    void     mmio_write32(uint32_t offset, uint32_t val);

    // RF Register Accessors (3-wire LSSI serial interface)
    uint32_t readRFRegister(uint8_t offset);
    void     writeRFRegister(uint8_t offset, uint32_t data);

    // Interrupt Callbacks
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    static void scanTimerAction(OSObject *owner, IOTimerEventSource *timer);

private:
    // Hardware State Machine & Initialization
    bool powerOn();
    bool powerOff();
    bool initLLT();
    bool readEfuse(CalibData &outCalib);
    bool decodePGStream(const uint8_t *pgStream, size_t len, CalibData &outCalib);
    bool downloadFirmware(const uint8_t *fwBuf, size_t fwLen);
    bool initBasebandAndRF();
    bool setChannel(uint8_t channel, uint8_t bw = 0);
    bool initDMARings();
    void freeDMARings();

    // DMA Ring Operations
    bool transmitRawFrame(RTLQueueId qId, const uint8_t *frame, size_t len);
    void handleRxInterrupt();
    void handleTxInterrupt();

    // 802.11 Protocol & Frame Handlers
    void processRxFrame(const uint8_t *frame, size_t len);
    bool parseBeaconOrProbe(const uint8_t *frame, size_t len, RTL8723BEDiscoveredAP &outAP);
    void handleAuthResponse(const uint8_t *frame, size_t len);
    void handleAssocResponse(const uint8_t *frame, size_t len);
    void handleEAPOLFrame(const uint8_t *frame, size_t len);
    void handleDeauthFrame(const uint8_t *frame, size_t len);

    // Challenger Fixes Helpers
    void deauthTeardown();
    void sendNullDataFrame(bool powerManagement);

    // IOKit Objects
    IOPCIDevice                   *fPCIDevice;
    IOMemoryMap                   *fMMIOMap;
    volatile uint8_t              *fMMIOBase;
    IOWorkLoop                    *fWorkLoop;
    IOFilterInterruptEventSource  *fInterruptSource;
    IOTimerEventSource            *fScanTimer;
    IOEthernetInterface           *fNetif;
    OSDictionary                  *fMediumDict;
    IOLock                        *fRFLock;
    IOLock                        *fStateLock;

    // Hardware State
    RTLPowerState                  fPowerState;
    CalibData                      fCalib;
    uint8_t                        fCurrentChannel;
    uint8_t                        fActiveAntenna; // 1 = Main, 2 = Aux

    // DMA Descriptors & Buffers
    IOBufferMemoryDescriptor      *fTxRingDescMem[8];
    TxDesc40                      *fTxRingDescVirt[8];
    uint64_t                       fTxRingDescPhys[8];
    IOBufferMemoryDescriptor      *fTxBufMem[8][RTL8723BE_TX_DESC_COUNT];
    uint16_t                       fTxHostIdx[8];

    IOBufferMemoryDescriptor      *fRxRingDescMem;
    RxDesc32                      *fRxRingDescVirt;
    uint64_t                       fRxRingDescPhys;
    IOBufferMemoryDescriptor      *fRxBufMem[RTL8723BE_RX_DESC_COUNT];
    uint16_t                       fRxHostIdx;

    // 802.11 Protocol State
    RTL8723BEWifiState             fState;
    uint8_t                        fMyMAC[6];
    uint8_t                        fConnectedBSSID[6];
    char                           fConnectedSSID[RTL8723BE_MAX_SSID_LEN + 1];
    char                           fPassphrase[RTL8723BE_MAX_PASSPHRASE_LEN + 1];

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
    uint8_t                        fScanCurrentChannel;
    uint8_t                        fHomeChannel;

    // Statistics
    uint64_t                       fTxPackets;
    uint64_t                       fRxPackets;
    uint64_t                       fTxErrors;
    uint64_t                       fRxErrors;
};

#endif // _RTL8723BE_HPP_
