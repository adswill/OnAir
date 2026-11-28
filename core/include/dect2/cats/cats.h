// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
//
// CATS (Communication And Telemetry System, https://cats.radio): packet radio used by high-altitude balloons as an APRS/AX.25
// replacement. 2-FSK 9600 bd, deviation 4.8 kHz; header 4x0x55 + sync AB CD EF 12 + 16-bit length; payload = whiskers + CRC-16,
// whitened, LDPC (CCSDS TC/TM, rate 1/2), 32-bit block interleaved. Written from the CATS standard (gitlab.scd31.com/cats/cats-standard,
// MIT); the LDPC matrices and the chunking follow libCATS by Cam K. VE3KCN (BSD-2-Clause, see THIRD_PARTY.md).
#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace cats {

constexpr double kBaud = 9600.0;
constexpr double kDeviationHz = 4800.0;
constexpr int kMaxLen = 8191;

// A decoded packet: the fields of the whiskers that matter on a balloon track.
struct Packet {
    bool hasId = false;
    std::string callsign;
    int ssid = 0;
    int icon = 0;
    bool hasTime = false;
    uint64_t unixTime = 0;
    bool hasGps = false;
    double lat = 0, lon = 0, altM = 0, speedMps = 0, headingDeg = 0;
    int errM = 0;
    std::string comment;
    int whiskers = 0;      // whiskers seen, including the ones not decoded
    bool dataFecOk = true; // the CRC matched
};

// Packet building: whiskers appended in order. add* return false when the packet would pass 8191 bytes.
struct Builder {
    std::vector<uint8_t> semi;   // whiskers so far (no CRC)
    bool addId(const std::string& call, int ssid, int icon);
    bool addTime(uint64_t unixTime);
    bool addGps(double lat, double lon, double altM, int errM, double headingDeg, double speedMps);
    bool addComment(const std::string& text);
};

uint16_t crc16(const uint8_t* p, size_t n);   // IBM SDLC (init FFFF, reflected 8408, final xor)
uint16_t floatToHalf(float v);
float halfToFloat(uint16_t h);

// whiskers + CRC -> whiten -> LDPC -> interleave: the bytes after the 10-byte header
std::vector<uint8_t> encodeBody(const std::vector<uint8_t>& semi);
// the header (preamble, sync, length) + body: the bytes sent over the air
std::vector<uint8_t> encodeFrame(const std::vector<uint8_t>& semi);
// body -> Packet; false when the length trailer or the CRC does not match
bool decodeBody(const std::vector<uint8_t>& body, Packet& out);
bool parseWhiskers(const uint8_t* p, size_t n, Packet& out);

// 2-FSK baseband at fs (a multiple of 9600, at least 4 x): modulate a frame; demodulate a capture (any number of frames).
std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& frame, double fs, double offsetHz = 0);
std::vector<Packet> demodulate(const std::vector<std::complex<float>>& iq, double fs);

}  // namespace cats
}  // namespace dect2
