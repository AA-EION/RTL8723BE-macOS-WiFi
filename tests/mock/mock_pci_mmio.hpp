#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <functional>
#include <map>
#include <string>

namespace rtl_mock {

// Register Offsets
constexpr uint16_t REG_SYS_FUNC_EN     = 0x0002; // 16-bit
constexpr uint16_t REG_APS_FSMCO       = 0x0004; // 16-bit
constexpr uint16_t REG_PWR_STATUS      = 0x0006; // 8-bit / 16-bit
constexpr uint16_t REG_SYS_CLKR        = 0x0008; // 16-bit
constexpr uint16_t REG_9346CR          = 0x000A; // 8-bit
constexpr uint16_t REG_RSV_CTRL        = 0x001C; // 16-bit
constexpr uint16_t REG_RF_CTRL         = 0x001F; // 8-bit
constexpr uint16_t REG_MULTI_FUNC_CTRL = 0x0020; // 8-bit
constexpr uint16_t REG_HWSEQ_CTRL      = 0x0023; // 8-bit
constexpr uint16_t REG_EFUSE_CTRL      = 0x0030; // 32-bit
constexpr uint16_t REG_EFUSE_TEST      = 0x0034; // 32-bit
constexpr uint16_t REG_GPIO_MUXCFG     = 0x0040; // 16-bit
constexpr uint16_t REG_HSIMR           = 0x0058; // 32-bit
constexpr uint16_t REG_HSISR           = 0x005C; // 32-bit
constexpr uint16_t REG_MCUFWDL         = 0x0080; // 32-bit
constexpr uint16_t REG_MCUFWDL_PAGE    = 0x0082; // 8-bit (offset 0x82)
constexpr uint16_t REG_HIMR            = 0x00B0; // 32-bit
constexpr uint16_t REG_HISR            = 0x00B4; // 32-bit
constexpr uint16_t REG_HIMRE           = 0x00B8; // 32-bit
constexpr uint16_t REG_HISRE           = 0x00BC; // 32-bit
constexpr uint16_t REG_EFUSE_ACCESS    = 0x00CF; // 8-bit
constexpr uint16_t REG_CR              = 0x0100; // 16-bit
constexpr uint16_t REG_C2HEVT_MSG_NORM = 0x01A0; // 32-bit
constexpr uint16_t REG_C2HEVT_CLEAR    = 0x01AF; // 8-bit
constexpr uint16_t REG_HMETFR          = 0x01CC; // 32-bit
constexpr uint16_t REG_HMEBOX_0        = 0x01D0; // 32-bit
constexpr uint16_t REG_LLT_INIT        = 0x01E0; // 32-bit
constexpr uint16_t REG_PCIE_CTRL_REG   = 0x0300; // 16-bit
constexpr uint16_t REG_INT_MIG         = 0x0304; // 32-bit
constexpr uint16_t REG_BCNQ_DESA       = 0x0308; // 32-bit
constexpr uint16_t REG_HQ_DESA         = 0x0310; // 32-bit
constexpr uint16_t REG_MGQ_DESA        = 0x0318; // 32-bit
constexpr uint16_t REG_VOQ_DESA        = 0x0320; // 32-bit
constexpr uint16_t REG_VIQ_DESA        = 0x0328; // 32-bit
constexpr uint16_t REG_BEQ_DESA        = 0x0330; // 32-bit
constexpr uint16_t REG_BKQ_DESA        = 0x0338; // 32-bit
constexpr uint16_t REG_RX_DESA         = 0x0340; // 32-bit
constexpr uint16_t REG_TCR             = 0x0604; // 32-bit
constexpr uint16_t REG_RCR             = 0x0608; // 32-bit
constexpr uint16_t REG_CAMCMD          = 0x0670; // 32-bit
constexpr uint16_t REG_CAMWRITE        = 0x0674; // 32-bit
constexpr uint16_t REG_SECCFG          = 0x0680; // 16-bit
constexpr uint16_t REG_RFPGA0_XA_LSSI  = 0x0840; // 32-bit
constexpr uint16_t REG_BB_ANT_DIV      = 0x092C; // 32-bit
constexpr uint16_t REG_FW_START_ADDR   = 0x1000; // 4096 bytes window

// MCUFWDL Bits
constexpr uint32_t MCUFWDL_FWDL_EN     = (1 << 0);
constexpr uint32_t MCUFWDL_RDY         = (1 << 1);
constexpr uint32_t MCUFWDL_CHKSUM_RPT  = (1 << 2);
constexpr uint32_t MCUFWDL_WINTINI_RDY = (1 << 6);
constexpr uint32_t MCUFWDL_FW_RESET    = (1 << 7);

// HISR / HIMR Interrupt Bits
constexpr uint32_t IMR_ROK             = (1 << 0);
constexpr uint32_t IMR_RDU             = (1 << 1);
constexpr uint32_t IMR_VODOK           = (1 << 2);
constexpr uint32_t IMR_VIDOK           = (1 << 3);
constexpr uint32_t IMR_BEDOK           = (1 << 4);
constexpr uint32_t IMR_BKDOK           = (1 << 5);
constexpr uint32_t IMR_MGNTDOK         = (1 << 6);
constexpr uint32_t IMR_HIGHDOK         = (1 << 7);
constexpr uint32_t IMR_BDOK            = (1 << 8);
constexpr uint32_t IMR_C2HCMD          = (1 << 10);
constexpr uint32_t IMR_HSISR_IND       = (1 << 15);

// Power States
enum PowerState {
    POWER_CARDDIS = 0,
    POWER_CARDEMU = 1,
    POWER_ACT     = 2
};

// Firmware Header (32 bytes)
struct FirmwareHeader {
    uint16_t signature;     // 0x5301 for RTL8723BE
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

// Calibration data structure extracted from eFuse
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

class MockPCIMmio {
public:
    MockPCIMmio();
    ~MockPCIMmio() = default;

