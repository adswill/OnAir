// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth LE advertising: the link layer pieces of the advertising physical channel (Core spec vol 6 part B) and the advertising data
// structures (Core Specification Supplement part A), as nRF Connect and other scanner apps show them. Receive side only: nothing here
// connects, pairs or answers; a CONNECT_IND is shown as a connection request and never followed.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace ble {

constexpr uint32_t kAdvAccess = 0x8E89BED6;   // the access address of every advertising channel packet
constexpr uint32_t kAdvCrcInit = 0x555555;
constexpr double kSymRate = 1e6;             // LE 1M PHY
constexpr double kDevHz = 250e3;             // modulation index 0.5
constexpr int kMaxPdu = 2 + 255;             // header + the longest payload

double channelMhz(int ch);                   // RF channel index 0 to 39 -> MHz (37 2402, 38 2426, 39 2480)
int advChannelOf(double mhz);                // 37, 38 or 39 when mhz is within 0.1 MHz of one, else 0

// CRC-24 of the bytes as sent (LSB first): the value's low byte is the first CRC byte on the air
uint32_t crc24(const uint8_t* d, size_t n, uint32_t init = kAdvCrcInit);
// Data whitening for the RF channel index (0 to 39): the same call whitens and dewhitens
void whiten(uint8_t* d, size_t n, int channel);

// PDU types of the advertising physical channel (legacy and the extended ADV_EXT_IND)
enum PduType { AdvInd = 0, AdvDirectInd = 1, AdvNonconnInd = 2, ScanReq = 3, ScanRsp = 4, ConnectInd = 5, AdvScanInd = 6, AdvExtInd = 7 };
const char* pduTypeName(int t);               // "ADV_IND" ...
const char* pduTypeText(int t);               // "connectable", "scan request", "connection request" ...
const char* companyName(int id);              // Bluetooth SIG company identifier -> name (a short table), nullptr unknown
const char* uuid16Name(int u);                // a few common 16-bit service UUIDs, nullptr unknown
const char* appleTypeName(int t);             // the type byte of Apple's manufacturer data (Nearby Info, Find My ...), nullptr unknown

// 0 public, 1 random static, 2 resolvable private (changes every few minutes), 3 non-resolvable private, 4 random (reserved type)
int addrKind(bool random, const uint8_t a[6]);
const char* addrKindName(int k);
std::string addrText(const uint8_t a[6]);     // "C0:FF:EE:12:34:56" (the order apps show: most significant byte first)

// One advertising channel PDU, decoded
struct Adv {
    int type = -1;
    bool chSel = false, txRandom = false, rxRandom = false;
    int len = 0;
    bool hasAdv = false, hasPeer = false;
    uint8_t adv[6] = {}, peer[6] = {};        // AdvA; peer: ScanA (SCAN_REQ), InitA (CONNECT_IND), TargetA (ADV_DIRECT_IND)
    std::vector<uint8_t> data;                // AdvData / ScanRspData (and an extended packet's own AdvData)
    // advertising data
    std::string name; bool nameComplete = false;
    int flags = -1, txPower = 127, appearance = -1;
    std::vector<std::string> services;        // "Battery (0x180F)", 32 and 128-bit UUIDs as text
    int company = -1;                         // manufacturer data: company identifier
    std::vector<uint8_t> mfr;                 // manufacturer data after the company identifier
    std::string vendor;                       // "Apple: Nearby Info", "Microsoft" ...
    std::string beacon;                       // iBeacon / Eddystone / exposure notification, as text
    // CONNECT_IND (shown only, never followed)
    uint32_t connAa = 0; int connInterval = 0;
    // ADV_EXT_IND: the common extended header
    int advMode = -1, auxChannel = -1, auxPhy = -1, adiSid = -1, adiDid = -1;
    std::string summary;                      // one line for the packet log
};
bool parsePdu(const uint8_t* pdu, size_t n, Adv& out);     // n = 2 + length; false when malformed
void parseAd(const uint8_t* d, size_t n, Adv& out);        // the AD structures (length, type, data)
std::string eddystoneUrl(const uint8_t* d, size_t n);      // scheme byte + encoded URL -> text

// ---- building packets (the test signal)
struct AdBuilder {
    std::vector<uint8_t> b;
    AdBuilder& add(int type, const std::vector<uint8_t>& v);
    AdBuilder& flags(int f) { return add(0x01, {(uint8_t)f}); }
    AdBuilder& name(const std::string& s, bool complete = true);
    AdBuilder& txPower(int dbm) { return add(0x0A, {(uint8_t)(int8_t)dbm}); }
    AdBuilder& uuid16(const std::vector<int>& u);
    AdBuilder& mfr(int company, const std::vector<uint8_t>& v);
    AdBuilder& serviceData16(int uuid, const std::vector<uint8_t>& v);
    AdBuilder& ibeacon(const uint8_t uuid[16], int major, int minor, int power);
    AdBuilder& eddystoneUrl(int power, int scheme, const std::string& encoded);   // encoded: the URL with its expansion bytes
    AdBuilder& eddystoneUid(int power, const uint8_t ns[10], const uint8_t inst[6]);
    AdBuilder& eddystoneTlm(int mv, double tempC, uint32_t advCount, uint32_t secTenths);
    AdBuilder& exposure(const uint8_t rpi[16], const uint8_t meta[4]);
};
// header + payload: the PDU before whitening; payload already holds AdvA and the rest
std::vector<uint8_t> makePdu(int type, bool txRandom, const std::vector<uint8_t>& payload, bool rxRandom = false);
std::vector<uint8_t> advPayload(const uint8_t adv[6], const std::vector<uint8_t>& data);   // AdvA + data
// The bits on the air, LSB first: preamble, access address, whitened PDU and CRC
void packetBits(const std::vector<uint8_t>& pdu, int channel, std::vector<uint8_t>& bits);

} // namespace ble
} // namespace dect2
