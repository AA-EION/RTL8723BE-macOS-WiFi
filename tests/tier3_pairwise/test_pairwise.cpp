#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/mock_crypto.hpp"
#include "../mock/mock_packet_injector.hpp"
#include "../mock/simulated_device.hpp"

using namespace rtl_mock;

// Pairwise Test 1: Active 2.4 GHz Channel Scan During Ongoing Background TX DMA
REGISTER_TEST(Tier3_Pairwise, test_pair_scan_during_tx_dma) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.hw_power_on());
    ASSERT_TRUE(dev.init_dma_rings(32, 32));
    dev.set_state(WIFI_CONNECTED);

    // Start channel scan while sending packets
    ASSERT_TRUE(dev.start_scan());
    ASSERT_EQ(dev.get_state(), WIFI_SCANNING);

    // Stream 10 background data packets interleaved with channel hopping and beacon reception
    for (uint8_t ch = 1; ch <= 10; ++ch) {
        // Channel hop
        ASSERT_TRUE(dev.hw_set_channel(ch));

        // Inject beacon discovered on this channel
        uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, ch};
        std::string ssid = "SSID_Ch" + std::to_string(ch);
        auto beacon = injector.build_beacon(ssid, bssid, ch);
        dev.process_inbound_packet(beacon.data(), beacon.size());

        // Background transmit packet
        uint8_t tx_data[48] = {ch};
        ASSERT_TRUE(dev.transmit_raw_frame(Q_BE, tx_data, sizeof(tx_data)));
    }

    // Verify all 10 APs were discovered in the scan cache
    const auto& results = dev.get_scan_results();
    ASSERT_EQ(results.size(), 10);

    // Verify all 10 background packets were transmitted without dropping
    const auto& captured = dma.get_transmitted_packets();
    ASSERT_EQ(captured.size(), 10);
}

// Pairwise Test 2: WPA2 GTK Rekeying Under Active Packet Bursts
REGISTER_TEST(Tier3_Pairwise, test_pair_rekey_during_traffic) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(32, 32));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    dev.connect("RekeyNet", "testPass123", bssid);
    dev.set_state(WIFI_4WAY_HANDSHAKE);
    dma.clear_transmitted_packets();

    // Complete initial handshake
    uint8_t anonce[32] = {1};
    auto m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), anonce, 1);
    dev.process_inbound_packet(m1.data(), m1.size());

    uint8_t initial_gtk[16] = {0x11, 0x11, 0x11, 0x11};
    auto m3 = injector.build_eapol_m3(bssid, dev.get_mac_address(), anonce, 2,
                                     dev.get_kck(), dev.get_kek(), initial_gtk);
    dev.process_inbound_packet(m3.data(), m3.size());
    ASSERT_EQ(dev.get_state(), WIFI_CONNECTED);
    ASSERT_MEMEQ(dev.get_gtk(), initial_gtk, 16);

    // Send 5 data packets under original keys
    for (int i = 0; i < 5; ++i) {
        uint8_t dummy_eth[64] = {0};
        dummy_eth[12] = 0x08; // IPv4
        ASSERT_TRUE(dev.send_ethernet_packet(dummy_eth, sizeof(dummy_eth)));
    }

    // Now AP initiates GTK rekey (EAPOL Group Key Handshake Msg 1/Msg 3)
    uint8_t rekey_gtk[16] = {0x99, 0x88, 0x77, 0x66};
    auto rekey_m3 = injector.build_eapol_m3(bssid, dev.get_mac_address(), anonce, 3,
                                           dev.get_kck(), dev.get_kek(), rekey_gtk);
    dev.process_inbound_packet(rekey_m3.data(), rekey_m3.size());

    // Verify new GTK is active
    ASSERT_MEMEQ(dev.get_gtk(), rekey_gtk, 16);
    ASSERT_EQ(dev.get_state(), WIFI_CONNECTED);

    // Send 5 more packets post-rekey
    for (int i = 0; i < 5; ++i) {
        uint8_t dummy_eth[64] = {1};
        dummy_eth[12] = 0x08;
        ASSERT_TRUE(dev.send_ethernet_packet(dummy_eth, sizeof(dummy_eth)));
    }

    // Total packets transmitted = 1 (M2) + 1 (M4) + 5 (data) + 1 (rekey M4) + 5 (data) = 13
    ASSERT_EQ(dma.get_transmitted_packets().size(), 13);
}

