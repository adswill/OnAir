// Transmitter frequency stability meter (a handheld, a repeater input, a beacon: one 12.5 / 25 kHz channel): for every transmission of 1 s or more the carrier frequency against time, so that the warm-up and drift
// of a transmitter show as numbers. The channel power is taken in 20 ms blocks (a transmission starts 10 dB over the noise floor, the 10th percentile of the last minute, and ends when it has been under 5 dB over
// for 0.3 s); during a transmission the mean instantaneous frequency (phase steps of the FM discriminator, 24 ksps) is taken every 50 ms. For each transmission: the frequency at the start (first 100 ms) and at the end (last 300 ms)
// against the mean, the key-up step (start minus end), the time to settle within the larger of 10 Hz and 10 % of the key-up step of its final value, the drift in Hz per second over the second half (straight-line fit), the
// mean offset from the tuned frequency (and in ppm with the tuned frequency known) and the scatter. Voice modulation is averaged out over 50 ms only if it is symmetrical: speech skews single readings, so the meter is
// best with a carrier, a tone or a quiet transmission; at 25 Hz of readings noise the step and settle times are good to about 50 ms and 10 Hz. Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 24000, kCut = 8000, kBlockSec = 0.02, kStepSec = 0.05, kOnDb = 10.0, kOffDb = 5.0, kHangSec = 0.3, kMinTxSec = 1.0;

struct Pt { double t, hz; };

class TxStabDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, std::min(kCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate(); blockN_ = (size_t)(kBlockSec * rate_); stepN_ = (size_t)(kStepSec * rate_);
        reset(); return true;
    }
    void reset() override { dec_.reset(); prev_ = cf32(1, 0); skip_ = (size_t)(0.1 * rate_); accP_ = 0; nP_ = 0; accF_ = 0; nF_ = 0; hist_.clear(); floorDb_ = -120; haveFloor_ = false; on_ = false; belowSince_ = -1; pts_.clear(); t_ = 0.1; nTx_ = 0; have_ = false; res_ = Res(); blocks_ = 0; }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            const cf32 d = a * std::conj(prev_); prev_ = a;
            if (skip_) { skip_--; return; }
            const double p = std::norm(a); accP_ += p; nP_++;
            if (on_) { accF_ += std::atan2(d.imag(), d.real()); nF_++; }
            if (nP_ >= blockN_) block();
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!res_.ok) { t.headline = on_ ? fmt("transmitting for %.1f s", t_ - startT_) : nTx_ ? fmt("%d transmissions, none of 1 s or more analysed", nTx_) : "waiting for a transmission of 1 s or more"; return; }
        t.headline = fmt("last transmission %.1f s: start %+.0f Hz, final %+.0f Hz, settled in %.2f s, drift %+.1f Hz/s, mean %+.0f Hz%s", res_.len, res_.start, res_.fin, res_.settle, res_.drift, res_.mean, centre_ > 0 ? fmt(" (%+.2f ppm)", res_.mean / centre_ * 1e6).c_str() : "");
        t.facts.push_back({"transmission", fmt("%.1f s, %d readings of %.0f ms", res_.len, res_.n, kStepSec * 1000)});
        t.facts.push_back({"frequency at the start", fmt("%+.0f Hz from the tuned frequency (first 100 ms)", res_.start)});
        t.facts.push_back({"frequency at the end", fmt("%+.0f Hz (last 300 ms)", res_.fin)});
        t.facts.push_back({"key-up step", fmt("%+.0f Hz (start minus end)", res_.start - res_.fin)});
        t.facts.push_back({"settling time", fmt("%.2f s to within %.0f Hz of the final frequency", res_.settle, res_.band)});
        t.facts.push_back({"drift (second half)", fmt("%+.2f Hz/s", res_.drift)});
        t.facts.push_back({"mean offset", centre_ > 0 ? fmt("%+.0f Hz = %+.2f ppm of %.4f MHz", res_.mean, res_.mean / centre_ * 1e6, centre_ / 1e6) : fmt("%+.0f Hz", res_.mean)});
        t.facts.push_back({"scatter", fmt("%.1f Hz RMS about the straight line of the second half", res_.sd)});
        t.facts.push_back({"transmissions seen", fmt("%d", nTx_)});
    }
