// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Issen Software Group
// Ported from Linux drivers/net/wireless/realtek/rtlwifi (rtl8723be, rtl8723com, efuse.c),
// Copyright (c) 2009-2014 Realtek Corporation (licensed GPL-2.0).

#ifndef _RTL8723BE_HW_HPP_
#define _RTL8723BE_HW_HPP_

#include <stdint.h>
#include <stddef.h>

// Realtek RTL8723BE Register Map (16KB BAR2 MMIO aperture).
// Addresses follow Linux drivers/net/wireless/realtek/rtlwifi/rtl8723be/reg.h.
#define REG_SYS_ISO_CTRL         0x0000 // 16-bit
#define REG_SYS_FUNC_EN          0x0002 // 16-bit
#define REG_APS_FSMCO            0x0004 // 16-bit
#define REG_PWR_STATUS           0x0006 // 8-bit / 16-bit
#define REG_SYS_CLKR             0x0008 // 16-bit
#define REG_9346CR               0x000A // 8-bit
#define REG_RSV_CTRL             0x001C // 16-bit
#define REG_RF_CTRL              0x001F // 8-bit
#define REG_AFE_XTAL_CTRL        0x0024 // 32-bit (crystal/AFE; NOT the crystal-cap register)
#define REG_MAC_PHY_CTRL         0x002C // 32-bit (crystal cap lives in bits [23:12])
#define REG_MULTI_FUNC_CTRL      0x0068 // 8-bit
#define REG_HWSEQ_CTRL           0x0423 // 8-bit
#define REG_EFUSE_CTRL           0x0030 // 32-bit
#define REG_EFUSE_TEST           0x0034 // 32-bit
#define REG_GPIO_MUXCFG          0x0040 // 16-bit
#define REG_LEDCFG0              0x004C
#define REG_LEDCFG1              0x004D
#define REG_LEDCFG2              0x004E
#define REG_HSIMR                0x0058 // 32-bit
#define REG_HSISR                0x005C // 32-bit
#define REG_MCUFWDL              0x0080 // 32-bit
#define REG_MCUFWDL_PAGE         0x0082 // 8-bit (MCU RAM Page Select)
#define REG_HIMR                 0x00B0 // 32-bit (Host Interrupt Mask)
#define REG_HISR                 0x00B4 // 32-bit (Host Interrupt Status)
#define REG_HIMRE                0x00B8 // 32-bit (Host Interrupt Mask Extension)
#define REG_HISRE                0x00BC // 32-bit (Host Interrupt Status Extension)
#define REG_EFUSE_ACCESS         0x00CF // 8-bit (0x69 enable, 0x00 disable)
#define REG_SYS_CFG              0x00F0 // 32-bit
#define REG_SYS_CFG1             0x00FC // 32-bit
#define REG_CR                   0x0100 // 16-bit (Command Register)
#define REG_MSR                  0x0102 // 8-bit (network type)
#define REG_PBP                  0x0104
#define REG_TRXDMA_CTRL          0x010C
#define REG_TRXFF_BNDY           0x0114
#define REG_FWIMR                0x0130
#define REG_MCUTST_1             0x01C0
#define REG_RQPN                 0x0200
#define REG_TDECTRL              0x0208
#define REG_RQPN_NPQ             0x0214
#define REG_RXDMA_CONTROL        0x0286
#define REG_RXPKT_NUM            0x0287
#define REG_FWHW_TXQ_CTRL        0x0420
#define REG_TXPKTBUF_BCNQ_BDNY   0x0424
#define REG_TXPKTBUF_MGQ_BDNY    0x0425
#define REG_RL                   0x042A
#define REG_DARFRC               0x0430
#define REG_RARFRC               0x0438
#define REG_RRSR                 0x0440
#define REG_ARFR0                0x0444
#define REG_ARFR1                0x044C
#define REG_AMPDU_MAX_TIME       0x0456
#define REG_FAST_EDCA_CTRL       0x0460
#define REG_HT_SINGLE_AMPDU      0x04C7
#define REG_MAX_AGGR_NUM         0x04CA
#define REG_TXPAUSE              0x0522
#define REG_TBTT_PROHIBIT        0x0540
#define REG_NAV_PROT_LEN         0x0546
#define REG_BCN_CTRL             0x0550
#define REG_SECONDARY_CCA_CTRL   0x0577
#define REG_BWOPMODE             0x0603
#define REG_RX_PKT_LIMIT         0x060C
#define REG_RX_DRVINFO_SZ        0x060F
#define REG_MACID                0x0610
#define REG_BSSID                0x0618
#define REG_NAV_UPPER            0x0652
#define REG_RXFLTMAP0            0x06A0
#define REG_RXFLTMAP1            0x06A2
#define REG_RXFLTMAP2            0x06A4