// Pairwise Test 3: Antenna Switching Under Heavy Bidirectional Traffic
REGISTER_TEST(Tier3_Pairwise, test_pair_antenna_switch_load) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(32, 32));
    dev.set_state(WIFI_CONNECTED);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    // Verify default Main antenna
    ASSERT_EQ(mmio.get_active_antenna(), 1);

    // Interleave packet streaming with switching to Aux antenna (2) and back to Main (1)
    for (int i = 0; i < 10; ++i) {
        uint8_t ant = (i % 2 == 0) ? 2 : 1;
        ASSERT_TRUE(dev.hw_set_antenna(ant));
        ASSERT_EQ(mmio.get_active_antenna(), ant);

        // Transmit packet
        uint8_t eth_tx[60] = {static_cast<uint8_t>(i)};
        eth_tx[12] = 0x08;
        ASSERT_TRUE(dev.send_ethernet_packet(eth_tx, sizeof(eth_tx)));

        // Receive packet
        uint8_t payload[20] = {static_cast<uint8_t>(i + 10)};
        auto rx_frame = injector.build_ccmp_data(bssid, dev.get_mac_address(), bssid,
                                                100 + i, dev.get_tk(), payload, sizeof(payload));
        std::vector<uint8_t> eth_rx;
        ASSERT_TRUE(dev.receive_ethernet_packet(rx_frame.data(), rx_frame.size(), eth_rx));
    }

    ASSERT_EQ(dma.get_transmitted_packets().size(), 10);
    ASSERT_EQ(dev.get_rx_pn(), 109);
}

// Pairwise Test 4: Host Suspend / Power-Down with Active DMA
REGISTER_TEST(Tier3_Pairwise, test_pair_suspend_dma_active) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.hw_power_on());
    ASSERT_TRUE(dev.init_dma_rings(16, 16));
    dev.set_state(WIFI_CONNECTED);

    // Queue packets in TX ring
    uint8_t dummy[64] = {0};
    ASSERT_TRUE(dev.transmit_raw_frame(Q_BE, dummy, sizeof(dummy)));

    // Transition power to CARDDIS (host sleep/suspend)
    ASSERT_TRUE(dev.hw_power_off());
    ASSERT_EQ(dev.get_state(), WIFI_DISCONNECTED);
    ASSERT_EQ(mmio.get_power_state(), POWER_CARDDIS);

    // Attempting to send while suspended should fail
    ASSERT_FALSE(dev.send_ethernet_packet(dummy, sizeof(dummy)));
}

// Pairwise Test 5: Concurrent Multicast/Broadcast Reception Mixed with Unicast CCMP
REGISTER_TEST(Tier3_Pairwise, test_pair_multicast_unicast_mix) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(32, 32));
    dev.set_state(WIFI_CONNECTED);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    for (int i = 0; i < 5; ++i) {
        // 1. Broadcast beacon
        auto beacon = injector.build_beacon("TestMix", bssid, 6);
        dev.process_inbound_packet(beacon.data(), beacon.size());

        // 2. Unicast CCMP packet
        uint8_t payload[16] = {static_cast<uint8_t>(i)};
        auto unicast = injector.build_ccmp_data(bssid, dev.get_mac_address(), bssid,
                                               200 + i, dev.get_tk(), payload, sizeof(payload));
        std::vector<uint8_t> eth_out;
        ASSERT_TRUE(dev.receive_ethernet_packet(unicast.data(), unicast.size(), eth_out));
        ASSERT_EQ(dev.get_rx_pn(), 200 + i);
    }

    ASSERT_EQ(dev.get_scan_results().size(), 1);
}
