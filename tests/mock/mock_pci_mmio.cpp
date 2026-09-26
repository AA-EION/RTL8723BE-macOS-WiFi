#include "mock_pci_mmio.hpp"
#include <cstring>
#include <algorithm>

namespace rtl_mock {

MockPCIMmio::MockPCIMmio() {
    reset();
}

void MockPCIMmio::reset() {
    mmio_space_.fill(0);
    logical_efuse_.fill(0xFF);
    physical_efuse_.clear();
    mcu_fw_ram_.clear();
    current_fw_page_ = 0;
    computed_fw_checksum_ = 0;
    force_checksum_fail_ = false;
    power_state_ = POWER_CARDDIS;
    active_channel_ = 1;
    active_antenna_ = 1;

    // Initialize default register values
    // REG_9346CR (0x000A): Bit 5 (Autoload OK) = 1, Bit 4 (eFuse) = 0
    mmio_space_[REG_9346CR] = 0x20;

    // REG_PWR_STATUS (0x0006): Power rail status
    mmio_space_[REG_PWR_STATUS] = 0x00;

    // Initialize default eFuse factory calibration
    // CCK TX power: offsets 0x10..0x15
    for (int i = 0x10; i <= 0x15; ++i) logical_efuse_[i] = 0x2D; // 45
    // HT40 TX power: offsets 0x16..0x1A
    for (int i = 0x16; i <= 0x1A; ++i) logical_efuse_[i] = 0x2D; // 45
    logical_efuse_[0x1B] = 0x02; // HT20 diff
    logical_efuse_[0x00B8] = 0x00; // Channel plan: World 13
    logical_efuse_[0x00B9] = 0x28; // Crystal cap trim
    logical_efuse_[0x00BA] = 0x1A; // Thermal meter
    logical_efuse_[0x00BB] = 0x00; // IQK / LCK
    logical_efuse_[0x00C1] = 0x00; // RF board option
    logical_efuse_[0x00C3] = 0x01; // BT setting / antenna 1T1R
    logical_efuse_[0x00C4] = 0x01; // EEPROM version

    // Factory MAC Address: 00:E0:4C:81:92:23
    uint8_t default_mac[6] = {0x00, 0xE0, 0x4C, 0x81, 0x92, 0x23};
    std::memcpy(&logical_efuse_[0x00D0], default_mac, 6);

    // PCI IDs in eFuse
    logical_efuse_[0x00D6] = 0xEC; // VID low (0x10EC)
    logical_efuse_[0x00D7] = 0x10;
    logical_efuse_[0x00D8] = 0x23; // DID low (0xB723)
    logical_efuse_[0x00D9] = 0xB7;
    logical_efuse_[0x00DA] = 0x3C; // SVID low (0x103C)
    logical_efuse_[0x00DB] = 0x10;
    logical_efuse_[0x00DC] = 0x4C; // SMID low (0x804C)
    logical_efuse_[0x00DD] = 0x80;

    // Generate physical PG packets from the logical map
    encode_pg_packets();
}

uint8_t MockPCIMmio::read8(uint16_t offset) const {
    if (offset >= 0x4000) return 0xFF;
    return mmio_space_[offset];
}

uint16_t MockPCIMmio::read16(uint16_t offset) const {
    if (offset + 1 >= 0x4000) return 0xFFFF;
    return static_cast<uint16_t>(mmio_space_[offset]) |
          (static_cast<uint16_t>(mmio_space_[offset + 1]) << 8);
}

uint32_t MockPCIMmio::read32(uint16_t offset) const {
    if (offset + 3 >= 0x4000) return 0xFFFFFFFF;
    return static_cast<uint32_t>(mmio_space_[offset]) |
          (static_cast<uint32_t>(mmio_space_[offset + 1]) << 8) |
          (static_cast<uint32_t>(mmio_space_[offset + 2]) << 16) |
          (static_cast<uint32_t>(mmio_space_[offset + 3]) << 24);
}

void MockPCIMmio::write8(uint16_t offset, uint8_t val) {
    if (offset >= 0x4000) return;
    handle_internal_write(offset, val, 1);
    if (write_hooks_.find(offset) != write_hooks_.end()) {
        write_hooks_[offset](offset, val, 1);
    }
}

void MockPCIMmio::write16(uint16_t offset, uint16_t val) {
    if (offset + 1 >= 0x4000) return;
    handle_internal_write(offset, val, 2);
    if (write_hooks_.find(offset) != write_hooks_.end()) {
        write_hooks_[offset](offset, val, 2);
    }
}

void MockPCIMmio::write32(uint16_t offset, uint32_t val) {
    if (offset + 3 >= 0x4000) return;
    handle_internal_write(offset, val, 4);
    if (write_hooks_.find(offset) != write_hooks_.end()) {
        write_hooks_[offset](offset, val, 4);
    }
}

void MockPCIMmio::handle_internal_write(uint16_t offset, uint32_t val, uint8_t size) {
    // Write into MMIO array
    for (uint8_t i = 0; i < size; ++i) {
        mmio_space_[offset + i] = static_cast<uint8_t>((val >> (i * 8)) & 0xFF);
    }

    // Intercept specific registers:

    // 1. Host Interrupt Status Register (REG_HISR, 0x00B4) is Write-1-to-Clear
    if (offset == REG_HISR && size == 4) {
        uint32_t current = read32(REG_HISR);
        uint32_t cleared = current & ~val;
        mmio_space_[REG_HISR]     = cleared & 0xFF;
        mmio_space_[REG_HISR + 1] = (cleared >> 8) & 0xFF;
        mmio_space_[REG_HISR + 2] = (cleared >> 16) & 0xFF;
        mmio_space_[REG_HISR + 3] = (cleared >> 24) & 0xFF;
        return;
    }

    // 2. Power State Machine: REG_RSV_CTRL (0x001C)
    if (offset == REG_RSV_CTRL) {
        if ((val & 0xFF) == 0x00) {
            // Power unlock: CARDDIS -> CARDEMU
            if (power_state_ == POWER_CARDDIS) {
                power_state_ = POWER_CARDEMU;
            }
        } else if ((val & 0xFF) == 0x0E) {
            // Power lock: back to CARDDIS
            power_state_ = POWER_CARDDIS;
            mmio_space_[REG_PWR_STATUS] &= ~0x02; // Power ready bit 1 cleared
        }
    }

    // REG_APS_FSMCO (0x0004) & REG_SYS_FUNC_EN (0x0002)
    if (offset == REG_SYS_FUNC_EN || offset == (REG_SYS_FUNC_EN + 1)) {
        uint16_t func_en = read16(REG_SYS_FUNC_EN);
        if ((func_en & 0x0001) && (func_en & 0x0400 || func_en & 0x0004)) {
            // Clocks and DMA enabled: transition to ACT
            if (power_state_ == POWER_CARDEMU) {
                power_state_ = POWER_ACT;
                mmio_space_[REG_PWR_STATUS] |= 0x02; // Power ready bit 1 set
            }
        }
    }

    // REG_CR (0x0100): Command register
    if (offset == REG_CR) {
        uint16_t cr_val = read16(REG_CR);
        if (cr_val == 0x02FF) {
            // Full TRX + MAC operational
            power_state_ = POWER_ACT;
            mmio_space_[REG_PWR_STATUS] |= 0x02;
        }
    }

    // 3. eFuse Controller (REG_EFUSE_CTRL, 0x0030)
    if (offset == REG_EFUSE_CTRL && size == 4) {
        handle_efuse_command(val);
    } else if (offset == (REG_EFUSE_CTRL + 3) && size == 1) {
        handle_efuse_command(read32(REG_EFUSE_CTRL));
    }

    // 4. MCU Firmware Download Control (REG_MCUFWDL, 0x0080)
    if (offset == REG_MCUFWDL) {
        if (val & MCUFWDL_FWDL_EN) {
            // FW download enable: prepare memory
            mcu_fw_ram_.clear();
            computed_fw_checksum_ = 0;
        } else {
            // FW download disable: host finished upload, evaluate checksum report
            if (!force_checksum_fail_) {
                // Report checksum OK
                mmio_space_[REG_MCUFWDL] |= MCUFWDL_CHKSUM_RPT;
            }
        }

        if (val & MCUFWDL_RDY) {
            // Host signals FW ready & triggers MCU reset
            // MCU simulates initialization and sets WINTINI_RDY (bit 6)
            mmio_space_[REG_MCUFWDL] |= MCUFWDL_WINTINI_RDY;
            mmio_space_[REG_MCUFWDL] &= ~MCUFWDL_FW_RESET;
        }
    }

    // MCU Page select (REG_MCUFWDL_PAGE, 0x0082)
    if (offset == REG_MCUFWDL_PAGE) {
        current_fw_page_ = val & 0x07;
    }

    // MCU RAM Download Window (0x1000 .. 0x1FFF)
    if (offset >= REG_FW_START_ADDR && offset < (REG_FW_START_ADDR + 0x1000)) {
        for (uint8_t i = 0; i < size; ++i) {
            handle_mcu_firmware_write(offset - REG_FW_START_ADDR + i,
                                      static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
    }

    // 5. Doorbell Trigger: REG_PCIE_CTRL_REG (0x0300)
    if (offset == REG_PCIE_CTRL_REG) {
        uint16_t doorbell_mask = read16(REG_PCIE_CTRL_REG);
        if (doorbell_cb_ && doorbell_mask != 0) {
            doorbell_cb_(doorbell_mask);
        }
    }

    // 6. Antenna Division register (REG_BB_ANT_DIV, 0x092C)
    if (offset == REG_BB_ANT_DIV) {
        active_antenna_ = static_cast<uint8_t>(val & 0x03);
    }

    // 7. 3-Wire LSSI RF Parameter register (REG_RFPGA0_XA_LSSI, 0x0840)
    if (offset == REG_RFPGA0_XA_LSSI && size == 4) {
        handle_lssi_rf_write(val);
    }
}

void MockPCIMmio::handle_efuse_command(uint32_t cmd) {
    // Format: [7:0] Data; [17:8] Address; [30:24] Mode (0x72 read, 0xF2 write); [31] Busy
    uint16_t addr = (cmd >> 8) & 0x03FF;
    uint8_t mode = (cmd >> 24) & 0x7F;

    if (mode == 0x72) {
        // Read mode: fetch byte from logical eFuse
        uint8_t byte_val = 0xFF;
        if (addr < logical_efuse_.size()) {
            byte_val = logical_efuse_[addr];
        }

        // Latch data byte into [7:0] and clear busy bit [31]
        uint32_t result = (cmd & 0x7FFFFF00) | byte_val;
        result &= ~0x80000000; // Busy bit = 0

        mmio_space_[REG_EFUSE_CTRL]     = result & 0xFF;
        mmio_space_[REG_EFUSE_CTRL + 1] = (result >> 8) & 0xFF;
        mmio_space_[REG_EFUSE_CTRL + 2] = (result >> 16) & 0xFF;
        mmio_space_[REG_EFUSE_CTRL + 3] = (result >> 24) & 0xFF;
    }
}

void MockPCIMmio::handle_mcu_firmware_write(uint16_t offset, uint8_t val) {
    size_t abs_offset = static_cast<size_t>(current_fw_page_) * 4096 + offset;
    if (abs_offset >= mcu_fw_ram_.size()) {
        mcu_fw_ram_.resize(abs_offset + 1, 0);
    }
    mcu_fw_ram_[abs_offset] = val;

    // Running 16-bit word checksum (little endian addition)
    if (abs_offset % 2 == 1) {
        uint16_t word = static_cast<uint16_t>(mcu_fw_ram_[abs_offset - 1]) |
                       (static_cast<uint16_t>(val) << 8);
        computed_fw_checksum_ += word;
    }
}

void MockPCIMmio::handle_lssi_rf_write(uint32_t val) {
    // Format: Bits [27:20] = RF Register offset, Bits [19:0] = RF data
    uint8_t rf_reg = (val >> 20) & 0xFF;
    uint32_t rf_data = val & 0x000FFFFF;

    // RF_CHNLBW is register 0x18
    if (rf_reg == 0x18) {
        uint8_t channel = rf_data & 0x3FF;
        if (channel >= 1 && channel <= 14) {
            active_channel_ = channel;
        }
    }
}

void MockPCIMmio::assert_interrupt(uint32_t isr_bits) {
    uint32_t current_hisr = read32(REG_HISR);
    current_hisr |= isr_bits;

    mmio_space_[REG_HISR]     = current_hisr & 0xFF;
    mmio_space_[REG_HISR + 1] = (current_hisr >> 8) & 0xFF;
    mmio_space_[REG_HISR + 2] = (current_hisr >> 16) & 0xFF;
    mmio_space_[REG_HISR + 3] = (current_hisr >> 24) & 0xFF;

    uint32_t himr = read32(REG_HIMR);
    if ((current_hisr & himr) != 0 && interrupt_cb_) {
        interrupt_cb_(current_hisr & himr);
    }
}

void MockPCIMmio::set_efuse_byte(uint16_t offset, uint8_t val) {
    if (offset < logical_efuse_.size()) {
        logical_efuse_[offset] = val;
    }
}

uint8_t MockPCIMmio::get_efuse_byte(uint16_t offset) const {
    if (offset < logical_efuse_.size()) {
        return logical_efuse_[offset];
    }
    return 0xFF;
}

void MockPCIMmio::set_mac_address(const uint8_t mac[6]) {
    std::memcpy(&logical_efuse_[0x00D0], mac, 6);
    encode_pg_packets();
}

void MockPCIMmio::get_mac_address(uint8_t mac[6]) const {
    std::memcpy(mac, &logical_efuse_[0x00D0], 6);
}

uint8_t MockPCIMmio::get_active_antenna() const {
    return active_antenna_;
}

void MockPCIMmio::encode_pg_packets() {
    physical_efuse_.clear();

    // 512 bytes = 32 blocks of 16 bytes (8 words)
    for (uint8_t block = 0; block < 32; ++block) {
        size_t block_base = block * 16;
        // Check words 0..3 (low half)
        uint8_t word_mask_low = 0;
        for (int w = 0; w < 4; ++w) {
            uint8_t b0 = logical_efuse_[block_base + w * 2];
            uint8_t b1 = logical_efuse_[block_base + w * 2 + 1];
            if (b0 != 0xFF || b1 != 0xFF) {
                word_mask_low |= (1 << w);
            }
        }

        if (word_mask_low != 0) {
            // Header: [7:4] = offset (block * 2), [3:0] = ~word_mask (in Realtek PG format, mask bits are active low 0)
            uint8_t header = static_cast<uint8_t>(((block * 2) << 4) | (~word_mask_low & 0x0F));
            physical_efuse_.push_back(header);
            for (int w = 0; w < 4; ++w) {
                if (word_mask_low & (1 << w)) {
                    physical_efuse_.push_back(logical_efuse_[block_base + w * 2]);
                    physical_efuse_.push_back(logical_efuse_[block_base + w * 2 + 1]);
                }
            }
        }

        // Check words 4..7 (high half)
        uint8_t word_mask_high = 0;
        for (int w = 0; w < 4; ++w) {
            uint8_t b0 = logical_efuse_[block_base + 8 + w * 2];
            uint8_t b1 = logical_efuse_[block_base + 8 + w * 2 + 1];
            if (b0 != 0xFF || b1 != 0xFF) {
                word_mask_high |= (1 << w);
            }
        }

        if (word_mask_high != 0) {
            uint8_t header = static_cast<uint8_t>((((block * 2) + 1) << 4) | (~word_mask_high & 0x0F));
            physical_efuse_.push_back(header);
            for (int w = 0; w < 4; ++w) {
                if (word_mask_high & (1 << w)) {
                    physical_efuse_.push_back(logical_efuse_[block_base + 8 + w * 2]);
                    physical_efuse_.push_back(logical_efuse_[block_base + 8 + w * 2 + 1]);
                }
            }
        }
    }

    // Trailing 0xFF marks end of eFuse packets
    physical_efuse_.push_back(0xFF);
}

} // namespace rtl_mock
