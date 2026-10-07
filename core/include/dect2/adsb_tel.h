// ADS-B receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct AdsbTrackPoint { float lat = 0, lon = 0; };

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
    int adsbVersion = -1; int nacp = -1;                     // operational status
    uint32_t messages = 0;           // messages received from this address
    float ageSec = 0;                // since the last message
    float posAgeSec = 0;             // since the last position
    float levelDbfs = -120;          // mean pulse level of the last messages
    int emergency = 0;               // emergency state 0 - 7 (TC 28), 0 none; 7500, 7600 and 7700 squawks count as well
    bool ground = false;             // on the ground
    bool alert = false, spi = false;
    bool tisb = false;               // seen in DF18 (TIS-B / ADS-R / non-transponder)
    std::vector<AdsbTrackPoint> track;   // recent positions, oldest first (for the map)
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
    uint64_t corrected = 0;              // messages repaired by the 1-bit (or 2-bit) correction
    uint64_t dfCount[32] = {};           // good messages by downlink format
    uint32_t aircraftCount = 0;          // aircraft in the table (the list below holds at most 128 of them, the most recent)
    uint32_t withPosition = 0;
    bool refValid = false; double refLat = 0, refLon = 0;   // reference position used for local decoding and ranges
    float maxRangeNm = 0;                // farthest position seen (needs a reference)
    std::vector<AdsbAircraft> aircraft;  // most recently heard first
    std::vector<AdsbFrameInfo> frames;   // the last 64 good messages, newest last
};

// One line for the command line and the log
inline std::string adsbSummary(const AdsbTelemetry& t) {
    char b[200];
    snprintf(b, sizeof b, "ADS-B: state %d  %.0f msg/s  %u aircraft (%u with position)  SNR %.1f dB  noise %.1f dBFS  ok %llu  bad %llu",
             t.state, t.msgsPerSec, t.aircraftCount, t.withPosition, t.snrDb, t.noiseDbfs, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
