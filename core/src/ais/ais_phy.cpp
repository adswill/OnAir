// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/ais/ais_phy.h"
#include "dect2/dmr/dmr_dsp.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include "dect2/dsp/fftutil.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using dect2::math::kPi;

// phase (unwrapped, in radians) at a fractional sample position
struct PhaseTrack {
    std::vector<double> ph;      // ph[i]: sum of the phase steps up to sample i
    double at(double t) const {
        if (t <= 0) return ph.front();
        const double last = (double)ph.size() - 1;
        if (t >= last) return ph.back();
        const size_t i = (size_t)t;
        const double f = t - (double)i;
        return ph[i] * (1 - f) + ph[i + 1] * f;
    }
};

struct Slice {
    std::vector<double> m;       // per bit: frequency integral minus the offset, radians (positive = frequency above the centre)
    int first = 0;               // index of the bit that m[0] belongs to (the bit time is tau + 5 * (first + i))
};

// Integrate and dump: the frequency over one bit as the phase change across a window of 'w' samples centred on the bit
Slice sliceBits(const PhaseTrack& pt, double tau, double f0, double w, size_t n) {
    Slice s;
    const double half = w / 2;
    const int k0 = (int)std::ceil((half - tau) / kAisSps);
    const int k1 = (int)std::floor(((double)n - 1 - half - tau) / kAisSps);
    s.first = k0;
    for (int k = k0; k <= k1; k++) {
        const double t = tau + kAisSps * k;
        s.m.push_back(pt.at(t + half) - pt.at(t - half) - f0 * w);
    }
    return s;
}

// The frequency pulse of GMSK (BT 0.4) is three bits wide: the integral over one bit is alpha * a[k] + beta * (a[k-1] + a[k+1]) with a = +-1
// (alpha 0.74 and beta 0.13 of the full swing before the receiver's filters). A four state Viterbi search over the line bits finds the sequence that
// fits the integrals best, so alternating bits (the training sequence, many flags) no longer lose half of their eye opening.
void mlseBits(const std::vector<double>& y, double al, double be, ais::Bits& out) {
    const size_t K = y.size();
    out.assign(K, 0);
    if (K < 3) return;
    std::vector<std::array<uint8_t, 4>> back(K + 1);
    double cost[4] = {0, 0, 0, 0}, nc[4];
    for (size_t k = 0; k < K; k++) {
        for (int ns = 0; ns < 4; ns++) nc[ns] = 1e300;
        for (int st = 0; st < 4; st++) {                       // st = (a[k-1] << 1) | a[k]
            const double pm = (st >> 1) ? 1.0 : -1.0, cu = (st & 1) ? 1.0 : -1.0;
            for (int nb = 0; nb < 2; nb++) {
                const double nx = nb ? 1.0 : -1.0;
                const double e = y[k] - (al * cu + be * (pm + nx));
                const double c = cost[st] + e * e;
                const int to = ((st & 1) << 1) | nb;
                if (c < nc[to]) { nc[to] = c; back[k + 1][to] = (uint8_t)st; }
            }
        }
        for (int i = 0; i < 4; i++) cost[i] = nc[i];
    }
    int st = 0;
    for (int i = 1; i < 4; i++) if (cost[i] < cost[st]) st = i;
    for (size_t k = K; k > 0; k--) {
        st = back[k][st];
        out[k - 1] = (uint8_t)(st & 1);                       // state before step k-1 is (a[k-2], a[k-1])
    }
}


