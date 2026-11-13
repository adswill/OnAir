// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct M17Position {
    int64_t wallTime = 0;
    std::string src;
    double lat = 0, lon = 0, altM = 0, speedKmh = 0;
    int bearing = 0;
    bool altValid = false, velValid = false;
    int station = 0;             // 0 fixed, 1 mobile, 2 handheld, 15 other
};

struct M17Message {
    int64_t wallTime = 0;
    std::string src, dst;
    std::string text;
    int can = 0;
    bool sms = true;             // a packet SMS; false: the text of an LSF META field
};

// One transmission (stream or packet), for the activity log and the "now" panel
struct M17Transmission {
    int carrierId = 0; double carrierHz = 0;   // whole-span decoding: the carrier it came from (id 0: single channel), its frequency relative to the span centre
    int64_t wallTime = 0;        // Unix time of its start
    double startSec = 0;         // signal seconds
    double durSec = 0;
    bool active = false;         // still on the air
    bool packet = false;
    std::string src, dst;        // "" until known (a late start: from the LICH)
    int can = -1;
    std::string type;            // "voice 3200", "voice + data 1600", "data", "packet"
    std::string enc;             // "" clear, else "AES-256", "scrambler 8-bit" ... (never decrypted)
    bool lsfFromLich = false;    // the LSF was missed: rebuilt from the LICH of the stream frames
    bool voice = false;          // Codec 2 audio was played
    std::string text;            // META text, or the SMS
    std::string ext1, ext2;      // extended callsign META (originator, reflector)
    bool hasGnss = false;
    double lat = 0, lon = 0;
    uint32_t frames = 0;         // stream or packet frames decoded
    uint32_t badFrames = 0;
    uint32_t lastFn = 0;
    float berPct = 0;            // channel bit errors corrected by the decoders (before FEC)
    std::string data;            // voice + data / data streams: the data bytes as text
};

struct M17Telemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 frames seen recently, 2 receiving a transmission
    double cfoHz = 0;            // carrier offset measured from the symbols
    float snrDb = 0;             // symbol SNR from the scatter around the four levels
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;      // receiving
    uint64_t blocksOk = 0;       // frames decoded
    uint64_t blocksBad = 0;      // frames that failed their check
    // details
    double timeSec = 0;
    double inputRate = 0;
    float deviationHz = 0;       // outer symbol deviation as measured (nominal 2400 Hz)
    bool inverted = false;       // the spectrum is mirrored (I and Q swapped): the symbols come upside down
    float berPct = 0;            // of the last frames
    uint64_t lsfOk = 0, lsfBad = 0, streamFrames = 0, packetFrames = 0, bertFrames = 0, packetsOk = 0, packetsBad = 0;
    uint64_t lichLsf = 0;        // LSFs rebuilt from the LICH
    uint64_t voiceFrames = 0;    // Codec 2 frames decoded to sound
    bool codec2 = false;         // libcodec2 found
    std::string codec2Path;
    M17Transmission now;         // the current (or last) transmission
    std::vector<M17Transmission> log;     // newest first, at most kM17LogMax
    std::vector<M17Message> messages;     // newest first
    std::vector<M17Position> positions;   // newest first
};

constexpr size_t kM17LogMax = 300;

inline std::string m17Summary(const M17Telemetry& t) {
    char b[200];
    const M17Transmission& n = t.now;
    if (n.active) {
        snprintf(b, sizeof b, "%s -> %s, %s%s%s, %.1f s", n.src.empty() ? "?" : n.src.c_str(), n.dst.empty() ? "?" : n.dst.c_str(), n.type.empty() ? "stream" : n.type.c_str(),
                 n.enc.empty() ? "" : ", encrypted ", n.enc.c_str(), n.durSec);
        return b;
    }
    if (t.blocksOk == 0) return "searching for M17";
    snprintf(b, sizeof b, "listening, %zu transmissions heard", t.log.size());
    return b;
}

} // namespace dect2