// Baseband / PHY registers
#define RFPGA0_RFMOD             0x0800
#define RFPGA0_XA_HSSIPARAMETER1 0x0820
#define RFPGA0_XA_HSSIPARAMETER2 0x0824
#define RFPGA0_XA_RFINTERFACEOE  0x0860
#define RFPGA0_XAB_RFINTERFACESW 0x0870
#define RFPGA0_XA_LSSIREADBACK   0x08A0
#define TRANSCEIVEA_HSPI_READBACK 0x08B8
#define RFPGA1_RFMOD             0x0900
#define ROFDM0_XAAGCCORE1        0x0C50
#define RTXAGC_A_RATE18_06       0x0E00
#define RTXAGC_A_RATE54_24       0x0E04
#define RTXAGC_A_CCK1_MCS32      0x0E08
#define RTXAGC_A_MCS03_MCS00     0x0E10
#define RTXAGC_A_MCS07_MCS04     0x0E14
#define RTXAGC_B_CCK11_A_CCK2_11 0x086C
#define BLSSIREADADDRESS         0x7F800000U
#define BLSSIREADEDGE            0x80000000U
#define BLSSIREADBACKDATA        0x000FFFFFU
#define BRFSI_RFENV              0x10U
#define B3WIREADDRESSLENGTH      0x400U
#define B3WIREDATALENGTH         0x800U
#define RF_CHNLBW                0x18

// RCR bits (Linux rtl8723be/reg.h)
#define RCR_APPFCS               (1U << 31)
#define RCR_APP_MIC              (1U << 30)
#define RCR_APP_ICV              (1U << 29)
#define RCR_APP_PHYST_RXFF       (1U << 28)
#define RCR_HTC_LOC_CTRL         (1U << 14)
#define RCR_AMF                  (1U << 13)
#define RCR_ACF                  (1U << 12)
#define RCR_ADF                  (1U << 11)
#define RCR_AICV                 (1U << 9)
#define RCR_ACRC32               (1U << 8)
#define RCR_CBSSID_BCN           (1U << 7)
#define RCR_CBSSID_DATA          (1U << 6)
#define RCR_AB                   (1U << 3)
#define RCR_AM                   (1U << 2)
#define RCR_APM                  (1U << 1)
#define RCR_AAP                  (1U << 0)
// Linux sw.c receive_config with ACRC32/AICV cleared as in rtl8723be_hw_init().
#define RTL8723BE_RCR_DEFAULT    (RCR_APPFCS | RCR_APP_MIC | RCR_APP_ICV | RCR_APP_PHYST_RXFF | \
                                  RCR_HTC_LOC_CTRL | RCR_AMF | RCR_ACF | RCR_ADF | \
                                  RCR_AB | RCR_AM | RCR_APM)
