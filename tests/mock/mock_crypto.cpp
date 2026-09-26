#include "mock_crypto.hpp"
#include <cstring>
#include <algorithm>

namespace rtl_crypto {

// ============================================================================
// SHA-1 Implementation (FIPS 180-1)
// ============================================================================

namespace {

inline uint32_t rol32(uint32_t val, uint32_t bits) {
    return (val << bits) | (val >> (32 - bits));
}

} // namespace

void sha1(const uint8_t* data, size_t len, uint8_t digest[20]) {
    uint32_t h[5] = {
        0x67452301,
        0xEFCDAB89,
        0x98BADCFE,
        0x10325476,
        0xC3D2E1F0
    };

    uint64_t total_bits = static_cast<uint64_t>(len) * 8;
    std::vector<uint8_t> buffer(data, data + len);

    // Padding: 0x80 followed by zeros, then 64-bit big-endian length
    buffer.push_back(0x80);
    while ((buffer.size() % 64) != 56) {
        buffer.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        buffer.push_back(static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF));
    }

    // Process 64-byte chunks
    for (size_t chunk = 0; chunk < buffer.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(buffer[chunk + i * 4]) << 24) |
                   (static_cast<uint32_t>(buffer[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(buffer[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(buffer[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];

        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol32(b, 30);
            b = a;
            a = temp;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
    }
}

// ============================================================================
// HMAC-SHA1 Implementation (RFC 2104)
// ============================================================================

void hmac_sha1(const uint8_t* key, size_t key_len,
               const uint8_t* data, size_t data_len,
               uint8_t mac[20]) {
    uint8_t k[64] = {0};
    if (key_len > 64) {
        sha1(key, key_len, k);
    } else {
        std::memcpy(k, key, key_len);
    }

    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    for (int i = 0; i < 64; ++i) {
        k_ipad[i] = k[i] ^ 0x36;
        k_opad[i] = k[i] ^ 0x5C;
    }

    // Inner hash: H(k_ipad || data)
    std::vector<uint8_t> inner_data(64 + data_len);
    std::memcpy(inner_data.data(), k_ipad, 64);
    if (data_len > 0) {
        std::memcpy(inner_data.data() + 64, data, data_len);
    }
    uint8_t inner_hash[20];
    sha1(inner_data.data(), inner_data.size(), inner_hash);

    // Outer hash: H(k_opad || inner_hash)
    uint8_t outer_data[64 + 20];
    std::memcpy(outer_data, k_opad, 64);
    std::memcpy(outer_data + 64, inner_hash, 20);
    sha1(outer_data, sizeof(outer_data), mac);
}

// ============================================================================
// PBKDF2-HMAC-SHA1 Implementation (RFC 2898 / RFC 6070)
// ============================================================================

void pbkdf2_sha1(const char* passphrase, size_t pass_len,
                 const uint8_t* salt, size_t salt_len,
                 uint32_t iterations,
                 uint8_t* out_key, size_t out_len) {
    uint32_t block_count = static_cast<uint32_t>((out_len + 19) / 20);
    size_t key_offset = 0;

    for (uint32_t i = 1; i <= block_count; ++i) {
        // U1 = HMAC-SHA1(Passphrase, Salt || INT(i))
        std::vector<uint8_t> salt_int(salt_len + 4);
        std::memcpy(salt_int.data(), salt, salt_len);
        salt_int[salt_len] = static_cast<uint8_t>((i >> 24) & 0xFF);
        salt_int[salt_len + 1] = static_cast<uint8_t>((i >> 16) & 0xFF);
        salt_int[salt_len + 2] = static_cast<uint8_t>((i >> 8) & 0xFF);
        salt_int[salt_len + 3] = static_cast<uint8_t>(i & 0xFF);

        uint8_t u[20];
        uint8_t t[20];
        hmac_sha1(reinterpret_cast<const uint8_t*>(passphrase), pass_len,
                  salt_int.data(), salt_int.size(), u);
        std::memcpy(t, u, 20);

        for (uint32_t iter = 1; iter < iterations; ++iter) {
            uint8_t u_next[20];
            hmac_sha1(reinterpret_cast<const uint8_t*>(passphrase), pass_len,
                      u, 20, u_next);
            std::memcpy(u, u_next, 20);
            for (int b = 0; b < 20; ++b) {
                t[b] ^= u[b];
            }
        }

        size_t to_copy = std::min<size_t>(20, out_len - key_offset);
        std::memcpy(out_key + key_offset, t, to_copy);
        key_offset += to_copy;
    }
}

// ============================================================================
// IEEE 802.11i PRF-512 Implementation
// ============================================================================

void prf_512(const uint8_t pmk[32],
             const char* label,
             const uint8_t addr1[6], const uint8_t addr2[6],
             const uint8_t nonce1[32], const uint8_t nonce2[32],
             uint8_t out_ptk[64]) {
    // Construct data context: Min(A1,A2) || Max(A1,A2) || Min(N1,N2) || Max(N1,N2)
    uint8_t context[76]; // 6 + 6 + 32 + 32 = 76 bytes

    if (std::memcmp(addr1, addr2, 6) < 0) {
        std::memcpy(context, addr1, 6);
        std::memcpy(context + 6, addr2, 6);
    } else {
        std::memcpy(context, addr2, 6);
        std::memcpy(context + 6, addr1, 6);
    }

    if (std::memcmp(nonce1, nonce2, 32) < 0) {
        std::memcpy(context + 12, nonce1, 32);
        std::memcpy(context + 44, nonce2, 32);
    } else {
        std::memcpy(context + 12, nonce2, 32);
        std::memcpy(context + 44, nonce1, 32);
    }

    size_t label_len = std::strlen(label);
    size_t prefix_len = label_len + 1 + 76 + 1; // label + 0x00 + context + count
    std::vector<uint8_t> prf_input(prefix_len);

    std::memcpy(prf_input.data(), label, label_len);
    prf_input[label_len] = 0x00;
    std::memcpy(prf_input.data() + label_len + 1, context, 76);

    // Generate output 64 bytes using 4 iterations of HMAC-SHA1 (20 bytes each, 80 total, truncate to 64)
    size_t generated = 0;
    for (uint8_t counter = 0; generated < 64; ++counter) {
        prf_input[prefix_len - 1] = counter;
        uint8_t digest[20];
        hmac_sha1(pmk, 32, prf_input.data(), prf_input.size(), digest);
        size_t to_copy = std::min<size_t>(20, 64 - generated);
        std::memcpy(out_ptk + generated, digest, to_copy);
        generated += to_copy;
    }
}

// ============================================================================
// AES-128 Implementation (FIPS 197)
// ============================================================================

namespace {

static const uint8_t s_box[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t inv_s_box[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static const uint32_t rcon[10] = {
    0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000,
    0x20000000, 0x40000000, 0x80000000, 0x1B000000, 0x36000000
};

inline uint8_t xtime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ (((x >> 7) & 1) * 0x1B));
}

inline uint8_t multiply(uint8_t x, uint8_t y) {
    uint8_t result = 0;
    while (y) {
        if (y & 1) result ^= x;
        x = xtime(x);
        y >>= 1;
    }
    return result;
}

} // namespace

AES128::AES128() {
    std::memset(round_keys_, 0, sizeof(round_keys_));
    std::memset(inv_round_keys_, 0, sizeof(inv_round_keys_));
}

AES128::AES128(const uint8_t key[16]) {
    set_key(key);
}

void AES128::set_key(const uint8_t key[16]) {
    for (int i = 0; i < 4; ++i) {
        round_keys_[i] = (static_cast<uint32_t>(key[i * 4]) << 24) |
                         (static_cast<uint32_t>(key[i * 4 + 1]) << 16) |
                         (static_cast<uint32_t>(key[i * 4 + 2]) << 8) |
                         (static_cast<uint32_t>(key[i * 4 + 3]));
    }

    for (int i = 4; i < 44; ++i) {
        uint32_t temp = round_keys_[i - 1];
        if (i % 4 == 0) {
            // RotWord + SubWord + Rcon
            uint32_t rot = (temp << 8) | (temp >> 24);
            uint8_t b0 = s_box[(rot >> 24) & 0xFF];
            uint8_t b1 = s_box[(rot >> 16) & 0xFF];
            uint8_t b2 = s_box[(rot >> 8) & 0xFF];
            uint8_t b3 = s_box[rot & 0xFF];
            temp = ((static_cast<uint32_t>(b0) << 24) |
                    (static_cast<uint32_t>(b1) << 16) |
                    (static_cast<uint32_t>(b2) << 8) |
                    (static_cast<uint32_t>(b3))) ^ rcon[(i / 4) - 1];
        }
        round_keys_[i] = round_keys_[i - 4] ^ temp;
    }

    // Inverse round keys for decryption
    for (int i = 0; i < 44; ++i) {
        inv_round_keys_[i] = round_keys_[i];
    }
}

void AES128::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t state[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            state[r][c] = in[r + 4 * c];
        }
    }

    // AddRoundKey round 0
    for (int c = 0; c < 4; ++c) {
        uint32_t k = round_keys_[c];
        state[0][c] ^= (k >> 24) & 0xFF;
        state[1][c] ^= (k >> 16) & 0xFF;
        state[2][c] ^= (k >> 8) & 0xFF;
        state[3][c] ^= k & 0xFF;
    }

    // Rounds 1 to 9
    for (int round = 1; round < 10; ++round) {
        // SubBytes
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                state[r][c] = s_box[state[r][c]];
            }
        }

        // ShiftRows
        uint8_t temp;
        // row 1: shift left by 1
        temp = state[1][0];
        state[1][0] = state[1][1];
        state[1][1] = state[1][2];
        state[1][2] = state[1][3];
        state[1][3] = temp;
        // row 2: shift left by 2
        temp = state[2][0];
        state[2][0] = state[2][2];
        state[2][2] = temp;
        temp = state[2][1];
        state[2][1] = state[2][3];
        state[2][3] = temp;
        // row 3: shift left by 3 (shift right by 1)
        temp = state[3][3];
        state[3][3] = state[3][2];
        state[3][2] = state[3][1];
        state[3][1] = state[3][0];
        state[3][0] = temp;

        // MixColumns
        for (int c = 0; c < 4; ++c) {
            uint8_t a0 = state[0][c];
            uint8_t a1 = state[1][c];
            uint8_t a2 = state[2][c];
            uint8_t a3 = state[3][c];

            state[0][c] = multiply(0x02, a0) ^ multiply(0x03, a1) ^ a2 ^ a3;
            state[1][c] = a0 ^ multiply(0x02, a1) ^ multiply(0x03, a2) ^ a3;
            state[2][c] = a0 ^ a1 ^ multiply(0x02, a2) ^ multiply(0x03, a3);
            state[3][c] = multiply(0x03, a0) ^ a1 ^ a2 ^ multiply(0x02, a3);
        }

        // AddRoundKey
        for (int c = 0; c < 4; ++c) {
            uint32_t k = round_keys_[round * 4 + c];
            state[0][c] ^= (k >> 24) & 0xFF;
            state[1][c] ^= (k >> 16) & 0xFF;
            state[2][c] ^= (k >> 8) & 0xFF;
            state[3][c] ^= k & 0xFF;
        }
    }

    // Round 10 (no MixColumns)
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            state[r][c] = s_box[state[r][c]];
        }
    }

    // ShiftRows
    uint8_t temp;
    temp = state[1][0];
    state[1][0] = state[1][1];
    state[1][1] = state[1][2];
    state[1][2] = state[1][3];
    state[1][3] = temp;

    temp = state[2][0];
    state[2][0] = state[2][2];
    state[2][2] = temp;
    temp = state[2][1];
    state[2][1] = state[2][3];
    state[2][3] = temp;

    temp = state[3][3];
    state[3][3] = state[3][2];
    state[3][2] = state[3][1];
    state[3][1] = state[3][0];
    state[3][0] = temp;

    // AddRoundKey
    for (int c = 0; c < 4; ++c) {
        uint32_t k = round_keys_[40 + c];
        state[0][c] ^= (k >> 24) & 0xFF;
        state[1][c] ^= (k >> 16) & 0xFF;
        state[2][c] ^= (k >> 8) & 0xFF;
        state[3][c] ^= k & 0xFF;
    }

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r + 4 * c] = state[r][c];
        }
    }
}

