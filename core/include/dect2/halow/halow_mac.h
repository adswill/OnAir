// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) MAC frames as far as the PHY gives bytes: the S1G beacon (frame control, SA, timestamp, change sequence, compressed SSID,
// information elements), the management beacon, probe response and request, data frames, the FCS, and the information elements a monitor lists:
// SSID, RSN (security shown for information only), S1G Operation and Capabilities, TIM, Country, power constraint, supported rates, RAW parameters
// and TWT. IEEE 802.11-2020 clause 9 (9.3.4 S1G beacon, 9.4.2 information elements). Written from the published description of the standard;
// element body layouts that could not be checked are marked "unverified" in halow_mac.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::halow {

enum Security { Open = 0, Wpa2Psk, Wpa3Sae, Owe, Wpa2Ent, RsnOther, Secured };
const char* securityName(int s);

struct BssSpec {
    uint8_t bssid[6] = {};
    std::string ssid;
    bool fullSsid = true;                // the SSID element in the beacon body (else only its 32-bit compressed form)
    bool compressedSsid = true;
    int bwInd = 0;                       // beacon bandwidth indication: 0 1 MHz, 1 2 MHz, 2 4 MHz, 3 8 MHz, 4 16 MHz
    int security = Wpa2Psk;
    int opClass = 6;                     // S1G Operation: operating class, primary channel number, channel centre frequency number, width
    int primary = 1;
    int ccf = 1;
    int opWidthMhz = 1;
    int primaryWidthMhz = 1;             // 1 or 2
    int intervalTu = 100;
    uint32_t timestamp = 0;
    uint8_t changeSeq = 0;
    bool ano = false;
    // optional elements
    std::string country;                 // "US", "EU" ...: a Country element when not empty
    bool tim = false;                    // TIM with this DTIM period
    int dtimPeriod = 1;
    bool raw = false;                    // RAW parameter set
    bool twt = false;                    // TWT element
    int txPowerDbm = -1;                 // power constraint element when >= 0
    bool longPreamble = true;            // S1G Capabilities: long preamble supported
    int maxMcsOneSs = 2;                 // basic MCS map, one stream: 0 MCS 0-2, 1 MCS 0-7, 2 MCS 0-9
    uint8_t vendorOui[3] = {};           // a vendor specific element (221) with this OUI when not all zero (Morse Micro and others add one)
    bool rates = false;                  // supported rates element (management frames; S1G beacons carry none)
};
void appendFcs(std::vector<uint8_t>& f);
bool fcsOk(const uint8_t* p, size_t n);                          // n includes the 4 byte FCS
std::vector<uint8_t> makeS1gBeacon(const BssSpec& b);            // FCS appended
std::vector<uint8_t> makeShortBeacon(const BssSpec& b);          // S1G beacon without information elements, compressed SSID only
std::vector<uint8_t> makeMgmtBeacon(const BssSpec& b);           // the full management beacon (subtype 8) sent at a low rate
std::vector<uint8_t> makeProbeResponse(const BssSpec& b, const uint8_t* client);   // a management frame; FCS appended
std::vector<uint8_t> makeProbeRequest(const uint8_t sta[6], const std::string& ssid);
std::vector<uint8_t> makeDataFrame(const uint8_t bssid[6], const uint8_t sta[6], size_t payload);

struct Frame {
    int type = -1, subtype = -1;
    bool s1gBeacon = false, probeResp = false, mgmtBeacon = false, probeReq = false, shortBeacon = false;
    uint8_t bssid[6] = {};
    uint8_t sa[6] = {}, da[6] = {};
    bool hasAddrs = false;
    bool hasSsid = false;
    std::string ssid;
    bool hasComp = false;
    uint32_t compSsid = 0;
    int bwInd = -1;                      // from the frame control of an S1G beacon
    bool secBit = false;
    int security = Open;
    bool hasRsn = false;
    bool hasOp = false;
    int opClass = 0, primary = 0, ccf = 0, opWidthMhz = 0, primaryWidthMhz = 0;
    int basicMcsOneSs = -1;              // 0 MCS 0-2, 1 MCS 0-7, 2 MCS 0-9, 3 none (-1: not announced)
    bool hasCap = false, longPreamble = false;
    int intervalTu = 0;
    uint32_t timestamp = 0;
    bool hasCountry = false;
    std::string country;
    bool hasTim = false;
    int dtimCount = 0, dtimPeriod = 0, timBytes = 0;
    bool hasRaw = false, hasTwt = false;
    int txPowerDbm = -1;                 // power constraint
    std::vector<float> ratesMbps;        // supported rates
    std::vector<std::string> vendorIes;  // OUIs "AA:BB:CC" of the vendor specific elements (221), the Wi-Fi Alliance's 50:6F:9A and the Microsoft 00:50:F2 ones included
    bool protectedFrame = false, qos = false;
    std::string summary;
};
bool parseFrame(const uint8_t* p, size_t n, Frame& f);           // n excludes the FCS (the caller checks it)
std::string macText(const uint8_t a[6]);
std::string compText(uint32_t c);                                // "c:1A2B3C4D"

} // namespace dect2::halow
