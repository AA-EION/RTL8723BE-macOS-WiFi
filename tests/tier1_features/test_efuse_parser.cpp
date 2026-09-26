#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/simulated_device.hpp"
#include <vector>
#include <cstring>

using namespace rtl_mock;

// Test 1: Factory MAC Address Readout via REG_EFUSE_CTRL
REGISTER_TEST(Tier1_eFuse, test_efuse_read_mac) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    // Set custom MAC in mock eFuse
    uint8_t custom_mac[6] = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
    mmio.set_mac_address(custom_mac);

    CalibData calib;
    ASSERT_TRUE(dev.hw_read_efuse(calib));

    ASSERT_MEMEQ(calib.mac_addr, custom_mac, 6);
    ASSERT_MEMEQ(dev.get_mac_address(), custom_mac, 6);
}

// Test 2: Autoload Status Detection in REG_9346CR
REGISTER_TEST(Tier1_eFuse, test_efuse_autoload_status) {
    MockPCIMmio mmio;

    // Verify initial power-up autoload OK flag (Bit 5 in REG_9346CR 0x000A)
    uint8_t cr9346 = mmio.read8(REG_9346CR);
    ASSERT_TRUE((cr9346 & 0x20) != 0); // Bit 5 == 1 (Autoload OK)
    ASSERT_TRUE((cr9346 & 0x10) == 0); // Bit 4 == 0 (eFuse mode, not EEPROM)

    // Simulate autoload failure
    mmio.write8(REG_9346CR, cr9346 & ~0x20);
    uint8_t cr9346_fail = mmio.read8(REG_9346CR);
    ASSERT_EQ((cr9346_fail & 0x20), 0);
}

// Test 3: Raw PG Packet Stream Decoding
REGISTER_TEST(Tier1_eFuse, test_efuse_pg_packet_decode) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    // Construct a raw physical PG packet byte stream
    // Packet 1: Header for block 13 (MAC address at 0x00D0 = block 13 word 0..2)
    // Offset index = 13 (0xD), word mask = words 0, 1, 2 active (low active in Realtek format: ~0x07 = 0x08)
    // Header byte = (13 << 4) | (~0x07 & 0x0F) = 0xD8
    std::vector<uint8_t> pg_stream = {
        0xD8,                         // Header
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, // 3 words (6 bytes MAC)
        0xFF                          // End of PG packets
    };

    CalibData calib;
    ASSERT_TRUE(dev.hw_decode_pg_stream(pg_stream.data(), pg_stream.size(), calib));

    uint8_t expected_mac[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    ASSERT_MEMEQ(calib.mac_addr, expected_mac, 6);
}

// Test 4: Factory Calibration Fields Extraction (Crystal, Thermal, Channel Plan)
REGISTER_TEST(Tier1_eFuse, test_efuse_calibration_fields) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    mmio.set_efuse_byte(0x00B8, 0x02); // Channel plan: FCC
    mmio.set_efuse_byte(0x00B9, 0x35); // Crystal cap trim: 0x35
    mmio.set_efuse_byte(0x00BA, 0x1E); // Thermal meter: 0x1E

    CalibData calib;
    ASSERT_TRUE(dev.hw_read_efuse(calib));

    ASSERT_EQ(calib.channel_plan, 0x02);
    ASSERT_EQ(calib.crystal_cap, 0x35);
    ASSERT_EQ(calib.thermal_meter, 0x1E);
}

// Test 5: Per-Channel TX Power Tables Extraction
REGISTER_TEST(Tier1_eFuse, test_efuse_tx_power_tables) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    // Set custom CCK TX power levels (offsets 0x10..0x15)
    uint8_t custom_cck[6] = {40, 42, 44, 44, 42, 40};
    for (int i = 0; i < 6; ++i) {
        mmio.set_efuse_byte(0x10 + i, custom_cck[i]);
    }
    // Set custom HT40 TX power levels (offsets 0x16..0x1A)
    uint8_t custom_ht40[5] = {38, 40, 42, 40, 38};
    for (int i = 0; i < 5; ++i) {
        mmio.set_efuse_byte(0x16 + i, custom_ht40[i]);
    }
    mmio.set_efuse_byte(0x1B, 0x04); // HT20 diff

    CalibData calib;
    ASSERT_TRUE(dev.hw_read_efuse(calib));

    ASSERT_MEMEQ(calib.tx_pwr_cck, custom_cck, 6);
    ASSERT_MEMEQ(calib.tx_pwr_ht40, custom_ht40, 5);
    ASSERT_EQ(calib.tx_pwr_ht20_diff, 0x04);
}

// Test 6: Fallback to Factory Defaults on Unprogrammed OTP
REGISTER_TEST(Tier1_eFuse, test_efuse_fallback_defaults) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    // Wipe eFuse to all 0xFF (unprogrammed blank state)
    for (uint16_t i = 0; i < 512; ++i) {
        mmio.set_efuse_byte(i, 0xFF);
    }

    CalibData calib;
    ASSERT_TRUE(dev.hw_read_efuse(calib));

    // Must fallback to conservative factory defaults:
    // Crystal cap = 0x20
    ASSERT_EQ(calib.crystal_cap, 0x20);
    // Thermal meter = 0x1A
    ASSERT_EQ(calib.thermal_meter, 0x1A);
    // CCK TX power = 0x2D (45)
    for (int i = 0; i < 6; ++i) {
        ASSERT_EQ(calib.tx_pwr_cck[i], 0x2D);
    }
}
