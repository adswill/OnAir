// Frequency-hopper analyser: a 256-point spectrum every 128 samples over the whole captured span finds the strongest narrow signal in each frame; a run of at least 3 frames at
// one frequency (within 1.6 bins, one missed frame allowed) is a dwell. From the dwells of the last 400 hops: the hop rate (one over the median time from the start of one dwell to the
// start of the next), the dwell and the guard time, the number of channels (distinct frequencies visited at least twice), the channel spacing (median distance of neighbouring channels) and the
// span. A hopper must visit at least 3 channels and give at least 40 dwells before it is reported. Resolution: the frequencies are good to a fraction of fs / 256 (a bin is fs / 256 wide,
// so channels closer than about 1.5 bins cannot be separated); the dwell time is blurred by the frame length (256 samples), so dwells shorter than about ten frames read long. One hopper at a
// time (the strongest in each frame); several at once make a mess of the numbers. It does not follow the sequence or predict the next channel. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr int kFft = 256, kStep = 128, kMinFrames = 3, kMaxDwells = 400, kMinDwells = 40;
constexpr double kPresentDb = 13.0, kTolBins = 1.6;

struct Dwell { double t0 = 0, dur = 0, hz = 0, db = 0; };

class HopDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs; binHz_ = fs / kFft; dt_ = (double)kStep / fs;
        win_.resize(kFft); for (int i = 0; i < kFft; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        reset(); return true;
    }
    void reset() override { ring_.assign(kFft, cf32(0, 0)); pos_ = 0; since_ = 0; fr_ = 0; cur_ = Dwell(); curN_ = 0; miss_ = 0; active_ = false; dw_.clear(); nDwellAll_ = 0; have_ = false; reported_ = false; lastRate_ = 0; lastN_ = 0; lastEval_ = -1e9; res_ = Res(); }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            ring_[pos_] = x[i]; pos_ = (pos_ + 1) % kFft;
            if (++since_ >= kStep) { since_ = 0; frame(); }
        }
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "listening"; return; }
        if (!res_.ok) { t.headline = fmt("%d dwells so far, not a hopper yet (needs %d dwells on at least 3 channels)", nDwellAll_, kMinDwells); return; }
        t.headline = fmt("hopping %.0f hops/s over %d channels, %.1f kHz apart", res_.rate, res_.channels, res_.spacing / 1000);
        t.facts.push_back({"hop rate", fmt("%.1f hops/s (one every %.3f ms)", res_.rate, 1000 / res_.rate)});
        t.facts.push_back({"dwell", fmt("%.3f ms (guard about %.3f ms), frames of %.0f us", res_.dwell * 1000, res_.guard * 1000, dt_ * 1e6)});
        t.facts.push_back({"channels", fmt("%d visited at least twice", res_.channels)});
        t.facts.push_back({"channel spacing", fmt("%.2f kHz (median of neighbours)%s", res_.spacing / 1000, res_.grid ? ", on a regular grid" : ", not a regular grid")});
        t.facts.push_back({"span", fmt("%.1f kHz, from %s to %s", res_.span / 1000, f(res_.lo).c_str(), f(res_.hi).c_str())});
        t.facts.push_back({"level", fmt("%.1f dB over the noise", res_.db)});
        t.facts.push_back({"dwells", fmt("%d seen in all, %zu in the window", nDwellAll_, dw_.size())});
        t.facts.push_back({"resolution", fmt("%.2f kHz bins, %.0f us frames", binHz_ / 1000, dt_ * 1e6)});
    }
