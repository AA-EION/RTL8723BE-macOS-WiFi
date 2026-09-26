#include "mock_packet_injector.hpp"
#include <cstring>

namespace rtl_mock {

PacketInjector::PacketInjector(MockDMA& dma) : dma_(dma) {}

bool PacketInjector::inject_frame(const std::vector<uint8_t>& frame,
                                 bool crc_err, bool icv_err, uint8_t shift) {
    return dma_.inject_rx_frame(frame.data(), frame.size(), crc_err, icv_err, shift);
}

std::vector<uint8_t> PacketInjector::build_beacon(const std::string& ssid,
                                                 const uint8_t bssid[6],
                                                 uint8_t channel,
                                                 int8_t /*rssi*/,
                                                 bool is_wpa2,
                                                 uint16_t beacon_int) {
    std::vector<uint8_t> frame;

    // 1. MAC Header (24 bytes)
    frame.push_back(0x80); // Frame Control: Subtype 8 (Beacon), Type 0 (Mgmt)
    frame.push_back(0x00);
    frame.push_back(0x00); // Duration
    frame.push_back(0x00);
    // Addr1 (DA): Broadcast FF:FF:FF:FF:FF:FF
    for (int i = 0; i < 6; ++i) frame.push_back(0xFF);
    // Addr2 (SA): BSSID
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);
    // Addr3 (BSSID): BSSID
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);
    // Sequence Control
    frame.push_back(0x00);
    frame.push_back(0x00);

    // 2. Fixed Parameters (12 bytes)
    // Timestamp (8 bytes)
    for (int i = 0; i < 8; ++i) frame.push_back(static_cast<uint8_t>(i + 1));
    // Beacon Interval (2 bytes)
    frame.push_back(static_cast<uint8_t>(beacon_int & 0xFF));
    frame.push_back(static_cast<uint8_t>((beacon_int >> 8) & 0xFF));
    // Capability Info (2 bytes): ESS (bit 0) | Privacy (bit 4 if WPA2)
    uint16_t caps = 0x0001;
    if (is_wpa2) caps |= 0x0010;
    frame.push_back(static_cast<uint8_t>(caps & 0xFF));
    frame.push_back(static_cast<uint8_t>((caps >> 8) & 0xFF));

    // 3. Information Elements (IEs)
    // Tag 0: SSID
    frame.push_back(0x00);
    frame.push_back(static_cast<uint8_t>(ssid.length()));
    for (char c : ssid) frame.push_back(static_cast<uint8_t>(c));

    // Tag 1: Supported Rates
    uint8_t rates[] = {0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};
    frame.push_back(0x01);
    frame.push_back(sizeof(rates));
    for (uint8_t r : rates) frame.push_back(r);

    // Tag 3: DS Parameter Set (Channel)
    frame.push_back(0x03);
    frame.push_back(0x01);
    frame.push_back(channel);

    // Tag 48 (0x30): RSN IE (WPA2)
    if (is_wpa2) {
        uint8_t rsn_ie[] = {
            0x30, 20,           // Tag 48, Length 20
            0x01, 0x00,         // Version 1
            0x00, 0x0F, 0xAC, 0x04, // Group Cipher: CCMP
            0x01, 0x00,         // Pairwise Cipher Count: 1
            0x00, 0x0F, 0xAC, 0x04, // Pairwise Cipher: CCMP
            0x01, 0x00,         // AKM Suite Count: 1
            0x00, 0x0F, 0xAC, 0x02, // AKM Suite: PSK
            0x00, 0x00          // RSN Capabilities
        };
        for (uint8_t b : rsn_ie) frame.push_back(b);
    }

    // 4. 802.11 FCS (CRC32)
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    return frame;
}

