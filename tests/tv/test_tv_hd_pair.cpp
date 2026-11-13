// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD / SD simulcast pairing: the name normaliser, the LCN rule, the picture size classes (service type, PMT stream type, SPS), the pairing across
// services and multiplexes, the user table and the catalogue round trip.
#include "dect2/tv/hd_pair.h"
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::hd;
#include "../common/test_util.h"

struct BitW {
    std::vector<uint8_t> v; int n = 0;
    void bit(int b) { if (n % 8 == 0) v.push_back(0); if (b) v.back() |= 0x80 >> (n % 8); n++; }
    void u(unsigned x, int len) { for (int i = len - 1; i >= 0; i--) bit((x >> i) & 1); }
    void ue(unsigned x) { x++; int l = 0; while ((x >> l) > 1) l++; u(0, l); u(x, l + 1); }
    // insert emulation prevention bytes
    std::vector<uint8_t> escaped() const {
        std::vector<uint8_t> o; int z = 0;
        for (uint8_t b : v) { if (z >= 2 && b <= 3) { o.push_back(3); z = 0; } o.push_back(b); z = b == 0 ? z + 1 : 0; }
        return o;
    }
};

// H.264 High profile SPS for a picture of w x h luma samples (4:2:0), frame_mbs_only given, cropped to the exact size
static std::vector<uint8_t> h264Sps(int w, int h, bool frameOnly) {
    BitW b;
    b.u(100, 8); b.u(0, 8); b.u(40, 8);
    b.ue(0);        // sps id
    b.ue(1);        // chroma_format_idc 4:2:0
    b.ue(0); b.ue(0); b.u(0, 1); b.u(0, 1);
    b.ue(0);        // log2_max_frame_num_minus4
    b.ue(0); b.ue(2);   // poc type 0, log2 max poc lsb
    b.ue(4); b.u(0, 1);
    const int wmb = (w + 15) / 16, hmb = (h + 15) / (frameOnly ? 16 : 32);
    b.ue(wmb - 1); b.ue(hmb - 1);
    b.u(frameOnly, 1);
    if (!frameOnly) b.u(0, 1);
    b.u(1, 1);
    const int padX = wmb * 16 - w, padY = hmb * (frameOnly ? 16 : 32) - h;
    if (padX || padY) { b.u(1, 1); b.ue(padX / 2); b.ue(0); b.ue(padY / (frameOnly ? 2 : 4)); b.ue(0); } else b.u(0, 1);
    b.u(0, 1);      // vui
    b.u(1, 1);      // stop bit
    std::vector<uint8_t> o{0x67};
    for (uint8_t x : b.escaped()) o.push_back(x);
    return o;
}

static std::vector<uint8_t> hevcSps(int w, int h, int cropBottom) {
    BitW b;
    b.u(0, 4); b.u(0, 3); b.u(1, 1);
    for (int i = 0; i < 12; i++) b.u(0x01, 8);   // 96 bits of profile, tier and level
    b.ue(0);        // sps id
    b.ue(1);        // 4:2:0
    b.ue(w); b.ue(h);
    if (cropBottom) { b.u(1, 1); b.ue(0); b.ue(0); b.ue(0); b.ue(cropBottom); } else b.u(0, 1);
    b.u(1, 1);
    std::vector<uint8_t> o{0x42, 0x01};
    for (uint8_t x : b.escaped()) o.push_back(x);
    return o;
}

static Service mk(double f, int sid, const char* name, int lcn, int type, const char* codec = "H.264") {
    Service s; s.freqMhz = f; s.onid = 9018; s.tsid = (int)f; s.sid = sid; s.name = name; s.lcn = lcn; s.serviceType = type; s.videoCodec = codec; s.provider = "BBC";
    return s;
}

