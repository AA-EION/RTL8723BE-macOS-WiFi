// SPDX-License-Identifier: GPL-2.0-or-later
// Linux btrtl.c copyright (C) 2015 Endless Mobile, Inc.
// Bounds-checked RTL8723B epatch v1 parser and fragment indexing.
// Source: Linux v6.12 drivers/bluetooth/btrtl.c, rtlbt_parse_firmware().
#ifndef RTL8723_BT_PATCH_HPP
#define RTL8723_BT_PATCH_HPP
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace rtl_bt {
inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | (uint32_t(le16(p + 2)) << 16); }
struct Patch { size_t offset, length; uint32_t version; };
inline bool parse(const uint8_t *fw, size_t size, uint8_t rom, Patch &patch) {
    static const uint8_t signature[] = {0x51,0x04,0xfd,0x77};
    patch = {};
    if (!fw || size < 22 || memcmp(fw, "Realtech", 8) || memcmp(fw + size - 4, signature, 4)) return false;
    size_t pos = size - 4;
    bool project = false;
    while (pos >= 17) {
        uint8_t opcode = fw[--pos], length = fw[--pos];
        if (opcode == 0xff) break;
        if (!length || length > pos - 14) return false;
        if (opcode == 0 && length == 1) { project = fw[pos - 1] == 1; break; }
        pos -= length;
    }
    if (!project) return false; // project 9 is 8723D: never flash it into a B
    size_t count = le16(fw + 12), metadata = 14 + count * 8;
    if (!count || metadata > size - 7) return false;
    for (size_t i = 0; i < count; ++i) {
        if (le16(fw + 14 + i * 2) != unsigned(rom) + 1) continue;
        size_t length = le16(fw + 14 + count * 2 + i * 2);
        size_t offset = le32(fw + 14 + count * 4 + i * 4);
        if (length < 4 || offset < metadata || offset > size - 7 || length > size - 7 - offset) return false;
        patch = {offset, length, le32(fw + 8)};
        return true;
    }
    return false;
}
inline size_t fragmentCount(size_t length) { return length / 252 + 1; }
inline uint8_t fragmentIndex(size_t fragment, size_t count) {
    // Linux: first index 0, then 1..127, wrapping to 1; high bit marks the end.
    uint8_t index = fragment == 0 ? 0 : uint8_t((fragment - 1) % 127 + 1);
    return uint8_t(index | (fragment + 1 == count ? 0x80 : 0));
}
}
extern const uint8_t rtl8723b_bt_firmware[];
extern const size_t rtl8723b_bt_firmware_size;
#endif