std::vector<uint8_t> PacketInjector::build_probe_response(const std::string& ssid,
                                                          const uint8_t bssid[6],
                                                          const uint8_t dest_mac[6],
                                                          uint8_t channel,
                                                          int8_t rssi,
                                                          bool is_wpa2) {
    auto frame = build_beacon(ssid, bssid, channel, rssi, is_wpa2);
    // Adjust Frame Control to Probe Response (Subtype 5 = 0x50)
    frame[0] = 0x50;
    // Replace DA with dest_mac
    for (int i = 0; i < 6; ++i) {
        frame[4 + i] = dest_mac[i];
    }
    // Recompute CRC32
    size_t payload_len = frame.size() - 4;
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), payload_len);
    frame[payload_len] = static_cast<uint8_t>(crc & 0xFF);
    frame[payload_len + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    frame[payload_len + 2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    frame[payload_len + 3] = static_cast<uint8_t>((crc >> 24) & 0xFF);

    return frame;
}

std::vector<uint8_t> PacketInjector::build_auth_response(const uint8_t bssid[6],
                                                         const uint8_t dest_mac[6],
                                                         uint16_t seq,
                                                         uint16_t status) {
    std::vector<uint8_t> frame;
    // Frame Control: Subtype 11 (Auth = 0xB0)
    frame.push_back(0xB0);
    frame.push_back(0x00);
    frame.push_back(0x00); // Duration
    frame.push_back(0x00);
    for (int i = 0; i < 6; ++i) frame.push_back(dest_mac[i]); // DA
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);    // SA
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);    // BSSID
    frame.push_back(0x00);
    frame.push_back(0x00);

    // Auth Algorithm: 0 (Open System)
    frame.push_back(0x00);
    frame.push_back(0x00);
    // Auth Seq: (seq)
    frame.push_back(static_cast<uint8_t>(seq & 0xFF));
    frame.push_back(static_cast<uint8_t>((seq >> 8) & 0xFF));
    // Status Code: (status)
    frame.push_back(static_cast<uint8_t>(status & 0xFF));
    frame.push_back(static_cast<uint8_t>((status >> 8) & 0xFF));

    // FCS
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    return frame;
}

std::vector<uint8_t> PacketInjector::build_assoc_response(const uint8_t bssid[6],
                                                          const uint8_t dest_mac[6],
                                                          uint16_t status,
                                                          uint16_t aid) {
    std::vector<uint8_t> frame;
    // Frame Control: Subtype 1 (Assoc Response = 0x10)
    frame.push_back(0x10);
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(0x00);
    for (int i = 0; i < 6; ++i) frame.push_back(dest_mac[i]);
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);
    frame.push_back(0x00);
    frame.push_back(0x00);

    // Capabilities (0x0411)
    frame.push_back(0x11);
    frame.push_back(0x04);
    // Status Code
    frame.push_back(static_cast<uint8_t>(status & 0xFF));
    frame.push_back(static_cast<uint8_t>((status >> 8) & 0xFF));
    // AID (0xC000 | aid)
    uint16_t full_aid = 0xC000 | (aid & 0x3FFF);
    frame.push_back(static_cast<uint8_t>(full_aid & 0xFF));
    frame.push_back(static_cast<uint8_t>((full_aid >> 8) & 0xFF));

    // Rates IE
    uint8_t rates[] = {0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};
    frame.push_back(0x01);
    frame.push_back(sizeof(rates));
    for (uint8_t r : rates) frame.push_back(r);

    // FCS
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    return frame;
}

std::vector<uint8_t> PacketInjector::build_deauth(const uint8_t bssid[6],
                                                  const uint8_t dest_mac[6],
                                                  uint16_t reason_code) {
    std::vector<uint8_t> frame;
    // Frame Control: Subtype 12 (Deauth = 0xC0)
    frame.push_back(0xC0);
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(0x00);
    for (int i = 0; i < 6; ++i) frame.push_back(dest_mac[i]);
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);
    frame.push_back(0x00);
    frame.push_back(0x00);

    // Reason Code (2 bytes)
    frame.push_back(static_cast<uint8_t>(reason_code & 0xFF));
    frame.push_back(static_cast<uint8_t>((reason_code >> 8) & 0xFF));

    // FCS
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    return frame;
}

