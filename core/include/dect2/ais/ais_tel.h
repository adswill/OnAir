// AIS receiver telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "ais_alertrec.h"

namespace dect2 {

struct AisTrackPoint { float lat = 0, lon = 0; };

enum AisClass { AIS_CLASS_A = 0, AIS_CLASS_B = 1, AIS_CLASS_BASE = 2, AIS_CLASS_ATON = 3, AIS_CLASS_SAR = 4, AIS_CLASS_OTHER = 5 };

// One station, as the table shows it. A value is only meaningful when it differs from its "not available" default.
struct AisVessel {
    uint32_t mmsi = 0;
    int cls = AIS_CLASS_OTHER;       // AisClass
    std::string name, callsign, destination;
    uint32_t imo = 0;                // 0 = not available
    int shipType = -1;               // 0 - 99, -1 = not received; aisShipTypeText() gives the words
    bool hasPos = false;
    double lat = 0, lon = 0;
    float sog = -1;                  // knots, -1 = not available
    float cog = -1;                  // degrees, -1 = not available
    int heading = -1;                // true heading in degrees, -1 = not available
    bool hasRot = false; float rotDegMin = 0;     // rate of turn in degrees per minute (class A)
    int navStatus = -1;              // 0 - 15, -1 = not received; aisNavStatusText() gives the words
    int etaMonth = 0, etaDay = 0, etaHour = 24, etaMin = 60;   // 0 / 0 / 24 / 60 = not available
    float draughtM = 0;              // 0 = not available
    int dimA = 0, dimB = 0, dimC = 0, dimD = 0;   // metres from the reference point to bow, stern, port, starboard (0 = not available)
    int aidType = -1;                // aid to navigation type (message 21), -1 = not an aid
    bool offPosition = false, virtualAid = false;
    int altitudeM = -1;              // search and rescue aircraft, -1 = not available
    int lastType = 0;                // message type of the last message
    char channel = 'A';              // of the last message: 'A' = AIS 1 (161.975 MHz), 'B' = AIS 2 (162.025 MHz), 'C' = channel 75 (156.775 MHz), 'D' = channel 76 (156.825 MHz)
    bool longRange = false;          // has sent message 27 (the long-range broadcast of ITU-R M.1371-5): a "new-type" vessel
    uint32_t longRangeMsgs = 0;      // message 27 received from it
    bool lrPosition = false;         // the position shown came from a message 27 (1/10 minute resolution, about 185 m)
    int posLatency = -1;             // message 27's GNSS position latency flag: 0 = under 5 s, 1 = 5 s or more, -1 = not received
    double lastUtc = 0;              // UNIX time of the last reception (the receiver's time base plus the signal time), 0 = not known
    uint32_t messages = 0;           // messages received from this station
    float ageSec = 0;                // since the last message (last heard)
    std::vector<AisTrackPoint> track;   // recent positions, oldest first; the telemetry carries it for the most recently heard stations only
};

// One good message as it was received: the per-message time stamp the long-range and satellite views use (ITU-R M.1371-5 annex 4:
// receivers of message 27 should time-stamp every reception, since the message itself carries no time).
struct AisRxRecord {
    uint64_t n = 0;                  // running number (grows by one per good message, never restarts): a reader takes the ones it has not seen
    double utc = 0;                  // UNIX time of the burst's start (time base + signal time)
    double timeSec = 0;              // signal time
    char channel = 'A';              // 'A', 'B', 'C' (75) or 'D' (76)
    int type = 0;
    uint32_t mmsi = 0;
    float snrDb = 0, cfoHz = 0;      // SNR in 48 kHz and the burst's frequency offset (the Doppler shift on a satellite receiver)
    bool hasPos = false; float lat = 0, lon = 0;
    float sog = -1, cog = -1;
    int navStatus = -1;
    int posLatency = -1;             // message 27 only
    int layer = 0;                   // 0 decoded directly, 1 found on another frequency or part of the burst, 2+ after cancelling the stronger burst(s)
};

struct AisTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;            // grows with every report and never restarts (not even after reset())
    int state = 0;               // 0 searching, 1 signal seen but no good message for 10 s, 2 good messages in the last 10 s
    double cfoHz = 0;            // carrier error of the last bursts (mean of the two channels, from the burst's own mean frequency)
    float snrDb = 0;             // mean signal-to-noise ratio of the last good bursts, in the 48 kHz channel bandwidth
    bool dataValid = false;      // a message was decoded in the last 10 s
    uint64_t blocksOk = 0;       // messages with a good CRC since the start
    uint64_t blocksBad = 0;      // bursts that had both flags and failed the CRC (or the length or stuffing checks)

