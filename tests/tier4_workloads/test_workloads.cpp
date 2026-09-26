#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/mock_crypto.hpp"
#include "../mock/mock_packet_injector.hpp"
#include "../mock/simulated_device.hpp"

using namespace rtl_mock;

namespace {

std::vector<uint8_t> create_synthetic_fw(size_t sz) {
    std::vector<uint8_t> fw(32 + sz, 0);
    fw[0] = 0x01; fw[1] = 0x53; // 0x5301
    fw[4] = 36;
    fw[14] = sz & 0xFF; fw[15] = (sz >> 8) & 0xFF;
    for (size_t i = 0; i < sz; ++i) fw[32 + i] = static_cast<uint8_t>(i ^ 0x3C);
    return fw;
}

} // namespace

// Workload 1: Full Cold Boot Bring-up Flow
REGISTER_TEST(Tier4_Workloads, test_workload_cold_bringup) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    // 1. Initial State: Card in power-down
    ASSERT_EQ(mmio.get_power_state(), POWER_CARDDIS);

    // 2. Execute Power-On Sequence (pwrseq)
    ASSERT_TRUE(dev.hw_power_on());
    ASSERT_EQ(mmio.get_power_state(), POWER_ACT);

    // 3. Upload 8051 MCU Firmware
    auto fw = create_synthetic_fw(8192); // 8KB microcode
    ASSERT_TRUE(dev.hw_download_firmware(fw.data(), fw.size()));
    ASSERT_TRUE(mmio.is_mcu_fw_ready());

    // 4. Read Factory eFuse OTP
    CalibData calib;
    ASSERT_TRUE(dev.hw_read_efuse(calib));
    uint8_t expected_mac[6] = {0x00, 0xE0, 0x4C, 0x81, 0x92, 0x23};
    ASSERT_MEMEQ(calib.mac_addr, expected_mac, 6);
    ASSERT_EQ(calib.crystal_cap, 0x28);
    ASSERT_EQ(calib.thermal_meter, 0x1A);

    // 5. Initialize TX and RX DMA Rings
    ASSERT_TRUE(dev.init_dma_rings(64, 64));
    ASSERT_NE(mmio.read32(REG_BEQ_DESA), 0);
    ASSERT_NE(mmio.read32(REG_RX_DESA), 0);

    // 6. Tune RF to Channel 6
    ASSERT_TRUE(dev.hw_set_channel(6));
    ASSERT_EQ(mmio.get_active_channel(), 6);
}

// Workload 2: Multi-Channel Network Discovery & Scan Results Cache
REGISTER_TEST(Tier4_Workloads, test_workload_scan_discovery) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.hw_power_on());
    ASSERT_TRUE(dev.init_dma_rings(32, 32));

    ASSERT_TRUE(dev.start_scan());

    // Inject 10 AP beacons across different channels
    for (uint8_t ch = 1; ch <= 10; ++ch) {
        dev.hw_set_channel(ch);

        uint8_t bssid[6] = {0x00, 0x50, 0xF2, 0x11, 0x22, ch};
        std::string ssid = "AccessPoint_" + std::to_string(ch);
        int8_t rssi = -30 - (ch * 4); // vary RSSI from -34 dBm to -70 dBm
        bool wpa2 = (ch % 2 == 0);

        auto beacon = injector.build_beacon(ssid, bssid, ch, rssi, wpa2);
        dev.process_inbound_packet(beacon.data(), beacon.size());
    }

    const auto& results = dev.get_scan_results();
    ASSERT_EQ(results.size(), 10);

    // Verify properties of discovered APs
    for (size_t i = 0; i < results.size(); ++i) {
        uint8_t expected_ch = static_cast<uint8_t>(i + 1);
        ASSERT_EQ(results[i].channel, expected_ch);
        std::string expected_ssid = "AccessPoint_" + std::to_string(expected_ch);
        ASSERT_EQ(results[i].ssid, expected_ssid);
        ASSERT_EQ(results[i].is_wpa2, (expected_ch % 2 == 0));
    }
}

