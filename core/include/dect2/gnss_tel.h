// GNSS receiver telemetry, shared by the receiver and the UI. Designed for every system in the signal plan (GPS, GLONASS, BeiDou, Galileo),
// whether or not the receiver decodes it yet: an unsupported system just never shows up in the tables.
// Every vector is capped; a report stays well below 100 kB.
#pragma once
#include "ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

enum GnssSystem { GnssGps = 0, GnssGlonass = 1, GnssBeidou = 2, GnssGalileo = 3, GnssSystems = 4 };
inline const char* gnssSystemName(int s) {
    static const char* n[] = {"GPS", "GLONASS", "BeiDou", "Galileo"};
    return s >= 0 && s < GnssSystems ? n[s] : "?";
}
inline char gnssSystemLetter(int s) {      // RINEX style satellite names: G05, R12, C19, E07
    static const char l[] = {'G', 'R', 'C', 'E'};
    return s >= 0 && s < GnssSystems ? l[s] : '?';
}
// bits of the systems mask (GnssReceiver::setSystems)
constexpr unsigned gnssSystemBit(int s) { return 1u << s; }

// How far a channel has got (GnssChannel::state)
enum GnssChState { GnssChPullIn = 0, GnssChLocked = 1, GnssChBitSync = 2, GnssChFrameSync = 3, GnssChEphemeris = 4 };
inline const char* gnssChStateName(int s) {
    static const char* n[] = {"pull-in", "locked", "bit sync", "frame sync", "ephemeris"};
    return s >= 0 && s <= GnssChEphemeris ? n[s] : "?";
}

struct GnssChannel {
    int sys = 0;                 // GnssSystem
    int prn = 0;                 // GPS 1..32, BeiDou 1..63, Galileo 1..36, GLONASS: the orbital slot 1..24 when known, else 0
    int fcn = 0;                 // GLONASS frequency channel number -7..6 (0 for the other systems)
    int state = 0;               // GnssChState
    float cn0 = 0;               // C/N0 in dB-Hz
    double dopplerHz = 0;        // carrier Doppler plus the receiver's frequency error, at the signal's carrier
    double codePhase = 0;        // code phase of the prompt replica in chips (0 .. code length)
    bool hasAzEl = false;        // azimuth and elevation are known (an almanac or ephemeris and a rough position)
    float azDeg = 0, elDeg = 0;
    bool used = false;           // this satellite is in the position solution
    int health = -1;             // -1 unknown, 0 healthy, 1 unhealthy (the satellite's own flag)
    uint32_t framesOk = 0;       // subframes (GPS) or strings (GLONASS) with a good check
    uint32_t framesBad = 0;
    float residualM = 0;         // pseudorange residual of the last fix, metres (valid when used)
    float lockSecs = 0;          // seconds since the carrier lock
    std::vector<float> cn0Hist;  // C/N0, one value a second, the last 60 (oldest first)
};

// A satellite that the almanac or ephemeris puts above the horizon, for the sky plot
struct GnssSky {
    int sys = 0, prn = 0, fcn = 0;
    float azDeg = 0, elDeg = 0;
    bool tracked = false;
    bool used = false;
    float cn0 = 0;               // 0 when not tracked
    int health = -1;
    bool fromEphemeris = false;  // the position came from a decoded ephemeris (else the almanac)
};

// What the navigation message of one satellite has delivered
struct GnssNavInfo {
    int sys = 0, prn = 0;
    bool hasEphemeris = false;
    int iode = -1;               // GPS IODE / BeiDou AODE, GLONASS: the ephemeris reference time index
    float ephAgeS = 0;           // seconds since the ephemeris reference time (negative: it lies ahead)
    int health = -1;
    bool hasAlmanac = false;     // this satellite's own almanac entry is known
    int week = -1;               // week number the satellite sent (GPS: full week with the rollovers resolved)
    int towS = -1;               // time of week of the last subframe, seconds
    float svClockBiasUs = 0;     // af0 in microseconds (shows the satellite clock offset)
    int ephParts = 0;            // GPS: subframes 1-3 of the ephemeris being collected that have arrived (0..3; 3 with hasEphemeris)
    float ephEtaS = -1;          // seconds until the ephemeris should be complete (-1: not known yet, before the frame is found)
};

struct GnssFix {
    bool valid = false;
    double latDeg = 0, lonDeg = 0, heightM = 0;      // WGS-84, height above the ellipsoid
    double ecef[3] = {0, 0, 0};
    double clockBiasM[GnssSystems] = {0, 0, 0, 0};   // the receiver clock error against each system's time, metres (0 when the system is not in the fix)
    bool systemInFix[GnssSystems] = {false, false, false, false};
    double clockDriftMps = 0;                        // frequency error of the receiver clock in metres per second (from the Doppler of the tracked satellites)
    bool timeValid = false;                          // the date and time below are known
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    double second = 0;                               // UTC (GPS time minus the leap seconds from the message)
    int gpsWeek = -1;
    double gpsTow = 0;                               // GPS time of week of the fix, seconds
    int leapSeconds = -1;                            // GPS minus UTC, -1 until the message has said
    int nSats = 0;                                   // satellites in the solution
    int nSatsPerSystem[GnssSystems] = {0, 0, 0, 0};
    float hdop = 0, vdop = 0, pdop = 0, tdop = 0;
    float hErrM = 0;                                 // estimated horizontal error (1 sigma), metres: DOP times the measured pseudorange scatter
    float vErrM = 0;
    float residualRmsM = 0;                          // rms of the pseudorange residuals
    std::string type;                                // "GPS 7 satellites", "GPS + BeiDou 9 satellites", "no fix"
    double firstFixSecs = -1;                        // signal time from the start of the signal to the first fix, -1 until there is one
    uint32_t fixCount = 0;                           // fixes since the start
    double speedMps = 0, courseDeg = 0;              // from the Doppler of the tracked satellites when there are enough, else 0
    bool hasVelocity = false;
};

