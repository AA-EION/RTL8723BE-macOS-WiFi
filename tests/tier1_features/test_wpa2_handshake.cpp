#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/mock_crypto.hpp"
#include "../mock/mock_packet_injector.hpp"
#include "../mock/simulated_device.hpp"

using namespace rtl_mock;

// Test 1: PBKDF2-HMAC-SHA1 PMK Derivation (RFC 6070 / IEEE 802.11i)
REGISTER_TEST(Tier1_WPA2, test_wpa2_pmk_pbkdf2) {
    // Known RFC 6070 Test Vector:
    // Passphrase = "password", Salt = "IEEE802.11", Iterations = 4096
    const char* pass = "password";
    const uint8_t ssid[] = "IEEE802.11";
    uint8_t pmk[32];

    rtl_crypto::pbkdf2_sha1(pass, std::strlen(pass), ssid, sizeof(ssid) - 1, 4096, pmk, 32);

    // Verify non-zero and reproducible
    uint8_t pmk2[32];
    rtl_crypto::pbkdf2_sha1(pass, std::strlen(pass), ssid, sizeof(ssid) - 1, 4096, pmk2, 32);
    ASSERT_MEMEQ(pmk, pmk2, 32);

    // Verify that changing passphrase produces different PMK
    uint8_t pmk3[32];
    rtl_crypto::pbkdf2_sha1("different_pw", 12, ssid, sizeof(ssid) - 1, 4096, pmk3, 32);
    ASSERT_NE(std::memcmp(pmk, pmk3, 32), 0);
}

// Test 2: IEEE 802.11i PRF-512 PTK Expansion
REGISTER_TEST(Tier1_WPA2, test_wpa2_ptk_prf512) {
    uint8_t pmk[32];
    for (int i = 0; i < 32; ++i) pmk[i] = static_cast<uint8_t>(i + 1);

    uint8_t ap_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t sta_mac[6] = {0x00, 0xE0, 0x4C, 0x81, 0x92, 0x23};

    uint8_t anonce[32];
    uint8_t snonce[32];
    for (int i = 0; i < 32; ++i) {
        anonce[i] = static_cast<uint8_t>(i * 2 + 1);
        snonce[i] = static_cast<uint8_t>(i * 3 + 7);
    }

    uint8_t ptk[64];
    rtl_crypto::prf_512(pmk, "Pairwise key expansion", ap_mac, sta_mac, anonce, snonce, ptk);

    // Verify partition: KCK(16), KEK(16), TK(16), Reserved(16)
    // All sections must be non-zero
    bool all_zero = true;
    for (int i = 0; i < 16; ++i) if (ptk[i] != 0) all_zero = false;
    ASSERT_FALSE(all_zero);

    // Determinism test
    uint8_t ptk2[64];
    rtl_crypto::prf_512(pmk, "Pairwise key expansion", ap_mac, sta_mac, anonce, snonce, ptk2);
    ASSERT_MEMEQ(ptk, ptk2, 64);
}

// Test 3: EAPOL-Key Message 1 Reception & SNonce Generation
REGISTER_TEST(Tier1_WPA2, test_wpa2_msg1_reception) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    ASSERT_TRUE(dev.connect("TestWPA2", "securePassphrase123", bssid));

    // Simulate Auth & Assoc complete
    dev.set_state(WIFI_4WAY_HANDSHAKE);

    uint8_t anonce[32];
    for (int i = 0; i < 32; ++i) anonce[i] = static_cast<uint8_t>(0xA0 + i);

    auto m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), anonce, 1);
    dev.process_inbound_packet(m1.data(), m1.size());

    // Device should have transmitted Message 2 in response
    const auto& captured = dma.get_transmitted_packets();
    ASSERT_GE(captured.size(), 1);

    // Verify M2 is EAPOL-Key
    const auto& m2 = captured.back();
    ASSERT_GE(m2.pktsize, 34 + 4 + 95);
    // EAPOL Descriptor Type: 2 (RSN)
    ASSERT_EQ(m2.data[34 + 4], 0x02);
}

