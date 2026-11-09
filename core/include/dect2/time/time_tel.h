// Time signal telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "time_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kTimeHist = 120;     // per-second histories, oldest first
constexpr size_t kTimeLogMax = 200;   // decoded minutes kept

struct TimeFrameLog {
    int64_t wallTime = 0;             // the computer's clock when it was decoded
    bool ok = false;                  // false: the minute did not pass its checks
    std::string text;                 // the time, or why it was rejected
    std::string bits;                 // the 60 symbols: 0 1 (MSF: 0 1 2 3 for A + 2B), M marker, - none, ? not read
};

struct TimeTelemetry {
    bool dcfPhaseLocked = false; float dcfPhaseCorr = 0; int dcfPhaseBit = -1;   // DCF77 phase code: chip sequence found, correlation, last bit
    uint64_t seq = 0;
    int state = 0;                    // 0 searching, 1 the seconds are found, 2 the time is decoded
    double cfoHz = 0;                 // carrier offset from the dial
    float snrDb = 0;                  // modulation (LF) or tick (HF) to noise, per second
    float levelDb = -200;             // carrier level after the channel filter, dBFS
    bool dataValid = false;
    uint64_t blocksOk = 0;            // minutes decoded
    uint64_t blocksBad = 0;           // minutes rejected (parity, fixed bits, range)
    // details
    int station = 0;                  // timesig::Station being received
    bool hawaii = false;              // WWV / WWVH: the 1200 Hz ticks of WWVH are the stronger
    float tick1000Db = -200, tick1200Db = -200;   // WWV / WWVH tick levels
    double timeSec = 0, inputRate = 0;
    double secPhaseMs = 0;            // where the seconds tick sits in the sample stream (ms after a whole second of signal time)
    double tickJitterMs = 0;          // rms of the single seconds about the averaged one
    float depth = 0;                  // LF: how deep the carrier keying is (0 none, 1 off); HF: tick to background
    int secondOfMinute = -1;          // the second now being received (-1 unknown)
    std::string bitsNow;              // the symbols of the minute so far
    bool timeValid = false;           // a minute has been decoded and the seconds are still followed
    bool confirmed = false;           // two decoded minutes in a row agree
    timesig::Fields fields;           // of the last minute decoded
    double utcNow = 0;                // the received time (UTC epoch seconds) at timeSec
    double secsSinceFrame = 0;        // signal time since the last minute decoded
    // the computer clock against the received time
    bool clockValid = false;          // the receiver has a clock to compare (a radio or the test signal, not a file)
    double clockOffsetMs = 0;         // computer clock minus the received time, after the path delay; positive: the computer is ahead
    double clockOffsetAvgMs = 0;      // averaged over the last minute
    bool locationSet = false;         // the receiver position is the user's (else a placeholder)
    double distanceKm = 0, propDelayMs = 0;
    std::vector<float> levelHist, snrHist, tickHist, offsetHist;   // per second; tickHist: ms of each second's tick about the average
    std::vector<TimeFrameLog> log;    // newest first
};

inline std::string timeSummary(const TimeTelemetry& t) {
    char b[160];
    const char* st = timesig::stationName(t.station);
    if (t.timeValid) {
        const int64_t u = (int64_t)t.utcNow;
        int y, m, d;
        timesig::civilFromDays(u / 86400, y, m, d);
        const int64_t s = u % 86400;
        snprintf(b, sizeof b, "%s %04d-%02d-%02d %02d:%02d:%02d UTC", st, y, m, d, (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
        return b;
    }
    if (t.state >= 1) { snprintf(b, sizeof b, "%s: seconds found, waiting for the minute", st); return b; }
    snprintf(b, sizeof b, "%s: searching", st);
    return b;
}

} // namespace dect2
