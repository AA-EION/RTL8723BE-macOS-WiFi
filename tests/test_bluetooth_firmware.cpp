// SPDX-License-Identifier: MIT
#include "firmware.hpp"
#include <assert.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <stdio.h>
int main() {
    std::ifstream stream("bluetooth/firmware/rtl8723b_fw.bin", std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)), {});
    assert(data.size()==45048);
    rtl_bt::Patch patch;
    unsigned matched=0;
    for(unsigned rom=0;rom<256;++rom) {
        if(!rtl_bt::parse(data.data(),data.size(),uint8_t(rom),patch)) continue;
        ++matched;
        assert(patch.length>=4 && patch.offset+patch.length<=data.size()-7);
        size_t count=rtl_bt::fragmentCount(patch.length), transferred=0;
        for(size_t i=0;i<count;++i) {
            uint8_t index=rtl_bt::fragmentIndex(i,count);
            assert(((index&0x80)!=0)==(i+1==count));
            assert((index&0x7f)==(i==0?0:(i-1)%127+1));
            size_t length=i+1==count?patch.length%252:252; transferred+=length;
        }
        assert(transferred==patch.length);
    }
    assert(matched>0);
    for(size_t n=0;n<data.size();++n) rtl_bt::parse(data.data(),n,0,patch);
    for(size_t i=0;i<40;++i) {
        auto malformed=data;malformed[i]^=0xff;rtl_bt::parse(malformed.data(),malformed.size(),0,patch);
    }
    auto other=data;other[0]^=1;assert(!rtl_bt::parse(other.data(),other.size(),0,patch));
    assert(rtl_bt::fragmentCount(252)==2 && rtl_bt::fragmentIndex(1,2)==0x81);
    assert(rtl_bt::fragmentIndex(127,200)==127 && rtl_bt::fragmentIndex(128,200)==1);
    puts("RTL8723B firmware parser, truncation and Linux fragment contract passed.");
}