// Workload 3: End-to-End Open System Association
REGISTER_TEST(Tier4_Workloads, test_workload_open_association) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.hw_power_on());
    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    // Connect to Open Network (empty passphrase)
    ASSERT_TRUE(dev.connect("PublicGuestNet", "", bssid));
    ASSERT_EQ(dev.get_state(), WIFI_AUTHENTICATING);

    // AP responds with Auth Success (Seq 2, Status 0)
    auto auth_resp = injector.build_auth_response(bssid, dev.get_mac_address(), 2, 0);
    dev.process_inbound_packet(auth_resp.data(), auth_resp.size());

    // Device should have transitioned to ASSOCIATING and sent Assoc Request
    ASSERT_EQ(dev.get_state(), WIFI_ASSOCIATING);

    // AP responds with Assoc Response (Status 0, AID 1)
    auto assoc_resp = injector.build_assoc_response(bssid, dev.get_mac_address(), 0, 1);
    dev.process_inbound_packet(assoc_resp.data(), assoc_resp.size());

    // For open network, device transitions directly to ready / 4-way stage
    ASSERT_EQ(dev.get_state(), WIFI_4WAY_HANDSHAKE);
}

// Workload 4: Complete End-to-End WPA2 Secure Connection Workflow
REGISTER_TEST(Tier4_Workloads, test_workload_wpa2_full_connect) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.hw_power_on());
    ASSERT_TRUE(dev.init_dma_rings(32, 32));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::string ssid = "Home_WiFi_Secure";
    std::string pass = "superSecretPassword987";

    // 1. Initiate Connect
    ASSERT_TRUE(dev.connect(ssid, pass, bssid));
    ASSERT_EQ(dev.get_state(), WIFI_AUTHENTICATING);

    // 2. AP Auth Response
    auto auth_resp = injector.build_auth_response(bssid, dev.get_mac_address(), 2, 0);
    dev.process_inbound_packet(auth_resp.data(), auth_resp.size());
    ASSERT_EQ(dev.get_state(), WIFI_ASSOCIATING);

    // 3. AP Assoc Response
    auto assoc_resp = injector.build_assoc_response(bssid, dev.get_mac_address(), 0, 5);
    dev.process_inbound_packet(assoc_resp.data(), assoc_resp.size());
    ASSERT_EQ(dev.get_state(), WIFI_4WAY_HANDSHAKE);

    // 4. AP EAPOL Msg 1
    uint8_t anonce[32];
    for (int i = 0; i < 32; ++i) anonce[i] = static_cast<uint8_t>(0xB0 + i);
    auto m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), anonce, 1);
    dev.process_inbound_packet(m1.data(), m1.size());

    // 5. AP EAPOL Msg 3
    uint8_t gtk[16] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    auto m3 = injector.build_eapol_m3(bssid, dev.get_mac_address(), anonce, 2,
                                     dev.get_kck(), dev.get_kek(), gtk);
    dev.process_inbound_packet(m3.data(), m3.size());

    // 6. Verification: State must be WIFI_CONNECTED with valid PTK and GTK
    ASSERT_EQ(dev.get_state(), WIFI_CONNECTED);
    ASSERT_MEMEQ(dev.get_gtk(), gtk, 16);
}

