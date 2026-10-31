// ADS-B receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct AdsbTrackPoint {   // alt100: barometric altitude in 100 ft steps for the altitude colours of the trail (valid when hasAlt)
    float lat = 0, lon = 0;
    int16_t alt100 = 0;
    bool hasAlt = false, ground = false;
    int altFt() const { return alt100 * 100; }
};

// One aircraft, as the table shows it. A value is only meaningful when its has... flag is set. Times are seconds of signal time.
struct AdsbAircraft {
    uint32_t icao = 0;               // 24 bit address; bit 24 set: not an ICAO address (anonymous address of DF18 control field 1 / 5)
    std::string callsign;            // empty until an identification message is received
    std::string category;            // "A3", "B1", ... empty when not sent or "no information"
    bool hasSquawk = false; int squawk = 0;                  // four octal digits written as a decimal number: 7700
    bool hasAlt = false; int altFt = 0;                      // barometric altitude, 1013.25 hPa
    bool hasGeoAlt = false; int geoAltFt = 0;                // GNSS height (ellipsoid), from type codes 20 - 22
    bool hasSpeed = false; float speedKt = 0; int speedKind = 0;       // 0 ground speed, 1 indicated airspeed, 2 true airspeed
    bool hasHeading = false; float headingDeg = 0; bool headingIsTrack = true;   // ground track (with ground speed) or magnetic heading (with airspeed)
    bool hasVrate = false; int vrateFpm = 0;
    bool hasGnssDiff = false; int gnssDiffFt = 0;            // GNSS height minus barometric altitude
    bool hasPos = false; double lat = 0, lon = 0;
    int posKind = 0;                 // 0 none, 1 local decode from the reference position (not confirmed), 2 global decode (two frames), 3 local decode from the last global fix
    bool hasRange = false; float distNm = 0, bearingDeg = 0;   // from the reference position
    bool hasIas = false; int iasKt = 0; bool hasMach = false; float mach = 0;     // Comm-B 6,0
    bool hasSelAlt = false; int selAltFt = 0; bool hasSelHdg = false; float selHdgDeg = 0; bool hasBaroSet = false; float baroSetMb = 0;   // target state, Comm-B 4,0
    bool hasRoll = false; float rollDeg = 0;                 // Comm-B 5,0
    // weather: derived from 5,0 (track, ground speed, TAS) + 6,0 (magnetic heading, Mach) heard within 4 s, smoothed; or reported in 4,4
    bool hasWindDer = false; float windDerDir = 0, windDerKt = 0;      // direction the wind blows from, degrees true (heading corrected by a model declination: +-3 deg)
    bool hasWindRep = false; float windRepDir = 0, windRepKt = 0;      // 4,4
    bool hasSat = false; float satC = 0; bool satDerived = false;     // static air temperature: from TAS and Mach, or reported (4,4 / 4,5)
    bool hasPressure = false; int pressureHpa = 0;                     // 4,4 / 4,5 static pressure
    bool hasHumidity = false; float humidityPct = 0;                   // 4,4
    int turbulence = -1, windShear = -1, microburst = -1, icing = -1, wakeVortex = -1;   // 4,4 / 4,5 levels 0 nil - 3 severe, -1 not reported
    // ACAS resolution advisory (3,0), shown for a minute after it was heard
    bool hasRa = false; unsigned raAra = 0, raRac = 0; bool raTerminated = false; uint32_t raThreat = 0; float raAgeSec = 0;
    int subnetVersion = -1; bool acasRa = false; uint32_t gicbMask = 0;   // 1,0 and 1,7 capability
    int adsbVersion = -1; int nacp = -1;                     // operational status
    int sil = -1; int nic = -1;                              // source integrity level (operational status); NIC from the airborne position type code (supplement bits not applied)
    uint8_t srcId = 0;               // the source that last updated this aircraft: 0 this receiver, 1.. the feeds from other receivers (AdsbTelemetry::sources)
    uint32_t messages = 0;           // messages received from this address
    float ageSec = 0;                // since the last message
    float posAgeSec = 0;             // since the last position
    float levelDbfs = -120;          // mean pulse level of the last messages
    int emergency = 0;               // emergency state 0 - 7 (TC 28), 0 none; 7500, 7600 and 7700 squawks count as well
    bool ground = false;             // on the ground
    bool alert = false, spi = false;
    bool tisb = false;               // seen in DF18 (TIS-B / ADS-R / non-transponder)
    std::vector<AdsbTrackPoint> track;   // recent positions, oldest first (for the map)
    int cprHave = 0;                 // bit 0: even CPR in the last 15 s, bit 1: odd (table Fix column when there is still no lat/lon)
};