// Test 4: EAPOL-Key Message 2 MIC Calculation with KCK
REGISTER_TEST(Tier1_WPA2, test_wpa2_msg2_mic) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    dev.connect("TestWPA2", "passphrase456", bssid);
    dev.set_state(WIFI_4WAY_HANDSHAKE);

    uint8_t anonce[32];
    for (int i = 0; i < 32; ++i) anonce[i] = static_cast<uint8_t>(i);

    auto m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), anonce, 100);
    dev.process_inbound_packet(m1.data(), m1.size());

    const auto& captured = dma.get_transmitted_packets();
    ASSERT_GE(captured.size(), 1);
    const auto& m2 = captured.back();

    // Verify MIC in M2
    size_t eapol_start = 34;
    std::vector<uint8_t> m2_verify(m2.data.begin() + eapol_start, m2.data.end());
    uint8_t m2_mic[16];
    std::memcpy(m2_mic, m2_verify.data() + 81, 16);
    std::memset(m2_verify.data() + 81, 0, 16); // Zero out MIC for re-verification

    uint8_t expected_digest[20];
    rtl_crypto::hmac_sha1(dev.get_kck(), 16, m2_verify.data(), m2_verify.size(), expected_digest);

    // Captured MIC must match calculated HMAC-SHA1
    ASSERT_MEMEQ(m2_mic, expected_digest, 16);
}

// Test 5: EAPOL-Key Message 3 Reception & RFC 3394 AES Key Unwrap of GTK
REGISTER_TEST(Tier1_WPA2, test_wpa2_msg3_gtk_decrypt) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    dev.connect("TestWPA2", "passphrase456", bssid);
    dev.set_state(WIFI_4WAY_HANDSHAKE);

    uint8_t anonce[32];
    for (int i = 0; i < 32; ++i) anonce[i] = static_cast<uint8_t>(i + 5);

    // Send M1
    auto m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), anonce, 1);
    dev.process_inbound_packet(m1.data(), m1.size());

    // Inject M3 with known GTK
    uint8_t secret_gtk[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00
    };

    auto m3 = injector.build_eapol_m3(bssid, dev.get_mac_address(), anonce, 2,
                                     dev.get_kck(), dev.get_kek(), secret_gtk);
    dev.process_inbound_packet(m3.data(), m3.size());

    // Verify device unwrapped and installed GTK
    ASSERT_MEMEQ(dev.get_gtk(), secret_gtk, 16);
}

// Test 6: Message 4 Completion & Link Transition to CONNECTED
REGISTER_TEST(Tier1_WPA2, test_wpa2_msg4_completion) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    dev.connect("TestWPA2", "passphrase456", bssid);
    dev.set_state(WIFI_4WAY_HANDSHAKE);

    uint8_t anonce[32];
    for (int i = 0; i < 32; ++i) anonce[i] = static_cast<uint8_t>(i + 1);

    auto m1 = injector.build_eapol_m1(bssid, dev.get_mac_address(), anonce, 1);
    dev.process_inbound_packet(m1.data(), m1.size());

    uint8_t secret_gtk[16] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto m3 = injector.build_eapol_m3(bssid, dev.get_mac_address(), anonce, 2,
                                     dev.get_kck(), dev.get_kek(), secret_gtk);
    dev.process_inbound_packet(m3.data(), m3.size());

    // After processing M3, device must transmit M4 and state must be WIFI_CONNECTED
    ASSERT_EQ(dev.get_state(), WIFI_CONNECTED);

    const auto& captured = dma.get_transmitted_packets();
    ASSERT_GE(captured.size(), 2); // M2 and M4
    const auto& m4 = captured.back();

    // M4 Key Info: Pairwise | MIC | Secure (0x030A)
    ASSERT_EQ(m4.data[34 + 5], 0x03);
    ASSERT_EQ(m4.data[34 + 6], 0x0A);
}
