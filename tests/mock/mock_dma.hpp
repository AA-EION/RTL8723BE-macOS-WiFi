#pragma once

#include "mock_pci_mmio.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <functional>
#include <map>

namespace rtl_mock {

// 40-Byte TX Descriptor Layout (10 dwords = 40 bytes)
struct alignas(4) TxDesc40 {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t dw4;
    uint32_t dw5;
    uint32_t dw6_reserved;
    uint32_t dw7;
    uint32_t dw8_txbuffaddr;
    uint32_t dw9_txbuffaddr64;

    // Helper Accessors
    uint16_t get_pktsize() const { return dw0 & 0xFFFF; }
    void set_pktsize(uint16_t sz) { dw0 = (dw0 & ~0xFFFF) | (sz & 0xFFFF); }

    uint8_t get_offset() const { return (dw0 >> 16) & 0xFF; }
    void set_offset(uint8_t off) { dw0 = (dw0 & ~(0xFF << 16)) | ((off & 0xFF) << 16); }

    bool get_own() const { return (dw0 & (1U << 31)) != 0; }
    void set_own(bool own) { if (own) dw0 |= (1U << 31); else dw0 &= ~(1U << 31); }

    bool get_firstseg() const { return (dw0 & (1U << 27)) != 0; }
    void set_firstseg(bool v) { if (v) dw0 |= (1U << 27); else dw0 &= ~(1U << 27); }

    bool get_lastseg() const { return (dw0 & (1U << 26)) != 0; }
    void set_lastseg(bool v) { if (v) dw0 |= (1U << 26); else dw0 &= ~(1U << 26); }

    uint8_t get_queuesel() const { return (dw1 >> 8) & 0x1F; }
    void set_queuesel(uint8_t q) { dw1 = (dw1 & ~(0x1F << 8)) | ((q & 0x1F) << 8); }

    uint64_t get_buffer_addr() const {
        return static_cast<uint64_t>(dw8_txbuffaddr) | (static_cast<uint64_t>(dw9_txbuffaddr64) << 32);
    }
    void set_buffer_addr(uint64_t addr) {
        dw8_txbuffaddr = static_cast<uint32_t>(addr & 0xFFFFFFFF);
        dw9_txbuffaddr64 = static_cast<uint32_t>((addr >> 32) & 0xFFFFFFFF);
    }
};
static_assert(sizeof(TxDesc40) == 40, "TxDesc40 must be exactly 40 bytes");

// 32-Byte RX Descriptor Layout (8 dwords = 32 bytes)
struct alignas(4) RxDesc32 {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t dw4_reserved;
    uint32_t dw5_tsfl;
    uint32_t dw6_bufferaddress;
    uint32_t dw7_bufferaddress64;

    // Helper Accessors
    uint16_t get_length() const { return dw0 & 0x3FFF; }
    void set_length(uint16_t len) { dw0 = (dw0 & ~0x3FFF) | (len & 0x3FFF); }

    bool get_crc32_err() const { return (dw0 & (1U << 14)) != 0; }
    void set_crc32_err(bool err) { if (err) dw0 |= (1U << 14); else dw0 &= ~(1U << 14); }

    bool get_icv_err() const { return (dw0 & (1U << 15)) != 0; }
    void set_icv_err(bool err) { if (err) dw0 |= (1U << 15); else dw0 &= ~(1U << 15); }

    uint8_t get_drv_infosize() const { return (dw0 >> 16) & 0x0F; }
    void set_drv_infosize(uint8_t sz) { dw0 = (dw0 & ~(0x0F << 16)) | ((sz & 0x0F) << 16); }

    uint8_t get_security() const { return (dw0 >> 20) & 0x07; }
    void set_security(uint8_t sec) { dw0 = (dw0 & ~(0x07 << 20)) | ((sec & 0x07) << 20); }

    uint8_t get_shift() const { return (dw0 >> 24) & 0x03; }
    void set_shift(uint8_t s) { dw0 = (dw0 & ~(0x03 << 24)) | ((s & 0x03) << 24); }

