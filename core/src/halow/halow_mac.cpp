// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) MAC frames, receive side and test-signal builders (halow_mac.h): S1G beacon, management beacon, probe request and response,
// data frames, the information elements a monitor lists (SSID, RSN, S1G Operation and Capabilities, TIM, Country, power, rates, RAW, TWT) and the FCS.
// IEEE 802.11-2020 clause 9 (frame formats: 9.3.4 S1G beacon, 9.4.2 information elements).
#include "dect2/halow/halow_mac.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2::halow {

namespace {

// little endian field of n bytes
void putLe(std::vector<uint8_t>& v, uint32_t x, int n) { for (int i = 0; i < n; i++) v.push_back((uint8_t)(x >> (8 * i))); }

// an information element: id, length, body
void addIe(std::vector<uint8_t>& v, int id, const std::vector<uint8_t>& body) {
    v.push_back((uint8_t)id);
    v.push_back((uint8_t)body.size());
    v.insert(v.end(), body.begin(), body.end());
}

// S1G Operation channel-width code (9.4.2.212): 0 1 MHz, 1 2 MHz, 3 4 MHz, 7 8 MHz, 15 16 MHz (a run of ones)
int widthCode(int mhz) { return mhz >= 16 ? 15 : mhz >= 8 ? 7 : mhz >= 4 ? 3 : mhz >= 2 ? 1 : 0; }

// The elements of a beacon or probe response body (withSsid: the SSID element; withCompat: the S1G Beacon Compatibility element, 213)
void addBodyIes(std::vector<uint8_t>& v, const BssSpec& b, bool withSsid, bool withCompat) {
    if (withSsid) addIe(v, 0, std::vector<uint8_t>(b.ssid.begin(), b.ssid.end()));
    if (b.rates) addIe(v, 1, {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C});   // 6 9 12 18 24 36 48 54 Mbit/s (legacy set a management beacon still lists)
    if (withCompat) {   // S1G Beacon Compatibility: capabilities, beacon interval, TSF completion
        std::vector<uint8_t> c;
        putLe(c, 0x0001, 2); putLe(c, (uint32_t)b.intervalTu, 2); putLe(c, 0, 4);
        addIe(v, 213, c);
    }
    if (b.tim) addIe(v, 5, {0, (uint8_t)b.dtimPeriod, 0, 0});   // TIM: DTIM count, DTIM period, bitmap control, one empty bitmap octet
    if (!b.country.empty()) {   // Country: the two letters, an environment octet, one triplet (first channel, number of channels, max power dBm)
        std::vector<uint8_t> c(b.country.begin(), b.country.end());
        c.resize(2, ' ');
        c.push_back(' '); c.push_back(1); c.push_back(26); c.push_back(30);
        addIe(v, 7, c);
    }
    if (b.txPowerDbm >= 0) addIe(v, 32, {(uint8_t)b.txPowerDbm});   // power constraint
    if (b.security != Open && b.security != Secured) {
        std::vector<uint8_t> r;
        putLe(r, 1, 2);                              // version
        r.insert(r.end(), {0x00, 0x0F, 0xAC, 0x04}); // group cipher CCMP
        putLe(r, 1, 2); r.insert(r.end(), {0x00, 0x0F, 0xAC, 0x04});   // pairwise: CCMP
        const uint8_t akm = b.security == Wpa3Sae ? 8 : b.security == Owe ? 18 : b.security == Wpa2Ent ? 1 : 2;
        putLe(r, 1, 2); r.insert(r.end(), {0x00, 0x0F, 0xAC, akm});
        putLe(r, 0x00C0, 2);                         // capabilities (management frame protection)
        addIe(v, 48, r);
    }
    if (b.vendorOui[0] | b.vendorOui[1] | b.vendorOui[2]) addIe(v, 221, {b.vendorOui[0], b.vendorOui[1], b.vendorOui[2], 0x01, 0x00});   // vendor specific: OUI and a short body
    if (b.raw) addIe(v, 180, {0x01, 0x00, 0x00, 0x00, 0x10, 0x00});   // RAW parameter set (unverified layout: only its presence is read)
    if (b.twt) addIe(v, 216, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});   // TWT (presence only)
    std::vector<uint8_t> cap(15, 0);                 // S1G Capabilities: B0 = long preamble supported, nothing else is read
    cap[0] = (uint8_t)(0x2A | (b.longPreamble ? 1 : 0));
    addIe(v, 217, cap);
    std::vector<uint8_t> op;                         // S1G Operation: width, class, primary channel, centre frequency number, MCS / NSS set
    op.push_back((uint8_t)(widthCode(b.opWidthMhz) << 1 | (b.primaryWidthMhz == 1 || b.opWidthMhz == 1 ? 1 : 0)));
    op.push_back((uint8_t)b.opClass); op.push_back((uint8_t)b.primary); op.push_back((uint8_t)b.ccf);
    putLe(op, 0xFFFC | (uint32_t)(b.maxMcsOneSs & 3), 2);
    addIe(v, 232, op);
}

