// Radar pulse analyser (S, C and X band marine, air-traffic and weather radars, or any pulsed emitter): the pulses of one dominant emitter are found in the envelope and
// measured, as a pulse-analysis receiver does: pulse width at the half-power point, time of arrival, level, the frequency of the carrier inside the pulse. The spacings (PRI)
// are classified: constant, staggered (a repeating sequence of 2 to 8 spacings), jittered (random spread) or irregular; the pulse level over the last 30 s gives the
// antenna's scan period (the beam sweeping past you once a turn). It measures the emitter: it says nothing of what the radar is. Receive only.
#include "x40_int.h"
#include <cmath>

namespace dect2::x40 {

namespace {
constexpr double kWorkMax = 2.5e6, kReportSec = 1.0, kScanWindow = 30.0;
constexpr float kSigmas = 6.5f;
constexpr double kPriWindow = 8.0;   // a scanning antenna is seen for a few ms per turn, so the PRI is taken over several turns

class RadarDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkMax));
        const double r = fs / d;
        if (!dec_.configure(fs, d, 0.42 * r)) return false;
        rate_ = dec_.outRate(); reset(); return true;
    }
    void reset() override {
        dec_.reset(); n_ = 0; mean_ = 0; var_ = 0; warm_ = 0; in_ = false; pul_.clear(); nextReport_ = kReportSec; have_ = false; cls_.clear(); cnt_ = 0; seen_ = 0;
        pri_ = pw_ = scan_ = 0; staggerN_ = 0; prfHz_ = 0; spread_ = 0; ifHz_ = 0; levelDb_ = -120; lastEmit_.clear(); cand_.clear(); history_.clear(); nPulses_ = 0; lastPulseT_ = -1e9;
    }
    void feed(const cf32* x, size_t n) override { dec_.process(x, n, [&](cf32 a) { step(a); }); }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (cls_.empty()) { t.headline = nPulses_ ? "pulses seen, no steady emitter" : "no pulses"; }
        else t.headline = fmt("%s, PRI %.1f us (%.1f Hz), width %.2f us%s", cls_.c_str(), pri_ * 1e6, prfHz_, pw_ * 1e6, scan_ > 0 ? fmt(", scan %.2f s", scan_).c_str() : "");
        t.facts.push_back({"pulses counted", fmt("%llu", (unsigned long long)nPulses_)});
        if (!cls_.empty()) {
            t.facts.push_back({"PRI", fmt("%.2f us mean, PRF %.2f Hz, spread %.2f %%", pri_ * 1e6, prfHz_, spread_ * 100)});
            t.facts.push_back({"PRI type", cls_});
            t.facts.push_back({"pulse width (half power)", fmt("%.3f us", pw_ * 1e6)});
            t.facts.push_back({"frequency inside the pulse", std::isnan(ifHz_) ? std::string("not measurable (pulses under 2.5 microseconds at this rate)") : fmt("%+.1f kHz from the tuned frequency", ifHz_ / 1000)});
            t.facts.push_back({"pulse level", fmt("%.1f dB (relative)", levelDb_)});
            t.facts.push_back({"antenna scan", scan_ > 0 ? fmt("%.2f s per turn (%.1f rpm)", scan_, 60.0 / scan_) : std::string("not seen")});
        }
    }
