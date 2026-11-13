// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BTSC stereo and SAP: see atv_btsc.h. Compander values from FCC OET Bulletin 60, section B(c).
#include "dect2/atv/atv_btsc.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <random>

namespace dect2 {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFh = 4.5e6 / 286;                 // the NTSC line rate
constexpr double kPilotDev = 5000, kMainDev = 25000, kSubDev = 50000, kSapDev = 10000;
constexpr double kDeemph = 75e-6;

// a real first-order section from a pole and a zero given in kHz (s plane, bilinear at kComp, prewarped at the corner)
struct Sos1 {
    double b0 = 1, b1 = 0, a1 = 0, z = 0;
    double run(double x) { const double y = b0 * x + z; z = b1 * x - a1 * y; return y; }
    void reset() { z = 0; }
};
double gComp = 192000;   // set by the filter builder, below
double warp(double fKhz) { return 2 * gComp * std::tan(kPi * fKhz * 1e3 / gComp) / (2 * kPi * 1e3); }   // the analogue corner, kHz
Sos1 pz(double zeroKhz, double poleKhz) {   // (1 + s/zero) / (1 + s/pole): unit gain at DC
    const double K = 2 * gComp / (2 * kPi * 1e3);
    const double z = K / warp(zeroKhz), p = K / warp(poleKhz);
    Sos1 s; const double d = 1 + p; s.b0 = (1 + z) / d; s.b1 = (1 - z) / d; s.a1 = (1 - p) / d; return s;
}
Sos1 hp(double poleKhz) {   // (s/pole) / (1 + s/pole)
    const double K = 2 * gComp / (2 * kPi * 1e3); const double p = K / warp(poleKhz);
    Sos1 s; const double d = 1 + p; s.b0 = p / d; s.b1 = -p / d; s.a1 = (1 - p) / d; return s;
}
Sos1 lp(double poleKhz) {   // 1 / (1 + s/pole)
    const double K = 2 * gComp / (2 * kPi * 1e3); const double p = K / warp(poleKhz);
    Sos1 s; const double d = 1 + p; s.b0 = 1 / d; s.b1 = 1 / d; s.a1 = (1 - p) / d; return s;
}
struct Sos2 {   // biquad, 1 / ((s/w)^2 + s/q + 1) with w, q in kHz
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void reset() { z1 = z2 = 0; }
};
Sos2 lp2(double wKhz, double qKhz) {
    const double K = 2 * gComp / (2 * kPi * 1e3);
    const double A = K * K / (wKhz * wKhz), B = K / qKhz;
    const double d = A + B + 1;
    Sos2 s; s.b0 = 1 / d; s.b1 = 2 / d; s.b2 = 1 / d; s.a1 = (2 - 2 * A) / d; s.a2 = (A - B + 1) / d; return s;
}
double gainF(double f) {    // magnitude of P(f) with f in kHz
    const std::complex<double> j(0, 1);
    return std::abs((j * f / 0.0354) / ((j * f / 0.0354 + 1.0) * (j * f / 2.09 + 1.0)));
}
double gainQ(double f) {
    const std::complex<double> j(0, 1);
    const std::complex<double> a = j * f / 5.86;
    return std::abs(a * a * a / ((-(f / 7.66) * (f / 7.66) + j * f / 7.31 + 1.0) * (j * f / 26.9 + 1.0) * (j * f / 3.92 + 1.0)));
}

// a symmetric low-pass FIR
struct Fir {
    std::vector<float> taps, hist; size_t pos = 0;
    void design(double cutoff, int n) {   // cutoff: cycles per sample
        taps.resize((size_t)n);
        double sum = 0;
        for (int i = 0; i < n; i++) {
            const double t = i - (n - 1) / 2.0;
            const double s = t == 0 ? 2 * cutoff : std::sin(2 * kPi * cutoff * t) / (kPi * t);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
            taps[(size_t)i] = (float)(s * w); sum += s * w;
        }
        for (auto& v : taps) v = (float)(v / sum);
        hist.assign((size_t)n, 0); pos = 0;
    }
    void push(float v) { hist[pos] = v; pos = (pos + 1) % hist.size(); }
    float out() const { float a = 0; size_t k = pos; for (float t : taps) { a += t * hist[k]; k = (k + 1) % hist.size(); } return a; }
    void reset() { std::fill(hist.begin(), hist.end(), 0.f); pos = 0; }
};
}   // namespace