    // Reset all registers and simulation state
    void reset();

    // 16KB MMIO Accessors
    uint8_t  read8(uint16_t offset) const;
    uint16_t read16(uint16_t offset) const;
    uint32_t read32(uint16_t offset) const;

    void write8(uint16_t offset, uint8_t val);
    void write16(uint16_t offset, uint16_t val);
    void write32(uint16_t offset, uint32_t val);

    // Raw pointer to 16KB MMIO space (for direct inspection)
    uint8_t* raw_mmio() { return mmio_space_.data(); }
    const uint8_t* raw_mmio() const { return mmio_space_.data(); }
    size_t mmio_size() const { return mmio_space_.size(); }

    // Power Sequence Emulation
    PowerState get_power_state() const { return power_state_; }
    void set_power_state(PowerState state) { power_state_ = state; }

    // eFuse Emulation
    void set_efuse_byte(uint16_t offset, uint8_t val);
    uint8_t get_efuse_byte(uint16_t offset) const;
    void set_mac_address(const uint8_t mac[6]);
    void get_mac_address(uint8_t mac[6]) const;
    const std::array<uint8_t, 512>& get_logical_efuse() const { return logical_efuse_; }
    const std::vector<uint8_t>& get_physical_efuse() const { return physical_efuse_; }
    void encode_pg_packets(); // Encodes logical eFuse into physical PG packets

    // 8051 MCU Download Handshake Emulation
    bool is_mcu_fw_ready() const { return (read32(REG_MCUFWDL) & MCUFWDL_WINTINI_RDY) != 0; }
    void set_force_checksum_failure(bool fail) { force_checksum_fail_ = fail; }
    const std::vector<uint8_t>& get_downloaded_fw_ram() const { return mcu_fw_ram_; }
    uint16_t get_computed_fw_checksum() const { return computed_fw_checksum_; }

    // Interrupt Hooks
    void assert_interrupt(uint32_t isr_bits);
    void set_interrupt_callback(std::function<void(uint32_t)> cb) { interrupt_cb_ = cb; }

    // Register Callbacks
    using RegHook = std::function<void(uint16_t offset, uint32_t val, uint8_t size)>;
    void set_write_hook(uint16_t offset, RegHook hook) { write_hooks_[offset] = hook; }
    void clear_write_hook(uint16_t offset) { write_hooks_.erase(offset); }

    // Doorbell Callback (fires when REG_PCIE_CTRL_REG 0x0300 is written)
    void set_doorbell_callback(std::function<void(uint16_t queue_mask)> cb) { doorbell_cb_ = cb; }

    // Antenna Selection status
    uint8_t get_active_antenna() const; // 1 = Main, 2 = Aux

    // Channel Tuning status
    uint8_t get_active_channel() const { return active_channel_; }

private:
    void handle_internal_write(uint16_t offset, uint32_t val, uint8_t size);
    void handle_efuse_command(uint32_t cmd);
    void handle_mcu_firmware_write(uint16_t offset, uint8_t val);
    void handle_lssi_rf_write(uint32_t val);

    std::array<uint8_t, 0x4000> mmio_space_;
    PowerState power_state_{POWER_CARDDIS};

    // eFuse Storage
    std::array<uint8_t, 512> logical_efuse_;
    std::vector<uint8_t> physical_efuse_;

    // MCU Firmware Download
    std::vector<uint8_t> mcu_fw_ram_;
    uint8_t current_fw_page_{0};
    uint16_t computed_fw_checksum_{0};
    bool force_checksum_fail_{false};

    // RF / BB State
    uint8_t active_channel_{1};
    uint8_t active_antenna_{1}; // 1 = Main, 2 = Aux

    // Callbacks
    std::function<void(uint32_t)> interrupt_cb_;
    std::function<void(uint16_t)> doorbell_cb_;
    std::map<uint16_t, RegHook> write_hooks_;
};

} // namespace rtl_mock
