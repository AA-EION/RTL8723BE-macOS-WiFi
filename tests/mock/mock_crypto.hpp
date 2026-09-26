#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <array>

namespace rtl_crypto {

// SHA-1 digest calculation
void sha1(const uint8_t* data, size_t len, uint8_t digest[20]);

// HMAC-SHA1
void hmac_sha1(const uint8_t* key, size_t key_len,
               const uint8_t* data, size_t data_len,
               uint8_t mac[20]);

// PBKDF2-HMAC-SHA1 (RFC 2898 / RFC 6070)
void pbkdf2_sha1(const char* passphrase, size_t pass_len,
                 const uint8_t* salt, size_t salt_len,
                 uint32_t iterations,
                 uint8_t* out_key, size_t out_len);

// IEEE 802.11i PRF-512 for WPA2 PTK derivation
// out_ptk must be at least 64 bytes (KCK=16, KEK=16, TK=16, etc.)
void prf_512(const uint8_t pmk[32],
             const char* label,
             const uint8_t addr1[6], const uint8_t addr2[6],
             const uint8_t nonce1[32], const uint8_t nonce2[32],
             uint8_t out_ptk[64]);

// AES-128 Single Block Cipher (FIPS 197)
class AES128 {
public:
    AES128();
    explicit AES128(const uint8_t key[16]);
    void set_key(const uint8_t key[16]);
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint32_t round_keys_[44];
    uint32_t inv_round_keys_[44];
};

// RFC 3394 AES Key Wrap & Unwrap (for WPA2 GTK encryption/decryption under KEK)
bool aes_key_wrap(const uint8_t kek[16], const uint8_t* plain, size_t plain_len, uint8_t* wrapped);
bool aes_key_unwrap(const uint8_t kek[16], const uint8_t* wrapped, size_t wrapped_len, uint8_t* plain);

// IEEE 802.11 CCMP (AES-128 CCM Mode)
// Nonce is 13 bytes: Priority (1 byte) || Addr2 (6 bytes) || PN (6 bytes)
// MIC is 8 bytes
bool ccmp_encrypt(const uint8_t key[16],
                  const uint8_t nonce[13],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* plaintext, size_t plain_len,
                  uint8_t* ciphertext,
                  uint8_t mic[8]);

bool ccmp_decrypt(const uint8_t key[16],
                  const uint8_t nonce[13],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* ciphertext, size_t cipher_len,
                  const uint8_t mic[8],
                  uint8_t* plaintext);

// CRC32 (IEEE 802.11 FCS)
uint32_t crc32_80211(const uint8_t* data, size_t len);

} // namespace rtl_crypto
