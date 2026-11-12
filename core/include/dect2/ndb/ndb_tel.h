// NDB (non-directional beacon) telemetry. RxTelemetry::ndb, engine standard 31, active standard 30.
//
// An aeronautical NDB is a steady carrier in the long-wave beacon band with a slow Morse ident. The ident is what this
// mode reports. HF beacons (NCDXF / IARU, long dashes, single letters) are a different mode.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One beacon of the census (all carriers inside the captured band)
struct NdbCensusRow { double offsetHz = 0; float levelDb = -200, snrDb = 0, wpm = 0; int toneHz = 0; std::string ident; int64_t lastWall = 0; };

struct NdbTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no carrier, 1 carrier, 2 an ident has repeated
    double cfoHz = 0;              // carrier, after the tune offset is mixed out
    float snrDb = 0;               // key-down against key-up on the path in use
    float levelDb = -200;          // power in the decimated channel, dBFS
    bool dataValid = false;        // a repeated ident is on screen
    uint64_t blocksOk = 0;         // idents accepted
    uint64_t blocksBad = 0;        // Morse characters not in the table
    // 4.0 ident folding (the average of the repeated idents): the last evaluation of the best path
    float foldPeriodSec = 0;       // ident period from the autocorrelation (0: none found)
    float foldZ = 0;               // how far that period stands out of the autocorrelation at other lags (robust z-score)
    float foldQuality = 0;         // on / off contrast of the average in noise deviations of the average
    std::string foldText;          // what the average reads as Morse
    int toneHz = 0;                // 400 or 1020; 0 when the path is not a tone yet; -1 keyed carrier (no modulating tone)
    float depth = 0;               // modulation depth of the path in use (0..1)
    float wpm = 0;                 // 0 until the CW timing decoder has a dot length
    bool key = false;              // the ident tone (or the carrier) is on now
    bool longDash = false;         // a mark longer than about a second: the dash some beacons send between idents
    std::string ident;             // letters that have repeated (2 or 3 is usual; 1 is kept only after three repeats)
    std::string text;              // the newest decoded characters, spaces between idents
    std::string partial;           // the ident still being keyed
    std::vector<std::string> history;   // confirmed idents, oldest first
    std::vector<NdbCensusRow> census;   // every beacon heard in the band, by frequency
};

inline std::string ndbToneName(const NdbTelemetry& t) {
    if (t.toneHz == 400 || t.toneHz == 1020) {
        char b[32];
        snprintf(b, sizeof b, "%d Hz", t.toneHz);
        return b;
    }
    if (t.toneHz < 0) return "keyed carrier";
    return "searching";
}

inline std::string ndbSummary(const NdbTelemetry& t) {
    char b[160];
    if (t.state == 0) return "NDB: no carrier";
    if (t.ident.empty()) {
        snprintf(b, sizeof b, "NDB: carrier, %s, no ident yet", ndbToneName(t).c_str());
        return b;
    }
    if (t.wpm > 0) snprintf(b, sizeof b, "NDB %s, %s, %.0f WPM", t.ident.c_str(), ndbToneName(t).c_str(), t.wpm);
    else snprintf(b, sizeof b, "NDB %s, %s", t.ident.c_str(), ndbToneName(t).c_str());
    return b;
}

} // namespace dect2
