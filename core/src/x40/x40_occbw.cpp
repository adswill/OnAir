// Occupied bandwidth meter (ITU-R SM.328, SM.443): the bandwidth of one signal in a wide capture, as a regulator or a transmitter test sheet states it. A 16384-point Blackman-Harris spectrum
// averaged over one second (0.5 s steps); the noise floor is the median bin, subtracted before any power is summed. The strongest contiguous region 3 dB over the floor (gaps up to 1 % of the
// span bridged) is the signal. Reported: the 99 % occupied bandwidth (0.5 % of the signal power below the lower edge and 0.5 % above the upper), the bandwidths at 3, 6, 20 and 26 dB below the spectral
// peak (of a 1 kHz-smoothed spectrum; the "x dB" of an emission designator is 26), the centre of the 99 % region against the tuned frequency, the signal power and its signal to noise ratio, and the adjacent power
// ratio: the power in bands of the same width as the 99 % bandwidth centred at +-1.25 occupied bandwidths (a quarter of a bandwidth of guard: the 0.5 % of the signal power outside each 99 % edge is -23 dB and would be read if the
// band touched it) and at +-2.5, in dB under the signal power (the floor included, so a clean signal reads its signal to noise). It is what arrives at this radio: noise, a neighbour, a strong signal's own splatter at the radio's ADC are all in it; compare with the attenuator in. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr int kN = 1 << 14;
constexpr double kStepSec = 0.5;

class OccBwDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs; binHz_ = fs / kN;
        win_.resize(kN); double s2 = 0; for (int i = 0; i < kN; i++) { const double a = 2 * kPi * i / kN; win_[i] = (float)(0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a)); s2 += (double)win_[i] * win_[i]; }
        sw2_ = s2; framesPer_ = std::max(1, (int)std::lround(kStepSec * 2 * fs / kN)); reset(); return true;
    }
    void reset() override { buf_.clear(); acc_.assign(kN, 0.0); nAcc_ = 0; have_ = false; res_ = Res(); t_ = 0; lastRow_ = -1e9; }
    void setCenterHz(double hz) override { centre_ = hz; }
    void feed(const cf32* x, size_t n) override { for (size_t i = 0; i < n; i++) { buf_.push_back(x[i]); if ((int)buf_.size() == kN) { frame(); buf_.clear(); } } }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "averaging"; return; }
        if (!res_.ok) { t.headline = "no signal 3 dB over the noise floor"; return; }
        t.headline = fmt("99 %% bandwidth %.2f kHz, -26 dB bandwidth %.2f kHz, centre %s, %.0f dB over the noise", res_.obw / 1000, res_.bw26 / 1000, f(res_.centre).c_str(), res_.snr);
        t.facts.push_back({"occupied bandwidth (99 %)", fmt("%.3f kHz (from %s to %s)", res_.obw / 1000, f(res_.lo).c_str(), f(res_.hi).c_str())});
        t.facts.push_back({"bandwidth at -3 / -6 dB", fmt("%.3f / %.3f kHz", res_.bw3 / 1000, res_.bw6 / 1000)});
        t.facts.push_back({"bandwidth at -20 / -26 dB", fmt("%.3f / %.3f kHz (the x dB of an emission designator is 26)", res_.bw20 / 1000, res_.bw26 / 1000)});
        t.facts.push_back({"centre of the signal", fmt("%s (%+.0f Hz from the tuned frequency)", f(res_.centre).c_str(), res_.centre)});
        t.facts.push_back({"signal power", fmt("%.1f dB re full scale, %.1f dB over the noise in the same bandwidth", res_.pwDb, res_.snr)});
        t.facts.push_back({"adjacent power, lower / upper", fmt("%s / %s under the signal (bands as wide as the occupied bandwidth, centred 1.25 bandwidths away)", lv(res_.adjLo).c_str(), lv(res_.adjHi).c_str())});
        t.facts.push_back({"alternate power, lower / upper", fmt("%s / %s (centred 2.5 bandwidths away)", lv(res_.altLo).c_str(), lv(res_.altHi).c_str())});
    }
