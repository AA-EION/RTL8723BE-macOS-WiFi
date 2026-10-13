// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Issen Software Group
#ifndef RTL8723BE_PROTOCOL_HPP
#define RTL8723BE_PROTOCOL_HPP

#include <stdint.h>
#include <stddef.h>

namespace rtl_wifi {
void erase(void *p, size_t len);
bool equal(const uint8_t *a, const uint8_t *b, size_t len);
bool derivePMK(const char *password, size_t length, const uint8_t *ssid, size_t ssidLength, uint8_t pmk[32]);

struct Peer {
    uint16_t rates;             // Linux rate bitmap: CCK bits 0..3, OFDM 4..11
    uint16_t basicRates;
    uint16_t capabilities;      // fixed beacon/association capability field
    bool wpa2;
    bool ht;
    bool shortGI;
    bool wmm;
    bool protection;
    uint8_t mcs;
    uint8_t density;
    uint16_t rsnLength;
    uint8_t rsn[257];
    uint32_t edca[4];           // BE, BK, VI, VO
};
// Parse complete IE streams; reject truncation and unsupported mandatory basic rates.
bool parseIEs(const uint8_t *ies, size_t length, Peer &peer);
bool compatibleRSN(const uint8_t *ie, size_t length);
uint8_t highestRate(const Peer &peer);
size_t associationIEs(const Peer &peer, uint8_t *out, size_t capacity);
// OpenBSD ieee80211_amsdu_decap_validate(): validate every subframe before delivery.
bool validateAMSDU(const uint8_t *data, size_t length, const uint8_t station[6]);
bool snapHeader(const uint8_t *data, size_t length);

// Portable WPA2-PSK/CCMP supplicant shared by the kext and production tests.
// No keys or packet numbers are installed until MIC, ANonce and wrapped data pass.
class Supplicant {
public:
    void reset();
    void begin(const uint8_t pmk[32], const uint8_t sta[6], const uint8_t ap[6], const uint8_t *rsn, size_t rsnLength);
    struct Result {
        size_t replyLength;
        bool pairwiseChanged;
        uint8_t groupChanged;  // bitmask: reset RX PN only for genuinely new GTKs
    };
    bool receive(const uint8_t *eapol, size_t length, const uint8_t randomNonce[32],
                 uint8_t *reply, size_t capacity, Result &result);
    const uint8_t *ptk() const { return installedPTK; }
    const uint8_t *gtk(unsigned id) const { return groupKeys[id & 3]; }
    bool hasGTK(unsigned id) const { return (groupValid & (1U << (id & 3))) != 0; }
    uint64_t gtkRSC(unsigned id) const { return groupRSC[id & 3]; }
    bool connected() const { return installed; }
private:
    uint8_t master[32], station[6], accessPoint[6];
    uint8_t anonce[32], snonce[32], candidatePTK[64], installedPTK[64];
    uint8_t groupKeys[4][16], groupValid;
    uint64_t groupRSC[4];
    uint8_t peerRSN[257];
    size_t peerRSNLength;
    uint64_t m1Replay, acceptedReplay;
    bool pending, installed, accepted;
};
} // namespace rtl_wifi
#endif
