// SPDX-License-Identifier: MIT
// Production protocol tests: OpenBSD PAE invariants, IEEE RSN fields,
// and Linux rate/HT encodings. This links the same supplicant as the kext.
#include "RTL8723BE_protocol.hpp"
#include "RTL8723BE_crypto.hpp"
#include "RTL8723BE_hw.hpp"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
using Bytes = std::vector<uint8_t>;
static const uint8_t rsn[] = {48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
static Bytes eapol(uint16_t info, uint64_t replay, const uint8_t *nonce = nullptr) {
    Bytes p(99); p[0] = 2; p[1] = 3; p[3] = 95; p[4] = 2;
    p[5] = uint8_t(info >> 8); p[6] = uint8_t(info); p[8] = 16;
    for (unsigned i = 0; i < 8; ++i) p[16 - i] = uint8_t(replay >> (i * 8));
    if (nonce) memcpy(p.data() + 17, nonce, 32);
    return p;
}
static void mic(Bytes &p, const uint8_t *ptk) {
    memset(p.data() + 81, 0, 16);
    uint8_t hash[20]; rtl_crypto::hmac_sha1(ptk, 16, p.data(), p.size(), hash);
    memcpy(p.data() + 81, hash, 16);
}
static Bytes keyMessage(uint16_t info, uint64_t replay, const uint8_t *nonce,
                        const uint8_t *ptk, uint8_t id, uint8_t keyByte, bool includeRSN = true) {
    Bytes p = eapol(info, replay, nonce), data;
    if (includeRSN) data.insert(data.end(), rsn, rsn + sizeof(rsn));
    data.insert(data.end(), {221,22,0,15,172,1,id,0});
    data.insert(data.end(), 16, keyByte);
    if (data.size() % 8) { data.push_back(221); while (data.size() % 8) data.push_back(0); }
    size_t n = data.size() + 8;
    p.resize(99 + n);
    assert(rtl_crypto::aes_key_wrap(ptk + 16, data.data(), data.size(), p.data() + 99));
    p[2] = uint8_t((p.size() - 4) >> 8); p[3] = uint8_t(p.size() - 4);
    p[97] = uint8_t(n >> 8); p[98] = uint8_t(n);
    p[65] = 7; // initial group receive sequence counter
    mic(p, ptk);
    return p;
}
int main() {
    uint8_t pmk[32], expected[] = {0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e};
    assert(rtl_wifi::derivePMK("password",8,(const uint8_t *)"IEEE",4,pmk));
    assert(!memcmp(pmk, expected, 32)); // published WPA2 password/IEEE PMK vector
    assert(!rtl_wifi::derivePMK("short",5,(const uint8_t *)"IEEE",4,pmk));
    assert(rtl_wifi::derivePMK("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",64,(const uint8_t *)"IEEE",4,pmk));
    for (unsigned i = 0; i < 32; ++i) assert(pmk[i] == i);
    assert(!rtl_wifi::derivePMK("z00102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",64,(const uint8_t *)"IEEE",4,pmk));
    assert(rtl_wifi::derivePMK("password",8,(const uint8_t *)"IEEE",4,pmk));

    rtl_wifi::Peer peer;
    Bytes ies = {1,4,0x82,0x84,0x8b,0x96};
    ies.insert(ies.end(),rsn,rsn+sizeof(rsn));
    ies.insert(ies.end(),{221,7,0,0x50,0xf2,2,0,1,0});
    Bytes ht(28); ht[0]=45; ht[1]=26; ht[2]=0x20; ht[5]=0xff;
    ies.insert(ies.end(),ht.begin(),ht.end());
    assert(rtl_wifi::parseIEs(ies.data(),ies.size(),peer));
    assert(peer.ht && peer.wpa2 && peer.wmm && peer.shortGI && rtl_wifi::highestRate(peer)==19);
    uint8_t association[37]; assert(rtl_wifi::associationIEs(peer,association,sizeof(association))==37);
    assert(association[9]==45 && association[11]==0x2c && association[14]==0xff && association[26]==1);
    assert(!rtl_wifi::associationIEs(peer,association,36));
    for (size_t n=1;n<sizeof(rsn)-2;++n) if (n != 18) assert(!rtl_wifi::compatibleRSN(rsn+2,n));
    assert(rtl_wifi::compatibleRSN(rsn+2,18)); // capabilities are optional
    Bytes bad(rsn,rsn+sizeof(rsn)); bad[20]=0x40; assert(!rtl_wifi::compatibleRSN(bad.data()+2,bad.size()-2));
    bad[20]=0;bad[19]=8; assert(!rtl_wifi::compatibleRSN(bad.data()+2,bad.size()-2)); // SAE only
    bad.assign({1,1,0xff}); assert(!rtl_wifi::parseIEs(bad.data(),bad.size(),peer)); // unknown mandatory rate
    bad.assign({1,2,0x82}); assert(!rtl_wifi::parseIEs(bad.data(),bad.size(),peer));
    bad.assign({1,4,0x82,0x84,0x8b,0x96});
    assert(rtl_wifi::parseIEs(bad.data(),bad.size(),peer)); assert(!peer.wpa2 && !peer.ht && rtl_wifi::highestRate(peer)==3);
    TxDescPci desc = {}; desc.set_shortgi(true); desc.set_security(3); desc.set_tx_rate(19);
    assert(desc.dw5==(1U<<4) && desc.dw1==(3U<<22) && desc.dw4==19);
    uint8_t camMAC[6]={0,1,2,3,4,5}, camKey[16];
    for (unsigned i=0;i<16;++i) camKey[i]=uint8_t(i);
    uint32_t cam[8]; rtlCCMPPairwiseCAM(camMAC,camKey,cam);
    assert(cam[0]==0x01008010 && cam[1]==0x05040302 && cam[2]==0x03020100);
    assert(cam[5]==0x0f0e0d0c && cam[6]==0 && cam[7]==0);
    RxDesc32 rx={};rx.dw2=1U<<28;assert(rx.get_c2h_report());

    // Authenticated aggregate validation, including the OpenBSD FragAttacks exclusion.
    Bytes sub={0,1,2,3,4,5,6,7,8,9,10,11,0,9,0xaa,0xaa,3,0,0,0,8,0,42};
    assert(rtl_wifi::validateAMSDU(sub.data(),sub.size(),camMAC));
    Bytes aggregate=sub; aggregate.push_back(0); aggregate.insert(aggregate.end(),sub.begin(),sub.end());
    assert(rtl_wifi::validateAMSDU(aggregate.data(),aggregate.size(),camMAC));
    for(size_t n=0;n<sub.size();++n) assert(!rtl_wifi::validateAMSDU(sub.data(),n,camMAC));
    Bytes malformed=aggregate; malformed.back()=0; malformed[sub.size()+1+13]=255;
    assert(!rtl_wifi::validateAMSDU(malformed.data(),malformed.size(),camMAC));
    malformed=sub;memcpy(malformed.data(),sub.data()+14,6);
    assert(!rtl_wifi::validateAMSDU(malformed.data(),malformed.size(),camMAC));
    malformed=sub;malformed[20]=0x88;malformed[21]=0x8e;
    assert(!rtl_wifi::validateAMSDU(malformed.data(),malformed.size(),camMAC));
    malformed=sub;malformed[0]=1;assert(!rtl_wifi::validateAMSDU(malformed.data(),malformed.size(),camMAC));

    rtl_wifi::Supplicant wpa;
    uint8_t sta[6]={0,1,2,3,4,5}, ap[6]={6,7,8,9,10,11}, snonce[32], anonce[32], ptk[64], reply[512];
    memset(snonce,0x55,32); memset(anonce,0xaa,32);
    wpa.begin(pmk,sta,ap,rsn,sizeof(rsn));
    rtl_wifi::Supplicant::Result result;
    auto m1=eapol(0x008a,0,anonce);
    assert(wpa.receive(m1.data(),m1.size(),snonce,reply,sizeof(reply),result));
    assert(result.replyLength==121 && reply[5]==1 && reply[6]==10 && !wpa.connected());
    Bytes firstReply(reply,reply+result.replyLength);
    uint8_t another[32]={};
    assert(wpa.receive(m1.data(),m1.size(),another,reply,sizeof(reply),result));
    assert(!memcmp(reply,firstReply.data(),firstReply.size())); // zero counter duplicate keeps SNonce
    rtl_crypto::prf_512(pmk,"Pairwise key expansion",ap,sta,anonce,snonce,ptk);
    auto m3=keyMessage(0x13ca,1,anonce,ptk,1,0xbb);
    auto corrupt=m3; corrupt[81]^=1;
    assert(!wpa.receive(corrupt.data(),corrupt.size(),snonce,reply,sizeof(reply),result)); assert(!wpa.connected());
    corrupt=m3; corrupt[17]^=1;mic(corrupt,ptk);
    assert(!wpa.receive(corrupt.data(),corrupt.size(),snonce,reply,sizeof(reply),result));
    auto missingRSN=keyMessage(0x13ca,1,anonce,ptk,1,0xbb,false);
    assert(!wpa.receive(missingRSN.data(),missingRSN.size(),snonce,reply,sizeof(reply),result));
    assert(wpa.receive(m3.data(),m3.size(),snonce,reply,sizeof(reply),result));
    assert(wpa.connected() && result.pairwiseChanged && result.groupChanged==2 && wpa.hasGTK(1) && wpa.gtkRSC(1)==7);
    assert(!memcmp(wpa.ptk(),ptk,64)); assert(reply[5]==3 && reply[6]==10 && result.replyLength==99);
    assert(wpa.receive(m3.data(),m3.size(),snonce,reply,sizeof(reply),result));
    assert(!result.pairwiseChanged && !result.groupChanged); // repeated M3 gets M4, never reinstall
    assert(!wpa.receive(m1.data(),m1.size(),snonce,reply,sizeof(reply),result));
    auto group=keyMessage(0x1382,2,nullptr,ptk,2,0xcc,false);
    group[8]=0; mic(group,ptk); // RSN group key length is unused; zero is valid
    assert(wpa.receive(group.data(),group.size(),snonce,reply,sizeof(reply),result));
    assert(result.groupChanged==4 && !result.pairwiseChanged && reply[6]==2 && wpa.gtk(2)[0]==0xcc);
    assert(wpa.receive(group.data(),group.size(),snonce,reply,sizeof(reply),result)); assert(!result.groupChanged);
    auto sameGTK=keyMessage(0x1382,3,nullptr,ptk,2,0xcc,false);
    assert(wpa.receive(sameGTK.data(),sameGTK.size(),snonce,reply,sizeof(reply),result)); assert(!result.groupChanged);
    assert(!wpa.receive(group.data(),group.size(),snonce,reply,sizeof(reply),result));
    // Fresh M1 derives a candidate without disrupting installed traffic keys.
    anonce[0]^=1; m1=eapol(0x008a,4,anonce);
    assert(wpa.receive(m1.data(),m1.size(),another,reply,sizeof(reply),result)); assert(!memcmp(wpa.ptk(),ptk,64));
    uint8_t newPTK[64]; rtl_crypto::prf_512(pmk,"Pairwise key expansion",ap,sta,anonce,another,newPTK);
    m3=keyMessage(0x13ca,5,anonce,newPTK,1,0xdd);
    assert(wpa.receive(m3.data(),m3.size(),snonce,reply,sizeof(reply),result)); assert(result.pairwiseChanged && result.groupChanged==2);
    // Fuzz every truncation and mutated length byte in real production parsing.
    for(size_t n=0;n<m3.size();++n) assert(!wpa.receive(m3.data(),n,snonce,reply,sizeof(reply),result));
    for(size_t n=0;n<sizeof(rsn);++n) {
        auto mutated=Bytes(rsn,rsn+sizeof(rsn)); mutated[n]^=0xff;
        rtl_wifi::parseIEs(mutated.data(),mutated.size(),peer);
    }
    wpa.reset(); assert(!wpa.connected() && !wpa.hasGTK(1));
    puts("Production WPA2, rekey/replay, RSN, HT/WMM and TX descriptor tests passed.");
}
