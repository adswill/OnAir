// FengYun geostationary telemetry (FY-4A / FY-4B LRIT, FY-4A HRIT, FY-2 S-VISSR), shared by the receiver and the UI.
// The first members are those of GoesTelemetry (copied into RxTelemetry: keep them and their meaning).
#pragma once
#include "dect2/goes/goes_tel.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// The links, in the order of the mode list (families 232 - 235)
enum FygeoLink { kFy4aLrit = 0, kFy4bLrit = 1, kFy4aHrit = 2, kFy2Svissr = 3, kFygeoLinks = 4 };

inline const char* fygeoLinkName(int link) {
    switch (link) {
    case kFy4bLrit: return "FY-4B LRIT";
    case kFy4aHrit: return "FY-4A HRIT-II/III";
    case kFy2Svissr: return "FY-2 S-VISSR";
    default: return "FY-4A LRIT";
    }
}
inline double fygeoSymbolRate(int link) {
    switch (link) {
    case kFy4bLrit: return 120e3;
    case kFy4aHrit: return 1e6;
    case kFy2Svissr: return 660e3;
    default: return 90e3;
    }
}
inline bool fygeoIsS2(int link) { return link != kFy2Svissr; }
inline double fygeoMhz(int link) {
    switch (link) {
    case kFy4aHrit: return 1679.0;
    case kFy2Svissr: return 1687.5;
    default: return 1697.0;
    }
}
// the DVB-S2 MODCOD the link uses (EN 302 307 table: 3 QPSK 2/5, 10 QPSK 8/9, 9 QPSK 5/6); 0 for S-VISSR
inline int fygeoModcod(int link) { return link == kFy4aLrit ? 3 : link == kFy4bLrit ? 10 : link == kFy4aHrit ? 9 : 0; }
inline const char* fygeoModcodName(int link) {
    return link == kFy4aLrit ? "QPSK 2/5" : link == kFy4bLrit ? "QPSK 8/9" : link == kFy4aHrit ? "QPSK 5/6" : "BPSK (NRZ-M)";
}
// the lowest Es/N0 (dB) at which the link's code gives quasi error free frames on an ideal AWGN channel: EN 302 307-1 table 13, normal frames, no pilots.
// S-VISSR has no code: the figure is the Es/N0 at which differential BPSK (coherent detection) reaches a bit error ratio of 1e-3.
inline double fygeoIdealEsN0(int link) {
    switch (link) {
    case kFy4aLrit: return -0.30;
    case kFy4bLrit: return 6.20;
    case kFy4aHrit: return 5.18;
    default: return 7.4;
    }
}

struct FygeoTelemetry : GoesTelemetry {
    int link = 0;
    std::string signalInfo;            // what the receiver sees: "DVB-S2 QPSK 2/5, 90.0 ksym/s"
    std::string hint;                  // why nothing decodes yet (plain text), empty when all is well
    // FY-4 (DVB-S2 carrier, transport stream, CADUs)
    bool s2Spectrum = false, s2Timing = false, s2Carrier = false, s2Frame = false, s2Fec = false, s2Ts = false;
    std::string s2Modulation, s2CodeRate;
    int s2Modcod = -1;
    double s2SymbolRate = 0;
    float s2MerDb = 0, s2PreFecBer = -1;
    uint64_t tsPackets = 0, caduCount = 0, tsErrors = 0, encryptedFiles = 0, imageSegments = 0, imagesDone = 0;
    std::vector<std::pair<int, uint64_t>> pids;      // transport stream PIDs and their packet counts
    // S-VISSR (uncoded BPSK)
    uint64_t svFrames = 0, svSyncs = 0, svRejected = 0;
    uint64_t svUsable = 0;             // frames whose preamble bit error ratio was under 2 % (pictures that are worth looking at)
    double svPreambleBer = -1;         // bit error ratio over the 10000 known preamble bits of the last frame (-1: none yet)
    double svDataBer = -1;             // from the zero spare field of the frames (-1: none yet)
    int svLine = -1;                   // line counter of the last frame
    int svImages = 0;                  // pictures started
};

inline std::string fygeoSummary(const FygeoTelemetry& t) {
    char b[240];
    const char* n = fygeoLinkName(t.link);
    if (t.state >= 3) {
        if (t.link == kFy2Svissr)
            snprintf(b, sizeof b, "%s: %llu lines, line %d, %zu products, BER %.1e, Es/N0 %.1f dB", n, (unsigned long long)t.svFrames, t.svLine, t.products.size(),
                     std::max(0.0, t.svPreambleBer), t.snrDb);
        else
            snprintf(b, sizeof b, "%s: %llu LDPC frames (%llu bad), %llu CADUs, %zu products, Es/N0 %.1f dB", n, (unsigned long long)t.blocksOk,
                     (unsigned long long)t.blocksBad, (unsigned long long)t.caduCount, t.products.size(), t.snrDb);
        return b;
    }
    if (t.state == 2) { snprintf(b, sizeof b, "%s: frames found, waiting for data", n); return b; }
    if (t.state == 1) { snprintf(b, sizeof b, "%s: carrier locked (%.1f kHz), looking for frames", n, t.cfoHz / 1000); return b; }
    snprintf(b, sizeof b, "%s: searching for the %.0f ksym/s carrier", n, fygeoSymbolRate(t.link) / 1e3);
    return b;
}

} // namespace dect2
