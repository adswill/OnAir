// Persistent carrier finder ("birdies": the narrow, steady carriers of a wide capture that are not a signal you tuned for: radio spurs, switching supplies, a neighbour's equipment, beacons).
// Same snapshots and tracks as the intermodulation finder (16384-point Hann spectra, 0.5 s snapshots, peaks 10 dB over the noise). A track is listed when it was present in at least 90 % of the
// last minute (and at least 10 s in all). For each: frequency (to a few Hz), level over the noise and how steady it is, the drift in Hz per minute (straight-line fit) and the frequency scatter,
// and whether it is a bare carrier (at least 75 % of the power within +-8 bins lies within +-2 bins of the peak) or something modulated or wide. A new carrier and a vanished one are logged.
// It lists; it does not say what the source is. Receive only.
#include "x40_tracks.h"

namespace dect2::x40 {

namespace {
constexpr double kSnapSec = 0.5, kThrDb = 10.0;
constexpr int kMinSnaps = 20, kWindow = 120, kEvery = 4;

struct Rep { double hz = 0; bool active = true; int missing = 0; };
struct Item { double hz = 0, level = 0, sdLevel = 0, driftMin = 0, jitter = 0, narrow = 0; int seen = 0, of = 0; };

class BirdieDec : public Decoder {
public:
    bool configure(double fs) override { fs_ = fs; if (!tr_.configure(fs, kSnapSec, kThrDb)) return false; tr_.onSnapshot = [this]() { snapshot(); }; reset(); return true; }
    void reset() override { tr_.reset(); reps_.clear(); items_.clear(); }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override { tr_.feed(x, n); }
    void fill(X40Telemetry& t) const override {
        if (tr_.snapshots() < kMinSnaps) { t.headline = "collecting spectra"; return; }
        t.headline = items_.empty() ? "no persistent narrow carriers" : fmt("%zu persistent carriers; strongest %s at %.0f dB over the noise", items_.size(), f(items_[0].hz).c_str(), items_[0].level);
        for (size_t i = 0; i < items_.size() && i < 10; i++) {
            const Item& x = items_[i];
            t.facts.push_back({fmt("carrier %zu", i + 1), fmt("%s, %.0f dB over the noise (+-%.1f dB), %s, drift %+.1f Hz/min, scatter %.1f Hz, present %d of %d snapshots", f(x.hz).c_str(), x.level, x.sdLevel, x.narrow >= 0.75 ? "bare carrier" : "modulated or wide", x.driftMin, x.jitter, x.seen, x.of)});
        }
        t.facts.push_back({"snapshots", fmt("%d of %.1f s, bins of %.1f Hz", tr_.snapshots(), kSnapSec, tr_.binHz())});
    }
private:
    std::string f(double hz) const { return centre_ > 0 ? fmt("%.6f MHz", (centre_ + hz) / 1e6) : fmt("%+.2f kHz", hz / 1000); }
    void snapshot() {
        const int s = tr_.snapshots();
        if (s < kMinSnaps || (s - kMinSnaps) % kEvery != 0) return;
        evaluate(s);
    }
    void evaluate(int snap) {
        auto& T = tr_.tracks();
        std::vector<Item> items;
        for (const Track& t : T) {
            const int n = std::min<int>((int)t.lv.size(), kWindow); if (n < kMinSnaps || t.lastSeen < snap - 2) continue;
            int seen = 0; double sl = 0, sll = 0; std::vector<std::pair<double, double>> xy;
            for (int i = (int)t.lv.size() - n; i < (int)t.lv.size(); i++) { const float v = t.lv[(size_t)i], q = t.fq[(size_t)i]; if (std::isnan(v)) continue; seen++; sl += v; sll += (double)v * v; xy.push_back({(double)i * kSnapSec, q}); }
            if (seen < (int)(0.9 * n)) continue;
            Item x; x.hz = t.hz; x.level = sl / seen; x.sdLevel = std::sqrt(std::max(0.0, sll / seen - x.level * x.level)); x.narrow = t.narrow; x.seen = seen; x.of = n;
            double sx = 0, sy = 0, sxx = 0, sxy = 0; for (auto& p : xy) { sx += p.first; sy += p.second; sxx += p.first * p.first; sxy += p.first * p.second; }
            const double m = (double)xy.size(), den = m * sxx - sx * sx; const double slope = den > 0 ? (m * sxy - sx * sy) / den : 0;
            double rs = 0; for (auto& p : xy) { const double r = p.second - (sy / m + slope * (p.first - sx / m)); rs += r * r; }
            x.driftMin = slope * 60; x.jitter = std::sqrt(rs / m);
            items.push_back(x);
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.level > b.level; });
        const double tol = 3 * tr_.binHz();
        for (auto& x : items) {
            bool known = false; for (auto& r : reps_) if (std::fabs(r.hz - x.hz) < tol) { known = true; r.hz = x.hz; r.missing = 0; if (!r.active) { r.active = true; out->row("Carrier", f(x.hz), "returned", (float)x.level); } }
            if (!known) { reps_.push_back({x.hz, true, 0}); out->row("Carrier", f(x.hz), fmt("%.0f dB over the noise, %s, drift %+.1f Hz/min, scatter %.1f Hz", x.level, x.narrow >= 0.75 ? "bare carrier" : "modulated or wide", x.driftMin, x.jitter), (float)x.level); }
        }
        for (auto& r : reps_) {
            bool there = false; for (auto& x : items) if (std::fabs(r.hz - x.hz) < tol) there = true;
            if (!there && r.active && ++r.missing >= 3) { r.active = false; out->row("Gone", f(r.hz), "no longer there", -120.f); }
        }
        items_ = items;
        if (!items_.empty()) out->good((float)items_[0].level, 0); else out->energy();
    }
    PeakTracker tr_; double fs_ = 1, centre_ = 0; std::vector<Rep> reps_; std::vector<Item> items_;
};
}

std::unique_ptr<Decoder> makeBirdie() { return std::make_unique<BirdieDec>(); }

} // namespace dect2::x40