// the first octet of an S1G beacon's frame control flags: compressed SSID (bit 1), next TBTT (bit 0), ANO (bit 2), bandwidth indication (bits 3 - 5), security (bit 6)
uint8_t beaconFlags(const BssSpec& b) { return (uint8_t)((b.compressedSsid ? 2 : 0) | (b.ano ? 4 : 0) | (b.bwInd & 7) << 3 | (b.security != Open ? 0x40 : 0)); }

} // namespace

// Display name of a security type.
const char* securityName(int s) {
    switch (s) {
        case Open: return "Open";
        case Wpa2Psk: return "WPA2-PSK";
        case Wpa3Sae: return "WPA3-SAE";
        case Owe: return "OWE";
        case Wpa2Ent: return "WPA2-Enterprise";
        case RsnOther: return "RSN";
        case Secured: return "Secured";
        default: return "?";
    }
}

// IEEE 802.11 FCS: CRC-32 over the frame, sent least significant byte first.
void appendFcs(std::vector<uint8_t>& f) { putLe(f, wifi::crc32(f.data(), f.size()), 4); }

// True when the last four bytes are the CRC-32 of the rest.
bool fcsOk(const uint8_t* p, size_t n) {
    if (n < 6) return false;
    const uint32_t c = wifi::crc32(p, n - 4);
    const uint32_t r = (uint32_t)p[n - 4] | (uint32_t)p[n - 3] << 8 | (uint32_t)p[n - 2] << 16 | (uint32_t)p[n - 1] << 24;
    return c == r;
}

// S1G beacon (9.3.4.? extension frame, subtype 1): FC(2), duration(2), SA(6), timestamp(4), change sequence(1), [next TBTT(3)], [compressed SSID(4)], [ANO(1)], elements
std::vector<uint8_t> makeS1gBeacon(const BssSpec& b) {
    std::vector<uint8_t> f;
    f.push_back(0x1C);   // version 0, type 3 (extension), subtype 1 (S1G beacon)
    f.push_back(beaconFlags(b));
    putLe(f, 0, 2);      // duration
    f.insert(f.end(), b.bssid, b.bssid + 6);
    putLe(f, b.timestamp, 4);
    f.push_back(b.changeSeq);
    if (b.compressedSsid) putLe(f, wifi::crc32(reinterpret_cast<const uint8_t*>(b.ssid.data()), b.ssid.size()), 4);
    if (b.ano) f.push_back(0);
    addBodyIes(f, b, b.fullSsid, true);
    appendFcs(f);
    return f;
}

// The short form: the fixed fields and the compressed SSID, no elements.
std::vector<uint8_t> makeShortBeacon(const BssSpec& b) {
    BssSpec s = b;
    s.compressedSsid = true;
    std::vector<uint8_t> f;
    f.push_back(0x1C);
    f.push_back(beaconFlags(s));
    putLe(f, 0, 2);
    f.insert(f.end(), s.bssid, s.bssid + 6);
    putLe(f, s.timestamp, 4);
    f.push_back(s.changeSeq);
    putLe(f, wifi::crc32(reinterpret_cast<const uint8_t*>(s.ssid.data()), s.ssid.size()), 4);
    if (s.ano) f.push_back(0);
    appendFcs(f);
    return f;
}

