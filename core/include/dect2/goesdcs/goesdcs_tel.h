// GOES / Meteosat DCS (data collection platform) telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kDcsMsgMax = 300;      // messages kept in the list
constexpr size_t kDcsPlatMax = 300;     // platforms kept in the table
constexpr int kDcsWfCols = 256;         // band waterfall: columns across the captured band
constexpr int kDcsWfRows = 48;          // rows, newest last
constexpr size_t kDcsBytesMax = 4096;   // bytes of one report kept

struct DcsMessage {
    uint32_t id = 0;
    int64_t wallTime = 0;               // UTC seconds of the start of the report
    uint32_t address = 0;               // the 31-bit platform address word (8 hex digits)
    bool addressOk = true;              // false: the BCH check failed (not shown as a report)
    int addressFixed = 0;               // bit errors the BCH (31,21) code corrected
    int bps = 0;                        // 100, 300 or 1200
    bool manchester = true;
    double freqHz = 0;                  // absolute when the radio centre is known, else relative to the centre
    bool freqAbsolute = false;
    int channel = 0;                    // satellite channel number in the plan below, 0 unknown
    int plan = 0;                       // the channel plan: 0 GOES, 1 Meteosat, 2 Himawari (dcs::Plan)
    float snrDb = 0;                    // carrier over the noise in one ~100 Hz bin
    int flags = -1;                     // the 8-bit flag word of the 300 / 1200 bps format, -1 when the format has none
    int format = 0;                     // 0 ASCII, 1 pseudo-binary B, 2 other
    bool eot = false;                   // ended with the end-of-transmission code
    int parityErrors = 0;
    std::string text;                   // the characters (ASCII) or the 6-bit values as characters (pseudo-binary), control characters shown as spaces
    std::vector<std::string> fields;    // the sensor data: "label value" for ASCII, "n: value" for pseudo-binary groups
    std::vector<uint8_t> raw;           // the bytes (parity bit removed)
};

struct DcsPlatform {
    uint32_t address = 0;
    uint32_t count = 0;
    int64_t firstTime = 0, lastTime = 0;
    int channel = 0;
    int bps = 0;
    float snrDb = 0;
    int format = 0;
    std::string last;                   // the newest report's text, shortened
};

struct DcsCarrier {                     // a carrier being followed
    double offsetHz = 0;                // from the radio centre
    int channel = 0;
    float snrDb = 0;
    int state = 0;                      // 0 carrier, 1 locked on the preamble, 2 receiving a report
    int bps = 0;
    uint32_t bytes = 0;
};

struct DcsTelemetry {
    uint64_t seq = 0;
    int state = 0;                      // 0 nothing, 1 carriers seen, 2 reports decoded
    double cfoHz = 0;
    float snrDb = 0;                    // the strongest carrier
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // reports decoded, reports whose address failed the BCH check or that were cut off
    // details
    double timeSec = 0, inputRate = 0, workRate = 0, binHz = 0, centreHz = 0;
    float noiseDb = -200;
    int plan = 0;                       // the channel plan in use: 0 GOES, 1 Meteosat, 2 Himawari
    uint64_t carriersSeen = 0;
    std::vector<DcsMessage> messages;   // newest first
    std::vector<DcsPlatform> platforms; // by last time, newest first
    std::vector<DcsCarrier> carriers;
    std::vector<uint8_t> waterfall;     // kDcsWfRows x kDcsWfCols, 0 = noise, 255 = 40 dB over it; row 0 oldest
    std::vector<float> spectrum;        // kDcsWfCols: dB over the noise
    double bandLowHz = 0, bandHighHz = 0;   // the frequencies (relative to the centre) the spectrum columns span
    bool logging = false;
    std::string logPath;
};

inline std::string dcsSummary(const DcsTelemetry& t) {
    char b[160];
    if (t.blocksOk == 0 && t.carriers.empty()) return "GOES DCS: no platform heard";
    snprintf(b, sizeof b, "GOES DCS: %llu reports from %zu platforms", (unsigned long long)t.blocksOk, t.platforms.size());
    return b;
}

} // namespace dect2
