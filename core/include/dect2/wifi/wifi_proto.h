// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi (IEEE 802.11) pieces of the network monitor: 2.4 GHz channels, the DSSS PHY's scrambler, PLCP header and CRCs, and the
// management frames access points announce themselves with (beacons, probe responses and, on 6 GHz, FILS discovery). Built from IEEE 802.11-2020
// clause 15 (DSSS) and 9.3.3 / 9.4.2 (management frames and elements); the capability and operation elements are in wifi_elems.h, the channel plans
// of all three bands in wifi_chan.h, the OFDM PHY in wifi_ofdm.h. Only beacons, probe responses and FILS discovery are parsed; other frames are only classified.
#pragma once
#include "dect2/wifi/wifi_elems.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::wifi {

constexpr double kChipRate = 11e6;       // Barker-11 chips per second
constexpr double kSymRate = 1e6;         // 1 Mbit/s DBPSK: one symbol per microsecond
constexpr uint16_t kSfd = 0xF3A0;        // long preamble start frame delimiter (sent LSB first)
constexpr uint8_t kSignal1M = 0x0A;      // PLCP SIGNAL: 1 Mbit/s
constexpr uint8_t kSignal2M = 0x14;      // PLCP SIGNAL: 2 Mbit/s (DQPSK)
constexpr uint16_t kSfdShort = 0x05CF;   // short preamble start frame delimiter (sent LSB first; the long one reversed in time)
constexpr int kSyncShortBits = 56;       // short preamble SYNC: scrambled zeros
constexpr int kSyncBits = 128;           // long preamble SYNC: scrambled ones
constexpr int kMaxMpdu = 2346;           // the longest DSSS MPDU, FCS included
extern const int kBarker[11];            // +1 -1 +1 +1 -1 +1 +1 +1 -1 -1 -1

double channelMhz(int ch);               // 1 .. 13: 2407 + 5 ch, 14: 2484; 0 otherwise
int channelOf(double mhz);               // the 2.4 GHz channel within 1 MHz of mhz, 0 none

uint32_t crc32(const uint8_t* p, size_t n);                    // the FCS (IEEE 802.3 CRC-32)
uint16_t crc16(const uint8_t* bits, size_t n);                 // PLCP header CRC-16 over bits in transmit order (preset ones, inverted)

// The whole long-preamble PPDU at 1 Mbit/s as the bits that DBPSK carries (scrambled, transmit order): SYNC, SFD, header, MPDU.
// mpdu includes its FCS.
std::vector<uint8_t> ppduBits(const std::vector<uint8_t>& mpdu, uint8_t seed = 0x1B);
// The same at 1 or 2 Mbit/s with the long or the short preamble (clause 15.2 / 16.2): bits[i] for i < qpskFrom are sent one to a DBPSK symbol, the
// rest two to a DQPSK symbol (d0 first; 00 -> 0, 01 -> +pi/2, 11 -> pi, 10 -> -pi/2). Long: SYNC, SFD and header at 1 Mbit/s, the MPDU at the rate.
// Short (2 Mbit/s only): 56 SYNC zeros and the short SFD at 1 Mbit/s, header and MPDU at 2 Mbit/s.
struct DsssPpdu { std::vector<uint8_t> bits; size_t qpskFrom = 0; };
DsssPpdu dsssPpdu(const std::vector<uint8_t>& mpdu, int rateMbps, bool shortPreamble, uint8_t seed = 0x1B);

enum Security { Open = 0, Wep, Wpa, WpaWpa2, Wpa2, Wpa2Wpa3, Wpa3, Owe };
const char* securityName(int s, bool enterprise = false);

