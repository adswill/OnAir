// ACARS / VDL2 channel load (4.0): per frequency, the message rate and the share of messages that were good (decoded without error), over a window,
// and a ranking of which frequencies are worth a receiver in a multi-channel setup with limited bandwidth. Pure code.
// Why: ACARS and VDL2 use several channels per region and most traffic is on two or three of them; the user picks those first.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace dect2 {

struct AcarsChanLoad { double freqMHz = 0; int msgs = 0, bad = 0; double perMin = 0; double goodPct = 0; double cumulativeSharePct = 0; };

class AcarsLoad {
public:
    void add(double freqMHz, bool ok, double tS) { v_.push_back({tS, (long)std::lround(freqMHz * 1e4), ok}); if (v_.size() > 50000) v_.erase(v_.begin(), v_.begin() + 10000); }
    // most messages first; cumulativeSharePct: the share of all traffic carried by this and the busier channels
    std::vector<AcarsChanLoad> ranking(double nowS, double windowS) const {
        std::map<long, AcarsChanLoad> m; int total = 0;
        for (const auto& e : v_) if (e.t >= nowS - windowS && e.t <= nowS) { auto& c = m[e.f]; c.freqMHz = e.f / 1e4; c.msgs++; if (!e.ok) c.bad++; total++; }
        std::vector<AcarsChanLoad> out;
        for (auto& kv : m) { auto c = kv.second; c.perMin = windowS > 0 ? c.msgs * 60.0 / windowS : 0; c.goodPct = 100.0 * (c.msgs - c.bad) / c.msgs; out.push_back(c); }
        std::sort(out.begin(), out.end(), [](const AcarsChanLoad& a, const AcarsChanLoad& b) { return a.msgs > b.msgs; });
        int cum = 0; for (auto& c : out) { cum += c.msgs; c.cumulativeSharePct = total ? 100.0 * cum / total : 0; }
        return out;
    }
    // the fewest channels that carry at least sharePct of the traffic
    std::vector<double> bestChannels(double nowS, double windowS, double sharePct = 90) const {
        std::vector<double> f;
        for (const auto& c : ranking(nowS, windowS)) { f.push_back(c.freqMHz); if (c.cumulativeSharePct >= sharePct) break; }
        return f;
    }
private:
    struct E { double t; long f; bool ok; };
    std::vector<E> v_;
};

} // namespace dect2
