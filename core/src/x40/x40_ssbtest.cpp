// SSB transmitter test meter: tune the transmitter's suppressed-carrier frequency (the dial frequency) while it sends a test tone, or two tones, and read what a transmitter test sheet asks for.
// Windows of 2^15 samples at the 8 - 16 ksps working rate (about 2.7 s) with a four-term Blackman-Harris window. The sideband is the side with the tones (upper or lower). One tone (100 - 3500 Hz
// from the carrier): carrier leak, opposite sideband, 2nd to 5th harmonics of the audio tone, all in dB under the tone. Two tones within 10 dB of each other: IMD3 (2 f1 - f2, 2 f2 - f1),
// IMD5 (3 f1 - 2 f2, 3 f2 - 2 f1) and IMD7 (4 f1 - 3 f2, 4 f2 - 3 f1), each in dB under one tone and under PEP (two equal tones: PEP is 6 dB over one tone), the worst of the pair; carrier leak and
// opposite sideband too. A product below the noise is reported as "under -X dB" (the noise floor of this measurement, not of the transmitter). The carrier position is taken from the leak
// if it stands 6 dB over the noise, otherwise assumed to be at the tuned frequency (then the opposite sideband and harmonic frequencies are only as good as the tuning).
// It measures at this radio: a strong signal into a radio that overloads shows the radio's own products (use the attenuator and see that the numbers do not move). Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 12000, kCut = 4200;
constexpr int kN = 1 << 15;

struct Tone { double hz = 0, db = 0; };

class SsbTestDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); binHz_ = rate_ / kN;
        win_.resize(kN); double s2 = 0; for (int i = 0; i < kN; i++) { const double a = 2 * kPi * i / kN; win_[i] = (float)(0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a)); s2 += (double)win_[i] * win_[i]; }
        reset(); return true;
    }
    void reset() override { dec_.reset(); buf_.clear(); skip_ = (size_t)(0.2 * rate_); have_ = false; res_ = Res(); nWin_ = 0; lastKind_ = -1; }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) { if (skip_) { skip_--; return; } buf_.push_back(a); if ((int)buf_.size() >= kN) { window(); buf_.clear(); } });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (res_.kind == 0) { t.headline = "no clean test tone (one tone, or two tones within 10 dB, 100 - 3500 Hz from the carrier)"; return; }
        if (res_.kind == 1) {
            t.headline = fmt("one tone %.0f Hz (%s): carrier %s, opposite sideband %s, 2nd harmonic %s", res_.f1, res_.usb ? "USB" : "LSB", lvl(res_.carrier).c_str(), lvl(res_.opp).c_str(), lvl(res_.h[0]).c_str());
        } else {
            t.headline = fmt("two tones %.0f and %.0f Hz (%s): IMD3 %s, IMD5 %s, IMD7 %s under one tone", res_.f1, res_.f2, res_.usb ? "USB" : "LSB", lvl(res_.im[0]).c_str(), lvl(res_.im[1]).c_str(), lvl(res_.im[2]).c_str());
        }
        t.facts.push_back({"sideband", res_.usb ? "upper (the tones are above the carrier)" : "lower (the tones are below the carrier)"});
        t.facts.push_back({"tone", res_.kind == 1 ? fmt("%.1f Hz", res_.f1) : fmt("%.1f Hz and %.1f Hz (%.1f dB apart in level)", res_.f1, res_.f2, res_.dLevel)});
        t.facts.push_back({"carrier leak", lvl(res_.carrier) + (res_.carrierFound ? "" : " (carrier assumed at the tuned frequency)")});
        t.facts.push_back({"opposite sideband", lvl(res_.opp)});
        if (res_.kind == 1) { for (int i = 0; i < 4; i++) t.facts.push_back({fmt("harmonic %d of the audio tone", i + 2), lvl(res_.h[i])}); }
        else {
            const char* nm[3] = {"IMD3", "IMD5", "IMD7"};
            for (int i = 0; i < 3; i++) t.facts.push_back({nm[i], fmt("%s under one tone, %s under PEP (worst of the two products)", lvl(res_.im[i]).c_str(), lvl(res_.im[i], 6.02).c_str())});
        }
        t.facts.push_back({"measurement floor", fmt("%.0f dB under the tone", res_.floorDb)});
    }
