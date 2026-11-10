// Telemetry of the satellite carrier and burst detector modes (satdetect_live.h), shared with the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kSdLineMax = 64;
constexpr size_t kSdChanMax = 128;

struct SdLine {                       // a carrier followed through time
    uint32_t id = 0;
    double offsetHz = 0;              // from the radio's centre, newest
    double mhz = 0;                   // absolute, when the centre is known
    float snrDb = 0;
    float driftHzS = 0;               // measured slope
    float durSec = 0;
    bool active = false;
    std::string channel;              // the catalogue channel it sits on, "" when it is on none
    std::string sat;                  // the satellite whose Doppler curve fits, "" when none (or no elements)
    int norad = 0;
    float rmsHz = 0;
    bool ambiguous = false;
};

struct SdChan {                       // a channel of the band: seen active
    double offsetHz = 0, mhz = 0;
    float widthHz = 0;                // measured occupied width
    float snrDb = 0;
    float duty = 0;                   // share of the time on, recent
    uint32_t bursts = 0;
    float lastSec = 0;                // session time of the last sighting
    std::string channel;
    bool active = false;
};

struct SdFrameLine {                  // a periodic structure searched for
    double rateHz = 0;                // expected
    double measuredHz = 0;
    float levelDb = 0;                // over the local background
    bool detected = false;
    std::string name;
};

struct SatDetectTelemetry {
    uint64_t seq = 0;
    int state = 0;                    // 0 waiting, 1 running, 2 signal found
    double cfoHz = 0;                 // strongest carrier from the centre
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // detections, tracks that ended unexplained
    // details
    int standard = 0;
    double timeSec = 0, inputRate = 0, binHz = 0, centreHz = 0;
    int catalog = 0;                  // satellites with elements given
    std::vector<SdLine> lines;        // active first, then newest
    std::vector<SdChan> channels;     // strongest first
    std::vector<SdFrameLine> frames;
    std::vector<float> spectrum;      // 256 points, dB over the background
};

inline std::string sdSummary(const SatDetectTelemetry& t) {
    char b[160];
    if (t.state == 0) return "waiting for samples";
    if (!t.frames.empty()) {
        for (const auto& f : t.frames) if (f.detected) { snprintf(b, sizeof b, "%s detected: %.1f dB", f.name.c_str(), (double)f.levelDb); return b; }
        snprintf(b, sizeof b, "no signature yet (%.0f s)", t.timeSec);
        return b;
    }
    int act = 0; for (const auto& l : t.lines) act += l.active;
    int chan = 0; for (const auto& c : t.channels) chan += c.active;
    if (!t.channels.empty()) snprintf(b, sizeof b, "%d active channels, %llu bursts", chan, (unsigned long long)t.blocksOk);
    else snprintf(b, sizeof b, "%d carriers now, %llu tracks", act, (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
