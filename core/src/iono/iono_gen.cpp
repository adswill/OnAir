// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/iono/iono_gen.h"
#include "dect2/iono/iono_dsp.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {

namespace {

using cd = std::complex<double>;

class IonoSynth : public ModeSynth {
public:
    IonoSynth(const SynthConfig& c, double fs) : fs_(fs), dt_(1.0 / fs) {
        fc_ = (c.modeVal[0] > 0 ? c.modeVal[0] : kIonoSynthDefMhz) * 1e6;
        noiseOnly_ = c.modeOpt[0] == 1;
        clean_ = c.snrDb >= 99 && !noiseOnly_;   // the transmitter's clean signal (TxSession): the sounder sweeps without the band's noise floor
        // a table of Gaussian noise, played from random places
        noise_.resize(kTab);
        const double sig = 0.2 / std::sqrt(2.0);
        for (size_t i = 0; i < kTab; i += 2) {
            double u; do { u = uni(); } while (u <= 1e-300);
            const double v = uni(), r = std::sqrt(-2 * std::log(u));
            noise_[i] = cf32((float)(sig * r * std::cos(2 * M_PI * v)), (float)(sig * r * std::sin(2 * M_PI * v)));
            do { u = uni(); } while (u <= 1e-300);
            const double v2 = uni(), r2 = std::sqrt(-2 * std::log(u));
            noise_[i + 1] = cf32((float)(sig * r2 * std::cos(2 * M_PI * v2)), (float)(sig * r2 * std::sin(2 * M_PI * v2)));
        }
        if (!noiseOnly_)
            for (const auto& s : kIonoSynth)
                for (int p = 0; p < 2; p++) {
                    Path q;
                    q.r = s.rateHz; q.P = s.periodSec; q.len = std::min(30e6 / s.rateHz, 0.95 * s.periodSec);
                    q.a = kIonoSynthAmp * s.amp[p];
                    q.u = std::fmod(-s.delaySec[p] - s.offsetSec, q.P);
                    if (q.u < 0) q.u += q.P;
                    if (q.u > q.len) q.u -= q.P;   // waiting for the next sweep
                    q.dw = std::polar(1.0, 2 * M_PI * q.r * dt_ * dt_);
                    paths_.push_back(q);
                }
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override {
        if (clean_) { std::fill(out, out + n, cf32(0.f, 0.f)); g_ += n; }
        else for (size_t i = 0; i < n; i++) {
            if (pos_ == kTab || (g_ & 4095) == 0) pos_ = (size_t)(next() % (kTab - 4096));
            out[i] = noise_[pos_++];
            g_++;
        }
        for (auto& q : paths_) {
            for (size_t i = 0; i < n; i++) {
                const double u = q.u;
                q.u += dt_;
                if (q.u >= q.P) q.u -= q.P;
                const double fb = q.r * u - fc_;
                const bool on = u >= 0 && u < q.len && std::fabs(fb) < 0.5 * fs_;
                if (!on) { q.on = false; continue; }
                if (!q.on || ((g_ - n + i) & 4095) == 0) {   // exact phase and step from the sweep's own time
                    const double ph = 2 * M_PI * std::fmod(q.r * u * u / 2 - fc_ * u, 1.0);
                    q.z = std::polar(q.a, ph);
                    q.w = std::polar(1.0, 2 * M_PI * (fb * dt_ + q.r * dt_ * dt_ / 2));
                    q.on = true;
                }
                out[i] += cf32((float)q.z.real(), (float)q.z.imag());
                q.z *= q.w; q.w *= q.dw;
            }
        }
    }

private:
    static constexpr size_t kTab = 1 << 18;
    struct Path { double r = 0, P = 1, len = 0, a = 0, u = 0; cd z = 0.0, w = 1.0, dw = 1.0; bool on = false; };
    double uni() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    uint64_t next() { s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17; return s_; }
    double fs_, dt_, fc_ = 0;
    bool noiseOnly_ = false, clean_ = false;
    std::vector<cf32> noise_;
    std::vector<Path> paths_;
    size_t pos_ = kTab;
    uint64_t g_ = 0, s_ = 0x9E3779B97F4A7C15ull;
};

} // namespace

// ---- layered ionosphere

namespace {

double fn2(const IonoLayerModel& m, double h) {
    auto par = [h](double fo, double hm, double ym) { if (fo <= 0) return 0.0; const double z = (h - hm) / ym; return std::fabs(z) < 1 ? fo * fo * (1 - z * z) : 0.0; };
    return std::max({par(m.foE, m.hmE, m.ymE), par(m.foF1, m.hmF1, m.ymF1), par(m.foF2, m.hmF2, m.ymF2)});
}

double vh(const IonoLayerModel& m, double f) {
    if (f <= 0) return NAN;
    const double dh = 0.25, f2 = f * f;
    double h = 60, sum = 60, u0 = 1 - fn2(m, h) / f2;
    for (; h < 1500; h += dh) {
        const double u1 = 1 - fn2(m, h + dh) / f2;
        if (u1 <= 0) return sum + 2 * (dh * u0 / (u0 - u1)) / std::sqrt(u0);   // the reflection inside this step (u linear)
        sum += 2 * dh / (std::sqrt(u0) + std::sqrt(u1));
        u0 = u1;
    }
    return NAN;
}

double maxFn(const IonoLayerModel& m) { return std::max({m.foE, m.foF1, m.foF2}); }

// an oblique ray of equivalent vertical frequency fv over n hops: the heard frequency and group path; false when screened by E
bool obliqueRay(const IonoLayerModel& m, double fv, int n, double& fob, double& P) {
    const double h = vh(m, fv);
    if (!std::isfinite(h)) return false;
    const double R = 6371.0, th = m.distKm / (2.0 * n * R);
    const double s = std::sqrt(R * R + (R + h) * (R + h) - 2 * R * (R + h) * std::cos(th));
    const double si = std::min(1.0, R * std::sin(th) / s);
    fob = fv / std::sqrt(1 - si * si);
    P = 2.0 * n * s;
    if (m.foE > 0 && fv > m.foE) {   // an F ray passing the E layer: turned back there if f cos(i at E) is under foE
        const double sie = std::min(1.0, R * (R + h) * std::sin(th) / (s * (R + m.hmE)));
        if (fob * std::sqrt(1 - sie * sie) < m.foE) return false;
    }
    return true;
}

} // namespace

double ionoVirtualHeightKm(const IonoLayerModel& m, double f, bool x) {
    if (!x) return vh(m, f);
    if (m.fH <= 0 || f <= m.fH) return NAN;
    return vh(m, std::sqrt(f * (f - m.fH)));
}

double ionoPathMufMhz(const IonoLayerModel& m, int layer, int hops) {
    const double lo = layer == 1 ? 0.3 : std::max(0.3, m.foE * 1.0001), hi = layer == 1 ? m.foE : maxFn(m);
    double best = NAN, fob, P;
    for (double fv = lo; fv < hi; fv += 0.0005)
        if (obliqueRay(m, fv, hops, fob, P) && (std::isnan(best) || fob > best)) best = fob;
    return best;
}

std::vector<float> makeIonoLayerIonogram(const IonoLayerModel& m) {
    using namespace iono;
    const int R = kRows, C = kCols;
    const double rowKm = kC * kDelayBin;
    std::vector<double> sig((size_t)R * C, 0.0);
    const double A = std::pow(10.0, m.snrDb / 10);
    auto put = [&](int c, double km, double a, bool sum) {
        const int j = (int)std::lround(km / rowKm);
        if (c < 0 || c >= C || j < 0 || j >= R) return;
        for (int d = -1; d <= 1; d++) {
            const int k = j + d;
            if (k < 0 || k >= R) continue;
            const double v = d == 0 ? a : 0.25 * a;
            double& cell = sig[(size_t)k * C + (size_t)c];
            cell = sum ? cell + v : std::max(cell, v);
        }
    };
    if (!m.noiseOnly) {
        if (m.distKm <= 0) {
            const int nsub = 5;
            for (int c = 0; c < C; c++) {
                const double f0 = c * kColHz / 1e6;
                put(c, 0, A, false);   // ground wave
                for (int k = 0; k < nsub; k++) {
                    const double f = f0 + (k + 0.5) / nsub * kColHz / 1e6;
                    for (int x = 0; x < 2; x++) {
                        const double h = ionoVirtualHeightKm(m, f, x == 1);
                        if (!std::isfinite(h)) continue;
                        const double ax = A * (x ? std::pow(10.0, m.xDb / 10) : 1.0) / nsub;
                        for (int n = 1; n <= std::max(1, m.hops); n++) put(c, 2 * n * h, ax * std::pow(10.0, (n - 1) * m.hopDb / 10), true);
                    }
                }
            }
        } else {
            for (int n = 1; n <= std::max(1, m.hops); n++) {
                const double a = A * std::pow(10.0, (n - 1) * m.hopDb / 10);
                double fob, P;
                for (double fv = 0.3; fv < maxFn(m); fv += 0.001)
                    if (obliqueRay(m, fv, n, fob, P)) put((int)std::floor(fob * 1e6 / kColHz), P, a, false);
            }
        }
    }
    for (double cm : m.carriersMhz) {
        const int c = (int)std::floor(cm * 1e6 / kColHz);
        if (c >= 0 && c < C) for (int j = 0; j < R; j++) sig[(size_t)j * C + (size_t)c] += 100.0 * (0.5 + 0.5 * std::sin(j * 0.7));
    }
    uint64_t s = m.seed * 0x9E3779B97F4A7C15ull + 12345;
    auto uni = [&s]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return ((s >> 11) + 0.5) * (1.0 / 9007199254740992.0); };
    std::vector<float> db((size_t)R * C, NAN);
    std::vector<double> col(R), srt;
    for (int c = 0; c < C; c++) {
        const double fc = (c + 0.5) * kColHz / 1e6;
        if (fc < m.fLoMhz || fc > m.fHiMhz) continue;
        for (int j = 0; j < R; j++) {
            double nz = 0;
            for (int b = 0; b < std::max(1, m.blocks); b++) nz -= std::log(uni());
            col[(size_t)j] = nz / std::max(1, m.blocks) + sig[(size_t)j * C + (size_t)c];
        }
        srt = col;
        std::nth_element(srt.begin(), srt.begin() + R / 2, srt.end());
        const double med = std::max(1e-30, srt[(size_t)R / 2]);
        for (int j = 0; j < R; j++) db[(size_t)j * C + (size_t)c] = (float)(10 * std::log10(std::max(1e-30, col[(size_t)j]) / med));
    }
    return db;
}

std::unique_ptr<ModeSynth> makeIonoSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<IonoSynth>(cfg, sampleRate);
}

} // namespace dect2
