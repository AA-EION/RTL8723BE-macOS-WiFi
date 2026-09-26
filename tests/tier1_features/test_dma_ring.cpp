#include "../mock/test_framework.hpp"
#include "../mock/mock_pci_mmio.hpp"
#include "../mock/mock_dma.hpp"
#include "../mock/simulated_device.hpp"
#include <vector>
#include <cstring>

using namespace rtl_mock;

// Test 1: 40-Byte TX Descriptor Layout & 256-Byte Ring Alignment
REGISTER_TEST(Tier1_DMA, test_dma_tx_desc_layout) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_EQ(sizeof(TxDesc40), 40);
    ASSERT_EQ(sizeof(RxDesc32), 32);

    ASSERT_TRUE(dev.init_dma_rings(64, 64));

    // Verify ring addresses in MMIO BAR2 are aligned to 256 bytes
    uint32_t beq_desa = mmio.read32(REG_BEQ_DESA);
    uint32_t rx_desa = mmio.read32(REG_RX_DESA);

    ASSERT_NE(beq_desa, 0);
    ASSERT_NE(rx_desa, 0);
    ASSERT_EQ(beq_desa % 256, 0);
    ASSERT_EQ(rx_desa % 256, 0);
}

// Test 2: TX Queue Doorbell Trigger & OWN Bit Arbitration
REGISTER_TEST(Tier1_DMA, test_dma_tx_doorbell_own) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    // Track TX interrupts
    bool tx_ok_raised = false;
    mmio.set_interrupt_callback([&](uint32_t hisr) {
        if (hisr & IMR_BEDOK) {
            tx_ok_raised = true;
        }
    });
    mmio.write32(REG_HIMR, IMR_BEDOK); // Unmask BE TX interrupt

    uint8_t dummy_packet[64];
    for (int i = 0; i < 64; ++i) dummy_packet[i] = static_cast<uint8_t>(i);

    ASSERT_TRUE(dev.transmit_raw_frame(Q_BE, dummy_packet, sizeof(dummy_packet)));

    // Verify mock DMA captured packet
    const auto& captured = dma.get_transmitted_packets();
    ASSERT_EQ(captured.size(), 1);
    ASSERT_EQ(captured[0].pktsize, 64);
    ASSERT_MEMEQ(captured[0].data.data(), dummy_packet, 64);

    // Verify interrupt was triggered and cleared
    ASSERT_TRUE(tx_ok_raised);
    // Verify write-1-to-clear on HISR
    mmio.write32(REG_HISR, IMR_BEDOK);
    ASSERT_EQ((mmio.read32(REG_HISR) & IMR_BEDOK), 0);
}

// Test 3: TX Ring Circular Wrap-Around
REGISTER_TEST(Tier1_DMA, test_dma_tx_ring_wrap) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    const size_t ring_size = 8;
    ASSERT_TRUE(dev.init_dma_rings(ring_size, 16));

    uint8_t dummy_packet[32] = {0xAA};

    // Transmit 20 packets (more than ring_size 8)
    for (size_t i = 0; i < 20; ++i) {
        dummy_packet[0] = static_cast<uint8_t>(i);
        ASSERT_TRUE(dev.transmit_raw_frame(Q_BE, dummy_packet, sizeof(dummy_packet)));
    }

    const auto& captured = dma.get_transmitted_packets();
    ASSERT_EQ(captured.size(), 20);

    // Verify hardware pointer wrapped properly (20 % 8 = 4)
    ASSERT_EQ(dma.get_tx_hw_index(Q_BE), 4);
}

// Test 4: RX Ring End-Of-Ring (EOR) Bit Circular Wrapping
REGISTER_TEST(Tier1_DMA, test_dma_rx_eor_wrap) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    const size_t ring_size = 4;
    ASSERT_TRUE(dev.init_dma_rings(16, ring_size));

    // Verify EOR flag set on last descriptor
    uint32_t rx_desa = mmio.read32(REG_RX_DESA);
    RxDesc32* last_desc = reinterpret_cast<RxDesc32*>(dma.phys_to_virt(rx_desa + 3 * sizeof(RxDesc32)));
    ASSERT_TRUE(last_desc->get_eor());

    // Inject packets up to and past ring boundary
    uint8_t test_frame[60] = {0};
    for (size_t i = 0; i < 6; ++i) {
        test_frame[0] = static_cast<uint8_t>(i);
        ASSERT_TRUE(dma.inject_rx_frame(test_frame, sizeof(test_frame)));

        std::vector<uint8_t> polled_frame;
        RxDesc32 polled_desc;
        ASSERT_TRUE(dev.poll_rx_frame(polled_frame, polled_desc));
        ASSERT_EQ(polled_frame[0], static_cast<uint8_t>(i));
    }

    // Hardware pointer should be at 2 (6 % 4 = 2)
    ASSERT_EQ(dma.get_rx_hw_index(), 2);
}

