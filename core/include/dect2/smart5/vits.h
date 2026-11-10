// VITS analyser (3.9, docs/SMART5-tv-3.9.md item 15). Analogue broadcasters put test signals in the vertical interval
// (ITU-R BT.473 / BT.628 insertion test signals; CCIR lines 17, 18, 330, 331). When the decoder hands over a line, this finds
// what is on it without being told the line layout:
//   white bar      -> bar level and bar tilt (K-bar)
//   2T sine-squared pulse -> half-amplitude duration and the pulse-to-bar K factor  K_PB = 1/4 |B - P| / P x 100 %
//   multiburst     -> each packet's frequency and its level against the first packet (frequency response, dB)
//   staircase with subcarrier -> differential gain and differential phase
// Input: one line in IRE (0 = blanking, 100 = white), at any sample rate of 13.5 MHz or more.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct VitsBurst { double freqHz = 0, amp = 0, relDb = 0; };

struct VitsResult {
    bool bar = false;      double barIre = 0, barTiltPct = 0;
    bool pulse2T = false;  double pulseIre = 0, hadNs = 0, kpbPct = 0;
    bool multiburst = false; std::vector<VitsBurst> bursts;
    bool staircase = false; int steps = 0; double dgPct = 0, dpDeg = 0, subcarrierHz = 0;
    bool any() const { return bar || pulse2T || multiburst || staircase; }
    std::string text() const;
};

VitsResult analyseVitsLine(const std::vector<float>& ire, double fs);

} // namespace smart5
} // namespace dect2
