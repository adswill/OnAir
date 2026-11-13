// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What the HF beacon propagation monitor reports (RxTelemetry::hfb, standard 69).
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// One history point of a beacon: a minute (Long Dashes, single letters, carriers) or one 10 s slot (NCDXF/IARU, one per 3 minutes)
struct HfbHistPt {
    double t = 0;               // UTC seconds
    float snr = NAN;            // dB in 100 Hz, NAN: listened but not heard
    uint8_t heard = 0;
};

struct HfbState {
    bool inSpan = false;        // the capture covers its frequency now
    bool heard = false;         // heard within the hold time of its kind
    int conf = 0;               // 0 nothing, 1 energy at the frequency but the keying not confirmed, 2 identity confirmed by the keying / schedule
    double lastHeard = 0;       // UTC seconds, 0 never
    float snr = NAN;            // of the latest detection, dB in 100 Hz
    float best1h = NAN, best24h = NAN;
    int steps = 0;              // NCDXF: power steps heard in the last slot (1 = 100 W only ... 4 = all, down to 0.1 W)
    float stepSnr[4] = {NAN, NAN, NAN, NAN};
    double distKm = NAN, bearing = NAN;   // from the receiver position
    std::vector<float> spark;   // the latest detections' SNR (dB, oldest first; NAN = a listen without detection)
    std::string note;
};

struct HfbBand {
    std::string name;
    int total = 0, inSpan = 0, heard = 0;
    double bestKm = NAN;        // the farthest beacon heard now
};

struct HfbTelemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 waiting for samples, 1 listening
    double rate = 0, centerHz = 0;
    double nowUtc = 0;          // stream time as UTC (with a radio: the computer clock; a file or the test signal: from 0)
    bool clockValid = false;    // nowUtc is the computer's clock (NCDXF slots can be attributed); false: from 0
    double clockOffsetSec = 0;  // the correction applied
    double spanLoHz = 0, spanHiHz = 0;
    int channels = 0;           // narrow channels being watched
    std::vector<HfbState> beacons;   // same order as the list
    std::vector<HfbBand> bands;
    int sel = -1;               // the beacon whose history follows
    std::vector<HfbHistPt> history;
    bool haveObserver = false;
    double obsLat = 0, obsLon = 0;
    uint64_t frames = 0;
    double powerDb = -200;      // input power, dBFS
    int heardNow = 0;
};

} // namespace dect2
