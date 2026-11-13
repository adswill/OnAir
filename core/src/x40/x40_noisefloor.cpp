// Noise floor logger for a wide capture: the receiver's noise floor in 16 sub-bands across the span, once a second, over a long time, so a rise of the noise (a switching supply, a solar inverter, an LED lamp,
// a neighbour's equipment, the weather) shows up as a number and a time. A 4096-point Hann spectrum every frame; in each sub-band the median bin power (so narrow signals do not count: only noise that
// raises most of the band does) averaged over one second, in dB against full scale. Per sub-band the last hour's 10th, 50th and 90th percentiles are kept; the baseline is the 20th percentile of the last
// 10 minutes. A rise of 3 dB or more over the baseline that lasts 3 s is logged, and when it ends. The band-wide floor (median of the sub-bands) is shown with its change over the last 10 minutes.
// A change of the radio's gain moves every sub-band at once: it is logged as one "Gain or whole-band change" row when 12 or more of the 16 sub-bands move together by 3 dB or more; reset after changing the gain.
// The numbers are relative (dB against full scale of the radio), not calibrated. Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr int kFft = 4096, kBands = 16, kHist = 3600, kBaseSec = 600;
constexpr double kEdge = 0.45, kRiseDb = 3.0, kHoldSec = 3.0, kReportSec = 1.0;

struct Band { std::deque<float> h; double acc = 0; int nAcc = 0; bool rising = false; double riseSince = -1, riseStart = 0; float peak = 0; };

class NoiseFloorDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs; binHz_ = fs / kFft;
        win_.resize(kFft); double s2 = 0; for (int i = 0; i < kFft; i++) { win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft)); s2 += (double)win_[i] * win_[i]; } sw2_ = s2;
        // the sub-band of every bin
        bandOf_.assign(kFft, -1); const int half = (int)(kEdge * kFft); const int per = std::max(1, 2 * half / kBands);
        for (int k = 0; k < kFft; k++) { const int j = k < kFft / 2 ? k : k - kFft; if (std::abs(j) < 3 || std::abs(j) > half) continue; const int b = (j + half) / per; if (b >= 0 && b < kBands) bandOf_[k] = b; }
        framesPerSec_ = std::max(1, (int)std::lround(kReportSec * fs / kFft)); reset(); return true;
    }
    void reset() override { buf_.clear(); for (auto& b : bands_) b = Band(); fr_ = 0; t_ = 0; have_ = false; wide_ = 0; lastWide_ = -1e9; wideBase_.clear(); }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override { for (size_t i = 0; i < n; i++) { buf_.push_back(x[i]); if ((int)buf_.size() == kFft) { frame(); buf_.clear(); } } }
    void fill(X40Telemetry& t) const override {
        if (!have_ || t_ < 10) { t.headline = "measuring the noise floor"; return; }
        const double w = wideNow_, wb = wideBase_.empty() ? w : wideBase_.front();
        t.headline = fmt("floor %.1f dB re full scale per bin (%+.1f dB over the last 10 min), %d of %d sub-bands raised", w, w - wb, nRaised(), kBands);
        t.facts.push_back({"band-wide floor", fmt("%.2f dB re full scale per bin (%+.2f dB against 10 minutes ago)", w, w - wb)});
        for (int b = 0; b < kBands; b++) {
            const Band& x = bands_[b]; if (x.h.empty()) continue;
            std::vector<float> v(x.h.begin(), x.h.end()); std::sort(v.begin(), v.end());
            auto q = [&](double f) { return v[std::min(v.size() - 1, (size_t)(f * (double)v.size()))]; };
            t.facts.push_back({fmt("sub-band %d, %s", b + 1, f(bandCentre(b)).c_str()), fmt("now %.1f dB, baseline %.1f, 10 / 50 / 90 %% over %.0f min: %.1f / %.1f / %.1f dB%s", x.h.back(), baseline(x), (double)v.size() / 60.0, q(0.1), q(0.5), q(0.9), x.rising ? ", RAISED" : "")});
        }
        t.facts.push_back({"note", "relative numbers (dB re full scale), not calibrated; a gain change moves all sub-bands"});
    }