struct GnssTelemetry {
    // The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
    uint64_t seq = 0;            // grows with every report
    int state = 0;               // 0 searching, 1 satellites tracked, 2 position fix
    double cfoHz = 0;            // the receiver's carrier error (the mean Doppler of the tracked satellites is not in it)
    float snrDb = 0;             // the best C/N0 in dB-Hz of a tracked satellite
    bool dataValid = false;      // a position fix
    uint64_t blocksOk = 0;       // navigation subframes (GPS, BeiDou) and strings (GLONASS) with a good parity or check
    uint64_t blocksBad = 0;      // the ones that failed it

    // Set-up
    double inputRate = 0;        // Hz, the radio's sample rate
    double centerMhz = 0;        // the tuned centre frequency the receiver assumes (setCenterMhz)
    unsigned systemsMask = 0;    // the systems the user asked for (gnssSystemBit)
    unsigned activeMask = 0;     // the systems the tuning and the sample rate can hold at the moment
    double signalSecs = 0;       // signal time since the start or the last reset

    // The channel table, strongest first, at most 24
    std::vector<GnssChannel> channels;
    int nTracked = 0;            // channels that are at least locked
    // The sky: every satellite known to be above the horizon, at most 64
    std::vector<GnssSky> sky;
    // Navigation data of the tracked satellites (same order as `channels`, shorter when a system has none yet)
    std::vector<GnssNavInfo> nav;
    GnssFix fix;

    // Data from the satellites that is not tied to one of them
    bool ionoValid = false;      // the ionosphere (Klobuchar) parameters are known
    bool utcValid = false;       // UTC parameters (leap seconds) are known
    int leapSeconds = -1;
    int almanacGps = 0;          // GPS satellites with an almanac entry (of 32)
    float ionoAlpha[4] = {0, 0, 0, 0}, ionoBeta[4] = {0, 0, 0, 0};

    // Search
    bool searching = false;      // the acquisition is running
    int searchSys = 0, searchPrn = 0;   // the satellite being searched now
    float searchProgress = 0;    // 0..1 through the list of satellites of this round
    uint32_t searchRounds = 0;   // complete rounds so far
    float searchCenterHz = 0;    // the frequency window being searched: the radio's error plus the satellites' Doppler lie in it
    float searchHalfHz = 0;
    int searchMs = 0;            // milliseconds integrated per search (longer for weak signals)
    int searchStage = 0;         // 0 the first window, 1 +-45 kHz, 2 +-170 kHz, 3..5 the same with a long integration (only while nothing is locked)
    int nPullIn = 0;             // channels started on a find that have not locked yet
    double firstLockSecs = -1;   // signal time of the first lock
    // The correlation power against code phase of the last search that found something or, before that, the one in progress.
    // Normalised: the largest value is 1; the sample for the best code phase is at `acqPeakIndex`. At most 256 points, one code period.
    int acqSys = 0, acqPrn = 0;
    float acqDopplerHz = 0, acqPeakToNoise = 0;      // peak against the mean of the rest (power ratio)
    int acqPeakIndex = -1;
    std::vector<float> acqCorr;

    // Prompt correlator output of the strongest locked channel (about the last 400 ms), normalised to the largest |value| = 1
    int scatterSys = 0, scatterPrn = 0;
    std::vector<float> scatterI, scatterQ;

    // Front end
    float levelDbfs = -99;       // rms of the input
    float clipPercent = 0;       // samples at full scale
    float dcI = 0, dcQ = 0;      // the DC offset that is being removed (the radio's spike), full scale = 1
    std::string status;          // one line for the log pane
};

inline std::string gnssSummary(const GnssTelemetry& t) {
    char b[200];
    if (t.state == 2 && t.fix.valid) {
        snprintf(b, sizeof b, "GNSS: %s, %.5f %.5f, %.0f m, HDOP %.1f, best %.0f dB-Hz", t.fix.type.c_str(), t.fix.latDeg, t.fix.lonDeg, t.fix.heightM, t.fix.hdop, t.snrDb);
    } else if (t.state == 1) {
        snprintf(b, sizeof b, "GNSS: tracking %d satellites, best %.0f dB-Hz, no fix yet", t.nTracked, t.snrDb);
    } else if (t.searching) {
        snprintf(b, sizeof b, "GNSS: searching (%s %02d)", gnssSystemName(t.searchSys), t.searchPrn);
    } else {
        snprintf(b, sizeof b, "GNSS: searching");
    }
    return b;
}

} // namespace dect2
