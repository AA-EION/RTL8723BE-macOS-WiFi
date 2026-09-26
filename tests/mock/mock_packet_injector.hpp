#pragma once

#include "mock_dma.hpp"
#include "mock_crypto.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace rtl_mock {

// Packet Injector Helper creating authentic IEEE 802.11 frames
class PacketInjector {
public:
    explicit PacketInjector(MockDMA& dma);
    ~PacketInjector() = default;

    // 802.11 Management Frames
    std::vector<uint8_t> build_beacon(const std::string& ssid,
                                     const uint8_t bssid[6],
                                     uint8_t channel,
                                     int8_t rssi = -50,
                                     bool is_wpa2 = true,
                                     uint16_t beacon_int = 100);

    std::vector<uint8_t> build_probe_response(const std::string& ssid,
                                              const uint8_t bssid[6],
                                              const uint8_t dest_mac[6],
                                              uint8_t channel,
                                              int8_t rssi = -50,
                                              bool is_wpa2 = true);

    std::vector<uint8_t> build_auth_response(const uint8_t bssid[6],
                                             const uint8_t dest_mac[6],
                                             uint16_t seq = 2,
                                             uint16_t status = 0);

    std::vector<uint8_t> build_assoc_response(const uint8_t bssid[6],
                                              const uint8_t dest_mac[6],
                                              uint16_t status = 0,
                                              uint16_t aid = 1);

    std::vector<uint8_t> build_deauth(const uint8_t bssid[6],
                                      const uint8_t dest_mac[6],
                                      uint16_t reason_code = 2);

    // WPA2 EAPOL-Key Handshake Frames
    // Msg 1: AP -> STA (ANonce, Replay Counter, no MIC)
    std::vector<uint8_t> build_eapol_m1(const uint8_t bssid[6],
                                        const uint8_t sta_mac[6],
                                        const uint8_t anonce[32],
                                        uint64_t replay_counter = 1);

    // Msg 3: AP -> STA (ANonce, Replay Counter, MIC with KCK, encrypted GTK under KEK)
    std::vector<uint8_t> build_eapol_m3(const uint8_t bssid[6],
                                        const uint8_t sta_mac[6],
                                        const uint8_t anonce[32],
                                        uint64_t replay_counter,
                                        const uint8_t kck[16],
                                        const uint8_t kek[16],
                                        const uint8_t gtk[16]);

    // CCMP Encrypted Data Frames
    std::vector<uint8_t> build_ccmp_data(const uint8_t src_mac[6],
                                         const uint8_t dst_mac[6],
                                         const uint8_t bssid[6],
                                         uint64_t pn,
                                         const uint8_t tk[16],
                                         const uint8_t* payload,
                                         size_t payload_len,
                                         uint16_t ether_type = 0x0800);

    // ARP Payload Builders
    static std::vector<uint8_t> build_arp_packet(uint16_t op, // 1 = Req, 2 = Reply
                                                 const uint8_t sender_mac[6],
                                                 const uint8_t sender_ip[4],
                                                 const uint8_t target_mac[6],
                                                 const uint8_t target_ip[4]);

    // High-Level Injection to Mock DMA RX Ring
    bool inject_frame(const std::vector<uint8_t>& frame,
                      bool crc_err = false,
                      bool icv_err = false,
                      uint8_t shift = 0);

private:
    MockDMA& dma_;
};

} // namespace rtl_mock
