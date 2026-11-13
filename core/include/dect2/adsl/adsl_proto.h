// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-L 4 SRD-860 (EASA, Issue 2, December 2025): the air format of the European light-aviation iConspicuity link. Written from the EASA specification;
// the OGN / SoftRF ads-l.h (GPL-3) was read to settle what the specification leaves open (see docs/ADSL-3.8.md).
//   M-band   868.2 / 868.4 MHz   GFSK BT 0.5, +-50 kHz, 100 kchip/s, Manchester (1 = chips 01) over sync and packet; preamble not Manchester coded.
//   O-LDR    869.525 MHz         GFSK BT 1.0, +-12.5 kHz, 38.4 kchip/s, no Manchester, a fixed legacy header, the packet, CRC-8.
//   O-HDR    869.525 MHz         GMSK BT 0.5, 200 kchip/s, no Manchester.
// Packet = length, network header (version, signature flag, key index, parity type), ADS-L data, parity. The data is XXTEA "scrambled" with the published
// all-zero key 0 (or not at all with key index 3); indices 1 and 2 are reserved, so no secret key exists yet and none is asked for.
// Integers are little endian, bit offsets count from the least significant bit; the CRC-24 (the Mode S polynomial) goes on air most significant byte first.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace adsl {

enum Band { BandM0 = 0, BandM1 = 1, BandLdr = 2, BandHdr = 3, kNumBands = 4 };
constexpr double kChannelHz[kNumBands] = {868.2e6, 868.4e6, 869.525e6, 869.525e6};
constexpr double kChipRate[kNumBands] = {100e3, 100e3, 38400.0, 200e3};
constexpr double kDeviationHz[kNumBands] = {50e3, 50e3, 12500.0, 50e3};
constexpr double kGaussBt[kNumBands] = {0.5, 0.5, 1.0, 0.5};

constexpr uint32_t kSyncMBand = 0xF5724Bu;    // after Manchester decoding: four preamble 1s, the 0101 of the "1001 1001" tail, sync 0x72 0x4B
constexpr uint32_t kSyncLdr = 0xAAB42Bu;      // eight preamble chips, sync 0xB4 0x2B
constexpr uint32_t kSyncHdr = 0xAA2DD4u;      // eight preamble chips, sync 0x2D 0xD4
constexpr int kSyncBits = 24;
constexpr int kLdrHeaderBytes = 6;
constexpr uint8_t kLdrHeader[kLdrHeaderBytes] = {0x00, 0x00, 0x00, 0x00, 0x18, 0x71};
constexpr int kTrafficDataBytes = 20;         // 5 header + 15 payload
constexpr int kTrafficNetBytes = 24;          // network header + data + CRC: the Length byte of a Traffic packet
constexpr int kStatusNetBytes = 16;
constexpr int kMaxTargets = 10;               // Traffic Uplink
constexpr int kRsParity = 32;

enum PayloadType { TypeTraffic = 2, TypeStatus = 3, TypeUplink = 4, TypeFisB = 5, TypeRemoteId = 6, TypeOgnDiag = 66 };
enum Amt { AmtRandom = 0, AmtIcao = 5, AmtFlarm = 6, AmtOgn = 7, AmtFanet = 8 };

uint32_t crc24(const uint8_t* d, size_t n);   // the specification's routine: the 3 low bytes
uint8_t crc8(const uint8_t* d, size_t n);     // O-band LDR, start 0x71

// Reed-Solomon of ADS-L: GF(256) with 0x11D, primitive element 11, first root 121, 32 parity bytes; a short block is padded with leading zeros to 223
void rsEncode(const uint8_t* data, size_t n, uint8_t parity[kRsParity]);
int rsDecode(uint8_t* block, size_t n);       // n = data + 32 parity bytes, corrected in place; returns the bytes fixed, or -1

// Exponential fields. N is the width of the base; signed fields have one more (sign) bit on top.
uint32_t expDecode(uint32_t raw, int N);
uint32_t expEncode(uint32_t value, int N);    // clamps to the largest code below the all-ones "invalid" one

struct Traffic {
    int amt = AmtFlarm;
    uint32_t addr = 0;                        // 24 bits
    bool relay = false;
    int timeQ = 0;                            // quarter seconds, 0 - 59
    int flightState = 0;                      // 0 undefined, 1 ground, 2 airborne
    int category = 0;                         // Table G.1.3
    int emergency = 1;                        // Table G.1.4
    bool hasPos = false; double lat = 0, lon = 0;
    bool hasAlt = false; int altM = 0;        // metres above the WGS-84 ellipsoid
    bool hasSpeed = false; double speedMs = 0;
    bool hasClimb = false; double climbMs = 0;
    bool hasTrack = false; double trackDeg = 0;   // false when the speed is zero or invalid
    int sil = 0, design = 0, nic = 0, hAcc = 0, vAcc = 0, velAcc = 0;
};

struct Status {
    int amt = 0; uint32_t addr = 0;
    int maxVersion = 0, mRx = 0, ldrRx = 0, hdrRx = 0, mobile = 0;
    int econRx = 0, econTx = 0, xpdr = 0;
    bool trafficClient = false, fisClient = false;
};

// Traffic (15 bytes) and Status (7 bytes) payloads
void packTraffic(const Traffic& t, uint8_t payload[15]);
void unpackTraffic(const uint8_t payload[15], int amt, uint32_t addr, bool relay, Traffic& out);
void packStatus(const Status& s, uint8_t payload[7]);
void unpackStatus(const uint8_t payload[7], int amt, uint32_t addr, Status& out);

// ADS-L Data = type, sender address, payload (clear text)
std::vector<uint8_t> makeData(int type, int amt, uint32_t addr, bool relay, const uint8_t* payload, size_t n);
// Network packet = header, scrambled data, parity. keyIndex 0 scrambles, 3 does not. fec: Reed-Solomon parity instead of the CRC.
std::vector<uint8_t> makeNetwork(const std::vector<uint8_t>& data, int version, int keyIndex, bool fec);

struct Parsed {
    enum Kind { Bad, Reserved, Encrypted, TrafficMsg, StatusMsg, UplinkMsg, Other } kind = Bad;
    int version = 0, keyIndex = 0, type = 0;
    bool signature = false, fec = false, relay = false;
    int amt = 0; uint32_t addr = 0;
    std::vector<Traffic> targets;             // one for Traffic, up to ten for Traffic Uplink
    Status status;
    int fixedBytes = 0;
};
// A network packet (net[0] = network header) whose parity already checked
Parsed parseNetwork(const uint8_t* net, size_t n);
// The check itself: a good CRC, or Reed-Solomon when the header asks for it (corrects net in place). Returns the bytes corrected, or -1.
int checkParity(uint8_t* net, size_t n);
// Try every error pattern over the given weakest bits of a Traffic-sized CRC packet, each with one more free bit; net is left corrected. Bits flipped, or -1.
int correctCrc(uint8_t* net, const int* weakBits, int nWeak);

// Whole frames, sync word first, as chips (0 / 1), preamble included. net = the network packet. preambleRepeats: -1 for the default.
std::vector<uint8_t> frameChips(Band band, const std::vector<uint8_t>& net, int preambleRepeats = -1);

const char* categoryName(int c);
const char* emergencyName(int e);
const char* amtName(int amt);

} // namespace adsl
} // namespace dect2