    bool get_eor() const { return (dw0 & (1U << 30)) != 0; }
    void set_eor(bool eor) { if (eor) dw0 |= (1U << 30); else dw0 &= ~(1U << 30); }

    bool get_own() const { return (dw0 & (1U << 31)) != 0; }
    void set_own(bool own) { if (own) dw0 |= (1U << 31); else dw0 &= ~(1U << 31); }

    uint64_t get_buffer_addr() const {
        return static_cast<uint64_t>(dw6_bufferaddress) | (static_cast<uint64_t>(dw7_bufferaddress64) << 32);
    }
    void set_buffer_addr(uint64_t addr) {
        dw6_bufferaddress = static_cast<uint32_t>(addr & 0xFFFFFFFF);
        dw7_bufferaddress64 = static_cast<uint32_t>((addr >> 32) & 0xFFFFFFFF);
    }
};
static_assert(sizeof(RxDesc32) == 32, "RxDesc32 must be exactly 32 bytes");

// Captured Transmitted Packet
struct TxPacketRecord {
    uint8_t queue_id;
    std::vector<uint8_t> data;
    uint16_t pktsize;
    uint8_t offset;
    uint8_t queuesel;
};

// Queue IDs
enum QueueId {
    Q_BK    = 0,
    Q_BE    = 1,
    Q_VI    = 2,
    Q_VO    = 3,
    Q_BCN   = 4,
    Q_MGNT  = 6,
    Q_HIGH  = 7
};

class MockDMA {
public:
    explicit MockDMA(MockPCIMmio& mmio);
    ~MockDMA() = default;

    void reset();

    // Physical Memory Manager
    // Allocates simulated host physical memory and returns 64-bit physical address
    uint64_t alloc_phys_mem(size_t size, size_t alignment = 256);
    void* phys_to_virt(uint64_t phys_addr);
    const void* phys_to_virt(uint64_t phys_addr) const;
    void free_all_phys_mem();

    // Ring Configuration
    void setup_tx_ring(QueueId q_id, uint64_t ring_phys_base, size_t count);
    void setup_rx_ring(uint64_t ring_phys_base, size_t count);

    // TX Processing
    void process_tx_doorbell(uint16_t doorbell_mask);
    const std::vector<TxPacketRecord>& get_transmitted_packets() const { return tx_history_; }
    void clear_transmitted_packets() { tx_history_.clear(); }

    // RX Injection
    bool inject_rx_frame(const uint8_t* frame_data, size_t frame_len,
                         bool crc_err = false, bool icv_err = false,
                         uint8_t shift = 0);

    // Starvation / Statistics
    size_t get_rx_drop_count() const { return rx_drop_count_; }
    size_t get_rx_hw_index() const { return rx_hw_index_; }
    size_t get_tx_hw_index(QueueId q_id) const;

    // Callbacks
    using TxCallback = std::function<void(const TxPacketRecord&)>;
    void set_tx_callback(TxCallback cb) { tx_cb_ = cb; }

private:
    struct TxRingState {
        uint64_t phys_base{0};
        size_t count{0};
        size_t hw_index{0};
        uint16_t doorbell_bit{0};
        uint32_t isr_ok_bit{0};
    };

    MockPCIMmio& mmio_;
    std::map<QueueId, TxRingState> tx_rings_;

    uint64_t rx_ring_base_{0};
    size_t rx_ring_count_{0};
    size_t rx_hw_index_{0};
    size_t rx_drop_count_{0};

    // Simulated Physical Memory Chunks
    struct MemChunk {
        uint64_t phys_addr;
        size_t size;
        std::vector<uint8_t> buffer;
    };
    std::vector<std::unique_ptr<MemChunk>> memory_pool_;
    uint64_t next_phys_addr_{0x10000000ULL}; // Start physical base at 256MB

    std::vector<TxPacketRecord> tx_history_;
    TxCallback tx_cb_;
};

} // namespace rtl_mock
