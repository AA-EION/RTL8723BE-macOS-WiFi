#include "mock_dma.hpp"
#include <cstring>
#include <stdexcept>

namespace rtl_mock {

MockDMA::MockDMA(MockPCIMmio& mmio) : mmio_(mmio) {
    mmio_.set_doorbell_callback([this](uint16_t mask) {
        this->process_tx_doorbell(mask);
    });
    reset();
}

void MockDMA::reset() {
    tx_rings_.clear();
    rx_ring_base_ = 0;
    rx_ring_count_ = 0;
    rx_hw_index_ = 0;
    rx_drop_count_ = 0;
    tx_history_.clear();
    free_all_phys_mem();

    // Initialize default queue mappings
    tx_rings_[Q_BK]   = {0, 0, 0, 1 << 0, IMR_BKDOK};
    tx_rings_[Q_BE]   = {0, 0, 0, 1 << 1, IMR_BEDOK};
    tx_rings_[Q_VI]   = {0, 0, 0, 1 << 2, IMR_VIDOK};
    tx_rings_[Q_VO]   = {0, 0, 0, 1 << 3, IMR_VODOK};
    tx_rings_[Q_BCN]  = {0, 0, 0, 1 << 4, IMR_BDOK};
    tx_rings_[Q_MGNT] = {0, 0, 0, 1 << 6, IMR_MGNTDOK};
    tx_rings_[Q_HIGH] = {0, 0, 0, 1 << 7, IMR_HIGHDOK};
}

uint64_t MockDMA::alloc_phys_mem(size_t size, size_t alignment) {
    if (alignment == 0) alignment = 1;
    if ((next_phys_addr_ % alignment) != 0) {
        next_phys_addr_ += alignment - (next_phys_addr_ % alignment);
    }

    uint64_t allocated_addr = next_phys_addr_;
    next_phys_addr_ += size;

    auto chunk = std::make_unique<MemChunk>();
    chunk->phys_addr = allocated_addr;
    chunk->size = size;
    chunk->buffer.resize(size, 0);

    memory_pool_.push_back(std::move(chunk));
    return allocated_addr;
}

void* MockDMA::phys_to_virt(uint64_t phys_addr) {
    for (const auto& chunk : memory_pool_) {
        if (phys_addr >= chunk->phys_addr && phys_addr < (chunk->phys_addr + chunk->size)) {
            size_t offset = static_cast<size_t>(phys_addr - chunk->phys_addr);
            return chunk->buffer.data() + offset;
        }
    }
    return nullptr;
}

const void* MockDMA::phys_to_virt(uint64_t phys_addr) const {
    for (const auto& chunk : memory_pool_) {
        if (phys_addr >= chunk->phys_addr && phys_addr < (chunk->phys_addr + chunk->size)) {
            size_t offset = static_cast<size_t>(phys_addr - chunk->phys_addr);
            return chunk->buffer.data() + offset;
        }
    }
    return nullptr;
}

void MockDMA::free_all_phys_mem() {
    memory_pool_.clear();
    next_phys_addr_ = 0x10000000ULL;
}

void MockDMA::setup_tx_ring(QueueId q_id, uint64_t ring_phys_base, size_t count) {
    if (tx_rings_.find(q_id) != tx_rings_.end()) {
        tx_rings_[q_id].phys_base = ring_phys_base;
        tx_rings_[q_id].count = count;
        tx_rings_[q_id].hw_index = 0;
    }
}

void MockDMA::setup_rx_ring(uint64_t ring_phys_base, size_t count) {
    rx_ring_base_ = ring_phys_base;
    rx_ring_count_ = count;
    rx_hw_index_ = 0;
    rx_drop_count_ = 0;
}

void MockDMA::process_tx_doorbell(uint16_t doorbell_mask) {
    for (auto& pair : tx_rings_) {
        QueueId q_id = pair.first;
        TxRingState& ring = pair.second;

        if ((doorbell_mask & ring.doorbell_bit) == 0) {
            continue;
        }

        if (ring.phys_base == 0 || ring.count == 0) {
            continue;
        }

        size_t processed_count = 0;
        while (processed_count < ring.count) {
            uint64_t desc_phys = ring.phys_base + ring.hw_index * sizeof(TxDesc40);
            TxDesc40* desc = reinterpret_cast<TxDesc40*>(phys_to_virt(desc_phys));
            if (!desc) break;

            if (!desc->get_own()) {
                // Not owned by DMA (CPU has not written packet or reached tail)
                break;
            }

            // Packet ready for transmission
            uint16_t pktsize = desc->get_pktsize();
            uint8_t offset = desc->get_offset();
            uint64_t buf_addr = desc->get_buffer_addr();

            TxPacketRecord record;
            record.queue_id = static_cast<uint8_t>(q_id);
            record.pktsize = pktsize;
            record.offset = offset;
            record.queuesel = desc->get_queuesel();

            const uint8_t* payload_ptr = reinterpret_cast<const uint8_t*>(phys_to_virt(buf_addr));
            if (payload_ptr && pktsize > 0) {
                record.data.assign(payload_ptr, payload_ptr + pktsize);
            }

            tx_history_.push_back(record);
            if (tx_cb_) {
                tx_cb_(record);
            }

            // Relinquish ownership back to CPU
            desc->set_own(false);

            // Assert queue TX_OK interrupt
            mmio_.assert_interrupt(ring.isr_ok_bit);

            // Advance ring pointer
            ring.hw_index = (ring.hw_index + 1) % ring.count;
            processed_count++;
        }
    }
}

bool MockDMA::inject_rx_frame(const uint8_t* frame_data, size_t frame_len,
                             bool crc_err, bool icv_err, uint8_t shift) {
    if (rx_ring_base_ == 0 || rx_ring_count_ == 0) {
        return false;
    }

    uint64_t desc_phys = rx_ring_base_ + rx_hw_index_ * sizeof(RxDesc32);
    RxDesc32* desc = reinterpret_cast<RxDesc32*>(phys_to_virt(desc_phys));
    if (!desc) {
        return false;
    }

    // Check if DMA owns the descriptor (OWN == 1)
    if (!desc->get_own()) {
        // Buffer starvation! CPU has not processed earlier packets
        rx_drop_count_++;
        mmio_.assert_interrupt(IMR_RDU);
        return false;
    }

    uint64_t buf_addr = desc->get_buffer_addr();
    uint8_t* dest_buf = reinterpret_cast<uint8_t*>(phys_to_virt(buf_addr));
    if (!dest_buf) {
        return false;
    }

    // Copy frame bytes into host buffer with specified shift offset
    if (frame_data && frame_len > 0) {
        std::memcpy(dest_buf + shift, frame_data, frame_len);
    }

    // Populate RX descriptor fields
    desc->set_length(static_cast<uint16_t>(frame_len));
    desc->set_crc32_err(crc_err);
    desc->set_icv_err(icv_err);
    desc->set_shift(shift);
    desc->set_drv_infosize(0);

    // Release ownership to CPU
    desc->set_own(false);

    // Raise RX OK interrupt
    mmio_.assert_interrupt(IMR_ROK);

    // Advance ring index (wrap if EOR flag is set or reached end)
    if (desc->get_eor()) {
        rx_hw_index_ = 0;
    } else {
        rx_hw_index_ = (rx_hw_index_ + 1) % rx_ring_count_;
    }

    return true;
}

size_t MockDMA::get_tx_hw_index(QueueId q_id) const {
    auto it = tx_rings_.find(q_id);
    if (it != tx_rings_.end()) {
        return it->second.hw_index;
    }
    return 0;
}

} // namespace rtl_mock