struct BtscFilters {
    Sos1 f1, f2, f1i, f2i;              // the fixed F(f) = (1+jf/0.408)(1+jf/2.19) / ((1+jf/5.23)(1+jf/62.5)), and its inverse
    Sos1 pH, pL;                        // P(f): band pass 35.4 Hz .. 2.09 kHz
    Sos1 q1, q2, q3; Sos2 qq;           // Q(f)
    double rmsP = 0, rmsQ = 0;
    double aP, aQ, ref2P, ref2Q;        // detector smoothing, the squares of the 0 dB levels
    double sZ = 0;                      // spectral section state
    double e = 1, b = 1;
    double kComp;
    explicit BtscFilters(double rate) : kComp(rate) {
        gComp = rate;
        f1 = pz(0.408, 5.23); f2 = pz(2.19, 62.5); f1i = pz(5.23, 0.408); f2i = pz(62.5, 2.19);
        pH = hp(0.0354); pL = lp(2.09);
        q1 = hp(5.86); q2 = hp(5.86); q3 = lp(26.9); qq = lp2(7.66, 7.31);
        aP = 1.0 - std::exp(-1.0 / (0.0347 * kComp)); aQ = 1.0 - std::exp(-1.0 / (0.0114 * kComp));
        const double rP = 0.0899 * gainF(0.300), rQ = 0.0516 * gainQ(8.0);
        ref2P = rP * rP / 2; ref2Q = rQ * rQ / 2;     // RMS of a sine with the reference peak
        rmsP = ref2P; rmsQ = ref2Q;
    }
    // Q has a third-order zero at DC: the three high passes are one pole each with the 5.86 kHz numerator, the poles at 3.92 kHz taken by the last section
    double weightQ(double y) { return qq.run(q3.run(q2.run(q1.run(y)))); }
    void update(double y) {
        // both detectors look at the encoded signal, so compressor and expander follow the same values
        const double p = pL.run(pH.run(y));
        const double q = weightQ(y) * (3.92 / 5.86);   // the 3.92 kHz pole as a fixed factor of the high pass pair above
        rmsP += aP * (p * p - rmsP);
        rmsQ += aQ * (q * q - rmsQ);
        e = std::sqrt(std::max(1e-10, rmsP / ref2P));
        b = std::clamp(std::sqrt(std::max(1e-10, rmsQ / ref2Q)), 0.003, 300.0);
    }
    // the spectral section (1 + jf/F (b+51)/(b+1)) / (1 + jf/F (1+51 b)/(b+1)), F = 20.1 kHz, direct and inverse
    void coeffs(double& A, double& C) const {
        const double K = 2 * kComp / (2 * kPi * 20.1e3);
        A = K * (b + 51) / (b + 1); C = K * (1 + 51 * b) / (b + 1);
    }
    double spectral(double x) {
        double A, C; coeffs(A, C);
        const double n0 = 1 + A, n1 = 1 - A, d0 = 1 + C, d1 = 1 - C;
        const double y = (n0 * x + sZ) / d0;
        sZ = n1 * x - d1 * y;
        return y;
    }
    double spectralInv(double y) {
        double A, C; coeffs(A, C);
        const double n0 = 1 + A, n1 = 1 - A, d0 = 1 + C, d1 = 1 - C;
        const double x = (d0 * y - sZ) / n0;
        sZ = n1 * x - d1 * y;
        return x;
    }
};

