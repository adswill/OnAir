// Clear transmit frequency finder for slotted weak-signal modes (FT8, FT4, JS8, JT65): from what was decoded in the last slots, find the widest free
// gap in the audio passband for each slot parity (even / odd), and say which parity is the quieter one. Pure code.
// Why: calling on a crowded FT8 band on top of another station wastes the whole slot; the quiet gap and the quiet slot are the two choices that matter.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {

struct SlotSignal { double audioHz = 0; int parity = 0; double bwHz = 50; };   // parity 0 = even slot, 1 = odd slot
struct ClearFreq {
    double hz[2] = {0, 0};        // best TX audio frequency per parity (centre of the widest gap), 0 when there is no gap
    double gapHz[2] = {0, 0};     // its width
    int signals[2] = {0, 0};
    int quieterParity = 0;        // the parity with fewer signals (even when equal)
};

// lowHz..highHz: the passband (200 - 2900 usual). txBwHz: the width of our own signal, plus guardHz on each side.
inline ClearFreq findClearFreq(const std::vector<SlotSignal>& sigs, double lowHz = 200, double highHz = 2900, double txBwHz = 50, double guardHz = 10) {
    ClearFreq r;
    for (int p = 0; p < 2; p++) {
        std::vector<std::pair<double, double>> occ;   // occupied [a,b]
        for (const auto& s : sigs) if (s.parity == p) {
            r.signals[p]++;
            occ.push_back({s.audioHz - s.bwHz / 2 - guardHz, s.audioHz + s.bwHz / 2 + guardHz});
        }
        std::sort(occ.begin(), occ.end());
        double pos = lowHz, bestW = 0, bestC = 0;
        auto gap = [&](double a, double b) { if (b - a > bestW && b - a >= txBwHz) { bestW = b - a; bestC = (a + b) / 2; } };
        for (const auto& o : occ) { if (o.second <= pos) continue; if (o.first > pos) gap(pos, std::min(o.first, highHz)); pos = std::max(pos, o.second); if (pos >= highHz) break; }
        if (pos < highHz) gap(pos, highHz);
        r.hz[p] = bestW > 0 ? bestC : 0; r.gapHz[p] = bestW;
    }
    r.quieterParity = r.signals[1] < r.signals[0] ? 1 : 0;
    return r;
}

} // namespace dect2
