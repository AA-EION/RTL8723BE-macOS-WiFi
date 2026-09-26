#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/mock_packet_injector.hpp"
#include "../mock/simulated_device.hpp"

using namespace rtl_mock;

// Test 1: Beacon Fixed Parameters Extraction (Timestamp, Beacon Interval, Capabilities)
REGISTER_TEST(Tier1_Beacon, test_beacon_fixed_fields) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    auto frame = injector.build_beacon("TestNet", bssid, 6, -60, true, 100);

    DiscoveredAP ap;
    ASSERT_TRUE(dev.parse_beacon_or_probe(frame.data(), frame.size(), ap));

    ASSERT_MEMEQ(ap.bssid, bssid, 6);
    ASSERT_EQ(ap.beacon_interval, 100);
    // ESS (bit 0) | Privacy (bit 4) -> 0x0011
    ASSERT_EQ(ap.capabilities & 0x0011, 0x0011);
}

// Test 2: Variable-Length SSID Information Element Extraction
REGISTER_TEST(Tier1_Beacon, test_beacon_ssid_ie) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    uint8_t bssid[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    std::string test_ssid = "Corporate_Secure_5G";
    auto frame = injector.build_beacon(test_ssid, bssid, 11);

    DiscoveredAP ap;
    ASSERT_TRUE(dev.parse_beacon_or_probe(frame.data(), frame.size(), ap));

    ASSERT_EQ(ap.ssid, test_ssid);
}

// Test 3: Supported Rates Information Element Extraction
REGISTER_TEST(Tier1_Beacon, test_beacon_rates_ie) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    auto frame = injector.build_beacon("RateTest", bssid, 1);

    // Verify Tag 1 exists in frame bytes
    bool found_tag1 = false;
    for (size_t i = 36; i + 1 < frame.size(); ++i) {
        if (frame[i] == 0x01 && frame[i + 1] == 8) {
            found_tag1 = true;
            // First basic rate is 0x82 (1 Mbps BSSBasicRate)
            ASSERT_EQ(frame[i + 2], 0x82);
            break;
        }
    }
    ASSERT_TRUE(found_tag1);
}

// Test 4: DS Parameter Set / Channel Extraction (Channels 1–13)
REGISTER_TEST(Tier1_Beacon, test_beacon_channel_ie) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    for (uint8_t ch = 1; ch <= 13; ++ch) {
        auto frame = injector.build_beacon("ChTest", bssid, ch);
        DiscoveredAP ap;
        ASSERT_TRUE(dev.parse_beacon_or_probe(frame.data(), frame.size(), ap));
        ASSERT_EQ(ap.channel, ch);
    }
}

// Test 5: WPA2 RSN Information Element Parsing (Tag 48)
REGISTER_TEST(Tier1_Beacon, test_beacon_rsn_ie_wpa2) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    // WPA2 beacon
    auto wpa2_frame = injector.build_beacon("SecureNet", bssid, 6, -50, true);
    DiscoveredAP wpa2_ap;
    ASSERT_TRUE(dev.parse_beacon_or_probe(wpa2_frame.data(), wpa2_frame.size(), wpa2_ap));
    ASSERT_TRUE(wpa2_ap.is_wpa2);

    // Open (unsecured) beacon
    auto open_frame = injector.build_beacon("OpenNet", bssid, 6, -50, false);
    DiscoveredAP open_ap;
    ASSERT_TRUE(dev.parse_beacon_or_probe(open_frame.data(), open_frame.size(), open_ap));
    ASSERT_FALSE(open_ap.is_wpa2);
}

// Test 6: Hidden / Broadcast SSID Handling (Zero-Length Tag)
REGISTER_TEST(Tier1_Beacon, test_beacon_hidden_ssid) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    // Empty SSID string
    auto frame = injector.build_beacon("", bssid, 6);

    DiscoveredAP ap;
    ASSERT_TRUE(dev.parse_beacon_or_probe(frame.data(), frame.size(), ap));
    ASSERT_TRUE(ap.ssid.empty());
    ASSERT_MEMEQ(ap.bssid, bssid, 6);
}