BtscCompander::BtscCompander(double rate) : rate_(rate), f_(new BtscFilters(rate)) {}
BtscCompander::~BtscCompander() = default;
void BtscCompander::reset() { f_.reset(new BtscFilters(rate_)); }
void BtscCompander::setRate(double rate) { rate_ = rate; reset(); }
double BtscCompander::wideband() const { return f_->e; }
double BtscCompander::spectral() const { return f_->b; }
float BtscCompander::compress(float x) {
    BtscFilters& f = *f_;
    double v = f.f2.run(f.f1.run(x));
    v /= std::max(0.002, f.e);
    v = std::clamp(v, -4.0, 4.0);
    const double y = f.spectral(v);
    f.update(y);
    return (float)y;
}
float BtscCompander::expand(float y) {
    BtscFilters& f = *f_;
    double v = f.spectralInv(y);
    v = std::clamp(v, -4.0, 4.0);
    v *= std::max(0.002, f.e);
    f.update(y);
    return (float)f.f2i.run(f.f1i.run(v));
}

// ------------------------------------------------------------------------------------------------ receiver

struct BtscRx::Impl {
    double fs = 0; int dec = 1; double outRate = 384000;
    Fir ifI, ifQ; size_t ifPhase = 0;
    double mixPh = 0;
    std::complex<float> prev{1, 0};
    double fsDisc = 384000;
    // pilot loop
    double ph = 0, frq = 0, pI = 0, pQ = 0, pilotT = 0; bool pilot = false; double pAmp = 0;
    // L+R, L-R, SAP filters at 192 kHz
    Fir lpMain, lpSub, lpOut1, lpOut2, sapI, sapQ, sapOut; size_t cnt = 0, cnt2 = 0;
    double dm = 0;                                   // de-emphasis state
    double dmA = 0, rate2 = 192000, outStep = 4, outPos = 0; float o1p = 0, o2p = 0;
    BtscCompander comp, compSap;
    double sapPh = 0; std::complex<float> sapPrev{1, 0}; double sapLvl = 0, sapNoise = 0; bool sap = false; double sapAvg = 0; double sapT = 0;
    float subAvg = 0;
    BtscStatus st;
    void configure(double f) {
        fs = f; dec = std::max(1, (int)std::lround(fs / 384000.0)); fsDisc = fs / dec;
        ifI.design(130e3 / fs, std::clamp(dec * 16 + 1, 31, 801)); ifQ = ifI;
        const double r = fsDisc;
        lpMain.design(16e3 / r, 127); lpSub = lpMain;
        sapI.design(14e3 / r, 127); sapQ = sapI; sapOut.design(8e3 / r, 63);
        rate2 = fsDisc / 2; dmA = 1.0 - std::exp(-1.0 / (kDeemph * rate2)); comp.setRate(rate2); compSap.setRate(rate2);
        lpOut1.design(16e3 / rate2, 63); lpOut2 = lpOut1; outStep = rate2 / 48000; outPos = 0;
        reset();
    }
    void reset() {
        ifPhase = 0; prev = {1, 0}; ph = 0; frq = 2 * kPi * kFh / fsDisc; pI = pQ = 0; pilot = false; pilotT = 0; mixPh = 0;
        ifI.reset(); ifQ.reset(); lpMain.reset(); lpSub.reset(); lpOut1.reset(); lpOut2.reset(); sapI.reset(); sapQ.reset(); sapOut.reset();
        cnt = cnt2 = 0; outPos = 0; o1p = o2p = 0; dm = 0; comp.reset(); compSap.reset(); sapPh = 0; sapPrev = {1, 0}; sapAvg = 0; sapT = 0; sap = false; st = BtscStatus(); subAvg = 0; sapLvl = sapNoise = 0;
    }
    void process(const cf32* x, size_t n, double carrierHz, bool wantSap, std::vector<float>& l, std::vector<float>& r) {
        const double w = -2 * kPi * carrierHz / fs;
        for (size_t i = 0; i < n; i++) {
            const cf32 m = x[i] * cf32((float)std::cos(mixPh), (float)std::sin(mixPh));
            mixPh = std::fmod(mixPh + w, 2 * kPi);
            ifI.push(m.real()); ifQ.push(m.imag());
            if (++ifPhase < (size_t)dec) continue;
            ifPhase = 0;
            const std::complex<float> v(ifI.out(), ifQ.out());
            const float dphi = std::arg(v * std::conj(prev)); prev = v;
            step(dphi / (2 * (float)kPi) * (float)fsDisc, wantSap, l, r);
        }
    }
    void step(float dev, bool wantSap, std::vector<float>& l, std::vector<float>& r) {   // dev: instantaneous deviation, Hz
        // pilot loop: integrate the deviation against the local oscillator (loop bandwidth about 4 Hz)
        const double c = std::cos(ph), s = std::sin(ph);
        pI += 0.0006 * (dev * c - pI); pQ += 0.0006 * (dev * -s - pQ);
        const double err = std::atan2(pQ, std::max(1e-6, pI));
        const double wn = 2 * kPi * 8 / fsDisc;
        if (pI * 2 > 1500) frq += wn * wn * err; else frq += 0.0002 * (2 * kPi * kFh / fsDisc - frq);
        ph = std::fmod(ph + frq + 1.4 * wn * err, 2 * kPi);
        pAmp = 2 * std::hypot(pI, pQ);
        const bool strong = pAmp > 1500 && std::fabs(pQ) < 0.4 * pI && std::fabs(frq * fsDisc / (2 * kPi) - kFh) < 80;
        if (strong) { pilotT += 1 / fsDisc; if (pilotT > 0.15) pilotT = 0.15; } else { pilotT -= 1 / fsDisc; if (pilotT < 0) pilotT = 0; }
        if (!pilot && pilotT > 0.1) pilot = true;
        if (pilot && pilotT < 0.02) pilot = false;
        // the subcarrier: twice the pilot phase, DSB-SC with the sine at phase 2 theta (the pilot is cos here)
        const double c2 = c * c - s * s;   // (cos of twice the phase the deviation was compared with)
        // the pilot is taken out first (it sits just above the audio band, and the sub channel's level detectors would follow it)
        const float clean = pilot ? dev - (float)(2 * pI * c + 2 * pQ * s) : dev;
        lpMain.push(clean); lpSub.push((float)(2 * clean * c2));
        // SAP: mix 5 fH (locked to the pilot when it is there) to baseband
        double sw = pilot ? 5 * ph : sapPh; if (!pilot) sapPh = std::fmod(sapPh + 2 * kPi * 5 * kFh / fsDisc, 2 * kPi);
        sapI.push((float)(dev * std::cos(sw))); sapQ.push((float)(-dev * std::sin(sw)));
        if (++cnt < 2) return;   // 384 kHz (or so) to 192 kHz
        cnt = 0;
        const float main0 = lpMain.out() / (float)kMainDev;
        dm += dmA * (main0 - dm);
        const float y = lpSub.out() / (float)kSubDev;
        subAvg += 0.001f * (std::fabs(y) - subAvg);
        const float x = pilot ? comp.expand(y) : 0.f;
        const std::complex<float> sv(sapI.out(), sapQ.out());
        const float sf = std::arg(sv * std::conj(sapPrev)) / (2 * (float)kPi) * (float)(fsDisc / 2); sapPrev = sv;
        const double smag = std::abs(sv);
        sapAvg += 0.002 * (smag - sapAvg);
        const bool sapOn = sapAvg > 1500;
        if (sapOn) { sapT = std::min(0.15, sapT + 2 / fsDisc); } else sapT = std::max(0.0, sapT - 2 / fsDisc);
        if (!sap && sapT > 0.1) sap = true; if (sap && sapT < 0.02) sap = false;
        const float sy = sf / (float)kSapDev;
        sapOut.push(sy);
        const float sapX = sap ? compSap.expand(sapOut.out()) : 0.f;
        const float L = (float)(dm + x) * 0.5f, R = (float)(dm - x) * 0.5f;
        lpOut1.push(wantSap && sap ? sapX : L); lpOut2.push(wantSap && sap ? sapX : R);
        const float o1 = lpOut1.out(), o2 = lpOut2.out();
        outPos += 1.0;
        while (outPos >= outStep) {    // 48 kHz: linear between the samples of the band-limited signal
            outPos -= outStep;
            const float fr = (float)(1.0 - outPos);
            l.push_back(o1p + fr * (o1 - o1p)); r.push_back(o2p + fr * (o2 - o2p));
        }
        o1p = o1; o2p = o2;
        st.pilot = pilot; st.sap = sap; st.pilotKhz = pAmp / 1e3; st.pilotErrHz = frq * fsDisc / (2 * kPi) - kFh; st.subLevel = subAvg;
    }
};

