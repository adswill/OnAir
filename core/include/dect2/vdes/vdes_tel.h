// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDES (VHF Data Exchange System, ITU-R M.2092) telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
//
// What is decoded and what is only detected:
//  - ASM (application specific messages, GMSK 9.6 kbit/s, 25 kHz): fully demodulated, HDLC with bit stuffing and CRC-16 checked (the AIS physical layer).
//    The payload is shown as raw bits / hex; AIS-style fields are shown only when the first bits look like an AIS message type.
//  - VDE-TER (25 / 50 / 100 kHz channels at 157.2 and 161.8 MHz): bursts are detected and logged with level, bandwidth and a modulation class
//    (pi/4-QPSK, 8PSK, 16QAM) from the symbol statistics; the frame content is not decoded.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kVdesAsmMax = 200, kVdeBurstsMax = 300;
constexpr double kVdesAsm1Hz = 161.950e6, kVdesAsm2Hz = 162.000e6;   // the ASM channels 2027 and 2028 (ITU-R M.2092, RR Appendix 18)
constexpr double kVdeBandHz[2] = {157.2e6, 161.8e6};                 // centres of the 100 kHz VDE-TER channels
constexpr double kVdesDefaultDialHz = 161.9e6;                       // assumed for a file without a tuned frequency

enum VdeMod { VdeModUnknown = 0, VdeModPi4Qpsk = 1, VdeMod8Psk = 2, VdeMod16Qam = 3 };
inline const char* vdeModName(int m) {
    switch (m) { case VdeModPi4Qpsk: return "pi/4-QPSK"; case VdeMod8Psk: return "8PSK"; case VdeMod16Qam: return "16QAM"; default: return "unknown"; }
}

struct VdesAsm {
    double timeSec = 0;
    int64_t wallTime = 0;           // the computer's clock (0: a file)
    char channel = 'A';             // 'A' = ASM 1 (161.950 MHz), 'B' = ASM 2 (162.000 MHz)
    int bits = 0;                   // payload length in bits, without the CRC
    std::string hex;                // the payload, MSB first, last byte padded with zeros
    int aisType = 0;                // first six bits read as an AIS message type, 0 when they are not 1 .. 27
    uint32_t aisMmsi = 0;           // bits 8 .. 37 read as an MMSI (shown only for a plausible AIS type)
    float snrDb = 0;
    float cfoHz = 0;
    // the position, when the payload is an AIS message with one (types 1 - 3, 9, 18, 19, 21, 27 ...): decoded with the AIS message layer
    bool hasPos = false;
    double lat = 0, lon = 0;
    float sogKt = -1, cogDeg = -1;
};

struct VdeBurst {
    double timeSec = 0;             // start of the burst
    int64_t wallTime = 0;
    int band = 0;                   // 0: 157.2 MHz, 1: 161.8 MHz
    double centreMhz = 0;           // centre of the occupied channel
    int bwKhz = 25;                 // 25, 50 or 100 (occupied 25 kHz sub-channels)
    int mod = VdeModUnknown;        // VdeMod
    float rssiDb = -120;            // mean power during the burst, dBFS
    float snrDb = 0;                // above the sub-channel noise floor, in the occupied bandwidth
    float durMs = 0;
    float cfoHz = 0;                // carrier offset from the channel centre (spectral centroid)
    float cv = 0;                   // spread of the symbol amplitudes (classifier input)
    float psi = 0;                  // mean cos(4 x phase step): -1 pi/4-QPSK, 0 8PSK, +1 QPSK (classifier input)
    int nSym = 0;                   // symbols the class is based on
};

struct VdesTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 searching, 1 energy seen (VDE-TER bursts or failed ASM) lately, 2 ASM frames decoded in the last 30 s
    double cfoHz = 0;               // carrier offset of the last ASM burst
    float snrDb = 0;                // SNR of the last ASM burst
    bool dataValid = false;         // an ASM frame was decoded in the last 30 s
    uint64_t blocksOk = 0;          // ASM frames with a good CRC
    uint64_t blocksBad = 0;         // ASM bursts that failed their checks

    double timeSec = 0, inputRate = 0, dialMhz = 0;
    bool asmInView = false;
    bool bandInView[2] = {false, false};
    uint64_t asmOk[2] = {}, asmBad[2] = {};
    float asmLevelDb[2] = {-120, -120}, asmNoiseDb[2] = {-120, -120};
    uint64_t vdeBursts = 0;
    uint64_t vdeByMod[4] = {};
    uint64_t vdeByBw[3] = {};       // 25, 50, 100 kHz
    float vdeLevelDb[2] = {-120, -120}, vdeNoiseDb[2] = {-120, -120};
    double lastVdeSec = -1e9;
    std::vector<VdesAsm> asms;      // newest first
    std::vector<VdeBurst> bursts;   // newest first
};

inline std::string vdesSummary(const VdesTelemetry& t) {
    char b[160];
    if (t.blocksOk || t.vdeBursts) {
        snprintf(b, sizeof b, "%llu ASM frames, %llu VDE-TER bursts", (unsigned long long)t.blocksOk, (unsigned long long)t.vdeBursts);
        return b;
    }
    return t.state ? "energy seen, nothing decoded" : "listening";
}

} // namespace dect2
