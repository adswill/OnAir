// COSPAS-SARSAT second-generation beacon burst receiver (see sgb_rx.h).
#include "dect2/sarsat/sgb_rx.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>

namespace dect2::sarsat {

using dect2::math::kPi;

namespace {

using cd = std::complex<double>;
constexpr int kSps = 4;                       // samples per chip
constexpr int kPreSamples = 6400 * kSps;      // 166.7 ms
constexpr int kBurstSamples = kSgbChips * kSps;   // 1 s
constexpr int kSyms = 150;                    // channel bits per channel: 25 preamble + 125 message
constexpr int kChipsPerSym = 256;

struct Prn {
    std::vector<int8_t> i, q;   // +-1
    Prn(bool selfTest) {
        const std::vector<uint8_t> a = sgbPrn(false, selfTest), b = sgbPrn(true, selfTest);
        i.resize(a.size()); q.resize(b.size());
        for (size_t k = 0; k < a.size(); k++) { i[k] = (int8_t)(1 - 2 * a[k]); q[k] = (int8_t)(1 - 2 * b[k]); }
    }
};
const Prn& prn(bool selfTest) { static const Prn n(false), t(true); return selfTest ? t : n; }

// the preamble waveform at 4 samples per chip: I chips, Q chips half a chip (2 samples) later
const std::vector<cd>& preamble(bool selfTest) {
    static std::vector<cd> w[2];
    std::vector<cd>& v = w[selfTest ? 1 : 0];
    if (v.empty()) {
        const Prn& p = prn(selfTest);
        v.assign(kPreSamples, cd(0, 0));
        for (int n = 0; n < kPreSamples; n++) {
            const double i = p.i[(size_t)(n / kSps)];
            const double q = n >= 2 ? p.q[(size_t)((n - 2) / kSps)] : 0.0;
            v[(size_t)n] = cd(i, q);
        }
    }
    return v;
}

}  // namespace

bool sgbDemodBurst(const std::vector<cf32>& x, long start, double cfoHz, SgbHit& out) {
    const bool kTests[2] = {false, true};
    for (bool selfTest : kTests) {
        if (start < 0 || (size_t)start + (size_t)kBurstSamples + 16 > x.size()) return false;
        const Prn& pr = prn(selfTest);
        const std::vector<cd>& pre = preamble(selfTest);
        const size_t len = (size_t)kBurstSamples + 16;
        std::vector<cd> y(len);
        double f = cfoHz;
        auto build = [&](long st) {
            const cd stp = std::polar(1.0, -2 * kPi * f / kSgbFs);
            cd lo(1, 0);
            for (size_t n = 0; n < len; n++) {
                const long idx = st + (long)n;
                y[n] = (idx >= 0 && (size_t)idx < x.size() ? cd(x[(size_t)idx].real(), x[(size_t)idx].imag()) : cd(0, 0)) * lo;
                lo *= stp;
                if ((n & 1023) == 1023) lo /= std::abs(lo);
            }
        };
        long st = start;
        build(st);
        constexpr int kBlk = 80, kNb = kPreSamples / kBlk;   // 320 blocks
        auto blocks = [&](std::vector<cd>& z) {
            z.assign(kNb, cd(0, 0));
            for (int b = 0; b < kNb; b++) {
                cd a(0, 0);
                for (int n = b * kBlk; n < (b + 1) * kBlk; n++) a += y[(size_t)n] * std::conj(pre[(size_t)n]);
                z[(size_t)b] = a;
            }
        };
        std::vector<cd> z;
        // timing: the shift of -3..+3 samples that gives the most preamble energy
        {
            double best = -1;
            long bs = 0;
            for (long sh = -3; sh <= 3; sh++) {
                double e = 0;
                // non-coherent over blocks: robust against the residual carrier offset
                for (int b = 0; b < kNb; b += 2) {
                    cd a(0, 0);
                    for (int n = b * kBlk; n < (b + 1) * kBlk; n++) { const long k = n + sh; if (k >= 0 && (size_t)k < len) a += y[(size_t)k] * std::conj(pre[(size_t)n]); }
                    e += std::abs(a);
                }
                if (e > best) { best = e; bs = sh; }
            }
            if (bs != 0) { st += bs; build(st); }
        }
        // carrier: block sums, lags 1, 8, 40 blocks (unambiguous +-960, +-120, +-24 Hz)
        for (int lag : {1, 8, 40}) {
            blocks(z);
            cd acc(0, 0);
            for (int b = 0; b + lag < kNb; b++) acc += z[(size_t)(b + lag)] * std::conj(z[(size_t)b]);
            const double d = std::arg(acc) / (2 * kPi * lag * kBlk / kSgbFs);
            f += d;
            const cd stp = std::polar(1.0, -2 * kPi * d / kSgbFs);
            cd lo(1, 0);
            for (size_t n = 0; n < len; n++) { y[n] *= lo; lo *= stp; if ((n & 1023) == 1023) lo /= std::abs(lo); }
        }
        blocks(z);
        cd p0(0, 0);
        for (int b = 0; b < 24; b++) p0 += z[(size_t)b];
        double phi = std::arg(p0);

        // prefix sums for the 4-sample chip windows
        std::vector<cd> P(len + 1, cd(0, 0));
        for (size_t n = 0; n < len; n++) P[n + 1] = P[n] + y[n];
        auto win = [&](double at) -> cd {
            const long i0 = std::lround(at);
            if (i0 < 0 || (size_t)(i0 + kSps) > len) return cd(0, 0);
            return P[(size_t)(i0 + kSps)] - P[(size_t)i0];
        };

        double pos = 0, omega = 0;   // omega in rad per channel bit
        const double spc = kSps / (1.0 + f / 406.05e6);
        const double kp = 0.1011, ki = 0.00539, gt = 0.1;
        std::vector<double> zi(kSyms), zq(kSyms);
        int preOk = 0;
        for (int j = 0; j < kSyms; j++) {
            cd aiP(0, 0), aqP(0, 0), aiE(0, 0), aiL(0, 0), aqE(0, 0), aqL(0, 0);
            for (int k = 0; k < kChipsPerSym; k++) {
                const size_t c = (size_t)(j * kChipsPerSym + k);
                const cd rot = std::polar(1.0, -(phi + omega * k / kChipsPerSym));
                const double ai = pos + k * spc, aq = ai + spc * 0.5;
                const double si = pr.i[c], sq = pr.q[c];
                aiP += win(ai) * rot * si;
                aiE += win(ai - 1) * rot * si;
                aiL += win(ai + 1) * rot * si;
                aqP += win(aq) * rot * sq;
                aqE += win(aq - 1) * rot * sq;
                aqL += win(aq + 1) * rot * sq;
            }
            const double Zi = aiP.real(), Zq = aqP.imag();
            zi[(size_t)j] = Zi; zq[(size_t)j] = Zq;
            double di = Zi >= 0 ? 1 : -1, dq = Zq >= 0 ? 1 : -1;
            if (j < kSyms - 125) { di = dq = 1; preOk += (Zi > 0) + (Zq > 0) == 2; }
            const double mag = std::sqrt(Zi * Zi + Zq * Zq) + 1e-30;
            const double e = (Zq * di - Zi * dq) / (mag * std::sqrt(2.0));
            phi += omega + kp * e;
            omega += ki * e;
            const double E = std::fabs(aiE.real()) + std::fabs(aqE.imag()), L = std::fabs(aiL.real()) + std::fabs(aqL.imag());
            pos += kChipsPerSym * spc + 4.0 * gt * (L - E) / (E + L + 1e-30);
        }
        if (preOk < 20) continue;
        std::vector<uint8_t> bits(kSgbMsgBits);
        for (int m = 0; m < 125; m++) {
            bits[(size_t)(2 * m)] = zi[(size_t)(25 + m)] < 0;
            bits[(size_t)(2 * m + 1)] = zq[(size_t)(25 + m)] < 0;
        }
        const std::vector<uint8_t> received = bits;
        const int fixed = sgbCorrect(bits);
        if (fixed < 0) continue;
        Beacon b;
        if (!sgbDecode(bits, b)) continue;
        b.corrected = fixed;
        b.rxHex = bitsToHex(received, 1, kSgbMsgBits);
        for (int i = 0; i < kSgbMsgBits; i++) if ((bits[(size_t)i] & 1) != (received[(size_t)i] & 1)) b.fixedBits.push_back(i + 1);
        double m1 = 0, m2 = 0;
        for (int m = 0; m < 125; m++) { const double a = std::fabs(zi[(size_t)(25 + m)]), c = std::fabs(zq[(size_t)(25 + m)]); m1 += a + c; m2 += a * a + c * c; }
        m1 /= 250; m2 /= 250;
        const double var = std::max(m2 - m1 * m1, 1e-12 * m1 * m1);
        out.b = b;
        out.b.selfTest = out.b.selfTest || selfTest;
        out.cfoHz = f;
        out.snrDb = (float)(10 * std::log10(std::max(1e-6, m1 * m1 / (2 * var))));
        return true;
    }
    return false;
}

struct SgbDetector::Impl {
    static constexpr size_t N = 262144, kStep = 76800, kMinFill = 184320;
    std::vector<cf32> ring = std::vector<cf32>(N);
    uint64_t total = 0, lastCheck = 0, cand = 0;
    Fft fft{(int)N};
    std::vector<SgbHit> hits;
    std::vector<double> done;   // absolute start samples of decoded bursts