private:
    struct Pulse { double t, w, amp, f; };
    void step(cf32 a) {
        n_++; const double t = n_ / rate_; const float e = std::sqrt(0.5f * (std::norm(a) + std::norm(prevA_)));     // two samples of power: a pulse of a microsecond is two samples wide
        if (t < 0.02) { mean_ = e; var_ = 0; prevA_ = a; return; }
        if (warm_ < (uint64_t)(0.01 * rate_)) { warm_++; const float k = 1.f / (float)warm_; const float d = e - mean_; mean_ += k * d; var_ += k * (d * d - var_); prevA_ = a; return; }
        const float thr = mean_ + kSigmas * std::sqrt(std::max(var_, 0.f)) + 1e-9f;
        if (e > thr) {
            if (!in_) { in_ = true; pk_ = e; pkT_ = t; seg_.clear(); acc_ = cf32(0, 0); startT_ = t; }
            seg_.push_back(e); if (e > pk_) { pk_ = e; pkT_ = t; }
            acc_ += a * std::conj(prevA_);
            if (seg_.size() > (size_t)(0.002 * rate_)) { in_ = false; seg_.clear(); }       // longer than 2 ms: not a pulse of this analyser
        } else {
            if (in_) { endPulse(t); in_ = false; }
            const float k = 1.f / (float)(0.002 * rate_); const float d = e - mean_; mean_ += k * d; var_ += k * (d * d - var_);
        }
        prevA_ = a;
        if (t >= nextReport_) { nextReport_ = t + kReportSec; report(t); }
    }
    void endPulse(double t) {
        // width at half power (0.707 of the peak amplitude), interpolated
        const float half = pk_ * 0.7071f;
        const size_t n = seg_.size(); size_t ip = 0; for (size_t i = 0; i < n; i++) if (seg_[i] >= pk_) { ip = i; break; }
        double a = 0, b = (double)n;
        size_t i = ip; while (i > 0 && seg_[i - 1] >= half) i--;
        a = i > 0 ? (double)(i - 1) + (half - seg_[i - 1]) / std::max(1e-12f, seg_[i] - seg_[i - 1]) : 0.0;
        size_t j = ip; while (j + 1 < n && seg_[j + 1] >= half) j++;
        b = j + 1 < n ? (double)j + (seg_[j] - half) / std::max(1e-12f, seg_[j] - seg_[j + 1]) : (double)n;
        const double w = std::max(1.0, b - a) / rate_;
        const double f = n >= 5 ? std::atan2(acc_.imag(), acc_.real()) * rate_ / (2 * kPi) : std::nan("");   // under 5 samples the phase steps of the edges and the noise dominate
        if (n < 2) return;                                    // a single sample over the threshold is noise, not a pulse
        const double toa = startT_ + ((a + b) * 0.5) / rate_;
        pul_.push_back({toa, w, (double)pk_, f}); nPulses_++; lastPulseT_ = t; out->energy();
        (void)t;
    }
    static double median(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
    void report(double t) {
        have_ = true;
        history_.push_back({t, 0, 0, 0});
        // keep 30 s of pulses
        while (!pul_.empty() && pul_.front().t < t - kScanWindow) pul_.erase(pul_.begin());
        // pulses of the last 2 s for the PRI; the dominant PW class is the emitter
        std::vector<Pulse> w; for (auto& p : pul_) if (p.t > t - kPriWindow) w.push_back(p);
        std::string cls; double pri = 0, spread = 0; int stag = 0;
        if (w.size() >= 12) {
            std::vector<double> d; for (size_t i = 1; i < w.size(); i++) d.push_back(w[i].t - w[i - 1].t);
            // follow the emitter: the pulses before the first gap of more than 3 times the median spacing (a scan pause or another emitter) are used
            const double md0 = median(d);
            { std::vector<double> kept; for (double v : d) if (v < 3.0 * md0) kept.push_back(v); d.swap(kept); }     // the gaps between two passes of the beam are not spacings
            if (d.size() < 10) { cnt_ = 0; cand_.clear(); return; }
            const double md = median(d);
            // constant: 90 % of spacings within 0.6 % of the median
            int within = 0; for (double v : d) if (std::fabs(v - md) < 0.006 * md) within++;
            if (within >= 0.85 * (int)d.size()) { cls = "constant PRI"; pri = md; }
            else {
                // stagger: a repeating sequence of N spacings (compare with the spacing N steps later)
                for (int N = 2; N <= 8 && stag == 0; N++) {
                    int ok = 0, tot = 0;
                    for (size_t i = 0; i + N < d.size(); i++) { tot++; if (std::fabs(d[i] - d[i + N]) < 0.006 * d[i]) ok++; }
                    if (tot >= 8 && ok >= 0.85 * tot) stag = N;
                }
                if (stag) {
                    double s = 0; for (double v : d) s += v; pri = s / (double)d.size(); cls = fmt("staggered PRI, %d positions", stag);
                    std::vector<double> sorted(d.begin(), d.begin() + std::min<size_t>(d.size(), (size_t)stag)); std::sort(sorted.begin(), sorted.end());
                    spread = sorted.empty() ? 0 : (sorted.back() - sorted.front()) / pri;
                } else {
                    double s = 0, s2 = 0; int k = 0;
                    for (double v : d) if (v > 0.5 * md && v < 1.5 * md) { s += v; s2 += v * v; k++; }
                    if (k >= 0.7 * (int)d.size()) { const double m = s / k, sd = std::sqrt(std::max(0.0, s2 / k - m * m)); pri = m; spread = sd / m; cls = sd / m < 0.12 ? "jittered PRI" : "irregular PRI"; }
                }
            }
            if (!cls.empty() && cls == "constant PRI") spread = 0;
        }
        if (cls.empty()) {
            cnt_ = 0; cand_.clear();
            if (!cls_.empty() && (t - lastPulseT_) > 3.0) { cls_.clear(); out->row("Emitter", "gone", "no pulses for 3 s", levelDb_); }
            return;
        }
        // PW, level, frequency, scan
        std::vector<double> ws, fs_; double amp = 0; for (auto& p : w) { ws.push_back(p.w); if (!std::isnan(p.f)) fs_.push_back(p.f); amp += p.amp; } amp /= (double)w.size();
        pw_ = median(ws); ifHz_ = fs_.size() >= 5 ? median(fs_) : std::nan(""); levelDb_ = 20.f * std::log10((float)std::max(amp, 1e-12) / std::max(mean_, 1e-12f)) ;
        pri_ = pri; prfHz_ = 1.0 / pri; spread_ = spread; staggerN_ = stag;
        scan_ = scanPeriod();
        // a new class or a PRI change of more than 2 % must hold for two reports before it is logged
        const bool same = !cls_.empty() && cls == cls_ && std::fabs(pri - lastPri_) < 0.02 * lastPri_;
        if (same) { lastPri_ += 0.2 * (pri - lastPri_); cnt_ = 0; cand_.clear(); out->good(levelDb_, std::isnan(ifHz_) ? 0 : ifHz_); }
        else {
            if (cand_ == cls && std::fabs(pri - candPri_) < 0.02 * candPri_) cnt_++; else { cand_ = cls; candPri_ = pri; cnt_ = 1; }
            if (cnt_ >= 2) {
                cls_ = cls; lastPri_ = pri; cnt_ = 0; cand_.clear(); out->good(levelDb_, std::isnan(ifHz_) ? 0 : ifHz_);
                out->row("Emitter", fmt("%.0f Hz", prfHz_), fmt("%s, PRI %.2f us (PRF %.1f Hz), pulse width %.3f us, %s%s", cls.c_str(), pri * 1e6, prfHz_, pw_ * 1e6, std::isnan(ifHz_) ? "carrier inside the pulse not measurable" : fmt("%+.0f kHz from the tuned frequency", ifHz_ / 1000).c_str(),
                         scan_ > 0 ? fmt(", antenna turn %.2f s", scan_).c_str() : ""), levelDb_);
            }
        }
    }
    // the period of the pulse level (the beam passing): autocorrelation of the peak amplitudes resampled to 20 ms
    double scanPeriod() const {
        if (pul_.size() < 100) return 0;
        const double t0 = pul_.front().t, t1 = pul_.back().t; if (t1 - t0 < 6.0) return 0;
        const double dt = 0.02; const int M = (int)((t1 - t0) / dt); if (M < 200) return 0;
        std::vector<double> s(M, 0.0); std::vector<int> c(M, 0);
        for (auto& p : pul_) { const int i = (int)((p.t - t0) / dt); if (i >= 0 && i < M) { s[i] = std::max(s[i], p.amp); c[i]++; } }
        // fill holes by the previous value, take the log
        double last = 1e-9; for (int i = 0; i < M; i++) { if (c[i]) last = s[i]; s[i] = std::log(std::max(last, 1e-9)); }
        double mu = 0; for (double v : s) mu += v; mu /= M; for (auto& v : s) v -= mu;
        double e0 = 0; for (double v : s) e0 += v * v; if (e0 < 1e-9) return 0;
        const int lmin = (int)(0.8 / dt), lmax = std::min(M / 2, (int)(12.0 / dt));
        double best = 0; int bl = 0;
        std::vector<double> r(lmax + 1, 0.0);
        for (int l = lmin; l <= lmax; l++) { double a = 0; for (int i = 0; i + l < M; i++) a += s[i] * s[i + l]; r[l] = a / e0 * (double)M / (double)(M - l); if (r[l] > best) { best = r[l]; bl = l; } }
        // the modulation must be real: strong correlation and the log-level swing of at least 4 dB peak to peak
        double mn = 1e9, mx = -1e9; for (double v : s) { mn = std::min(mn, v); mx = std::max(mx, v); }
        if (best < 0.5 || (mx - mn) * 8.686 < 4.0) return 0;
        // choose the fundamental: the smallest lag with 85 % of the best
        for (int l = lmin; l <= bl; l++) if (r[l] >= 0.85 * best && r[l] >= r[l - 1] && r[l] >= r[std::min(lmax, l + 1)]) { bl = l; break; }
        return bl * dt;
    }
    Decimator dec_; double rate_ = 1, pkT_ = 0, startT_ = 0, nextReport_ = kReportSec, lastPulseT_ = -1e9, lastPri_ = 0, candPri_ = 0; uint64_t n_ = 0, warm_ = 0, nPulses_ = 0;
    float mean_ = 0, var_ = 0, pk_ = 0, levelDb_ = -120; cf32 prevA_{0, 0}, acc_{0, 0}; bool in_ = false, have_ = false; int cnt_ = 0, seen_ = 0, staggerN_ = 0;
    std::vector<float> seg_; std::vector<Pulse> pul_, history_; std::string cls_, cand_, lastEmit_; double pri_ = 0, pw_ = 0, scan_ = 0, prfHz_ = 0, spread_ = 0, ifHz_ = 0;
};
}

std::unique_ptr<Decoder> makeRadar() { return std::make_unique<RadarDec>(); }

} // namespace dect2::x40