// management header + fixed fields shared by the beacon (subtype 8) and the probe response (subtype 5)
static std::vector<uint8_t> mgmtBody(const BssSpec& b, int subtype, const uint8_t* da) {
    std::vector<uint8_t> f;
    f.push_back((uint8_t)(subtype << 4)); f.push_back(0x00);
    putLe(f, 0, 2);
    static const uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    f.insert(f.end(), da ? da : bc, (da ? da : bc) + 6);
    f.insert(f.end(), b.bssid, b.bssid + 6);
    f.insert(f.end(), b.bssid, b.bssid + 6);
    putLe(f, 0, 2);
    putLe(f, b.timestamp, 4); putLe(f, 0, 4);
    putLe(f, (uint32_t)b.intervalTu, 2);
    putLe(f, b.security != Open ? 0x0011 : 0x0001, 2);
    addBodyIes(f, b, true, false);
    appendFcs(f);
    return f;
}

std::vector<uint8_t> makeMgmtBeacon(const BssSpec& b) { return mgmtBody(b, 8, nullptr); }
std::vector<uint8_t> makeProbeResponse(const BssSpec& b, const uint8_t* client) { return mgmtBody(b, 5, client); }

// A probe request for an SSID ("" asks for any).
std::vector<uint8_t> makeProbeRequest(const uint8_t sta[6], const std::string& ssid) {
    std::vector<uint8_t> f = {0x40, 0x00, 0x00, 0x00};
    static const uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    f.insert(f.end(), bc, bc + 6);
    f.insert(f.end(), sta, sta + 6);
    f.insert(f.end(), bc, bc + 6);
    putLe(f, 0, 2);
    addIe(f, 0, std::vector<uint8_t>(ssid.begin(), ssid.end()));
    appendFcs(f);
    return f;
}

// A data frame to the AP with a dummy payload.
std::vector<uint8_t> makeDataFrame(const uint8_t bssid[6], const uint8_t sta[6], size_t payload) {
    std::vector<uint8_t> f = {0x08, 0x01, 0x00, 0x00};   // data, to DS
    f.insert(f.end(), bssid, bssid + 6);
    f.insert(f.end(), sta, sta + 6);
    f.insert(f.end(), bssid, bssid + 6);
    putLe(f, 0, 2);
    for (size_t i = 0; i < payload; i++) f.push_back((uint8_t)(i * 29 + 11));
    appendFcs(f);
    return f;
}

std::string macText(const uint8_t a[6]) { return wifi::macText(a); }
std::string compText(uint32_t c) { char b[16]; snprintf(b, sizeof b, "c:%08X", c); return b; }