    // ---- this mode's own fields below
    double timeSec = 0;                  // signal time since the start (or since the last reset)
    uint64_t bursts = 0;                 // bursts that the power detector handed to the decoder
    // the four channels: 0 = A / AIS 1 (161.975), 1 = B / AIS 2 (162.025), 2 = C / channel 75 (156.775), 3 = D / channel 76 (156.825 MHz)
    uint64_t typeCount[4][32] = {};      // good messages by channel and message type (index 0 = types above 27)
    uint64_t channelOk[4] = {}, channelBad[4] = {};
    float burstsPerMin[4] = {};          // good messages per minute on each channel, over the last 60 s (shorter at the start)
    float levelDbfs[4] = {-120, -120, -120, -120};   // mean power in each channel's 48 kHz band
    float noiseDbfs[4] = {-120, -120, -120, -120};   // noise floor in each channel
    int channelMask = 3;                 // the channels the capture covers (bit c = channel c)
    bool satellite = false;              // satellite mode: wide Doppler filters, collision decoding
    float dopplerHz[4] = {};             // tracked frequency offset of each channel's recent bursts (satellite mode), and its rate
    float dopplerRateHzS[4] = {};
    uint64_t recovered = 0;              // satellite mode: messages found on another frequency or part of a burst
    uint64_t cancelled = 0;              // satellite mode: messages found after cancelling a stronger burst on top of them
    uint32_t longRangeVessels = 0;       // stations that sent message 27
    std::vector<AisRxRecord> recent;     // the last 500 good messages, oldest first
    std::vector<AisAlertRecord> alerts;  // the last 50 CPA and geofence alerts (ais_alerts.h), oldest first
    uint64_t collisionRecovered = 0;     // coast mode: messages found under a stronger burst or on a slightly different frequency (collision cancellation, ais_rx.h)
    uint32_t vesselCount = 0;            // stations in the table (the list below holds at most 300, the most recently heard)
    std::vector<AisVessel> vessels;      // most recently heard first
    std::vector<std::string> nmea;       // the last 50 good messages as !AIVDM sentences, newest last
};

// ITU-R M.1371-5 table 50 (ship and cargo type) in words
// the four channels by index (as in AisTelemetry) and by the channel letter of AisVessel / AisRxRecord / the !AIVDM sentence
inline int aisChannelIndex(char c) { return c == 'B' ? 1 : c == 'C' ? 2 : c == 'D' ? 3 : 0; }
inline char aisChannelLetter(int i) { return "ABCD"[i & 3]; }
inline double aisChannelHz(int i) { static const double f[4] = {161.975e6, 162.025e6, 156.775e6, 156.825e6}; return f[i & 3]; }
inline const char* aisChannelLabel(int i) { static const char* const n[4] = {"1", "2", "75", "76"}; return n[i & 3]; }
inline const char* aisChannelName(int i) { static const char* const n[4] = {"AIS 1 (87B)", "AIS 2 (88B)", "Long range 75", "Long range 76"}; return n[i & 3]; }

inline const char* aisShipTypeText(int t) {
    if (t < 0 || t > 99) return "";
    static const char* const tens[10] = {"Not available", "Reserved", "Wing in ground", "Special", "High-speed craft",
                                         "Special craft", "Passenger", "Cargo", "Tanker", "Other"};
    switch (t) {
    case 0: return "Not available";
    case 30: return "Fishing";
    case 31: return "Towing";
    case 32: return "Towing, large";
    case 33: return "Dredging or underwater ops";
    case 34: return "Diving ops";
    case 35: return "Military ops";
    case 36: return "Sailing";
    case 37: return "Pleasure craft";
    case 50: return "Pilot vessel";
    case 51: return "Search and rescue";
    case 52: return "Tug";
    case 53: return "Port tender";
    case 54: return "Anti-pollution";
    case 55: return "Law enforcement";
    case 58: return "Medical transport";
    case 59: return "Non-combatant ship";
    default: return tens[t / 10];
    }
}

inline const char* aisNavStatusText(int s) {
    static const char* const txt[16] = {"Under way (engine)", "At anchor", "Not under command", "Restricted manoeuvrability",
                                        "Constrained by draught", "Moored", "Aground", "Fishing", "Under way (sailing)",
                                        "Reserved 9", "Reserved 10", "Towing astern", "Pushing or towing alongside",
                                        "Reserved 13", "AIS-SART or similar", "Not defined"};
    return s >= 0 && s < 16 ? txt[s] : "";
}

inline const char* aisClassText(int c) {
    static const char* const txt[6] = {"Class A", "Class B", "Base station", "Aid to navigation", "SAR aircraft", "Other"};
    return c >= 0 && c < 6 ? txt[c] : "";
}

inline std::string aisSummary(const AisTelemetry& t) {
    if (t.state == 0) return "AIS: searching";
    char b[160];
    snprintf(b, sizeof b, "AIS: %s, %u stations, %llu messages (%llu bad)", t.state == 1 ? "signal" : "decoding", t.vesselCount,
             (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
