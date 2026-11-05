// DME telemetry. RxTelemetry::dme, engine standard 123 (family 121).
//
// The DME receiver reports the X or Y channel of the beacon, the pulse-pair rate, the envelope level and the Morse ident.
// The channel number comes from the tuned frequency (ICAO Annex 10 channel plan), the X / Y letter from the pulse spacing.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace dect2 {

struct DmeTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no pulse pairs yet, 1 pairs but no ident letter yet, 2 ident letters read
    double cfoHz = 0;              // not measured
    float snrDb = 0;               // not measured
    bool dataValid = false;        // a pair rate is measured
    uint64_t blocksOk = 0;         // ident letters decoded
    uint64_t blocksBad = 0;        // not used
    int channel = 0;               // 0 none, 1 X (12 us pair spacing), 2 Y (30 us reply spacing, 36 us for an aircraft interrogation)
    double pairRatePps = 0;        // pulse pairs per second over the last second
    int pairsX = 0, pairsY = 0;    // pairs counted since the start
    int pairsYInterrogation = 0;   // of pairsY, the 36 us ones: an aircraft interrogating (the radio is on an interrogation frequency)
    double identIntervalUs = 0;    // interval of the keyed ident pairs (740.7 us at 1350 pairs/s)
    double centerHz = 0;           // the tuned frequency, 0 when not known
    int channelNumber = 0;         // the DME channel of that frequency as a reply frequency (1 - 126), 0 when it is not one
    bool channelIsY = false;       // that channel is a Y channel
    float levelDb = -200;          // envelope peak level, dB
    bool identKeyed = false;       // the ident tone is on now
    std::string ident;             // decoded ident letters, a space between words
};

inline const char* dmeChannelName(int channel) {
    return channel == 1 ? "X" : channel == 2 ? "Y" : "none";
}

// "17X" when the plan gives the channel of the tuned frequency, else just the letter seen in the pulse spacing
inline std::string dmeChannelLabel(const DmeTelemetry& t) {
    if (t.channelNumber > 0) return std::to_string(t.channelNumber) + (t.channelIsY ? "Y" : "X");
    return dmeChannelName(t.channel);
}

inline std::string dmeSummary(const DmeTelemetry& t) {
    char b[160];
    if (t.state == 0) return "DME: no pulse pairs";
    if (t.ident.empty()) {
        snprintf(b, sizeof b, "DME %s, %.0f pairs/s, no ident yet", dmeChannelLabel(t).c_str(), t.pairRatePps);
        return b;
    }
    snprintf(b, sizeof b, "DME %s, %.0f pairs/s, ident %s", dmeChannelLabel(t).c_str(), t.pairRatePps, t.ident.c_str());
    return b;
}

} // namespace dect2