// Coherent detection. GMSK with modulation index 0.5 turns the carrier by +-90 degrees per bit, so at the boundaries between bits the phase is
// theta + 90 degrees * S, S being the running sum of the line levels (+-1) modulo 4. A half-sine window two bits wide centred on each boundary
// collects the signal there; along a phase reference taken from the guide's levels (32 bits around each boundary, after a fine frequency search
// over the whole burst by FFT) a four-state Viterbi over S finds the levels. The frequency discriminator's noise (the clicks near the threshold) is
// gone, which is worth about 2 dB. `guide` holds line levels for the bits of the slice (bit i at tau + 5 (first + i)).
static bool coherentTry(const cf32* x, size_t n, double tau, int first, const ais::Bits& guide, double off, ais::HdlcResult& out, Fft& fft,
                        std::vector<cf32>& fbuf, ais::Bits* levels = nullptr, double* offOut = nullptr) {
    const size_t K = guide.size();
    if (K < 40) return false;
    // boundary j sits before bit j: t = tau + 5 (first + j) - 2.5, j = 0 .. K
    const size_t J = K + 1;
    std::vector<std::complex<double>> z(J);
    auto sampleAt = [&](double t) -> std::complex<double> {
        if (t < 0 || t > (double)n - 1.001) return 0.0;
        const size_t i = (size_t)t;
        const double f = t - (double)i;
        const cf32 v = x[i] * (float)(1 - f) + x[i + 1] * (float)f;
        return std::polar(1.0, -off * t) * std::complex<double>(v.real(), v.imag());
    };
    for (size_t j = 0; j < J; j++) {
        const double tb = tau + kAisSps * ((double)first + (double)j) - 2.5;
        std::complex<double> acc = 0;
        for (int d = -4; d <= 4; d++) acc += std::cos(kPi * d / 10.0) * sampleAt(tb + d);   // half sine over two bits (10 samples)
        z[j] = acc;
    }
    // the guide's phase steps: S_j = sum of the levels before boundary j
    std::vector<int> Sg(J, 0);
    for (size_t j = 1; j < J; j++) Sg[j] = (Sg[j - 1] + (guide[j - 1] ? 1 : 3)) & 3;
    static const std::complex<double> jpow[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    std::vector<std::complex<double>> v(J);
    for (size_t j = 0; j < J; j++) v[j] = z[j] * std::conj(jpow[Sg[j]]);
    // fine frequency: the turn per bit that lines the boundaries up, within +-0.4 rad per bit (+-600 Hz)
    {
        const int N = fft.size();
        fbuf.assign((size_t)N, cf32(0, 0));
        for (size_t j = 0; j < J && (int)j < N; j++) fbuf[j] = cf32((float)v[j].real(), (float)v[j].imag());
        fft.forward(fbuf.data());
        const int Kb = (int)std::ceil(0.4 * N / (2 * kPi));
        auto at = [&](int k) { return (double)std::norm(fbuf[(size_t)((k + N) % N)]); };
        int bi = 0; double best = -1;
        for (int k = -Kb; k <= Kb; k++) { const double e = at(k); if (e > best) { best = e; bi = k; } }
        double kf = bi;
        const double y0 = at(bi - 1), y1 = at(bi), y2 = at(bi + 1), den = y0 - 2 * y1 + y2;
        if (den < 0) kf += 0.5 * (y0 - y2) / den;
        double w = 2 * kPi * kf / N;
        auto pw = [&](double ww) { std::complex<double> s(0, 0); for (size_t j = 0; j < J; j++) s += v[j] * std::polar(1.0, -ww * (double)j); return std::norm(s); };
        if (pw(-w) > pw(w)) w = -w;                       // whichever sign convention the FFT has
        for (size_t j = 0; j < J; j++) { const std::complex<double> r = std::polar(1.0, -w * (double)j); z[j] *= r; v[j] *= r; }
        if (offOut) *offOut = off + w / kAisSps;   // the burst's frequency, better than the discriminator's (which another burst on top can pull)
    }
    // phase reference per boundary: the guide-corrected samples of 32 boundaries around it, itself left out
    std::vector<std::complex<double>> ref(J);
    {
        const int H = 16;
        std::complex<double> win = 0;
        size_t lo = 0, hi = 0;
        for (size_t j = 0; j < J; j++) {
            const size_t wl = j > (size_t)H ? j - H : 0, wh = std::min(J, j + H + 1);
            while (hi < wh) win += v[hi++];
            while (lo < wl) win -= v[lo++];
            const std::complex<double> r = win - v[j];
            const double m = std::abs(r);
            ref[j] = m > 0 ? std::conj(r) / m : std::complex<double>(0, 0);
        }
    }
    // Viterbi over S (mod 4): a level of +1 is S + 1, -1 is S - 1
    std::vector<std::array<uint8_t, 4>> back(J);
    double cost[4], nc[4];
    for (int st = 0; st < 4; st++) cost[st] = (z[0] * ref[0] * std::conj(jpow[st])).real();
    for (size_t j = 1; j < J; j++) {
        for (int st = 0; st < 4; st++) nc[st] = -1e300;
        for (int st = 0; st < 4; st++) for (int up = 0; up < 2; up++) {
            const int to = (st + (up ? 1 : 3)) & 3;
            const double c = cost[st] + (z[j] * ref[j] * std::conj(jpow[to])).real();
            if (c > nc[to]) { nc[to] = c; back[j][to] = (uint8_t)st; }
        }
        for (int st = 0; st < 4; st++) cost[st] = nc[st];
    }
    int st = 0;
    for (int i = 1; i < 4; i++) if (cost[i] > cost[st]) st = i;
    ais::Bits line(K);
    for (size_t j = J - 1; j > 0; j--) {
        const int pr = back[j][st];
        line[j - 1] = (uint8_t)(((st - pr) & 3) == 1);
        st = pr;
    }
    const ais::Bits dec = ais::nrziDecode(line, line.empty() ? 1 : line[0]);
    out = ais::hdlcFrames(dec);
    if (levels) *levels = std::move(line);
    return !out.good.empty();
}

} // namespace

