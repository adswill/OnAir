// FSK parameter analyser: for an unknown FSK or GFSK signal in the 90 kHz around the tuned frequency (default 250 ksps; symbol rates from about 100 Bd to 20 kBd): the symbol rate, whether it
// is 2-level or 4-level, the shift, the modulation index and the centre frequency. Every 2 s of the FM-demodulated signal (instantaneous frequency in Hz): the symbol rate from the line of the squared
// demodulated signal (transitions happen only on the symbol grid, so |y|^2 has a line at the symbol rate); the symbol timing as the phase where |y| is largest; the signal sampled at the symbol centres and
// clustered into 2 or 4 levels (4 when it leaves a quarter of the 2-level residual); shift = distance between the level centres at the symbol centres (a Gaussian-filtered signal reaches its full deviation only after several equal symbols, so the shift of GFSK with BT 0.5 reads about 7 % under the peak deviation), deviation = half of the outer distance,
// modulation index h = outer distance / symbol rate (2-level; MSK is 0.5), level spacing equal or not, and the signal to noise of the levels. It does not decode the bits or the framing; it tells which decoder to
// try and with what settings. The demodulator is a plain FM discriminator in the whole tuned bandwidth: below about 18 dB carrier to noise in 50 kHz (9600 Bd, 4.8 kHz shift) it fails, and a small deviation (a few hundred Hz) in a 90 kHz channel needs a clean signal: tune a lower sample rate to narrow the channel. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 100000, kCut = 45000, kBlockSec = 2.0, kMinBaud = 100;

class FskDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, std::min(kCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate(); blockN_ = (size_t)(kBlockSec * rate_); nf_ = 1; while (nf_ < blockN_) nf_ *= 2;
        reset(); return true;
    }
    void reset() override { dec_.reset(); y_.clear(); prev_ = cf32(1, 0); skip_ = (size_t)(0.1 * rate_); have_ = false; res_ = Res(); lastRow_ = -1e9; t_ = 0; lastLevels_ = 0; lastBaud_ = 0; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            t_ += 1.0 / rate_;
            const cf32 d = a * std::conj(prev_); prev_ = a;
            if (skip_) { skip_--; return; }
            y_.push_back((float)(std::atan2(d.imag(), d.real()) * rate_ / (2 * kPi)));
            if (y_.size() >= blockN_) { analyse(); y_.clear(); }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!res_.ok) { t.headline = "no FSK found (needs a steady symbol rate and at least two frequency levels)"; return; }
        t.headline = fmt("%d-FSK %.0f Bd, shift %.2f kHz, h %.2f, centre %+.0f Hz", res_.levels, res_.baud, res_.shift / 1000, res_.h, res_.centre);
        t.facts.push_back({"modulation", fmt("%d-level FSK%s", res_.levels, std::fabs(res_.h - 0.5) < 0.06 && res_.levels == 2 ? " (modulation index 0.5: MSK-like)" : "")});
        t.facts.push_back({"symbol rate", fmt("%.1f Bd", res_.baud)});
        t.facts.push_back({"shift", res_.levels == 2 ? fmt("%.0f Hz between the two tones (deviation +-%.0f Hz)", res_.shift, res_.shift / 2) : fmt("%.0f Hz between neighbouring levels (outer levels +-%.0f Hz)", res_.shift, res_.outer / 2)});
        std::string lv; for (size_t i = 0; i < res_.lv.size(); i++) lv += (i ? ", " : "") + fmt("%+.0f", res_.lv[i]); t.facts.push_back({"levels", lv + " Hz"});
        t.facts.push_back({"modulation index", fmt("h = %.2f (outer distance over symbol rate)", res_.h)});
        t.facts.push_back({"centre frequency", fmt("%+.0f Hz from the tuned frequency", res_.centre)});
        t.facts.push_back({"level spacing", res_.levels == 4 ? fmt("%s (outer / inner spacing %.2f)", std::fabs(res_.ratio - 3.0) < 0.35 ? "equal" : "unequal", res_.ratio) : std::string("-")});
        t.facts.push_back({"eye signal to noise", fmt("%.1f dB (level spacing over the spread inside a level)", res_.snr)});
    }
