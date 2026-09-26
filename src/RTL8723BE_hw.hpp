#ifndef _RTL8723BE_HW_HPP_
#define _RTL8723BE_HW_HPP_

#include <stdint.h>
#include <stddef.h>

// Realtek RTL8723BE Register Map (16KB BAR2 MMIO aperture)
#define REG_SYS_ISO_CTRL         0x0000 // 16-bit
#define REG_SYS_FUNC_EN          0x0002 // 16-bit
#define REG_APS_FSMCO            0x0004 // 16-bit
#define REG_PWR_STATUS           0x0006 // 8-bit / 16-bit
#define REG_SYS_CLKR             0x0008 // 16-bit
#define REG_9346CR               0x000A // 8-bit
#define REG_RSV_CTRL             0x001C // 16-bit
#define REG_RF_CTRL              0x001F // 8-bit
#define REG_MULTI_FUNC_CTRL      0x0020 // 8-bit
#define REG_HWSEQ_CTRL           0x0023 // 8-bit
#define REG_EFUSE_CTRL           0x0030 // 32-bit
#define REG_EFUSE_TEST           0x0034 // 32-bit
#define REG_GPIO_MUXCFG          0x0040 // 16-bit
#define REG_HSIMR                0x0058 // 32-bit
#define REG_HSISR                0x005C // 32-bit
#define REG_MCUFWDL              0x0080 // 32-bit
#define REG_MCUFWDL_PAGE         0x0082 // 8-bit (MCU RAM Page Select)
#define REG_HIMR                 0x00B0 // 32-bit (Host Interrupt Mask)
#define REG_HISR                 0x00B4 // 32-bit (Host Interrupt Status)
#define REG_HIMRE                0x00B8 // 32-bit (Host Interrupt Mask Extension)
#define REG_HISRE                0x00BC // 32-bit (Host Interrupt Status Extension)
#define REG_EFUSE_ACCESS         0x00CF // 8-bit (0x69 enable, 0x00 disable)
#define REG_CR                   0x0100 // 16-bit (Command Register)
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

// 40-Byte TX Descriptor Layout (10 dwords = 40 bytes)
struct alignas(4) TxDesc40 {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t dw4;
    uint32_t dw5;
    uint32_t dw6_reserved;
    uint32_t dw7;
    uint32_t dw8_txbuffaddr;
    uint32_t dw9_txbuffaddr64;

    uint16_t get_pktsize() const { return dw0 & 0xFFFF; }
    void set_pktsize(uint16_t sz) { dw0 = (dw0 & ~0xFFFF) | (sz & 0xFFFF); }

    uint8_t get_offset() const { return (dw0 >> 16) & 0xFF; }
    void set_offset(uint8_t off) { dw0 = (dw0 & ~(0xFF << 16)) | ((off & 0xFF) << 16); }

    bool get_own() const { return (dw0 & (1U << 31)) != 0; }
    void set_own(bool own) { if (own) dw0 |= (1U << 31); else dw0 &= ~(1U << 31); }

    bool get_firstseg() const { return (dw0 & (1U << 27)) != 0; }
    void set_firstseg(bool v) { if (v) dw0 |= (1U << 27); else dw0 &= ~(1U << 27); }

    bool get_lastseg() const { return (dw0 & (1U << 26)) != 0; }
    void set_lastseg(bool v) { if (v) dw0 |= (1U << 26); else dw0 &= ~(1U << 26); }

    uint8_t get_queuesel() const { return (dw1 >> 8) & 0x1F; }
    void set_queuesel(uint8_t q) { dw1 = (dw1 & ~(0x1F << 8)) | ((q & 0x1F) << 8); }

    uint64_t get_buffer_addr() const {
        return ((uint64_t)dw8_txbuffaddr) | (((uint64_t)dw9_txbuffaddr64) << 32);
    }
    void set_buffer_addr(uint64_t addr) {
        dw8_txbuffaddr = (uint32_t)(addr & 0xFFFFFFFF);
        dw9_txbuffaddr64 = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    }
};

// 32-Byte RX Descriptor Layout (8 dwords = 32 bytes)
struct alignas(4) RxDesc32 {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t dw4_reserved;
    uint32_t dw5_tsfl;
    uint32_t dw6_bufferaddress;
    uint32_t dw7_bufferaddress64;

    uint16_t get_length() const { return dw0 & 0x3FFF; }
    void set_length(uint16_t len) { dw0 = (dw0 & ~0x3FFF) | (len & 0x3FFF); }

    bool get_crc32_err() const { return (dw0 & (1U << 14)) != 0; }
    void set_crc32_err(bool err) { if (err) dw0 |= (1U << 14); else dw0 &= ~(1U << 14); }

    bool get_icv_err() const { return (dw0 & (1U << 15)) != 0; }
    void set_icv_err(bool err) { if (err) dw0 |= (1U << 15); else dw0 &= ~(1U << 15); }

    uint8_t get_drv_infosize() const { return (dw0 >> 16) & 0x0F; }
    void set_drv_infosize(uint8_t sz) { dw0 = (dw0 & ~(0x0F << 16)) | ((sz & 0x0F) << 16); }

    uint8_t get_security() const { return (dw0 >> 20) & 0x07; }
    void set_security(uint8_t sec) { dw0 = (dw0 & ~(0x07 << 20)) | ((sec & 0x07) << 20); }

    uint8_t get_shift() const { return (dw0 >> 24) & 0x03; }
    void set_shift(uint8_t s) { dw0 = (dw0 & ~(0x03 << 24)) | ((s & 0x03) << 24); }

    bool get_eor() const { return (dw0 & (1U << 30)) != 0; }
    void set_eor(bool eor) { if (eor) dw0 |= (1U << 30); else dw0 &= ~(1U << 30); }

    bool get_own() const { return (dw0 & (1U << 31)) != 0; }
    void set_own(bool own) { if (own) dw0 |= (1U << 31); else dw0 &= ~(1U << 31); }

    uint64_t get_buffer_addr() const {
        return ((uint64_t)dw6_bufferaddress) | (((uint64_t)dw7_bufferaddress64) << 32);
    }
    void set_buffer_addr(uint64_t addr) {
        dw6_bufferaddress = (uint32_t)(addr & 0xFFFFFFFF);
        dw7_bufferaddress64 = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    }
};

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
};

#endif // _RTL8723BE_HW_HPP_