#define RTL8723BE_TCR_DEFAULT    ((1U << 9) | (1U << 15) | (1U << 24) | (1U << 25))
#define REG_C2HEVT_MSG_NORM      0x01A0 // 32-bit
#define REG_C2HEVT_CLEAR         0x01AF // 8-bit
#define REG_HMETFR               0x01CC // 32-bit
#define REG_HMEBOX_0             0x01D0 // 32-bit
#define REG_LLT_INIT             0x01E0 // 32-bit (Linked List Table Init)
#define REG_PCIE_CTRL_REG        0x0300 // 16-bit (Queue Doorbell / Polling)
#define REG_INT_MIG              0x0304 // 32-bit (Interrupt Mitigation)
#define REG_BCNQ_DESA            0x0308 // 32-bit
#define REG_HQ_DESA              0x0310 // 32-bit
#define REG_MGQ_DESA             0x0318 // 32-bit
#define REG_VOQ_DESA             0x0320 // 32-bit
#define REG_VIQ_DESA             0x0328 // 32-bit
#define REG_BEQ_DESA             0x0330 // 32-bit
#define REG_BKQ_DESA             0x0338 // 32-bit
#define REG_RX_DESA              0x0340 // 32-bit
#define REG_TCR                  0x0604 // 32-bit (Transmit Configuration)
#define REG_RCR                  0x0608 // 32-bit (Receive Configuration)
#define REG_CAMCMD               0x0670 // 32-bit (CAM Command)
#define REG_CAMWRITE             0x0674 // 32-bit (CAM Write)
#define REG_SECCFG               0x0680 // 16-bit (Security Configuration)
#define REG_RFPGA0_XA_LSSI       0x0840 // 32-bit (3-Wire LSSI RF Control)
#define REG_BB_PAD_CTRL          0x092C // 32-bit (Antenna Switch Control)
#define REG_BB_ANT_DIV           0x092C // Alias for antenna diversity
#define REG_FW_START_ADDR        0x1000 // 4096-byte MCU RAM window

// MCUFWDL Bits
#define MCUFWDL_FWDL_EN          (1U << 0)
#define MCUFWDL_RDY              (1U << 1)
#define MCUFWDL_CHKSUM_RPT       (1U << 2)
#define MCUFWDL_WINTINI_RDY      (1U << 6)
#define MCUFWDL_FW_RESET         (1U << 7)

// Interrupt Bits (HISR / HIMR)
#define IMR_ROK                  (1U << 0)
#define IMR_RDU                  (1U << 1)
#define IMR_VODOK                (1U << 2)
#define IMR_VIDOK                (1U << 3)
#define IMR_BEDOK                (1U << 4)
#define IMR_BKDOK                (1U << 5)
#define IMR_MGNTDOK              (1U << 6)
#define IMR_HIGHDOK              (1U << 7)
#define IMR_BDOK                 (1U << 8)
#define IMR_C2HCMD               (1U << 10)
#define IMR_HSISR_IND            (1U << 15)
#define IMR_PSTIMEOUT            (1U << 29)
#define IMR_RXFOVW               (1U << 8)  // in HIMRE/HISRE
// Linux rtl8723be sw.c irq_mask[0] / irq_mask[1]
#define RTL8723BE_IMR_DEFAULT    (IMR_PSTIMEOUT | IMR_HSISR_IND | IMR_C2HCMD | IMR_HIGHDOK | \
                                  IMR_MGNTDOK | IMR_BKDOK | IMR_BEDOK | IMR_VIDOK | \
                                  IMR_VODOK | IMR_RDU | IMR_ROK)
#define RTL8723BE_IMRE_DEFAULT   IMR_RXFOVW

// TX descriptor rate codes (DESC92C_RATE*)
#define DESC_RATE1M              0x00
#define DESC_RATE11M             0x03
#define DESC_RATE6M              0x04
#define DESC_RATE24M             0x08
#define DESC_RATE54M             0x0B

// Power States
enum RTLPowerState {
    POWER_CARDDIS = 0,
    POWER_CARDEMU = 1,
    POWER_ACT     = 2
};

// Queue Identifiers
enum RTLQueueId {
    Q_BK    = 0,
    Q_BE    = 1,
    Q_VI    = 2,
    Q_VO    = 3,
    Q_BCN   = 4,
    Q_MGNT  = 6,
    Q_HIGH  = 7
};

// Firmware Header (32 bytes packed)
struct __attribute__((packed)) RTLFirmwareHeader {
    uint16_t signature;     // Magic: 0x5301 for RTL8723BE (little-endian)
    uint8_t  category;      // 0x10
    uint8_t  function;      // 0x00
    uint16_t version;       // e.g. 15 or 36
    uint8_t  subversion;
    uint8_t  rsvd1;
    uint8_t  month;
    uint8_t  date;
    uint8_t  hour;
    uint8_t  minute;
    uint16_t ramcodesize;   // microcode payload size in bytes
    uint16_t rsvd2;
    uint32_t svnindex;
    uint32_t rsvd3;
    uint32_t rsvd4;
    uint32_t rsvd5;
};

