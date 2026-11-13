// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) MAC frames and channel plans: every beacon form (S1G beacon with and without elements, short beacon, management beacon, probe
// request and response, data in all To DS / From DS directions), the elements a monitor lists (SSID, RSN, S1G Operation with every width, S1G
// Capabilities, TIM, Country, power, rates, RAW, TWT), the FCS, garbage input, and the channel tables of every region (counts, edges, numbering, classes).
#include "dect2/halow/halow_proto.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::halow;
#include "../common/test_util.h"

int main() {
    const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0x01, 0x02, 0x03}, sta[6] = {0xB8, 0x27, 0xEB, 0x0A, 0x0B, 0x0C};
    BssSpec b;
    std::copy(mac, mac + 6, b.bssid);
    b.ssid = "Sensors-5"; b.security = Wpa2Psk; b.timestamp = 99999; b.intervalTu = 300; b.changeSeq = 7;
    b.country = "US"; b.tim = true; b.dtimPeriod = 3; b.raw = true; b.twt = true; b.txPowerDbm = 24; b.longPreamble = true; b.maxMcsOneSs = 2;
    // an S1G beacon with every element, at every operating width
    for (int mhz : {1, 2, 4, 8, 16}) {
        b.opWidthMhz = mhz; b.primaryWidthMhz = mhz == 1 ? 1 : 2; b.bwInd = mhz == 1 ? 0 : mhz == 2 ? 1 : mhz == 4 ? 2 : mhz == 8 ? 3 : 4;
        b.opClass = mhz == 1 ? 1 : mhz == 2 ? 2 : mhz == 4 ? 3 : mhz == 8 ? 4 : 5; b.ccf = 16; b.primary = 15;
        const auto f = makeS1gBeacon(b);
        CHECK(fcsOk(f.data(), f.size()), "FCS");
        Frame r;
        CHECK(parseFrame(f.data(), f.size() - 4, r) && r.s1gBeacon && !r.shortBeacon, "parse %d MHz", mhz);
        CHECK(r.hasSsid && r.ssid == "Sensors-5" && r.hasComp && r.timestamp == 99999 && r.intervalTu == 300 && r.bwInd == b.bwInd, "fields %d MHz", mhz);
        CHECK(r.hasOp && r.opWidthMhz == mhz && r.primaryWidthMhz == (mhz == 1 ? 1 : 2) && r.ccf == 16 && r.primary == 15 && r.opClass == b.opClass && r.basicMcsOneSs == 2, "S1G Operation at %d MHz: %d / %d", mhz, r.opWidthMhz, r.primaryWidthMhz);
        CHECK(r.hasCountry && r.country == "US" && r.hasTim && r.dtimPeriod == 3 && r.hasRaw && r.hasTwt && r.txPowerDbm == 24 && r.hasCap && r.longPreamble, "optional elements");
        CHECK(chanMhz(r.opClass, r.ccf) == 902.0 + 0.5 * 16, "operating class %d channel %d is %.1f MHz", r.opClass, r.ccf, chanMhz(r.opClass, r.ccf));
    }
    {   // independent constants: the CRC-32 check value of the FCS polynomial, and the frame control octets of the standard (version 0; type, subtype)
        CHECK(wifi::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, "CRC-32 check value");
        CHECK(makeS1gBeacon(b)[0] == 0x1C && makeMgmtBeacon(b)[0] == 0x80 && makeProbeResponse(b, sta)[0] == 0x50 && makeProbeRequest(sta, "x")[0] == 0x40 && makeDataFrame(mac, sta, 1)[0] == 0x08,
              "frame control octets: S1G beacon (extension, subtype 1) 0x1C, beacon 0x80, probe response 0x50, probe request 0x40, data 0x08");
        const auto sb = makeS1gBeacon(b);
        CHECK(sb.size() > 15 && std::equal(mac, mac + 6, sb.begin() + 4), "S1G beacon: the source address follows frame control and duration");
        const auto mb = makeMgmtBeacon(b);
        CHECK(std::equal(mac, mac + 6, mb.begin() + 10) && std::equal(mac, mac + 6, mb.begin() + 16) && mb[4] == 0xFF && mb[9] == 0xFF, "beacon: broadcast DA, SA and BSSID");
    }
    {   // vendor specific elements: the OUI is listed (Morse Micro systems add one), none when absent
        Frame r0; { const auto f = makeS1gBeacon(b); parseFrame(f.data(), f.size() - 4, r0); }
        CHECK(r0.vendorIes.empty(), "no vendor element when none is sent");
        BssSpec v = b; v.vendorOui[0] = 0x0C; v.vendorOui[1] = 0xBF; v.vendorOui[2] = 0x74;
        for (int k = 0; k < 2; k++) {
            const auto f = k ? makeMgmtBeacon(v) : makeS1gBeacon(v);
            Frame r;
            CHECK(fcsOk(f.data(), f.size()) && parseFrame(f.data(), f.size() - 4, r) && r.vendorIes.size() == 1 && r.vendorIes[0] == "0C:BF:74" && r.hasSsid, "vendor element 0C:BF:74 (%s)", k ? "management beacon" : "S1G beacon");
        }
    }
    // the basic MCS map and the long preamble flag
    for (int m = 0; m < 3; m++) { b.maxMcsOneSs = m; b.longPreamble = m != 1; const auto f = makeS1gBeacon(b); Frame r; parseFrame(f.data(), f.size() - 4, r); CHECK(r.basicMcsOneSs == m && r.longPreamble == (m != 1), "MCS map %d", m); }
    {   // short beacon: only the fixed fields and the compressed SSID
        const auto f = makeShortBeacon(b);
        Frame r;
        CHECK(fcsOk(f.data(), f.size()) && parseFrame(f.data(), f.size() - 4, r) && r.shortBeacon && r.s1gBeacon && !r.hasSsid && r.hasComp && r.summary == "S1G short beacon", "short beacon %s", r.summary.c_str());
        CHECK(r.compSsid == wifi::crc32(reinterpret_cast<const uint8_t*>("Sensors-5"), 9) && macText(r.bssid) == "24:0A:C4:01:02:03", "short beacon fields");
        CHECK(f.size() < 30, "short beacon is %zu bytes", f.size());
    }
    {   // the management beacon and the probe request / response
        b.rates = true;
        const auto mb = makeMgmtBeacon(b);
        Frame r;
        CHECK(fcsOk(mb.data(), mb.size()) && parseFrame(mb.data(), mb.size() - 4, r) && r.mgmtBeacon && r.ssid == "Sensors-5" && r.ratesMbps.size() == 8 && std::fabs(r.ratesMbps[0] - 6.f) < 0.01f && r.hasCountry, "management beacon");
        const auto pr = makeProbeResponse(b, sta);
        CHECK(parseFrame(pr.data(), pr.size() - 4, r) && r.probeResp && r.ssid == "Sensors-5" && macText(r.da) == "B8:27:EB:0A:0B:0C" && macText(r.sa) == macText(r.bssid), "probe response");
        const auto rq = makeProbeRequest(sta, "Sensors-5");
        CHECK(fcsOk(rq.data(), rq.size()) && parseFrame(rq.data(), rq.size() - 4, r) && r.probeReq && r.hasSsid && r.ssid == "Sensors-5" && macText(r.sa) == "B8:27:EB:0A:0B:0C" && r.summary == "probe req Sensors-5", "probe request %s", r.summary.c_str());
        const auto wq = makeProbeRequest(sta, "");
        CHECK(parseFrame(wq.data(), wq.size() - 4, r) && r.probeReq && r.summary == "probe req (any)", "wildcard probe request");
    }
    {   // data frames in the four address modes, QoS, protected
        std::vector<uint8_t> d = makeDataFrame(mac, sta, 20);
        Frame r;
        CHECK(parseFrame(d.data(), d.size() - 4, r) && r.type == 2 && r.hasAddrs && macText(r.bssid) == "24:0A:C4:01:02:03" && macText(r.sa) == "B8:27:EB:0A:0B:0C" && !r.s1gBeacon, "to DS data");
        d[1] = 0x02;   // from DS: addr1 DA, addr2 BSSID, addr3 SA
        parseFrame(d.data(), d.size() - 4, r);
        CHECK(macText(r.da) == "24:0A:C4:01:02:03" && macText(r.bssid) == "B8:27:EB:0A:0B:0C", "from DS data");
        d[1] = 0x00;
        parseFrame(d.data(), d.size() - 4, r);
        CHECK(macText(r.da) == "24:0A:C4:01:02:03" && macText(r.sa) == "B8:27:EB:0A:0B:0C" && macText(r.bssid) == macText(r.da), "ad hoc data");
        d[0] = 0x88; d[1] = 0x41;   // QoS data, to DS, protected
        parseFrame(d.data(), d.size() - 4, r);
        CHECK(r.qos && r.protectedFrame && r.summary == "QoS data (protected)", "QoS data: %s", r.summary.c_str());
    }
    {   // control and management names, disassociation reason
        std::vector<uint8_t> a(30, 0);
        a[0] = 0xA0;   // management, subtype 10 (disassociation)
        a[24] = 3;
        Frame r;
        CHECK(parseFrame(a.data(), 26, r) && r.type == 0 && r.summary == "disassoc, reason 3", "disassoc: %s", r.summary.c_str());
        a[0] = 0xD4;   // control, subtype 13: ACK
        CHECK(parseFrame(a.data(), 10, r) && r.type == 1 && r.summary == "ACK", "ACK: %s", r.summary.c_str());
        a[0] = 0xB0;   // authentication
        CHECK(parseFrame(a.data(), 26, r) && r.summary == "auth", "auth");
    }
    {   // security of every kind, and garbage never crashes
        for (int sec : {Open, Wpa2Psk, Wpa3Sae, Owe, Wpa2Ent}) { BssSpec c = b; c.security = sec; const auto g = makeS1gBeacon(c); Frame q; parseFrame(g.data(), g.size() - 4, q); CHECK(q.security == sec, "security %d", sec); }
        std::mt19937 rng(31);
        for (int i = 0; i < 4000; i++) {
            std::vector<uint8_t> g((size_t)(rng() % 150));
            for (auto& v : g) v = (uint8_t)rng();
            if (!g.empty() && i % 3 == 0) g[0] = (uint8_t)(i % 2 ? 0x1C : 0x80);
            Frame z;
            parseFrame(g.data(), g.size(), z);
        }
        // a truncated element list
        auto f = makeS1gBeacon(b);
        f.resize(f.size() - 20);
        Frame z;
        parseFrame(f.data(), f.size(), z);
        CHECK(true, "truncated");
    }
    // channel plans of every region: counts of the non-overlapping channels per width, edges, numbering, classes
    {
        struct E { int region, w, count; double first, last; };
        const E want[] = {
            {RegUS, 1, 26, 902.5, 927.5}, {RegUS, 2, 13, 903.0, 927.0}, {RegUS, 4, 6, 906.0, 926.0}, {RegUS, 8, 3, 908.0, 924.0}, {RegUS, 16, 1, 912.0, 912.0},
            {RegEU, 1, 5, 863.5, 867.5}, {RegEU, 2, 2, 864.0, 866.0},
            {RegJP, 1, 11, 917.0, 927.0}, {RegKR, 1, 6, 918.0, 923.0}, {RegSG, 1, 8, 866.5, 924.5}, {RegAU, 1, 13, 915.5, 927.5}, {RegAU, 4, 3, 918.0, 926.0}, {RegAU, 8, 1, 924.0, 924.0},
            {RegNZ, 2, 6, 916.0, 926.0}, {RegCN, 1, 32, 755.5, 786.5}, {RegIN, 1, 2, 865.5, 866.5}, {RegIN, 2, 1, 866.0, 866.0}, {RegTW, 1, 8, 920.5, 927.5}, {RegTW, 4, 2, 922.0, 926.0}, {RegTW, 8, 1, 924.0, 924.0},
        };
        for (const E& e : want) {
            const auto v = regionChannels(e.region, e.w);
            CHECK((int)v.size() == e.count && !v.empty() && std::fabs(v.front().mhz - e.first) < 1e-6 && std::fabs(v.back().mhz - e.last) < 1e-6, "%s %d MHz: %zu channels %.1f - %.1f", regionInfo(e.region).code, e.w, v.size(), v.empty() ? 0 : v.front().mhz, v.empty() ? 0 : v.back().mhz);
        }
        for (int r = 0; r < kNumRegions; r++) {
            const RegionInfo& ri = regionInfo(r);
            const auto tab = channelTable(r);
            CHECK(!tab.empty() && std::string(ri.code).size() == 2, "region %d", r);
            for (const ChanRow& c : tab) {
                // every channel lies inside a band, its number gives its centre, its class gives the same centre, channels of one width do not overlap
                bool inBand = false;
                for (const Band& bd : ri.bands) if (c.mhz - c.widthMhz / 2.0 >= bd.loMhz - 1e-6 && c.mhz + c.widthMhz / 2.0 <= bd.hiMhz + 1e-6) inBand = true;
                CHECK(inBand, "%s channel %d (%.1f MHz, %d MHz wide) outside its band", ri.code, c.ccf, c.mhz, c.widthMhz);
                CHECK(std::fabs(ri.startMhz + 0.5 * c.ccf - c.mhz) < 1e-9 && std::fabs(chanMhz(c.opClass, c.ccf) - c.mhz) < 1e-9 && c.opClass == operatingClass(r, c.widthMhz), "%s channel %d numbering / class", ri.code, c.ccf);
            }
            for (int w : ri.widths) {
                const auto v = regionChannels(r, w);
                for (size_t i = 1; i < v.size(); i++) CHECK(v[i].mhz - v[i - 1].mhz >= w - 1e-9, "%s %d MHz channels overlap", ri.code, w);
            }
        }
        {   // issue 192: US 4 MHz channels 8 ... 48 (the top one touches 928, none touches 902), 8 MHz 12 / 28 / 44, 16 MHz 20; 1 and 2 MHz as before
            const int w4[] = {8, 16, 24, 32, 40, 48}; const auto v4 = regionChannels(RegUS, 4);
            CHECK(v4.size() == 6, "US 4 MHz count");
            for (size_t i = 0; i < v4.size() && i < 6; i++) CHECK(v4[i].ccf == w4[i] && v4[i].mhz == 902.0 + 0.5 * w4[i], "US 4 MHz channel %d", v4[i].ccf);
            CHECK(v4.back().mhz + 2.0 == 928.0 && v4.front().mhz - 2.0 == 904.0, "US 4 MHz edges");
            const auto v8 = regionChannels(RegUS, 8), v16 = regionChannels(RegUS, 16), v1 = regionChannels(RegUS, 1), v2 = regionChannels(RegUS, 2);
            CHECK(v8.size() == 3 && v8[0].ccf == 12 && v8[1].ccf == 28 && v8[2].ccf == 44 && v16.size() == 1 && v16[0].ccf == 20, "US 8 / 16 MHz numbers");
            CHECK(v1.front().ccf == 1 && v1.back().ccf == 51 && v2.front().ccf == 2 && v2[1].ccf == 6 && v2.back().ccf == 50, "US 1 / 2 MHz numbers");
        }
        CHECK(regionOf(915.0) == RegUS && regionOf(865.0) == RegEU && regionOf(920.5) == RegUS && regionOf(760.0) == RegCN && regionOf(433.9) == -1 && regionOf(2437) == -1, "region of a frequency");
        CHECK(nearestChannel(RegUS, 915.4, 1) == 27 && nearestChannel(RegUS, 915.0, 2) == 26 && nearestChannel(RegUS, 916.4, 8) == 28 && nearestChannel(RegEU, 915.0, 1) == 0, "nearest");
        CHECK(operatingClass(RegUS, 16) == 5 && operatingClass(RegEU, 2) == 7 && operatingClass(RegEU, 4) == 0 && chanMhz(6, 1) == 863.5 && chanMhz(99, 1) == 0, "classes");
    }
    return testSummary("halow frames");
}