private:
    struct Lv { double db = 0; bool under = false; };
    struct Res { int kind = 0; bool usb = true, carrierFound = false; double f1 = 0, f2 = 0, dLevel = 0, floorDb = 0; Lv carrier, opp, h[4], im[3]; };
    static std::string lvl(const Lv& v, double pepOffset = 0) { return v.under ? fmt("under -%.0f dB", v.db + pepOffset) : fmt("-%.1f dB", v.db + pepOffset); }
    void window() {
        have_ = true; nWin_++;
        std::vector<cf32> a((size_t)kN); for (int i = 0; i < kN; i++) a[i] = buf_[i] * win_[i];
        fftx(a);
        std::vector<double> p((size_t)kN); for (int k = 0; k < kN; k++) p[k] = std::norm(a[k]);
        auto bin = [&](double hz) { return ((int)std::lround(hz / binHz_) % kN + kN) % kN; };
        auto peakAt = [&](double hz, double tolHz, double* where = nullptr) { double best = 0; int kb = bin(hz); const int w = std::max(2, (int)std::lround(tolHz / binHz_)); const int k0 = bin(hz); for (int d = -w; d <= w; d++) { const int k = (k0 + d + kN) % kN; if (p[k] > best) { best = p[k]; kb = k; } } if (where) *where = (kb < kN / 2 ? kb : kb - kN) * binHz_; double sum = 0; for (int d = -3; d <= 3; d++) sum += p[(kb + d + kN) % kN]; return sum; };
        std::vector<double> s(p.begin(), p.end()); 
        // the noise: the median of the bins in the band 100 - 3800 Hz on both sides
        std::vector<double> nb; for (double f = 200; f < 3800; f += 7.3) { nb.push_back(p[bin(f)]); nb.push_back(p[bin(-f)]); }
        std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end()); const double noise = std::max(nb[nb.size() / 2], 1e-30) / 0.693;
        const double noise7 = noise * 7 * 1.0;                                        // the 7-bin sum of noise
        // the tones: bins that are local maxima within 100 - 3500 Hz on either side, 25 dB over the noise
        std::vector<Tone> peaks; double sideUp = 0, sideDn = 0;
        for (int k = 3; k < kN / 2 - 3; k++) {
            const double fU = k * binHz_; if (fU > 3500) break; if (fU < 100) continue;
            for (int sgn = 1; sgn >= -1; sgn -= 2) {
                const int kk = sgn > 0 ? k : kN - k; const double v = p[kk];
                if (v < 316.0 * noise) continue; bool top = true; for (int d = -4; d <= 4 && top; d++) if (d && p[(kk + d + kN) % kN] > v) top = false;
                if (!top) continue;
                double sum = 0; for (int d = -3; d <= 3; d++) sum += p[(kk + d + kN) % kN];
                peaks.push_back({sgn * fU, 10 * std::log10(sum)}); (sgn > 0 ? sideUp : sideDn) += sum;
            }
        }
        Res r; r.usb = sideUp >= sideDn;
        std::vector<Tone> side; for (auto& t : peaks) if ((t.hz > 0) == r.usb) side.push_back(t);
        std::sort(side.begin(), side.end(), [](const Tone& a, const Tone& b) { return a.db > b.db; });
        // refine the tone frequencies (parabola on the three bins) is not needed: bins are 0.37 Hz
        std::vector<Tone> strong; for (auto& t : side) if (t.db > side[0].db - 30 && !(strong.size() && std::fabs(std::fabs(t.hz) - std::fabs(strong[0].hz)) < 40) ) strong.push_back(t);
        if (strong.empty()) { res_ = Res(); finish(0); return; }
        // tones: the strongest one, and a second within 10 dB that is not one of its products' places is a two-tone test
        int nTones = 1; Tone T1 = strong[0], T2 = strong[0];
        for (size_t i = 1; i < strong.size(); i++) if (strong[i].db > strong[0].db - 10 && std::fabs(std::fabs(strong[i].hz) - std::fabs(strong[0].hz)) > 40) { T2 = strong[i]; nTones = 2; break; }
        // the carrier: the leak near 0 Hz (within +-60 Hz) if it stands 6 dB over the noise, else the tuned frequency
        double fcWhere = 0; const double cl = peakAt(0, 60, &fcWhere);
        r.carrierFound = cl > noise7 * 4.0; const double fc = r.carrierFound ? fcWhere : 0.0;
        const double ref = std::pow(10.0, T1.db / 10);
        const double floorDbRef = 10 * std::log10(ref / (noise7 * 1.0));
        r.floorDb = floorDbRef;
        auto level = [&](double hz, double refPow) { Lv v; const double pw = peakAt(hz, 2.5); const bool under = pw < noise7 * 4.0; const double use = under ? noise7 * 4.0 : pw; v.db = 10 * std::log10(refPow / use); v.under = under; return v; };
        r.carrier = level(fc, ref);
        const double sgn = r.usb ? 1.0 : -1.0; const double a1 = std::fabs(T1.hz), a2 = std::fabs(T2.hz);
        auto absf = [&](double aud) { return fc + sgn * aud; };                   // audio frequency to RF-relative frequency
        if (nTones == 1) {
            r.kind = 1; r.f1 = a1 - std::fabs(fc) * 0; r.f1 = std::fabs(T1.hz - fc);
            r.opp = level(fc - sgn * r.f1, ref);
            for (int i = 0; i < 4; i++) { const double af = (i + 2) * r.f1; r.h[i] = af < 3900 ? level(absf(af), ref) : Lv{99, true}; }
        } else {
            r.kind = 2; const double f1 = std::fabs(T1.hz - fc), f2 = std::fabs(T2.hz - fc); r.f1 = std::min(f1, f2); r.f2 = std::max(f1, f2); r.dLevel = std::fabs(T1.db - T2.db);
            const double lo = r.f1, hi = r.f2;
            const double weak = std::pow(10.0, std::min(T1.db, T2.db) / 10);
            const double prod[3][2] = {{2 * lo - hi, 2 * hi - lo}, {3 * lo - 2 * hi, 3 * hi - 2 * lo}, {4 * lo - 3 * hi, 4 * hi - 3 * lo}};
            for (int i = 0; i < 3; i++) {
                Lv worst; bool first = true;
                for (int j = 0; j < 2; j++) { const double af = prod[i][j]; if (af < 30 || af > 3900) continue; const Lv v = level(absf(af), weak); if (first || v.db < worst.db) { worst = v; first = false; } }
                r.im[i] = first ? Lv{99, true} : worst;
            }
            r.opp = level(fc - sgn * lo, weak);
            r.carrier = level(fc, weak);
        }
        res_ = r; finish(r.kind);
    }
    void finish(int kind) {
        if (kind == 0) { out->energy(); return; }
        out->good(0, 0);
        if (kind != lastKind_ || nWin_ % 4 == 1) {
            lastKind_ = kind;
            if (kind == 1) out->row("One tone", fmt("%.0f Hz", res_.f1), fmt("carrier %s, opposite sideband %s, harmonics %s %s %s %s", lvl(res_.carrier).c_str(), lvl(res_.opp).c_str(), lvl(res_.h[0]).c_str(), lvl(res_.h[1]).c_str(), lvl(res_.h[2]).c_str(), lvl(res_.h[3]).c_str()), (float)0);
            else out->row("Two tones", fmt("%.0f + %.0f Hz", res_.f1, res_.f2), fmt("IMD3 %s, IMD5 %s, IMD7 %s under one tone (PEP: %s, %s, %s); carrier %s", lvl(res_.im[0]).c_str(), lvl(res_.im[1]).c_str(), lvl(res_.im[2]).c_str(), lvl(res_.im[0], 6.02).c_str(), lvl(res_.im[1], 6.02).c_str(), lvl(res_.im[2], 6.02).c_str(), lvl(res_.carrier).c_str()), (float)0);
        }
    }
    Decimator dec_; double rate_ = 1, binHz_ = 1; size_t skip_ = 0; int nWin_ = 0, lastKind_ = -1; bool have_ = false;
    std::vector<float> win_; std::vector<cf32> buf_; Res res_;
};
}

std::unique_ptr<Decoder> makeSsbTest() { return std::make_unique<SsbTestDec>(); }

} // namespace dect2::x40
