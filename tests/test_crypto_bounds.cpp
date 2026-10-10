// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group

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
    // IEEE Std 802.11-2012 Annex M.6.4 CCMP test vector.
    {
        const uint8_t tk[16] = {0xc9,0x7c,0x1f,0x67,0xce,0x37,0x11,0x85,0x51,0x4a,0x8a,0x19,0xf2,0xbd,0xd5,0x2f};
        const uint64_t pn = 0xB5039776E70CULL;
        const uint8_t hdr[24] = {0x08,0x48,0xc3,0x2c,0x0f,0xd2,0xe1,0x28,0xa5,0x7c,0x50,0x30,
                                 0xf1,0x84,0x44,0x08,0xab,0xae,0xa5,0xb8,0xfc,0xba,0x80,0x33};
        const uint8_t plain[20] = {0xf8,0xba,0x1a,0x55,0xd0,0x2f,0x85,0xae,0x96,0x7b,
                                   0xb6,0x2f,0xb6,0xcd,0xa8,0xeb,0x7e,0x78,0xa0,0x50};
        const uint8_t cipher[20] = {0xf3,0xd0,0xa2,0xfe,0x9a,0x3d,0xbf,0x23,0x42,0xa6,
                                    0x43,0xe4,0x32,0x46,0xe8,0x0c,0x3c,0x04,0xd0,0x19};
        const uint8_t micExp[8] = {0x78,0x45,0xce,0x0b,0x16,0xf9,0x76,0x23};
        uint8_t aad[30], nonce[13], out[20], mic[8], back[20];
        size_t aadLen = rtl_crypto::ccmp_build_aad(hdr, sizeof(hdr), aad);
        assert(aadLen == 22);
        rtl_crypto::ccmp_build_nonce(hdr, sizeof(hdr), pn, nonce);
        assert(rtl_crypto::ccmp_encrypt(tk, nonce, aad, aadLen, plain, sizeof(plain), out, mic));
        assert(std::memcmp(out, cipher, sizeof(cipher)) == 0);
        assert(std::memcmp(mic, micExp, sizeof(mic)) == 0);
        assert(rtl_crypto::ccmp_decrypt(tk, nonce, aad, aadLen, cipher, sizeof(cipher), micExp, back));
        assert(std::memcmp(back, plain, sizeof(plain)) == 0);
        // Including the Duration field (the old bug) must not verify.
        uint8_t badAad[26];
        std::memcpy(badAad, hdr, 24);
        assert(!rtl_crypto::ccmp_decrypt(tk, nonce, badAad, 24, cipher, sizeof(cipher), micExp, back));

        // QoS data header: AAD is 24 bytes and the nonce priority is the TID.
        const uint8_t qos[26] = {0x88,0x42,0x2c,0x00,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,0x31,0x02,0x95,0x00};
        assert(rtl_crypto::ccmp_build_aad(qos, sizeof(qos), aad) == 24);
        assert(aad[0] == 0x88 && aad[1] == 0x42 && aad[20] == 0x01 && aad[21] == 0 && aad[22] == 0x05 && aad[23] == 0);
        rtl_crypto::ccmp_build_nonce(qos, sizeof(qos), 1, nonce);
        assert(nonce[0] == 0x05 && nonce[1] == 7 && nonce[12] == 1);
    }
    (void)expected;
    std::puts("Production crypto bounds: PASS (RFC 3394, destination capacity, IEEE 802.11 CCMP vector)");
}