namespace {

// the elements of a beacon, probe response or request body (9.4.2); unknown ones are skipped
void parseIes(const uint8_t* p, size_t n, Frame& f) {
    size_t i = 0;
    int akmBest = Open;
    while (i + 2 <= n) {
        const int id = p[i], len = p[i + 1];
        if (i + 2 + (size_t)len > n) break;
        const uint8_t* b = p + i + 2;
        if (id == 0) { f.hasSsid = true; f.ssid.assign(reinterpret_cast<const char*>(b), (size_t)len); }
        else if (id == 1 || id == 50) { for (int k = 0; k < len; k++) if (b[k] & 0x7F) f.ratesMbps.push_back((b[k] & 0x7F) * 0.5f); }
        else if (id == 5 && len >= 3) { f.hasTim = true; f.dtimCount = b[0]; f.dtimPeriod = b[1]; f.timBytes = len - 3; }
        else if (id == 7 && len >= 2) { f.hasCountry = true; f.country.assign(reinterpret_cast<const char*>(b), 2); }
        else if (id == 32 && len >= 1) f.txPowerDbm = b[0];
        else if (id == 221 && len >= 3) {   // vendor specific (9.4.2.24): the first three octets are the OUI
            char o[16]; snprintf(o, sizeof o, "%02X:%02X:%02X", b[0], b[1], b[2]);
            if (std::find(f.vendorIes.begin(), f.vendorIes.end(), o) == f.vendorIes.end() && f.vendorIes.size() < 8) f.vendorIes.push_back(o);
        }
        else if (id == 180) f.hasRaw = true;
        else if (id == 216) f.hasTwt = true;
        else if (id == 217 && len >= 1) { f.hasCap = true; f.longPreamble = b[0] & 1; }
        else if (id == 213 && len >= 4) { f.intervalTu = b[2] | b[3] << 8; }
        else if (id == 232 && len >= 4) {
            f.hasOp = true;
            const int code = b[0] >> 1 & 0xF;
            f.opWidthMhz = code >= 15 ? 16 : code >= 7 ? 8 : code >= 3 ? 4 : code >= 1 ? 2 : 1;
            f.primaryWidthMhz = (b[0] & 1) ? 1 : 2;
            f.opClass = b[1]; f.primary = b[2]; f.ccf = b[3];
            if (len >= 5) f.basicMcsOneSs = b[4] & 3;
        } else if (id == 48 && len >= 8) {
            f.hasRsn = true;
            size_t o = 2 + 4;
            auto u16 = [&](size_t at) { return at + 2 <= (size_t)len ? b[at] | b[at + 1] << 8 : 0; };
            const int pc = u16(o); o += 2 + 4 * (size_t)pc;
            const int ac = o + 2 <= (size_t)len ? u16(o) : 0; o += 2;
            int best = RsnOther;
            for (int s = 0; s < ac && o + 4 <= (size_t)len; s++, o += 4) {
                if (b[o] != 0x00 || b[o + 1] != 0x0F || b[o + 2] != 0xAC) continue;
                const int t = b[o + 3];
                const int k = (t == 8 || t == 9 || t == 24) ? Wpa3Sae : t == 18 ? Owe : (t == 2 || t == 4 || t == 6) ? Wpa2Psk : (t == 1 || t == 3 || t == 5 || t == 11 || t == 12) ? Wpa2Ent : RsnOther;
                const int rank[] = {0, 3, 5, 2, 4, 1, 0};   // Open, Wpa2Psk, Wpa3Sae, Owe, Wpa2Ent, RsnOther
                if (best == RsnOther || rank[k] > rank[best]) best = k;
            }
            akmBest = best;
        }
        i += 2 + (size_t)len;
    }
    if (f.hasRsn) f.security = akmBest;
    else f.security = f.secBit ? Secured : Open;
}

}