private:
    std::string f(double hz) const { return centre_ > 0 ? fmt("%.3f MHz", (centre_ + hz) / 1e6) : fmt("%+.0f kHz", hz / 1000); }
    double bandCentre(int b) const { const int half = (int)(kEdge * kFft); const int per = std::max(1, 2 * half / kBands); return ((b * per + per / 2) - half) * binHz_; }
    static float baseline(const Band& b) { const size_t n = std::min<size_t>(b.h.size(), (size_t)kBaseSec); std::vector<float> v(b.h.end() - (long)n, b.h.end()); std::nth_element(v.begin(), v.begin() + (long)(v.size() / 5), v.end()); return v[v.size() / 5]; }
    int nRaised() const { int n = 0; for (auto& b : bands_) n += b.rising; return n; }
    void frame() {
        have_ = true;
        std::vector<cf32> a(kFft); for (int i = 0; i < kFft; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        // the median power of each sub-band's bins in this frame
        std::vector<std::vector<float>> pb(kBands);
        for (int k = 0; k < kFft; k++) if (bandOf_[k] >= 0) pb[bandOf_[k]].push_back(std::norm(a[k]) / (float)((double)kFft * sw2_));
        for (int b = 0; b < kBands; b++) { auto& v = pb[b]; if (v.empty()) continue; std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end()); bands_[b].acc += v[v.size() / 2] / 0.693; bands_[b].nAcc++; }
        if (++fr_ < framesPerSec_) return;
        fr_ = 0; t_ += kReportSec;
        std::vector<float> now(kBands, -200.f);
        for (int b = 0; b < kBands; b++) { Band& x = bands_[b]; if (!x.nAcc) continue; now[b] = (float)(10 * std::log10(std::max(x.acc / x.nAcc, 1e-20))); x.acc = 0; x.nAcc = 0; x.h.push_back(now[b]); if (x.h.size() > (size_t)kHist) x.h.pop_front(); }
        std::vector<float> s(now); std::sort(s.begin(), s.end()); wideNow_ = s[kBands / 2];
        wideHist_.push_back((float)wideNow_); if (wideHist_.size() > (size_t)kBaseSec) wideHist_.pop_front(); wideBase_.assign(1, wideHist_.front());
        if (t_ < 30) return;
        int moved = 0; for (int b = 0; b < kBands; b++) { const Band& x = bands_[b]; if (x.h.size() > 20 && x.h.back() - baseline(x) >= kRiseDb) moved++; }
        if (moved >= 12) { if (t_ - lastWide_ > 120) { lastWide_ = t_; out->row("Whole band", fmt("%+.1f dB", wideNow_ - (wideHist_.size() > 30 ? wideHist_.front() : wideNow_)), "gain or whole-band change: 12 or more of the 16 sub-bands moved together", (float)wideNow_); } for (auto& x : bands_) { x.rising = false; x.riseSince = -1; } return; }
        for (int b = 0; b < kBands; b++) {
            Band& x = bands_[b]; if (x.h.size() < 30) continue;
            const float over = x.h.back() - baseline(x);
            if (!x.rising) {
                if (over >= (float)kRiseDb) { if (x.riseSince < 0) x.riseSince = t_; if (t_ - x.riseSince >= kHoldSec) { x.rising = true; x.riseStart = x.riseSince; x.peak = over; out->row("Noise rise", f(bandCentre(b)), fmt("sub-band %d: %.1f dB over its baseline since %.0f s (now %.1f dB, baseline %.1f)", b + 1, over, x.riseStart, x.h.back(), baseline(x)), x.h.back()); out->bad(); } }
                else x.riseSince = -1;
            } else {
                x.peak = std::max(x.peak, over);
                if (over < 1.5f) { x.rising = false; x.riseSince = -1; out->row("Noise back", f(bandCentre(b)), fmt("sub-band %d: back to its baseline after %.0f s, the largest rise %.1f dB", b + 1, t_ - x.riseStart, x.peak), x.h.back()); }
            }
        }
        if (nRaised() == 0) out->good((float)wideNow_, 0);
    }
    double fs_ = 1, binHz_ = 1, sw2_ = 1, centre_ = 0, t_ = 0, wideNow_ = 0, lastWide_ = -1e9; int framesPerSec_ = 1, fr_ = 0; double wide_ = 0; bool have_ = false;
    std::vector<float> win_; std::vector<cf32> buf_; std::vector<int> bandOf_; Band bands_[kBands]; std::deque<float> wideHist_; std::vector<float> wideBase_;
};
}

std::unique_ptr<Decoder> makeNoiseFloor() { return std::make_unique<NoiseFloorDec>(); }

} // namespace dect2::x40