private:
    struct Res { bool ok = false; int levels = 0; double baud = 0, shift = 0, outer = 0, h = 0, centre = 0, snr = 0, ratio = 0; std::vector<double> lv; };
    static double refine(const std::vector<float>& p, int k) { const double a = std::log(p[k - 1] + 1e-30), b = std::log(p[k] + 1e-30), c = std::log(p[k + 1] + 1e-30), den = a - 2 * b + c; return k + (den < -1e-12 ? 0.5 * (a - c) / den : 0.0); }
    static float median(std::vector<float> v) { if (v.empty()) return 1e-30f; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return std::max(v[v.size() / 2], 1e-30f); }
    // k-means in one dimension: returns the centres sorted and the residual sum of squares
    static double kmeans(const std::vector<float>& s, int k, std::vector<double>& c) {
        std::vector<float> t(s); std::sort(t.begin(), t.end()); c.assign((size_t)k, 0.0);
        for (int i = 0; i < k; i++) c[i] = t[(size_t)((i + 0.5) / k * (double)(t.size() - 1))];
        double res = 0;
        for (int it = 0; it < 30; it++) {
            std::vector<double> sum((size_t)k, 0.0); std::vector<int> cnt((size_t)k, 0); res = 0;
            for (float v : s) { int b = 0; double bd = 1e30; for (int i = 0; i < k; i++) { const double d = std::fabs(v - c[i]); if (d < bd) { bd = d; b = i; } } sum[b] += v; cnt[b]++; res += bd * bd; }
            for (int i = 0; i < k; i++) if (cnt[i]) c[i] = sum[i] / cnt[i];
        }
        std::sort(c.begin(), c.end());
        return res;
    }
    void analyse() {
        have_ = true;
        const size_t N = y_.size(); Res r;
        double mean = 0; for (float v : y_) mean += v; mean /= (double)N;
        // 1. the symbol rate: the line of the squared demodulated signal
        std::vector<cf32> d(nf_, cf32(0, 0)); double m2 = 0; for (size_t i = 0; i < N; i++) { const double v = y_[i] - mean; m2 += v * v; } m2 /= (double)N;
        if (m2 < 1.0) { res_ = r; out->bad(); return; }
        std::vector<double> sq(N); double ms = 0; for (size_t i = 0; i < N; i++) { const double v = y_[i] - mean; sq[i] = v * v; ms += sq[i]; } ms /= (double)N;
        for (size_t i = 0; i < N; i++) d[i] = cf32((float)((sq[i] - ms) * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)N))), 0);
        fftx(d); std::vector<float> E(nf_ / 2); for (size_t k = 0; k < nf_ / 2; k++) E[k] = std::norm(d[k]);
        const double bh = rate_ / (double)nf_; const int kLo = std::max(3, (int)(kMinBaud / bh)), kHi = (int)(rate_ / 5.0 / bh);
        std::vector<float> band(E.begin() + kLo, E.begin() + kHi); const float med = median(band);
        std::vector<int> cand;
        for (int pass = 0; pass < 3; pass++) { int kb = -1; for (int k = kLo; k < kHi; k++) { bool near = false; for (int c : cand) if (std::abs(k - c) < 20) near = true; if (!near && (kb < 0 || E[k] > E[kb])) kb = k; } if (kb < 0 || E[kb] < 8.f * med) break; cand.push_back(kb); }
        // a symbol-rate line is also the strongest at the baud and the lower candidates are its sub-harmonics: try each, keep the best eye
        Res best; best.snr = -1e9;
        for (int kc : cand) {
            const double baud = refine(E, kc) * bh; const double T = rate_ / baud;
            if (T < 6) continue;
            // smooth y over T / 4, then the timing phase with the largest mean |y|
            const int W = std::max(1, (int)(T / 4)); std::vector<float> sm(N, 0.f); double acc = 0;
            for (size_t i = 0; i < N; i++) { acc += y_[i] - mean; if (i >= (size_t)W) acc -= y_[i - (size_t)W] - mean; sm[i] = (float)(acc / std::min<size_t>(i + 1, (size_t)W)); }
            double bestE = -1, tau = 0; for (int s = 0; s < 16; s++) { const double t0 = T + T * s / 16.0; double e = 0; int c = 0; for (double p = t0; p < (double)N - 2 && c < 600; p += T) { const size_t i = (size_t)p; const double u = p - (double)i; const double v = sm[i] * (1 - u) + sm[i + 1] * u; e += v * v; c++; } if (c && e / c > bestE) { bestE = e / c; tau = t0; } }
            std::vector<float> syms; for (double p = tau; p < (double)N - 2; p += T) { const size_t i = (size_t)p; const double u = p - (double)i; syms.push_back((float)(sm[i] * (1 - u) + sm[i + 1] * u)); }
            if (syms.size() < 200) continue;
            std::vector<double> c2, c4; const double r2 = kmeans(syms, 2, c2), r4 = kmeans(syms, 4, c4);
            Res q; q.baud = baud;
            const bool four = r4 < 0.25 * r2 && (c4[3] - c4[0]) > 4 * 0 && c4[1] - c4[0] > 0 && c4[2] - c4[1] > 0 && c4[3] - c4[2] > 0;
            if (four) {
                const double o1 = c4[1] - c4[0], o2 = c4[2] - c4[1], o3 = c4[3] - c4[2]; const double inner = o2, outer = 0.5 * (o1 + o3);
                q.levels = 4; q.lv = {c4[0] + mean, c4[1] + mean, c4[2] + mean, c4[3] + mean}; q.shift = (o1 + o2 + o3) / 3; q.outer = c4[3] - c4[0]; q.ratio = outer / std::max(inner, 1e-9);
                q.centre = 0.25 * (c4[0] + c4[1] + c4[2] + c4[3]) + mean; q.h = q.outer / baud; q.snr = 10 * std::log10(q.shift * q.shift / 12.0 / std::max(r4 / (double)syms.size(), 1e-12));   // the spread inside the levels, not counting the filtering's own spread
            } else {
                q.levels = 2; q.lv = {c2[0] + mean, c2[1] + mean}; q.shift = c2[1] - c2[0]; q.outer = q.shift; q.centre = 0.5 * (c2[0] + c2[1]) + mean; q.h = q.shift / baud; q.snr = 10 * std::log10(q.shift * q.shift / 4.0 / std::max(r2 / (double)syms.size(), 1e-12));
            }
            q.ok = q.snr > 6.0 && q.shift > 4 * bh && q.outer > 0.3 * baud * 0 + 0;
            if (q.ok && q.snr > best.snr) best = q;
        }
        if (!best.ok) { res_ = Res(); out->energy(); return; }
        res_ = best; out->good((float)best.snr, best.centre);
        const bool changed = best.levels != lastLevels_ || std::fabs(best.baud - lastBaud_) > 0.02 * lastBaud_ || t_ - lastRow_ > 30;
        if (changed) { lastLevels_ = best.levels; lastBaud_ = best.baud; lastRow_ = t_; out->row("FSK", fmt("%d-FSK %.0f Bd", best.levels, best.baud), fmt("shift %.0f Hz, h %.2f, centre %+.0f Hz, eye %.1f dB", best.shift, best.h, best.centre, best.snr), (float)best.snr); }
    }
    Decimator dec_; double rate_ = 1, t_ = 0, lastRow_ = -1e9, lastBaud_ = 0; size_t blockN_ = 0, nf_ = 1, skip_ = 0; int lastLevels_ = 0; cf32 prev_{1, 0}; bool have_ = false; std::vector<float> y_; Res res_;
};
}

std::unique_ptr<Decoder> makeFskAnalyser() { return std::make_unique<FskDec>(); }

} // namespace dect2::x40
