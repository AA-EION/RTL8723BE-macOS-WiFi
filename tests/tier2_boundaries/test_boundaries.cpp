#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/mock_crypto.hpp"
#include "../mock/mock_packet_injector.hpp"
#include "../mock/simulated_device.hpp"

using namespace rtl_mock;

// Boundary Test 1: Zero-Length Frame & Null Buffer Handling
REGISTER_TEST(Tier2_Boundaries, test_bound_zero_length_frame) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    // Sending zero-length frame should be safely ignored or rejected without crash
    ASSERT_FALSE(dev.send_ethernet_packet(nullptr, 0));

    // Processing zero-length inbound packet
    dev.process_inbound_packet(nullptr, 0);
    ASSERT_EQ(dev.get_scan_results().size(), 0);
}

// Boundary Test 2: Standard Maximum Ethernet MTU (1514 Bytes)
REGISTER_TEST(Tier2_Boundaries, test_bound_max_mtu_ethernet) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));
    dev.set_state(WIFI_CONNECTED);

    // 1514-byte Ethernet packet (14 header + 1500 payload)
    std::vector<uint8_t> eth_pkt(1514);
    // DA
    for (int i = 0; i < 6; ++i) eth_pkt[i] = 0xAA;
    // SA
    for (int i = 0; i < 6; ++i) eth_pkt[6 + i] = 0xBB;
    // EtherType (0x0800 IPv4)
    eth_pkt[12] = 0x08;
    eth_pkt[13] = 0x00;
    // Payload
    for (size_t i = 14; i < 1514; ++i) eth_pkt[i] = static_cast<uint8_t>(i & 0xFF);

    ASSERT_TRUE(dev.send_ethernet_packet(eth_pkt.data(), eth_pkt.size()));

    const auto& captured = dma.get_transmitted_packets();
    ASSERT_EQ(captured.size(), 1);
    // 802.11 QoS MAC(26) + CCMP(8) + LLC(8) + Payload(1500) + MIC(8) + FCS(4) = 1554 bytes
    ASSERT_EQ(captured[0].pktsize, 1554);
}

// Boundary Test 3: Maximum 802.11 MSDU Payload (2304 Bytes)
REGISTER_TEST(Tier2_Boundaries, test_bound_max_msdu_80211) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));
    dev.set_state(WIFI_CONNECTED);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    // 2304 bytes payload (max 802.11 MSDU limit)
    std::vector<uint8_t> max_payload(2304);
    for (size_t i = 0; i < 2304; ++i) max_payload[i] = static_cast<uint8_t>((i ^ 0x5A) & 0xFF);

    auto frame = injector.build_ccmp_data(bssid, dev.get_mac_address(), bssid,
                                         10, dev.get_tk(), max_payload.data(), max_payload.size());

    std::vector<uint8_t> eth_out;
    ASSERT_TRUE(dev.receive_ethernet_packet(frame.data(), frame.size(), eth_out));
    // Header(14) + Payload(2304) = 2318
    ASSERT_EQ(eth_out.size(), 2318);
    ASSERT_MEMEQ(eth_out.data() + 14, max_payload.data(), 2304);
}

// Boundary Test 4: Corrupted 802.11 CRC32 / FCS Error Handling
REGISTER_TEST(Tier2_Boundaries, test_bound_corrupted_crc32) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    auto frame = injector.build_beacon("GoodNet", bssid, 6);

    // Inject with crc_err flag set to true
    ASSERT_TRUE(injector.inject_frame(frame, true, false));

    std::vector<uint8_t> polled_frame;
    RxDesc32 desc;
    ASSERT_TRUE(dev.poll_rx_frame(polled_frame, desc));

    // RX descriptor must reflect CRC error flag
    ASSERT_TRUE(desc.get_crc32_err());
}

// Boundary Test 5: Corrupted CCMP MIC Authentication Failure
REGISTER_TEST(Tier2_Boundaries, test_bound_corrupted_ccmp_mic) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));
    dev.set_state(WIFI_CONNECTED);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t payload[] = "Tampered Network Data";

    auto frame = injector.build_ccmp_data(bssid, dev.get_mac_address(), bssid,
                                         20, dev.get_tk(), payload, sizeof(payload));

    // Tamper with one byte in the ciphertext payload
    frame[36] ^= 0xFF;

    std::vector<uint8_t> eth_out;
    // Decryption must fail MIC check and drop packet cleanly
    ASSERT_FALSE(dev.receive_ethernet_packet(frame.data(), frame.size(), eth_out));
    ASSERT_TRUE(eth_out.empty());
}