private:
    struct Res { bool ok = false; double len = 0, start = 0, fin = 0, settle = 0, band = 0, drift = 0, mean = 0, sd = 0; int n = 0; };
    void block() {
        have_ = true; blocks_++;
        const double db = 10 * std::log10(std::max(accP_ / (double)nP_, 1e-18)); accP_ = 0; nP_ = 0; t_ += kBlockSec;
        hist_.push_back((float)db); if (hist_.size() > (size_t)(60 / kBlockSec)) hist_.pop_front();
        if (blocks_ % 50 == 0 || !haveFloor_) { if (t_ >= 1.0) { std::vector<float> v(hist_.begin(), hist_.end()); std::nth_element(v.begin(), v.begin() + (long)(v.size() / 10), v.end()); const double q = v[v.size() / 10]; if (!on_ || !haveFloor_) { floorDb_ = haveFloor_ ? 0.9 * floorDb_ + 0.1 * q : q; haveFloor_ = true; } } }
        if (!haveFloor_ || t_ < 3.0) return;
        const double over = db - floorDb_;
        if (!on_) { if (over > kOnDb) { on_ = true; startT_ = t_ - kBlockSec; belowSince_ = -1; pts_.clear(); accF_ = 0; nF_ = 0; stepT_ = t_; out->good(0, 0); } else out->energy(); return; }
        if (over < kOffDb) { if (belowSince_ < 0) belowSince_ = t_; if (t_ - belowSince_ >= kHangSec) { endTx(belowSince_); return; } } else belowSince_ = -1;
        if (nF_ >= stepN_) { const double f = accF_ / (double)nF_ * rate_ / (2 * kPi); pts_.push_back({t_ - 0.5 * (double)nF_ / rate_ - startT_, f}); accF_ = 0; nF_ = 0; }
    }
    void endTx(double endT) {
        on_ = false; nTx_++;
        const double len = endT - startT_;
        // drop the readings after the carrier went away
        while (!pts_.empty() && pts_.back().t + 0.05 > len) pts_.pop_back();      // a reading that reaches into the hang time holds noise
        if (len < kMinTxSec || pts_.size() < 12) return;
        Res r; r.ok = true; r.len = len; r.n = (int)pts_.size();
        auto meanOf = [&](double t0, double t1) { double s = 0; int c = 0; for (auto& p : pts_) if (p.t >= t0 && p.t < t1) { s += p.hz; c++; } return c ? s / c : 0.0; };
        r.start = meanOf(0.05, 0.2); r.fin = meanOf(len - 0.4, len + 1); { double s = 0; for (auto& p : pts_) s += p.hz; r.mean = s / (double)pts_.size(); }
        const double step = std::fabs(r.start - r.fin); r.band = std::max(10.0, 0.1 * step);
        // settling: the last reading outside the band around the final frequency
        double lastOut = 0; for (auto& p : pts_) if (std::fabs(p.hz - r.fin) > r.band) lastOut = p.t + kStepSec; r.settle = lastOut;
        // drift over the second half
        double st = 0, sy = 0, stt = 0, sty = 0; int c = 0; for (auto& p : pts_) if (p.t >= 0.5 * len) { st += p.t; sy += p.hz; stt += p.t * p.t; sty += p.t * p.hz; c++; }
        const double den = c * stt - st * st; r.drift = c > 3 && den > 0 ? (c * sty - st * sy) / den : 0;
        double rs = 0; if (c > 3 && den > 0) { const double b = r.drift, a0 = (sy - b * st) / c; for (auto& p : pts_) if (p.t >= 0.5 * len) { const double e = p.hz - (a0 + b * p.t); rs += e * e; } r.sd = std::sqrt(rs / c); }
        res_ = r;
        out->row("Transmission", fmt("%.1f s", len), fmt("start %+.0f Hz, final %+.0f Hz, settled in %.2f s (within %.0f Hz), drift %+.1f Hz/s, mean %+.0f Hz%s, scatter %.1f Hz", r.start, r.fin, r.settle, r.band, r.drift, r.mean, centre_ > 0 ? fmt(" = %+.2f ppm", r.mean / centre_ * 1e6).c_str() : "", r.sd), 0.f);
    }
    Decimator dec_; double rate_ = 1, accP_ = 0, accF_ = 0, floorDb_ = -120, belowSince_ = -1, startT_ = 0, stepT_ = 0, t_ = 0.1, centre_ = 0; size_t blockN_ = 1, stepN_ = 1, skip_ = 0, nP_ = 0, nF_ = 0; long blocks_ = 0; int nTx_ = 0;
    cf32 prev_{1, 0}; bool haveFloor_ = false, on_ = false, have_ = false; std::deque<float> hist_; std::vector<Pt> pts_; Res res_;
};
}

std::unique_ptr<Decoder> makeTxStability() { return std::make_unique<TxStabDec>(); }

} // namespace dect2::x40