std::vector<uint8_t> PacketInjector::build_eapol_m1(const uint8_t bssid[6],
                                                   const uint8_t sta_mac[6],
                                                   const uint8_t anonce[32],
                                                   uint64_t replay_counter) {
    std::vector<uint8_t> frame;

    // 1. 802.11 QoS Data Header (24 bytes)
    frame.push_back(0x88); // QoS Data
    frame.push_back(0x02); // From DS = 1
    frame.push_back(0x00); // Duration
    frame.push_back(0x00);
    for (int i = 0; i < 6; ++i) frame.push_back(sta_mac[i]); // Addr1 (DA)
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);   // Addr2 (BSSID)
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);   // Addr3 (SA)
    frame.push_back(0x00); // Seq
    frame.push_back(0x00);
    // QoS Control (2 bytes)
    frame.push_back(0x00);
    frame.push_back(0x00);

    // 2. LLC/SNAP Header (8 bytes): 0xAA 0xAA 0x03 0x00 0x00 0x00 0x88 0x8E (EAPOL)
    uint8_t llc_snap[] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    for (uint8_t b : llc_snap) frame.push_back(b);

    // 3. EAPOL Header (4 bytes)
    frame.push_back(0x02); // EAPOL Version 2
    frame.push_back(0x03); // EAPOL-Key (3)
    // Length: 95 bytes for key body
    frame.push_back(0x00);
    frame.push_back(0x5F); // 95

    // 4. EAPOL-Key Body (95 bytes)
    frame.push_back(0x02); // Descriptor Type: RSN / 802.11i (2)
    // Key Information: Pairwise (bit 3), Ack (bit 7) -> 0x008A (little-endian byte order in memory: 0x00, 0x8A or big endian?)
    // EAPOL-Key Key Info is big-endian: 0x008A
    frame.push_back(0x00);
    frame.push_back(0x8A);
    // Key Length: 16 bytes (AES-128)
    frame.push_back(0x00);
    frame.push_back(0x10);
    // Key Replay Counter (8 bytes, big-endian)
    for (int i = 7; i >= 0; --i) {
        frame.push_back(static_cast<uint8_t>((replay_counter >> (i * 8)) & 0xFF));
    }
    // Key Nonce (ANonce, 32 bytes)
    for (int i = 0; i < 32; ++i) frame.push_back(anonce[i]);
    // Key IV (16 bytes zeroes)
    for (int i = 0; i < 16; ++i) frame.push_back(0x00);
    // Key RSC (8 bytes zeroes)
    for (int i = 0; i < 8; ++i) frame.push_back(0x00);
    // Reserved (8 bytes zeroes)
    for (int i = 0; i < 8; ++i) frame.push_back(0x00);
    // Key MIC (16 bytes zeroes in Msg 1)
    for (int i = 0; i < 16; ++i) frame.push_back(0x00);
    // Key Data Length (2 bytes: 0)
    frame.push_back(0x00);
    frame.push_back(0x00);

    // 5. FCS
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    return frame;
}

