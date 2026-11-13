// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS receiver telemetry, shared by the receiver and the UI. Designed for every system in the signal plan (GPS, GLONASS, BeiDou, Galileo),
// whether or not the receiver decodes it yet: an unsupported system just never shows up in the tables.
// Every vector is capped; a report stays well below 100 kB.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// QZSS and SBAS send on the GPS L1 C/A band with their own codes; their PRN numbers are the L1 C/A ones (QZSS 193..202, SBAS 120..158).
enum GnssSystem { GnssGps = 0, GnssGlonass = 1, GnssBeidou = 2, GnssGalileo = 3, GnssQzss = 4, GnssSbas = 5, GnssSystems = 6 };
inline const char* gnssSystemName(int s) {
    static const char* n[] = {"GPS", "GLONASS", "BeiDou", "Galileo", "QZSS", "SBAS"};
    return s >= 0 && s < GnssSystems ? n[s] : "?";
}
inline char gnssSystemLetter(int s) {      // RINEX style satellite names: G05, R12, C19, E07, J01, S27
    static const char l[] = {'G', 'R', 'C', 'E', 'J', 'S'};
    return s >= 0 && s < GnssSystems ? l[s] : '?';
}
// the RINEX number of a satellite: QZSS PRN 193 is J01, SBAS PRN 127 is S27; the others are their PRN
inline int gnssRinexNumber(int sys, int prn) { return sys == GnssQzss ? prn - 192 : sys == GnssSbas ? prn - 100 : prn; }
inline std::string gnssSatName(int sys, int prn) {
    char b[16];
    snprintf(b, sizeof b, "%c%02d", gnssSystemLetter(sys), gnssRinexNumber(sys, prn));
    return b;
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
    int prn = 0;                 // GPS 1..32, BeiDou 1..63, Galileo 1..36, QZSS 193..202, SBAS 120..158, GLONASS: the orbital slot 1..24 when known, else 0
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

// Sub-satellite point from a decoded ephemeris or almanac (the ECEF used for the fix, as lat/lon)
struct GnssSatPos {
    int sys = 0, prn = 0, fcn = 0;
    double lat = 0, lon = 0, altKm = 0;
    bool tracked = false;
    bool used = false;
    bool fromEphemeris = false;
    int health = -1;
    float cn0 = 0;
    double ephAgeS = -1;         // seconds from the ephemeris reference time (toe / tb) to the time the position is for; -1 for an almanac
};

// The longest an ephemeris is trusted for a position: 4 h from toe (GPS, QZSS, Galileo, BeiDou), half an hour from tb (GLONASS)
inline double gnssEphemerisValidS(int sys) { return sys == GnssGlonass ? 1800.0 : 4 * 3600.0; }
inline bool gnssEphemerisStale(const GnssSatPos& s) { return s.ephAgeS < 0 || s.ephAgeS > gnssEphemerisValidS(s.sys); }

// What goes to the public map for one satellite: only a position from a broadcast ephemeris that is still valid (never an almanac, never a guess).
// No azimuth or elevation: with the sub-point they would give away where the receiver is.
struct GnssMapRow {
    std::string id, sub, extra;
    double lat = 0, lon = 0, altKm = 0;
};
inline bool gnssMapRow(const GnssSatPos& s, GnssMapRow& r) {
    if (!s.fromEphemeris || gnssEphemerisStale(s)) return false;
    if (!(s.lat >= -90 && s.lat <= 90 && s.lon >= -180 && s.lon <= 180)) return false;
    r.id = gnssSatName(s.sys, s.prn);
    if (s.sys == GnssGlonass && s.prn == 0) return false;   // slot not known
    r.sub = gnssSystemName(s.sys);
    r.lat = s.lat; r.lon = s.lon; r.altKm = s.altKm;
    char b[96];
    snprintf(b, sizeof b, "from the broadcast ephemeris, %.1f h from its reference time%s", s.ephAgeS / 3600.0, s.health > 0 ? ", flagged unhealthy" : "");
    r.extra = b;
    return true;
}

// What the navigation message of one satellite has delivered
struct GnssNavInfo {
    int sys = 0, prn = 0;
    int fcn = 0;                 // GLONASS frequency channel
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
    // SBAS: the messages with a good CRC (the corrections are not applied)
    int sbasLastType = -1;       // message type of the last one (0..63)
    uint32_t sbasMessages = 0;
    uint32_t sbasTypesSeen = 0;  // bit t: type t (0..31) has been received
    // Galileo: the GST-GPS time offset (word type 10) when this satellite sent it, ns
    bool ggtoValid = false;
    float ggtoNs = 0;
};

struct GnssFix {
    bool valid = false;
    double latDeg = 0, lonDeg = 0, heightM = 0;      // WGS-84, height above the ellipsoid
    double ecef[3] = {0, 0, 0};
    double clockBiasM[GnssSystems] = {};             // the receiver clock error against each system's time, metres (0 when the system is not in the fix);
                                                     // QZSS shares the GPS clock. Galileo: its own unknown, or the GPS one with the broadcast GST-GPS offset applied
    bool systemInFix[GnssSystems] = {};
    bool ggtoApplied = false;                        // the Galileo pseudoranges were moved to GPS time with the broadcast offset (word type 10)
    double ggtoNs = 0;                               // that offset, GST minus GPS time
    bool bdtGpsApplied = false;                      // the BeiDou pseudoranges were moved to GPS time with the broadcast A0GPS, A1GPS
    double bdtGpsNs = 0;                             // that offset, BDT minus GPS time less the whole 14 s
    bool gloTimeValid = false;                       // GLONASS string 5 has given tau_GPS and tau_c
    double gloTauGpsNs = 0;                          // GPS minus GLONASS time less the whole seconds (tau_GPS); the GLONASS satellites keep a clock unknown of their own
    double gloTauCNs = 0;                            // GLONASS time to UTC(SU) + 3 h (tau_c)
    double clockDriftMps = 0;                        // frequency error of the receiver clock in metres per second (from the Doppler of the tracked satellites)
    bool timeValid = false;                          // the date and time below are known
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    double second = 0;                               // UTC (GPS time minus the leap seconds from the message)
    int gpsWeek = -1;
    double gpsTow = 0;                               // GPS time of week of the fix, seconds
    int leapSeconds = -1;                            // GPS minus UTC, -1 until the message has said
    double unixAtSignal = 0;                         // UTC (epoch seconds) at signal time signalS, from the satellites; 0 until the time is known
    double signalS = 0;                              // the signal time of the measurement that gave this fix
    int nSats = 0;                                   // satellites in the solution
    int nSatsPerSystem[GnssSystems] = {};
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

// ---------------------------------------------------------------- interference and spoofing monitor (gnss_interf.h makes these)
// Receive-only: what is in the band besides the satellites, and whether the solution looks like it is being fed false signals.
enum GnssInterfStatus { GnssInterfClear = 0, GnssInterfRaised = 1, GnssInterfAlarm = 2 };
inline const char* gnssInterfStatusName(int s) {
    static const char* n[] = {"clear", "raised noise", "interference"};
    return s >= 0 && s <= 2 ? n[s] : "?";
}
// what kind of signal is in the band (bits of GnssInterference::kinds)
enum GnssInterfKind { GnssKindCw = 1, GnssKindChirp = 2, GnssKindPulsed = 4, GnssKindWideband = 8 };
inline std::string gnssInterfKindNames(unsigned k) {
    std::string s;
    if (k & GnssKindCw) s += "narrowband";
    if (k & GnssKindChirp) s += std::string(s.empty() ? "" : ", ") + "swept (chirp)";
    if (k & GnssKindPulsed) s += std::string(s.empty() ? "" : ", ") + "pulsed";
    if (k & GnssKindWideband) s += std::string(s.empty() ? "" : ", ") + "wideband";
    return s.empty() ? "none" : s;
}
// one narrowband line in the averaged spectrum
struct GnssInterfSpike {
    double offsetHz = 0;         // from the tuned centre
    float aboveDb = 0;           // peak above the noise floor
    float widthHz = 0;
    bool known = false;          // it was already there while the monitor learned the band (the radio's own spur, usually)
};
// one point of the history; the points are one second apart at first and get coarser (pairs merged) as the run grows
struct GnssInterfPoint {
    double t = 0;                // signal seconds (the end of the interval)
    double unixS = 0;            // the computer's clock, seconds since 1970
    float noiseDb = 0;           // noise floor, dBFS/Hz
    float powerDb = 0;           // total power in the band, dBFS (full-scale complex sine = 0)
    float cn0 = 0;               // mean C/N0 of the tracked satellites, 0 with none
    float flatness = 0;
    uint8_t tracked = 0;
    uint8_t status = 0;          // GnssInterfStatus (the worst in the interval)
    uint8_t spoof = 0;           // a spoofing indicator was raised in the interval
};
struct GnssInterfEvent {
    double t = 0, unixS = 0;
    int level = 0;               // 0 information, 1 warning, 2 alarm
    std::string text;
};
struct GnssInterference {
    bool valid = false;          // a first second has been measured
    bool learning = true;        // the first seconds: the baseline of the noise floor is being learned
    int status = GnssInterfClear;
    std::string reason;          // why the status is what it is ("" when clear)
    unsigned kinds = 0;          // GnssInterfKind bits seen in the last second
    // levels of the last second
    float noiseDb = 0;           // noise floor (median of the spectrum), dBFS/Hz
    float baselineDb = 0;        // what the floor was without interference
    float riseDb = 0;            // noiseDb - baselineDb
    float powerDb = 0;           // total power, dBFS
    float flatness = 0;          // spectral flatness (geometric / arithmetic mean of the band's spectrum): 1 white noise, near 0 a few lines
    // detectors
    float chirpScore = 0;        // fraction of strong short-time spectra whose peak moves steadily from one to the next
    float chirpRateHzPerUs = 0;  // the sweep rate when swept
    float pulseRateHz = 0;       // pulses per second found in the band
    float pulseDuty = 0;         // fraction of time inside a pulse
    float pulseAboveDb = 0;      // mean pulse power above the band's median power
    // the satellites
    float cn0Avg = 0, cn0Ref = 0, cn0DropDb = 0, cn0Spread = 0;   // mean C/N0 of the tracked, its clean-sky reference, the drop, the standard deviation
    int cn0N = 0;
    // spectrum of the band, averaged over the last second (dBFS/Hz, lowest frequency first)
    std::vector<float> spectrum;
    double spanHz = 0;           // the spectrum covers centre +- spanHz / 2
    std::vector<GnssInterfSpike> spikes;   // strongest first, at most 16
    // spoofing indicators from the receiver's own solution
    bool spoofing = false;
    std::string spoofReason;
    bool cn0Uniform = false;     // five or more satellites within 1 dB of each other (a real sky spreads them by elevation)
    bool posJump = false;        // the position jumped between two fixes (held for a minute)
    bool clockJump = false;      // the receiver clock jumped
    bool farFromAverage = false; // the fix is far from the long-term average position
    bool almanacMismatch = false;   // a satellite's broadcast ephemeris puts it far from where the almanac does
    float lastJumpM = 0, lastClockJumpM = 0, distFromAvgM = 0, almMismatchKm = 0;
    int avgFixes = 0;            // fixes in the long-term average
    std::vector<GnssInterfPoint> history;    // at most 900
    double historyStepS = 1;
    std::vector<GnssInterfEvent> events;     // the last 200, oldest first
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
    unsigned activeMask = 0;     // the bands the tuning and the sample rate can hold at the moment, by the system that owns the band (the L1 band is GPS)
    unsigned decodeMask = 0;     // every system being searched or tracked in those bands (GPS, QZSS and SBAS on L1 C/A, Galileo E1 at 4 Msps and more)
    double signalSecs = 0;       // signal time since the start or the last reset

    // The channel table, strongest first, at most 24
    std::vector<GnssChannel> channels;
    int nTracked = 0;            // channels that are at least locked
    // The sky: every satellite known to be above the horizon, at most 64
    std::vector<GnssSky> sky;
    // Sub-points of satellites with an ephemeris or almanac, at most 96 (the geographic map)
    std::vector<GnssSatPos> sats;
    // Navigation data of the tracked satellites (same order as `channels`, shorter when a system has none yet)
    std::vector<GnssNavInfo> nav;
    GnssFix fix;

    // Data from the satellites that is not tied to one of them
    bool ionoValid = false;      // the ionosphere (Klobuchar) parameters are known
    bool utcValid = false;       // UTC parameters (leap seconds) are known
    int leapSeconds = -1;
    bool leapPending = false;    // the message announces another leap second: GPS - UTC becomes leapNext after day leapDn of week (mod 256) leapWn
    int leapNext = -1, leapWn = -1, leapDn = -1;
    // The computer clock against the satellites' UTC (GnssReceiver::setClockCompare): the computer's clock when each sample arrived, earliest arrival of the
    // last minute, minus the UTC of that sample from the fix. Includes the radio's USB / driver buffering (a few ms to tens of ms).
    bool clockValid = false;
    double clockOffsetMs = 0, clockOffsetAvgMs = 0;      // positive: the computer is ahead
    std::vector<float> levelHist, snrHist, offsetHist;   // one a second, the last 120 (oldest first): input level dBFS, best C/N0, clock offset ms (NaN when none)
    int almanacGps = 0;          // GPS satellites with an almanac entry (of 32)
    uint64_t sbasMessages = 0;   // SBAS messages with a good CRC, all satellites
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

    GnssInterference interf;     // the interference and spoofing monitor
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