private:
    struct Res { bool ok = false; double obw = 0, bw3 = 0, bw6 = 0, bw20 = 0, bw26 = 0, centre = 0, lo = 0, hi = 0, pwDb = 0, snr = 0, adjLo = 0, adjHi = 0, altLo = 0, altHi = 0; };
    std::string f(double hz) const { return centre_ > 0 ? fmt("%.4f MHz", (centre_ + hz) / 1e6) : fmt("%+.2f kHz", hz / 1000); }
    static std::string lv(double v) { return v > 900 ? std::string("-") : fmt("%.1f dB", v); }
    void frame() {
        std::vector<cf32> a(kN); for (int i = 0; i < kN; i++) a[i] = buf_[i] * win_[i]; fftx(a);
        for (int k = 0; k < kN; k++) acc_[k] += std::norm(a[k]); 
        if (++nAcc_ < framesPer_) return;
        have_ = true; t_ += kStepSec; evaluate(); std::fill(acc_.begin(), acc_.end(), 0.0); nAcc_ = 0;
    }
    int idxOf(int j) const { return (j + kN) % kN; }                                  // j = signed bin
    void evaluate() {
        // power per bin (Parseval scaling so that sums are powers), in bin order from -N/2
        std::vector<double> S((size_t)kN); for (int j = -kN / 2; j < kN / 2; j++) S[(size_t)(j + kN / 2)] = acc_[(size_t)idxOf(j)] / ((double)nAcc_ * (double)kN * sw2_);
        std::vector<double> m(S); std::nth_element(m.begin(), m.begin() + kN / 2, m.end()); const double floorP = std::max(m[kN / 2], 1e-30) / 0.693;
        std::vector<double> P((size_t)kN); for (int k = 0; k < kN; k++) P[(size_t)k] = std::max(0.0, S[(size_t)k] - floorP);
        // the signal region: bins over floor + 3 dB (smoothed over 5), gaps up to 1 % of the span bridged; the one with the most power
        std::vector<char> hot((size_t)kN, 0); for (int k = 2; k < kN - 2; k++) { double s = 0; for (int d = -2; d <= 2; d++) s += S[(size_t)(k + d)]; hot[(size_t)k] = s / 5 > 2.0 * floorP; }
        struct Reg { int a, b; double pw; }; std::vector<Reg> regs; const int bridge = std::max(4, kN / 100);
        for (int k = 0; k < kN;) { if (!hot[(size_t)k]) { k++; continue; } int a = k, b = k, gap = 0; int q = k; while (q < kN && gap <= bridge) { if (hot[(size_t)q]) { b = q; gap = 0; } else gap++; q++; } double pw = 0; for (int i = a; i <= b; i++) pw += P[(size_t)i]; regs.push_back({a, b, pw}); k = b + 1; }
        Res r;
        if (regs.empty()) { res_ = r; out->energy(); return; }
        const Reg g = *std::max_element(regs.begin(), regs.end(), [](const Reg& x, const Reg& y) { return x.pw < y.pw; });
        if (g.b - g.a < 3) { res_ = r; out->energy(); return; }
        const int pad = std::max(8, (g.b - g.a) / 4); const int a0 = std::max(0, g.a - pad), b0 = std::min(kN - 1, g.b + pad);
        double tot = 0; for (int i = a0; i <= b0; i++) tot += P[(size_t)i];
        if (tot <= 0) { res_ = r; return; }
        // 99 %: 0.5 % below, 0.5 % above (with a linear interpolation inside the bin)
        double c = 0; int lo = a0; for (int i = a0; i <= b0; i++) { c += P[(size_t)i]; if (c >= 0.005 * tot) { lo = i; break; } }
        c = 0; int hi = b0; for (int i = b0; i >= a0; i--) { c += P[(size_t)i]; if (c >= 0.005 * tot) { hi = i; break; } }
        auto fHz = [&](double idx) { return (idx - kN / 2) * binHz_; };
        r.lo = fHz(lo); r.hi = fHz(hi + 1); r.obw = r.hi - r.lo; r.centre = 0.5 * (r.lo + r.hi);
        // x-dB bandwidths on a 1 kHz boxcar of the denoised spectrum
        const int w = std::max(1, (int)(1000 / binHz_) | 1); std::vector<double> sm((size_t)kN, 0.0); { double acc = 0; for (int i = 0; i < kN; i++) { acc += P[(size_t)i]; if (i >= w) acc -= P[(size_t)(i - w)]; sm[(size_t)i] = acc / w; } }
        int pk = a0; for (int i = a0; i <= b0; i++) if (sm[(size_t)i] > sm[(size_t)pk]) pk = i;
        auto bwAt = [&](double dB) { const double th = sm[(size_t)pk] * std::pow(10.0, -dB / 10); int l = pk, h = pk; for (int i = pk; i >= a0; i--) if (sm[(size_t)i] >= th) l = i; for (int i = pk; i <= b0; i++) if (sm[(size_t)i] >= th) h = i; return (h - l + 1) * binHz_; };
        r.bw3 = bwAt(3); r.bw6 = bwAt(6); r.bw20 = bwAt(20); r.bw26 = bwAt(26);
        // signal power and the adjacent bands
        double sig = 0; for (int i = lo; i <= hi; i++) sig += P[(size_t)i]; double sigPlus = 0; for (int i = lo; i <= hi; i++) sigPlus += S[(size_t)i];
        r.pwDb = 10 * std::log10(std::max(sig, 1e-30)); r.snr = 10 * std::log10(std::max(sig / (floorP * (hi - lo + 1)), 1e-9));
        const int W = hi - lo + 1; auto band = [&](int cI, double& out_) { const int a = cI - W / 2, b = a + W - 1; if (a < 1 || b >= kN - 1) { out_ = 999; return; } double s = 0; for (int i = a; i <= b; i++) s += S[(size_t)i]; out_ = 10 * std::log10(std::max(sigPlus, 1e-30) / std::max(s, 1e-30)); };
        const int cI = (lo + hi) / 2; const int gd = (int)std::lround(1.25 * W); band(cI - gd, r.adjLo); band(cI + gd, r.adjHi); band(cI - 2 * gd, r.altLo); band(cI + 2 * gd, r.altHi);
        r.ok = true; res_ = r;
        out->good((float)r.snr, r.centre);
        if (t_ - lastRow_ >= 10 || lastRow_ < 0) { lastRow_ = t_; out->row("Bandwidth", fmt("%.2f kHz", r.obw / 1000), fmt("99 %% %.3f kHz, -26 dB %.3f kHz, centre %s, %.0f dB over the noise, adjacent %s / %s", r.obw / 1000, r.bw26 / 1000, f(r.centre).c_str(), r.snr, lv(r.adjLo).c_str(), lv(r.adjHi).c_str()), (float)r.pwDb); }
    }
    double fs_ = 1, binHz_ = 1, sw2_ = 1, centre_ = 0, t_ = 0, lastRow_ = -1e9; int framesPer_ = 1, nAcc_ = 0; bool have_ = false;
    std::vector<float> win_; std::vector<cf32> buf_; std::vector<double> acc_; Res res_;
};
}

std::unique_ptr<Decoder> makeOccBw() { return std::make_unique<OccBwDec>(); }

} // namespace dect2::x40