std::vector<uint8_t> PacketInjector::build_eapol_m3(const uint8_t bssid[6],
                                                   const uint8_t sta_mac[6],
                                                   const uint8_t anonce[32],
                                                   uint64_t replay_counter,
                                                   const uint8_t kck[16],
                                                   const uint8_t kek[16],
                                                   const uint8_t gtk[16]) {
    std::vector<uint8_t> frame;

    // 1. MAC Header (26 bytes with QoS)
    frame.push_back(0x88); // QoS Data
    frame.push_back(0x02); // From DS = 1
    frame.push_back(0x00);
    frame.push_back(0x00);
    for (int i = 0; i < 6; ++i) frame.push_back(sta_mac[i]);
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(0x00);

    // 2. LLC/SNAP (8 bytes)
    uint8_t llc_snap[] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    for (uint8_t b : llc_snap) frame.push_back(b);

    // Prepare Wrapped GTK Key Data
    // GTK KDE: Tag 0xDD, Len 22 (0x16), OUI 00-0F-AC:1, KeyID 1, Res 0, GTK (16 bytes)
    std::vector<uint8_t> kde = {
        0xDD, 22,
        0x00, 0x0F, 0xAC, 0x01,
        0x01, 0x00
    };
    for (int i = 0; i < 16; ++i) kde.push_back(gtk[i]);
    // Pad to 8-byte multiple for AES Key Wrap: 24 bytes is already a multiple of 8 (24 = 3 * 8)
    std::vector<uint8_t> wrapped_kde(kde.size() + 8);
    rtl_crypto::aes_key_wrap(kek, kde.data(), kde.size(), wrapped_kde.data());

    // 3. EAPOL Header (4 bytes)
    size_t eapol_start_index = frame.size();
    frame.push_back(0x02); // EAPOL Version 2
    frame.push_back(0x03); // EAPOL-Key
    uint16_t eapol_body_len = static_cast<uint16_t>(95 + wrapped_kde.size());
    frame.push_back(static_cast<uint8_t>((eapol_body_len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(eapol_body_len & 0xFF));

    // 4. EAPOL-Key Body
    frame.push_back(0x02); // Descriptor Type: RSN
    // Key Info: Pairwise (bit 3), Install (bit 6), Ack (bit 7), MIC (bit 8), Secure (bit 9), Encrypted (bit 10)
    // 0x13CA big-endian
    frame.push_back(0x13);
    frame.push_back(0xCA);
    // Key Length: 16
    frame.push_back(0x00);
    frame.push_back(0x10);
    // Key Replay Counter
    for (int i = 7; i >= 0; --i) {
        frame.push_back(static_cast<uint8_t>((replay_counter >> (i * 8)) & 0xFF));
    }
    // Key Nonce (ANonce)
    for (int i = 0; i < 32; ++i) frame.push_back(anonce[i]);
    // Key IV
    for (int i = 0; i < 16; ++i) frame.push_back(0x00);
    // Key RSC
    for (int i = 0; i < 8; ++i) frame.push_back(0x00);
    // Reserved
    for (int i = 0; i < 8; ++i) frame.push_back(0x00);

    // Key MIC (placeholder 16 bytes zeroes)
    size_t mic_index = frame.size();
    for (int i = 0; i < 16; ++i) frame.push_back(0x00);

    // Key Data Length
    frame.push_back(static_cast<uint8_t>((wrapped_kde.size() >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(wrapped_kde.size() & 0xFF));
    // Key Data
    for (uint8_t b : wrapped_kde) frame.push_back(b);

    // Compute HMAC-SHA1 MIC over EAPOL frame (from eapol_start_index)
    size_t eapol_len = frame.size() - eapol_start_index;
    uint8_t full_digest[20];
    rtl_crypto::hmac_sha1(kck, 16, frame.data() + eapol_start_index, eapol_len, full_digest);
    // Copy first 16 bytes of digest into Key MIC field
    for (int i = 0; i < 16; ++i) {
        frame[mic_index + i] = full_digest[i];
    }

    // 5. FCS
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    return frame;
}

std::vector<uint8_t> PacketInjector::build_ccmp_data(const uint8_t src_mac[6],
                                                    const uint8_t dst_mac[6],
                                                    const uint8_t bssid[6],
                                                    uint64_t pn,
                                                    const uint8_t tk[16],
                                                    const uint8_t* payload,
                                                    size_t payload_len,
                                                    uint16_t ether_type) {
    std::vector<uint8_t> frame;

    // 1. MAC Header (24 bytes QoS Data)
    frame.push_back(0x88); // QoS Data
    frame.push_back(0x02); // From DS = 1
    frame.push_back(0x00); // Duration
    frame.push_back(0x00);
    for (int i = 0; i < 6; ++i) frame.push_back(dst_mac[i]); // Addr1 (DA)
    for (int i = 0; i < 6; ++i) frame.push_back(bssid[i]);   // Addr2 (BSSID)
    for (int i = 0; i < 6; ++i) frame.push_back(src_mac[i]); // Addr3 (SA)
    frame.push_back(0x00); // Seq
    frame.push_back(0x00);
    // QoS Control (2 bytes)
    frame.push_back(0x00);
    frame.push_back(0x00);

    // 2. CCMP Header (8 bytes)
    // PN0, PN1, 0x00, 0x20 (ExtIV=1, KeyID=0), PN2, PN3, PN4, PN5
    uint8_t ccmp_hdr[8];
    ccmp_hdr[0] = static_cast<uint8_t>(pn & 0xFF);
    ccmp_hdr[1] = static_cast<uint8_t>((pn >> 8) & 0xFF);
    ccmp_hdr[2] = 0x00;
    ccmp_hdr[3] = 0x20; // ExtIV bit = 1
    ccmp_hdr[4] = static_cast<uint8_t>((pn >> 16) & 0xFF);
    ccmp_hdr[5] = static_cast<uint8_t>((pn >> 24) & 0xFF);
    ccmp_hdr[6] = static_cast<uint8_t>((pn >> 32) & 0xFF);
    ccmp_hdr[7] = static_cast<uint8_t>((pn >> 40) & 0xFF);

    for (int i = 0; i < 8; ++i) frame.push_back(ccmp_hdr[i]);

    // 3. Construct Nonce (13 bytes): Priority (0x00) || Addr2 (6 bytes) || PN (6 bytes big-endian)
    uint8_t nonce[13];
    nonce[0] = 0x00; // Priority
    for (int i = 0; i < 6; ++i) nonce[1 + i] = bssid[i];
    for (int i = 0; i < 6; ++i) {
        nonce[7 + i] = static_cast<uint8_t>((pn >> ((5 - i) * 8)) & 0xFF);
    }

    // 4. Construct AAD (24 or 26 bytes)
    // MAC Header with masked fields per IEEE 802.11i §12.5.3.3.2
    std::vector<uint8_t> aad(26);
    std::memcpy(aad.data(), frame.data(), 26);
    // Mask subtype bits 4,5,6 of Frame Control: FC & 0xC78F
    aad[0] &= 0x8F;
    aad[1] &= 0xC7;
    // Mask SeqCtrl: aad[22] = 0, aad[23] = 0
    aad[22] = 0x00;
    aad[23] = 0x00;
    // QoS Control: aad[24] &= 0x0F, aad[25] = 0
    aad[24] &= 0x0F;
    aad[25] = 0x00;

    // 5. Construct Plaintext: LLC/SNAP (8 bytes) + payload
    std::vector<uint8_t> plaintext = {
        0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00,
        static_cast<uint8_t>((ether_type >> 8) & 0xFF),
        static_cast<uint8_t>(ether_type & 0xFF)
    };
    if (payload && payload_len > 0) {
        plaintext.insert(plaintext.end(), payload, payload + payload_len);
    }

    // 6. Encrypt with AES-CCM
    std::vector<uint8_t> ciphertext(plaintext.size());
    uint8_t mic[8];
    rtl_crypto::ccmp_encrypt(tk, nonce, aad.data(), aad.size(),
                             plaintext.data(), plaintext.size(),
                             ciphertext.data(), mic);

    for (uint8_t b : ciphertext) frame.push_back(b);
    for (int i = 0; i < 8; ++i) frame.push_back(mic[i]);

    // 7. FCS (CRC32)
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    return frame;
}

std::vector<uint8_t> PacketInjector::build_arp_packet(uint16_t op,
                                                      const uint8_t sender_mac[6],
                                                      const uint8_t sender_ip[4],
                                                      const uint8_t target_mac[6],
                                                      const uint8_t target_ip[4]) {
    std::vector<uint8_t> arp = {
        0x00, 0x01, // Hardware Type: Ethernet (1)
        0x08, 0x00, // Protocol Type: IPv4 (0x0800)
        0x06,       // Hardware Size: 6
        0x04,       // Protocol Size: 4
        static_cast<uint8_t>((op >> 8) & 0xFF),
        static_cast<uint8_t>(op & 0xFF)
    };
    for (int i = 0; i < 6; ++i) arp.push_back(sender_mac[i]);
    for (int i = 0; i < 4; ++i) arp.push_back(sender_ip[i]);
    for (int i = 0; i < 6; ++i) arp.push_back(target_mac[i]);
    for (int i = 0; i < 4; ++i) arp.push_back(target_ip[i]);
    return arp;
}

} // namespace rtl_mock