void AES128::decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t state[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            state[r][c] = in[r + 4 * c];
        }
    }

    // AddRoundKey round 10
    for (int c = 0; c < 4; ++c) {
        uint32_t k = round_keys_[40 + c];
        state[0][c] ^= (k >> 24) & 0xFF;
        state[1][c] ^= (k >> 16) & 0xFF;
        state[2][c] ^= (k >> 8) & 0xFF;
        state[3][c] ^= k & 0xFF;
    }

    for (int round = 9; round >= 1; --round) {
        // InvShiftRows
        uint8_t temp;
        // row 1: shift right by 1
        temp = state[1][3];
        state[1][3] = state[1][2];
        state[1][2] = state[1][1];
        state[1][1] = state[1][0];
        state[1][0] = temp;
        // row 2: shift right by 2
        temp = state[2][0];
        state[2][0] = state[2][2];
        state[2][2] = temp;
        temp = state[2][1];
        state[2][1] = state[2][3];
        state[2][3] = temp;
        // row 3: shift right by 3 (shift left by 1)
        temp = state[3][0];
        state[3][0] = state[3][1];
        state[3][1] = state[3][2];
        state[3][2] = state[3][3];
        state[3][3] = temp;

        // InvSubBytes
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                state[r][c] = inv_s_box[state[r][c]];
            }
        }

        // AddRoundKey
        for (int c = 0; c < 4; ++c) {
            uint32_t k = round_keys_[round * 4 + c];
            state[0][c] ^= (k >> 24) & 0xFF;
            state[1][c] ^= (k >> 16) & 0xFF;
            state[2][c] ^= (k >> 8) & 0xFF;
            state[3][c] ^= k & 0xFF;
        }

        // InvMixColumns
        for (int c = 0; c < 4; ++c) {
            uint8_t a0 = state[0][c];
            uint8_t a1 = state[1][c];
            uint8_t a2 = state[2][c];
            uint8_t a3 = state[3][c];

            state[0][c] = multiply(0x0e, a0) ^ multiply(0x0b, a1) ^ multiply(0x0d, a2) ^ multiply(0x09, a3);
            state[1][c] = multiply(0x09, a0) ^ multiply(0x0e, a1) ^ multiply(0x0b, a2) ^ multiply(0x0d, a3);
            state[2][c] = multiply(0x0d, a0) ^ multiply(0x09, a1) ^ multiply(0x0e, a2) ^ multiply(0x0b, a3);
            state[3][c] = multiply(0x0b, a0) ^ multiply(0x0d, a1) ^ multiply(0x09, a2) ^ multiply(0x0e, a3);
        }
    }

    // InvShiftRows
    uint8_t temp;
    temp = state[1][3];
    state[1][3] = state[1][2];
    state[1][2] = state[1][1];
    state[1][1] = state[1][0];
    state[1][0] = temp;

    temp = state[2][0];
    state[2][0] = state[2][2];
    state[2][2] = temp;
    temp = state[2][1];
    state[2][1] = state[2][3];
    state[2][3] = temp;

    temp = state[3][0];
    state[3][0] = state[3][1];
    state[3][1] = state[3][2];
    state[3][2] = state[3][3];
    state[3][3] = temp;

    // InvSubBytes
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            state[r][c] = inv_s_box[state[r][c]];
        }
    }

    // AddRoundKey round 0
    for (int c = 0; c < 4; ++c) {
        uint32_t k = round_keys_[c];
        state[0][c] ^= (k >> 24) & 0xFF;
        state[1][c] ^= (k >> 16) & 0xFF;
        state[2][c] ^= (k >> 8) & 0xFF;
        state[3][c] ^= k & 0xFF;
    }

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r + 4 * c] = state[r][c];
        }
    }
}

