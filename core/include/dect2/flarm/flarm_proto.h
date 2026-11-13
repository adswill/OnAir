// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM air protocol "version 6" (the old, openly documented one: SoftRF / OGN) and the frame around it.
//  - on air: 868.2 and 868.4 MHz (Europe), 2-FSK +-50 kHz, 100 kchip/s Manchester coded; sync = Manchester(0xF531FAB6) (chips 55 99 A5 A9 55 66 65 96,
//    a data 1 is the chip pair 01); then 24 payload bytes and a CRC-16 (CCITT, 0xFFFF start) of them.
//  - payload word 0 (address, packet type, address type) is clear; words 1 - 5 are XXTEA (6 rounds) encrypted with a key made from the sender's
//    address and the time (64 s steps) and fixed constants. Packet type 0 = V6 position, 2 = the newer protocol (V7, counted only here).
// Privacy: stealth / no-track / anonymous aircraft are flagged here and dropped by the receiver before anything is stored (flarm_rx.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace flarm {

constexpr uint32_t kSyncWord = 0xF531FAB6u;     // after Manchester decoding
constexpr int kPayloadBytes = 24;
constexpr int kFrameBytes = 26;                 // payload + CRC
constexpr double kChipRate = 100000.0;
constexpr double kChannelHz[2] = {868.2e6, 868.4e6};

uint16_t crc16(const uint8_t* d, size_t n);     // CCITT, start 0xFFFF, MSB first

// XXTEA on n 32-bit words (n > 0 encrypt, n < 0 decrypt). rounds = 0: the standard 6 + 52 / n; FLARM uses 6.
void btea(uint32_t* v, int n, const uint32_t key[4], int rounds = 6);
uint32_t obscure(uint32_t key, uint32_t seed);
void makeV6Key(uint32_t key[4], uint32_t timestamp, uint32_t addrShifted);   // addrShifted = (addr << 8) & 0xFFFFFF

enum AddrType { AddrRandom = 0, AddrIcao = 1, AddrFlarm = 2, AddrAnonymous = 3 };

// The decrypted V6 position packet
struct V6 {
    uint32_t addr = 0;              // 24 bit device ID
    int type = 0;                   // 0
    int addrType = AddrFlarm;
    int vsRaw = 0;                  // 10 bit signed, units of 0.1 m/s >> smult
    bool airborne = true, stealth = false, noTrack = false;
    int gps = 323;
    int aircraftType = 1;
    uint32_t lat19 = 0, lon20 = 0;  // the position's low bits (units of 128 * 1e-7 degrees); needs a reference position
    uint32_t alt = 0;               // metres above the WGS84 ellipsoid, 13 bits
    int smult = 0;
    int8_t ns[4] = {}, ew[4] = {};  // four velocity samples, units of 0.25 m/s << smult (north / east)
};

// One packet's 24 bytes <-> fields. encode() encrypts (timestamp = unix seconds), decode() decrypts and checks the parity of the result.
void encodeV6(const V6& p, uint32_t timestamp, uint8_t out[kPayloadBytes]);
enum DecodeResult { DecOk = 0, DecNotV6, DecBadParity };
DecodeResult decodeV6(const uint8_t in[kPayloadBytes], uint32_t timestamp, V6& out);
int packetType(const uint8_t in[kPayloadBytes]);          // the clear header: 0 V6, 1 other, 2 newer protocol
uint32_t packetAddr(const uint8_t in[kPayloadBytes]);

// Position fields <-> degrees, around a reference position (valid within +- 3 degrees of latitude, +- 6 degrees of longitude)
void packPosition(double latDeg, double lonDeg, uint32_t& lat19, uint32_t& lon20);
void unpackPosition(uint32_t lat19, uint32_t lon20, double refLat, double refLon, double& latDeg, double& lonDeg);
// Ground speed (m/s), track (degrees), climb (m/s) from the velocity fields
void unpackVelocity(const V6& p, double& speedMs, double& trackDeg, double& climbMs);
void packVelocity(double speedMs, double trackDeg, double climbMs, V6& p);

const char* aircraftTypeName(int t);
const char* addrTypeName(int t);

// Frame <-> chips. chips: Manchester coded air bits (0 / 1), the sync word first. frameToChips takes 24 payload bytes and adds the CRC.
std::vector<uint8_t> manchester(const uint8_t* bytes, size_t n);                  // 1 -> 01, 0 -> 10, MSB first, one chip per element
std::vector<uint8_t> frameToChips(const uint8_t payload[kPayloadBytes], bool withSync = true);
bool checkCrc(const uint8_t frame[kFrameBytes]);          // either byte order of the CRC

} // namespace flarm
} // namespace dect2