// An access point's announcement, for the test signal
struct ApSpec {
    uint8_t bssid[6] = {};
    std::string ssid;
    bool hidden = false;                 // an empty SSID element
    int channel = 6;                     // DS parameter set
    int interval = 100;                  // TU (1.024 ms)
    int security = Wpa2;
    bool enterprise = false;
    bool gRates = true;                  // 6 - 54 Mbit/s besides 1, 2, 5.5 and 11
    uint64_t tsf = 0;
    uint16_t seq = 0;
    // 5 / 6 GHz and the newer standards (the test signal; wifi_mk.cpp writes the elements)
    int band = 0;                        // 0 2.4 GHz, 1 5 GHz, 2 6 GHz (OFDM rates only, no DS parameter set)
    bool ht = false, vht = false, he = false, eht = false;   // capability and operation elements to send
    int width = 20;                      // BSS channel width, MHz (the primary stays 20 MHz)
    int streams = 2;                     // spatial streams announced
    int bssColor = -1;                   // HE operation: BSS color (-1: no HE operation element color 0 disabled)
    bool twt = false;                    // HE capabilities: TWT responder
    std::string country;                 // "DE" (+ environment letter); empty: no country element
    int powerConstraint = -1;
    int mdid = -1;                       // mobility domain id, -1 none
    int he6Reg = 1;                      // 6 GHz regulatory info: 0 indoor, 1 standard power, 2 very low power
    bool he6Dup = false;
    std::vector<Neighbor> rnr;           // Reduced Neighbor Report entries (co-located 6 GHz APs on a 2.4 / 5 GHz beacon)
};
void appendModernElements(std::vector<uint8_t>& f, const ApSpec& s);   // wifi_mk.cpp: country, HT / VHT / HE / EHT, mobility domain, RNR
std::vector<uint8_t> makeFilsDiscovery(const ApSpec& s);               // a Public Action FILS Discovery frame, FCS appended (wifi_mk.cpp)
std::vector<uint8_t> makeBeacon(const ApSpec& s, bool probeResponse = false, const uint8_t* client = nullptr);   // FCS appended
std::vector<uint8_t> makeDataFrame(const uint8_t bssid[6], const uint8_t sta[6], size_t payload);               // FCS appended, payload random-ish
void appendFcs(std::vector<uint8_t>& f);

// What a frame says. n excludes the FCS (checked by the caller).
struct Frame {
    int type = -1, subtype = -1;         // 0 management, 1 control, 2 data
    bool beacon = false, probeResp = false;
    uint8_t addr1[6] = {}, addr2[6] = {}, bssid[6] = {};
    std::string ssid;
    bool hidden = false;
    int channel = 0;                     // DS parameter set (0: not sent)
    int interval = 0;                    // TU
    uint16_t cap = 0;
    uint64_t tsf = 0;
    std::vector<int> rates;              // in 500 kbit/s units, | 0x80 for a basic rate
    int security = Open;
    bool enterprise = false;
    std::string ciphers;                 // "CCMP", "TKIP+CCMP" ...
    std::string summary;
    Elems el;                            // capabilities / operation (wifi_elems.h), combined for the band
    bool fils = false;                   // a FILS Discovery frame (6 GHz)
    bool unsolicited = false;            // a probe response to the broadcast address: an unsolicited probe response
    uint32_t shortSsid = 0;              // FILS: the short SSID
};
// band: 0 / 1 / 2 = 2.4 / 5 / 6 GHz (the radio's channel), -1 unknown; it decides how the elements are combined into standard and width
bool parseFrame(const uint8_t* p, size_t n, Frame& f, int band = -1);   // false: too short to say anything
std::string ratesText(const std::vector<int>& r);        // "1 2 5.5 11 6 9 12 18 24 36 48 54"
std::string macText(const uint8_t a[6]);
const char* vendorOf(const uint8_t a[6]);                // the OUI table (wifi_oui.cpp); "" unknown, "local" for a locally administered address
const char* ouiVendor(uint32_t oui);                     // the vendor of the first three octets (24 bits), "" unknown
const char* frameTypeName(int type, int subtype);

} // namespace dect2::wifi
