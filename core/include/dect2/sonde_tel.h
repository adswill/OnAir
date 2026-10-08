// Radiosonde receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
//
// One report holds every sonde heard so far (up to 16, most recently heard first) and the carriers the search saw in the band.
// Size: a sonde carries at most 600 track points of 24 bytes, and only the 5 most recently heard carry their track (trackIncluded),
// so a report stays under 100 kB.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

enum SondeKind { SondeUnknown = 0, SondeRs41, SondeRs92, SondeDfm, SondeM10, SondeM20, SondeLms6 };

inline const char* sondeKindName(int k) {
    switch (k) {
    case SondeRs41: return "RS41";
    case SondeRs92: return "RS92";
    case SondeDfm: return "DFM";
    case SondeM10: return "M10";
    case SondeM20: return "M20";
    case SondeLms6: return "LMS6";
    default: return "?";
    }
}

struct SondeTrackPoint {
    float lat = 0, lon = 0;      // degrees (float keeps about 0.5 m at 55 degrees)
    float altM = 0;
    uint32_t unixT = 0;          // UTC seconds from the sonde (0 when the sonde sent no time: the points are still in time order)
    float tempC = -999;          // -999: the frame had no temperature yet (plots of temperature against altitude skip these)
    float humidity = -1;         // percent, -1: none
};

struct SondeInfo {
    int kind = SondeUnknown;     // SondeKind
    std::string type;            // "RS41", "DFM", ...
    std::string subtype;         // "RS41-SG", "DFM-17", ...
    std::string serial;
    int channel = -1;            // receiver channel now following it (-1: not being received)
    double freqHz = 0;           // carrier frequency; 0 when unknown (offsetHz is always known)
    double offsetHz = 0;         // carrier relative to the tuned centre frequency
    float snrDb = 0;             // signal to noise ratio in about 10 kHz
    int frame = -1;              // frame counter of the last frame
    bool hasTime = false;
    double unixTime = 0;         // UTC seconds since 1970 sent by the sonde
    bool hasPos = false;
    double lat = 0, lon = 0, altM = 0;
    bool hasVel = false;
    double vSpeed = 0, hSpeed = 0, headingDeg = 0;    // m/s up, m/s along the ground, degrees from north
    int sats = -1;               // satellites used by the sonde's GPS, -1 unknown
    bool hasTemp = false, hasHumidity = false, hasPressure = false;
    double tempC = 0, humidity = 0, pressureHpa = 0;
    double batteryV = -1;        // -1: not sent
    int burstKillS = -1;         // burst / kill timer in seconds where the sonde sends one, else -1
    std::string note;            // for example "calibrating 12/51"
    int calDone = 0, calTotal = 0;     // calibration subframes collected (RS41: of 51); calTotal 0 when the type has none
    uint64_t framesOk = 0, framesBad = 0;
    double lastHeardS = 0;       // seconds since the last frame (receiver time)
    double firstHeardS = 0;      // seconds since the first frame
    double maxAltM = 0;          // highest altitude seen (a falling sonde has passed its burst)
    bool active = false;         // a frame in the last 30 s
    bool trackIncluded = false;
    std::vector<SondeTrackPoint> track;     // oldest first, thinned to at most 600 points
};

struct SondeCarrier {
    double offsetHz = 0;         // relative to the tuned centre frequency
    double freqHz = 0;           // absolute when the centre is known, else 0
    float levelDb = 0;           // dB over the noise floor in about 10 kHz
    bool assigned = false;       // a channel is receiving it
    int channel = -1;
};

struct SondeTelemetry {
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 signal, 2 decoding
    double cfoHz = 0;            // carrier error of the strongest decoded sonde against its own announced frequency (RS41 with a known centre), else 0
    float snrDb = 0;             // signal to noise ratio of the best sonde (or best carrier)
    bool dataValid = false;      // a frame has been decoded
    uint64_t blocksOk = 0;       // frames with a good check
    uint64_t blocksBad = 0;      // the ones that failed it
    // Radiosonde specific
    double centerHz = 0;         // tuned centre frequency when the app told the receiver (setCenterMhz), else 0
    double bandHz = 0;           // input rate: the width of the band searched
    double streamTimeS = 0;      // seconds of signal processed since the start
    int channelsUsed = 0, channelsMax = 8;
    uint64_t searches = 0;       // carrier searches done
    std::vector<SondeInfo> sondes;          // most recently heard first
    std::vector<SondeCarrier> carriers;     // strongest first, up to 32
};

inline std::string sondeSummary(const SondeTelemetry& t) {
    if (t.state == 0) return "Radiosonde: searching";
    if (t.sondes.empty()) return "Radiosonde: signal";
    char b[200];
    int act = 0;
    for (const auto& s : t.sondes) act += s.active ? 1 : 0;
    const SondeInfo& s = t.sondes.front();
    if (s.hasPos)
        snprintf(b, sizeof b, "Radiosonde: %d heard, %s %s at %.1f km", act, s.type.c_str(), s.serial.c_str(), s.altM / 1000.0);
    else
        snprintf(b, sizeof b, "Radiosonde: %d heard, %s %s", act, s.type.c_str(), s.serial.c_str());
    return b;
}

} // namespace dect2
