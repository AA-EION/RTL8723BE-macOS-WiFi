#include "RTL8723BE_hw.hpp"
#include "RTL8723BE_tables.hpp"
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

    // Verify MAC_1T_ARRAY (8-bit registers), PHY_REG_1T, AGCTAB_1T, and 6-tuple PHY_REG_ARRAY_PG
    // never produce an out-of-bounds BAR2 (>= 0x4000) or unaligned 32-bit BB address.
    assert(RTL8723BEMAC_1T_ARRAYLEN % 2 == 0);
    for (size_t i = 0; i < RTL8723BEMAC_1T_ARRAYLEN; i += 2) {
        assert(RTL8723BEMAC_1T_ARRAY[i] < 0x4000U);
        assert(RTL8723BEMAC_1T_ARRAY[i + 1] <= 0xFFU);
    }
    assert(RTL8723BEPHY_REG_1TARRAYLEN % 2 == 0);
    for (size_t i = 0; i < RTL8723BEPHY_REG_1TARRAYLEN; i += 2) {
        uint32_t addr = RTL8723BEPHY_REG_1TARRAY[i];
        assert(addr < 0x4000U);
        assert((addr & 0x3U) == 0);
    }
    assert(RTL8723BEPHY_REG_ARRAY_PGLEN % 6 == 0);
    for (size_t i = 0; i < RTL8723BEPHY_REG_ARRAY_PGLEN; i += 6) {
        uint32_t addr = RTL8723BEPHY_REG_ARRAY_PG[i + 3];
        uint32_t mask = RTL8723BEPHY_REG_ARRAY_PG[i + 4];
        assert(addr >= 0x800U && addr < 0x4000U);
        assert((addr & 0x3U) == 0);
        assert(mask != 0);
    }

    RxDesc32 rx = {};
    rx.set_buffer_addr(0x23456000);
    rx.set_length(2048);
    assert(REG_MAC_PHY_CTRL == 0x2C && REG_AFE_XTAL_CTRL == 0x24);
    assert(REG_RXDMA_CONTROL == 0x286 && REG_PBP == 0x104 && REG_RQPN == 0x200);
    rx.set_eor(true);
    rx.set_own(true);
    assert(sizeof(rx) == 32);
    assert(wordAt(&rx, 24) == 0x23456000);
    assert(wordAt(&rx, 0) == 0xC0000800);
    // eFuse PG stream decoding (Linux efuse.c read_efuse): sections are 8 bytes wide,
    // word-enable bits are active-low, and sections >= 16 use a 2-byte extended header.
    {
        uint8_t phys[64];
        std::memset(phys, 0xFF, sizeof(phys));
        size_t p = 0;
        phys[p++] = 0x0E;                          // section 0, word 0 only (wren = 1110b)
        phys[p++] = 0x29; phys[p++] = 0x81;        // EEPROM ID 0x8129
        // Section 0x1A (logical 0xD0..0xD7) via extended header: hdr = (sec&7)<<5 | 0x0F,
        // hdr2 = (sec>>3)<<4 | wren.
        phys[p++] = (uint8_t)(((0x1A & 0x7) << 5) | 0x0F);
        phys[p++] = (uint8_t)(((0x1A >> 3) << 4) | 0x08); // words 0..2 present
        const uint8_t mac[6] = {0x1C, 0x39, 0x47, 0xAB, 0xCD, 0xEF};
        for (int i = 0; i < 6; ++i) phys[p++] = mac[i];
        phys[p++] = 0x78;                          // section 7 (0x38..0x3F), word 0 only... wren=1000b -> words 0-2
        phys[p++] = 0x11; phys[p++] = 0x22; phys[p++] = 0x33; phys[p++] = 0x44; phys[p++] = 0x55; phys[p++] = 0x66;
        uint8_t map[512];
        size_t used = rtlEfuseDecode(phys, sizeof(phys), map, sizeof(map));
        assert(used == p);
        assert(map[0] == 0x29 && map[1] == 0x81);
        assert(map[2] == 0xFF);
        assert(std::memcmp(&map[0xD0], mac, 6) == 0);
        assert(map[0xD6] == 0xFF);
        assert(map[0x38] == 0x11 && map[0x3D] == 0x66 && map[0x3E] == 0xFF);
    }

    std::puts("Production hardware contract: PASS (descriptor ABI, links, queues, registers, tables, DMA limits)");
}
