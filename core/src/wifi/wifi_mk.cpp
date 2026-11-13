// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Builders for the elements that 2.4 / 5 / 6 GHz access points announce their abilities with, and the FILS Discovery frame: the test signal's
// side of wifi_elems.cpp (the layouts are those of IEEE 802.11-2020 9.4.2, 802.11ax-2021 9.4.2.248 - 9.4.2.249 and 802.11be-2024 9.4.2.312 - 9.4.2.313).
// Nothing is transmitted; the bytes feed the generator and the tests.
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cstring>

namespace dect2::wifi {

namespace {
void put16(std::vector<uint8_t>& v, unsigned x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void element(std::vector<uint8_t>& f, int id, const std::vector<uint8_t>& body) { f.push_back((uint8_t)id); f.push_back((uint8_t)body.size()); f.insert(f.end(), body.begin(), body.end()); }
void ext(std::vector<uint8_t>& f, int extId, std::vector<uint8_t> body) { body.insert(body.begin(), (uint8_t)extId); element(f, 255, body); }
int widthCode(int w) { return w >= 320 ? 4 : w >= 160 ? 3 : w >= 80 ? 2 : w >= 40 ? 1 : 0; }
// 2 bits per stream, stream 1 first: value v for the streams in use, 3 (not supported) for the others
unsigned mcsMapFor(int streams, unsigned v) { unsigned m = 0; for (int n = 0; n < 8; n++) m |= (n < streams ? v : 3u) << (2 * n); return m; }
} // namespace

// Country, power constraint, mobility domain, HT / VHT / HE / EHT capability and operation elements, and the RNR, for the spec's band and width.
void appendModernElements(std::vector<uint8_t>& f, const ApSpec& s) {
    const int primary = s.channel;
    const int centre = s.width > 20 ? blockCentreChannel(s.band, primary, s.width) : 0;
    if (!s.country.empty()) {   // country string (3), one triplet: first channel, number of channels, max power (dBm)
        std::vector<uint8_t> e(3, ' ');
        for (size_t i = 0; i < 3 && i < s.country.size(); i++) e[i] = (uint8_t)s.country[i];
        e.push_back((uint8_t)primary); e.push_back(1); e.push_back(20);
        element(f, 7, e);
    }
    if (s.powerConstraint >= 0) element(f, 32, {(uint8_t)s.powerConstraint});
    if (s.mdid >= 0) element(f, 54, {(uint8_t)s.mdid, (uint8_t)(s.mdid >> 8), 0x01});
    if (s.ht) {   // HT Capabilities (26): capability info, A-MPDU parameters, supported MCS set (Rx masks for MCS 0 - 31 first), extended, beamforming, ASEL
        std::vector<uint8_t> e;
        put16(e, (s.width >= 40 ? 0x0002u : 0u) | 0x0020u | (s.width >= 40 ? 0x0040u : 0u));
        e.push_back(0x17);
        for (int k = 0; k < 16; k++) e.push_back(k < 4 && k < s.streams ? 0xFF : 0);
        e.insert(e.end(), 7, 0);
        element(f, 45, e);
        // HT Operation (22): primary channel, B0-1 secondary offset, B2 STA channel width, then 20 octets of the rest
        std::vector<uint8_t> o;
        o.push_back((uint8_t)primary);
        const int off = s.width >= 40 ? (centre > primary ? 1 : 3) : 0;
        o.push_back((uint8_t)(off | (s.width >= 40 ? 4 : 0)));
        o.insert(o.end(), 20, 0);
        element(f, 61, o);
    }
    if (s.vht && s.band != 2) {   // VHT Capabilities (12) and Operation (5): the 5 GHz band only
        std::vector<uint8_t> e;
        const uint32_t ci = (uint32_t)(s.width >= 160 ? 1 : 0) << 2 | 1u << 5;
        put16(e, ci & 0xFFFF); put16(e, ci >> 16);
        put16(e, mcsMapFor(s.streams, 2)); put16(e, 0); put16(e, mcsMapFor(s.streams, 2)); put16(e, 0);
        element(f, 191, e);
        std::vector<uint8_t> o;
        const int c80 = s.width >= 160 ? blockCentreChannel(s.band, primary, 80) : centre;
        o.push_back((uint8_t)(s.width >= 80 ? 1 : 0));
        o.push_back((uint8_t)(s.width >= 80 ? c80 : 0));
        o.push_back((uint8_t)(s.width >= 160 ? centre : 0));
        put16(o, mcsMapFor(s.streams, 2));
        element(f, 192, o);
    }
    if (s.he) {
        // HE Capabilities: MAC (6), PHY (11), Rx MCS / NSS <= 80 MHz (2), Tx (2). PHY octet 0 B1 40 MHz in 2.4, B2 40 / 80 and B3 160 in 5 and 6 GHz
        std::vector<uint8_t> e(6, 0);
        if (s.twt) e[0] |= 0x04;   // TWT responder
        std::vector<uint8_t> phy(11, 0);
        if (s.band == 0) { if (s.width >= 40) phy[0] |= 0x02; }
        else { if (s.width >= 80) phy[0] |= 0x04; if (s.width >= 160) phy[0] |= 0x08; }
        e.insert(e.end(), phy.begin(), phy.end());
        put16(e, mcsMapFor(s.streams, 2)); put16(e, mcsMapFor(s.streams, 2));
        ext(f, 35, e);
        // HE Operation: parameters (3; B17 = 6 GHz operation information present), BSS color (1), basic MCS / NSS (2), 6 GHz information (5)
        std::vector<uint8_t> o;
        const unsigned par = s.band == 2 ? 1u << 17 : 0;
        o.push_back((uint8_t)par); o.push_back((uint8_t)(par >> 8)); o.push_back((uint8_t)(par >> 16));
        o.push_back(s.bssColor >= 0 ? (uint8_t)(s.bssColor & 0x3F) : (uint8_t)0x80);
        put16(o, 0xFFFC);
        if (s.band == 2) {
            const int c0 = s.width >= 160 ? blockCentreChannel(s.band, primary, 80) : s.width > 20 ? centre : 0;
            o.push_back((uint8_t)primary);
            o.push_back((uint8_t)((s.width >= 160 ? 3 : s.width >= 80 ? 2 : s.width >= 40 ? 1 : 0) | (s.he6Dup ? 4 : 0) | (s.he6Reg & 7) << 3));
            o.push_back((uint8_t)(s.width > 20 ? c0 : primary));
            o.push_back((uint8_t)(s.width >= 160 ? centre : 0));
            o.push_back(0);
        }
        ext(f, 36, o);
    }
    if (s.eht) {
        ext(f, 108, std::vector<uint8_t>(15, 0));   // EHT Capabilities: MAC (2), PHY (9), supported EHT-MCS / NSS (4)
        std::vector<uint8_t> o = {0x01, 0, 0, 0, 0};   // parameters (B0: operation information present), basic EHT-MCS / NSS (4)
        o.push_back((uint8_t)widthCode(s.width));
        o.push_back((uint8_t)(s.width > 20 ? (s.width >= 160 ? blockCentreChannel(s.band, primary, s.width >= 320 ? 160 : 80) : centre) : primary));
        o.push_back((uint8_t)(s.width >= 160 ? centre : 0));
        ext(f, 106, o);
    }
    if (!s.rnr.empty()) {   // RNR: one Neighbor AP Information field per neighbour, TBTT information 12 octets: offset, BSSID, short SSID, BSS parameters
        std::vector<uint8_t> e;
        for (const Neighbor& n : s.rnr) {
            put16(e, 0u | 12u << 8);   // type 0, one TBTT information set, length 12
            e.push_back((uint8_t)n.opClass); e.push_back((uint8_t)n.channel);
            e.push_back(0xFF);   // TBTT offset: unknown
            e.insert(e.end(), n.bssid, n.bssid + 6);
            for (int k = 0; k < 4; k++) e.push_back((uint8_t)(n.shortSsid >> (8 * k)));
            e.push_back((uint8_t)((n.sameSsid ? 2 : 0) | (n.memberOfEss24or5 ? 16 : 0) | (n.unsolicitedProbe ? 32 : 0) | (n.coLocated ? 64 : 0)));
        }
        element(f, 201, e);
    }
}

// FILS Discovery (9.6.7.36): Public Action (category 4), action 34, FD frame control (B0-4: SSID length - 1), timestamp, beacon interval, SSID
std::vector<uint8_t> makeFilsDiscovery(const ApSpec& s) {
    std::vector<uint8_t> f = {0xD0, 0x00, 0, 0};   // management, action
    f.insert(f.end(), 6, 0xFF);
    f.insert(f.end(), s.bssid, s.bssid + 6);
    f.insert(f.end(), s.bssid, s.bssid + 6);
    put16(f, (unsigned)(s.seq & 0xFFF) << 4);
    f.push_back(4); f.push_back(34);
    put16(f, (unsigned)((std::max<size_t>(1, s.ssid.size()) - 1) & 0x1F));
    for (int k = 0; k < 8; k++) f.push_back((uint8_t)(s.tsf >> (8 * k)));
    put16(f, (unsigned)s.interval);
    f.insert(f.end(), s.ssid.begin(), s.ssid.end());
    if (s.ssid.empty()) f.push_back(0);
    appendFcs(f);
    return f;
}

} // namespace dect2::wifi