static AisBurstResult decodeOnce(const cf32* x, size_t n, size_t core0, size_t core1, const AisPhyConfig& cfg) {
    AisBurstResult res;
    if (n < 100) return res;
    PhaseTrack pt;
    pt.ph.assign(n, 0.0);
    std::vector<double> step(n, 0.0);
    for (size_t i = 1; i < n; i++) {
        const cf32 p = x[i] * std::conj(x[i - 1]);
        step[i] = std::atan2((double)p.imag(), (double)p.real());
        pt.ph[i] = pt.ph[i - 1] + step[i];
    }
    // mean frequency inside the burst, away from its edges
    const size_t margin = 24;
    size_t a = std::min(core0 + margin, n - 1), b = core1 > margin ? std::min(core1 - margin, n) : 0;
    if (b <= a + 20) { a = std::min(core0, n - 1); b = std::min(core1, n); }
    if (b <= a + 20) return res;
    double f0 = 0;
    for (size_t i = a; i < b; i++) f0 += step[i];
    f0 /= (double)(b - a);

    // eye opening for each timing phase: the mean of |integral| inside the burst
    struct Hyp { double tau, metric; };
    std::vector<Hyp> hyp;
    for (int ti = 0; ti < 10; ti++) {
        const double tau = 0.5 * ti;
        const Slice s = sliceBits(pt, tau, f0, cfg.intWidth, n);
        double sum = 0; int cnt = 0;
        for (size_t i = 0; i < s.m.size(); i++) {
            const double t = tau + kAisSps * (s.first + (int)i);
            if (t < (double)a || t > (double)b) continue;
            sum += std::fabs(s.m[i]); cnt++;
        }
        hyp.push_back({tau, cnt ? sum / cnt : 0.0});
    }
    std::sort(hyp.begin(), hyp.end(), [](const Hyp& p, const Hyp& q) { return p.metric > q.metric; });

    const int tries = std::min<int>(cfg.hypotheses, (int)hyp.size());
    for (int h = 0; h < tries; h++) {
        res.tauTried++;
        const double tau = hyp[h].tau;
        double off = f0;
        Slice s = sliceBits(pt, tau, off, cfg.intWidth, n);
        for (int it = 0; it < cfg.refine; it++) {
            // the two levels of the frequency (marks and spaces), their middle is the slicer
            double hi = 0, lo = 0; int nh = 0, nl = 0;
            for (size_t i = 0; i < s.m.size(); i++) {
                const double t = tau + kAisSps * (s.first + (int)i);
                if (t < (double)a || t > (double)b) continue;
                if (s.m[i] > 0) { hi += s.m[i]; nh++; } else { lo += s.m[i]; nl++; }
            }
            if (!nh || !nl) break;
            const double mid = 0.5 * (hi / nh + lo / nl) / cfg.intWidth;
            if (std::fabs(mid) < 1e-6) break;
            off += mid;
            s = sliceBits(pt, tau, off, cfg.intWidth, n);
        }
        ais::Bits line(s.m.size());
        for (size_t i = 0; i < s.m.size(); i++) line[i] = s.m[i] > 0;
        const ais::Bits dec = ais::nrziDecode(line, line.empty() ? 1 : line[0]);
        ais::HdlcResult r = ais::hdlcFrames(dec);
        ais::Bits guideBits = line;
        if (r.good.empty() && cfg.mlse && s.m.size() > 40) {
            // fit alpha and beta to the hard decisions inside the burst, then search for the best sequence
            std::vector<double> y(s.m.size());
            for (size_t i = 0; i < y.size(); i++) y[i] = s.m[i] / (kPi / 2);
            ais::Bits cur = line;
            for (int pass = 0; pass < 2; pass++) {
                double saa = 0, sab = 0, sbb = 0, sya = 0, syb = 0;
                for (size_t i = 1; i + 1 < cur.size(); i++) {
                    const double t = tau + kAisSps * (s.first + (int)i);
                    if (t < (double)a || t > (double)b) continue;
                    const double ak = cur[i] ? 1 : -1, nb = (cur[i - 1] ? 1 : -1) + (cur[i + 1] ? 1 : -1);
                    saa += ak * ak; sab += ak * nb; sbb += nb * nb; sya += y[i] * ak; syb += y[i] * nb;
                }
                const double det = saa * sbb - sab * sab;
                if (std::fabs(det) < 1e-9) break;
                const double al = (sya * sbb - syb * sab) / det, be = (syb * saa - sya * sab) / det;
                if (al < 0.2) break;
                ais::Bits ml;
                mlseBits(y, al, be, ml);
                const ais::Bits d2 = ais::nrziDecode(ml, ml.empty() ? 1 : ml[0]);
                ais::HdlcResult r2 = ais::hdlcFrames(d2);
                if (!r2.good.empty()) { r = std::move(r2); break; }
                guideBits = ml;
                if (ml == cur) break;
                cur = ml;
            }
        }
        if (r.good.empty() && cfg.coherent && s.m.size() > 40) {
            // coherent detection, guided by the levels found so far (the sequence search's when it ran, the hard decisions otherwise)
            static thread_local Fft fft(1024);
            static thread_local std::vector<cf32> fbuf;
            ais::HdlcResult rc;
            ais::Bits lv;
            double offC = off;
            if (coherentTry(x, n, tau, s.first, guideBits, off, rc, fft, fbuf, &lv, &offC)) { r = std::move(rc); off = offC; }
            else if (lv != guideBits && coherentTry(x, n, tau, s.first, lv, off, rc, fft, fbuf, nullptr, &offC)) { r = std::move(rc); off = offC; }   // again, guided by its own levels
        }
        if (h == 0) res.bad = r.bad;
        if (!r.good.empty()) {
            res.frames = std::move(r.good);
            res.bad = 0;
            res.tauOk = (int)std::lround(tau * 10);
            res.cfoHz = off * kAisWorkRate / (2 * kPi);
            return res;
        }
    }
    res.cfoHz = f0 * kAisWorkRate / (2 * kPi);
    return res;
}

