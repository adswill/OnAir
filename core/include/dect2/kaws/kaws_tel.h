// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS receiver telemetry. RxTelemetry::kaws, engine standard 135, active standard 134 (family 133), poll slot 136.
// KAWS is an amateur HF weak-signal mode (2.4 kHz of audio, chat / beacons / voice). Simulation-only: nothing here has been tested on a real HF path.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct KawsChat {
    std::string from, to, text;
    double utc = 0;            // seconds since the epoch (computer clock) when the message completed
    int profile = 0;
    float snrDb = 0;
    bool firstMessage = false; // the full recipient callsign was sent
};

struct KawsBeaconEntry {
    std::string call, grid, text;
    bool full = false;         // a full beacon (position, text) instead of the short one
    double lat = 0, lon = 0;   // full beacon only
    double powerDbm = 0;       // EIRP of the sender as it announced it
    double utc = 0;
    int profile = 0;
    float snrDb = 0;
};

struct KawsTelemetry {
    uint64_t seq = 0;
    // 0 no input, 1 listening (no burst yet or none for 5 minutes), 2 receiving (a block passed its CRC within the last 5 minutes).
    int state = 0;
    double inputRate = 0, timeSec = 0;
    double utcNow = 0;         // the receiver's UTC (seconds since the epoch) when this report was made: the slot grid comes from this clock
    bool clockSet = false;     // false: the system clock of the computer, true: a clock given by the caller (tests)
    bool dataValid = false;    // blocks have passed their CRC
    // the last good burst
    int profile = -1;          // 0 .. 5 (-1 none yet)
    float snrDb = -99;         // estimate in 2.4 kHz of the last burst that was detected
    float cfoHz = 0;
    double clockErrSec = 0;    // how far the burst began from slot start + 1.5 s (positive = late)
    double lastGoodAgeSec = 0;
    // the woven-lattice signature (the preamble): the last try on each profile's slot grid
    bool latticeSeen[8] = {};
    float latticeStat[8] = {};     // detection statistic of that try
    double latticeAgeSec[8] = {};  // how long ago that try was (stream time), -1 never
    bool syncLock = false;         // a lattice was found on some grid within the last two slots of that grid
    uint64_t slotsTried = 0;       // slot windows searched, all profiles
    uint64_t latticeHits = 0;
    uint64_t blocksOk = 0, blocksBad = 0;
    uint64_t messages = 0, beacons = 0, dataFrames = 0;
    uint64_t profileBlocks[8] = {};
    bool voicePresent = false;     // reserved for the voice profiles P6 / P7: never set by this receiver (see voiceNote)
    std::string voiceNote;         // plain words about voice
    std::vector<float> spectrum;   // audio spectrum 0 .. 3000 Hz, 60 bins, dB relative to the strongest bin (0 = top, down to -60)
    std::vector<KawsChat> chats;           // newest first, up to 64
    std::vector<KawsBeaconEntry> beaconList;   // newest first, up to 64
};

inline std::string kawsSummary(const KawsTelemetry& t) {
    char b[200];
    if (t.state == 0) return "KAWS: waiting for samples";
    if (t.state == 1) return t.syncLock ? "KAWS: preamble found" : "KAWS: listening";
    snprintf(b, sizeof b, "KAWS: P%d, SNR %.0f dB, %llu messages", t.profile, (double)t.snrDb, (unsigned long long)t.messages);
    return b;
}

} // namespace dect2
