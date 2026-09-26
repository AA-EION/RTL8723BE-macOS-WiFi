#include "RTL8723BE_hw.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

// Decode raw bytes at independently specified hardware offsets; do not use
// the same getter as the setter under test or the simulator's duplicate ABI.
static uint32_t wordAt(const void *p, size_t offset) {
    uint32_t word;
    std::memcpy(&word, static_cast<const uint8_t *>(p) + offset, sizeof(word));
    return word;
}

int main() {
    TxDescPci ring[3] = {};
    const uint32_t base = 0x10000000;
    for (size_t i = 0; i < 3; ++i) {
        ring[i].set_buffer_addr(0x12345000 + i * 4096);
        ring[i].set_next_addr(base + ((i + 1) % 3) * sizeof(TxDescPci));
        ring[i].set_offset(RTL_TX_HEADER_SIZE);
        ring[i].set_buffer_size(1500);
        ring[i].set_pktsize(1500);
        ring[i].set_own(true);
        assert(wordAt(&ring[i], 32) == 0); // Control word, never an address.
        assert(wordAt(&ring[i], 40) == 0x12345000 + i * 4096);
        assert(wordAt(&ring[i], 44) == 0);
        assert((wordAt(&ring[i], 28) & 0xFFFF) == 1500);
        assert(((wordAt(&ring[i], 0) >> 16) & 0xFF) == 40);
    }
    // Emulate hardware following the link at byte 48, including wraparound.
    uint32_t cursor = base;
    for (size_t n = 0; n < 10; ++n) {
        assert(cursor == base + (n % 3) * 64);
        cursor = wordAt(reinterpret_cast<const uint8_t *>(ring) + cursor - base, 48);
    }
    assert(rtlTxQueueSelect(Q_MGNT) == 0x12);
    assert(rtlTxQueueSelect(Q_BE) == 0);
    assert(rtlTxQueueSelect(Q_VO) == 6);
    assert(REG_MULTI_FUNC_CTRL == 0x68);
    assert(REG_HWSEQ_CTRL == 0x423);
    assert(rtlDmaRangeValid(0x10000000, 4096, 256));
    assert(rtlDmaRangeValid(0xFFFFFF00, 256, 256));
    assert(!rtlDmaRangeValid(0xFFFFFF00, 257, 256));
    assert(!rtlDmaRangeValid(0x100000000ULL, 256, 256));
    assert(!rtlDmaRangeValid(0, 256, 256));
    assert(!rtlDmaRangeValid(0x10000001, 256, 256));
    assert(!rtlDmaRangeValid(0x10000000, 0, 256));

    RxDesc32 rx = {};
    rx.set_buffer_addr(0x23456000);
    rx.set_length(2048);
    rx.set_eor(true);
    rx.set_own(true);
    assert(sizeof(rx) == 32);
    assert(wordAt(&rx, 24) == 0x23456000);
    assert(wordAt(&rx, 0) == 0xC0000800);
    std::puts("Production hardware contract: PASS (descriptor ABI, links, queues, registers, DMA limits)");
}