// The burst shifted down by 'w' radians per sample and filtered with the narrow filter
static std::vector<cf32> narrowAt(const cf32* x, size_t n, double w, const AisPhyConfig& cfg) {
    const std::vector<float> h = dmr::lowpassTaps(cfg.narrowPassHz, cfg.narrowStopHz, kAisWorkRate, 60, 9);
    const int m = (int)h.size() / 2;
    std::vector<cf32> rot(n), y(n);
    for (size_t i = 0; i < n; i++) rot[i] = x[i] * std::polar(1.0f, (float)(-w * (double)i));
    for (size_t i = 0; i < n; i++) {
        float re = 0, im = 0;
        const long lo = std::max<long>(0, (long)i - m), hi = std::min<long>((long)n - 1, (long)i + m);
        for (long k = lo; k <= hi; k++) { const float hv = h[(size_t)(k - (long)i + m)]; re += hv * rot[(size_t)k].real(); im += hv * rot[(size_t)k].imag(); }
        y[i] = cf32(re, im);
    }
    return y;
}

// Two passes: the channel filter of the receiver has to pass every carrier offset, so it is wide. Once the burst's own frequency is known, the burst is
// shifted to zero and filtered again with a narrower filter, which takes away noise (about 1.2 dB at 90 % of the bursts) whatever the offset was.
AisBurstResult aisDecodeBurst(const cf32* x, size_t n, size_t core0, size_t core1, const AisPhyConfig& cfg) {
    if (cfg.narrowPassHz > 0 && n > 200 && core1 > core0 + 40) {
        // frequency of the burst: the mean phase step inside it
        const size_t margin = 24;
        const size_t a = std::min(core0 + margin, n - 2), b = core1 > margin ? std::min(core1 - margin, n) : 0;
        if (b > a + 20) {
            double acc = 0;
            cf32 sum(0, 0);
            for (size_t i = a; i < b; i++) sum += x[i] * std::conj(x[i - 1]);
            acc = std::atan2((double)sum.imag(), (double)sum.real());
            const std::vector<cf32> y = narrowAt(x, n, acc, cfg);
            AisBurstResult r = decodeOnce(y.data(), n, core0, core1, cfg);
            r.cfoHz += acc * kAisWorkRate / (2 * kPi);
            r.narrow = true;
            if (!r.frames.empty()) return r;
            AisBurstResult w = decodeOnce(x, n, core0, core1, cfg);     // the wide pass as the second chance
            if (!w.frames.empty() || r.bad <= w.bad) return w;
            r.narrow = false;
            return r;
        }
    }
    return decodeOnce(x, n, core0, core1, cfg);
}

AisBurstResult aisDecodeBurstAt(const cf32* x, size_t n, size_t core0, size_t core1, double freqHz, const AisPhyConfig& cfg) {
    const double w = 2 * kPi * freqHz / kAisWorkRate;
    AisPhyConfig c = cfg;
    if (c.narrowPassHz <= 0) { c.narrowPassHz = 3500; c.narrowStopHz = 8500; }
    const std::vector<cf32> y = narrowAt(x, n, w, c);
    AisBurstResult r = decodeOnce(y.data(), n, core0, core1, c);
    r.cfoHz += freqHz;
    r.narrow = true;
    return r;
}

} // namespace dect2
