#include "RTL8723BE_crypto.hpp"
#include <string.h>

namespace rtl_crypto {

// ============================================================================
// SHA-1 Implementation (FIPS 180-1)
// ============================================================================

namespace {

inline uint32_t rol32(uint32_t val, uint32_t bits) {
    return (val << bits) | (val >> (32 - bits));
}

void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t w[80];

    for (int i = 0; i < 16; ++i) {
        w[i] = (((uint32_t)buffer[i * 4]) << 24) |
               (((uint32_t)buffer[i * 4 + 1]) << 16) |
               (((uint32_t)buffer[i * 4 + 2]) << 8) |
               (((uint32_t)buffer[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

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

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

} // namespace

void sha1_init(SHA1_CTX* ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

void sha1_update(SHA1_CTX* ctx, const uint8_t* data, size_t len) {
    size_t i = 0;
    size_t j = (size_t)((ctx->count >> 3) & 63);
    ctx->count += (uint64_t)len << 3;
    if ((j + len) > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        sha1_transform(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, &data[i]);
        }
        j = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

void sha1_final(SHA1_CTX* ctx, uint8_t digest[20]) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; ++i) {
        finalcount[i] = (uint8_t)((ctx->count >> ((7 - i) * 8)) & 0xFF);
    }
    uint8_t c = 0x80;
    sha1_update(ctx, &c, 1);
    while ((ctx->count & 504) != 448) {
        c = 0x00;
        sha1_update(ctx, &c, 1);
    }
    sha1_update(ctx, finalcount, 8);
    for (int i = 0; i < 5; ++i) {
        digest[i * 4]     = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFF);
    }
}

void sha1(const uint8_t* data, size_t len, uint8_t digest[20]) {
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}

// ============================================================================
// HMAC-SHA1 Implementation (RFC 2104)
// ============================================================================

void hmac_sha1(const uint8_t* key, size_t key_len,
               const uint8_t* data, size_t data_len,
               uint8_t mac[20]) {
    uint8_t k[64];
    memset(k, 0, 64);
    if (key_len > 64) {
        sha1(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    for (int i = 0; i < 64; ++i) {
        k_ipad[i] = k[i] ^ 0x36;
        k_opad[i] = k[i] ^ 0x5C;
    }

    SHA1_CTX ctx;
    uint8_t inner_hash[20];
    sha1_init(&ctx);
    sha1_update(&ctx, k_ipad, 64);
    if (data && data_len > 0) {
        sha1_update(&ctx, data, data_len);
    }
    sha1_final(&ctx, inner_hash);

    sha1_init(&ctx);
    sha1_update(&ctx, k_opad, 64);
    sha1_update(&ctx, inner_hash, 20);
    sha1_final(&ctx, mac);
}

// ============================================================================
// PBKDF2-HMAC-SHA1 Implementation (RFC 2898 / RFC 6070)
// ============================================================================

void pbkdf2_sha1(const char* passphrase, size_t pass_len,
                 const uint8_t* salt, size_t salt_len,
                 uint32_t iterations,
                 uint8_t* out_key, size_t out_len) {
    uint32_t block_count = (uint32_t)((out_len + 19) / 20);
    size_t key_offset = 0;

    // Buffer for salt || INT(i)
    uint8_t salt_int[128];
    if (salt_len + 4 > sizeof(salt_int)) return;
    memcpy(salt_int, salt, salt_len);

    for (uint32_t i = 1; i <= block_count; ++i) {
        salt_int[salt_len]     = (uint8_t)((i >> 24) & 0xFF);
        salt_int[salt_len + 1] = (uint8_t)((i >> 16) & 0xFF);
        salt_int[salt_len + 2] = (uint8_t)((i >> 8) & 0xFF);
        salt_int[salt_len + 3] = (uint8_t)(i & 0xFF);

        uint8_t u[20];
        uint8_t t[20];
        hmac_sha1((const uint8_t*)passphrase, pass_len, salt_int, salt_len + 4, u);
        memcpy(t, u, 20);

        for (uint32_t iter = 1; iter < iterations; ++iter) {
            uint8_t u_next[20];
            hmac_sha1((const uint8_t*)passphrase, pass_len, u, 20, u_next);
            memcpy(u, u_next, 20);
            for (int b = 0; b < 20; ++b) {
                t[b] ^= u[b];
            }
        }

        size_t to_copy = (out_len - key_offset) < 20 ? (out_len - key_offset) : 20;
        memcpy(out_key + key_offset, t, to_copy);
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
    uint8_t context[76];

    if (memcmp(addr1, addr2, 6) < 0) {
        memcpy(context, addr1, 6);
        memcpy(context + 6, addr2, 6);
    } else {
        memcpy(context, addr2, 6);
        memcpy(context + 6, addr1, 6);
    }

    if (memcmp(nonce1, nonce2, 32) < 0) {
        memcpy(context + 12, nonce1, 32);
        memcpy(context + 44, nonce2, 32);
    } else {
        memcpy(context + 12, nonce2, 32);
        memcpy(context + 44, nonce1, 32);
    }

    size_t label_len = strlen(label);
    uint8_t prf_input[128];
    if (label_len + 1 + 76 + 1 > sizeof(prf_input)) return;

    memcpy(prf_input, label, label_len);
    prf_input[label_len] = 0x00;
    memcpy(prf_input + label_len + 1, context, 76);
    size_t prefix_len = label_len + 1 + 76 + 1;

    size_t generated = 0;
    for (uint8_t counter = 0; generated < 64; ++counter) {
        prf_input[prefix_len - 1] = counter;
        uint8_t digest[20];
        hmac_sha1(pmk, 32, prf_input, prefix_len, digest);
        size_t to_copy = (64 - generated) < 20 ? (64 - generated) : 20;
        memcpy(out_ptk + generated, digest, to_copy);
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
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1B));
}

inline uint8_t multiply(uint8_t x, uint8_t y) {
    return (uint8_t)(((y & 1) * x) ^
                     (((y >> 1) & 1) * xtime(x)) ^
                     (((y >> 2) & 1) * xtime(xtime(x))) ^
                     (((y >> 3) & 1) * xtime(xtime(xtime(x)))) ^
                     (((y >> 4) & 1) * xtime(xtime(xtime(xtime(x))))));
}

} // namespace

AES128::AES128() {
    memset(round_keys_, 0, sizeof(round_keys_));
}

AES128::AES128(const uint8_t key[16]) {
    set_key(key);
}

void AES128::set_key(const uint8_t key[16]) {
    for (int i = 0; i < 4; ++i) {
        round_keys_[i] = (((uint32_t)key[i * 4]) << 24) |
                         (((uint32_t)key[i * 4 + 1]) << 16) |
                         (((uint32_t)key[i * 4 + 2]) << 8) |
                         (((uint32_t)key[i * 4 + 3]));
    }
    for (int i = 4; i < 44; ++i) {
        uint32_t temp = round_keys_[i - 1];
        if ((i % 4) == 0) {
            temp = (temp << 8) | (temp >> 24);
            temp = (((uint32_t)s_box[(temp >> 24) & 0xFF]) << 24) |
                   (((uint32_t)s_box[(temp >> 16) & 0xFF]) << 16) |
                   (((uint32_t)s_box[(temp >> 8) & 0xFF]) << 8) |
                   (((uint32_t)s_box[temp & 0xFF]));
            temp ^= rcon[(i / 4) - 1];
        }
        round_keys_[i] = round_keys_[i - 4] ^ temp;
    }


}

void AES128::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t state[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            state[r][c] = in[r + 4 * c] ^ (uint8_t)((round_keys_[c] >> (24 - 8 * r)) & 0xFF);
        }
    }

    for (int round = 1; round <= 10; ++round) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                state[r][c] = s_box[state[r][c]];
            }
        }

