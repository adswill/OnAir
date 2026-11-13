// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw) monitor telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kLrLogMax = 500;

// What the user sets (plain LoRa between two radios: nothing is assumed about the payload)
struct LoraRawSettings {
    bool autoSf = true;            // search SF7 - SF12
    int sf = 7;                    // 5..12 when not auto
    bool autoBw = true;            // search 125, 250 and 500 kHz
    int bwKhz = 125;               // 125, 250 or 500 (also 62 for 62.5 kHz)
    int cr = 5;                    // implicit header only: 5..8 for 4/5..4/8 (explicit frames carry their own)
    bool crc = true;               // implicit header only: a payload CRC follows
    bool implicit = false;         // implicit header: length, rate and CRC are not sent, the settings above and the length below apply
    int length = 16;               // implicit header only: payload bytes
    int sync = 0x12;               // sync word (0x12 private networks, 0x34 LoRaWAN, 0x2B Meshtastic ...), -1: try 0x12, 0x34 and 0x2B
    int preamble = 8;              // symbols (only places the frame start)
    int ldro = -1;                 // low data rate optimisation: -1 automatic (symbol >= 16 ms), 0 off, 1 on
    double offsetHz = 0;           // the channel's offset from the tuned frequency
};

struct LrFrame {
    int64_t wallTime = 0;
    double sec = 0;                // signal time of the frame's start
    int sf = 0;
    double bwHz = 0;
    int cr = 0;                    // 5..8
    bool hasCrc = false;           // the frame carries a payload CRC
    bool crcOk = false;
    bool implicit = false;
    int sync = 0;                  // the sync word searched
    int len = 0;
    int corrected = 0;             // codewords with a corrected bit
    float snrDb = 0, levelDb = 0;  // SNR in the LoRa bandwidth, level dB re full scale
    double cfoHz = 0;              // carrier offset from the channel centre
    double freqHz = 0;             // the channel centre plus the offset, offset from the tuned frequency
    std::vector<uint8_t> payload;
    std::string hex, ascii;
};

struct LrDecoderInfo {
    int sf = 0;
    double bwHz = 0;
    int sync = 0;
    bool inBand = false;
    uint64_t preambles = 0, frames = 0, crcBad = 0, headerBad = 0, syncBad = 0;
};

struct LoraRawTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 nothing heard, 1 frames heard in the last seconds
    double cfoHz = 0;              // carrier offset of the last good frame
    float snrDb = 0;               // SNR of the last good frame
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0;         // frames with a good CRC (or no CRC)
    uint64_t blocksBad = 0;        // frames whose CRC failed
    // details
    double timeSec = 0, inputRate = 0;
    LoraRawSettings cfg;
    uint64_t preambles = 0, headerBad = 0;
    uint64_t perSf[13] = {};       // good frames by spreading factor
    uint64_t perBw[3] = {};        // by 125, 250, 500 kHz
    std::vector<LrDecoderInfo> decoders;
    std::vector<LrFrame> log;      // newest first, at most kLrLogMax
};

inline std::string loraRawSummary(const LoraRawTelemetry& t) {
    char b[160];
    if (t.blocksOk + t.blocksBad == 0) {
        size_t n = 0;
        for (const auto& d : t.decoders) if (d.inBand) n++;
        if (t.cfg.autoSf || t.cfg.autoBw) snprintf(b, sizeof b, "searching %zu LoRa settings", n);
        else snprintf(b, sizeof b, "listening SF%d %d kHz", t.cfg.sf, t.cfg.bwKhz);
        return b;
    }
    snprintf(b, sizeof b, "%llu frames, %llu CRC errors", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
