#include "RTL8723BE_crypto.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    // RFC 3394 section 4.1: independently specified key-wrap test vector.
    const uint8_t key[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    const uint8_t wrapped[24] = {
        0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,
        0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,
        0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5
    };
    const uint8_t expected[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    uint8_t rewrapped[24] = {};
    assert(rtl_crypto::aes_key_wrap(key, expected, sizeof(expected), rewrapped));
    assert(std::memcmp(rewrapped, wrapped, sizeof(wrapped)) == 0);
    uint8_t small[8] = {};
    assert(!rtl_crypto::aes_key_unwrap(key, wrapped, sizeof(wrapped), small, sizeof(small)));
    for (uint8_t value : small) assert(value == 0);
    uint8_t output[16] = {};
    assert(rtl_crypto::aes_key_unwrap(key, wrapped, sizeof(wrapped), output, sizeof(output)));
    assert(std::memcmp(output, expected, sizeof(expected)) == 0);
    assert(!rtl_crypto::aes_key_unwrap(key, wrapped, sizeof(wrapped), output, 15));
    assert(!rtl_crypto::aes_key_unwrap(key, wrapped, 23, output, sizeof(output)));
    assert(!rtl_crypto::aes_key_unwrap(key, wrapped, 0, output, sizeof(output)));
    assert(!rtl_crypto::aes_key_unwrap(key, nullptr, 24, output, sizeof(output)));
    // The EAPOL handler has a 64-byte destination, while the old API could
    // emit up to 128 bytes. Reject that size before reading/decrypting input.
    uint8_t large[136] = {};
    uint8_t gtk[64] = {};
    uint8_t largePlain[128] = {};
    assert(rtl_crypto::aes_key_wrap(key, largePlain, sizeof(largePlain), large));
    assert(!rtl_crypto::aes_key_unwrap(key, large, sizeof(large), gtk, sizeof(gtk)));
    for (uint8_t value : gtk) assert(value == 0);
    uint8_t fullOutput[128];
    assert(rtl_crypto::aes_key_unwrap(key, large, sizeof(large), fullOutput, sizeof(fullOutput)));
    assert(std::memcmp(fullOutput, largePlain, sizeof(fullOutput)) == 0);
    (void)expected;
    std::puts("Production crypto bounds: PASS (RFC 3394 and destination capacity)");
}