        uint8_t temp;
        // Shift rows
        temp = state[1][0]; state[1][0] = state[1][1]; state[1][1] = state[1][2]; state[1][2] = state[1][3]; state[1][3] = temp;
        temp = state[2][0]; state[2][0] = state[2][2]; state[2][2] = temp;
        temp = state[2][1]; state[2][1] = state[2][3]; state[2][3] = temp;
        temp = state[3][3]; state[3][3] = state[3][2]; state[3][2] = state[3][1]; state[3][1] = state[3][0]; state[3][0] = temp;

        if (round < 10) {
            for (int c = 0; c < 4; ++c) {
                uint8_t s0 = state[0][c], s1 = state[1][c], s2 = state[2][c], s3 = state[3][c];
                state[0][c] = xtime(s0 ^ s1) ^ s1 ^ s2 ^ s3;
                state[1][c] = xtime(s1 ^ s2) ^ s2 ^ s3 ^ s0;
                state[2][c] = xtime(s2 ^ s3) ^ s3 ^ s0 ^ s1;
                state[3][c] = xtime(s3 ^ s0) ^ s0 ^ s1 ^ s2;
            }
        }

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                state[r][c] ^= (uint8_t)((round_keys_[round * 4 + c] >> (24 - 8 * r)) & 0xFF);
            }
        }
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
            state[r][c] = in[r + 4 * c] ^ (uint8_t)((round_keys_[40 + c] >> (24 - 8 * r)) & 0xFF);
        }
    }

    for (int round = 9; round >= 0; --round) {
        uint8_t temp;
        // Inverse Shift rows
        temp = state[1][3]; state[1][3] = state[1][2]; state[1][2] = state[1][1]; state[1][1] = state[1][0]; state[1][0] = temp;
        temp = state[2][0]; state[2][0] = state[2][2]; state[2][2] = temp;
        temp = state[2][1]; state[2][1] = state[2][3]; state[2][3] = temp;
        temp = state[3][0]; state[3][0] = state[3][1]; state[3][1] = state[3][2]; state[3][2] = state[3][3]; state[3][3] = temp;

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                state[r][c] = inv_s_box[state[r][c]];
            }
        }

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                // Standard inverse cipher: AddRoundKey precedes InvMixColumns,
                // so use the original key, not an already mixed round key.
                state[r][c] ^= (uint8_t)((round_keys_[round * 4 + c] >> (24 - 8 * r)) & 0xFF);
            }
        }

        if (round > 0) {
            for (int c = 0; c < 4; ++c) {
                uint8_t s0 = state[0][c], s1 = state[1][c], s2 = state[2][c], s3 = state[3][c];
                state[0][c] = multiply(s0, 0x0E) ^ multiply(s1, 0x0B) ^ multiply(s2, 0x0D) ^ multiply(s3, 0x09);
                state[1][c] = multiply(s0, 0x09) ^ multiply(s1, 0x0E) ^ multiply(s2, 0x0B) ^ multiply(s3, 0x0D);
                state[2][c] = multiply(s0, 0x0D) ^ multiply(s1, 0x09) ^ multiply(s2, 0x0E) ^ multiply(s3, 0x0B);
                state[3][c] = multiply(s0, 0x0B) ^ multiply(s1, 0x0D) ^ multiply(s2, 0x09) ^ multiply(s3, 0x0E);
            }
        }
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
    if ((plain_len % 8) != 0 || plain_len < 16) return false;
    size_t n = plain_len / 8;
    if (n > 16) return false; // Supported limit for WPA2 key wrap

    AES128 aes(kek);
    uint8_t a[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
    uint8_t r[128];
    memcpy(r, plain, plain_len);

    for (int j = 0; j <= 5; ++j) {
        for (size_t i = 1; i <= n; ++i) {
            uint8_t b[16];
            memcpy(b, a, 8);
            memcpy(b + 8, r + (i - 1) * 8, 8);
            aes.encrypt_block(b, b);
            memcpy(a, b, 8);
            uint32_t t = (uint32_t)(n * (size_t)j + i);
            a[7] ^= (uint8_t)(t & 0xFF);
            a[6] ^= (uint8_t)((t >> 8) & 0xFF);
            a[5] ^= (uint8_t)((t >> 16) & 0xFF);
            a[4] ^= (uint8_t)((t >> 24) & 0xFF);
            memcpy(r + (i - 1) * 8, b + 8, 8);
        }
    }

    memcpy(wrapped, a, 8);
    memcpy(wrapped + 8, r, plain_len);
    return true;
}