int main() {
    // ---- name normaliser
    CHECK(normalizeName("BBC ONE South").key == "bbcone", "region stripped");
    CHECK(normalizeName("BBC ONE HD").key == "bbcone" && normalizeName("BBC ONE HD").marker == kHD, "HD suffix");
    CHECK(normalizeName("bbc  one   hd").key == "bbcone", "case and spaces");
    CHECK(normalizeName("RTL HD+").marker == kHD && normalizeName("RTL HD+").key == "rtl", "HD+");
    CHECK(normalizeName("Das Erste UHD").marker == kUHD, "UHD");
    CHECK(normalizeName("TF1HD").key == "tf1" && normalizeName("TF1HD").marker == kHD, "attached HD");
    CHECK(normalizeName("ITV2 +1").timeshift == 1 && normalizeName("ITV2 +1").key == "itv2", "+1");
    CHECK(normalizeName("Sky Cinema +25").timeshift == 25, "+25");
    CHECK(normalizeName("ITV2+1").timeshift == 1 && normalizeName("ITV2+1").key == "itv2", "attached +1");
    CHECK(normalizeName("Channel 4").key == "channel4" && normalizeName("Channel 4").marker == kUnknown, "plain");
    CHECK(normalizeName("North").key == "north", "a name that is only a region stays");
    CHECK(normalizeName("Ahd Radio").marker == kUnknown || normalizeName("Ahd Radio").key.size() > 0, "no crash on odd names");
    CHECK(normalizeName("").key.empty(), "empty");

    // ---- LCN rule
    CHECK(lcnPairs(1, 101) && lcnPairs(7, 107) && lcnPairs(5, 1005), "LCN + 100 / + 1000");
    CHECK(!lcnPairs(1, 102) && !lcnPairs(0, 100) && !lcnPairs(101, 1), "LCN not paired");

    // ---- classification
    CHECK(tierFromResolution(720, 576) == kSD && tierFromResolution(1280, 720) == kHD && tierFromResolution(1920, 1080) == kHD, "HD from resolution");
    CHECK(tierFromResolution(3840, 2160) == kUHD && tierFromResolution(0, 0) == kUnknown && tierFromResolution(544, 576) == kSD, "UHD and unknown");
    CHECK(tierFromServiceType(0x19) == kHD && tierFromServiceType(0x16) == kSD && tierFromServiceType(0x01) == kSD && tierFromServiceType(0x02) == kUnknown, "service type");
    CHECK(tierFromStreamType(0x02) == kSD && tierFromStreamType(0x24) == kHD && tierFromStreamType(0x1B) == kUnknown, "stream type");
    {
        Service s = mk(474, 1, "BBC ONE", 1, 0x01, "MPEG-2");
        CHECK(s.tier() == kSD, "MPEG-2 576i is SD");
        s.streamType = 0x02; s.serviceType = 0;
        CHECK(s.tier() == kSD, "stream type SD");
        s.serviceType = 0; s.streamType = 0; s.videoCodec = "HEVC";
        CHECK(s.tier() == kHD, "HEVC hints HD");
        s.width = 720; s.height = 576;
        CHECK(s.tier() == kSD, "the seen size wins over the codec");
        Service t = mk(474, 2, "BBC ONE HD", 101, 0x01);
        CHECK(t.tier() == kHD, "name marker beats a mis-signalled service type");
    }
    {   // SPS
        int w = 0, h = 0;
        auto a = h264Sps(1920, 1080, true);
        CHECK(spsResolution(false, a.data(), a.size(), w, h) && w == 1920 && h == 1080, "H.264 1080p got %dx%d", w, h);
        auto b = h264Sps(720, 576, false);
        CHECK(spsResolution(false, b.data(), b.size(), w, h) && w == 720 && h == 576, "H.264 576i got %dx%d", w, h);
        CHECK(tierFromResolution(w, h) == kSD, "576i SD");
        auto c = h264Sps(1280, 720, true);
        CHECK(spsResolution(false, c.data(), c.size(), w, h) && w == 1280 && h == 720 && tierFromResolution(w, h) == kHD, "H.264 720p got %dx%d", w, h);
        auto d = hevcSps(3840, 2160, 0);
        CHECK(spsResolution(true, d.data(), d.size(), w, h) && w == 3840 && h == 2160 && tierFromResolution(w, h) == kUHD, "HEVC 2160p got %dx%d", w, h);
        auto e = hevcSps(1920, 1088, 4);
        CHECK(spsResolution(true, e.data(), e.size(), w, h) && w == 1920 && h == 1080, "HEVC cropped got %dx%d", w, h);
        uint8_t junk[3] = {0x67, 0x64, 0};
        CHECK(!spsResolution(false, junk, 3, w, h) && !spsResolution(true, junk, 3, w, h) && !spsResolution(false, nullptr, 0, w, h), "short data rejected");
        uint8_t notSps[8] = {0x68, 1, 2, 3, 4, 5, 6, 7};
        CHECK(!spsResolution(false, notSps, 8, w, h), "not an SPS");
    }

    // ---- pairing, one multiplex
    std::vector<Service> v;
    v.push_back(mk(474, 6301, "BBC ONE South", 1, 0x16));          // 0
    v.push_back(mk(474, 6302, "BBC TWO", 2, 0x16));                // 1
    v.push_back(mk(474, 6401, "BBC ONE HD", 101, 0x19));           // 2
    v.push_back(mk(474, 6402, "BBC TWO HD", 102, 0x19));           // 3
    v.push_back(mk(474, 6403, "BBC THREE HD", 107, 0x19));         // 4: no SD in the list
    v.push_back(mk(474, 6310, "ITV2 +1", 22, 0x16));               // 5
    v.push_back(mk(474, 6410, "ITV2 HD", 122, 0x19));              // 6
    {
        Pairing p = findHd(v, 0);
        CHECK(p.index == 2, "BBC ONE South pairs with BBC ONE HD, got %d", p.index);
        CHECK(findHd(v, 1).index == 3, "BBC TWO pairs with BBC TWO HD");
        CHECK(findHd(v, 4).index == -1, "an HD service has no better counterpart");
        CHECK(findHd(v, 5).index == -1, "ITV2 +1 is not ITV2 HD");
        CHECK(findSd(v, 2).index == 0, "BBC ONE HD goes back to BBC ONE South, got %d", findSd(v, 2).index);
        CHECK(findSd(v, 4).index == -1, "BBC THREE HD has no SD version");
        CHECK(findSd(v, 0).index == -1, "an SD service has no SD counterpart");
    }
    {   // LCN only (names differ), same multiplex and provider
        std::vector<Service> w{mk(474, 1, "Alpha", 10, 0x16), mk(474, 2, "Studio Beta", 110, 0x19)};
        Pairing p = findHd(w, 0);
        CHECK(p.index == 1 && p.reason.find("LCN") != std::string::npos, "LCN rule in the same multiplex");
        w[1].freqMhz = 490;   // other multiplex: not enough
        CHECK(findHd(w, 0).index == -1, "LCN alone does not pair across multiplexes");
        w[1].provider = "other";
        w[1].freqMhz = 474;
        CHECK(findHd(w, 0).index == -1, "LCN alone needs the same provider");
    }
    {   // linkage
        std::vector<Service> w{mk(474, 1, "Kanal Eins", 1, 0x16), mk(490, 77, "K1 Hochauflösend", 800, 0x19)};
        w[0].linkOnid = 9018; w[0].linkTsid = 490; w[0].linkSid = 77;
        Pairing p = findHd(w, 0);
        CHECK(p.index == 1 && p.reason == "linkage", "linkage descriptor pairs across multiplexes");
        w[0].linkTsid = 12;   // names another transport stream
        CHECK(findHd(w, 0).index == -1, "linkage to another transport stream is not this service");
    }
    {   // across multiplexes, prefer the same one
        std::vector<Service> w{mk(474, 1, "Das Erste", 1, 0x16), mk(498, 5, "Das Erste HD", 101, 0x19), mk(474, 9, "Das Erste HD", 101, 0x19)};
        CHECK(findHd(w, 0).index == 2, "same multiplex first");
        w.erase(w.begin() + 2);
        CHECK(findHd(w, 0).index == 1, "another multiplex of the scan");
    }
    {   // user table
        std::vector<Service> w{mk(474, 1, "Kanal 1", 1, 0x16), mk(474, 2, "Kanal 1 HD", 101, 0x19), mk(474, 3, "Sonder HD", 300, 0x19)};
        CHECK(findHd(w, 0).index == 1, "automatic");
        CHECK(findHd(w, 0, {{"Kanal 1", "Sonder HD"}}).index == 2, "table overrides");
        CHECK(findHd(w, 0, {{"Kanal 1", "-"}}).index == -1, "table can forbid a pair");
        CHECK(findHd(w, 0, {{"kanal  1", "sonder hd"}}).index == 2, "table compares like the normaliser");
        CHECK(findSd(w, 2, {{"Kanal 1", "Sonder HD"}}).index == 0, "table works backwards");
    }

    // ---- catalogue round trip
    {
        Catalog c;
        c.update(474, {v[0], v[2]});
        c.update(490, {mk(490, 5, "Tab\tName", 3, 0x16)});
        c.setResolution(474, 6401, 1920, 1080);
        CHECK(c.all().size() == 3, "three services");
        c.update(474, {});   // an empty list keeps what is known
        CHECK(c.all().size() == 3, "empty update keeps");
        Catalog d; d.decode(c.encode());
        CHECK(d.all().size() == 3 && d.find(474, 6401) >= 0 && d.all()[d.find(474, 6401)].width == 1920, "round trip keeps the size");
        CHECK(d.all()[d.find(490, 5)].name == "Tab Name", "tabs are cleaned");
        c.update(474, {mk(474, 6401, "BBC ONE HD", 101, 0x19)});   // size seen earlier survives a rescan
        CHECK(c.all()[c.find(474, 6401)].width == 1920 && c.find(474, 6301) < 0, "rescan replaces the multiplex, keeps seen sizes");
        auto o = decodeOverrides(encodeOverrides({{"A", "B"}, {"C", "-"}}));
        CHECK(o.size() == 2 && o[1].hd == "-", "overrides round trip");
    }
    return testSummary("tv_hd_pair");
}