BtscRx::BtscRx() : p_(new Impl) {}
BtscRx::~BtscRx() = default;
void BtscRx::configure(double fs) { p_->configure(fs); }
void BtscRx::reset() { p_->reset(); }
void BtscRx::process(const cf32* x, size_t n, double f, bool s, std::vector<float>& l, std::vector<float>& r) { if (p_->fs > 0) p_->process(x, n, f, s, l, r); }
BtscStatus BtscRx::status() const { return p_->st; }

// ------------------------------------------------------------------------------------------------ transmitter

struct BtscTx::Impl {
    BtscTxConfig c; double t = 0; double ph = 0; BtscCompander comp, compSap; double pre = 0; float y = 0, sapY = 0; int sub = 0; double sapPh = 0; double preA;
    std::mt19937 rng; dect2::normal_distribution<float> nd{0.f, 1.f}; float sigma = 0;
    Impl(const BtscTxConfig& cc) : c(cc), rng(cc.seed) {
        const int step = std::max(1, (int)std::lround(c.rate / 192000.0));
        const double ar = c.rate / step; comp.setRate(ar); compSap.setRate(ar);
        preA = 1.0 - std::exp(-1.0 / (kDeemph * ar));
        if (c.cnrDb < 200) sigma = (float)std::sqrt(std::pow(10.0, -c.cnrDb / 10) * c.rate / 200e3 / 2);
    }
    double main0 = 0; double preY = 0, prevX = 0;
    void generate(size_t n, std::vector<cf32>& out) {
        out.reserve(out.size() + n);
        const int step = std::max(1, (int)std::lround(c.rate / 192000.0));
        for (size_t i = 0; i < n; i++) {
            if (sub == 0) {
                const double L = c.level * std::sin(2 * kPi * c.toneLHz * t), R = c.level * std::sin(2 * kPi * c.toneRHz * t);
                const double m = L + R;
                // pre-emphasis 75 us (the inverse of the receiver's one-pole de-emphasis)
                main0 = (m - (1 - preA) * prevX) / preA; prevX = m;
                y = c.stereo ? comp.compress((float)(L - R)) : 0.f;
                sapY = c.sap ? compSap.compress((float)(c.level * std::sin(2 * kPi * c.sapHz * t))) : 0.f;
            }
            if (++sub >= step) sub = 0;
            const double th = 2 * kPi * kFh * t;
            double d = main0 * kMainDev;
            if (c.stereo) d += kPilotDev * std::cos(th) + y * kSubDev * std::cos(2 * th);
            if (c.sap) { const double f5 = 5 * kFh + sapY * kSapDev; sapPh = std::fmod(sapPh + 2 * kPi * f5 / c.rate, 2 * kPi); d += 15000 * std::cos(sapPh); }
            ph = std::fmod(ph + 2 * kPi * (d + c.carrierHz) / c.rate, 2 * kPi);
            cf32 v((float)std::cos(ph), (float)std::sin(ph));
            if (sigma > 0) v += cf32(nd(rng) * sigma, nd(rng) * sigma);
            out.push_back(v);
            t += 1 / c.rate;
        }
    }
};
BtscTx::BtscTx(const BtscTxConfig& c) : p_(new Impl(c)) {}
BtscTx::~BtscTx() = default;
void BtscTx::generate(size_t n, std::vector<cf32>& out) { p_->generate(n, out); }

} // namespace dect2
