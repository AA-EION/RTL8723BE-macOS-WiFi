#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/mock_crypto.hpp"
#include "../mock/mock_packet_injector.hpp"
#include "../mock/simulated_device.hpp"

using namespace rtl_mock;

// Test 1: CCMP Header Layout & 48-bit Packet Number (PN) Extraction
REGISTER_TEST(Tier1_CCMP, test_ccmp_header_and_pn) {
    uint64_t test_pn = 0x112233445566ULL;
    uint8_t ccmp_hdr[8];

    // Format per IEEE 802.11-2016 §12.5.3.3
    ccmp_hdr[0] = static_cast<uint8_t>(test_pn & 0xFF);         // PN0 (0x66)
    ccmp_hdr[1] = static_cast<uint8_t>((test_pn >> 8) & 0xFF);  // PN1 (0x55)
    ccmp_hdr[2] = 0x00;                                         // Reserved
    ccmp_hdr[3] = 0x20;                                         // ExtIV=1, KeyID=0
    ccmp_hdr[4] = static_cast<uint8_t>((test_pn >> 16) & 0xFF); // PN2 (0x44)
    ccmp_hdr[5] = static_cast<uint8_t>((test_pn >> 24) & 0xFF); // PN3 (0x33)
    ccmp_hdr[6] = static_cast<uint8_t>((test_pn >> 32) & 0xFF); // PN4 (0x22)
    ccmp_hdr[7] = static_cast<uint8_t>((test_pn >> 40) & 0xFF); // PN5 (0x11)

    // Reconstruct PN
    uint64_t recovered_pn = static_cast<uint64_t>(ccmp_hdr[0]) |
                           (static_cast<uint64_t>(ccmp_hdr[1]) << 8) |
                           (static_cast<uint64_t>(ccmp_hdr[4]) << 16) |
                           (static_cast<uint64_t>(ccmp_hdr[5]) << 24) |
                           (static_cast<uint64_t>(ccmp_hdr[6]) << 32) |
                           (static_cast<uint64_t>(ccmp_hdr[7]) << 40);

    ASSERT_EQ(recovered_pn, test_pn);
    ASSERT_EQ(ccmp_hdr[3] & 0x20, 0x20); // ExtIV flag verified
}