// RTL8723BE: 40-byte control header followed by PCI DMA addresses.
// Linux rtl8723be/trx.h accessors use dwords 10 (buffer) and 12 (next).
// Match the 64-byte rtlwifi/pci.h allocation stride, not TX_DESC_SIZE.
constexpr uint8_t RTL_TX_HEADER_SIZE = 40;
struct alignas(4) TxDescPci {
    volatile uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t dw4;
    uint32_t dw5;
    uint32_t dw6_reserved;
    uint32_t dw7;
    uint32_t dw8;
    uint32_t dw9;
    uint32_t dw10_txbuffaddr;
    uint32_t dw11_txbuffaddr64;
    uint32_t dw12_nextdesc;
    uint32_t dw13_nextdesc64;
    uint32_t reserved[2];

    void set_buffer_size(uint16_t size) { dw7 = (dw7 & ~0xFFFFU) | size; }
    uint64_t get_next_addr() const {
        return uint64_t(dw12_nextdesc) | (uint64_t(dw13_nextdesc64) << 32);
    }
    void set_next_addr(uint64_t addr) {
        dw12_nextdesc = uint32_t(addr);
        dw13_nextdesc64 = uint32_t(addr >> 32);
    }

    uint16_t get_pktsize() const { return (uint16_t)(dw0 & 0xFFFFU); }
    void set_pktsize(uint16_t sz) { dw0 = (dw0 & ~0xFFFFU) | ((uint32_t)sz & 0xFFFFU); }

    uint8_t get_offset() const { return (uint8_t)((dw0 >> 16) & 0xFFU); }
    void set_offset(uint8_t off) { dw0 = (dw0 & ~(0xFFU << 16)) | (((uint32_t)off & 0xFFU) << 16); }

    bool get_own() const { return (dw0 & (1U << 31)) != 0; }
    void set_own(bool own) { if (own) dw0 |= (1U << 31); else dw0 &= ~(1U << 31); }

    bool get_firstseg() const { return (dw0 & (1U << 27)) != 0; }
    void set_firstseg(bool v) { if (v) dw0 |= (1U << 27); else dw0 &= ~(1U << 27); }

    bool get_lastseg() const { return (dw0 & (1U << 26)) != 0; }
    void set_lastseg(bool v) { if (v) dw0 |= (1U << 26); else dw0 &= ~(1U << 26); }

    uint8_t get_queuesel() const { return (uint8_t)((dw1 >> 8) & 0x1FU); }
    void set_queuesel(uint8_t q) { dw1 = (dw1 & ~(0x1FU << 8)) | (((uint32_t)q & 0x1FU) << 8); }

    // Remaining fields used by Linux rtl8723be_tx_fill_desc() (trx.h bit positions).
    void set_bmc(bool v) { if (v) dw0 |= (1U << 24); else dw0 &= ~(1U << 24); }
    void set_macid(uint8_t id) { dw1 = (dw1 & ~0x7FU) | (id & 0x7FU); }
    void set_rate_id(uint8_t id) { dw1 = (dw1 & ~(0x1FU << 16)) | ((uint32_t)(id & 0x1FU) << 16); }
    void set_use_rate(bool v) { if (v) dw3 |= (1U << 8); else dw3 &= ~(1U << 8); }
    void set_disable_fb(bool v) { if (v) dw3 |= (1U << 10); else dw3 &= ~(1U << 10); }
    void set_tx_rate(uint8_t r) { dw4 = (dw4 & ~0x7FU) | (r & 0x7FU); }
    void set_data_rate_fb_limit(uint8_t l) { dw4 = (dw4 & ~(0x1FU << 8)) | ((uint32_t)(l & 0x1FU) << 8); }
    void set_rts_rate_fb_limit(uint8_t l) { dw4 = (dw4 & ~(0xFU << 13)) | ((uint32_t)(l & 0xFU) << 13); }
    void set_hwseq_en(bool v) { if (v) dw8 |= (1U << 15); else dw8 &= ~(1U << 15); }
    void set_seq(uint16_t s) { dw9 = (dw9 & ~(0xFFFU << 12)) | ((uint32_t)(s & 0xFFFU) << 12); }