    void check() {
        std::vector<cf32> x(N);
        const uint64_t base = total - N;
        for (size_t i = 0; i < N; i++) x[i] = ring[(size_t)((base + i) & (N - 1))];
        std::vector<cf32> dr(N, cf32(0, 0));
        for (size_t n = 2; n < N; n++) dr[n] = x[n] * std::conj(x[n - 2]);
        fft.forward(dr.data());
        for (int test = 0; test < 2; test++) {
            const std::vector<cd>& pre = preamble(test != 0);
            std::vector<cf32> t(N, cf32(0, 0));
            // the differential product has a large constant part (the chips overlap): taking it out of the template keeps it from correlating with everything
            cd dc(0, 0);
            for (size_t n = 2; n < (size_t)kPreSamples; n++) dc += pre[n] * std::conj(pre[n - 2]);
            dc /= (double)(kPreSamples - 2);
            for (size_t n = 2; n < (size_t)kPreSamples; n++) { const cd v = pre[n] * std::conj(pre[n - 2]) - dc; t[n] = cf32((float)v.real(), (float)v.imag()); }
            fft.forward(t.data());
            std::vector<cf32> c(N);
            for (size_t k = 0; k < N; k++) c[k] = dr[k] * std::conj(t[k]);
            fft.inverse(c.data());
            const size_t valid = N - (size_t)kPreSamples;
            std::vector<float> p(valid);
            double mean = 0;
            for (size_t k = 0; k < valid; k++) { p[k] = std::norm(c[k]); mean += p[k]; }
            mean /= (double)valid;
            if (mean <= 1e-12) continue;   // silence (a noise-free simulation); a real input always has noise
            for (int rep = 0; rep < 3; rep++) {
                size_t pk = 0;
                float pm = 0;
                for (size_t k = 0; k < valid; k++) if (p[k] > pm) { pm = p[k]; pk = k; }
                if (pm < 22.0 * mean) break;
                for (size_t k = pk > 2048 ? pk - 2048 : 0; k < std::min(valid, pk + 2048); k++) p[k] = 0;
                // the mean again without the peak, for the next round
                cand++;
                const double startAbs = (double)(base + pk);
                bool dup = false;
                for (double d : done) if (std::fabs(d - startAbs) < 40000) dup = true;
                if (dup) continue;
                if (pk + (size_t)kBurstSamples + 32 > N) continue;   // not complete yet, a later look finds it
                const double cfo = std::arg(std::complex<double>(c[pk].real(), c[pk].imag())) * kSgbFs / (4 * kPi);
                SgbHit h;
                if (sgbDemodBurst(x, (long)pk, cfo, h)) {
                    h.endSample = startAbs + kBurstSamples;
                    hits.push_back(h);
                    done.push_back(startAbs);
                    if (done.size() > 16) done.erase(done.begin());
                }
            }
        }
    }
};

SgbDetector::SgbDetector() : p_(std::make_unique<Impl>()) {}
SgbDetector::~SgbDetector() = default;
void SgbDetector::reset() { p_->total = p_->lastCheck = p_->cand = 0; p_->hits.clear(); p_->done.clear(); }
uint64_t SgbDetector::candidates() const { return p_->cand; }
std::vector<SgbHit> SgbDetector::take() { std::vector<SgbHit> v; v.swap(p_->hits); return v; }
void SgbDetector::push(const cf32* x, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < n; i++) {
        m.ring[(size_t)(m.total & (Impl::N - 1))] = x[i];
        m.total++;
        if (m.total >= Impl::kMinFill && m.total - m.lastCheck >= Impl::kStep && m.total >= Impl::N) { m.lastCheck = m.total; m.check(); }
    }
}

}  // namespace dect2::sarsat