// Test 2: AES-128 CCM 13-Byte Nonce Construction
REGISTER_TEST(Tier1_CCMP, test_ccmp_nonce_construction) {
    uint8_t priority = 0x00;
    uint8_t addr2[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint64_t pn = 0x010203040506ULL;

    uint8_t nonce[13];
    nonce[0] = priority;
    for (int i = 0; i < 6; ++i) nonce[1 + i] = addr2[i];
    for (int i = 0; i < 6; ++i) {
        nonce[7 + i] = static_cast<uint8_t>((pn >> ((5 - i) * 8)) & 0xFF);
    }

    ASSERT_EQ(nonce[0], 0x00);
    ASSERT_MEMEQ(nonce + 1, addr2, 6);
    // Big-endian PN check: 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
    uint8_t expected_pn_be[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    ASSERT_MEMEQ(nonce + 7, expected_pn_be, 6);
}

// Test 3: Additional Authentication Data (AAD) Masking per IEEE 802.11i §12.5.3.3.2
REGISTER_TEST(Tier1_CCMP, test_ccmp_aad_construction) {
    // 26-byte MAC header with QoS Control
    uint8_t raw_mac_header[26] = {
        0x88, 0x02,             // Frame Control: QoS Data, From DS=1
        0x3A, 0x01,             // Duration (masked)
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, // DA
        0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, // BSSID
        0x00, 0x66, 0x77, 0x88, 0x99, 0x00, // SA
        0x70, 0x12,             // SeqCtrl (masked)
        0x05, 0x00              // QoS Control: TID 5 (bits 4..7 masked)
    };

    uint8_t aad[26];
    std::memcpy(aad, raw_mac_header, 26);

    // Apply 802.11 AAD masking rules:
    aad[0] &= 0x8F; // Mask subtype bits 4, 5, 6
    aad[1] &= 0xC7; // Mask Retry, PwrMgmt, MoreData
    aad[22] = 0x00; // Sequence control masked to 0
    aad[23] = 0x00;
    aad[24] &= 0x0F; // QoS TID preserved, high nibble masked
    aad[25] = 0x00;

    ASSERT_EQ(aad[22], 0x00);
    ASSERT_EQ(aad[23], 0x00);
    ASSERT_EQ(aad[24], 0x05); // TID 5 preserved
    ASSERT_EQ(aad[25], 0x00);
}

// Test 4: RFC 3610 / IEEE 802.11 CCMP Encryption Test Vector
REGISTER_TEST(Tier1_CCMP, test_ccmp_encrypt_vector) {
    // Official RFC 3610 Test Vector #1
    uint8_t key[16] = {
        0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
        0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF
    };

    uint8_t nonce[13] = {
        0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0xA0,
        0xA1, 0xA2, 0xA3, 0xA4, 0xA5
    };

    uint8_t aad[8] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    };

    uint8_t plaintext[23] = {
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E
    };

    uint8_t expected_cipher[23] = {
        0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2,
        0xF0, 0x66, 0xD0, 0xC2, 0xC0, 0xF9, 0x89, 0x80,
        0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3, 0x84
    };

    uint8_t expected_mic[8] = {
        0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0
    };

    uint8_t actual_cipher[23];
    uint8_t actual_mic[8];

    ASSERT_TRUE(rtl_crypto::ccmp_encrypt(key, nonce, aad, sizeof(aad),
                                        plaintext, sizeof(plaintext),
                                        actual_cipher, actual_mic));

    ASSERT_MEMEQ(actual_cipher, expected_cipher, sizeof(expected_cipher));
    ASSERT_MEMEQ(actual_mic, expected_mic, sizeof(expected_mic));
}

// Test 5: RFC 3610 / IEEE 802.11 CCMP Decryption & MIC Validation
REGISTER_TEST(Tier1_CCMP, test_ccmp_decrypt_vector) {
    uint8_t key[16] = {
        0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
        0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF
    };

    uint8_t nonce[13] = {
        0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0xA0,
        0xA1, 0xA2, 0xA3, 0xA4, 0xA5
    };

    uint8_t aad[8] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    };

    uint8_t ciphertext[23] = {
        0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2,
        0xF0, 0x66, 0xD0, 0xC2, 0xC0, 0xF9, 0x89, 0x80,
        0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3, 0x84
    };

    uint8_t mic[8] = {
        0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0
    };

    uint8_t expected_plain[23] = {
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E
    };

    uint8_t actual_plain[23];
    ASSERT_TRUE(rtl_crypto::ccmp_decrypt(key, nonce, aad, sizeof(aad),
                                        ciphertext, sizeof(ciphertext),
                                        mic, actual_plain));

    ASSERT_MEMEQ(actual_plain, expected_plain, sizeof(expected_plain));
}

// Test 6: CCMP Packet Number (PN) Replay Attack Detection
REGISTER_TEST(Tier1_CCMP, test_ccmp_replay_detection) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    dev.connect("TestWPA2", "passphrase456", bssid);

    // Advance device state to CONNECTED
    dev.set_state(WIFI_CONNECTED);

    uint8_t payload[20] = "Ping Data Packet";

    // 1. Inbound packet with PN = 5: Should succeed
    auto frame1 = injector.build_ccmp_data(bssid, dev.get_mac_address(), bssid,
                                          5, dev.get_tk(), payload, sizeof(payload));
    std::vector<uint8_t> eth1;
    ASSERT_TRUE(dev.receive_ethernet_packet(frame1.data(), frame1.size(), eth1));
    ASSERT_EQ(dev.get_rx_pn(), 5);

    // 2. Inbound packet with PN = 5 again (replay): Must be rejected
    std::vector<uint8_t> eth_replayed;
    ASSERT_FALSE(dev.receive_ethernet_packet(frame1.data(), frame1.size(), eth_replayed));

    // 3. Inbound packet with stale PN = 4 (less than current 5): Must be rejected
    auto frame_stale = injector.build_ccmp_data(bssid, dev.get_mac_address(), bssid,
                                               4, dev.get_tk(), payload, sizeof(payload));
    std::vector<uint8_t> eth_stale;
    ASSERT_FALSE(dev.receive_ethernet_packet(frame_stale.data(), frame_stale.size(), eth_stale));

    // 4. Inbound packet with strictly increasing PN = 6: Should succeed
    auto frame_fresh = injector.build_ccmp_data(bssid, dev.get_mac_address(), bssid,
                                               6, dev.get_tk(), payload, sizeof(payload));
    std::vector<uint8_t> eth_fresh;
    ASSERT_TRUE(dev.receive_ethernet_packet(frame_fresh.data(), frame_fresh.size(), eth_fresh));
    ASSERT_EQ(dev.get_rx_pn(), 6);
}