    uint64_t get_buffer_addr() const {
        return ((uint64_t)dw10_txbuffaddr) | (((uint64_t)dw11_txbuffaddr64) << 32);
    }
    void set_buffer_addr(uint64_t addr) {
        dw10_txbuffaddr = (uint32_t)(addr & 0xFFFFFFFFULL);
        dw11_txbuffaddr64 = (uint32_t)((addr >> 32) & 0xFFFFFFFFULL);
    }
};

static_assert(sizeof(TxDescPci) == 64, "PCI TX ring stride");
static_assert(offsetof(TxDescPci, dw10_txbuffaddr) == 40, "PCI TX buffer offset");
static_assert(offsetof(TxDescPci, dw12_nextdesc) == 48, "PCI TX next-link offset");

// Queue selector is an on-air priority/firmware queue, not the PCI doorbell bit.
inline uint8_t rtlTxQueueSelect(RTLQueueId q) {
    switch (q) {
        case Q_BK: return 1;
        case Q_BE: return 0;
        case Q_VI: return 4;
        case Q_VO: return 6;
        case Q_BCN: return 0x10;
        case Q_HIGH: return 0x11;
        case Q_MGNT: return 0x12;
    }
    return 0;
}

inline bool rtlDmaRangeValid(uint64_t address, size_t size, size_t alignment) {
    return address != 0 && size != 0 && alignment != 0 &&
           address % alignment == 0 && address <= 0xFFFFFFFFULL &&
           size - 1 <= 0xFFFFFFFFULL - address;
}

// 32-Byte RX Descriptor Layout (8 dwords = 32 bytes)
struct alignas(4) RxDesc32 {
    volatile uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t dw4_reserved;
    uint32_t dw5_tsfl;
    uint32_t dw6_bufferaddress;
    uint32_t dw7_bufferaddress64;

    uint16_t get_length() const { return (uint16_t)(dw0 & 0x3FFFU); }
    void set_length(uint16_t len) { dw0 = (dw0 & ~0x3FFFU) | ((uint32_t)len & 0x3FFFU); }

    bool get_crc32_err() const { return (dw0 & (1U << 14)) != 0; }
    void set_crc32_err(bool err) { if (err) dw0 |= (1U << 14); else dw0 &= ~(1U << 14); }

    bool get_icv_err() const { return (dw0 & (1U << 15)) != 0; }
    void set_icv_err(bool err) { if (err) dw0 |= (1U << 15); else dw0 &= ~(1U << 15); }

    uint8_t get_drv_infosize() const { return (uint8_t)((dw0 >> 16) & 0x0FU); }
    void set_drv_infosize(uint8_t sz) { dw0 = (dw0 & ~(0x0FU << 16)) | (((uint32_t)sz & 0x0FU) << 16); }

    uint8_t get_security() const { return (uint8_t)((dw0 >> 20) & 0x07U); }
    void set_security(uint8_t sec) { dw0 = (dw0 & ~(0x07U << 20)) | (((uint32_t)sec & 0x07U) << 20); }

    uint8_t get_shift() const { return (uint8_t)((dw0 >> 24) & 0x03U); }
    void set_shift(uint8_t s) { dw0 = (dw0 & ~(0x03U << 24)) | (((uint32_t)s & 0x03U) << 24); }

    bool get_eor() const { return (dw0 & (1U << 30)) != 0; }
    void set_eor(bool eor) { if (eor) dw0 |= (1U << 30); else dw0 &= ~(1U << 30); }

    bool get_own() const { return (dw0 & (1U << 31)) != 0; }
    void set_own(bool own) { if (own) dw0 |= (1U << 31); else dw0 &= ~(1U << 31); }

