// APRS / packet channel use: who occupies the channel and how often each station beacons. From the frames heard in a time window: per station the
// frame count, the median and shortest gap between its frames, the estimated airtime and its share of the window, how many frames repeated the
// previous one unchanged, and flags a sysop acts on: "fast" (beacons faster than the usual 60 s minimum), "hog" (a big share of the airtime),
// "repeat" (the same text again and again at short gaps). Also the channel occupancy over the window. Pure code on PacketFrameInfo.
#pragma once
#include "dect2/packet/packet_tel.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct BeaconStat {
    std::string call;
    int frames = 0;
    double medianGapS = 0;   // 0 with fewer than two frames
    double minGapS = 0;
    double airtimeS = 0;
    double sharePct = 0;     // of the window
    int repeats = 0;         // frames whose text equals the station's previous frame
    bool fast = false, hog = false, repeat = false;
};
struct ChannelUse {
    double occupancyPct = 0;     // all airtime / window
    std::vector<BeaconStat> stations;   // most airtime first
};

// bytes on the air for one frame: two addresses, the digipeaters, control + PID, the text, the FCS and the flags; 20 % bit stuffing; plus the TX delay
inline double frameAirtimeS(const PacketFrameInfo& f) {
    int digis = 0;
    if (!f.path.empty()) { digis = 1; for (char c : f.path) if (c == ',') digis++; }
    const double bytes = 14.0 + 7.0 * digis + 2.0 + (double)f.info.size() + 2.0 + 2.0;
    const double baud = f.baud > 0 ? f.baud : 1200;
    return bytes * 8.0 * 1.2 / baud + (baud >= 9600 ? 0.05 : 0.3);
}

// nowS: the end of the window; windowS: its length. Frames outside it are ignored; frames may come in any order.
inline ChannelUse analyseChannel(const std::vector<PacketFrameInfo>& frames, double nowS, double windowS) {
    ChannelUse u;
    if (windowS <= 0) return u;
    std::map<std::string, std::vector<const PacketFrameInfo*>> by;
    double total = 0;
    for (const auto& f : frames) {
        if (f.from.empty() || f.timeSec < nowS - windowS || f.timeSec > nowS) continue;
        by[f.from].push_back(&f);
        total += frameAirtimeS(f);
    }
    for (auto& kv : by) {
        auto& v = kv.second;
        std::stable_sort(v.begin(), v.end(), [](const PacketFrameInfo* a, const PacketFrameInfo* b) { return a->timeSec < b->timeSec; });
        BeaconStat s; s.call = kv.first; s.frames = (int)v.size();
        std::vector<double> gaps;
        int run = 0, bestRun = 0; double runGapMax = 0;
        for (size_t i = 0; i < v.size(); i++) {
            s.airtimeS += frameAirtimeS(*v[i]);
            if (i == 0) continue;
            const double gap = v[i]->timeSec - v[i - 1]->timeSec;
            gaps.push_back(gap);
            if (v[i]->info == v[i - 1]->info) {
                s.repeats++; run++;
                runGapMax = std::max(runGapMax, gap);
                if (run > bestRun && runGapMax < 120.0) bestRun = run;
            } else { run = 0; runGapMax = 0; }
        }
        if (!gaps.empty()) {
            std::sort(gaps.begin(), gaps.end());
            s.medianGapS = gaps[gaps.size() / 2]; s.minGapS = gaps.front();
        }
        s.sharePct = 100.0 * s.airtimeS / windowS;
        s.fast = gaps.size() >= 3 && s.medianGapS < 60.0;
        s.hog = s.sharePct >= 15.0;
        s.repeat = bestRun >= 4;
        u.stations.push_back(s);
    }
    u.occupancyPct = 100.0 * total / windowS;
    std::stable_sort(u.stations.begin(), u.stations.end(), [](const BeaconStat& a, const BeaconStat& b) { return a.airtimeS > b.airtimeS; });
    return u;
}

}  // namespace dect2
