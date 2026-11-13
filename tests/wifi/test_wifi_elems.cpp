// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi channel plans (wifi_chan.h) and information element parsers (wifi_elems.h) against byte strings built here from the standard's layouts
// (not from wifi_mk.cpp): IEEE 802.11-2020 9.4.2 / 9.6.7.36, 802.11ax-2021 9.4.2.248 - 9.4.2.249 / 9.4.2.170, 802.11be-2024 9.4.2.313.
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_elems.h"
#include "dect2/wifi/wifi_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2::wifi;
#include "../common/test_util.h"

static std::vector<uint8_t> el(int id, std::vector<uint8_t> b) { b.insert(b.begin(), (uint8_t)b.size()); b.insert(b.begin(), (uint8_t)id); return b; }
static std::vector<uint8_t> ex(int ext, std::vector<uint8_t> b) { b.insert(b.begin(), (uint8_t)ext); return el(255, b); }
static void cat(std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); }

int main() {
    // ---- channel plans (802.11-2020 Annex E.1: 5 GHz 5000 + 5 n; 802.11ax Annex E.4: 6 GHz 5950 + 5 n, channel 2 at 5935)
    CHECK(bandChannelMhz(Band5, 36) == 5180 && bandChannelMhz(Band5, 64) == 5320 && bandChannelMhz(Band5, 100) == 5500 && bandChannelMhz(Band5, 144) == 5720 &&
          bandChannelMhz(Band5, 149) == 5745 && bandChannelMhz(Band5, 165) == 5825 && bandChannelMhz(Band5, 177) == 5885, "5 GHz frequencies");
    CHECK(bandChannelMhz(Band5, 37) == 0 && bandChannelMhz(Band5, 68) == 0 && bandChannelMhz(Band5, 148) == 0 && bandChannelMhz(Band5, 34) == 0, "5 GHz invalid channels");
    CHECK(bandChannelMhz(Band6, 1) == 5955 && bandChannelMhz(Band6, 5) == 5975 && bandChannelMhz(Band6, 93) == 6415 && bandChannelMhz(Band6, 233) == 7115 && bandChannelMhz(Band6, 2) == 5935 &&
          bandChannelMhz(Band6, 3) == 0, "6 GHz frequencies");
    CHECK(bandChannelMhz(Band24, 1) == 2412 && bandChannelMhz(Band24, 14) == 2484 && bandChannelOf(Band5, 5500.4) == 100 && bandChannelOf(Band6, 5975) == 5, "channel of frequency");
    CHECK(bandChannelOf(Band5, 5190) == 0 && bandChannelOf(Band5, 5181.5) == 36, "channel of frequency: between channels");
    CHECK(bandOfMhz(2437) == Band24 && bandOfMhz(5180) == Band5 && bandOfMhz(5955) == Band6 && bandOfMhz(7115) == Band6 && bandOfMhz(5900) == -1 && bandOfMhz(3500) == -1, "band of frequency");
    CHECK(channelList(Band5, DomAll).size() == 28 && channelList(Band5, DomEu).size() == 19 && channelList(Band5, DomUs).size() == 28 && channelList(Band5, DomCn).size() == 8 + 5, "5 GHz list sizes %zu %zu %zu %zu",
          channelList(Band5, DomAll).size(), channelList(Band5, DomEu).size(), channelList(Band5, DomUs).size(), channelList(Band5, DomCn).size());
    CHECK(channelList(Band6, DomAll).size() == 60 && channelList(Band6, DomEu).size() == 24 && channelList(Band6, DomUs).size() == 60, "6 GHz list sizes %zu %zu", channelList(Band6, DomAll).size(), channelList(Band6, DomEu).size());
    const auto psc = pscChannels();
    CHECK(psc.size() == 15 && psc.front() == 5 && psc[1] == 21 && psc[2] == 37 && psc.back() == 229 && isPsc(133) && !isPsc(9) && isPsc(5), "PSC list");
    CHECK(isDfs(52) && isDfs(64) && isDfs(100) && isDfs(144) && !isDfs(36) && !isDfs(149) && !isDfs(48), "DFS");
    CHECK(std::string(unii(Band5, 36)) == "UNII-1" && std::string(unii(Band5, 60)) == "UNII-2A" && std::string(unii(Band5, 120)) == "UNII-2C" && std::string(unii(Band5, 149)) == "UNII-3" &&
          std::string(unii(Band5, 173)) == "UNII-4" && std::string(unii(Band6, 5)) == "UNII-5" && std::string(unii(Band6, 97)) == "UNII-6" && std::string(unii(Band6, 149)) == "UNII-7" &&
          std::string(unii(Band6, 193)) == "UNII-8", "UNII names");
    // every channel of every list is valid and sorted
    for (int b = 0; b < 3; b++) for (int d = 0; d < kDomains; d++) { int last = 0; for (int c : channelList(b, d)) { CHECK(validChannel(b, c) && c > last && bandChannelMhz(b, c) > 0, "list %d/%d ch %d", b, d, c); last = c; } }
    // wider channels: 5 GHz 80 MHz centres 42 58 106 122 138 155 171, 160 MHz 50 114 163, 40 MHz 38 46 ...; 6 GHz blocks from channel 1
    CHECK(blockCentreChannel(Band5, 36, 80) == 42 && blockCentreChannel(Band5, 48, 80) == 42 && blockCentreChannel(Band5, 52, 80) == 58 && blockCentreChannel(Band5, 100, 80) == 106 &&
          blockCentreChannel(Band5, 149, 80) == 155 && blockCentreChannel(Band5, 165, 80) == 171 && blockCentreChannel(Band5, 36, 160) == 50 && blockCentreChannel(Band5, 64, 160) == 50 &&
          blockCentreChannel(Band5, 100, 160) == 114 && blockCentreChannel(Band5, 149, 160) == 163 && blockCentreChannel(Band5, 36, 40) == 38 && blockCentreChannel(Band5, 40, 40) == 38 &&
          blockCentreChannel(Band5, 44, 40) == 46, "5 GHz centre channels");
    CHECK(blockCentreChannel(Band6, 5, 80) == 7 && blockCentreChannel(Band6, 21, 80) == 23 && blockCentreChannel(Band6, 5, 160) == 15 && blockCentreChannel(Band6, 37, 160) == 47 &&
          blockCentreChannel(Band6, 5, 40) == 3 && blockCentreChannel(Band6, 5, 320) == 31 && blockCentreChannel(Band6, 5, 20) == 5, "6 GHz centre channels");
    CHECK(operatingClassWidth(131) == 20 && operatingClassWidth(132) == 40 && operatingClassWidth(133) == 80 && operatingClassWidth(134) == 160 && operatingClassBand(131) == Band6 && operatingClassBand(115) == Band5 &&
          operatingClassBand(81) == Band24 && operatingClassBand(1) == -1, "operating classes");
    CHECK(channelsInSpan(Band5, 5190, 50e6).size() == 2 && channelsInSpan(Band5, 5180, 20e6).size() == 0 && channelsInSpan(Band5, 5180, 25e6).size() == 1 && channelsInSpan(Band6, 5975, 80e6).size() >= 3, "channels in span");

    // ---- HT Capabilities (9.4.2.55): info 0x0062 = 40 MHz supported (B1), short GI 20 (B5) and 40 (B6); MCS set: Rx bitmask MCS 0 - 15
    {
        std::vector<uint8_t> b = {0x62, 0x00, 0x17, 0xFF, 0xFF, 0, 0};
        b.resize(26, 0);
        std::vector<uint8_t> f = el(45, b);
        cat(f, el(61, [] { std::vector<uint8_t> o(22, 0); o[0] = 36; o[1] = 0x05; return o; }()));   // HT Operation: primary 36, secondary above (1), any width (B2)
        Elems e;
        CHECK(parseElements(f.data(), f.size(), e) == 2 && e.ht && e.ht40 && e.htSgi && e.htStreams == 2 && e.htOp && e.htPrimary == 36 && e.htSecOffset == 1 && e.htOpWide, "HT elements");
        combineElems(e, Band5);
        CHECK(e.primary == 36 && e.widthMhz == 40 && e.standard == "a/n", "HT combined %d %d %s", e.primary, e.widthMhz, e.standard.c_str());
        CHECK(2 * htRateX10(40, true) == 3000 && htRateX10(20, false) == 650 && htRateX10(20, true) == 722 && htRateX10(40, false) == 1350, "HT rates");   // 802.11-2020 Table 19-27 - 19-30: MCS 7: 65 / 72.2 and 135 / 150
        CHECK(capsText(e).find("Wi-Fi 4") != std::string::npos && capsText(e).find("2x2") != std::string::npos && capsText(e).find("300 Mbit/s") != std::string::npos, "HT text: %s", capsText(e).c_str());
    }
    // ---- VHT (9.4.2.157 / 9.4.2.158): width set 1 (B2), short GI 80 (B5), Rx map streams 1 - 2 MCS 0 - 9 (10b each), the rest 11b = 0xFFFA
    {
        std::vector<uint8_t> v = {0x24, 0x00, 0x00, 0x00, 0xFA, 0xFF, 0x00, 0x00, 0xFA, 0xFF, 0x00, 0x00};
        std::vector<uint8_t> f = el(191, v);
        cat(f, el(192, {1, 42, 0, 0xFC, 0xFF}));   // 80 MHz, centre channel 42
        Elems e;
        parseElements(f.data(), f.size(), e);
        CHECK(e.vht && e.vhtWidthSet == 1 && e.vhtSgi80 && e.vhtStreams == 2 && e.vhtMaxMcs == 9 && e.vhtOp && e.vhtOpWidth == 1 && e.vhtC0 == 42, "VHT elements");
        e.htOp = true; e.htPrimary = 36;
        combineElems(e, Band5);
        CHECK(e.widthMhz == 80 && e.centreChannel == 42 && e.standard == "a/ac", "VHT combined %d %d %s", e.widthMhz, e.centreChannel, e.standard.c_str());
        CHECK(2 * vhtRateX10(160, true, 9) == 17334 && vhtRateX10(80, true, 9) == 4333 && vhtRateX10(20, true, 8) == 867 && vhtRateX10(40, true, 9) == 2000, "VHT rates");   // 866.7 x 2, 433.3, 86.7, 200
        // 160 MHz in VHT Operation (802.11-2020 9.4.2.158 Table 9-253): width 1, CCFS0 = the 80 MHz segment with the primary, CCFS1 = the 160 MHz centre
        std::vector<uint8_t> g = el(192, {1, 42, 50, 0xFC, 0xFF});
        Elems h;
        parseElements(g.data(), g.size(), h);
        combineElems(h, Band5);
        CHECK(h.widthMhz == 160 && h.centreChannel == 50, "VHT 160: %d %d", h.widthMhz, h.centreChannel);
    }
    // ---- HE (802.11ax 9.4.2.248 / .249): TWT responder (MAC B2), PHY B2 (80 MHz in 5 GHz), 2 streams MCS 0 - 11 (10b), BSS color 5, 6 GHz information
    {
        std::vector<uint8_t> c(6, 0); c[0] = 0x04;
        std::vector<uint8_t> phy(11, 0); phy[0] = 0x04;
        c.insert(c.end(), phy.begin(), phy.end());
        for (int k = 0; k < 2; k++) { c.push_back(0xFA); c.push_back(0xFF); }
        std::vector<uint8_t> f = ex(35, c);
        // HE Operation: parameters 24 bits with B17 (6 GHz operation information present) = 0x020000, BSS color 5, basic MCS 0xFFFC, then 6 GHz information
        std::vector<uint8_t> o = {0x00, 0x00, 0x02, 0x05, 0xFC, 0xFF};
        const uint8_t g6[5] = {5, 0x02, 7, 0, 0};   // primary 5, control: width 80 (2), indoor (0), CCFS0 7, CCFS1 0
        o.insert(o.end(), g6, g6 + 5);
        cat(f, ex(36, o));
        Elems e;
        parseElements(f.data(), f.size(), e);
        CHECK(e.he && e.heTwtResp && !e.heTwtReq && e.heStreams == 2 && e.heMaxMcs == 11 && (e.heWidthBits & 2), "HE capabilities");
        CHECK(e.heOp && e.bssColor == 5 && !e.bssColorDisabled && e.he6 && e.he6Primary == 5 && e.he6Width == 80 && e.he6C0 == 7 && e.he6C1 == 0 && e.he6Reg == 0 && !e.he6Dup, "HE operation");
        combineElems(e, Band6);
        CHECK(e.primary == 5 && e.widthMhz == 80 && e.centreChannel == 7 && e.standard == "ax", "HE combined: %d %d %s", e.primary, e.widthMhz, e.standard.c_str());
        CHECK(heRateX10(80, 11) == 6005 && heRateX10(20, 11) == 1434 && heRateX10(160, 11) == 12010 && heRateX10(40, 11) == 2868, "HE rates %d", heRateX10(80, 11));   // 600.5, 143.4, 1201.0, 286.8 Mbit/s
        CHECK(capsText(e).find("Wi-Fi 6") != std::string::npos && capsText(e).find("1201 Mbit/s") != std::string::npos && capsText(e).find("TWT") != std::string::npos && capsText(e).find("BSS color 5") != std::string::npos, "HE text: %s", capsText(e).c_str());
        // BSS color disabled (B7 of the BSS color information) and the 6 GHz regulatory info values and duplicate beacon bit
        std::vector<uint8_t> o2 = {0x00, 0x00, 0x02, 0x83, 0xFC, 0xFF, 37, 0x03 | 0x04 | (1 << 3), 47, 47, 0};
        Elems h;
        std::vector<uint8_t> f2 = ex(36, o2);
        parseElements(f2.data(), f2.size(), h);
        combineElems(h, Band6);
        CHECK(h.bssColor == 3 && h.bssColorDisabled && h.he6Primary == 37 && h.he6Width == 160 && h.he6Dup && h.he6Reg == 1 && h.centreChannel == 47, "HE 6 GHz 160 / standard power / dup");
    }
    // ---- EHT (802.11be): operation with information present, 320 MHz, CCFS0 31; capabilities present
    {
        std::vector<uint8_t> o = {0x01, 0, 0, 0, 0, 0x04, 31, 15};
        std::vector<uint8_t> f = ex(106, o);
        cat(f, ex(108, std::vector<uint8_t>(15, 0)));
        Elems e;
        parseElements(f.data(), f.size(), e);
        CHECK(e.eht && e.ehtOp && e.ehtWidth == 320 && e.ehtC0 == 31 && e.ehtC1 == 15, "EHT elements");
        combineElems(e, Band6);
        CHECK(e.widthMhz == 320 && e.standard == "be" && capsText(e).find("Wi-Fi 7") != std::string::npos, "EHT combined %d %s", e.widthMhz, e.standard.c_str());
    }
    // ---- Reduced Neighbor Report (9.4.2.170): one Neighbor AP Information field, operating class 131 channel 5, TBTT information length 12
    {
        std::vector<uint8_t> b = {0x00, 0x0C, 131, 5, 0x10};   // header: type 0, count 1 (B4-7 = 0), length 12 (B8-15); then the TBTT information set
        const uint8_t bssid[6] = {0x24, 0xA4, 0x3C, 0x60, 0x00, 0x01};
        b.insert(b.end(), bssid, bssid + 6);
        const uint8_t ss[4] = {0x78, 0x56, 0x34, 0x12};
        b.insert(b.end(), ss, ss + 4);
        b.push_back(0x62);   // BSS parameters: same SSID (B1), unsolicited probe responses (B5), co-located (B6)
        std::vector<uint8_t> f = el(201, b);
        Elems e;
        parseElements(f.data(), f.size(), e);
        CHECK(e.rnr.size() == 1 && e.rnr[0].opClass == 131 && e.rnr[0].channel == 5 && e.rnr[0].band == Band6 && e.rnr[0].hasBssid && !memcmp(e.rnr[0].bssid, bssid, 6) &&
              e.rnr[0].hasShortSsid && e.rnr[0].shortSsid == 0x12345678 && e.rnr[0].coLocated && e.rnr[0].unsolicitedProbe && e.rnr[0].sameSsid && !e.rnr[0].memberOfEss24or5, "RNR entry");
        // two sets in one field (count 2) with 7-byte TBTT information (offset + BSSID), then a field with a short-SSID-only set of length 5
        std::vector<uint8_t> b2 = {0x10, 0x07, 133, 21, 0x01};
        b2.insert(b2.end(), bssid, bssid + 6);
        b2.push_back(0x02); b2.insert(b2.end(), bssid, bssid + 6);
        b2.insert(b2.end(), {0x00, 0x05, 134, 37, 0x00, 1, 2, 3, 4});
        std::vector<uint8_t> f2 = el(201, b2);
        Elems h;
        parseElements(f2.data(), f2.size(), h);
        CHECK(h.rnr.size() == 3 && h.rnr[0].channel == 21 && h.rnr[1].opClass == 133 && h.rnr[1].hasBssid && h.rnr[1].params < 0 && h.rnr[2].opClass == 134 && h.rnr[2].hasShortSsid && !h.rnr[2].hasBssid &&
              h.rnr[2].shortSsid == 0x04030201, "RNR two fields: %zu entries", h.rnr.size());
    }
    // ---- RSN (9.4.2.24): WPA3 transition, SAE and PSK, management frame protection capable
    {
        std::vector<uint8_t> r = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 2, 0, 0x00, 0x0F, 0xAC, 8, 0x00, 0x0F, 0xAC, 2, 0x80, 0x00};   // RSN capabilities MFPC (B7)
        std::vector<uint8_t> f = el(48, r);
        Elems e;
        parseElements(f.data(), f.size(), e);
        CHECK(e.rsnVersion == 1 && e.akms == "SAE, PSK" && e.pmf == 1, "RSN transition: %s pmf %d", e.akms.c_str(), e.pmf);
        std::vector<uint8_t> r2 = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 18, 0xC0, 0x00};   // OWE, MFPR + MFPC
        f = el(48, r2);
        Elems h;
        parseElements(f.data(), f.size(), h);
        CHECK(h.akms == "OWE" && h.pmf == 2, "RSN OWE: %s pmf %d", h.akms.c_str(), h.pmf);
        std::vector<uint8_t> r3 = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 2, 0, 0x00, 0x0F, 0xAC, 1, 0x00, 0x0F, 0xAC, 5, 0x00, 0x00};   // 802.1X and 802.1X-SHA256
        f = el(48, r3);
        Elems k;
        parseElements(f.data(), f.size(), k);
        CHECK(k.akms == "802.1X, 802.1X-SHA256" && k.pmf == 0, "RSN enterprise: %s", k.akms.c_str());
    }
    // ---- country (9.4.2.8), power constraint (9.4.2.13), mobility domain (9.4.2.46)
    {
        std::vector<uint8_t> f = el(7, {'D', 'E', 'I', 36, 4, 23});
        cat(f, el(32, {3}));
        cat(f, el(54, {0x34, 0x12, 0x01}));
        Elems e;
        parseElements(f.data(), f.size(), e);
        CHECK(e.country == "DEI" && e.powerConstraint == 3 && e.mobility && e.mdid == 0x1234, "country / power constraint / mobility domain");
    }
    // ---- whole frames through parseFrame: a 5 GHz beacon (HT primary, no DS parameter set) and a 6 GHz one with RNR on a 5 GHz beacon
    {
        ApSpec a;
        const uint8_t m[6] = {0x24, 0xA4, 0x3C, 1, 2, 3};
        memcpy(a.bssid, m, 6);
        a.ssid = "Five"; a.band = Band5; a.channel = 44; a.ht = a.vht = a.he = true; a.width = 80; a.streams = 2; a.bssColor = 9; a.country = "LT "; a.powerConstraint = 6; a.mdid = 0x00AB;
        const auto f = makeBeacon(a);
        Frame fr;
        CHECK(parseFrame(f.data(), f.size() - 4, fr, Band5) && fr.beacon && fr.channel == 44 && fr.el.primary == 44 && fr.el.widthMhz == 80 && fr.el.centreChannel == 42 && fr.el.standard == "a/n/ac/ax" &&
              fr.el.bssColor == 9 && fr.el.country == "LT " && fr.el.powerConstraint == 6 && fr.el.mdid == 0xAB && fr.security == Wpa2 && ratesText(fr.rates) == "6 9 12 18 24 36 48 54", "5 GHz beacon: ch %d width %d std %s",
              fr.channel, fr.el.widthMhz, fr.el.standard.c_str());
        a.band = Band6; a.channel = 37; a.vht = a.ht = false; a.eht = true; a.width = 160; a.he6Reg = 1; a.security = Wpa3;
        const auto g = makeBeacon(a);
        Frame f6;
        CHECK(parseFrame(g.data(), g.size() - 4, f6, Band6) && f6.channel == 37 && f6.el.widthMhz == 160 && f6.el.centreChannel == 47 && f6.el.standard == "ax/be" && f6.el.he6Reg == 1 && f6.security == Wpa3 &&
              f6.el.pmf == 2, "6 GHz beacon: ch %d width %d std %s", f6.channel, f6.el.widthMhz, f6.el.standard.c_str());
        // FILS discovery (9.6.7.36): action frame, category 4, action 34, SSID length - 1 in B0-4
        const auto fd = makeFilsDiscovery(a);
        Frame ff;
        CHECK(parseFrame(fd.data(), fd.size() - 4, ff, Band6) && ff.fils && !ff.beacon && ff.ssid == "Five" && macText(ff.bssid) == "24:A4:3C:01:02:03" && ff.interval == 100 && ff.shortSsid == shortSsid("Five"), "FILS discovery");
        FilsInfo fi;
        const uint8_t body[] = {4, 34, 0x03, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 100, 0, 'T', 'e', 's', 't'};
        CHECK(parseFils(body, sizeof body, fi) && fi.ssid == "Test" && fi.interval == 100 && fi.tsf == 0x0807060504030201ull, "FILS bytes");
        CHECK(!parseFils(body, 10, fi) && !parseFils(body, 17, fi), "FILS truncated");
        // a probe response sent to the broadcast address is an unsolicited probe response
        const auto up = makeBeacon(a, true, nullptr);
        Frame fu;
        CHECK(parseFrame(up.data(), up.size() - 4, fu, Band6) && fu.probeResp && fu.unsolicited, "unsolicited probe response");
        const uint8_t sta[6] = {2, 0, 0, 0, 0, 1};
        const auto pr = makeBeacon(a, true, sta);
        CHECK(parseFrame(pr.data(), pr.size() - 4, fu, Band6) && fu.probeResp && !fu.unsolicited, "solicited probe response");
        // the RNR of a 5 GHz beacon names a 6 GHz AP
        ApSpec b = a;
        b.band = Band5; b.channel = 36; b.width = 20; b.he = true; b.eht = false; b.rnr.clear();
        Neighbor n; n.opClass = 131; n.channel = 5; n.hasBssid = true; memcpy(n.bssid, m, 6); n.coLocated = true; n.hasShortSsid = true; n.shortSsid = shortSsid("Five");
        b.rnr.push_back(n);
        const auto rb = makeBeacon(b);
        Frame fb;
        CHECK(parseFrame(rb.data(), rb.size() - 4, fb, Band5) && fb.el.rnr.size() == 1 && fb.el.rnr[0].channel == 5 && fb.el.rnr[0].coLocated, "RNR in a beacon");
    }
    // ---- no element parser reads past its end: every prefix of a rich beacon and of random bytes
    {
        ApSpec a;
        a.band = Band6; a.channel = 5; a.he = a.eht = true; a.width = 80; a.country = "DE "; a.mdid = 1; a.powerConstraint = 2; a.security = Wpa3;
        Neighbor n; n.opClass = 131; n.channel = 5; n.hasBssid = true; n.hasShortSsid = true; a.rnr.push_back(n);
        const auto f = makeBeacon(a);
        for (size_t k = 0; k <= f.size(); k++) { Frame x; parseFrame(f.data(), k, x, Band6); }
        for (size_t k = 0; k < 400; k++) { std::vector<uint8_t> r(k % 90 + 2); uint32_t s = (uint32_t)k * 2654435761u; for (auto& v : r) { s = s * 1664525u + 1013904223u; v = (uint8_t)(s >> 24); } Elems e; parseElements(r.data(), r.size(), e); combineElems(e, Band5); capsText(e); }
    }
    // ---- OUI table
    CHECK(ouiVendor(0x24A43C) == std::string("Ubiquiti") && ouiVendor(0x00040E) == std::string("AVM") && ouiVendor(0x000393) == std::string("Apple") && ouiVendor(0xFFFFFF) == std::string(""), "OUI table");
    return testSummary("wifi elems");
}