// Reads a frame (without its FCS) into Frame; false when it is too short to be one.
bool parseFrame(const uint8_t* p, size_t n, Frame& f) {
    f = Frame();
    if (n < 2) return false;
    f.type = p[0] >> 2 & 3;
    f.subtype = p[0] >> 4;
    if (f.type == 3 && f.subtype == 1) {   // S1G beacon
        if (n < 15) return false;
        f.s1gBeacon = true;
        f.secBit = p[1] >> 6 & 1;
        f.bwInd = p[1] >> 3 & 7;
        memcpy(f.bssid, p + 4, 6);
        memcpy(f.sa, p + 4, 6);
        f.hasAddrs = true;
        f.timestamp = (uint32_t)p[10] | (uint32_t)p[11] << 8 | (uint32_t)p[12] << 16 | (uint32_t)p[13] << 24;
        size_t o = 15;
        if (p[1] & 1) o += 3;
        if (p[1] & 2) {
            if (o + 4 > n) return false;
            f.hasComp = true;
            f.compSsid = (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24;
            o += 4;
        }
        if (p[1] & 4) o += 1;
        if (o > n) return false;
        f.shortBeacon = o == n;
        parseIes(p + o, n - o, f);
        char b[96];
        if (f.shortBeacon) snprintf(b, sizeof b, "S1G short beacon");
        else snprintf(b, sizeof b, "S1G beacon%s%s", f.hasSsid ? " " : "", f.hasSsid ? (f.ssid.empty() ? "(hidden)" : f.ssid.c_str()) : "");
        f.summary = b;
        return true;
    }
    if (f.type == 0 && (f.subtype == 5 || f.subtype == 8)) {   // probe response, beacon: 24 byte header, timestamp 8, interval 2, capability 2
        if (n < 36) return false;
        f.probeResp = f.subtype == 5; f.mgmtBeacon = f.subtype == 8;
        memcpy(f.da, p + 4, 6); memcpy(f.sa, p + 10, 6); memcpy(f.bssid, p + 16, 6);
        f.hasAddrs = true;
        f.timestamp = (uint32_t)p[24] | (uint32_t)p[25] << 8 | (uint32_t)p[26] << 16 | (uint32_t)p[27] << 24;
        f.intervalTu = p[32] | p[33] << 8;
        f.secBit = (p[34] | p[35] << 8) & 0x10;
        parseIes(p + 36, n - 36, f);
        f.summary = f.probeResp ? "probe response" : "beacon";
        if (f.hasSsid) { f.summary += " "; f.summary += f.ssid.empty() ? "(hidden)" : f.ssid; }
        return true;
    }
    static const char* mg[] = {"assoc req", "assoc resp", "reassoc req", "reassoc resp", "probe req", "probe resp", "timing adv", "", "beacon", "ATIM", "disassoc", "auth", "deauth", "action", "action no-ack", ""};
    char b[96];
    if (f.type == 0) {
        if (n >= 24) { memcpy(f.da, p + 4, 6); memcpy(f.sa, p + 10, 6); memcpy(f.bssid, p + 16, 6); f.hasAddrs = true; }
        snprintf(b, sizeof b, "%s", f.subtype < 16 && mg[f.subtype][0] ? mg[f.subtype] : "management");
        if (f.subtype == 4 && n >= 24) {   // probe request: the SSID asked for
            f.probeReq = true;
            parseIes(p + 24, n - 24, f);
            if (f.hasSsid) snprintf(b, sizeof b, "probe req %s", f.ssid.empty() ? "(any)" : f.ssid.c_str());
        } else if ((f.subtype == 10 || f.subtype == 12) && n >= 26) snprintf(b, sizeof b, "%s, reason %d", mg[f.subtype], p[24] | p[25] << 8);
    } else if (f.type == 1) {
        static const char* ct[] = {"control 0", "control 1", "control 2", "control 3", "control 4", "control 5", "control 6", "control wrapper", "block ack req", "block ack", "PS-Poll", "RTS", "CTS", "ACK", "CF-End", "CF-End+Ack"};
        snprintf(b, sizeof b, "%s", ct[f.subtype & 15]);
    } else if (f.type == 2) {   // data: the addresses by the To DS / From DS bits
        const int toDs = p[1] & 1, fromDs = p[1] >> 1 & 1;
        f.protectedFrame = p[1] >> 6 & 1;
        f.qos = f.subtype & 8;
        snprintf(b, sizeof b, "%s%s%s", f.qos ? "QoS data" : "data", f.protectedFrame ? " (protected)" : "", f.subtype & 4 ? " null" : "");
        if (n >= 22) {
            const uint8_t *a1 = p + 4, *a2 = p + 10, *a3 = p + 16;
            if (!toDs && !fromDs) { memcpy(f.da, a1, 6); memcpy(f.sa, a2, 6); memcpy(f.bssid, a3, 6); }
            else if (toDs && !fromDs) { memcpy(f.bssid, a1, 6); memcpy(f.sa, a2, 6); memcpy(f.da, a3, 6); }
            else if (!toDs && fromDs) { memcpy(f.da, a1, 6); memcpy(f.bssid, a2, 6); memcpy(f.sa, a3, 6); }
            else { memcpy(f.bssid, a1, 6); memcpy(f.da, a3, 6); }
            f.hasAddrs = true;
        }
    } else snprintf(b, sizeof b, "S1G extension %d", f.subtype);
    f.summary = b;
    return true;
}

} // namespace dect2::halow