private:
    struct Res { bool ok = false; double rate = 0, dwell = 0, guard = 0, spacing = 0, span = 0, lo = 0, hi = 0, db = 0; int channels = 0; bool grid = false; };
    std::string f(double hz) const { return centre_ > 0 ? fmt("%.4f MHz", (centre_ + hz) / 1e6) : fmt("%+.1f kHz", hz / 1000); }
    void frame() {
        std::vector<cf32> a(kFft); for (int i = 0; i < kFft; i++) a[i] = ring_[(pos_ + (size_t)i) % kFft] * win_[i];
        fftx(a);
        std::vector<float> p(kFft); for (int k = 0; k < kFft; k++) p[k] = std::norm(a[k]);
        std::vector<float> s(p); std::nth_element(s.begin(), s.begin() + kFft / 2, s.end()); const double med = std::max((double)s[kFft / 2], 1e-30);
        int kp = 0; for (int k = 1; k < kFft; k++) if (p[k] > p[kp]) kp = k;
        const double t = (double)(++fr_) * dt_;
        const double db = 10 * std::log10(p[kp] / med);
        const bool present = db > kPresentDb && std::abs(kp < kFft / 2 ? kp : kp - kFft) > 1;
        double fq = 0;
        if (present) {
            const double la = std::log(p[(kp + kFft - 1) % kFft] + 1e-30), lb = std::log(p[kp] + 1e-30), lc = std::log(p[(kp + 1) % kFft] + 1e-30), den = la - 2 * lb + lc;
            fq = ((kp < kFft / 2 ? kp : kp - kFft) + (den < -1e-12 ? 0.5 * (la - lc) / den : 0.0)) * binHz_;
        }
        if (present && active_ && std::fabs(fq - cur_.hz / std::max(curN_, 1)) <= kTolBins * binHz_) { cur_.hz += fq; cur_.db += db; cur_.dur = t - cur_.t0 + dt_; curN_++; miss_ = 0; }
        else if (present) { closeDwell(); active_ = true; cur_ = Dwell(); cur_.t0 = t; cur_.hz = fq; cur_.db = db; cur_.dur = dt_; curN_ = 1; miss_ = 0; }
        else if (active_ && ++miss_ > 1) closeDwell();
        have_ = true;
    }
    void closeDwell() {
        if (!active_) return; active_ = false;
        if (curN_ < kMinFrames) return;
        Dwell d = cur_; d.hz /= curN_; d.db /= curN_; d.dur = curN_ * dt_;
        dw_.push_back(d); nDwellAll_++;
        if (dw_.size() > (size_t)kMaxDwells) dw_.erase(dw_.begin());
        if (nDwellAll_ % 20 == 0 && d.t0 - lastEval_ > 0.0) { lastEval_ = d.t0; evaluate(); }
        out->energy();
    }
    static double med(std::vector<double> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; }
    void evaluate() {
        Res r; const size_t n = dw_.size();
        if ((int)n >= kMinDwells) {
            std::vector<double> per, dur, gap; double dbs = 0;
            for (size_t i = 1; i < n; i++) { const double d = dw_[i].t0 - dw_[i - 1].t0; if (d > 0 && d < 0.5) { per.push_back(d); gap.push_back(d - dw_[i - 1].dur); } }
            for (auto& d : dw_) { dur.push_back(d.dur); dbs += d.db; }
            // channels: cluster the dwell frequencies
            std::vector<double> fq; for (auto& d : dw_) fq.push_back(d.hz); std::sort(fq.begin(), fq.end());
            std::vector<std::pair<double, int>> cl;                                  // centre, count
            { double sum = fq[0]; int c = 1; for (size_t i = 1; i < fq.size(); i++) { if (fq[i] - fq[i - 1] > 1.2 * binHz_) { cl.push_back({sum / c, c}); sum = 0; c = 0; } sum += fq[i]; c++; } cl.push_back({sum / c, c}); }
            std::vector<double> ch; for (auto& c : cl) if (c.second >= 2) ch.push_back(c.first);
            if (ch.size() >= 3 && per.size() >= 20) {
                std::vector<double> sp; for (size_t i = 1; i < ch.size(); i++) sp.push_back(ch[i] - ch[i - 1]);
                r.ok = true; const double mp = med(per); r.rate = 1.0 / mp; r.dwell = med(dur); r.guard = std::max(0.0, med(gap)); r.channels = (int)ch.size();
                r.spacing = med(sp); r.lo = ch.front(); r.hi = ch.back(); r.span = ch.back() - ch.front() + r.spacing; r.db = dbs / n;
                int onGrid = 0; for (double c : ch) { const double q = (c - ch.front()) / r.spacing; if (std::fabs(q - std::round(q)) < 0.15) onGrid++; } r.grid = onGrid >= (int)(0.9 * ch.size());
            }
        }
        res_ = r;
        if (!r.ok) return;
        out->good((float)r.db, 0);
        const bool changed = !reported_ || std::fabs(r.rate - lastRate_) > 0.05 * lastRate_ || r.channels != lastN_;
        if (changed) {
            reported_ = true; lastRate_ = r.rate; lastN_ = r.channels;
            out->row("Hopper", fmt("%.0f hops/s", r.rate), fmt("%d channels %.1f kHz apart over %.0f kHz (%s to %s), dwell %.2f ms, guard %.2f ms", r.channels, r.spacing / 1000, r.span / 1000, f(r.lo).c_str(), f(r.hi).c_str(), r.dwell * 1000, r.guard * 1000), (float)r.db);
        }
    }
    double fs_ = 1, binHz_ = 1, dt_ = 1, centre_ = 0, lastEval_ = -1e9, lastRate_ = 0; size_t pos_ = 0; int since_ = 0, curN_ = 0, miss_ = 0, nDwellAll_ = 0, lastN_ = 0; long fr_ = 0;
    bool active_ = false, have_ = false, reported_ = false; Dwell cur_; Res res_; std::vector<float> win_; std::vector<cf32> ring_; std::vector<Dwell> dw_;
};
}

std::unique_ptr<Decoder> makeHopAnalyser() { return std::make_unique<HopDec>(); }

} // namespace dect2::x40
