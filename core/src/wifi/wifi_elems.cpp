// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi information element parsers (see wifi_elems.h).
#include "dect2/wifi/wifi_elems.h"
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2::wifi {

namespace {
inline unsigned le16(const uint8_t* p) { return p[0] | (unsigned)p[1] << 8; }

// 2 bits per spatial stream (stream 1 first): the highest stream that is not "not supported" (value 3), and the highest MCS those streams name.
// vht: 0 = MCS 0-7, 1 = 0-8, 2 = 0-9; he: 0 = 0-7, 1 = 0-9, 2 = 0-11.
void mcsMap(unsigned map, bool he, int& streams, int& maxMcs) {
    streams = 0; maxMcs = 0;
    for (int n = 0; n < 8; n++) {
        const unsigned v = map >> (2 * n) & 3;
        if (v == 3) continue;
        streams = n + 1;
        const int m = he ? (v == 0 ? 7 : v == 1 ? 9 : 11) : (v == 0 ? 7 : v == 1 ? 8 : 9);
        maxMcs = std::max(maxMcs, m);
    }
}

const char* akmName(int t) {
    switch (t) {
    case 1: return "802.1X"; case 2: return "PSK"; case 3: return "FT-802.1X"; case 4: return "FT-PSK";
    case 5: return "802.1X-SHA256"; case 6: return "PSK-SHA256"; case 8: return "SAE"; case 9: return "FT-SAE";
    case 11: return "Suite-B"; case 12: return "Suite-B-192"; case 18: return "OWE"; case 24: return "SAE-EXT"; case 25: return "FT-SAE-EXT";
    default: return nullptr;
    }
}

// RSN element (9.4.2.24): version, group cipher, pairwise suites, AKM suites, capabilities (B6 MFPR, B7 MFPC)
void parseRsn(const uint8_t* e, size_t n, Elems& o) {
    if (n < 2) return;
    o.rsnVersion = (int)le16(e);
    size_t i = 2;
    if (n < i + 4) return;
    i += 4;
    if (n < i + 2) return;
    const unsigned np = le16(e + i); i += 2;
    i += 4 * (size_t)np;
    if (n < i + 2) return;
    const unsigned na = le16(e + i); i += 2;
    for (unsigned k = 0; k < na && i + 4 <= n; k++, i += 4) {
        if (e[i] != 0x00 || e[i + 1] != 0x0F || e[i + 2] != 0xAC) continue;
        const char* nm = akmName(e[i + 3]);
        char b[16];
        if (!nm) { snprintf(b, sizeof b, "AKM%d", e[i + 3]); nm = b; }
        if (o.akms.find(nm) != std::string::npos) continue;
        if (!o.akms.empty()) o.akms += ", ";
        o.akms += nm;
    }
    if (n >= i + 2) { const unsigned cap = le16(e + i); o.pmf = cap & 0x40 ? 2 : cap & 0x80 ? 1 : 0; }
}

// Reduced Neighbor Report (9.4.2.170): Neighbor AP Information fields, each a TBTT header, operating class, channel and TBTT information sets
void parseRnr(const uint8_t* e, size_t n, Elems& o) {
    size_t i = 0;
    while (i + 4 <= n) {
        const unsigned hdr = le16(e + i);
        const int count = (int)(hdr >> 4 & 0xF) + 1, len = (int)(hdr >> 8);
        const int oc = e[i + 2], ch = e[i + 3];
        i += 4;
        if (len < 1 || i + (size_t)count * (size_t)len > n) return;
        for (int c = 0; c < count; c++, i += (size_t)len) {
            const uint8_t* t = e + i;
            Neighbor nb;
            nb.opClass = oc; nb.channel = ch; nb.band = operatingClassBand(oc);
            const bool hasB = len == 7 || len == 8 || len == 11 || len == 12 || len == 13 || len == 16;
            const bool hasS = len == 5 || len == 6 || len == 11 || len == 12 || len == 13 || len == 16;
            const bool hasP = len == 2 || len == 6 || len == 8 || len == 12 || len == 13 || len == 16;
            size_t k = 1;   // the TBTT offset
            if (hasB) { nb.hasBssid = true; memcpy(nb.bssid, t + k, 6); k += 6; }
            if (hasS) { nb.hasShortSsid = true; nb.shortSsid = (uint32_t)t[k] | (uint32_t)t[k + 1] << 8 | (uint32_t)t[k + 2] << 16 | (uint32_t)t[k + 3] << 24; k += 4; }
            if (hasP) {
                nb.params = t[k];
                nb.sameSsid = t[k] >> 1 & 1; nb.memberOfEss24or5 = t[k] >> 4 & 1; nb.unsolicitedProbe = t[k] >> 5 & 1; nb.coLocated = t[k] >> 6 & 1;
            }
            if (hasB || hasS) o.rnr.push_back(nb);
        }
    }
}

// one extension element (id 255, extension id first)
void parseExt(const uint8_t* e, size_t n, Elems& o) {
    if (n < 1) return;
    switch (e[0]) {
    case 35:   // HE Capabilities: ext id, MAC cap (6), PHY cap (11), Rx / Tx MCS and NSS for <= 80 MHz (2 + 2)
        if (n < 1 + 6 + 11 + 4) return;
        o.he = true;
        o.heTwtReq = e[1] >> 1 & 1;
        o.heTwtResp = e[1] >> 2 & 1;
        o.heWidthBits = e[7] >> 1 & 0x7F;
        mcsMap(le16(e + 18), true, o.heStreams, o.heMaxMcs);
        break;
    case 36: {   // HE Operation: parameters (3), BSS color info (1), basic MCS / NSS (2), then optional VHT info (3), co-hosted (1), 6 GHz info (5)
        if (n < 1 + 3 + 1 + 2) return;
        const unsigned par = e[1] | (unsigned)e[2] << 8 | (unsigned)e[3] << 16;
        o.heOp = true;
        o.heTwtRequired = par >> 3 & 1;
        o.bssColor = e[4] & 0x3F;
        o.bssColorDisabled = e[4] >> 7 & 1;
        size_t k = 7;
        if (par >> 14 & 1) k += 3;
        if (par >> 15 & 1) k += 1;
        if ((par >> 17 & 1) && n >= k + 5) {
            o.he6 = true;
            o.he6Primary = e[k];
            const int w = e[k + 1] & 3;
            o.he6Width = w == 0 ? 20 : w == 1 ? 40 : w == 2 ? 80 : 160;
            o.he6Dup = e[k + 1] >> 2 & 1;
            o.he6Reg = e[k + 1] >> 3 & 7;
            o.he6C0 = e[k + 2]; o.he6C1 = e[k + 3];
        }
        break;
    }
    case 106: {   // EHT Operation: parameters (1), basic EHT-MCS / NSS (4), operation information (control, CCFS0, CCFS1) when parameters B0 is set
        if (n < 1 + 1 + 4) return;
        o.ehtOp = true;
        if ((e[1] & 1) && n >= 1 + 1 + 4 + 3) {
            static const int w[] = {20, 40, 80, 160, 320};
            const int c = e[6] & 7;
            o.ehtWidth = c < 5 ? w[c] : 20;
            o.ehtC0 = e[7]; o.ehtC1 = e[8];
        }
        break;
    }
    case 108: o.eht = true; break;   // EHT Capabilities
    default: break;
    }
}
} // namespace

int parseElements(const uint8_t* p, size_t n, Elems& o) {
    int cnt = 0;
    size_t i = 0;
    while (i + 2 <= n) {
        const int id = p[i];
        const size_t len = p[i + 1];
        if (i + 2 + len > n) break;
        const uint8_t* e = p + i + 2;
        cnt++;
        switch (id) {
        case 7:
            if (len >= 3) { o.country.assign(3, ' '); for (int k = 0; k < 3; k++) o.country[(size_t)k] = e[k] >= 0x20 && e[k] < 0x7F ? (char)e[k] : '?'; }
            break;
        case 32: if (len >= 1) o.powerConstraint = e[0]; break;
        case 45:   // HT Capabilities: capability info (2), A-MPDU (1), supported MCS set (16) whose first four octets are the Rx masks for MCS 0 - 31
            if (len >= 26) {
                o.ht = true;
                const unsigned ci = le16(e);
                o.ht40 = ci >> 1 & 1;
                o.htSgi = (ci >> 5 & 1) || (ci >> 6 & 1);
                int s = 0;
                for (int k = 0; k < 4; k++) if (e[3 + k]) s = k + 1;
                o.htStreams = s;
            }
            break;
        case 61:   // HT Operation: primary channel, then B0-1 secondary channel offset, B2 STA channel width
            if (len >= 2) { o.htOp = true; o.htPrimary = e[0]; o.htSecOffset = e[1] & 3; o.htOpWide = e[1] >> 2 & 1; }
            break;
        case 191:   // VHT Capabilities: info (4: B2-3 width set, B5 short GI 80, B11 SU beamformer, B19 MU beamformer), Rx MCS map (2) ...
            if (len >= 12) {
                o.vht = true;
                const uint32_t ci = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
                o.vhtWidthSet = ci >> 2 & 3;
                o.vhtSgi80 = ci >> 5 & 1;
                o.vhtSuBfer = ci >> 11 & 1;
                o.vhtMuBfer = ci >> 19 & 1;
                mcsMap(le16(e + 4), false, o.vhtStreams, o.vhtMaxMcs);
            }
            break;
        case 192:   // VHT Operation: channel width, centre frequency segment 0 and 1
            if (len >= 3) { o.vhtOp = true; o.vhtOpWidth = e[0]; o.vhtC0 = e[1]; o.vhtC1 = e[2]; }
            break;
        case 201: parseRnr(e, len, o); break;
        case 54: if (len >= 3) { o.mobility = true; o.mdid = (int)le16(e); } break;
        case 48: parseRsn(e, len, o); break;
        case 255: parseExt(e, len, o); break;
        default: break;
        }
        i += 2 + len;
    }
    return cnt;
}

// rate of one stream from the standard's data subcarrier counts and the symbol time
static int rateX10(int nsd, double bitsPerSc, double tsymUs) { return (int)std::lround(nsd * bitsPerSc * 10 / tsymUs); }
static int nsdHtVht(int w) { return w >= 160 ? 468 : w >= 80 ? 234 : w >= 40 ? 108 : 52; }
int htRateX10(int w, bool sgi) { return rateX10(w >= 40 ? 108 : 52, 5.0, sgi ? 3.6 : 4.0); }               // MCS 7: 64-QAM 5/6 = 5 bits
int vhtRateX10(int w, bool sgi, int mcs) { return rateX10(nsdHtVht(w), mcs >= 9 ? 8 * 5.0 / 6 : mcs == 8 ? 6.0 : 5.0, sgi ? 3.6 : 4.0); }
int heRateX10(int w, int mcs) {
    const int nsd = w >= 160 ? 1960 : w >= 80 ? 980 : w >= 40 ? 468 : 234;
    return rateX10(nsd, mcs >= 11 ? 10 * 5.0 / 6 : mcs >= 9 ? 8 * 5.0 / 6 : 5.0, 13.6);                    // 0.8 us guard interval: 12.8 + 0.8
}

uint32_t shortSsid(const std::string& s) { return crc32((const uint8_t*)s.data(), s.size()); }
std::string fmtShortSsid(uint32_t s) { char b[12]; snprintf(b, sizeof b, "%08X", s); return b; }

void combineElems(Elems& e, int band) {
    e.primary = e.he6 && e.he6Primary ? e.he6Primary : e.htOp ? e.htPrimary : 0;
    int w = 20, c = 0;
    if (e.htOp && e.htOpWide && e.htSecOffset) w = 40;
    if (e.vhtOp) {
        if (e.vhtOpWidth == 1) { w = e.vhtC1 == 0 ? 80 : (std::abs(e.vhtC1 - e.vhtC0) == 8 ? 160 : 80); c = e.vhtC0; if (w == 160) c = e.vhtC1 ? e.vhtC1 : e.vhtC0; }
        else if (e.vhtOpWidth == 2) { w = 160; c = e.vhtC0; }
        else if (e.vhtOpWidth == 3) { w = 80; c = e.vhtC0; }
    }
    if (e.he6) { w = e.he6Width; c = w == 160 && e.he6C1 ? e.he6C1 : e.he6C0; }
    if (e.ehtOp && e.ehtWidth) { w = e.ehtWidth; c = w >= 160 && e.ehtC1 ? e.ehtC1 : e.ehtC0; }
    e.widthMhz = w; e.centreChannel = c;
    std::string s;
    auto add = [&](const char* x) { if (!s.empty()) s += "/"; s += x; };
    if (band == 1) add("a"); else if (band == 0) add("g");
    if (e.ht) add("n");
    if (e.vht && band != 0) add("ac");
    if (e.he) add("ax");
    if (e.eht) add("be");
    e.standard = s;
}

std::string capsText(const Elems& e) {
    std::string s;
    char b[96];
    int maxW = 20, ss = 0, mbps10 = 0;
    const char* gen = "";
    if (e.eht) { gen = "Wi-Fi 7 (EHT)"; maxW = std::max(maxW, e.widthMhz); }
    else if (e.he) {
        gen = "Wi-Fi 6 (HE)";
        const int wb = e.heWidthBits;
        maxW = wb & 0xC ? 160 : wb & 2 ? 80 : wb & 1 ? 40 : 20;
        ss = e.heStreams;
        mbps10 = ss * heRateX10(maxW, e.heMaxMcs);
    } else if (e.vht) {
        gen = "Wi-Fi 5 (VHT)";
        maxW = e.vhtWidthSet ? 160 : 80;
        ss = e.vhtStreams;
        mbps10 = ss * vhtRateX10(maxW, true, e.vhtMaxMcs);
    } else if (e.ht) {
        gen = "Wi-Fi 4 (HT)";
        maxW = e.ht40 ? 40 : 20;
        ss = e.htStreams;
        mbps10 = ss * htRateX10(maxW, e.htSgi);
    }
    s = gen;
    if (ss) { snprintf(b, sizeof b, " %dx%d", ss, ss); s += b; }
    if (*gen) { snprintf(b, sizeof b, ", up to %d MHz wide", maxW); s += b; }
    if (mbps10) { snprintf(b, sizeof b, ", up to %d Mbit/s", mbps10 / 10); s += b; }
    if (e.he && (e.heTwtReq || e.heTwtResp)) s += ", TWT";
    if (e.bssColor >= 0) { snprintf(b, sizeof b, ", BSS color %d%s", e.bssColor, e.bssColorDisabled ? " (disabled)" : ""); s += b; }
    return s;
}

bool parseFils(const uint8_t* p, size_t n, FilsInfo& f) {
    f = FilsInfo();
    if (n < 4 + 8 + 2 || p[0] != 4 || p[1] != 34) return false;
    f.control = (uint16_t)le16(p + 2);
    for (int k = 0; k < 8; k++) f.tsf |= (uint64_t)p[4 + k] << (8 * k);
    f.interval = (int)le16(p + 12);
    size_t i = 14;
    const size_t ssidLen = (size_t)(f.control & 0x1F) + 1;
    if (f.control >> 6 & 1) {   // the short SSID is sent instead of the name
        if (n < i + 4) return false;
        f.hasShort = true;
        f.shortSsid = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
    } else {
        if (n < i + ssidLen) return false;
        for (size_t k = 0; k < ssidLen; k++) f.ssid += p[i + k] >= 0x20 && p[i + k] < 0x7F ? (char)p[i + k] : '.';
        f.hasShort = true;
        f.shortSsid = shortSsid(f.ssid);
    }
    f.valid = true;
    return true;
}

} // namespace dect2::wifi