bool aes_key_unwrap(const uint8_t kek[16], const uint8_t* wrapped, size_t wrapped_len, uint8_t* plain, size_t plain_capacity) {
    if (!kek || !wrapped || !plain || (wrapped_len % 8) != 0 || wrapped_len < 24) return false;
    if (wrapped_len - 8 > plain_capacity) return false;
    size_t n = (wrapped_len - 8) / 8;
    if (n > 16) return false;

    AES128 aes(kek);
    uint8_t a[8];
    memcpy(a, wrapped, 8);
    uint8_t r[128];
    memcpy(r, wrapped + 8, n * 8);

    for (int j = 5; j >= 0; --j) {
        for (size_t i = n; i >= 1; --i) {
            uint32_t t = (uint32_t)(n * (size_t)j + i);
            a[7] ^= (uint8_t)(t & 0xFF);
            a[6] ^= (uint8_t)((t >> 8) & 0xFF);
            a[5] ^= (uint8_t)((t >> 16) & 0xFF);
            a[4] ^= (uint8_t)((t >> 24) & 0xFF);

            uint8_t b[16];
            memcpy(b, a, 8);
            memcpy(b + 8, r + (i - 1) * 8, 8);
            aes.decrypt_block(b, b);
            memcpy(a, b, 8);
            memcpy(r + (i - 1) * 8, b + 8, 8);
        }
    }

    for (int i = 0; i < 8; ++i) {
        if (a[i] != 0xA6) return false;
    }

    memcpy(plain, r, n * 8);
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

    uint8_t b0[16];
    memset(b0, 0, 16);
    b0[0] = (aad_len > 0 ? 0x40 : 0x00) | 0x18 | 0x01;
    memcpy(b0 + 1, nonce, 13);
    b0[14] = (uint8_t)((plain_len >> 8) & 0xFF);
    b0[15] = (uint8_t)(plain_len & 0xFF);

    uint8_t x[16];
    aes.encrypt_block(b0, x);

    if (aad_len > 0) {
        uint8_t block[16];
        memset(block, 0, 16);
        block[0] = (uint8_t)((aad_len >> 8) & 0xFF);
        block[1] = (uint8_t)(aad_len & 0xFF);
        size_t first_copy = aad_len < 14 ? aad_len : 14;
        memcpy(block + 2, aad, first_copy);

        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        aes.encrypt_block(x, x);

        size_t processed = first_copy;
        while (processed < aad_len) {
            memset(block, 0, 16);
            size_t copy_len = (aad_len - processed) < 16 ? (aad_len - processed) : 16;
            memcpy(block, aad + processed, copy_len);
            for (int i = 0; i < 16; ++i) x[i] ^= block[i];
            aes.encrypt_block(x, x);
            processed += copy_len;
        }
    }

    size_t processed = 0;
    while (processed < plain_len) {
        uint8_t block[16];
        memset(block, 0, 16);
        size_t copy_len = (plain_len - processed) < 16 ? (plain_len - processed) : 16;
        memcpy(block, plaintext + processed, copy_len);
        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        aes.encrypt_block(x, x);
        processed += copy_len;
    }

    uint8_t a0[16];
    memset(a0, 0, 16);
    a0[0] = 0x01;
    memcpy(a0 + 1, nonce, 13);

    uint8_t s0[16];
    aes.encrypt_block(a0, s0);
    for (int i = 0; i < 8; ++i) {
        mic[i] = x[i] ^ s0[i];
    }

    uint16_t counter = 1;
    processed = 0;
    while (processed < plain_len) {
        uint8_t ai[16];
        memset(ai, 0, 16);
        ai[0] = 0x01;
        memcpy(ai + 1, nonce, 13);
        ai[14] = (uint8_t)((counter >> 8) & 0xFF);
        ai[15] = (uint8_t)(counter & 0xFF);

        uint8_t si[16];
        aes.encrypt_block(ai, si);

        size_t copy_len = (plain_len - processed) < 16 ? (plain_len - processed) : 16;
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

    uint16_t counter = 1;
    size_t processed = 0;
    while (processed < cipher_len) {
        uint8_t ai[16];
        memset(ai, 0, 16);
        ai[0] = 0x01;
        memcpy(ai + 1, nonce, 13);
        ai[14] = (uint8_t)((counter >> 8) & 0xFF);
        ai[15] = (uint8_t)(counter & 0xFF);

        uint8_t si[16];
        aes.encrypt_block(ai, si);

        size_t copy_len = (cipher_len - processed) < 16 ? (cipher_len - processed) : 16;
        for (size_t i = 0; i < copy_len; ++i) {
            plaintext[processed + i] = ciphertext[processed + i] ^ si[i];
        }

        processed += copy_len;
        counter++;
    }

    uint8_t b0[16];
    memset(b0, 0, 16);
    b0[0] = (aad_len > 0 ? 0x40 : 0x00) | 0x18 | 0x01;
    memcpy(b0 + 1, nonce, 13);
    b0[14] = (uint8_t)((cipher_len >> 8) & 0xFF);
    b0[15] = (uint8_t)(cipher_len & 0xFF);

    uint8_t x[16];
    aes.encrypt_block(b0, x);

    if (aad_len > 0) {
        uint8_t block[16];
        memset(block, 0, 16);
        block[0] = (uint8_t)((aad_len >> 8) & 0xFF);
        block[1] = (uint8_t)(aad_len & 0xFF);
        size_t first_copy = aad_len < 14 ? aad_len : 14;
        memcpy(block + 2, aad, first_copy);

        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        aes.encrypt_block(x, x);

        size_t aad_proc = first_copy;
        while (aad_proc < aad_len) {
            memset(block, 0, 16);
            size_t copy_len = (aad_len - aad_proc) < 16 ? (aad_len - aad_proc) : 16;
            memcpy(block, aad + aad_proc, copy_len);
            for (int i = 0; i < 16; ++i) x[i] ^= block[i];
            aes.encrypt_block(x, x);
            aad_proc += copy_len;
        }
    }

    processed = 0;
    while (processed < cipher_len) {
        uint8_t block[16];
        memset(block, 0, 16);
        size_t copy_len = (cipher_len - processed) < 16 ? (cipher_len - processed) : 16;
        memcpy(block, plaintext + processed, copy_len);
        for (int i = 0; i < 16; ++i) x[i] ^= block[i];
        aes.encrypt_block(x, x);
        processed += copy_len;
    }

    uint8_t a0[16];
    memset(a0, 0, 16);
    a0[0] = 0x01;
    memcpy(a0 + 1, nonce, 13);

    uint8_t s0[16];
    aes.encrypt_block(a0, s0);

    uint8_t computed_mic[8];
    for (int i = 0; i < 8; ++i) {
        computed_mic[i] = x[i] ^ s0[i];
    }

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
