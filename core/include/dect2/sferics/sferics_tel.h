// Sferics (lightning) telemetry, shared by the receiver and the UI. Receive only; nothing is transmitted.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kSfericsRecentMax = 100;     // strikes kept in the list
constexpr int kSfericsMinutes = 60;           // minutes in the activity histogram

struct SfericsStrike {
    double timeSec = 0;       // seconds of signal time since the start of the capture (sample clock)
    int64_t utc = 0;          // wall clock of the start of the capture + timeSec (a file shows the time of playback)
    float peakDb = 0;         // peak power over the noise floor, dB
    float widthUs = 0;        // time above the threshold, microseconds
    int flash = 0;            // number of the flash it belongs to (strikes closer than 0.5 s share one)
};

struct SfericsTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 no samples, 1 measuring the noise floor, 2 quiet (no strike for 10 min), 3 lightning heard in the last 10 minutes
    double cfoHz = 0;
    float snrDb = 0;                // peak of the last strike over the floor, dB
    bool dataValid = false;         // at least one strike counted
    uint64_t blocksOk = 0;          // strikes counted
    uint64_t blocksBad = 0;         // impulses thrown away (too long, or mains interference)
    uint64_t strikes = 0;           // sferics counted (every impulse that is not mains interference or too long)
    uint64_t flashes = 0;           // groups of strikes closer than 0.5 s
    uint64_t mains = 0;             // impulses recognised as 50 / 60 Hz mains interference and not counted
    uint64_t tooLong = 0;           // impulses longer than 5 ms (carriers, switching, not lightning)
    bool mainsActive = false;       // a regular 100 / 120 Hz impulse train is being ignored right now
    float floorDb = -200;           // noise floor, dBFS (mean power in the input band)
    float thresholdDb = 10;         // detection threshold over the floor, dB
    double perMin1 = 0;             // strikes per minute, last 60 s
    double perMin10 = 0;            // strikes per minute, averaged over the last 10 minutes
    double lastAgoSec = -1;         // seconds since the last strike, -1 none yet
    std::vector<int> perMinute;     // kSfericsMinutes bins, oldest first, the last one is the running minute
    std::vector<SfericsStrike> recent;   // newest first
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;
};

// none / low / moderate / high from strikes per minute (a local storm gives tens per minute, a distant one a few)
inline const char* sfericsActivity(double perMin) { return perMin < 0.2 ? "none" : perMin < 3 ? "low" : perMin < 15 ? "moderate" : "high"; }

}  // namespace dect2
