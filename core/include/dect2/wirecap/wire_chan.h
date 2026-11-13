// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wireshark output: the table of modes that can send packets to Wireshark. Header only and without dependencies, because the extcap program
// (tools/onair_extcap.cpp) is a small stand-alone binary that needs the same names and link types. See docs/WIRESHARK-3.8.md.
#pragma once
#include <cstdint>
#include <cstring>

namespace dect2::wire {

enum Chan : int { Wifi, Halow, Zigbee, Thread, WiSun, Ble, Zwave, LoRaWan, DvbIp, AdsB, Ais, Aprs, Asterix, GsmR, Tetra, Lte, kChanCount };

// How the bytes given to Hub::publish reach Wireshark.
enum class Encap : uint8_t {
    Direct,    // written as they are, under the channel's link type
    Gsmtap,    // GSMTAP bytes: sent over UDP as they are; in a file or a live capture inside IPv4 + UDP (port 4729), link type RAW
    UdpPort,   // application data: inside IPv4 + UDP to udpPort, link type RAW, so that the dissector of that port reads it
};

constexpr uint32_t kLinkAx25 = 3, kLinkRaw = 101, kLinkRadiotap = 127, kLinkUser0 = 147, kLinkUser1 = 148, kLink154Fcs = 195, kLink154NoFcs = 230, kLinkBleLl = 251, kLinkLoraTap = 270;
constexpr uint16_t kGsmtapPort = 4729, kNmeaPort = 10110, kAsterixPort = 8600;
constexpr int kDefaultStreamPort = 47290;   // the local TCP port between OnAir and its extcap program

struct ChanInfo {
    const char* id;          // file names and the extcap interface ("onair_" + id)
    const char* display;     // the mode's name in the Wireshark window and in the interface list
    uint32_t linktype;       // what a file or live capture of this channel is written as
    const char* linkName;    // the link type's DLT name
    Encap encap;
    uint16_t udpPort;        // Encap::Gsmtap and Encap::UdpPort
    const char* dissector;   // what reads it in Wireshark
};

inline constexpr ChanInfo kChans[kChanCount] = {
    {"wifi",     "Wi-Fi",               kLinkRadiotap, "IEEE802_11_RADIO",     Encap::Direct,  0,            "radiotap, wlan"},
    {"halow",    "Wi-Fi HaLow",         kLinkRadiotap, "IEEE802_11_RADIO",     Encap::Direct,  0,            "radiotap, wlan"},
    {"zigbee",   "Zigbee / 802.15.4",   kLink154Fcs,   "IEEE802_15_4_WITHFCS", Encap::Direct,  0,            "wpan, zbee"},
    {"thread",   "Thread",              kLink154Fcs,   "IEEE802_15_4_WITHFCS", Encap::Direct,  0,            "wpan, 6lowpan, mle"},
    {"wisun",    "Wi-SUN",              kLink154NoFcs, "IEEE802_15_4_NOFCS",   Encap::Direct,  0,            "wpan, wisun"},
    {"ble",      "Bluetooth LE",        kLinkBleLl,    "BLUETOOTH_LE_LL",      Encap::Direct,  0,            "btle"},
    {"zwave",    "Z-Wave",              kLinkUser1,    "USER1",                Encap::Direct,  0,            "onair.lua (no built-in dissector)"},
    {"lorawan",  "LoRaWAN",             kLinkLoraTap,  "LORATAP",              Encap::Direct,  0,            "loratap, lorawan"},
    {"dvbip",    "DVB-S/S2 IP (GSE, MPE, ULE)", kLinkRaw, "RAW",               Encap::Direct,  0,            "ip, ipv6"},
    {"adsb",     "ADS-B / Mode S",      kLinkUser0,    "USER0",                Encap::Direct,  0,            "onair.lua (no built-in dissector)"},
    {"ais",      "AIS",                 kLinkRaw,      "RAW",                  Encap::UdpPort, kNmeaPort,    "nmea0183 (port added by onair.lua, or Decode As)"},
    {"aprs",     "APRS / AX.25",        kLinkAx25,     "AX25",                 Encap::Direct,  0,            "ax25, aprs"},
    {"asterix",  "ASTERIX",             kLinkRaw,      "RAW",                  Encap::UdpPort, kAsterixPort, "asterix"},
    {"gsmr",     "GSM-R",               kLinkRaw,      "RAW",                  Encap::Gsmtap,  kGsmtapPort,  "gsmtap, gsm_a_ccch"},
    {"tetra",    "TETRA",               kLinkRaw,      "RAW",                  Encap::Gsmtap,  kGsmtapPort,  "gsmtap, tetra"},
    {"lte",      "LTE cell broadcast",  kLinkRaw,      "RAW",                  Encap::Gsmtap,  kGsmtapPort,  "gsmtap, lte-rrc"},
};

inline int chanById(const char* id) {
    for (int i = 0; i < kChanCount; i++) if (!std::strcmp(kChans[i].id, id)) return i;
    return -1;
}

} // namespace dect2::wire
