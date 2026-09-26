#pragma once

#include "mock_pci_mmio.hpp"
#include "mock_dma.hpp"
#include "mock_crypto.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <array>
#include <map>

namespace rtl_mock {

enum WifiState {
    WIFI_DISCONNECTED,
    WIFI_SCANNING,
    WIFI_AUTHENTICATING,
    WIFI_ASSOCIATING,
    WIFI_4WAY_HANDSHAKE,
    WIFI_CONNECTED
};

struct DiscoveredAP {
    uint8_t bssid[6];
    std::string ssid;
    uint8_t channel;
    int8_t rssi;
    bool is_wpa2;
    uint16_t beacon_interval;
    uint16_t capabilities;
};

class SimulatedDevice {
public:
    SimulatedDevice(MockPCIMmio& mmio, MockDMA& dma);
    ~SimulatedDevice() = default;

    void reset();

    // Hardware Control
    bool hw_power_on();
    bool hw_power_off();
    bool hw_download_firmware(const uint8_t* fw_buf, size_t fw_len);
    bool hw_read_efuse(CalibData& out_calib);
    bool hw_decode_pg_stream(const uint8_t* pg_stream, size_t len, CalibData& out_calib);
    bool hw_set_channel(uint8_t channel);
    bool hw_set_antenna(uint8_t ant); // 1 = Main, 2 = Aux

    // DMA Ring Allocation & Management
    bool init_dma_rings(size_t tx_count = 64, size_t rx_count = 64);
    bool transmit_raw_frame(QueueId q_id, const uint8_t* frame, size_t len);
    bool poll_rx_frame(std::vector<uint8_t>& out_frame, RxDesc32& out_desc);

    // 802.11 Protocol & Scanner
    bool parse_beacon_or_probe(const uint8_t* frame, size_t len, DiscoveredAP& out_ap);
    bool start_scan();
    const std::vector<DiscoveredAP>& get_scan_results() const { return scan_results_; }

    // WPA2-PSK Authentication, Association & 4-Way Handshake
    bool connect(const std::string& ssid, const std::string& passphrase, const uint8_t target_bssid[6]);
    void process_inbound_packet(const uint8_t* frame, size_t len);

    // Data Path Translation (Ethernet II <-> 802.11 CCMP)
    bool send_ethernet_packet(const uint8_t* eth_packet, size_t len);
    bool receive_ethernet_packet(const uint8_t* frame, size_t len, std::vector<uint8_t>& out_eth_packet);

    // State & Status
    WifiState get_state() const { return state_; }
    void set_state(WifiState s) { state_ = s; }
    const uint8_t* get_mac_address() const { return my_mac_; }
    void set_mac_address(const uint8_t mac[6]) { std::memcpy(my_mac_, mac, 6); }
    uint64_t get_tx_pn() const { return tx_pn_; }
    uint64_t get_rx_pn() const { return last_rx_pn_; }

    // Handshake Key Material Inspection (for tests)
    const uint8_t* get_pmk() const { return pmk_; }
    const uint8_t* get_kck() const { return ptk_; }
    const uint8_t* get_kek() const { return ptk_ + 16; }
    const uint8_t* get_tk() const { return ptk_ + 32; }
    const uint8_t* get_gtk() const { return gtk_; }

private:
    void handle_auth_response(const uint8_t* frame, size_t len);
    void handle_assoc_response(const uint8_t* frame, size_t len);
    void handle_eapol_frame(const uint8_t* frame, size_t len);

    MockPCIMmio& mmio_;
    MockDMA& dma_;

    WifiState state_{WIFI_DISCONNECTED};
    uint8_t my_mac_[6]{0x00, 0xE0, 0x4C, 0x81, 0x92, 0x23};
    uint8_t connected_bssid_[6]{0};
    std::string connected_ssid_;
    std::string passphrase_;

    // DMA Ring Info (per hardware queue)
    uint64_t tx_ring_bases_[8]{0};
    size_t tx_ring_counts_[8]{0};
    size_t tx_host_indices_[8]{0};

    uint64_t rx_ring_base_{0};
    size_t rx_ring_count_{0};
    size_t rx_host_index_{0};

    // Scan Cache
    std::vector<DiscoveredAP> scan_results_;

    // Cryptographic Keys
    uint8_t pmk_[32]{0};
    uint8_t ptk_[64]{0}; // KCK (16), KEK (16), TK (16), etc.
    uint8_t gtk_[16]{0};
    uint8_t snonce_[32]{0};
    uint8_t anonce_[32]{0};

    // CCMP Replay Prevention
    uint64_t tx_pn_{1};
    uint64_t last_rx_pn_{0};
    uint64_t last_replay_counter_{0};
};

} // namespace rtl_mock