// Workload 5: Bidirectional ARP & IP Traffic Flow Over CCMP
REGISTER_TEST(Tier4_Workloads, test_workload_bidirectional_arp_ip) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.hw_power_on());
    ASSERT_TRUE(dev.init_dma_rings(32, 32));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    dev.connect("TestNet", "pass123", bssid);
    dev.set_state(WIFI_4WAY_HANDSHAKE);

    // Fast-forward to CONNECTED state
    uint8_t anonce[32] = {1};
    auto m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), anonce, 1);
    dev.process_inbound_packet(m1.data(), m1.size());
    uint8_t gtk[16] = {2};
    auto m3 = injector.build_eapol_m3(bssid, dev.get_mac_address(), anonce, 2,
                                     dev.get_kck(), dev.get_kek(), gtk);
    dev.process_inbound_packet(m3.data(), m3.size());
    ASSERT_EQ(dev.get_state(), WIFI_CONNECTED);

    // 1. Host transmits ARP Request: Who has 192.168.1.1? Tell 192.168.1.50
    uint8_t host_ip[4] = {192, 168, 1, 50};
    uint8_t gw_ip[4]   = {192, 168, 1, 1};
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t zero_mac[6]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto arp_req_body = PacketInjector::build_arp_packet(1, dev.get_mac_address(), host_ip, zero_mac, gw_ip);

    // Wrap in Ethernet II frame (DA=FF:FF:FF:FF:FF:FF, SA=host_mac, EtherType=0x0806 ARP)
    std::vector<uint8_t> eth_arp_req;
    for (int i = 0; i < 6; ++i) eth_arp_req.push_back(broadcast_mac[i]);
    for (int i = 0; i < 6; ++i) eth_arp_req.push_back(dev.get_mac_address()[i]);
    eth_arp_req.push_back(0x08);
    eth_arp_req.push_back(0x06); // ARP
    eth_arp_req.insert(eth_arp_req.end(), arp_req_body.begin(), arp_req_body.end());

    ASSERT_TRUE(dev.send_ethernet_packet(eth_arp_req.data(), eth_arp_req.size()));

    // Verify TX DMA received CCMP-encrypted 802.11 frame
    const auto& captured = dma.get_transmitted_packets();
    ASSERT_GE(captured.size(), 1);
    const auto& last_tx = captured.back();
    // 802.11 QoS(26) + CCMP(8) + LLC(8) + ARP(28) + MIC(8) + FCS(4) = 82 bytes
    ASSERT_EQ(last_tx.pktsize, 82);

    // 2. Gateway responds with inbound CCMP-encrypted ARP Reply: 192.168.1.1 is at bssid
    auto arp_reply_body = PacketInjector::build_arp_packet(2, bssid, gw_ip, dev.get_mac_address(), host_ip);
    auto rx_ccmp_frame = injector.build_ccmp_data(bssid, dev.get_mac_address(), bssid,
                                                 10, dev.get_tk(),
                                                 arp_reply_body.data(), arp_reply_body.size(),
                                                 0x0806); // ARP EtherType

    std::vector<uint8_t> received_eth;
    ASSERT_TRUE(dev.receive_ethernet_packet(rx_ccmp_frame.data(), rx_ccmp_frame.size(), received_eth));

    // Verify decrypted Ethernet frame delivered to host network interface
    ASSERT_EQ(received_eth.size(), 14 + 28); // 14 Ethernet header + 28 ARP
    // DA must be host MAC
    ASSERT_MEMEQ(received_eth.data(), dev.get_mac_address(), 6);
    // SA must be gateway MAC (bssid)
    ASSERT_MEMEQ(received_eth.data() + 6, bssid, 6);
    // EtherType must be ARP (0x0806)
    ASSERT_EQ(received_eth[12], 0x08);
    ASSERT_EQ(received_eth[13], 0x06);
    // Op must be 2 (Reply)
    ASSERT_EQ(received_eth[14 + 6], 0x00);
    ASSERT_EQ(received_eth[14 + 7], 0x02);
}

// Workload 6: Access Point Deauthentication & Reconnection Recovery
REGISTER_TEST(Tier4_Workloads, test_workload_ap_deauth_recovery) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.hw_power_on());
    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    dev.connect("FlakyNet", "pw123", bssid);
    dev.set_state(WIFI_4WAY_HANDSHAKE);

    // Bring to CONNECTED
    uint8_t anonce[32] = {1};
    auto m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), anonce, 1);
    dev.process_inbound_packet(m1.data(), m1.size());
    uint8_t gtk[16] = {2};
    auto m3 = injector.build_eapol_m3(bssid, dev.get_mac_address(), anonce, 2,
                                     dev.get_kck(), dev.get_kek(), gtk);
    dev.process_inbound_packet(m3.data(), m3.size());
    ASSERT_EQ(dev.get_state(), WIFI_CONNECTED);

    // 1. Inbound sudden Deauthentication frame from AP (Reason 2 = Previous auth not valid)
    auto deauth = injector.build_deauth(bssid, dev.get_mac_address(), 2);
    dev.process_inbound_packet(deauth.data(), deauth.size());

    // Verify teardown: state must reset to DISCONNECTED
    ASSERT_EQ(dev.get_state(), WIFI_DISCONNECTED);

    // 2. Recovery: Initiate reconnection to AP
    ASSERT_TRUE(dev.connect("FlakyNet", "pw123", bssid));
    ASSERT_EQ(dev.get_state(), WIFI_AUTHENTICATING);

    // Auth & Assoc
    auto auth_resp = injector.build_auth_response(bssid, dev.get_mac_address(), 2, 0);
    dev.process_inbound_packet(auth_resp.data(), auth_resp.size());
    auto assoc_resp = injector.build_assoc_response(bssid, dev.get_mac_address(), 0, 1);
    dev.process_inbound_packet(assoc_resp.data(), assoc_resp.size());

    // Re-handshake
    uint8_t new_anonce[32] = {9};
    auto new_m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), new_anonce, 10);
    dev.process_inbound_packet(new_m1.data(), new_m1.size());
    auto new_m3 = injector.build_eapol_m3(bssid, dev.get_mac_address(), new_anonce, 11,
                                         dev.get_kck(), dev.get_kek(), gtk);
    dev.process_inbound_packet(new_m3.data(), new_m3.size());

    // Link restored!
    ASSERT_EQ(dev.get_state(), WIFI_CONNECTED);
}
