// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell Waverider buoy HF link telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kDwBitsShown = 320;     // bits of the live bit stream shown
constexpr size_t kDwFrameMax = 40;
constexpr size_t kDwSymMax = 256;        // symbol means kept for the scatter plot
constexpr size_t kDwPsd = 256;           // columns of the spectrum plot

struct DwDisp { int h = 0, n = 0, w = 0; int fixed = 0; bool wrsg = false; };   // cm; wrsg: h and n are two vertical samples   // cm, repaired bits
struct DwSystem {
    bool havePos = false;
    int tn = 0, tolWeeks = 0, battery = 0;
    bool gpsFix = false, gpsModuleOk = false;
    double hrms = 0, hs = 0, tz = 0, psdMax = 0, tr = 0, tw = 0, av0 = 0, lat = 0, lon = 0, orientDeg = 0, inclDeg = 0;
    bool have[16] = {};
};
struct DwSpec { bool valid = false; double hz = 0, dirDeg = 0, spreadDeg = 0, m2 = 0, n2 = 0, k = 0, rpsd = 0, psd = 0; };

struct DwFrame {
    int64_t wall = 0;
    std::string hex;                     // the frame as received, bytes from the bit stream (bit order and polarity of the CRC attempt that fits best)
    bool crcOk = false;                  // the best CRC attempt accepts it (never a verified Datawell CRC: see crcName)
};

struct DatawellTelemetry {
    uint64_t seq = 0;
    int state = 0;                       // 0 nothing, 1 an FSK-like signal found, 2 symbols locked
    double cfoHz = 0;                    // unused
    float snrDb = 0;                     // of the signal in 100 Hz
    float levelDb = -200;                // input level, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;               // frames the best CRC attempt accepts
    uint64_t blocksBad = 0;              // frames cut at the found period that it does not accept
    // the carrier
    double timeSec = 0, inputRate = 0;
    double centerMhz = 0;                // where the radio is tuned (0: not known)
    double offsetHz = 0;                 // signal centre from the radio's tuned frequency
    double freqMhz = 0;                  // absolute (0 when the radio's frequency is not known)
    std::string channel;                 // the listed buoy channel within 6 kHz ("" none)
    float peakDb = 0;                    // peak over the noise floor, dB
    double bwHz = 0;                     // bandwidth holding 90% of the signal power
    std::string format;                  // "2FSK", "4MFSK" or "carrier / narrow tone": a guess from the bandwidth, never an identification
    bool formatForced = false;
    // the demodulator
    double baud = 0;                     // assumed (published) symbol rate
    double clockPpm = 0;                 // symbol clock offset measured against it
    double stepHz = 0;                   // measured tone shift (2FSK) or spacing (4MFSK)
    float symErr = 1;                    // rms distance of the symbol means to the nearest tone, as a fraction of the spacing
    uint64_t symbols = 0;
    std::string bits;                    // the last bits, '0' / '1', oldest first
    // the frames
    int periodBits = 0;                  // repetition of the bit stream found (0: none yet)
    double periodSec = 0;
    float periodZ = 0;                   // how many standard deviations above chance
    std::string syncHex;                 // the constant pattern at the start of every frame ("" none)
    int syncBits = 0;
    std::string crcName;                 // best CRC attempt ("" none)
    int crcPass = 0, crcTotal = 0;       // frames it accepts of those tried
    bool crcInverted = false, crcLsbFirst = false;
    std::vector<DwFrame> frames;         // newest first
    std::vector<uint8_t> psd;            // kDwPsd columns across psdSpanHz: 0 .. 255 = 0 .. 40 dB over the floor
    double psdSpanHz = 0;
    std::vector<float> symMeans;         // newest last, Hz from the signal centre
    // HXV (2FSK, DWR-MkIII / DWR-G / WR-SG) real-time format, decoded per the Datawell Reference Manual section 5.7 (datawell_hxv.h)
    bool hxvLocked = false;              // vector alignment found (three clean BCH vectors in a row)
    bool hxvInverted = false;            // the tones were taken the other way round
    uint64_t hxvVectors = 0, hxvClean = 0, hxvRepaired = 0, hxvLost = 0, hxvBlocks = 0;
    std::vector<DwDisp> disp;            // the last displacement samples, oldest first (1.28 Hz)
    DwSystem sys;                        // the system file words decoded so far
    DwSpec spec[64];                     // the directional spectrum points decoded so far
    int specCount = 0;
    int lastBlock = -1;
};

inline std::string datawellSummary(const DatawellTelemetry& t) {
    char b[200];
    if (t.state == 0) {
        if (t.centerMhz > 0) { snprintf(b, sizeof b, "listening on %.4f MHz for a buoy FSK link", t.centerMhz); return b; }
        return "listening for a buoy FSK link";
    }
    if (t.state == 1) { snprintf(b, sizeof b, "%s-like signal, %.0f Hz wide, %.0f dB", t.format.c_str(), t.bwHz, t.snrDb); return b; }
    if (t.hxvLocked) {
        if (t.sys.have[1]) snprintf(b, sizeof b, "HXV buoy data, Hs %.2f m, Tz %.1f s, %llu vectors, %llu repaired", t.sys.hs, t.sys.tz, (unsigned long long)t.hxvVectors, (unsigned long long)t.hxvRepaired);
        else snprintf(b, sizeof b, "HXV buoy data, %llu vectors, %llu repaired, waiting for the sea state words", (unsigned long long)t.hxvVectors, (unsigned long long)t.hxvRepaired);
        return b;
    }
    if (t.periodBits > 0) { snprintf(b, sizeof b, "%s, %.2f baud, frames of %d bits%s", t.format.c_str(), t.baud, t.periodBits, t.crcPass > 1 ? ", CRC attempt fits" : ""); return b; }
    snprintf(b, sizeof b, "%s, %.2f baud, symbols locked", t.format.c_str(), t.baud);
    return b;
}

} // namespace dect2