// Test 5: RX Packet Reception, Length, Shift Offset & OWN Bit Handling
REGISTER_TEST(Tier1_DMA, test_dma_rx_packet_reception) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    ASSERT_TRUE(dev.init_dma_rings(16, 16));

    uint8_t test_payload[128];
    for (int i = 0; i < 128; ++i) test_payload[i] = static_cast<uint8_t>(i * 3);

    // Inject frame with 2-byte shift (to align IP header to 4 bytes)
    ASSERT_TRUE(dma.inject_rx_frame(test_payload, sizeof(test_payload), false, false, 2));

    std::vector<uint8_t> polled;
    RxDesc32 desc;
    ASSERT_TRUE(dev.poll_rx_frame(polled, desc));

    ASSERT_EQ(desc.get_length(), 128);
    ASSERT_EQ(desc.get_shift(), 2);
    ASSERT_FALSE(desc.get_crc32_err());
    ASSERT_FALSE(desc.get_icv_err());
    ASSERT_EQ(polled.size(), 128);
    ASSERT_MEMEQ(polled.data(), test_payload, 128);
}

// Test 6: Multi-Queue Priority Arbitration (VO, VI, BE, BK)
REGISTER_TEST(Tier1_DMA, test_dma_tx_priority_queues) {
    MockPCIMmio mmio;
    MockDMA dma(mmio);
    SimulatedDevice dev(mmio, dma);

    // Setup all 4 priority rings
    uint64_t vo_ring = dma.alloc_phys_mem(16 * sizeof(TxDesc40), 256);
    uint64_t vi_ring = dma.alloc_phys_mem(16 * sizeof(TxDesc40), 256);
    uint64_t be_ring = dma.alloc_phys_mem(16 * sizeof(TxDesc40), 256);
    uint64_t bk_ring = dma.alloc_phys_mem(16 * sizeof(TxDesc40), 256);

    dma.setup_tx_ring(Q_VO, vo_ring, 16);
    dma.setup_tx_ring(Q_VI, vi_ring, 16);
    dma.setup_tx_ring(Q_BE, be_ring, 16);
    dma.setup_tx_ring(Q_BK, bk_ring, 16);

    // Write a packet into each queue descriptor directly
    auto enqueue_desc = [&](uint64_t ring_base, QueueId q_id, uint8_t tag) {
        TxDesc40* desc = reinterpret_cast<TxDesc40*>(dma.phys_to_virt(ring_base));
        uint64_t buf = dma.alloc_phys_mem(64, 256);
        uint8_t* p = reinterpret_cast<uint8_t*>(dma.phys_to_virt(buf));
        p[0] = tag;
        desc->set_buffer_addr(buf);
        desc->set_pktsize(64);
        desc->set_queuesel(static_cast<uint8_t>(q_id));
        desc->set_own(true);
    };

    enqueue_desc(bk_ring, Q_BK, 0x10);
    enqueue_desc(be_ring, Q_BE, 0x20);
    enqueue_desc(vi_ring, Q_VI, 0x30);
    enqueue_desc(vo_ring, Q_VO, 0x40);

    // Trigger doorbell simultaneously for BK, BE, VI, VO: bits 0, 1, 2, 3 = 0x000F
    mmio.write16(REG_PCIE_CTRL_REG, 0x000F);

    const auto& captured = dma.get_transmitted_packets();
    ASSERT_EQ(captured.size(), 4);

    // Verify all 4 queues were processed
    bool has_bk = false, has_be = false, has_vi = false, has_vo = false;
    for (const auto& pkt : captured) {
        if (pkt.queue_id == Q_BK) has_bk = true;
        if (pkt.queue_id == Q_BE) has_be = true;
        if (pkt.queue_id == Q_VI) has_vi = true;
        if (pkt.queue_id == Q_VO) has_vo = true;
    }
    ASSERT_TRUE(has_bk);
    ASSERT_TRUE(has_be);
    ASSERT_TRUE(has_vi);
    ASSERT_TRUE(has_vo);
}
