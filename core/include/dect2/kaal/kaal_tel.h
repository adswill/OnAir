// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL receiver telemetry. RxTelemetry::kaal, engine standard 124, active standard 123.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

struct KaalTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no input, 1 searching, 2 locked
    double cfoHz = 0;
    float snrDb = 0;
    float sweepDb = 0;             // level of the sweep signature
    float audioLevel = 0;          // 0 .. 1, peak of the decoded speech, decays
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // good and bad frames
    double inputRate = 0, timeSec = 0;
    int symRate = 8000;            // 8000 (12 / 16 kbit/s voice, about 10 kHz wide) or 16000 (24 / 32 kbit/s, about 20 kHz wide), from the width of the spectrum
    // the last valid beacon (station identity frame)
    bool beaconValid = false;
    uint64_t beacons = 0;          // valid beacon frames so far
    double beaconAgeSec = 0;       // seconds since the last valid frame
    std::string callsign, grid, text;
    double lat = 0, lon = 0, powerDbm = 0;
    int linkQuality = 0;
    // the codec profile of the last decoded frame (12, 16, 24 or 32 kbit/s, read from the frame header) and the station ID carried in the sweep
    int bitrateKbps = 0;
    bool sweepIdValid = false;
    std::string sweepId;           // callsign with suffix, e.g. LY2ABC/P
    float sweepIdGateDb = 0;
    // frame version 4 (docs/KAAL-NEXT.md): four slots, chirp sync, control channel
    int frameVersion = 3;          // 4 while the v4 receiver is locked
    int v4Profile[4] = {0, 0, 0, 0};   // kaal::v4::Profile of each slot (last decoded)
    uint64_t v4SlotsOk = 0, v4SlotsBad = 0, v4Texts = 0;
    std::string v4Call[4];         // the talker of each slot (control channel)
    int v4PlaySlot = -1;           // the voice slot being played, -1 none
};

inline std::string kaalSummary(const KaalTelemetry& t) {
    char b[200];
    if (t.state == 0) return "KAAL: waiting for samples";
    if (t.state == 1) return "KAAL: searching";
    if (t.frameVersion == 4) {
        snprintf(b, sizeof b, "KAAL v4: locked, %s Es/N0 %.0f dB", t.beaconValid ? t.callsign.c_str() : "4 slots", (double)t.snrDb);
        return b;
    }
    if (t.beaconValid) snprintf(b, sizeof b, "KAAL: locked, %s %s", t.callsign.c_str(), t.grid.c_str());
    else if (t.sweepIdValid) snprintf(b, sizeof b, "KAAL: locked, %s (sweep ID)", t.sweepId.c_str());
    else snprintf(b, sizeof b, "KAAL: locked, %d ksym/s, SNR %.0f dB", t.symRate / 1000, (double)t.snrDb);
    return b;
}

} // namespace dect2