// Range-ring statistics (per session; reset button; saved with the results). Ranges need a reference position and a confirmed (global) position.
constexpr int kAdsbSectors = 36;            // 10 degree bearing sectors, sector 0 = 0 - 10 degrees
constexpr int kAdsbRangeBins = 50;          // 10 nm per bin, the last one is "500 nm and more"
// Coverage by altitude band: the farthest position per sector for each band of barometric altitude (a polar coverage plot per height).
// Band 0 is under 10000 ft (the ground included), then 10000 - 20000, 20000 - 30000, 30000 - 40000, 40000 ft and above. A position with no known
// altitude is in the all-heights figure only.
constexpr int kAdsbAltBands = 5;
inline int adsbAltBand(bool hasAlt, int altFt, bool ground) {
    if (ground) return 0;
    if (!hasAlt) return -1;
    return altFt < 0 ? 0 : altFt >= 40000 ? kAdsbAltBands - 1 : altFt / 10000;
}
inline const char* adsbAltBandName(int b) {
    static const char* n[kAdsbAltBands] = {"under 10000 ft", "10000-20000 ft", "20000-30000 ft", "30000-40000 ft", "40000 ft and above"};
    return b >= 0 && b < kAdsbAltBands ? n[b] : "all heights";
}
struct AdsbRangeStats {
    float sectorMaxNm[kAdsbSectors] = {};   // farthest position per bearing sector
    float sectorBandMaxNm[kAdsbAltBands][kAdsbSectors] = {};   // the same per altitude band (adsbAltBand)
    uint32_t rangeHist[kAdsbRangeBins] = {};
    uint64_t positions = 0;                 // positions counted
    float maxRangeNm = 0;
    bool hasMaxAlt = false; int maxAltFt = 0;
    std::vector<float> rateSeries;          // messages per second, one point per 10 s of signal time
    std::vector<uint16_t> countSeries;      // aircraft in the table, the same points
    double nextSample = 10;
};

// The coverage plot as CSV (band,from_deg,to_deg,max_range_nm,max_range_km): "all heights" first, then each altitude band. Unit-free, so a file means the same everywhere.
inline std::string adsbCoverageCsv(const AdsbRangeStats& st) {
    std::string o = "band,from_deg,to_deg,max_range_nm,max_range_km\n";
    char b[160];
    auto rows = [&](const char* name, const float* v) {
        for (int i = 0; i < kAdsbSectors; i++) {
            snprintf(b, sizeof b, "%s,%d,%d,%.1f,%.1f\n", name, i * 10, i * 10 + 10, v[i], v[i] * 1.852f);
            o += b;
        }
    };
    rows("all heights", st.sectorMaxNm);
    for (int k = 0; k < kAdsbAltBands; k++) rows(adsbAltBandName(k), st.sectorBandMaxNm[k]);
    return o;
}

// A source of aircraft: this receiver (always the first) and the feeds in from other receivers
struct AdsbSourceInfo {
    std::string name;                // "this receiver" or host:port
    std::string format;              // "", "Beast", "SBS"
    int state = 0;                   // 0 off / connecting, 1 connected
    uint64_t msgs = 0;               // messages (Beast frames or SBS lines) taken from it
    uint32_t aircraft = 0;           // aircraft in the table that it updated last
    std::string error;
};

struct AdsbFrameInfo {               // one line of the message monitor
    std::string hex;
    uint32_t icao = 0;
    int df = 0;
    int tc = -1;                     // type code, -1 when there is none
    int corrected = 0;               // bits that error correction flipped
    double timeSec = 0;
    float levelDbfs = 0;
    std::string what;                // "Identification KLM1023", "Airborne position 38000 ft", ...
};

// A message whose level is above this (dBFS) is "loud" (the receiver is near the top of its range; readsb's adaptive gain counts these)
constexpr float kAdsbLoudDbfs = -7.0f;   // levelDbfs is the mean power of the preamble pulses: it reads about 4 dB under the pulse peak, so this is a peak within about 3 dB of full scale

struct AdsbTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;            // grows with every report and never restarts (not even after reset())
    int state = 0;               // 0 searching (no message for 10 s), 1 pulses seen but no good message lately, 2 receiving messages
    double cfoHz = 0;            // not measured (the receiver works on the pulse envelope): always 0
    float snrDb = 0;             // mean signal-to-noise ratio of the last good messages (pulse level over the noise floor)
    bool dataValid = false;      // messages are coming out
    uint64_t blocksOk = 0, blocksBad = 0;   // messages with a good CRC (or repaired) / frames that passed the preamble test and failed the CRC, since the start

    // ---- this mode's own fields below
    double timeSec = 0;                  // signal time since the start (or since the last reset)
    float msgsPerSec = 0;                // good messages per second, over the last second
    float noiseDbfs = -120;              // noise floor (mean power per sample)
    float levelDbfs = -120;              // mean pulse level of the last good messages
    uint64_t preambles = 0;              // preamble candidates examined
    uint64_t loudMsgs = 0;               // good messages louder than kAdsbLoudDbfs (cumulative, like blocksOk)
    uint64_t corrected = 0;              // messages repaired by the 1-bit (or 2-bit) correction
    uint64_t dfCount[32] = {};           // good messages by downlink format
    uint32_t aircraftCount = 0;          // aircraft in the table (the list below holds at most 128 of them, the most recent)
    uint32_t withPosition = 0;
    bool refValid = false; double refLat = 0, refLon = 0;   // reference position used for local decoding and ranges
    float maxRangeNm = 0;                // farthest position seen (needs a reference)
    std::vector<AdsbAircraft> aircraft;  // most recently heard first
    std::vector<AdsbFrameInfo> frames;   // the last 64 good messages, newest last
    AdsbRangeStats stats;
    std::vector<AdsbSourceInfo> sources; // [0] is this receiver
};

// One line for the command line and the log
inline std::string adsbSummary(const AdsbTelemetry& t) {
    char b[200];
    snprintf(b, sizeof b, "ADS-B: state %d  %.0f msg/s  %u aircraft (%u with position)  SNR %.1f dB  noise %.1f dBFS  ok %llu  bad %llu",
             t.state, t.msgsPerSec, t.aircraftCount, t.withPosition, t.snrDb, t.noiseDbfs, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
