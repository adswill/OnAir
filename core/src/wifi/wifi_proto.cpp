// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi 802.11 DSSS PHY and management frames (see wifi_proto.h).
#include "dect2/wifi/wifi_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include "dect2/common/crc.h"
#include "dect2/common/endian.h"

namespace dect2::wifi {

const int kBarker[11] = {1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1};

double channelMhz(int ch) {
    if (ch >= 1 && ch <= 13) return 2407 + 5.0 * ch;
    if (ch == 14) return 2484;
    return 0;
}

int channelOf(double mhz) {
    for (int c = 1; c <= 14; c++) if (std::fabs(mhz - channelMhz(c)) <= 1.0) return c;
    return 0;
}

uint32_t crc32(const uint8_t* p, size_t n) { return crc::crc32(p, n); }

uint16_t crc16(const uint8_t* bits, size_t n) { return (uint16_t)crc::bits(bits, n, 16, 0x1021, 0xFFFF, 0xFFFF); }

void appendFcs(std::vector<uint8_t>& f) {
    const uint32_t c = crc32(f.data(), f.size());
    for (int k = 0; k < 4; k++) f.push_back((uint8_t)(c >> (8 * k)));
}

std::vector<uint8_t> ppduBits(const std::vector<uint8_t>& mpdu, uint8_t seed) {
    std::vector<uint8_t> d;
    d.reserve(kSyncBits + 64 + mpdu.size() * 8);
    for (int i = 0; i < kSyncBits; i++) d.push_back(1);
    for (int k = 0; k < 16; k++) d.push_back((uint8_t)(kSfd >> k & 1));
    const uint16_t lenUs = (uint16_t)(mpdu.size() * 8);   // 1 Mbit/s: a microsecond per bit
    const uint8_t hdr[4] = {kSignal1M, 0x04, (uint8_t)lenUs, (uint8_t)(lenUs >> 8)};   // SERVICE: locked clocks
    uint8_t hb[32];
    for (int i = 0; i < 32; i++) hb[i] = (uint8_t)(hdr[i / 8] >> (i % 8) & 1);
    const uint16_t crc = crc16(hb, 32);
    d.insert(d.end(), hb, hb + 32);
    for (int k = 15; k >= 0; k--) d.push_back((uint8_t)(crc >> k & 1));
    for (uint8_t b : mpdu) for (int k = 0; k < 8; k++) d.push_back((uint8_t)(b >> k & 1));
    // self-synchronising scrambler z^-7 + z^-4 + 1
    uint8_t s = seed & 0x7F;
    for (uint8_t& b : d) {
        const uint8_t o = (uint8_t)(b ^ (s >> 3 & 1) ^ (s >> 6 & 1));
        s = (uint8_t)(((s << 1) | o) & 0x7F);
        b = o;
    }
    return d;
}

DsssPpdu dsssPpdu(const std::vector<uint8_t>& mpdu, int rateMbps, bool shortPre, uint8_t seed) {
    const bool two = rateMbps == 2 || shortPre;
    DsssPpdu out;
    std::vector<uint8_t>& d = out.bits;
    d.reserve(kSyncBits + 64 + mpdu.size() * 8);
    if (shortPre) {
        for (int i = 0; i < kSyncShortBits; i++) d.push_back(0);
        for (int k = 0; k < 16; k++) d.push_back((uint8_t)(kSfdShort >> k & 1));
    } else {
        for (int i = 0; i < kSyncBits; i++) d.push_back(1);
        for (int k = 0; k < 16; k++) d.push_back((uint8_t)(kSfd >> k & 1));
    }
    const uint16_t lenUs = (uint16_t)(mpdu.size() * (two ? 4 : 8));   // microseconds of the MPDU
    const uint8_t hdr[4] = {two ? kSignal2M : kSignal1M, 0x04, (uint8_t)lenUs, (uint8_t)(lenUs >> 8)};
    uint8_t hb[32];
    for (int i = 0; i < 32; i++) hb[i] = (uint8_t)(hdr[i / 8] >> (i % 8) & 1);
    const uint16_t crc = crc16(hb, 32);
    const size_t hdrAt = d.size();
    d.insert(d.end(), hb, hb + 32);
    for (int k = 15; k >= 0; k--) d.push_back((uint8_t)(crc >> k & 1));
    out.qpskFrom = !two ? std::numeric_limits<size_t>::max() : shortPre ? hdrAt : d.size();
    for (uint8_t b : mpdu) for (int k = 0; k < 8; k++) d.push_back((uint8_t)(b >> k & 1));
    if (out.qpskFrom > d.size()) out.qpskFrom = d.size();
    uint8_t s = seed & 0x7F;
    for (uint8_t& b : d) {
        const uint8_t o = (uint8_t)(b ^ (s >> 3 & 1) ^ (s >> 6 & 1));
        s = (uint8_t)(((s << 1) | o) & 0x7F);
        b = o;
    }
    return out;
}

const char* securityName(int s, bool ent) {
    switch (s) {
    case Open: return "Open";
    case Wep: return "WEP";
    case Wpa: return ent ? "WPA-Enterprise" : "WPA";
    case WpaWpa2: return ent ? "WPA/WPA2-Enterprise" : "WPA/WPA2";
    case Wpa2: return ent ? "WPA2-Enterprise" : "WPA2";
    case Wpa2Wpa3: return ent ? "WPA2/WPA3-Enterprise" : "WPA2/WPA3";
    case Wpa3: return ent ? "WPA3-Enterprise" : "WPA3";
    case Owe: return "OWE (Enhanced Open)";
    default: return "?";
    }
}

namespace {
void put16(std::vector<uint8_t>& v, unsigned x) { dect2::endian::putLe16(v, x); }
void putMac(std::vector<uint8_t>& v, const uint8_t* a) { v.insert(v.end(), a, a + 6); }
void suite(std::vector<uint8_t>& v, const uint8_t oui[3], uint8_t t) { v.insert(v.end(), oui, oui + 3); v.push_back(t); }
const uint8_t kRsnOui[3] = {0x00, 0x0F, 0xAC}, kMsOui[3] = {0x00, 0x50, 0xF2};
} // namespace

std::vector<uint8_t> makeBeacon(const ApSpec& s, bool probe, const uint8_t* client) {
    static const uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<uint8_t> f;
    f.push_back(probe ? 0x50 : 0x80); f.push_back(0);
    put16(f, 0);
    putMac(f, probe && client ? client : bc);
    putMac(f, s.bssid); putMac(f, s.bssid);
    put16(f, (unsigned)(s.seq & 0xFFF) << 4);
    for (int k = 0; k < 8; k++) f.push_back((uint8_t)(s.tsf >> (8 * k)));
    put16(f, (unsigned)s.interval);
    unsigned cap = 0x0001 | 0x0020 | 0x0400;   // ESS, short preamble, short slot
    if (s.security != Open && s.security != Owe) cap |= 0x0010;   // privacy
    put16(f, cap);
    f.push_back(0);
    if (s.hidden && !probe) f.push_back(0);
    else { f.push_back((uint8_t)s.ssid.size()); f.insert(f.end(), s.ssid.begin(), s.ssid.end()); }
    const uint8_t b[4] = {0x82, 0x84, 0x8B, 0x96}, g1[4] = {0x0C, 0x12, 0x18, 0x24}, g2[4] = {0x30, 0x48, 0x60, 0x6C};
    if (s.band != 0) {   // 5 and 6 GHz: OFDM rates only, 6 / 12 / 24 Mbit/s basic; no DS parameter set (the primary channel is in HT / HE operation)
        const uint8_t o[8] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
        f.push_back(1); f.push_back(8); f.insert(f.end(), o, o + 8);
        if (!probe) { const uint8_t tim[6] = {5, 4, 0, 1, 0, 0}; f.insert(f.end(), tim, tim + 6); }
    } else {
    f.push_back(1); f.push_back(s.gRates ? 8 : 4); f.insert(f.end(), b, b + 4);
    if (s.gRates) f.insert(f.end(), g1, g1 + 4);
    f.push_back(3); f.push_back(1); f.push_back((uint8_t)s.channel);
    if (!probe) { const uint8_t tim[6] = {5, 4, 0, 1, 0, 0}; f.insert(f.end(), tim, tim + 6); }
    if (s.gRates) { f.push_back(50); f.push_back(4); f.insert(f.end(), g2, g2 + 4); }
    }
    auto rsn = [&](std::vector<uint8_t> akms, uint8_t pairwise, bool mfp) {
        std::vector<uint8_t> e;
        put16(e, 1);
        suite(e, kRsnOui, pairwise == 2 ? 2 : 4);
        put16(e, 1); suite(e, kRsnOui, pairwise);
        put16(e, (unsigned)akms.size());
        for (uint8_t a : akms) suite(e, kRsnOui, a);
        put16(e, mfp ? 0x0080 | (akms.size() == 1 ? 0x0040 : 0) : 0);   // MFPC, and MFPR when SAE only
        f.push_back(48); f.push_back((uint8_t)e.size()); f.insert(f.end(), e.begin(), e.end());
    };
    auto wpa = [&] {
        std::vector<uint8_t> e;
        suite(e, kMsOui, 1);
        put16(e, 1); suite(e, kMsOui, 2);
        put16(e, 1); suite(e, kMsOui, 2);
        put16(e, 1); suite(e, kMsOui, s.enterprise ? 1 : 2);
        f.push_back(221); f.push_back((uint8_t)e.size()); f.insert(f.end(), e.begin(), e.end());
    };
    const uint8_t base = s.enterprise ? 1 : 2;
    switch (s.security) {
    case Wpa: wpa(); break;
    case WpaWpa2: rsn({base}, 4, false); wpa(); break;
    case Wpa2: rsn({base}, 4, false); break;
    case Wpa2Wpa3: rsn({base, (uint8_t)(s.enterprise ? 5 : 8)}, 4, true); break;
    case Wpa3: rsn({(uint8_t)(s.enterprise ? 12 : 8)}, 4, true); break;
    case Owe: rsn({18}, 4, true); break;
    default: break;
    }
    appendModernElements(f, s);
    appendFcs(f);
    return f;
}

std::vector<uint8_t> makeDataFrame(const uint8_t bssid[6], const uint8_t sta[6], size_t payload) {
    std::vector<uint8_t> f = {0x08, 0x42};   // data, from DS, protected
    put16(f, 44);
    putMac(f, sta); putMac(f, bssid); putMac(f, bssid);
    put16(f, 0x120);
    uint32_t x = 0x12345678;
    for (size_t i = 0; i < payload; i++) { x = x * 1664525u + 1013904223u; f.push_back((uint8_t)(x >> 24)); }
    appendFcs(f);
    return f;
}

std::string macText(const uint8_t a[6]) {
    char b[20];
    snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    return b;
}

const char* vendorOf(const uint8_t a[6]) {
    if (a[0] & 0x02) return "local";   // locally administered: a virtual or randomised BSSID
    return ouiVendor((uint32_t)a[0] << 16 | (uint32_t)a[1] << 8 | a[2]);
}

const char* frameTypeName(int type, int sub) {
    if (type == 0) {
        static const char* const m[16] = {"assoc req", "assoc resp", "reassoc req", "reassoc resp", "probe req", "probe resp", "timing adv", "mgmt 7",
                                          "beacon", "ATIM", "disassoc", "auth", "deauth", "action", "action no ack", "mgmt 15"};
        return sub >= 0 && sub < 16 ? m[sub] : "mgmt";
    }
    if (type == 1) return "control";
    if (type == 2) return "data";
    return "extension";
}

std::string ratesText(const std::vector<int>& r) {
    std::string s;
    char b[16];
    for (int x : r) {
        const int v = x & 0x7F;
        if (v == 0x7F || v == 0) continue;   // BSS membership selectors
        if (v % 2) snprintf(b, sizeof b, "%s%.1f", s.empty() ? "" : " ", v / 2.0); else snprintf(b, sizeof b, "%s%d", s.empty() ? "" : " ", v / 2);
        s += b;
    }
    return s;
}

namespace {
// the AKM and cipher suites of an RSN element (or the WPA vendor element, the same layout after its OUI and type)
void suites(const uint8_t* e, size_t n, bool wpa, bool& psk, bool& sae, bool& ent, bool& owe, bool& suiteB, std::string& ciph) {
    const uint8_t* oui = wpa ? kMsOui : kRsnOui;
    size_t i = 2;              // version
    if (n < i + 4) return;
    i += 4;                    // group cipher
    if (n < i + 2) return;
    const unsigned np = e[i] | e[i + 1] << 8; i += 2;
    for (unsigned k = 0; k < np && i + 4 <= n; k++, i += 4) {
        if (memcmp(e + i, oui, 3)) continue;
        const char* c = e[i + 3] == 2 ? "TKIP" : e[i + 3] == 4 ? "CCMP" : e[i + 3] == 8 ? "GCMP" : e[i + 3] == 9 ? "GCMP-256" : e[i + 3] == 10 ? "CCMP-256" : nullptr;
        if (c && ciph.find(c) == std::string::npos) { if (!ciph.empty()) ciph += "+"; ciph += c; }
    }
    if (n < i + 2) return;
    const unsigned na = e[i] | e[i + 1] << 8; i += 2;
    for (unsigned k = 0; k < na && i + 4 <= n; k++, i += 4) {
        if (memcmp(e + i, oui, 3)) continue;
        switch (e[i + 3]) {
        case 1: case 3: case 5: ent = true; break;
        case 2: case 4: case 6: psk = true; break;
        case 8: case 9: case 24: case 25: sae = true; break;
        case 11: case 12: ent = true; suiteB = true; break;
        case 18: owe = true; break;
        default: break;
        }
    }
}
} // namespace

bool parseFrame(const uint8_t* p, size_t n, Frame& f, int band) {
    f = Frame();
    if (n < 10) return false;
    f.type = p[0] >> 2 & 3;
    f.subtype = p[0] >> 4;
    if ((p[0] & 3) != 0) return false;   // protocol version 0 only
    memcpy(f.addr1, p + 4, 6);
    if (n >= 16) memcpy(f.addr2, p + 10, 6);
    if (n >= 22) memcpy(f.bssid, p + 16, 6);
    f.beacon = f.type == 0 && f.subtype == 8;
    f.probeResp = f.type == 0 && f.subtype == 5;
    if (f.type == 0 && f.subtype == 13 && n >= 24 + 4) {   // action: a FILS Discovery frame announces a 6 GHz AP (sender = the BSSID)
        FilsInfo fi;
        if (parseFils(p + 24, n - 24, fi)) {
            f.fils = true; f.ssid = fi.ssid; f.hidden = fi.ssid.empty(); f.interval = fi.interval; f.tsf = fi.tsf; f.shortSsid = fi.shortSsid;
            memcpy(f.bssid, p + 10, 6);
            char b[120];
            snprintf(b, sizeof b, "FILS discovery %s%s%s, %d TU", fi.ssid.empty() ? "(short SSID " : "\"", fi.ssid.empty() ? fmtShortSsid(fi.shortSsid).c_str() : fi.ssid.c_str(), fi.ssid.empty() ? ")" : "\"", fi.interval);
            f.summary = b;
            return true;
        }
    }
    if (!f.beacon && !f.probeResp) { f.summary = frameTypeName(f.type, f.subtype); return true; }
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    f.unsolicited = f.probeResp && !memcmp(f.addr1, bcast, 6);
    size_t i = 24 + ((p[1] & 0x80) ? 4 : 0);   // +HTC
    if (n < i + 12) return false;
    for (int k = 0; k < 8; k++) f.tsf |= (uint64_t)p[i + k] << (8 * k);
    f.interval = p[i + 8] | p[i + 9] << 8;
    f.cap = (uint16_t)(p[i + 10] | p[i + 11] << 8);
    i += 12;
    parseElements(p + i, n - i, f.el);
    combineElems(f.el, band);
    bool rsn = false, wpaIe = false, psk = false, sae = false, ent = false, owe = false, suiteB = false, haveSsid = false;
    bool wpsk = false, wsae = false, went = false, wowe = false, wsb = false;
    while (i + 2 <= n) {
        const int id = p[i], len = p[i + 1];
        if (i + 2 + (size_t)len > n) break;
        const uint8_t* e = p + i + 2;
        if (id == 0 && !haveSsid) {
            haveSsid = true;
            bool zero = true;
            for (int k = 0; k < len; k++) zero &= e[k] == 0;
            f.hidden = len == 0 || zero;
            if (!f.hidden)
                for (int k = 0; k < len; k++) f.ssid += (e[k] >= 0x20 && e[k] < 0x7F) || e[k] >= 0x80 ? (char)e[k] : '.';
        } else if (id == 1 || id == 50) {
            for (int k = 0; k < len; k++) f.rates.push_back(e[k]);
        } else if (id == 3 && len >= 1) {
            f.channel = e[0];
        } else if (id == 48) {
            rsn = true;
            suites(e, (size_t)len, false, psk, sae, ent, owe, suiteB, f.ciphers);
        } else if (id == 221 && len >= 4 && !memcmp(e, kMsOui, 3) && e[3] == 1) {
            wpaIe = true;
            suites(e + 4, (size_t)len - 4, true, wpsk, wsae, went, wowe, wsb, f.ciphers);
        }
        i += 2 + (size_t)len;
    }
    if (rsn) {
        f.enterprise = ent && !psk && !sae;
        if (owe && !psk && !sae && !ent) f.security = Owe;
        else if (sae || suiteB) f.security = (psk || (ent && !suiteB)) ? Wpa2Wpa3 : Wpa3;
        else f.security = wpaIe ? WpaWpa2 : Wpa2;
        if (suiteB) f.enterprise = true;
    } else if (wpaIe) {
        f.security = Wpa;
        f.enterprise = went && !wpsk;
    } else {
        f.security = (f.cap & 0x0010) ? Wep : Open;
    }
    if (!f.channel) f.channel = f.el.primary;
    char b[200];
    snprintf(b, sizeof b, "%s %s%s%s, ch %d, %s, %d TU", f.beacon ? "beacon" : "probe resp", f.hidden ? "(hidden" : "\"", f.hidden ? ")" : f.ssid.c_str(),
             f.hidden ? "" : "\"", f.channel, securityName(f.security, f.enterprise), f.interval);
    f.summary = b;
    return true;
}

} // namespace dect2::wifi
