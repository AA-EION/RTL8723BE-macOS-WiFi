#include "simulated_device.hpp"
#include <cstring>
#include <algorithm>

namespace rtl_mock {

SimulatedDevice::SimulatedDevice(MockPCIMmio& mmio, MockDMA& dma)
    : mmio_(mmio), dma_(dma) {
    reset();
}

void SimulatedDevice::reset() {
    state_ = WIFI_DISCONNECTED;
    for (int i = 0; i < 8; ++i) {
        tx_ring_bases_[i] = 0;
        tx_ring_counts_[i] = 0;
        tx_host_indices_[i] = 0;
    }
    rx_ring_base_ = 0;
    rx_ring_count_ = 0;
    rx_host_index_ = 0;
    scan_results_.clear();
    std::memset(connected_bssid_, 0, 6);
    connected_ssid_.clear();
    passphrase_.clear();
    std::memset(pmk_, 0, sizeof(pmk_));
    std::memset(ptk_, 0, sizeof(ptk_));
    std::memset(gtk_, 0, sizeof(gtk_));
    std::memset(snonce_, 0, sizeof(snonce_));
    std::memset(anonce_, 0, sizeof(anonce_));
    tx_pn_ = 1;
    last_rx_pn_ = 0;
    last_replay_counter_ = 0;
}

bool SimulatedDevice::hw_power_on() {
    // 1. Power unlock: write 0x00 to REG_RSV_CTRL (0x001C)
    mmio_.write8(REG_RSV_CTRL, 0x00);

    // 2. Clear auto power down in REG_APS_FSMCO+1 (0x0005)
    uint8_t aps = mmio_.read8(REG_APS_FSMCO + 1);
    mmio_.write8(REG_APS_FSMCO + 1, aps & ~0x80);

    // 3. Clocks and Function Enable: REG_SYS_FUNC_EN (0x0002) & REG_SYS_CLKR (0x0008)
    mmio_.write16(REG_SYS_FUNC_EN, 0x0405); // PCIe DMA enable + CPU reset & clock
    mmio_.write16(REG_SYS_CLKR, 0x0808);    // MAC clock enable + Ring enable

    // 4. Set Command Register REG_CR (0x0100) = 0x02FF (TRX enable + MAC enable)
    mmio_.write16(REG_CR, 0x02FF);

    return (mmio_.get_power_state() == POWER_ACT);
}

bool SimulatedDevice::hw_power_off() {
    // Write 0x0E to REG_RSV_CTRL (0x001C)
    mmio_.write8(REG_RSV_CTRL, 0x0E);
    mmio_.write16(REG_CR, 0x0000);
    state_ = WIFI_DISCONNECTED;
    return (mmio_.get_power_state() == POWER_CARDDIS);
}

bool SimulatedDevice::hw_download_firmware(const uint8_t* fw_buf, size_t fw_len) {
    if (!fw_buf || fw_len < 32) return false;

    // 1. Validate Header
    uint16_t sig = static_cast<uint16_t>(fw_buf[0]) | (static_cast<uint16_t>(fw_buf[1]) << 8);
    if ((sig & 0xFFF0) != 0x5300) {
        return false; // Signature mismatch
    }

    const uint8_t* microcode = fw_buf + 32;
    size_t payload_len = fw_len - 32;

    // 2. MCU self-reset if already active
    if (mmio_.read32(REG_MCUFWDL) & MCUFWDL_FW_RESET) {
        mmio_.write32(REG_MCUFWDL, 0x00000000);
    }

    // 3. Enter download mode
    mmio_.write32(REG_MCUFWDL, MCUFWDL_FWDL_EN);

    // 4. Download page by page (4096 bytes per page)
    size_t num_pages = (payload_len + 4095) / 4096;
    if (num_pages > 8) {
        return false; // Page overflow (> 8 pages)
    }

    for (size_t p = 0; p < num_pages; ++p) {
        mmio_.write8(REG_MCUFWDL_PAGE, static_cast<uint8_t>(p));
        size_t page_offset = p * 4096;
        size_t chunk_len = std::min<size_t>(4096, payload_len - page_offset);

        for (size_t i = 0; i < chunk_len; ++i) {
            mmio_.write8(REG_FW_START_ADDR + static_cast<uint16_t>(i),
                         microcode[page_offset + i]);
        }
    }

    // 5. Exit download mode
    mmio_.write32(REG_MCUFWDL, 0x00000000);

    // 6. Checksum Handshake
    uint32_t fwdl_status = mmio_.read32(REG_MCUFWDL);
    if ((fwdl_status & MCUFWDL_CHKSUM_RPT) == 0) {
        return false; // Checksum verification failed
    }

    // 7. Ready handshake & MCU reset
    mmio_.write32(REG_MCUFWDL, MCUFWDL_RDY);

    // Verify MCU active
    fwdl_status = mmio_.read32(REG_MCUFWDL);
    return ((fwdl_status & MCUFWDL_WINTINI_RDY) != 0);
}

bool SimulatedDevice::hw_read_efuse(CalibData& out_calib) {
    // 1. Enable eFuse access
    mmio_.write8(REG_EFUSE_ACCESS, 0x69);

    std::array<uint8_t, 512> shadow_map;
    // 2. Read 512 bytes via REG_EFUSE_CTRL (0x0030)
    for (uint16_t addr = 0; addr < 512; ++addr) {
        // Trigger read: [17:8] = addr, [30:24] = 0x72
        uint32_t cmd = (static_cast<uint32_t>(addr) << 8) | (0x72U << 24) | 0x80000000U;
        mmio_.write32(REG_EFUSE_CTRL, cmd);

        uint32_t res = mmio_.read32(REG_EFUSE_CTRL);
        shadow_map[addr] = static_cast<uint8_t>(res & 0xFF);
    }

    // 3. Disable eFuse access
    mmio_.write8(REG_EFUSE_ACCESS, 0x00);

    // 4. Extract fields into out_calib
    std::memcpy(out_calib.mac_addr, &shadow_map[0x00D0], 6);
    std::memcpy(my_mac_, out_calib.mac_addr, 6);

    out_calib.crystal_cap = shadow_map[0x00B9];
    out_calib.thermal_meter = shadow_map[0x00BA];
    out_calib.channel_plan = shadow_map[0x00B8];

    std::memcpy(out_calib.tx_pwr_cck, &shadow_map[0x0010], 6);
    std::memcpy(out_calib.tx_pwr_ht40, &shadow_map[0x0016], 5);
    out_calib.tx_pwr_ht20_diff = shadow_map[0x001B];

    out_calib.vid  = shadow_map[0x00D6] | (static_cast<uint16_t>(shadow_map[0x00D7]) << 8);
    out_calib.did  = shadow_map[0x00D8] | (static_cast<uint16_t>(shadow_map[0x00D9]) << 8);
    out_calib.svid = shadow_map[0x00DA] | (static_cast<uint16_t>(shadow_map[0x00DB]) << 8);
    out_calib.smid = shadow_map[0x00DC] | (static_cast<uint16_t>(shadow_map[0x00DD]) << 8);

    // Fallback checks
    if (out_calib.crystal_cap == 0xFF) out_calib.crystal_cap = 0x20;
    if (out_calib.thermal_meter == 0xFF) out_calib.thermal_meter = 0x1A;
    if (out_calib.tx_pwr_cck[0] == 0xFF) {
        for (int i = 0; i < 6; ++i) out_calib.tx_pwr_cck[i] = 0x2D;
    }

    return true;
}

bool SimulatedDevice::hw_decode_pg_stream(const uint8_t* pg_stream, size_t len, CalibData& out_calib) {
    if (!pg_stream || len == 0) return false;

    std::array<uint8_t, 512> shadow_map;
    shadow_map.fill(0xFF);

    size_t idx = 0;
    while (idx < len) {
        uint8_t header = pg_stream[idx++];
        if (header == 0xFF) break; // End of PG stream

        uint8_t offset_index = (header >> 4) & 0x0F;
        uint8_t word_mask = ~header & 0x0F;

        size_t base_word = static_cast<size_t>(offset_index) * 8;
        for (int w = 0; w < 4; ++w) {
            if (word_mask & (1 << w)) {
                if (idx + 1 >= len) break; // Truncated
                uint8_t b0 = pg_stream[idx++];
                uint8_t b1 = pg_stream[idx++];
                size_t byte_addr = (base_word + w) * 2;
                if (byte_addr + 1 < 512) {
                    shadow_map[byte_addr] = b0;
                    shadow_map[byte_addr + 1] = b1;
                }
            }
        }
    }

    std::memcpy(out_calib.mac_addr, &shadow_map[0x00D0], 6);
    out_calib.crystal_cap = shadow_map[0x00B9];
    out_calib.thermal_meter = shadow_map[0x00BA];
    out_calib.channel_plan = shadow_map[0x00B8];

    // Fallbacks
    if (out_calib.crystal_cap == 0xFF) out_calib.crystal_cap = 0x20;
    if (out_calib.thermal_meter == 0xFF) out_calib.thermal_meter = 0x1A;
    if (out_calib.tx_pwr_cck[0] == 0xFF) {
        for (int i = 0; i < 6; ++i) out_calib.tx_pwr_cck[i] = 0x2D;
    }

    return true;
}

bool SimulatedDevice::hw_set_channel(uint8_t channel) {
    if (channel < 1 || channel > 14) return false;
    // Baseband 3-wire LSSI RF write: register 0x18 (RF_CHNLBW)
    // Bits [27:20] = 0x18, Bits [9:0] = channel
    uint32_t val = (0x18U << 20) | (channel & 0x3FF);
    mmio_.write32(REG_RFPGA0_XA_LSSI, val);
    return (mmio_.get_active_channel() == channel);
}

bool SimulatedDevice::hw_set_antenna(uint8_t ant) {
    if (ant != 1 && ant != 2) return false;
    mmio_.write32(REG_BB_ANT_DIV, ant);
    return (mmio_.get_active_antenna() == ant);
}

bool SimulatedDevice::init_dma_rings(size_t tx_count, size_t rx_count) {
    rx_ring_count_ = rx_count;
    rx_host_index_ = 0;

    // Configure dedicated HW rings for all queues
    const QueueId queues[] = { Q_BK, Q_BE, Q_VI, Q_VO, Q_BCN, Q_MGNT, Q_HIGH };
    for (QueueId q : queues) {
        size_t idx = static_cast<size_t>(q);
        tx_ring_counts_[idx] = tx_count;
        tx_host_indices_[idx] = 0;
        tx_ring_bases_[idx] = dma_.alloc_phys_mem(tx_count * sizeof(TxDesc40), 256);
        dma_.setup_tx_ring(q, tx_ring_bases_[idx], tx_count);
    }

    // Allocate RX Ring (aligned to 256 bytes)
    rx_ring_base_ = dma_.alloc_phys_mem(rx_count * sizeof(RxDesc32), 256);

    // Initialize RX Ring buffers
    for (size_t i = 0; i < rx_count; ++i) {
        uint64_t desc_phys = rx_ring_base_ + i * sizeof(RxDesc32);
        RxDesc32* desc = reinterpret_cast<RxDesc32*>(dma_.phys_to_virt(desc_phys));
        std::memset(desc, 0, sizeof(RxDesc32));

        uint64_t buf_phys = dma_.alloc_phys_mem(2048, 256);
        desc->set_buffer_addr(buf_phys);
        desc->set_own(true); // DMA owns it, ready to receive

        if (i == (rx_count - 1)) {
            desc->set_eor(true); // End of Ring flag
        }
    }

    dma_.setup_rx_ring(rx_ring_base_, rx_count);

    // Write DESA registers in MMIO
    mmio_.write32(REG_BKQ_DESA, static_cast<uint32_t>(tx_ring_bases_[Q_BK] & 0xFFFFFFFF));
    mmio_.write32(REG_BEQ_DESA, static_cast<uint32_t>(tx_ring_bases_[Q_BE] & 0xFFFFFFFF));
    mmio_.write32(REG_VIQ_DESA, static_cast<uint32_t>(tx_ring_bases_[Q_VI] & 0xFFFFFFFF));
    mmio_.write32(REG_VOQ_DESA, static_cast<uint32_t>(tx_ring_bases_[Q_VO] & 0xFFFFFFFF));
    mmio_.write32(REG_BCNQ_DESA, static_cast<uint32_t>(tx_ring_bases_[Q_BCN] & 0xFFFFFFFF));
    mmio_.write32(REG_MGQ_DESA, static_cast<uint32_t>(tx_ring_bases_[Q_MGNT] & 0xFFFFFFFF));
    mmio_.write32(REG_HQ_DESA, static_cast<uint32_t>(tx_ring_bases_[Q_HIGH] & 0xFFFFFFFF));
    mmio_.write32(REG_RX_DESA, static_cast<uint32_t>(rx_ring_base_ & 0xFFFFFFFF));

    return true;
}

bool SimulatedDevice::transmit_raw_frame(QueueId q_id, const uint8_t* frame, size_t len) {
    size_t q_idx = static_cast<size_t>(q_id);
    if (q_idx >= 8) return false;
    if (tx_ring_bases_[q_idx] == 0 || tx_ring_counts_[q_idx] == 0) return false;

    uint64_t desc_phys = tx_ring_bases_[q_idx] + tx_host_indices_[q_idx] * sizeof(TxDesc40);
    TxDesc40* desc = reinterpret_cast<TxDesc40*>(dma_.phys_to_virt(desc_phys));
    if (!desc) return false;

    if (desc->get_own()) {
        // Ring is full / backpressure!
        return false;
    }

    uint64_t buf_phys = desc->get_buffer_addr();
    if (buf_phys == 0) {
        buf_phys = dma_.alloc_phys_mem(4096, 256);
        desc->set_buffer_addr(buf_phys);
    }

    uint8_t* dest = reinterpret_cast<uint8_t*>(dma_.phys_to_virt(buf_phys));
    if (frame && len > 0) {
        std::memcpy(dest, frame, len);
    }

    desc->set_pktsize(static_cast<uint16_t>(len));
    desc->set_offset(40);
    desc->set_firstseg(true);
    desc->set_lastseg(true);
    desc->set_queuesel(static_cast<uint8_t>(q_id));
    desc->set_own(true); // Relinquish to DMA

    // Trigger Doorbell
    uint16_t doorbell_bit = 1 << static_cast<uint8_t>(q_id);
    mmio_.write16(REG_PCIE_CTRL_REG, doorbell_bit);

    tx_host_indices_[q_idx] = (tx_host_indices_[q_idx] + 1) % tx_ring_counts_[q_idx];
    return true;
}

bool SimulatedDevice::poll_rx_frame(std::vector<uint8_t>& out_frame, RxDesc32& out_desc) {
    if (rx_ring_base_ == 0 || rx_ring_count_ == 0) return false;

    uint64_t desc_phys = rx_ring_base_ + rx_host_index_ * sizeof(RxDesc32);
    RxDesc32* desc = reinterpret_cast<RxDesc32*>(dma_.phys_to_virt(desc_phys));
    if (!desc) return false;

    if (desc->get_own()) {
        // Still owned by DMA, nothing to read
        return false;
    }

    out_desc = *desc;
    uint16_t len = desc->get_length();
    uint8_t shift = desc->get_shift();
    uint64_t buf_phys = desc->get_buffer_addr();
    const uint8_t* src = reinterpret_cast<const uint8_t*>(dma_.phys_to_virt(buf_phys));

    out_frame.clear();
    if (src && len > 0) {
        out_frame.assign(src + shift, src + shift + len);
    }

    // Re-arm descriptor for DMA
    bool is_eor = desc->get_eor();
    desc->set_own(true);

    if (is_eor) {
        rx_host_index_ = 0;
    } else {
        rx_host_index_ = (rx_host_index_ + 1) % rx_ring_count_;
    }

    return true;
}

bool SimulatedDevice::parse_beacon_or_probe(const uint8_t* frame, size_t len, DiscoveredAP& out_ap) {
    if (!frame || len < 36) return false; // Minimum 24 header + 12 fixed

    uint8_t fc_subtype = (frame[0] >> 4) & 0x0F;
    if (fc_subtype != 8 && fc_subtype != 5) {
        return false; // Not Beacon (8) or Probe Response (5)
    }

    // BSSID is Addr3 at offset 16
    std::memcpy(out_ap.bssid, frame + 16, 6);

    // Fixed parameters
    out_ap.beacon_interval = frame[32] | (static_cast<uint16_t>(frame[33]) << 8);
    out_ap.capabilities = frame[34] | (static_cast<uint16_t>(frame[35]) << 8);

    out_ap.is_wpa2 = (out_ap.capabilities & 0x0010) != 0;
    out_ap.channel = 1;
    out_ap.rssi = -50;
    out_ap.ssid.clear();

    // Parse IEs
    size_t offset = 36;
    while (offset + 1 < (len - 4)) { // exclude 4-byte FCS
        uint8_t tag = frame[offset];
        uint8_t tag_len = frame[offset + 1];
        offset += 2;

        if (offset + tag_len > (len - 4)) break;

        if (tag == 0) { // SSID
            out_ap.ssid.assign(reinterpret_cast<const char*>(frame + offset), tag_len);
        } else if (tag == 3 && tag_len >= 1) { // DS Parameter Set (Channel)
            out_ap.channel = frame[offset];
        } else if (tag == 48) { // RSN IE (WPA2)
            out_ap.is_wpa2 = true;
        }

        offset += tag_len;
    }

    return true;
}

bool SimulatedDevice::start_scan() {
    state_ = WIFI_SCANNING;
    scan_results_.clear();
    return true;
}

bool SimulatedDevice::connect(const std::string& ssid,
                             const std::string& passphrase,
                             const uint8_t target_bssid[6]) {
    connected_ssid_ = ssid;
    passphrase_ = passphrase;
    std::memcpy(connected_bssid_, target_bssid, 6);

    // Derive PMK (PBKDF2 HMAC-SHA1 4096 iterations)
    rtl_crypto::pbkdf2_sha1(passphrase.c_str(), passphrase.length(),
                            reinterpret_cast<const uint8_t*>(ssid.c_str()), ssid.length(),
                            4096, pmk_, 32);

    state_ = WIFI_AUTHENTICATING;

    // Transmit Authentication Request frame (Seq 1)
    std::vector<uint8_t> auth_req = {
        0xB0, 0x00, 0x00, 0x00,
        connected_bssid_[0], connected_bssid_[1], connected_bssid_[2],
        connected_bssid_[3], connected_bssid_[4], connected_bssid_[5],
        my_mac_[0], my_mac_[1], my_mac_[2],
        my_mac_[3], my_mac_[4], my_mac_[5],
        connected_bssid_[0], connected_bssid_[1], connected_bssid_[2],
        connected_bssid_[3], connected_bssid_[4], connected_bssid_[5],
        0x00, 0x00, // Seq
        0x00, 0x00, // Open System
        0x01, 0x00, // Seq 1
        0x00, 0x00  // Success status
    };
    transmit_raw_frame(Q_MGNT, auth_req.data(), auth_req.size());

    return true;
}

void SimulatedDevice::process_inbound_packet(const uint8_t* frame, size_t len) {
    if (!frame || len < 24) return;

    uint8_t type = (frame[0] >> 2) & 0x03;
    uint8_t subtype = (frame[0] >> 4) & 0x0F;

    if (type == 0) { // Management
        if (subtype == 8 || subtype == 5) { // Beacon or Probe Response
            DiscoveredAP ap;
            if (parse_beacon_or_probe(frame, len, ap)) {
                // Merge or add to scan results
                bool found = false;
                for (auto& existing : scan_results_) {
                    if (std::memcmp(existing.bssid, ap.bssid, 6) == 0) {
                        found = true;
                        existing = ap;
                        break;
                    }
                }
                if (!found) {
                    scan_results_.push_back(ap);
                }
            }
        } else if (subtype == 11) { // Auth
            handle_auth_response(frame, len);
        } else if (subtype == 1) { // Assoc Response
            handle_assoc_response(frame, len);
        } else if (subtype == 12) { // Deauth
            state_ = WIFI_DISCONNECTED;
            std::memset(ptk_, 0, sizeof(ptk_));
            std::memset(gtk_, 0, sizeof(gtk_));
        }
    } else if (type == 2) { // Data
        // Check if EAPOL (24-byte MAC header or 26-byte QoS Data header)
        if ((len >= 32 && frame[24] == 0xAA && frame[25] == 0xAA && frame[26] == 0x03 &&
             frame[30] == 0x88 && frame[31] == 0x8E) ||
            (len >= 34 && frame[26] == 0xAA && frame[27] == 0xAA && frame[28] == 0x03 &&
             frame[32] == 0x88 && frame[33] == 0x8E)) {
            handle_eapol_frame(frame, len);
        }
    }
}

void SimulatedDevice::handle_auth_response(const uint8_t* frame, size_t len) {
    if (len < 30 || state_ != WIFI_AUTHENTICATING) return;

    uint16_t status = frame[28] | (static_cast<uint16_t>(frame[29]) << 8);
    if (status == 0) { // Success
        state_ = WIFI_ASSOCIATING;

        // Send Association Request
        std::vector<uint8_t> assoc_req = {
            0x00, 0x00, 0x00, 0x00,
            connected_bssid_[0], connected_bssid_[1], connected_bssid_[2],
            connected_bssid_[3], connected_bssid_[4], connected_bssid_[5],
            my_mac_[0], my_mac_[1], my_mac_[2],
            my_mac_[3], my_mac_[4], my_mac_[5],
            connected_bssid_[0], connected_bssid_[1], connected_bssid_[2],
            connected_bssid_[3], connected_bssid_[4], connected_bssid_[5],
            0x00, 0x00,
            0x11, 0x04, // Capabilities
            0x0A, 0x00  // Listen Interval
        };
        // Append SSID IE
        assoc_req.push_back(0x00);
        assoc_req.push_back(static_cast<uint8_t>(connected_ssid_.length()));
        for (char c : connected_ssid_) assoc_req.push_back(static_cast<uint8_t>(c));

        transmit_raw_frame(Q_MGNT, assoc_req.data(), assoc_req.size());
    }
}

void SimulatedDevice::handle_assoc_response(const uint8_t* frame, size_t len) {
    if (len < 30 || state_ != WIFI_ASSOCIATING) return;

    uint16_t status = frame[26] | (static_cast<uint16_t>(frame[27]) << 8);
    if (status == 0) {
        state_ = WIFI_4WAY_HANDSHAKE;
    }
}

void SimulatedDevice::handle_eapol_frame(const uint8_t* frame, size_t len) {
    size_t eapol_start = 34;
    if (len >= 32 && frame[24] == 0xAA) {
        eapol_start = 32;
    }
    if (len < eapol_start + 4 + 95) return;

    uint16_t key_info = (static_cast<uint16_t>(frame[eapol_start + 5]) << 8) | frame[eapol_start + 6];
    bool is_mic_set = (key_info & 0x0100) != 0;
    bool is_install_set = (key_info & 0x0040) != 0;

    uint64_t replay_counter = 0;
    for (int i = 0; i < 8; ++i) {
        replay_counter = (replay_counter << 8) | frame[eapol_start + 9 + i];
    }

    if (!is_mic_set) {
        // Message 1 (AP -> STA)
        // Extract ANonce (32 bytes at offset eapol_start + 17)
        std::memcpy(anonce_, frame + eapol_start + 17, 32);

        // Generate synthetic deterministic SNonce
        for (int i = 0; i < 32; ++i) snonce_[i] = static_cast<uint8_t>(0x55 ^ i);

        // Derive PTK via PRF-512
        rtl_crypto::prf_512(pmk_, "Pairwise key expansion",
                           connected_bssid_, my_mac_,
                           anonce_, snonce_,
                           ptk_);

        // Send Message 2 (STA -> AP)
        std::vector<uint8_t> m2_frame;
        // MAC Header (26 bytes)
        m2_frame.push_back(0x88);
        m2_frame.push_back(0x01); // To DS = 1
        m2_frame.push_back(0x00);
        m2_frame.push_back(0x00);
        for (int i = 0; i < 6; ++i) m2_frame.push_back(connected_bssid_[i]); // DA
        for (int i = 0; i < 6; ++i) m2_frame.push_back(my_mac_[i]);          // SA
        for (int i = 0; i < 6; ++i) m2_frame.push_back(connected_bssid_[i]); // BSSID
        m2_frame.push_back(0x00);
        m2_frame.push_back(0x00);
        m2_frame.push_back(0x00);
        m2_frame.push_back(0x00);

        // LLC/SNAP
        uint8_t llc[] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
        for (uint8_t b : llc) m2_frame.push_back(b);

        size_t m2_eapol_start = m2_frame.size();
        m2_frame.push_back(0x02); // EAPOL version
        m2_frame.push_back(0x03); // EAPOL-Key
        m2_frame.push_back(0x00);
        m2_frame.push_back(0x5F); // 95 bytes

        m2_frame.push_back(0x02); // Desc type
        // Key Info: Pairwise (bit 3) | MIC (bit 8) -> 0x010A
        m2_frame.push_back(0x01);
        m2_frame.push_back(0x0A);
        m2_frame.push_back(0x00);
        m2_frame.push_back(0x10); // Key length 16

        // Replay counter matching M1
        for (int i = 7; i >= 0; --i) {
            m2_frame.push_back(static_cast<uint8_t>((replay_counter >> (i * 8)) & 0xFF));
        }

        // SNonce (32 bytes)
        for (int i = 0; i < 32; ++i) m2_frame.push_back(snonce_[i]);

        // IV, RSC, Reserved (32 bytes zeroes)
        for (int i = 0; i < 32; ++i) m2_frame.push_back(0x00);

        // Key MIC placeholder
        size_t mic_pos = m2_frame.size();
        for (int i = 0; i < 16; ++i) m2_frame.push_back(0x00);

        // Key Data Length: 0
        m2_frame.push_back(0x00);
        m2_frame.push_back(0x00);

        // Compute HMAC-SHA1 MIC using KCK (first 16 bytes of PTK)
        uint8_t full_digest[20];
        rtl_crypto::hmac_sha1(ptk_, 16,
                              m2_frame.data() + m2_eapol_start,
                              m2_frame.size() - m2_eapol_start,
                              full_digest);
        for (int i = 0; i < 16; ++i) {
            m2_frame[mic_pos + i] = full_digest[i];
        }

        transmit_raw_frame(Q_BE, m2_frame.data(), m2_frame.size());
    } else if (is_mic_set && is_install_set) {
        // Message 3 (AP -> STA)
        // 1. Verify MIC with KCK
        std::vector<uint8_t> verify_buf(frame + eapol_start, frame + len - 4); // exclude FCS
        uint8_t received_mic[16];
        std::memcpy(received_mic, verify_buf.data() + 81, 16);
        std::memset(verify_buf.data() + 81, 0, 16); // Zero out MIC for calculation

        uint8_t comp_digest[20];
        rtl_crypto::hmac_sha1(ptk_, 16, verify_buf.data(), verify_buf.size(), comp_digest);
        if (std::memcmp(received_mic, comp_digest, 16) != 0) {
            return; // Corrupted MIC! Discard
        }

        // 2. Unwrap GTK from Key Data
        if (verify_buf.size() >= 99) {
            uint16_t key_data_len = (static_cast<uint16_t>(verify_buf[97]) << 8) | verify_buf[98];
            if (key_data_len >= 32 && (99 + key_data_len) <= verify_buf.size()) {
                const uint8_t* wrapped_gtk = verify_buf.data() + 99;
                std::vector<uint8_t> unwrapped(key_data_len - 8);
                if (rtl_crypto::aes_key_unwrap(ptk_ + 16, wrapped_gtk, key_data_len, unwrapped.data())) {
                    // KDE: Tag (0xDD), Len (22), OUI (00-0F-AC:1), KeyID/Flags, Res, GTK (16)
                    if (unwrapped.size() >= 24 && unwrapped[0] == 0xDD) {
                        std::memcpy(gtk_, unwrapped.data() + 8, 16);
                    }
                }
            }
        }

        // 3. Send Message 4 (STA -> AP)
        std::vector<uint8_t> m4_frame;
        // MAC Header
        m4_frame.push_back(0x88);
        m4_frame.push_back(0x01);
        m4_frame.push_back(0x00);
        m4_frame.push_back(0x00);
        for (int i = 0; i < 6; ++i) m4_frame.push_back(connected_bssid_[i]);
        for (int i = 0; i < 6; ++i) m4_frame.push_back(my_mac_[i]);
        for (int i = 0; i < 6; ++i) m4_frame.push_back(connected_bssid_[i]);
        m4_frame.push_back(0x00);
        m4_frame.push_back(0x00);
        m4_frame.push_back(0x00);
        m4_frame.push_back(0x00);

        uint8_t llc[] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
        for (uint8_t b : llc) m4_frame.push_back(b);

        size_t m4_eapol_start = m4_frame.size();
        m4_frame.push_back(0x02);
        m4_frame.push_back(0x03);
        m4_frame.push_back(0x00);
        m4_frame.push_back(0x5F);

        m4_frame.push_back(0x02);
        // Key Info: Pairwise | MIC | Secure -> 0x030A
        m4_frame.push_back(0x03);
        m4_frame.push_back(0x0A);
        m4_frame.push_back(0x00);
        m4_frame.push_back(0x00); // Key length 0

        for (int i = 7; i >= 0; --i) {
            m4_frame.push_back(static_cast<uint8_t>((replay_counter >> (i * 8)) & 0xFF));
        }

        for (int i = 0; i < 64; ++i) m4_frame.push_back(0x00); // Nonce + IV + RSC + Res

        size_t m4_mic_pos = m4_frame.size();
        for (int i = 0; i < 16; ++i) m4_frame.push_back(0x00);

        m4_frame.push_back(0x00);
        m4_frame.push_back(0x00);

        uint8_t m4_digest[20];
        rtl_crypto::hmac_sha1(ptk_, 16,
                              m4_frame.data() + m4_eapol_start,
                              m4_frame.size() - m4_eapol_start,
                              m4_digest);
        for (int i = 0; i < 16; ++i) {
            m4_frame[m4_mic_pos + i] = m4_digest[i];
        }

        transmit_raw_frame(Q_BE, m4_frame.data(), m4_frame.size());

        // State is now CONNECTED!
        state_ = WIFI_CONNECTED;
    }
}

bool SimulatedDevice::send_ethernet_packet(const uint8_t* eth_packet, size_t len) {
    if (!eth_packet || len < 14 || state_ != WIFI_CONNECTED) return false;

    // Ethernet II Header: DA (6), SA (6), EtherType (2)
    const uint8_t* da = eth_packet;
    const uint8_t* sa = eth_packet + 6;
    uint16_t ether_type = (static_cast<uint16_t>(eth_packet[12]) << 8) | eth_packet[13];
    const uint8_t* payload = eth_packet + 14;
    size_t payload_len = len - 14;

    std::vector<uint8_t> frame;

    // 1. MAC Header (26 bytes with QoS Control)
    frame.push_back(0x88);
    frame.push_back(0x01); // To DS = 1
    frame.push_back(0x00);
    frame.push_back(0x00);
    for (int i = 0; i < 6; ++i) frame.push_back(connected_bssid_[i]); // Addr1: BSSID
    for (int i = 0; i < 6; ++i) frame.push_back(sa[i]);                 // Addr2: SA
    for (int i = 0; i < 6; ++i) frame.push_back(da[i]);                 // Addr3: DA
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(0x00); // QoS Control
    frame.push_back(0x00);

    // 2. CCMP Header (8 bytes)
    uint64_t pn = tx_pn_++;
    uint8_t ccmp_hdr[8];
    ccmp_hdr[0] = static_cast<uint8_t>(pn & 0xFF);
    ccmp_hdr[1] = static_cast<uint8_t>((pn >> 8) & 0xFF);
    ccmp_hdr[2] = 0x00;
    ccmp_hdr[3] = 0x20; // ExtIV = 1
    ccmp_hdr[4] = static_cast<uint8_t>((pn >> 16) & 0xFF);
    ccmp_hdr[5] = static_cast<uint8_t>((pn >> 24) & 0xFF);
    ccmp_hdr[6] = static_cast<uint8_t>((pn >> 32) & 0xFF);
    ccmp_hdr[7] = static_cast<uint8_t>((pn >> 40) & 0xFF);
    for (uint8_t b : ccmp_hdr) frame.push_back(b);

    // 3. Nonce (13 bytes): Priority (0) || Addr2 (SA, 6 bytes) || PN (6 bytes big-endian)
    uint8_t nonce[13];
    nonce[0] = 0x00;
    for (int i = 0; i < 6; ++i) nonce[1 + i] = sa[i];
    for (int i = 0; i < 6; ++i) {
        nonce[7 + i] = static_cast<uint8_t>((pn >> ((5 - i) * 8)) & 0xFF);
    }

    // 4. AAD
    std::vector<uint8_t> aad(26);
    std::memcpy(aad.data(), frame.data(), 26);
    aad[0] &= 0x8F;
    aad[1] &= 0xC7;
    aad[22] = 0x00;
    aad[23] = 0x00;
    aad[24] &= 0x0F;
    aad[25] = 0x00;

    // 5. Plaintext: RFC 1042 LLC/SNAP (8 bytes) + payload
    std::vector<uint8_t> plaintext = {
        0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00,
        static_cast<uint8_t>((ether_type >> 8) & 0xFF),
        static_cast<uint8_t>(ether_type & 0xFF)
    };
    if (payload && payload_len > 0) {
        plaintext.insert(plaintext.end(), payload, payload + payload_len);
    }

    // 6. CCMP Encrypt under TK (ptk_ + 32)
    std::vector<uint8_t> ciphertext(plaintext.size());
    uint8_t mic[8];
    rtl_crypto::ccmp_encrypt(ptk_ + 32, nonce, aad.data(), aad.size(),
                             plaintext.data(), plaintext.size(),
                             ciphertext.data(), mic);

    for (uint8_t b : ciphertext) frame.push_back(b);
    for (int i = 0; i < 8; ++i) frame.push_back(mic[i]);

    // 7. FCS
    uint32_t crc = rtl_crypto::crc32_80211(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    return transmit_raw_frame(Q_BE, frame.data(), frame.size());
}

bool SimulatedDevice::receive_ethernet_packet(const uint8_t* frame, size_t len,
                                             std::vector<uint8_t>& out_eth_packet) {
    if (!frame || len < (26 + 8 + 8 + 8 + 4)) return false; // MAC(26) + CCMP(8) + LLC(8) + MIC(8) + FCS(4)

    // Check Frame Control (QoS Data)
    if (frame[0] != 0x88) return false;

    // Extract PN from CCMP Header
    uint64_t pn = static_cast<uint64_t>(frame[26]) |
                 (static_cast<uint64_t>(frame[27]) << 8) |
                 (static_cast<uint64_t>(frame[30]) << 16) |
                 (static_cast<uint64_t>(frame[31]) << 24) |
                 (static_cast<uint64_t>(frame[32]) << 32) |
                 (static_cast<uint64_t>(frame[33]) << 40);

    // Replay attack check: PN must be strictly greater than last_rx_pn_
    if (pn <= last_rx_pn_) {
        return false; // Replay detected!
    }

    // Construct Nonce
    uint8_t nonce[13];
    nonce[0] = 0x00;
    // Addr2 (BSSID/AP MAC) is at frame + 10
    for (int i = 0; i < 6; ++i) nonce[1 + i] = frame[10 + i];
    for (int i = 0; i < 6; ++i) {
        nonce[7 + i] = static_cast<uint8_t>((pn >> ((5 - i) * 8)) & 0xFF);
    }

    // AAD (26 bytes)
    std::vector<uint8_t> aad(26);
    std::memcpy(aad.data(), frame, 26);
    aad[0] &= 0x8F;
    aad[1] &= 0xC7;
    aad[22] = 0x00;
    aad[23] = 0x00;
    aad[24] &= 0x0F;
    aad[25] = 0x00;

    // Ciphertext and MIC
    size_t cipher_len = len - 26 - 8 - 8 - 4; // Total - MAC - CCMP - MIC - FCS
    const uint8_t* ciphertext = frame + 26 + 8;
    const uint8_t* mic = frame + 26 + 8 + cipher_len;

    std::vector<uint8_t> plaintext(cipher_len);
    if (!rtl_crypto::ccmp_decrypt(ptk_ + 32, nonce, aad.data(), aad.size(),
                                  ciphertext, cipher_len, mic, plaintext.data())) {
        return false; // Decryption / MIC check failed!
    }

    // Update replay counter
    last_rx_pn_ = pn;

    // Validate LLC/SNAP (8 bytes: AA AA 03 00 00 00 EtherType)
    if (plaintext.size() < 8 || plaintext[0] != 0xAA || plaintext[1] != 0xAA || plaintext[2] != 0x03) {
        return false;
    }

    uint16_t ether_type = (static_cast<uint16_t>(plaintext[6]) << 8) | plaintext[7];

    // Reconstruct Ethernet II packet: DA (Addr1: frame + 4), SA (Addr3: frame + 16), EtherType, Payload
    out_eth_packet.clear();
    for (int i = 0; i < 6; ++i) out_eth_packet.push_back(frame[4 + i]);  // DA
    for (int i = 0; i < 6; ++i) out_eth_packet.push_back(frame[16 + i]); // SA
    out_eth_packet.push_back(static_cast<uint8_t>((ether_type >> 8) & 0xFF));
    out_eth_packet.push_back(static_cast<uint8_t>(ether_type & 0xFF));

    // Append payload (after LLC/SNAP)
    if (plaintext.size() > 8) {
        out_eth_packet.insert(out_eth_packet.end(), plaintext.begin() + 8, plaintext.end());
    }

    return true;
}

} // namespace rtl_mock
