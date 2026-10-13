// SPDX-License-Identifier: MIT AND BSD-3-Clause AND ISC
// Copyright (c) 2026 Issen Software Group
/*-
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002, 2003 Sam Leffler, Errno Consulting
 * Copyright (c) 2007-2009 Damien Bergamini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*-
 * Copyright (c) 2007,2008 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "RTL8723BE_protocol.hpp"
#include "RTL8723BE_crypto.hpp"
#include <string.h>

namespace rtl_wifi {
static uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }
void erase(void *p, size_t len) {
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(p);
    while (len--) *bytes++ = 0;
}
bool equal(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t difference = 0;
    for (size_t i = 0; i < len; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool derivePMK(const char *password, size_t length, const uint8_t *ssid, size_t ssidLength, uint8_t pmk[32]) {
    if (!password || !ssid || !pmk || !ssidLength || ssidLength > 32) return false;
    if (length == 64) {
        for (size_t i = 0; i < 32; ++i) {
            int a = hex(password[i * 2]), b = hex(password[i * 2 + 1]);
            if (a < 0 || b < 0) { erase(pmk, 32); return false; }
            pmk[i] = uint8_t((a << 4) | b);
        }
        return true;
    }
    if (length < 8 || length > 63) return false;
    for (size_t i = 0; i < length; ++i) if (uint8_t(password[i]) < 32 || uint8_t(password[i]) > 126) return false;
    rtl_crypto::pbkdf2_sha1(password, length, ssid, ssidLength, 4096, pmk, 32);
    return true;
}
static bool suite(const uint8_t *p, uint8_t type) {
    return p[0] == 0 && p[1] == 0x0f && p[2] == 0xac && p[3] == type;
}
bool compatibleRSN(const uint8_t *ie, size_t length) {
    if (!ie || length < 18 || le16(ie) != 1 || !suite(ie + 2, 4)) return false;
    size_t pos = 6;
    uint16_t count = le16(ie + pos); pos += 2;
    if (!count || count > (length - pos) / 4) return false;
    bool ccmp = false;
    for (unsigned i = 0; i < count; ++i, pos += 4) ccmp |= suite(ie + pos, 4);
    if (!ccmp || length - pos < 2) return false;
    count = le16(ie + pos); pos += 2;
    if (!count || count > (length - pos) / 4) return false;
    bool psk = false;
    for (unsigned i = 0; i < count; ++i, pos += 4) psk |= suite(ie + pos, 2);
    if (!psk) return false;
    if (length == pos) return true;
    if (length - pos < 2 || (le16(ie + pos) & 0x0040)) return false; // PMF required
    pos += 2;
    if (length == pos) return true;
    if (length - pos < 2) return false;
    count = le16(ie + pos); pos += 2;
    if (count > (length - pos) / 16) return false;
    pos += size_t(count) * 16;
    return length == pos || length - pos == 4; // optional group-management cipher
}
bool parseIEs(const uint8_t *ies, size_t length, Peer &peer) {
    memset(&peer, 0, sizeof(peer));
    if (!ies && length) return false;
    static const uint8_t rates[12] = {2,4,11,22,12,18,24,36,48,72,96,108};
    size_t pos = 0;
    bool htSeen = false;
    while (pos < length) {
        if (length - pos < 2) return false;
        uint8_t tag = ies[pos], n = ies[pos + 1]; pos += 2;
        if (n > length - pos) return false;
        const uint8_t *p = ies + pos;
        if (tag == 1 || tag == 50) {
            for (unsigned i = 0; i < n; ++i) {
                unsigned rate = 0;
                while (rate < 12 && rates[rate] != (p[i] & 0x7f)) ++rate;
                if (rate == 12) { if (p[i] & 0x80) return false; continue; }
                peer.rates |= uint16_t(1U << rate);
                if (p[i] & 0x80) peer.basicRates |= uint16_t(1U << rate);
            }
        } else if (tag == 48) {
            if (peer.rsnLength) return false;
            peer.wpa2 = compatibleRSN(p, n);
            peer.rsnLength = uint16_t(n + 2);
            memcpy(peer.rsn, p - 2, peer.rsnLength);
        } else if (tag == 45 && n == 26) {
            htSeen = true;
            peer.shortGI = (le16(p) & (1U << 5)) != 0;
            peer.mcs = p[3];
            peer.density = (p[2] >> 2) & 7;
        } else if (tag == 42 && n == 1) {
            peer.protection = (p[0] & 2) != 0;
        } else if (tag == 61 && n == 22) {
            peer.protection |= (le16(p + 2) & 3) != 0;
        } else if (tag == 221 && n >= 7 && p[0] == 0 && p[1] == 0x50 && p[2] == 0xf2 &&
                   p[3] == 2 && p[5] == 1 && (p[4] == 0 || p[4] == 1)) {
            peer.wmm = true;
            if (p[4] == 1) {
                if (n != 24) return false;
                uint8_t seen = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    const uint8_t *ac = p + 8 + i * 4;
                    unsigned index = (ac[0] >> 5) & 3;
                    if (seen & (1U << index)) return false;
                    seen |= uint8_t(1U << index);
                    unsigned aifs = ac[0] & 15;
                    unsigned ecwMin = ac[1] & 15, ecwMax = ac[1] >> 4;
                    if (!aifs || ecwMin > ecwMax) return false;
                    // Linux EDCA registers: TXOP(16), ECWmax(4), ECWmin(4), AIFS(us).
                    peer.edca[index] = (uint32_t(le16(ac + 2)) << 16) | (uint32_t(ac[1]) << 8) | (10 + aifs * 9);
                }
            }
        }
        pos += n;
    }
    peer.ht = htSeen && peer.wmm && peer.mcs != 0;
    return true;
}
uint8_t highestRate(const Peer &peer) {
    if (peer.ht) for (int i = 7; i >= 0; --i) if (peer.mcs & (1U << i)) return uint8_t(12 + i);
    for (int i = 11; i >= 0; --i) if (peer.rates & (1U << i)) return uint8_t(i);
    return 0;
}
bool snapHeader(const uint8_t *data, size_t length) {
    static const uint8_t snap[] = {0xaa,0xaa,3,0,0,0};
    return data && length >= 8 && memcmp(data, snap, sizeof(snap)) == 0;
}
bool validateAMSDU(const uint8_t *data, size_t length, const uint8_t station[6]) {
    // Whole-aggregate validation prevents delivery of a valid prefix of a malformed packet.
    // The RFC1042 destination exclusion also prevents forged A-MSDU-present attacks.
    static const uint8_t forbidden[] = {0xaa,0xaa,3,0,0,0};
    if (!data || !station || length > 3839 || !length) return false;
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < 22) return false;
        const uint8_t *sub = data + offset;
        size_t amount = be16(sub + 12);
        if (amount < 8 || amount > 1508 || amount > length - offset - 14 ||
            memcmp(sub, forbidden, 6) == 0 || memcmp(sub, station, 6) != 0 || (sub[6] & 1)) return false;
        // EAPOL is exchanged as individual MSDUs; never accept an aggregate key exchange.
        if (snapHeader(sub + 14, amount) && be16(sub + 20) == 0x888e) return false;
        offset += 14 + amount;
        if (offset == length) return true;
        size_t padding = (4 - ((14 + amount) & 3)) & 3;
        if (padding >= length - offset) return false;
        offset += padding;
    }
    return false;
}
size_t associationIEs(const Peer &peer, uint8_t *out, size_t capacity) {
    size_t needed = (peer.wmm ? 9U : 0U) + (peer.ht ? 28U : 0U);
    if (!out || capacity < needed) return 0;
    size_t pos = 0;
    if (peer.wmm) {
        static const uint8_t wmm[] = {221,7,0,0x50,0xf2,2,0,1,0};
        memcpy(out, wmm, sizeof(wmm)); pos += sizeof(wmm);
    }
    if (peer.ht) {
        memset(out + pos, 0, 28);
        out[pos] = 45; out[pos + 1] = 26;
        // HT20, one stream, SGI20, no A-MSDU >3839, no STBC/LDPC/40MHz.
        // SM power save disabled; RX AMPDU limit 8191, density 16us.
        out[pos + 2] = uint8_t(0x0c | (peer.shortGI ? 0x20 : 0));
        out[pos + 4] = 7U << 2;
        out[pos + 5] = 0xff;
        out[pos + 17] = 1; // ieee80211_mcs_info.tx_params: MCS octet 12
        pos += 28;
    }
    return pos;
}

void Supplicant::reset() { erase(this, sizeof(*this)); }
void Supplicant::begin(const uint8_t pmk[32], const uint8_t sta[6], const uint8_t ap[6], const uint8_t *rsnIE, size_t rsnLength) {
    reset(); memcpy(master, pmk, 32); memcpy(station, sta, 6); memcpy(accessPoint, ap, 6);
    if (rsnIE && rsnLength <= sizeof(peerRSN)) { memcpy(peerRSN, rsnIE, rsnLength); peerRSNLength = rsnLength; }
}
static const uint8_t rsn[] = {48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
bool Supplicant::receive(const uint8_t *e, size_t length, const uint8_t randomNonce[32],
                        uint8_t *reply, size_t capacity, Result &result) {
    memset(&result, 0, sizeof(result));
    if (!e || !reply || length < 99 || length > 512 || (e[0] != 1 && e[0] != 2) || e[1] != 3 || e[4] != 2) return false;
    size_t packetLength = 4 + be16(e + 2), dataLength = be16(e + 97);
    if (packetLength != length || dataLength != length - 99) return false;
    uint16_t info = be16(e + 5);
    if ((info & 7) != 2 || !(info & 0x80) || (info & (0x0400 | 0x0800 | 0xe000))) return false;
    uint64_t replay = 0;
    for (unsigned i = 0; i < 8; ++i) replay = (replay << 8) | e[9 + i];
    bool pairwise = (info & 8) != 0, mic = (info & 0x100) != 0;
    // RSN group messages may set this unused field to zero (BSD validates KDE length).
    if (pairwise ? be16(e + 7) != 16 : (be16(e + 7) != 0 && be16(e + 7) != 16)) return false;
    if (info & 0x0030) return false; // WPA descriptor key-index bits are reserved for RSN
    bool m1 = pairwise && !mic;
    size_t replyLength = m1 ? 99 + sizeof(rsn) : 99;
    if (capacity < replyLength) return false;
    const uint8_t *kck = installedPTK;
    bool duplicate = false;
    if (m1) {
        if (info != 0x008a || !randomNonce || (accepted && replay <= acceptedReplay)) return false;
        if (pending && replay < m1Replay) return false;
        bool repeated = pending && replay == m1Replay;
        if (repeated && !equal(anonce, e + 17, 32)) return false;
        if (!repeated) {
            memcpy(anonce, e + 17, 32); memcpy(snonce, randomNonce, 32);
            rtl_crypto::prf_512(master, "Pairwise key expansion", accessPoint, station, anonce, snonce, candidatePTK);
        }
        m1Replay = replay; pending = true; kck = candidatePTK;
    } else {
        if (!(info & 0x0200)) return false; // secure bit
        if (pairwise) {
            if (!(info & 0x40) || !pending || replay <= m1Replay || !equal(anonce, e + 17, 32)) return false;
            kck = candidatePTK;
        } else {
            if (!installed || (info & 0x40) || !(info & 0x1000)) return false;
        }
        if (accepted && replay < acceptedReplay) return false;
        duplicate = accepted && replay == acceptedReplay;
        if (duplicate && pairwise && !installed) return false;
        uint8_t verify[512], digest[20];
        memcpy(verify, e, length); memset(verify + 81, 0, 16);
        rtl_crypto::hmac_sha1(kck, 16, verify, length, digest);
        bool valid = equal(digest, e + 81, 16);
        erase(verify, sizeof(verify)); erase(digest, sizeof(digest));
        if (!valid) return false;
        uint8_t keys[4][16] = {}, keyMask = 0;
        bool rsnSeen = false;
        if (dataLength) {
            if (!(info & 0x1000) || dataLength < 24 || dataLength % 8) return false;
            uint8_t data[504];
            if (!rtl_crypto::aes_key_unwrap(kck + 16, e + 99, dataLength, data, sizeof(data))) { erase(data, sizeof(data)); return false; }
            size_t plainLength = dataLength - 8, pos = 0;
            valid = true;
            while (pos < plainLength) {
                if (data[pos] == 0xdd && (pos + 1 == plainLength || data[pos + 1] == 0)) break;
                if (plainLength - pos < 2) { valid = false; break; }
                uint8_t id = data[pos], n = data[pos + 1]; pos += 2;
                if (n > plainLength - pos) { valid = false; break; }
                if (id == 48) {
                    if (!compatibleRSN(data + pos, n) || (!rsnSeen &&
                        (peerRSNLength != size_t(n) + 2 || !equal(peerRSN, data + pos - 2, peerRSNLength)))) {
                        valid = false; break;
                    }
                    rsnSeen = true;
                }
                if (id == 221 && n >= 4 && suite(data + pos, 1)) {
                    if (n != 22 || (data[pos + 4] & 0xf8) || data[pos + 5]) { valid = false; break; }
                    unsigned index = data[pos + 4] & 3;
                    if (keyMask & (1U << index)) { valid = false; break; }
                    keyMask |= uint8_t(1U << index);
                    memcpy(keys[index], data + pos + 6, 16);
                }
                pos += n;
            }
            erase(data, sizeof(data));
            if (!valid) { erase(keys, sizeof(keys)); return false; }
        }
        if (pairwise && !rsnSeen) { erase(keys, sizeof(keys)); return false; }
        if (!duplicate) {
            if ((!installed || !pairwise) && !keyMask) { erase(keys, sizeof(keys)); return false; }
            if (pairwise) {
                result.pairwiseChanged = !installed || !equal(installedPTK + 32, candidatePTK + 32, 16);
                memcpy(installedPTK, candidatePTK, 64);
                installed = true;
            }
            for (unsigned i = 0; i < 4; ++i) if (keyMask & (1U << i)) {
                if (!hasGTK(i) || !equal(groupKeys[i], keys[i], 16)) {
                    memcpy(groupKeys[i], keys[i], 16); result.groupChanged |= uint8_t(1U << i);
                    groupRSC[i] = 0;
                    for (unsigned byte = 0; byte < 6; ++byte) groupRSC[i] |= uint64_t(e[65 + byte]) << (byte * 8);
                }
                groupValid |= uint8_t(1U << i);
            }
            accepted = true; acceptedReplay = replay;
        }
        erase(keys, sizeof(keys));
    }
    memset(reply, 0, replyLength);
    reply[0] = e[0]; reply[1] = 3;
    reply[2] = uint8_t((replyLength - 4) >> 8); reply[3] = uint8_t(replyLength - 4); reply[4] = 2;
    uint16_t replyInfo = m1 ? 0x010a : (pairwise ? 0x030a : 0x0302);
    reply[5] = uint8_t(replyInfo >> 8); reply[6] = uint8_t(replyInfo);
    memcpy(reply + 9, e + 9, 8);
    if (m1) {
        memcpy(reply + 17, snonce, 32);
        reply[98] = sizeof(rsn); memcpy(reply + 99, rsn, sizeof(rsn));
    }
    uint8_t digest[20];
    rtl_crypto::hmac_sha1(kck, 16, reply, replyLength, digest);
    memcpy(reply + 81, digest, 16); erase(digest, sizeof(digest));
    result.replyLength = replyLength;
    return true;
}
} // namespace rtl_wifi
