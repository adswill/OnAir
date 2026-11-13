// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells test signal (nr_gen.h).
#include "dect2/nr/nr_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/dsp/fir_dot.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

namespace dect2 {

std::vector<NrTestCell> nrTestScene(int scene) {
    std::vector<NrTestCell> v;
    auto cellOf = [](int pci, int scs, int lmax, int sfn, int cs0, int ss0, int kSsb, bool barred) {
        nr::GenCell g;
        g.pci = pci; g.scsKhz = scs; g.lmax = lmax; g.ssbMask = lmax == 8 ? 0xFF : 0x0F;
        for (int i = 0; i < lmax; i++) g.beamDb.push_back(-1.5f * (float)std::abs(i - 2));   // beam 2 points at the receiver
        g.mib.sfn = sfn; g.mib.scs30 = scs == 30; g.mib.kSsb = kSsb; g.mib.dmrsTypeAPos = 2; g.mib.coreset0 = cs0; g.mib.ss0 = ss0;
        g.mib.barred = barred; g.mib.intraFreqResel = true;
        g.seed = 5 + (uint32_t)pci;
        return g;
    };
    if (scene == 1) v.push_back({cellOf(222, 15, 4, 100, 3, 0, 6, false), 0});
    else if (scene == 2) v.push_back({cellOf(17, 30, 8, 900, 0, 0, 31, true), 0});
    else {
        v.push_back({cellOf(501, 30, 8, 512, 12, 2, 4, false), -4.32e6});   // GSCN 7872 at the default 3541.44 MHz
        v.push_back({cellOf(1007, 30, 8, 33, 6, 0, 18, false), 4.32e6});    // GSCN 7878
        v.back().g.beamDb = {-12, -9, -6, -3, 0, -3, -6, -9};
    }
    return v;
}

namespace {

// A polyphase resampler for a rational step p / q (7.68 MHz to 20 MHz is 48 / 125): the q filter phases are worked out once, so every output
// sample is one plain dot product of 2H taps. It is the same Blackman-windowed sinc as cell::Resampler, which interpolates its table per tap
// and per output sample and was most of the NR generator's time (0.7 x real time at 20 Msps with two cells).
struct PolyResampler {
    int H = 0, q = 0;
    long p = 0;
    std::vector<float> taps;   // q phases of 2H taps, tap k for the input sample c - H + 1 + k, in firDot's doubled layout
    bool build(double cutoff, double step) {
        for (int qq = 1; qq <= 4096; qq++) {
            const double pp = std::round(step * qq);
            if (pp >= 1 && std::fabs(pp / qq - step) < 1e-12) { q = qq; p = (long)pp; break; }
        }
        if (!q) return false;
        const double fc = std::max(0.005, std::min(0.5, cutoff));
        H = std::max(8, (int)std::ceil(4.0 / fc));
        taps.assign((size_t)q * 4 * (size_t)H, 0.f);
        for (int j = 0; j < q; j++) {
            const double fr = (double)j / q;
            for (int k = 0; k < 2 * H; k++) {
                const double t = fr + (double)(H - 1 - k);   // tau - i for i = c - H + 1 + k
                const double a = 2 * fc * t;
                const double sn = std::fabs(a) < 1e-9 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
                const double u = t / H;
                const double w = std::fabs(u) >= 1 ? 0.0 : 0.42 + 0.5 * std::cos(M_PI * u) + 0.08 * std::cos(2 * M_PI * u);
                const float v = (float)(2 * fc * sn * w);
                taps[(size_t)j * 4 * (size_t)H + 2 * (size_t)k] = v;
                taps[(size_t)j * 4 * (size_t)H + 2 * (size_t)k + 1] = v;
            }
        }
        return true;
    }
};

class NrSynth : public ModeSynth {
public:
    NrSynth(const SynthConfig& c, double rate) : out_(rate), rng_(17) {
        for (const auto& tc : nrTestScene(c.modeOpt[0])) {
            Src s;
            s.g = tc.g; s.off = tc.offsetHz;
            s.native = 256e3 * tc.g.scsKhz;
            s.step = s.native / out_;
            const double cut = std::min(0.5, 0.5 * out_ / s.native) * 0.97;
            s.rs.reset(new cell::Resampler(cut));
            s.polyOk = s.poly.build(cut, s.step);
            if (s.polyOk) { s.ipos = s.poly.H; s.pos = s.poly.H; }
            src_.push_back(std::move(s));
        }
        cfo_ = c.cfoHz;
        const double nat = src_.empty() ? out_ : src_[0].native;
        // the generator's rms is 0.2 over its native band: noise over the whole output band for snrDb inside the SSB
        noise_ = (float)std::sqrt(0.04 * std::max(1.0, out_ / nat) / std::pow(10.0, c.snrDb / 10) / 2);
        if (c.snrDb >= 99) noise_ = 0;
    }
    double sampleRate() const override { return out_; }
    void generate(cf32* o, size_t n) override {
        for (size_t i = 0; i < n; i++) o[i] = 0;
        if (tmp_.size() < n) tmp_.resize(n);
        for (auto& s : src_) {
            if (s.polyOk) resamplePoly(s, tmp_.data(), n);
            else resampleTable(s, tmp_.data(), n);
            const double w = 2 * M_PI * (s.off + cfo_) / out_;
            if (w == 0 && s.ph == 0) {
                for (size_t i = 0; i < n; i++) o[i] += tmp_[i];
            } else {
                // a phasor per sample instead of cos / sin, anchored again every 4096 samples so that rounding never builds up
                for (size_t i0 = 0; i0 < n; i0 += 4096) {
                    const size_t m = std::min<size_t>(4096, n - i0);
                    // written out by hand: a std::complex product may go through the NaN-checking library call
                    double rr = std::cos(s.ph), ri = std::sin(s.ph);
                    const double sr = std::cos(w), si = std::sin(w);
                    for (size_t i = 0; i < m; i++) {
                        const cf32 v = tmp_[i0 + i];
                        const float fr = (float)rr, fi = (float)ri;
                        o[i0 + i] += cf32(v.real() * fr - v.imag() * fi, v.real() * fi + v.imag() * fr);
                        const double nr = rr * sr - ri * si;
                        ri = rr * si + ri * sr;
                        rr = nr;
                    }
                    s.ph = std::fmod(s.ph + w * (double)m, 2 * M_PI);
                }
            }
        }
        dect2::normal_distribution<float> nd(0.f, 1.f);
        if (noise_ > 0) for (size_t i = 0; i < n; i++) o[i] += cf32{noise_ * nd(rng_), noise_ * nd(rng_)};
    }

private:
    struct Src {
        nr::GenCell g;
        double off = 0, native = 1, step = 1, pos = 0, ph = 0;
        int half = 0;   // half frames made so far
        std::unique_ptr<cell::Resampler> rs;
        PolyResampler poly;
        bool polyOk = false;
        long ipos = 0;   // polyphase: the input sample c of the next output, and its phase j (tau = c + j / q)
        int jph = 0;
        std::vector<cf32> buf;
    };
    void resampleTable(Src& s, cf32* t, size_t n) {
        size_t done = 0;
        const int H = s.rs->halfLen();
        while (done < n) {
            if (s.pos + H + 2 >= (double)s.buf.size()) { more(s); continue; }
            const size_t fit = (size_t)std::floor(((double)s.buf.size() - H - 2 - s.pos) / s.step) + 1;
            const size_t m = std::min(n - done, fit);
            s.rs->run(s.buf.data(), s.buf.size(), s.pos, s.step, t + done, m);
            s.pos += s.step * (double)m;
            done += m;
        }
    }
    void resamplePoly(Src& s, cf32* t, size_t n) {
        const PolyResampler& P = s.poly;
        const int L2 = 4 * P.H;   // floats: 2H interleaved samples
        for (size_t o = 0; o < n; o++) {
            if (s.ipos + P.H + 1 >= (long)s.buf.size()) { more(s); o--; continue; }
            const float* h = &P.taps[(size_t)s.jph * (size_t)L2];
            t[o] = firDot(h, &s.buf[(size_t)(s.ipos - P.H + 1)], 2 * P.H);
            s.jph += (int)P.p;   // no division per sample: p < q steps at most once, a larger p a few times
            while (s.jph >= P.q) { s.jph -= P.q; s.ipos++; }
        }
        s.pos = (double)s.ipos + (double)s.jph / P.q;
    }
    void more(Src& s) {
        const int H = s.polyOk ? s.poly.H : s.rs->halfLen();
        const long keep = std::max(0L, (long)std::floor(s.pos) - H - 2);
        if (keep > 0) { s.buf.erase(s.buf.begin(), s.buf.begin() + keep); s.pos -= (double)keep; s.ipos -= keep; }
        // the next 20 ms (four half frames), SFN running on
        nr::GenCell g = s.g;
        const int hf0 = (s.g.mib.hrf ? 1 : 0) + s.half;
        g.mib.sfn = (s.g.mib.sfn + hf0 / 2) & 1023;
        g.mib.hrf = (hf0 & 1) != 0;
        g.seed = s.g.seed + (uint32_t)s.half;
        const std::vector<cf32> f = nr::generate(g, 4);
        s.half += 4;
        if (s.polyOk && s.buf.empty()) s.buf.assign((size_t)H, cf32(0.f, 0.f));   // the polyphase start: H zeros of history, like the table version's edge
        s.buf.insert(s.buf.end(), f.begin(), f.end());
    }
    double out_, cfo_ = 0;
    float noise_ = 0;
    std::vector<Src> src_;
    std::vector<cf32> tmp_;
    std::mt19937 rng_;
};

} // namespace

std::unique_ptr<ModeSynth> makeNrSynth(const SynthConfig& cfg, double sampleRate) { return std::unique_ptr<ModeSynth>(new NrSynth(cfg, sampleRate)); }

} // namespace dect2
