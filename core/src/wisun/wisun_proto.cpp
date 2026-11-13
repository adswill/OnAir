// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN / IEEE 802.15.4g SUN FSK frame layers (see wisun_proto.h).
#include "dect2/wisun/wisun_proto.h"
#include "dect2/zigbee/zigbee_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "dect2/common/crc.h"
#include "dect2/common/endian.h"

namespace dect2 {
namespace wisun {

// ---------------------------------------------------------------- PHY

void addFcs(std::vector<uint8_t>& m, bool fcs2) {
    if (fcs2) {
        const uint16_t c = crc16(m.data(), m.size());
        m.push_back((uint8_t)c); m.push_back((uint8_t)(c >> 8));
    } else {
        const uint32_t c = crc32(m.data(), m.size());
        for (int i = 0; i < 4; i++) m.push_back((uint8_t)(c >> (8 * i)));
    }
}

// ---------------------------------------------------------------- band and channel plans

const std::vector<BandPlan>& bandPlans() {
    // The Wi-SUN frequency band table: band, region, regulatory domain value (channel spacings and counts in channelPlans())
    static const std::vector<BandPlan> v = {
        {"ANZ", "Australia / New Zealand", 0x08, 915, 928, 0, 0},
        {"NA", "North America", 0x01, 902, 928, 0, 0},
        {"MX", "Mexico", 0x06, 902, 928, 0, 0},
        {"BZ", "Brazil", 0x07, 902, 928, 907.5, 915},
        {"JP", "Japan", 0x02, 920, 928, 0, 0},
        {"KR", "Korea", 0x09, 917, 923.5, 0, 0},
        {"PH", "Philippines", 0x0A, 915, 918, 0, 0},
        {"MY", "Malaysia", 0x0B, 919, 923, 0, 0},
        {"HK", "Hong Kong", 0x0C, 920, 925, 0, 0},
        {"SG", "Singapore (920 MHz)", 0x0D, 920, 925, 0, 0},
        {"SG866", "Singapore (866 MHz)", 0x10, 866, 869, 0, 0},
        {"TH", "Thailand", 0x0E, 920, 925, 0, 0},
        {"VN", "Vietnam", 0x0F, 920, 925, 0, 0},
        {"EU863", "Europe (863 MHz)", 0x03, 863, 870, 0, 0},
        {"EU870", "Europe (870 MHz)", 0x03, 870, 876, 0, 0},
        {"IN", "India", 0x05, 865, 867, 0, 0},
        {"CN470", "China (470 MHz)", 0x04, 470, 510, 0, 0},
        {"CN779", "China (779 MHz)", 0x04, 779, 787, 0, 0},
        {"CN920", "China (920 MHz)", 0x04, 920.5, 924.5, 0, 0},
        {"WW", "Worldwide 2.4 GHz", 0x00, 2400, 2483.5, 0, 0},
    };
    return v;
}

const BandPlan* bandPlan(const std::string& id) {
    for (const BandPlan& b : bandPlans()) if (id == b.id) return &b;
    return nullptr;
}

const BandPlan& bandPlanFor(double mhz) {
    const char* id = "ANZ";
    if (mhz >= 900 && mhz < 914.5) id = "NA";
    else if (mhz >= 862 && mhz < 870) id = "EU863";
    else if (mhz >= 870 && mhz < 877) id = "EU870";
    else if (mhz >= 469 && mhz < 511) id = "CN470";
    else if (mhz >= 778 && mhz < 788) id = "CN779";
    else if (mhz >= 2399 && mhz < 2485) id = "WW";
    return *bandPlan(id);
}

const std::vector<ChannelPlan>& channelPlans() {
    // Channel counts and spacings from the band table; ch0 and the FSK modes as wisun_proto.h says
    constexpr PhyRate r50h1{50000, 1.0}, r50{50000, 0.5}, r100{100000, 0.5}, r150{150000, 0.5}, r200{200000, 0.5}, r300{300000, 0.5}, none{};
    static const std::vector<ChannelPlan> v = {
        {"ANZ", 1, 915.2, 200, 64, {r50h1, none}},
        {"ANZ", 2, 915.4, 400, 32, {r100, r150}},
        {"NA", 1, 902.2, 200, 129, {r50h1, none}},
        {"NA", 2, 902.4, 400, 64, {r100, r150}},
        {"NA", 3, 902.6, 600, 42, {r200, r300}},
        {"MX", 1, 902.2, 200, 129, {r50h1, none}},
        {"MX", 2, 902.4, 400, 64, {r100, r150}},
        {"BZ", 1, 902.2, 200, 129, {r50h1, none}},
        {"BZ", 2, 902.4, 400, 64, {r100, r150}},
        {"BZ", 3, 902.6, 600, 42, {r200, r300}},
        {"JP", 1, 920.6, 200, 38, {r50h1, none}},
        {"JP", 2, 920.9, 400, 18, {r100, r150}},
        {"JP", 3, 920.8, 600, 12, {r200, r300}},
        {"KR", 1, 917.1, 200, 32, {r50h1, none}},
        {"KR", 2, 917.3, 400, 16, {r100, r150}},
        {"PH", 1, 915.2, 200, 14, {r50h1, none}},
        {"PH", 2, 915.4, 400, 7, {r100, r150}},
        {"MY", 1, 919.2, 200, 19, {r50h1, none}},
        {"MY", 2, 919.4, 400, 10, {r100, r150}},
        {"HK", 1, 920.2, 200, 24, {r50h1, none}},
        {"HK", 2, 920.4, 400, 12, {r100, r150}},
        {"SG", 1, 920.2, 200, 24, {r50h1, none}},
        {"SG", 2, 920.4, 400, 12, {r100, r150}},
        {"SG866", 1, 866.1, 100, 29, {r50, none}},
        {"SG866", 2, 866.1, 200, 15, {r100, none}},
        {"SG866", 3, 866.3, 400, 7, {r150, none}},
        {"TH", 1, 920.2, 200, 24, {r50h1, none}},
        {"TH", 2, 920.4, 400, 12, {r100, r150}},
        {"VN", 1, 920.2, 200, 24, {r50h1, none}},
        {"VN", 2, 920.4, 400, 12, {r100, r150}},
        {"EU863", 1, 863.1, 100, 69, {r50, none}},
        {"EU863", 2, 863.1, 200, 35, {r100, none}},
        {"EU870", 1, 870.1, 100, 59, {r50, none}},
        {"EU870", 2, 870.2, 200, 30, {r100, none}},
        {"IN", 1, 865.1, 100, 19, {r50, none}},
        {"IN", 2, 865.1, 200, 10, {r100, none}},
        {"CN470", 1, 470.2, 200, 199, {r50h1, none}},
        {"CN779", 1, 779.2, 200, 39, {r50h1, none}},
        {"CN779", 2, 779.4, 400, 19, {r100, r150}},
        {"CN920", 1, 920.625, 250, 16, {r50h1, r100}},
        {"WW", 1, 2400.2, 200, 416, {r50h1, none}},
        {"WW", 2, 2400.4, 400, 207, {r100, r150}},
    };
    return v;
}

std::vector<ChannelPlan> plansOf(const std::string& region) {
    std::vector<ChannelPlan> out;
    for (const ChannelPlan& p : channelPlans()) if (region == p.region) out.push_back(p);
    return out;
}

bool channelUsable(const ChannelPlan& p, int ch) {
    if (ch < 0 || ch >= p.channels) return false;
    const BandPlan* b = bandPlan(p.region);
    if (!b || b->gapHiMhz <= b->gapLoMhz) return true;
    const double f = channelMhz(p, ch), h = p.spacingKhz / 2000.0;
    return f + h <= b->gapLoMhz || f - h >= b->gapHiMhz;   // wholly outside the gap
}

int channelOf(const ChannelPlan& p, double mhz) {
    const double k = (mhz - p.ch0Mhz) * 1000.0 / p.spacingKhz;
    const int ch = (int)(k + (k >= 0 ? 0.5 : -0.5));
    if (!channelUsable(p, ch)) return -1;
    return std::abs(k - ch) <= 0.5 ? ch : -1;
}


// ---------------------------------------------------------------- names

const char* frameTypeName(int t) { return zigbee::frameTypeName(t); }


std::string euiText(uint64_t e) { return zigbee::addrText(e); }

std::string srcText(const Frame& f) {
    if (f.srcMode == 3) return euiText(f.srcExt);
    if (f.srcMode == 2) return zigbee::shortText(f.srcShort);
    return "-";
}
std::string dstText(const Frame& f) {
    if (f.destMode == 3) return euiText(f.destExt);
    if (f.destMode == 2) return f.destShort == 0xFFFF ? "broadcast" : zigbee::shortText(f.destShort);
    return f.type == Data || f.type == Ack ? "broadcast" : "-";
}

// ---------------------------------------------------------------- parsing


// ---------------------------------------------------------------- building

namespace {
void put16(std::vector<uint8_t>& v, unsigned x) { dect2::endian::putLe16(v, x); }
void put24(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 3; i++) v.push_back((uint8_t)(x >> (8 * i))); }
void put32(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putLe32(v, x); }
void put64(std::vector<uint8_t>& v, uint64_t x) { dect2::endian::putLe64(v, x); }

void headerIe(std::vector<uint8_t>& v, int id, const std::vector<uint8_t>& body) {
    put16(v, (unsigned)(body.size() & 0x7F) | (unsigned)id << 7);
    v.insert(v.end(), body.begin(), body.end());
}
void wh(std::vector<uint8_t>& v, int sub, std::vector<uint8_t> body) {
    body.insert(body.begin(), (uint8_t)sub);
    headerIe(v, 0x2A, body);
}
void channels(std::vector<uint8_t>& v, const Schedule& s) {
    v.push_back((uint8_t)((s.chPlan & 7) | (s.chFunction & 7) << 3));
    if (s.chPlan == 0) { v.push_back((uint8_t)s.regDomain); v.push_back((uint8_t)s.opClass); }
    else if (s.chPlan == 1) { put24(v, (uint32_t)(s.ch0Mhz * 1000 + 0.5)); v.push_back((uint8_t)s.spacingCode); put16(v, (unsigned)s.numChannels); }
    else { v.push_back((uint8_t)s.regDomain); v.push_back((uint8_t)s.chPlanId); }
    if (s.chFunction == 0) put16(v, (unsigned)s.fixedChannel);
}
void subShort(std::vector<uint8_t>& v, int sub, const std::vector<uint8_t>& body) {
    put16(v, (unsigned)(body.size() & 0xFF) | (unsigned)sub << 8);
    v.insert(v.end(), body.begin(), body.end());
}
void subLong(std::vector<uint8_t>& v, int sub, const std::vector<uint8_t>& body) {
    put16(v, (unsigned)(body.size() & 0x7FF) | (unsigned)sub << 11 | 0x8000u);
    v.insert(v.end(), body.begin(), body.end());
}
void payloadIe(std::vector<uint8_t>& v, int group, const std::vector<uint8_t>& body) {
    put16(v, (unsigned)(body.size() & 0x7FF) | (unsigned)group << 11 | 0x8000u);
    v.insert(v.end(), body.begin(), body.end());
}
}

std::vector<uint8_t> buildMpdu(const Build& b) {
    std::vector<uint8_t> v;
    const int dm = b.dst ? 3 : 0, sm = b.src ? 3 : 0;
    const bool seqSup = b.seq < 0;
    // PAN IDs: with a destination (both extended) none is sent (PAN ID compression); a frame without one carries the source PAN when asked
    const bool panComp = dm != 0 ? true : !b.srcPanPresent;
    const unsigned fc = Data | (b.secured ? 1u << 3 : 0u) | (b.ackReq ? 1u << 5 : 0u) | (panComp ? 1u << 6 : 0u) | (seqSup ? 1u << 8 : 0u) | 1u << 9 |
                        (unsigned)dm << 10 | 2u << 12 | (unsigned)sm << 14;
    put16(v, fc);
    if (!seqSup) v.push_back((uint8_t)b.seq);
    if (dm == 0 && sm == 3 && b.srcPanPresent) put16(v, b.pan);
    if (dm == 3) put64(v, b.dst);
    if (sm == 3) put64(v, b.src);
    if (b.secured) {
        v.push_back(0x0E);   // ENC-MIC-64, key identifier mode 1
        put32(v, b.frameCounter);
        v.push_back((uint8_t)b.keyIndex);
    }
    {
        std::vector<uint8_t> u = {(uint8_t)(b.utt & 0x0F)};
        put24(u, b.ufsi);
        wh(v, 0x01, u);
    }
    if (b.bt) { std::vector<uint8_t> u; put16(u, (unsigned)b.bsn); put24(u, b.bio); wh(v, 0x02, u); }
    if (b.rslDbm > -999) wh(v, 0x04, {(uint8_t)(b.rslDbm + 174)});
    if (b.ea) { std::vector<uint8_t> u; put64(u, b.ea); wh(v, 0x09, u); }
    if (b.lbc) { std::vector<uint8_t> u; put24(u, b.lfnBcastMs); u.push_back(5); wh(v, 0x80, u); }
    std::vector<uint8_t> pies;
    if (!b.secured) {
        std::vector<uint8_t> wp;
        if (b.us.present) {
            std::vector<uint8_t> u = {(uint8_t)b.us.dwellMs, (uint8_t)b.us.clockDrift, (uint8_t)b.us.timingAccuracy};
            channels(u, b.us);
            subLong(wp, 1, u);
        }
        if (b.bs.present) {
            std::vector<uint8_t> u;
            put32(u, b.bs.bcastIntervalMs); put16(u, (unsigned)b.bs.bsi);
            u.push_back((uint8_t)b.bs.dwellMs); u.push_back((uint8_t)b.bs.clockDrift); u.push_back((uint8_t)b.bs.timingAccuracy);
            channels(u, b.bs);
            subLong(wp, 2, u);
        }
        if (b.panIe) {
            std::vector<uint8_t> u;
            put16(u, (unsigned)b.panSize); put16(u, (unsigned)b.routingCost);
            u.push_back((uint8_t)((b.useParentBs ? 1 : 0) | 2 | (b.tpsVersion & 7) << 5));
            subShort(wp, 0x04, u);
        }
        if (!b.netName.empty()) subShort(wp, 0x05, std::vector<uint8_t>(b.netName.begin(), b.netName.end()));
        if (b.panVersion >= 0) { std::vector<uint8_t> u; put16(u, (unsigned)b.panVersion); subShort(wp, 0x06, u); }
        if (b.gtkHash) subShort(wp, 0x07, std::vector<uint8_t>(32, 0x5A));
        if (!b.phyModes.empty()) {
            std::vector<uint8_t> u = {(uint8_t)b.phyModes.size()};
            for (int m : b.phyModes) u.push_back((uint8_t)m);
            subShort(wp, 0x08, u);
        }
        if (!wp.empty()) payloadIe(pies, 0x4, wp);
        if (b.mpxId >= 0) {
            std::vector<uint8_t> u = {0x00};   // full frame
            put16(u, (unsigned)b.mpxId);
            if (b.kmpId >= 0) u.push_back((uint8_t)b.kmpId);
            u.insert(u.end(), b.payload.begin(), b.payload.end());
            payloadIe(pies, 0x3, u);
        }
    }
    if (!pies.empty() || b.secured) {
        headerIe(v, 0x7E, {});
        v.insert(v.end(), pies.begin(), pies.end());
    } else {
        headerIe(v, 0x7F, {});
    }
    if (b.secured || b.mpxId < 0) v.insert(v.end(), b.payload.begin(), b.payload.end());
    return v;
}

} // namespace wisun
} // namespace dect2