// ============================================================================
// RFC 3394 AES Key Wrap & Unwrap
// ============================================================================

bool aes_key_wrap(const uint8_t kek[16], const uint8_t* plain, size_t plain_len, uint8_t* wrapped) {
    if (plain_len == 0 || (plain_len % 8) != 0) return false;
    size_t n = plain_len / 8;
    AES128 aes(kek);

    uint8_t a[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
    std::vector<uint8_t> r(plain_len);
    std::memcpy(r.data(), plain, plain_len);

    for (uint32_t j = 0; j <= 5; ++j) {
        for (size_t i = 1; i <= n; ++i) {
            uint8_t b[16];
            std::memcpy(b, a, 8);
            std::memcpy(b + 8, r.data() + (i - 1) * 8, 8);
            aes.encrypt_block(b, b);
            std::memcpy(a, b, 8);
            uint32_t t = static_cast<uint32_t>(n * j + i);
            a[7] ^= static_cast<uint8_t>(t & 0xFF);
            a[6] ^= static_cast<uint8_t>((t >> 8) & 0xFF);
            a[5] ^= static_cast<uint8_t>((t >> 16) & 0xFF);
            a[4] ^= static_cast<uint8_t>((t >> 24) & 0xFF);
            std::memcpy(r.data() + (i - 1) * 8, b + 8, 8);
        }
    }

    std::memcpy(wrapped, a, 8);
    std::memcpy(wrapped + 8, r.data(), plain_len);
    return true;
}

bool aes_key_unwrap(const uint8_t kek[16], const uint8_t* wrapped, size_t wrapped_len, uint8_t* plain) {
    if (wrapped_len < 16 || (wrapped_len % 8) != 0) return false;
    size_t n = (wrapped_len / 8) - 1;
    AES128 aes(kek);

    uint8_t a[8];
    std::memcpy(a, wrapped, 8);
    std::vector<uint8_t> r(n * 8);
    std::memcpy(r.data(), wrapped + 8, n * 8);

    for (int j = 5; j >= 0; --j) {
        for (size_t i = n; i >= 1; --i) {
            uint32_t t = static_cast<uint32_t>(n * j + i);
            a[7] ^= static_cast<uint8_t>(t & 0xFF);
            a[6] ^= static_cast<uint8_t>((t >> 8) & 0xFF);
            a[5] ^= static_cast<uint8_t>((t >> 16) & 0xFF);
            a[4] ^= static_cast<uint8_t>((t >> 24) & 0xFF);

            uint8_t b[16];
            std::memcpy(b, a, 8);
            std::memcpy(b + 8, r.data() + (i - 1) * 8, 8);
            aes.decrypt_block(b, b);
            std::memcpy(a, b, 8);
            std::memcpy(r.data() + (i - 1) * 8, b + 8, 8);
        }
    }

    // Check IV: 0xA6A6A6A6A6A6A6A6
    for (int i = 0; i < 8; ++i) {
        if (a[i] != 0xA6) return false;
    }

    std::memcpy(plain, r.data(), n * 8);
    return true;
}

// ============================================================================
// IEEE 802.11 CCMP (AES-128 CCM Mode, RFC 3610 / IEEE 802.11-2016 §12.5.3)
// ============================================================================

bool ccmp_encrypt(const uint8_t key[16],
                  const uint8_t nonce[13],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* plaintext, size_t plain_len,
                  uint8_t* ciphertext,
                  uint8_t mic[8]) {
    AES128 aes(key);

    // 1. CBC-MAC computation for MIC
    // B0 formatting (RFC 3610 / 802.11i):
    // Flags = Adata (bit 6 = 1 if aad_len > 0) | ((M - 2) / 2) << 3 | (L - 1)
    // M = 8 (MIC length), (8 - 2)/2 = 3. 3 << 3 = 24 = 0x18.
    // L = 2 (Length field = 16 - 1 - 13 = 2 bytes). L - 1 = 1.
    // Flags: if aad: 0x40 | 0x18 | 0x01 = 0x59. If no aad: 0x19.
    uint8_t b0[16] = {0};
    b0[0] = (aad_len > 0 ? 0x40 : 0x00) | 0x18 | 0x01;
    std::memcpy(b0 + 1, nonce, 13);
    b0[14] = static_cast<uint8_t>((plain_len >> 8) & 0xFF);
    b0[15] = static_cast<uint8_t>(plain_len & 0xFF);

    uint8_t x[16];
    aes.encrypt_block(b0, x);

    // Process AAD if present
    if (aad_len > 0) {
        // First block has 2-byte aad_len encoding (big endian)
        uint8_t block[16] = {0};
        block[0] = static_cast<uint8_t>((aad_len >> 8) & 0xFF);
        block[1] = static_cast<uint8_t>(aad_len & 0xFF);
        size_t first_copy = std::min<size_t>(aad_len, 14);
        std::memcpy(block + 2, aad, first_copy);

        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        aes.encrypt_block(x, x);

        size_t processed = first_copy;
        while (processed < aad_len) {
            std::memset(block, 0, 16);
            size_t copy_len = std::min<size_t>(aad_len - processed, 16);
            std::memcpy(block, aad + processed, copy_len);
            for (int i = 0; i < 16; ++i) x[i] ^= block[i];
            aes.encrypt_block(x, x);
            processed += copy_len;
        }
    }

    // Process Plaintext blocks into CBC-MAC
    size_t processed = 0;
    while (processed < plain_len) {
        uint8_t block[16] = {0};
        size_t copy_len = std::min<size_t>(plain_len - processed, 16);
        std::memcpy(block, plaintext + processed, copy_len);
        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        aes.encrypt_block(x, x);
        processed += copy_len;
    }

    // 2. CTR Mode Encryption
    // A0 formatting: Flags = L - 1 = 0x01, Nonce, Counter = 0
    uint8_t a0[16] = {0};
    a0[0] = 0x01;
    std::memcpy(a0 + 1, nonce, 13);
    a0[14] = 0x00;
    a0[15] = 0x00;

    uint8_t s0[16];
    aes.encrypt_block(a0, s0);
    // Encrypt CBC-MAC output to produce final 8-byte MIC
    for (int i = 0; i < 8; ++i) {
        mic[i] = x[i] ^ s0[i];
    }

    // Encrypt plaintext payload with counter blocks (counter = 1, 2, ...)
    uint16_t counter = 1;
    processed = 0;
    while (processed < plain_len) {
        uint8_t ai[16] = {0};
        ai[0] = 0x01;
        std::memcpy(ai + 1, nonce, 13);
        ai[14] = static_cast<uint8_t>((counter >> 8) & 0xFF);
        ai[15] = static_cast<uint8_t>(counter & 0xFF);

        uint8_t si[16];
        aes.encrypt_block(ai, si);

        size_t copy_len = std::min<size_t>(plain_len - processed, 16);
        for (size_t i = 0; i < copy_len; ++i) {
            ciphertext[processed + i] = plaintext[processed + i] ^ si[i];
        }

        processed += copy_len;
        counter++;
    }

    return true;
}

bool ccmp_decrypt(const uint8_t key[16],
                  const uint8_t nonce[13],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* ciphertext, size_t cipher_len,
                  const uint8_t mic[8],
                  uint8_t* plaintext) {
    AES128 aes(key);

    // 1. Decrypt ciphertext using CTR mode
    uint16_t counter = 1;
    size_t processed = 0;
    while (processed < cipher_len) {
        uint8_t ai[16] = {0};
        ai[0] = 0x01;
        std::memcpy(ai + 1, nonce, 13);
        ai[14] = static_cast<uint8_t>((counter >> 8) & 0xFF);
        ai[15] = static_cast<uint8_t>(counter & 0xFF);

        uint8_t si[16];
        aes.encrypt_block(ai, si);

        size_t copy_len = std::min<size_t>(cipher_len - processed, 16);
        for (size_t i = 0; i < copy_len; ++i) {
            plaintext[processed + i] = ciphertext[processed + i] ^ si[i];
        }

        processed += copy_len;
        counter++;
    }

    // 2. Compute CBC-MAC over decrypted plaintext to verify MIC
    uint8_t b0[16] = {0};
    b0[0] = (aad_len > 0 ? 0x40 : 0x00) | 0x18 | 0x01;
    std::memcpy(b0 + 1, nonce, 13);
    b0[14] = static_cast<uint8_t>((cipher_len >> 8) & 0xFF);
    b0[15] = static_cast<uint8_t>(cipher_len & 0xFF);

    uint8_t x[16];
    aes.encrypt_block(b0, x);

    if (aad_len > 0) {
        uint8_t block[16] = {0};
        block[0] = static_cast<uint8_t>((aad_len >> 8) & 0xFF);
        block[1] = static_cast<uint8_t>(aad_len & 0xFF);
        size_t first_copy = std::min<size_t>(aad_len, 14);
        std::memcpy(block + 2, aad, first_copy);

        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        aes.encrypt_block(x, x);

        size_t aad_proc = first_copy;
        while (aad_proc < aad_len) {
            std::memset(block, 0, 16);
            size_t copy_len = std::min<size_t>(aad_len - aad_proc, 16);
            std::memcpy(block, aad + aad_proc, copy_len);
            for (int i = 0; i < 16; ++i) x[i] ^= block[i];
            aes.encrypt_block(x, x);
            aad_proc += copy_len;
        }
    }

    processed = 0;
    while (processed < cipher_len) {
        uint8_t block[16] = {0};
        size_t copy_len = std::min<size_t>(cipher_len - processed, 16);
        std::memcpy(block, plaintext + processed, copy_len);
        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        aes.encrypt_block(x, x);
        processed += copy_len;
    }

    uint8_t a0[16] = {0};
    a0[0] = 0x01;
    std::memcpy(a0 + 1, nonce, 13);

    uint8_t s0[16];
    aes.encrypt_block(a0, s0);

    uint8_t computed_mic[8];
    for (int i = 0; i < 8; ++i) {
        computed_mic[i] = x[i] ^ s0[i];
    }

    // Constant-time compare MIC
    uint8_t diff = 0;
    for (int i = 0; i < 8; ++i) {
        diff |= (computed_mic[i] ^ mic[i]);
    }

    return (diff == 0);
}

// ============================================================================
// IEEE 802.11 FCS CRC32 Implementation
// ============================================================================

namespace {

static uint32_t crc32_table[256];
static bool crc32_table_inited = false;

void init_crc32_table() {
    uint32_t poly = 0xEDB88320;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_inited = true;
}

} // namespace

uint32_t crc32_80211(const uint8_t* data, size_t len) {
    if (!crc32_table_inited) {
        init_crc32_table();
    }

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace rtl_crypto
