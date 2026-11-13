// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Information elements of 802.11 beacons, probe responses and FILS discovery frames that say what an access point can do: HT / VHT / HE / EHT
// capabilities and operation (the channel width and primary channel), the 6 GHz operation information, the Reduced Neighbor Report (co-located
// 6 GHz APs announced on 2.4 / 5 GHz), RSN key management (AKM suites, management frame protection), country, power constraint and mobility domain.
// Clauses: IEEE 802.11-2020 9.4.2 (elements) and 9.6.7.36 (FILS Discovery), IEEE 802.11ax-2021 9.4.2.248 - 9.4.2.249 (HE), 9.4.2.170 (RNR),
// IEEE 802.11be-2024 9.4.2.312 - 9.4.2.313 (EHT). Parsing is bounds-checked: a truncated element is skipped, never over-read.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::wifi {

// One entry of a Reduced Neighbor Report: an AP on another channel (802.11-2020 9.4.2.170.2, TBTT Information field)
struct Neighbor {
    int opClass = 0;            // operating class: 131 - 137 are 6 GHz (width in wifi_chan.h)
    int channel = 0;
    int band = -1;              // from the operating class
    bool hasBssid = false;
    uint8_t bssid[6] = {};
    bool hasShortSsid = false;
    uint32_t shortSsid = 0;     // CRC-32 of the SSID
    int params = -1;            // BSS parameters octet (-1: not sent)
    bool sameSsid = false;      // bit 1
    bool coLocated = false;     // bit 6: co-located with the reporting AP
    bool unsolicitedProbe = false;   // bit 5: sends unsolicited probe responses on that channel
    bool memberOfEss24or5 = false;   // bit 4: a 6 GHz AP with a co-located 2.4 / 5 GHz AP
};

struct Elems {
    // HT (802.11-2020 9.4.2.55 / 9.4.2.56)
    bool ht = false, ht40 = false, htSgi = false, htOp = false;
    int htStreams = 0, htPrimary = 0, htSecOffset = 0;   // 0 none, 1 above, 3 below
    bool htOpWide = false;      // HT Operation: the STA channel width bit (any width)
    int htMaxMbps = 0;
    // VHT (9.4.2.157 / 9.4.2.158)
    bool vht = false, vhtOp = false, vhtSgi80 = false, vhtSuBfer = false, vhtMuBfer = false;
    int vhtWidthSet = 0;        // 0: up to 80 MHz, 1: 160 MHz, 2: 160 and 80 + 80 MHz
    int vhtStreams = 0, vhtMaxMcs = 0;
    int vhtOpWidth = 0, vhtC0 = 0, vhtC1 = 0;   // VHT Operation: 0 20/40, 1 80/160/80+80, 2 160, 3 80+80; centre channels
    int vhtMaxMbps = 0;
    // HE (802.11ax 9.4.2.248 / 9.4.2.249)
    bool he = false, heOp = false, heTwtReq = false, heTwtResp = false, heTwtRequired = false, bssColorDisabled = false;
    int heWidthBits = 0, heStreams = 0, heMaxMcs = 0, bssColor = -1;
    int heMaxMbps = 0;
    bool he6 = false;           // 6 GHz Operation Information present
    int he6Primary = 0, he6Width = 0, he6C0 = 0, he6C1 = 0, he6Reg = -1;   // width 20 / 40 / 80 / 160; reg: 0 indoor, 1 standard power, 2 very low power
    bool he6Dup = false;        // duplicate beacons on the 20 MHz channels of a wider BSS
    // EHT (802.11be 9.4.2.313 / 9.4.2.312)
    bool eht = false, ehtOp = false;
    int ehtWidth = 0, ehtC0 = 0, ehtC1 = 0;   // MHz 20 / 40 / 80 / 160 / 320
    // others
    std::string country;        // "DE" plus the environment letter (I indoor, O outdoor, space any), e.g. "DE I"
    int powerConstraint = -1;   // dB below the regulatory maximum
    bool mobility = false;
    int mdid = 0;
    int rsnVersion = 0;
    std::string akms;           // "SAE, PSK" ...
    int pmf = 0;                // 0 none, 1 capable, 2 required
    std::vector<Neighbor> rnr;
    // the result of putting it together
    int primary = 0;            // primary channel from HT / HE 6 GHz operation (0 not announced)
    int widthMhz = 20;
    int centreChannel = 0;      // CCFS0 of the widest operation element (0: the primary)
    std::string standard;       // "a", "n", "ac", "ax", "be" (2.4 GHz: "b/g" ...): the newest and the ones below it, e.g. "n/ac/ax"
};

// Parses the element list in p[0 .. n) into e (the standard / width come from combine()). Returns the number of elements read.
int parseElements(const uint8_t* p, size_t n, Elems& e);
// Sets e.primary, e.widthMhz, e.centreChannel and e.standard once all elements are in; band: 0 / 1 / 2 (2.4 / 5 / 6 GHz) or -1 unknown
void combineElems(Elems& e, int band);
// Plain words: "HE 2x2, 80 MHz, up to 1201 Mbit/s, TWT, BSS colour 5"
std::string capsText(const Elems& e);
// Max PHY rate of one stream (Mbit/s x 10 to stay integer): rows for HT (MCS 7), VHT (MCS 8 / 9), HE (MCS 9 / 11), from the subcarrier counts in the standard
int htRateX10(int widthMhz, bool sgi);                    // MCS 7, one stream
int vhtRateX10(int widthMhz, bool sgi, int maxMcs);       // MCS 7 / 8 / 9, one stream
int heRateX10(int widthMhz, int maxMcs);                  // MCS 7 / 9 / 11, one stream, 0.8 us guard interval
uint32_t shortSsid(const std::string& ssid);              // CRC-32 of the SSID (802.11ax 9.4.2.170.3)
std::string fmtShortSsid(uint32_t s);

// A FILS Discovery frame (802.11-2020 9.6.7.36): Public Action, category 4 / action 34, sent every 20 TU on 6 GHz preferred scanning channels
struct FilsInfo {
    bool valid = false;
    std::string ssid;
    bool hasShort = false;
    uint32_t shortSsid = 0;
    int interval = 0;           // beacon interval, TU
    uint64_t tsf = 0;
    uint16_t control = 0;
};
// body: the frame body after the 24-byte MAC header (category first). false: not a FILS discovery frame or too short
bool parseFils(const uint8_t* body, size_t n, FilsInfo& f);

} // namespace dect2::wifi
