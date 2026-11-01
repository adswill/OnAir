// PACTOR-II / -III / -IV telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kPactorHfEventsMax = 60;
constexpr size_t kPactorHfHist = 120;
constexpr size_t kPactorHfRowSyms = 200;

struct PactorHfEvent {
    int64_t utc = 0;
    double timeSec = 0;
    std::string text;
};

struct PactorHfTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 nothing, 1 carriers found, 2 frame structure found
    double cfoHz = 0;            // centre of the carriers relative to the nominal audio centre
    float snrDb = 0;             // clock line over the noise
    float levelDb = -200;
    bool dataValid = false;      // a repeating frame was found
    uint64_t blocksOk = 0;       // frame copies seen
    uint64_t blocksBad = 0;
    // what is wanted (the mode) and what was found
    int wanted = 2;              // 2, 3 or 4
    int variant = 0;             // what the analysis found (2, 3, 4), 0 nothing
    double baud = 0, centreHz = 0, spacingHz = 0, bwHz = 0, clockPpm = 0, lineDb = 0;
    int nCarriers = 0, level = 0, constellation = 0;
    std::vector<double> carrierHz;
    std::vector<int> carrierOrder;
    int framePeriod = 0, syncPos = 0, syncLen = 0, frameCopies = 0;
    double framePeriodSec = 0, frameMatch = 0;
    std::string syncHex, note;
    std::vector<std::string> rows;   // the newest symbols of the first carriers, one character per symbol
    uint64_t analyses = 0, detections = 0;
    double timeSec = 0, inputRate = 0;
    std::vector<PactorHfEvent> events;   // newest first
    std::vector<float> levelHist;
};

inline const char* pactorHfName(int v) { return v == 3 ? "PACTOR-III" : v == 4 ? "PACTOR-IV" : "PACTOR-II"; }

inline std::string pactorHfSummary(const PactorHfTelemetry& t) {
    char b[200];
    const char* nm = pactorHfName(t.wanted);
    if (t.variant == t.wanted && t.nCarriers > 0) {
        if (t.wanted == 4) snprintf(b, sizeof b, "%s: %.0f baud single carrier, %s", nm, t.baud, t.constellation ? "constellation estimated" : "constellation unknown");
        else if (t.framePeriod) snprintf(b, sizeof b, "%s: %d carriers, frame %.2f s", nm, t.nCarriers, t.framePeriodSec);
        else snprintf(b, sizeof b, "%s: %d carriers at %.0f baud", nm, t.nCarriers, t.baud);
        return b;
    }
    if (t.variant != 0) { snprintf(b, sizeof b, "%s: this looks like %s", nm, pactorHfName(t.variant)); return b; }
    snprintf(b, sizeof b, "%s: no signal", nm);
    return b;
}

} // namespace dect2