// Boundary Test 6: RX Ring Descriptor Starvation (RDU Interrupt)
REGISTER_TEST(Tier2_Boundaries, test_bound_rx_ring_starvation) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);
    PacketInjector injector(dma);

    const size_t small_ring = 2;
    ASSERT_TRUE(dev.init_dma_rings(16, small_ring));

    bool rdu_fired = false;
    mmio.set_interrupt_callback([&](uint32_t hisr) {
        if (hisr & IMR_RDU) {
            rdu_fired = true;
        }
    });
    mmio.write32(REG_HIMR, IMR_RDU);

    uint8_t bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    auto frame = injector.build_beacon("TestNet", bssid, 6);

    // Slot 0 filled
    ASSERT_TRUE(injector.inject_frame(frame));
    // Slot 1 filled
    ASSERT_TRUE(injector.inject_frame(frame));

    // Both slots occupied and not yet polled by host. Injecting third packet triggers starvation!
    bool injected = injector.inject_frame(frame);
    ASSERT_FALSE(injected);
    ASSERT_TRUE(rdu_fired);
    ASSERT_EQ(dma.get_rx_drop_count(), 1);
}

// Boundary Test 7: TX Ring Full Backpressure Detection
REGISTER_TEST(Tier2_Boundaries, test_bound_tx_ring_full) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    const size_t small_tx_ring = 3;
    ASSERT_TRUE(dev.init_dma_rings(small_tx_ring, 16));

    // Simulate all descriptors in TX ring having OWN = 1 (hardware busy)
    uint32_t beq_desa = mmio.read32(REG_BEQ_DESA);
    for (size_t i = 0; i < small_tx_ring; ++i) {
        TxDesc40* desc = reinterpret_cast<TxDesc40*>(dma.phys_to_virt(beq_desa + i * sizeof(TxDesc40)));
        desc->set_own(true);
    }

    uint8_t dummy[32] = {0};
    // Transmitting when ring is full must fail cleanly without corrupting memory
    ASSERT_FALSE(dev.transmit_raw_frame(Q_BE, dummy, sizeof(dummy)));
}

// Boundary Test 8: Truncated / Corrupt eFuse PG Stream
REGISTER_TEST(Tier2_Boundaries, test_bound_corrupt_efuse) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    // Truncated PG stream: Header indicates 4 words follow, but stream ends prematurely
    std::vector<uint8_t> truncated_pg = {
        0xD0, // Header: word mask 0x0F (4 words expected)
        0x11  // Only 1 byte provided!
    };

    CalibData calib;
    // Parser must safely abort and not read past buffer or crash
    ASSERT_TRUE(dev.hw_decode_pg_stream(truncated_pg.data(), truncated_pg.size(), calib));
    // Default fallback values should be applied for missing fields
    ASSERT_EQ(calib.crystal_cap, 0x20);
    ASSERT_EQ(calib.thermal_meter, 0x1A);
}

// Boundary Test 9: Malformed Firmware Binary Header Signature Mismatch
REGISTER_TEST(Tier2_Boundaries, test_bound_invalid_fw_sig) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.hw_power_on());

    // Firmware with corrupted signature 0xFFFF instead of 0x5301
    std::vector<uint8_t> corrupted_fw(32 + 1024, 0);
    corrupted_fw[0] = 0xFF;
    corrupted_fw[1] = 0xFF;

    ASSERT_FALSE(dev.hw_download_firmware(corrupted_fw.data(), corrupted_fw.size()));
    ASSERT_FALSE(mmio.is_mcu_fw_ready());
}

// Boundary Test 10: Out-of-Bounds MMIO Register Access Safety
REGISTER_TEST(Tier2_Boundaries, test_bound_unaligned_mmio) {
    MockPCIMmio mmio;

    // Normal access within 16KB window
    mmio.write32(0x0100, 0x12345678);
    ASSERT_EQ(mmio.read32(0x0100), 0x12345678);

    // Out-of-bounds access past 16KB (0x4000)
    mmio.write32(0x4000, 0xDEADBEEF);
    ASSERT_EQ(mmio.read32(0x4000), 0xFFFFFFFF);
    ASSERT_EQ(mmio.read8(0x5000), 0xFF);
    ASSERT_EQ(mmio.read16(0x6000), 0xFFFF);
}