    bool get_physt() const { return (dw0 & (1U << 26)) != 0; }
    uint8_t get_rxmcs() const { return (uint8_t)(dw3 & 0x7FU); }

    uint64_t get_buffer_addr() const {
        return ((uint64_t)dw6_bufferaddress) | (((uint64_t)dw7_bufferaddress64) << 32);
    }
    void set_buffer_addr(uint64_t addr) {
        dw6_bufferaddress = (uint32_t)(addr & 0xFFFFFFFFULL);
        dw7_bufferaddress64 = (uint32_t)((addr >> 32) & 0xFFFFFFFFULL);
    }
};

// eFuse logical map layout (Linux rtl8723be/reg.h)
#define RTL8723BE_EFUSE_MAP_LEN        512
#define RTL8723BE_EFUSE_REAL_LEN       256   // physical bytes holding the WLAN PG stream
#define RTL8723BE_EFUSE_MAX_SECTION    64
#define RTL8723BE_EEPROM_ID            0x8129
#define EEPROM_TX_PWR_INX              0x10
#define EEPROM_CHANNELPLAN             0xB8
#define EEPROM_XTAL_8723BE             0xB9
#define EEPROM_THERMAL_METER           0xBA
#define EEPROM_RF_BOARD_OPTION         0xC1
#define EEPROM_RF_BT_SETTING_8723B     0xC3
#define EEPROM_CUSTOMER_ID             0xC5
#define EEPROM_MAC_ADDR                0xD0
#define EEPROM_VID                     0xD6
#define EEPROM_DID                     0xD8
#define EEPROM_SVID                    0xDA
#define EEPROM_SMID                    0xDC

// Decode a Realtek physical eFuse PG stream into the 512-byte logical map
// (Linux efuse.c read_efuse(), including 2-byte extended headers).
// Returns the number of physical bytes consumed.
inline size_t rtlEfuseDecode(const uint8_t *phys, size_t physLen, uint8_t *logical, size_t logicalLen) {
    for (size_t i = 0; i < logicalLen; ++i) logical[i] = 0xFF;
    size_t addr = 0;
    while (addr < physLen && phys[addr] != 0xFF) {
        uint8_t hdr = phys[addr++];
        uint8_t section, wren;
        if ((hdr & 0x1F) == 0x0F) { // extended header
            if (addr >= physLen) break;
            uint8_t hdr2 = phys[addr++];
            if ((hdr2 & 0x0F) == 0x0F) continue; // invalid/unused extended header
            section = (uint8_t)(((hdr2 & 0xF0) >> 1) | ((hdr & 0xE0) >> 5));
            wren = hdr2 & 0x0F;
        } else {
            section = (hdr >> 4) & 0x0F;
            wren = hdr & 0x0F;
        }
        for (int w = 0; w < 4; ++w, wren >>= 1) {
            if (wren & 0x01) continue; // 0 = word present
            if (addr + 1 >= physLen) return physLen;
            size_t off = (size_t)section * 8 + (size_t)w * 2;
            if (section < RTL8723BE_EFUSE_MAX_SECTION && off + 1 < logicalLen) {
                logical[off] = phys[addr];
                logical[off + 1] = phys[addr + 1];
            }
            addr += 2;
        }
    }
    return addr;
}

// eFuse Calibration Structure
struct CalibData {
    uint8_t mac_addr[6];
    uint8_t crystal_cap;
    uint8_t thermal_meter;
    uint8_t channel_plan;
    uint8_t tx_pwr_cck[6];
    uint8_t tx_pwr_ht40[5];
    uint8_t tx_pwr_ht20_diff;
    uint16_t vid;
    uint16_t did;
    uint16_t svid;
    uint16_t smid;
    uint8_t  customer_id;
    uint8_t  bt_setting;      // EEPROM 0xC3: bit0 = 2 antennas, bit6 = single-ant path is AUX
    uint8_t  package_type;    // 0 default, 1 QFN68, 2 TFBGA90, 3 TFBGA80, 4 TFBGA79
    bool     autoload_ok;     // EEPROM ID == 0x8129
    uint16_t efuse_used;
};

#endif // _RTL8723BE_HW_HPP_
