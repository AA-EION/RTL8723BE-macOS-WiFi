#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/simulated_device.hpp"
#include <vector>
#include <cstring>

using namespace rtl_mock;

namespace {

// Helper to construct a synthetic valid firmware buffer
std::vector<uint8_t> create_test_firmware(size_t payload_size, uint16_t sig = 0x5301, uint16_t version = 36) {
    std::vector<uint8_t> fw(32 + payload_size);
    // Signature
    fw[0] = sig & 0xFF;
    fw[1] = (sig >> 8) & 0xFF;
    fw[2] = 0x10; // Category
    fw[3] = 0x00; // Function
    fw[4] = version & 0xFF;
    fw[5] = (version >> 8) & 0xFF;
    fw[6] = 0x01; // Subversion
    fw[10] = 0x09; // Month
    fw[11] = 0x1A; // Date
    fw[12] = 0x0C; // Hour
    fw[13] = 0x1E; // Minute
    fw[14] = payload_size & 0xFF;
    fw[15] = (payload_size >> 8) & 0xFF;

    // Fill payload with deterministic microcode
    for (size_t i = 0; i < payload_size; ++i) {
        fw[32 + i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    }
    return fw;
}

} // namespace

// Test 1: Valid Firmware Header Verification
REGISTER_TEST(Tier1_FW, test_fw_valid_header) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.hw_power_on());

    // 8KB payload (2 pages)
    auto fw = create_test_firmware(8192, 0x5301, 36);
    ASSERT_TRUE(dev.hw_download_firmware(fw.data(), fw.size()));
    ASSERT_TRUE(mmio.is_mcu_fw_ready());

    // Verify downloaded RAM size in mock MMIO
    ASSERT_EQ(mmio.get_downloaded_fw_ram().size(), 8192);
}

// Test 2: Page-by-Page RAM Download Across Multiple Pages
REGISTER_TEST(Tier1_FW, test_fw_page_download) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.hw_power_on());

    // 16KB payload = 4 pages of 4096 bytes each
    auto fw = create_test_firmware(16384, 0x5301, 36);
    ASSERT_TRUE(dev.hw_download_firmware(fw.data(), fw.size()));

    const auto& downloaded = mmio.get_downloaded_fw_ram();
    ASSERT_EQ(downloaded.size(), 16384);

    // Verify byte fidelity for each page
    for (size_t p = 0; p < 4; ++p) {
        size_t offset = p * 4096;
        for (size_t i = 0; i < 4096; ++i) {
            uint8_t expected = fw[32 + offset + i];
            uint8_t actual = downloaded[offset + i];
            ASSERT_EQ(expected, actual);
        }
    }
}

// Test 3: MCU Checksum Handshake Failure Simulation
REGISTER_TEST(Tier1_FW, test_fw_checksum_failure) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.hw_power_on());

    // Force mock MMIO to fail checksum report
    mmio.set_force_checksum_failure(true);

    auto fw = create_test_firmware(4096, 0x5301, 36);
    bool result = dev.hw_download_firmware(fw.data(), fw.size());
    // Should fail and reject firmware
    ASSERT_FALSE(result);
    ASSERT_FALSE(mmio.is_mcu_fw_ready());
}

// Test 4: MCU Readiness and Handshake Status Verification
REGISTER_TEST(Tier1_FW, test_fw_ready_handshake) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.hw_power_on());

    // Check register state before download
    uint32_t status_before = mmio.read32(REG_MCUFWDL);
    ASSERT_EQ((status_before & MCUFWDL_WINTINI_RDY), 0);

    auto fw = create_test_firmware(4096, 0x5301, 36);
    ASSERT_TRUE(dev.hw_download_firmware(fw.data(), fw.size()));

    // Check register state after download: WINTINI_RDY must be set
    uint32_t status_after = mmio.read32(REG_MCUFWDL);
    ASSERT_NE((status_after & MCUFWDL_WINTINI_RDY), 0);
    ASSERT_TRUE(mmio.is_mcu_fw_ready());
}

// Test 5: Firmware Self-Reset Sequence on Running Controller
REGISTER_TEST(Tier1_FW, test_fw_self_reset) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.hw_power_on());

    // Simulate MCU running with FW_RESET bit set in REG_MCUFWDL
    mmio.write32(REG_MCUFWDL, MCUFWDL_FW_RESET | MCUFWDL_WINTINI_RDY);
    ASSERT_TRUE((mmio.read32(REG_MCUFWDL) & MCUFWDL_FW_RESET) != 0);

    // Downloading new firmware should cleanly self-reset and complete
    auto fw = create_test_firmware(4096, 0x5301, 36);
    ASSERT_TRUE(dev.hw_download_firmware(fw.data(), fw.size()));
    ASSERT_TRUE(mmio.is_mcu_fw_ready());
}

// Test 6: Page Overflow Rejection (> 8 Pages)
REGISTER_TEST(Tier1_FW, test_fw_page_overflow) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.hw_power_on());

    // 9 pages = 9 * 4096 = 36864 bytes
    auto fw = create_test_firmware(36864, 0x5301, 36);
    bool result = dev.hw_download_firmware(fw.data(), fw.size());
    // Max page count is 8, 9 pages must be rejected
    ASSERT_FALSE(result);
}
