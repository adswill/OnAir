// Signal identifier (sig_ident.h): measures a carrier from its samples and ranks the modes it could be.
#include "dect2/common/thread_name.h"
#include "dect2/engine/sig_ident.h"
#include "sig_known.h"
#include "dect2/dmr/dmr_proto.h"
#include "dect2/dpmr/dpmr_proto.h"
#include "dect2/bandplan/bandplan.h"
#include "dect2/channels/freq_db.h"
#include "dect2/channels/tvplan.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/pager/pager_proto.h"
#include "dect2/ros/ros_detect.h"
#include "dect2/specx/specx_measure.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace sigid {

namespace {

using dect2::math::kPi;

double lin(double db) { return std::pow(10.0, db / 10); }
double dB(double x) { return 10 * std::log10(std::max(1e-30, x)); }
size_t pow2At(size_t n) { size_t k = 1; while (k < n) k <<= 1; return k; }
template <class T> double median(std::vector<T> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
    return (double)v[v.size() / 2];
}
template <class T> double quantile(std::vector<T> v, double q) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, (size_t)(q * (double)v.size()));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return (double)v[k];
}

// |FFT|^2 of z (zero padded to a power of two, at most maxN points: z is cut to that)
std::vector<float> powerSpec(const cf32* z, size_t n, size_t maxN = (size_t)1 << 20) {
    size_t N = pow2At(std::max<size_t>(n, 16));
    if (N > maxN) { N = maxN; n = std::min(n, N); }
    std::vector<cf32> b(N, cf32(0, 0));
    std::copy(z, z + n, b.begin());
    Fft f((int)N);
    f.forward(b.data());
    std::vector<float> p(N);
    for (size_t k = 0; k < N; k++) p[k] = std::norm(b[k]);
    return p;
}

struct Line { double hz = 0, db = 0; bool ok = false; };

// The strongest line of a power spectrum P (length N, rate fs, FFT order) with its frequency in [f0, f1] (signed Hz), measured against
// the mean of the bins around it (a window of 2W+1 bins without the 5 in the middle). exclude: bins around this frequency are skipped.
Line findLine(const std::vector<float>& P, double fs, double f0, double f1, double excludeHz = 1e300, double excludeWidth = 0) {
    Line r;
    const int N = (int)P.size();
    if (N < 64) return r;
    const double bin = fs / N;
    const int W = std::max(24, std::min(N / 64, 400));
    int k0 = (int)std::ceil(f0 / bin), k1 = (int)std::floor(f1 / bin);
    k0 = std::max(k0, -N / 2 + 1); k1 = std::min(k1, N / 2 - 1);
    if (k1 < k0) return r;
    // prefix sums over the bins the local means need (circular)
    const int c0 = k0 - W - 1, span = k1 - k0 + 2 * W + 3;
    std::vector<double> cs((size_t)span + 1, 0.0);
    for (int i = 0; i < span; i++) cs[(size_t)i + 1] = cs[(size_t)i] + P[(size_t)(((c0 + i) % N + N) % N)];
    auto sum = [&](int a, int b) { return cs[(size_t)(b - c0 + 1)] - cs[(size_t)(a - c0)]; };   // bins a..b
    double best = 0; int bk = 0;
    for (int k = k0; k <= k1; k++) {
        if (excludeWidth > 0 && std::fabs(k * bin - excludeHz) < excludeWidth) continue;
        const int kk = (k + N) % N;
        const double p3 = P[(size_t)kk] + P[(size_t)((kk + 1) % N)] + P[(size_t)((kk + N - 1) % N)];
        const double around = (sum(k - W, k + W) - sum(k - 2, k + 2)) / (2 * W - 4);
        const double ratio = p3 / 3 / std::max(1e-30, around);
        if (ratio > best) { best = ratio; bk = k; }
    }
    if (best <= 0) return r;
    // parabolic peak on the log spectrum
    const int kk = (bk + N) % N;
    const double a = dB(P[(size_t)((kk + N - 1) % N)]), b = dB(P[(size_t)kk]), c = dB(P[(size_t)((kk + 1) % N)]);
    const double den = a - 2 * b + c, off = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0;
    r.hz = (bk + off) * bin;
    r.db = dB(best);
    r.ok = true;
    return r;
}

// one-sided [fmin, fmax] line search of a real feature (mean removed), with a look at f/2 and f/3 (the lowest strong one is the rate)
Line rateLine(const std::vector<float>& feat, double fs, double fmin, double fmax, std::vector<float>* Pout = nullptr) {
    std::vector<cf32> z(feat.size());
    double m = 0; size_t c = 0;
    for (float v : feat) if (v != 0) { m += v; c++; }   // the mean of the samples that are on (0 = off)
    m /= std::max<size_t>(1, c);
    for (size_t i = 0; i < feat.size(); i++) z[i] = cf32((float)(feat[i] == 0 ? 0 : feat[i] - m), 0);
    const auto P = powerSpec(z.data(), z.size());
    if (Pout) *Pout = P;
    Line l = findLine(P, fs, fmin, fmax);
    if (!l.ok) return l;
    for (int d = 3; d >= 2; d--) {
        const double f = l.hz / d;
        if (f < fmin) continue;
        const Line s = findLine(P, fs, f * 0.985, f * 1.015);
        if (s.ok && s.db > 9 && s.db > l.db - 6) { l = s; break; }
    }
    return l;
}

// 1-D k-means with k = 2 or 4 centres started at the quantiles
struct KM { double c[4] = {0, 0, 0, 0}; int cnt[4] = {0, 0, 0, 0}; double spread = 0; int k = 0; };
KM kmeans(const std::vector<float>& v, int k) {
    KM r; r.k = k;
    if (v.size() < (size_t)(k * 8)) return r;
    for (int j = 0; j < k; j++) r.c[j] = quantile(v, (j + 0.5) / k);
    for (int it = 0; it < 25; it++) {
        double s[4] = {0, 0, 0, 0}; int c[4] = {0, 0, 0, 0};
        for (float x : v) {
            int b = 0;
            for (int j = 1; j < k; j++) if (std::fabs(x - r.c[j]) < std::fabs(x - r.c[b])) b = j;
            s[b] += x; c[b]++;
        }
        for (int j = 0; j < k; j++) if (c[j]) r.c[j] = s[j] / c[j];
        std::sort(r.c, r.c + k);
    }
    double e = 0;
    for (float x : v) {
        int b = 0;
        for (int j = 1; j < k; j++) if (std::fabs(x - r.c[j]) < std::fabs(x - r.c[b])) b = j;
        r.cnt[b]++; e += (x - r.c[b]) * (x - r.c[b]);
    }
    r.spread = std::sqrt(e / v.size());
    return r;
}

// ---------------------------------------------------------------------------------------------------- frame syncs


// soft 4-level symbols (outer = +-3) against a pattern: windows of normalised correlation >= thr; both polarities, the better one counts.
// period > 0: only hits with another hit a multiple of period symbols away (+-1) count.
int countSync(const std::vector<float>& s, const int* p, int L, double thr, int period = 0) {
    double pp = 0;
    for (int j = 0; j < L; j++) pp += (double)p[j] * p[j];
    std::vector<long> hits[2];
    for (size_t i = 0; i + (size_t)L <= s.size(); i++) {
        double c = 0, ss = 0; bool bad = false;
        for (int j = 0; j < L; j++) { const float v = s[i + (size_t)j]; if (std::isnan(v)) { bad = true; break; } c += v * p[j]; ss += (double)v * v; }
        if (bad || ss <= 0) continue;
        const double r = c / std::sqrt(ss * pp);
        for (int pol = 0; pol < 2; pol++) {
            if ((pol ? -r : r) < thr) continue;
            auto& h = hits[pol];
            if (!h.empty() && (long)i - h.back() < L) continue;
            h.push_back((long)i);
        }
    }
    int best = 0;
    for (auto& h : hits) {
        int n = 0;
        if (period <= 0) n = (int)h.size();
        else for (size_t a = 0; a < h.size(); a++) {
            bool ok = false;
            for (size_t b = 0; b < h.size() && !ok; b++) {
                if (a == b) continue;
                const long d = std::labs(h[a] - h[b]);
                const long q = (d + period / 2) / period;
                if (q >= 1 && q <= 8 && std::labs(d - q * period) <= 1) ok = true;
            }
            if (ok) n++;
        }
        best = std::max(best, n);
    }
    return best;
}

// bits (0/1, 255 = gap) against a 32-bit word with up to maxErr errors, either polarity
int countWord(const std::vector<uint8_t>& b, uint64_t word, int L, int maxErr) {
    int best = 0;
    for (int pol = 0; pol < 2; pol++) {
        int n = 0; long last = -100000;
        for (size_t i = 0; i + (size_t)L <= b.size(); i++) {
            int e = 0; bool bad = false;
            for (int j = 0; j < L && e <= maxErr; j++) {
                const uint8_t v = b[i + (size_t)j];
                if (v > 1) { bad = true; break; }
                const int w = (int)((word >> (L - 1 - j)) & 1) ^ pol;
                if (v != w) e++;
            }
            if (bad || e > maxErr) continue;
            if ((long)i - last < L) continue;
            last = (long)i; n++;
        }
        best = std::max(best, n);
    }
    return best;
}

// ---------------------------------------------------------------------------------------------------- the table of modes

// the table of modes is in sig_known.cpp

bool related(int a, int b) {
    auto fam = [](int m) {
        switch (m) {
        case ModFsk2: case ModGmsk: return 1;
        case ModFsk4: return 2;
        case ModBpsk: case ModQpsk: case ModPi4Dqpsk: case ModPsk8: case ModQam: return 3;
        case ModAm: case ModCarrier: return 4;
        case ModPulse: return 5;
        default: return 10 + m;
        }
    };
    return fam(a) == fam(b);
}

// ---------------------------------------------------------------------------------------------------- the steps

// mix the region's centre to 0 and low-pass it to +-cut, every D-th sample (a band-pass FIR on the input: no mixed copy)
std::vector<cf32> filterDecimate(const cf32* x, size_t n, double fs, double fc, double cut, int D, int L) {
    std::vector<cf32> h((size_t)L);
    const int M = L / 2;
    double g = 0;
    std::vector<double> hr((size_t)L);
    for (int k = 0; k < L; k++) {
        const double t = k - M;
        const double s = t == 0 ? 2 * cut / fs : std::sin(2 * kPi * cut / fs * t) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (L - 1)) + 0.08 * std::cos(4 * kPi * k / (L - 1));
        hr[(size_t)k] = s * w; g += hr[(size_t)k];
    }
    // y(i) = sum h_lp[k] x[i-k] e^{-j w0 (i-k)} = e^{-j w0 i} sum (h_lp[k] e^{j w0 k}) x[i-k]
    for (int k = 0; k < L; k++) {
        const double ph = 2 * kPi * fc / fs * k;
        h[(size_t)k] = cf32((float)(hr[(size_t)k] / g * std::cos(ph)), (float)(hr[(size_t)k] / g * std::sin(ph)));
    }
    std::vector<cf32> y;
    if (n < (size_t)L) return y;
    y.reserve((n - (size_t)L) / (size_t)D + 1);
    const double w0 = 2 * kPi * fc / fs;
    // taps reversed, so that the loop runs forward over both and the four partial sums are independent (the compiler vectorises that)
    std::vector<float> hre((size_t)L), him((size_t)L);
    for (int k = 0; k < L; k++) { hre[(size_t)(L - 1 - k)] = h[(size_t)k].real(); him[(size_t)(L - 1 - k)] = h[(size_t)k].imag(); }
    for (size_t i = (size_t)L - 1; i < n; i += (size_t)D) {
        const cf32* xp = x + (i + 1 - (size_t)L);
        float re[4] = {0, 0, 0, 0}, im[4] = {0, 0, 0, 0};
        int k = 0;
        for (; k + 4 <= L; k += 4)
            for (int q = 0; q < 4; q++) {
                const float a = hre[(size_t)(k + q)], b = him[(size_t)(k + q)], xr = xp[k + q].real(), xi = xp[k + q].imag();
                re[q] += a * xr - b * xi;
                im[q] += a * xi + b * xr;
            }
        float sr = re[0] + re[1] + re[2] + re[3], si = im[0] + im[1] + im[2] + im[3];
        for (; k < L; k++) {
            const float a = hre[(size_t)k], b = him[(size_t)k], xr = xp[k].real(), xi = xp[k].imag();
            sr += a * xr - b * xi; si += a * xi + b * xr;
        }
        const double ph = -w0 * (double)i;
        const double c = std::cos(ph), s2 = std::sin(ph);
        y.push_back(cf32((float)(sr * c - si * s2), (float)(sr * s2 + si * c)));
    }
    return y;
}

// Two stages when the decimation is large: a first filter (short in relation to the rate it works at) takes the region to four times its cut-off,
// the second one has the sharp edge at that low rate. About a sixth of the work of one long filter at the input rate. D is the decimation done (D1 * D2).
std::vector<cf32> downconvert(const cf32* x, size_t n, double fs, double fc, double cut, int& D, int L) {
    const int D1 = std::max(1, std::min(D, (int)std::floor(fs / (4 * cut))));
    if (D1 < 24 || L < 300) return filterDecimate(x, n, fs, fc, cut, D, L);
    const int D2 = std::max(1, D / D1);
    const double tr1 = fs / D1 - 2 * cut;   // from the edge of the region to where the first stage folds
    const int L1 = (int)std::min(8191.0, std::max(31.0, std::ceil(5.5 * fs / std::max(1.0, tr1)))) | 1;
    const std::vector<cf32> y1 = filterDecimate(x, n, fs, fc, 0.5 * fs / D1, D1, L1);
    const int L2 = (int)std::min(8191.0, std::max(31.0, std::ceil(13.2 * (fs / D1) / cut))) | 1;   // as the caller sizes a filter: 5.5 / (0.5 half) of the rate it works at, half = cut / 1.2
    D = D1 * D2;
    return filterDecimate(y1.data(), y1.size(), fs / D1, 0, cut, D2, L2);
}

struct Work {
    std::vector<cf32> y;           // the signal at the centre, rate fsd
    double fsd = 0;
    std::vector<uint8_t> on;       // per sample: the signal is on (a block more each side of a burst)
    std::vector<uint8_t> core;     // on, without that margin (envelope statistics)
    std::vector<std::pair<size_t, size_t>> runs;   // on-runs [a, b)
    double noise = 0;              // noise power per sample in y
    std::vector<float> d;          // FM discriminator, Hz (0 where off)
    std::vector<float> ds;         // smoothed
    std::vector<float> P[4];       // power spectra of (x/|x|)^1, ^2, ^4, ^8
};

void gate(Work& w, Features& f, double sigBw) {
    const size_t m = w.y.size();
    const double blockSec = std::max(20e-6, std::min(1e-3, 40.0 / std::max(1.0, sigBw)));
    const size_t B = std::max<size_t>(4, (size_t)(blockSec * w.fsd));
    const size_t nb = m / B;
    w.on.assign(m, 1);
    w.core.assign(m, 1);
    f.duty = 1; f.bursts = 0;
    if (nb < 8) { w.runs = {{0, m}}; return; }
    std::vector<float> p(nb);
    for (size_t b = 0; b < nb; b++) { double s = 0; for (size_t i = b * B; i < (b + 1) * B; i++) s += std::norm(w.y[i]); p[b] = (float)(s / B); }
    // the off level: the quietest tenth of the blocks (or the noise); the on level: a high quantile, so that a few short bursts in a long
    // silence (AIS, ACARS, a pager) still stand out; a continuous signal has both close together
    const double p10 = quantile(p, 0.10), p90 = quantile(p, 0.90);
    const double lo = std::max(p10, w.noise);
    // the on level: the 90th percentile when the signal is on a tenth of the time or more, else the strongest few blocks (a pulse of a few
    // microseconds in a second of silence: ADS-B); the noise alone must not do that: its strongest blocks stand about 1 + 2.2 sqrt(ln(n) / B) above its mean
    double hi = p90;
    if (hi < 4 * lo) hi = quantile(p, 1.0 - std::max(3.0, 0.002 * (double)nb) / (double)nb);
    const double noiseMax = 1.0 + 2.5 * std::sqrt(std::log((double)nb) / (double)B);
    if (hi < 4 * lo || hi < 2 * w.noise || hi < 1.6 * noiseMax * lo) { w.runs = {{0, m}}; return; }   // under 6 dB of swing: continuous
    const double thr = std::sqrt(lo * hi);
    std::vector<uint8_t> ob(nb);
    for (size_t b = 0; b < nb; b++) ob[b] = p[b] > thr;
    // close one-block holes, then count
    for (size_t b = 1; b + 1 < nb; b++) if (!ob[b] && ob[b - 1] && ob[b + 1]) ob[b] = 1;
    size_t onCount = 0;
    for (size_t b = 0; b < nb; b++) onCount += ob[b];
    f.duty = (double)onCount / nb;
    if (f.duty > 0.97) { w.runs = {{0, m}}; f.duty = 1; return; }
    std::vector<double> lens;
    w.runs.clear();
    for (size_t b = 0; b < nb;) {
        if (!ob[b]) { b++; continue; }
        size_t e = b;
        while (e < nb && ob[e]) e++;
        w.runs.push_back({b > 0 ? (b - 1) * B : 0, std::min(m, (e + 1) * B)});   // a block more each side: the ramps and a preamble
        lens.push_back((double)(e - b) * B / w.fsd);
        b = e;
    }
    for (size_t i = 0; i < m; i++) w.core[i] = (i / B < nb) ? ob[i / B] : 0;
    std::fill(w.on.begin(), w.on.end(), 0);
    for (const auto& r : w.runs) std::fill(w.on.begin() + (long)r.first, w.on.begin() + (long)r.second, 1);
    // The blocks are coarse (a burst a block long straddles two): move each edge to where the signal crosses the threshold, at a quarter of a block
    {
        const size_t W = std::max<size_t>(2, B / 4);
        std::vector<double> pre(m + 1, 0.0);
        for (size_t i = 0; i < m; i++) pre[i + 1] = pre[i] + std::norm(w.y[i]);
        auto smooth = [&](size_t i) { const size_t a0 = i >= W / 2 ? i - W / 2 : 0, a1 = std::min(m, a0 + W); return (pre[a1] - pre[a0]) / (double)(a1 - a0); };
        lens.clear();
        std::fill(w.core.begin(), w.core.end(), 0);
        for (auto& r : w.runs) {
            const size_t cb = r.first + B < r.second ? r.first + B : r.first;   // inside the run (it was widened by a block each side)
            const size_t ce = r.second > B + cb ? r.second - B : r.second;
            size_t a0 = cb, b0 = ce;
            while (a0 > r.first && smooth(a0 - 1) > thr) a0--;
            while (b0 < r.second && smooth(b0) > thr) b0++;
            while (a0 < b0 && smooth(a0) <= thr) a0++;
            while (b0 > a0 && smooth(b0 - 1) <= thr) b0--;
            lens.push_back((double)(b0 - a0) / w.fsd);
            for (size_t i = a0; i < b0; i++) w.core[i] = 1;
        }
        size_t on2 = 0;
        for (size_t i = 0; i < m; i++) on2 += w.core[i];
        f.duty = (double)on2 / (double)m;
    }
    f.bursts = (int)w.runs.size();
    f.burstMs = median(lens) * 1e3;
    if (w.runs.size() >= 4) {   // the spacing of the burst starts: the frame (or the slot) period of a time-division system
        std::vector<double> gaps;
        for (size_t i = 1; i < w.runs.size(); i++) gaps.push_back((double)(w.runs[i].first - w.runs[i - 1].first) / w.fsd);
        const double md = median(gaps);
        size_t nearBy = 0;
        for (double g : gaps) nearBy += std::fabs(g / md - 1) < 0.1 || std::fabs(g / md - 2) < 0.1 || std::fabs(g / md - 3) < 0.1;
        if (nearBy * 10 >= gaps.size() * 6) f.burstPeriodMs = md * 1e3;   // regular: most of the gaps are one, two or three periods
    }
}

// u^k (phase only) lines near the centre
void powerLines(Work& w, Features& f) {
    const size_t m = std::min<size_t>(w.y.size(), (size_t)1 << 20);
    std::vector<cf32> z(m);
    const int pw[4] = {1, 2, 4, 8};
    for (int j = 0; j < 4; j++) {
        for (size_t i = 0; i < m; i++) {
            const cf32 v = w.y[i];
            const float a = std::abs(v);
            if (!w.core[i] || a < 1e-12f) { z[i] = 0; continue; }
            cf32 u = v / a, r = u;
            for (int q = 1; q < pw[j]; q++) r *= u;
            z[i] = r;
        }
        w.P[j] = powerSpec(z.data(), m);
        const Line l = findLine(w.P[j], w.fsd, -w.fsd * 0.48, w.fsd * 0.48);
        f.lineDb[j] = l.ok ? l.db : 0; f.lineHz[j] = l.hz;
    }
}

// OFDM: the cyclic prefix shows as a peak of the autocorrelation at the useful symbol length; the symbol period as a line of y(t) y*(t + Tu)
// The correlation of the signal with itself delayed by the lags tmin .. (as magnitude, normalised), kept by ofdmTest for the checks of a
// mode's useful symbol length (corrAt).
struct OfdmCorr {
    std::vector<float> mag;
    size_t tmin = 0;
    double med = 0, fsd = 1;
    // the strength of the correlation at the useful symbol length tuSec: the highest value within +-1.5 % of the lag over the median (0 when out of range)
    double at(double tuSec) const {
        if (mag.empty()) return 0;
        const double lag = tuSec * fsd, w = std::max(2.0, 0.015 * lag);
        const long lo = (long)std::floor(lag - w) - (long)tmin, hi = (long)std::ceil(lag + w) - (long)tmin;
        if (hi < 0 || lo >= (long)mag.size()) return 0;
        float best = 0;
        for (long i = std::max<long>(0, lo); i <= std::min<long>((long)mag.size() - 1, hi); i++) best = std::max(best, mag[(size_t)i]);
        return best / std::max(1e-6, med);
    }
};

bool ofdmTest(const Work& w, Features& f, OfdmCorr* oc = nullptr) {
    const size_t m = std::min<size_t>(w.y.size(), (size_t)1 << 18);
    if (m < 4096) return false;
    const size_t N = pow2At(2 * m);
    std::vector<cf32> b(N, cf32(0, 0));
    for (size_t i = 0; i < m; i++) b[i] = w.on[i] ? w.y[i] : cf32(0, 0);
    Fft F((int)N);
    F.forward(b.data());
    for (auto& v : b) v = cf32(std::norm(v), 0);
    F.inverse(b.data());
    const double r0 = std::abs(b[0]);
    if (r0 <= 0) return false;
    const size_t tmin = std::max<size_t>(8, (size_t)(40 * w.fsd / std::max(1.0, f.obwHz)));
    const size_t tmax = std::min<size_t>(m / 3, (size_t)(0.03 * w.fsd));
    if (tmax <= tmin + 16) return false;
    std::vector<float> mag;
    for (size_t t = tmin; t <= tmax; t++) mag.push_back((float)(std::abs(b[t]) / r0 * (double)m / (double)(m - t)));   // fewer overlapping samples at larger lags
    // The cyclic prefix gives one narrow peak at the useful symbol length; the pilots and the symbol structure give others, at fractions and
    // multiples of it. The strongest one is reported, and each mode that is a candidate checks its own symbol lengths against the correlation (OfdmCorr::at).
    size_t bt = tmin; double bv = 0;
    for (size_t i = 0; i < mag.size(); i++) if (mag[i] > bv) { bv = mag[i]; bt = tmin + i; }
    const double med = median(mag);
    if (oc) { oc->mag = mag; oc->tmin = tmin; oc->med = med; oc->fsd = w.fsd; }
    const double sbv = bv;
    if (sbv < 0.03 || sbv < 8 * med) return false;
    if (f.envKurt < 1.8 && sbv < 0.15) return false;   // a single carrier with a pulse shape (cable QAM): no cyclic prefix, its kurtosis is not Gaussian
    // the symbol period: a line of p(t) = y(t) y*(t + Tu) between 1 / (1.85 Tu) and 1 / (1.01 Tu)
    const size_t mp = std::min<size_t>(w.y.size() - bt, (size_t)1 << 20);
    std::vector<cf32> p(mp);
    for (size_t i = 0; i < mp; i++) p[i] = (w.on[i] && w.on[i + bt]) ? w.y[i] * std::conj(w.y[i + bt]) : cf32(0, 0);
    const auto P = powerSpec(p.data(), mp);
    const double tu = bt / w.fsd;
    const Line l = findLine(P, w.fsd, 1 / (1.85 * tu), 1 / (1.01 * tu));
    f.ofdmTuSec = tu;
    f.ofdmCorr = sbv;
    if (l.ok && l.db > 8) { const double ts = 1 / l.hz; f.ofdmTgSec = std::max(0.0, ts - tu); f.symRate = l.hz; f.symLineDb = l.db; f.symSrc = 5; }
    return true;
}

// LoRa: y(t + T/4) y*(t) of a chirp of width B and length T = 2^SF / B is a tone at B/4 (3/4 of the time); at half the lag, B/8
bool chirpTest(const Work& w, Features& f) {
    double B = f.obwHz > 0 ? f.obwHz : f.bw20Hz;
    static const double kBws[] = {7.8e3, 10.4e3, 15.6e3, 20.8e3, 31.25e3, 41.7e3, 62.5e3, 125e3, 250e3, 500e3, 812.5e3, 1625e3};
    for (double b : kBws) if (std::fabs(B / b - 1) < 0.15) { B = b; break; }
    if (B < 20e3) return false;   // narrower than 20 kHz: narrow-band FSK looks like a chirp at this resolution, and nobody uses LoRa that narrow on air
    // The tone's phase changes from one data symbol to the next (by a quarter turn per symbol value), so the product's spectrum is averaged
    // over stretches of about a symbol (Welch, Hann windows, half overlap): the tone then shows whatever the data.
    auto tone = [&](size_t L, double& hz) {
        const size_t m = std::min<size_t>(w.y.size() > L ? w.y.size() - L : 0, (size_t)1 << 20);
        const size_t S = std::max<size_t>(256, std::min<size_t>(pow2At(4 * L), 2048));   // a stretch may be shorter than a symbol: the tone lasts
        if (m < std::max<size_t>(1024, 2 * S)) return 0.0;
        std::vector<float> P(S, 0.f), win(S);
        for (size_t i = 0; i < S; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / S));
        Fft Fs((int)S);
        std::vector<cf32> z(S);
        size_t segs = 0;
        const size_t hop = std::max<size_t>(S / 2, (m - S) / 96 + 1);   // at most about a hundred stretches, spread over the capture
        for (size_t o0 = 0; o0 + S <= m; o0 += hop) {
            size_t on = 0;
            for (size_t i = 0; i < S; i++) { const bool ok = w.on[o0 + i] && w.on[o0 + i + L]; on += ok; z[i] = ok ? w.y[o0 + i + L] * std::conj(w.y[o0 + i]) * win[i] : cf32(0, 0); }
            if (on * 2 < S) continue;
            Fs.forward(z.data());
            for (size_t i = 0; i < S; i++) P[i] += std::norm(z[i]);
            segs++;
        }
        if (segs < 2) return 0.0;
        double mp = 0;
        for (float v : P) mp += v;
        const float flo = (float)(1e-5 * mp / (double)S);   // a floor: a clean chirp leaves bins with nothing in them, which no line should be measured against
        for (float& v : P) v += flo;
        const Line l = findLine(P, w.fsd, -w.fsd * 0.48, w.fsd * 0.48);
        hz = l.hz;
        return l.ok ? l.db : 0.0;
    };
    int bestSf = 0; double bestDb = 0; bool down = false;
    for (int sf = 6; sf <= 12; sf++) {
        const double T = std::ldexp(1.0, sf) / B;
        const size_t L = (size_t)std::lround(T * w.fsd / 4);
        if (L < 4 || L * 4 > w.y.size()) continue;
        double hz = 0;
        const double db = tone(L, hz);
        if (db < 12 || std::fabs(std::fabs(hz) - B / 4) > 0.08 * B) continue;
        double hz2 = 0;
        const double db2 = tone(L / 2, hz2);
        if (db2 < 8 || std::fabs(std::fabs(hz2) - B / 8) > 0.06 * B || (hz2 > 0) != (hz > 0)) continue;
        if (db > bestDb) { bestDb = db; bestSf = sf; down = hz < 0; }
    }
    if (!bestSf) return false;
    f.chirpBwHz = B; f.loraSf = bestSf;
    f.symRate = B / std::ldexp(1.0, bestSf); f.symLineDb = bestDb; f.symSrc = 4;
    (void)down;
    return true;
}

// symbols of the smoothed discriminator at rate rs: blocks of 128 symbols, each at the phase of the most energy; NaN between bursts
std::vector<float> sampleSymbols(const Work& w, double rs, double mid) {
    std::vector<float> out;
    const double sps = w.fsd / rs;
    if (sps < 2) return out;
    auto at = [&](double t) {
        const size_t i = (size_t)t;
        if (i + 1 >= w.ds.size()) return w.ds.back();
        const double fr = t - i;
        return (float)(w.ds[i] * (1 - fr) + w.ds[i + 1] * fr);
    };
    for (const auto& r : w.runs) {
        const double a = (double)r.first + sps, b = (double)r.second - sps - 1;
        if (b - a < 16 * sps) continue;
        for (double t0 = a; t0 + sps < b;) {
            const int ns = (int)std::min(128.0, std::floor((b - t0) / sps));
            if (ns < 8) break;
            int bp = 0; double be = -1;
            for (int ph = 0; ph < 16; ph++) {
                double e = 0;
                for (int i = 0; i < ns; i++) { const double v = at(t0 + (ph / 16.0) * sps + i * sps) - mid; e += v * v; }
                if (e > be) { be = e; bp = ph; }
            }
            for (int i = 0; i < ns; i++) out.push_back(at(t0 + (bp / 16.0) * sps + i * sps));
            t0 += ns * sps;
        }
        out.push_back(NAN);
    }
    return out;
}

// FSK: levels at the symbol instants
bool fskTest(const Work& w, Features& f, double rs, std::vector<float>& syms, double& mid, double& unit) {
    std::vector<float> on;
    for (size_t i = 0; i < w.ds.size(); i++) if (w.on[i]) on.push_back(w.ds[i]);
    if (on.size() < 256) return false;
    const double m0 = median(on);
    syms = sampleSymbols(w, rs, m0);
    std::vector<float> v;
    for (float s : syms) if (!std::isnan(s)) v.push_back(s);
    if (v.size() < 64) return false;
    const KM k4 = kmeans(v, 4), k2 = kmeans(v, 2);
    const double s1 = k4.c[1] - k4.c[0], s2 = k4.c[2] - k4.c[1], s3 = k4.c[3] - k4.c[2];
    const double span = k4.c[3] - k4.c[0];
    const int minCnt = std::min(std::min(k4.cnt[0], k4.cnt[1]), std::min(k4.cnt[2], k4.cnt[3]));
    const bool four = k4.k == 4 && std::min(s1, std::min(s2, s3)) > 2.2 * k4.spread && minCnt > (int)(0.06 * v.size()) &&
                      span / std::max(1e-9, s2) > 2.0 && span / std::max(1e-9, s2) < 4.6;
    if (four) {
        mid = (k4.c[0] + k4.c[3]) / 2; unit = span / 6;
        f.fskLevels = 4;
        for (int j = 0; j < 4; j++) f.devHz[j] = k4.c[j] - mid;
        f.devSpread = k4.spread;
        return true;
    }
    const double sep = k2.c[1] - k2.c[0];
    const int min2 = std::min(k2.cnt[0], k2.cnt[1]);
    if (k2.k == 2 && sep > 3.0 * k2.spread && min2 > (int)(0.1 * v.size())) {
        mid = (k2.c[0] + k2.c[1]) / 2; unit = sep / 2;
        f.fskLevels = 2;
        f.devHz[0] = k2.c[0] - mid; f.devHz[1] = k2.c[1] - mid;
        f.devSpread = k2.spread;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------- spectral shape, tones, comb

// A Welch spectrum of y (the signal at the centre, Hann windows, the stretches where it is on) at about 1/4000 of its width over 1.2 times
// the width: the tones that stand out of it (local maxima 6 dB over the band's median with 4 dB of prominence), their common spacing, and the
// strongest periodicity of the detrended spectrum (its autocorrelation: an OFDM pilot comb, an MFSK grid). Then the shape in a word.
void spectralShape(const Work& w, Features& f, double sigW) {
    const size_t m = w.y.size();
    size_t Ns = pow2At((size_t)std::ceil(w.fsd / std::max(1e-3, sigW / 4096)));
    Ns = std::max<size_t>(256, std::min<size_t>(Ns, 16384));
    while (Ns > 256 && Ns * 4 > m) Ns >>= 1;
    if (Ns * 2 > m) return;
    std::vector<float> win(Ns);
    for (size_t i = 0; i < Ns; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / Ns));
    const size_t segs = std::min<size_t>(32, (m - Ns) / (Ns / 2) + 1);
    const double stride = segs > 1 ? (double)(m - Ns) / (double)(segs - 1) : 0;
    std::vector<double> acc(Ns, 0.0);
    Fft F((int)Ns);
    std::vector<cf32> b(Ns);
    size_t used = 0;
    for (size_t s = 0; s < segs; s++) {
        const size_t o0 = (size_t)(s * stride);
        if (!w.core[o0 + Ns / 2]) continue;   // off between bursts
        for (size_t i = 0; i < Ns; i++) b[i] = w.y[o0 + i] * win[i];
        F.forward(b.data());
        for (size_t i = 0; i < Ns; i++) acc[i] += std::norm(b[i]);
        used++;
    }
    if (used < 2) return;
    const double bin = w.fsd / (double)Ns;
    const int half = (int)std::min<double>((double)Ns / 2 - 1, std::ceil(0.6 * sigW / bin));
    if (half < 16) return;
    const int len = 2 * half + 1;
    std::vector<double> S((size_t)len), L((size_t)len);
    for (int j = -half; j <= half; j++) {
        const double p = acc[(size_t)((j + (long)Ns) % (long)Ns)] / (double)used;
        L[(size_t)(j + half)] = p; S[(size_t)(j + half)] = dB(p);
    }
    const int inner = (int)std::min<double>(half, std::ceil(0.5 * sigW / bin));
    const double med = median(std::vector<double>(S.begin() + (half - inner), S.begin() + (half + inner + 1)));
    // tones
    const int pw = std::max(3, len / 25);
    std::vector<double> pk;
    for (int i = 2; i + 2 < len; i++) {
        if (S[(size_t)i] < med + 6) continue;
        bool mx = true;
        for (int d = -2; d <= 2 && mx; d++) if (S[(size_t)(i + d)] > S[(size_t)i] || (d < 0 && S[(size_t)(i + d)] == S[(size_t)i])) mx = false;   // a plateau counts once
        if (!mx) continue;
        double lmin = S[(size_t)i], rmin = S[(size_t)i];
        for (int d = 1; d <= pw; d++) { if (i - d >= 0) lmin = std::min(lmin, S[(size_t)(i - d)]); if (i + d < len) rmin = std::min(rmin, S[(size_t)(i + d)]); }
        if (S[(size_t)i] - std::max(lmin, rmin) >= 4) pk.push_back((i - half) * bin);
    }
    f.tones = (int)pk.size();
    if (pk.size() >= 2) {
        std::vector<double> dif;
        for (size_t i = 1; i < pk.size(); i++) dif.push_back(pk[i] - pk[i - 1]);
        const double g0 = *std::min_element(dif.begin(), dif.end());
        // the spacing: the smallest gap, when most gaps are whole multiples of it (an MFSK tone set with tones that were not sent)
        double num = 0, den = 0; size_t fit = 0;
        for (double d : dif) { const double k = std::round(d / g0); if (k >= 1 && std::fabs(d / (k * g0) - 1) < 0.12) { fit++; num += d; den += k; } }
        if (fit * 10 >= dif.size() * 7 && den > 0) f.toneSpacingHz = num / den;
    }
    // comb: the autocorrelation of the spectrum over its inner part, the slow shape (a moving average) taken out
    {
        const int a0 = half - inner, n0 = 2 * inner + 1;
        const int tw = std::max(8, n0 / 8);
        std::vector<double> pre((size_t)n0 + 1, 0.0), r((size_t)n0);
        for (int i = 0; i < n0; i++) pre[(size_t)i + 1] = pre[(size_t)i] + L[(size_t)(a0 + i)];
        for (int i = 0; i < n0; i++) {
            const int lo = std::max(0, i - tw), hi = std::min(n0 - 1, i + tw);
            const double t = (pre[(size_t)hi + 1] - pre[(size_t)lo]) / (hi - lo + 1);
            r[(size_t)i] = t > 0 ? L[(size_t)(a0 + i)] / t - 1 : 0;
        }
        double r0 = 0;
        for (double v : r) r0 += v * v;
        const int maxLag = std::min(n0 / 4, tw);   // a period longer than the trend window would be taken out with the trend
        double best = 0; int bl = 0;
        std::vector<double> ac((size_t)maxLag + 2, 0.0);
        for (int l = 1; l <= maxLag + 1; l++) { double s = 0; for (int i = 0; i + l < n0; i++) s += r[(size_t)i] * r[(size_t)(i + l)]; ac[(size_t)l] = r0 > 0 ? s / r0 : 0; }
        for (int l = 2; l <= maxLag; l++) if (ac[(size_t)l] > best && ac[(size_t)l] >= ac[(size_t)(l - 1)] && ac[(size_t)l] >= ac[(size_t)(l + 1)]) { best = ac[(size_t)l]; bl = l; }
        if (best > 0.2 && bl > 0) { f.combHz = bl * bin; f.combStrength = best; }
    }
    if (f.flatTop) f.shape = ShapeFlat;
    else if (sigW < 400 && f.carrierDb > 15) f.shape = ShapeLine;
    else if (f.tones >= 2 && f.toneSpacingHz > 0) f.shape = ShapeTones;
    else f.shape = ShapeRounded;
}

// MFSK (constant envelope, narrow): the instantaneous frequency, averaged over half a symbol, sits on the tones, so its histogram is a comb at
// the tone spacing whatever tones the data picks; the autocorrelation of the detrended histogram finds that spacing. Tried for half symbols of
// 200, 60, 20 and 6 Bd (the rate is not known yet), the clearest comb wins. Only spacings up to 40 % of the width (the levels of 2FSK and 4FSK
// are fskTest's).
void mfskTones(const Work& w, Features& f, double sigW) {
    const size_t m = w.d.size();
    if (m < 2048 || sigW <= 0) return;
    const double binHz = sigW / 400;
    const int nb = 600, half = nb / 2;
    std::vector<double> pre(m + 1, 0.0);
    std::vector<uint32_t> onPre(m + 1, 0);
    for (size_t i = 0; i < m; i++) { pre[i + 1] = pre[i] + (w.on[i] ? w.d[i] : 0.f); onPre[i + 1] = onPre[i] + (w.on[i] ? 1u : 0u); }
    double mean = 0; size_t cnt = 0;
    for (size_t i = 0; i < m; i++) if (w.on[i]) { mean += w.d[i]; cnt++; }
    if (cnt < 1024) return;
    mean /= (double)cnt;
    double bestS = 0, bestSp = 0;
    for (double rs : {200.0, 60.0, 20.0, 6.0}) {
        const size_t K = (size_t)std::max(1.0, std::round(0.5 * w.fsd / rs));
        if (K * 40 > m) continue;
        std::vector<double> h((size_t)nb, 0.0);
        const size_t step = std::max<size_t>(1, K / 4);
        for (size_t i = 0; i + K <= m; i += step) {
            if (onPre[i + K] - onPre[i] != K) continue;
            const double v = (pre[i + K] - pre[i]) / (double)K - mean;
            const long b = (long)std::floor(v / binHz) + half;
            if (b >= 0 && b < nb) h[(size_t)b] += 1;
        }
        // smooth by a bin each side, detrend with a window of a quarter of the width, the autocorrelation
        std::vector<double> s((size_t)nb, 0.0);
        for (int i = 1; i + 1 < nb; i++) s[(size_t)i] = (h[(size_t)i - 1] + 2 * h[(size_t)i] + h[(size_t)i + 1]) / 4;
        const int tw = 100;
        std::vector<double> sp((size_t)nb + 1, 0.0), r((size_t)nb, 0.0);
        for (int i = 0; i < nb; i++) sp[(size_t)i + 1] = sp[(size_t)i] + s[(size_t)i];
        double tot = sp[(size_t)nb];
        if (tot < 200) continue;
        for (int i = 0; i < nb; i++) {
            const int lo = std::max(0, i - tw), hi = std::min(nb - 1, i + tw);
            const double t = (sp[(size_t)hi + 1] - sp[(size_t)lo]) / (hi - lo + 1);
            r[(size_t)i] = t > tot / nb * 0.05 ? s[(size_t)i] - t : 0;
        }
        double r0 = 0;
        for (double v : r) r0 += v * v;
        if (r0 <= 0) continue;
        const int maxLag = 160;
        std::vector<double> ac((size_t)maxLag + 2, 0.0);
        for (int l = 1; l <= maxLag + 1; l++) { double a = 0; for (int i = 0; i + l < nb; i++) a += r[(size_t)i] * r[(size_t)(i + l)]; ac[(size_t)l] = a / r0; }
        auto peak = [&](int l) { return ac[(size_t)l] >= ac[(size_t)l - 1] && ac[(size_t)l] >= ac[(size_t)l + 1]; };
        double mx = 0;
        for (int l = 4; l <= maxLag; l++) if (peak(l)) mx = std::max(mx, ac[(size_t)l]);
        for (int l = 4; l <= maxLag; l++)   // the first peak about as high as the highest: the spacing, not a multiple of it
            if (peak(l) && ac[(size_t)l] >= 0.8 * mx) { if (ac[(size_t)l] > bestS) { bestS = ac[(size_t)l]; bestSp = l * binHz; } break; }
    }
    if (bestS > 0.35 && (f.toneSpacingHz <= 0 || f.tones < 4)) { f.toneSpacingHz = bestSp; f.tones = std::max(f.tones, (int)std::lround(sigW / bestSp)); }
}

// root raised cosine, t in symbols
double rrc(double t, double beta) {
    const double eps = 1e-6;
    if (std::fabs(t) < eps) return 1 - beta + 4 * beta / kPi;
    if (beta > 0 && std::fabs(std::fabs(t) - 1 / (4 * beta)) < eps)
        return beta / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * beta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * beta)));
    const double a = kPi * t, d = a * (1 - 16 * beta * beta * t * t);
    return (std::sin(a * (1 - beta)) + 4 * beta * t * std::cos(a * (1 + beta))) / d;
}

// Linear modulation with a symbol rate: the symbols of the longest stretch the signal is on, after a matched filter (root raised cosine,
// roll-off 0.35, 6 symbols) at the best of 8 timing phases, the carrier's own offset taken out. How many amplitude levels (k-means), how well
// u^2, u^4, u^8 line up (u = s / |s|; a clean M-PSK gives 1 at M and above, whatever a small frequency error), and the order they suggest.
void constellation(const Work& w, Features& f, double offHz) {
    const double rs = f.symRate, sps = rs > 0 ? w.fsd / rs : 0;
    if (sps < 2 || sps > 96) return;
    size_t a = 0, e = 0;
    for (const auto& r : w.runs) if (r.second - r.first > e - a) { a = r.first; e = r.second; }
    const int nt = (int)std::ceil(6 * sps) | 1, h2 = nt / 2;
    if (e < a + (size_t)(64 * sps) + (size_t)nt) return;
    const size_t nsym = std::min<size_t>(4096, (size_t)((double)(e - a - (size_t)nt) / sps) - 2);
    std::vector<float> h((size_t)nt);
    for (int i = 0; i < nt; i++) h[(size_t)i] = (float)rrc((i - h2) / sps, 0.35);
    const double dph = -2 * kPi * offHz / w.fsd;
    auto mf = [&](double t) {   // the matched filter's output at sample time t (nearest sample), derotated
        const size_t c = (size_t)std::lround(t);
        cf32 s(0, 0);
        for (int i = 0; i < nt; i++) s += h[(size_t)i] * w.y[c + (size_t)i - (size_t)h2];
        const double ph = dph * (double)c;
        return s * cf32((float)std::cos(ph), (float)std::sin(ph));
    };
    const double t0 = (double)a + h2 + 1;
    int bp = 0; double be = -1;
    for (int p = 0; p < 8; p++) {
        double en = 0;
        for (size_t n = 0; n < nsym; n += 4) en += std::norm(mf(t0 + p * sps / 8 + (double)n * sps));
        if (en > be) { be = en; bp = p; }
    }
    std::vector<cf32> s(nsym);
    double m2 = 0, m4 = 0;
    for (size_t n = 0; n < nsym; n++) { s[n] = mf(t0 + bp * sps / 8 + (double)n * sps); const double q = std::norm(s[n]); m2 += q; m4 += q * q; }
    m2 /= (double)nsym; m4 /= (double)nsym;
    if (m2 <= 0) return;
    // the noise in the symbols: the filter's gain on the noise of y
    double hh = 0;
    for (float v : h) hh += (double)v * v;
    const double nz = w.noise * hh, s2 = std::max(0.05 * m2, m2 - nz), s4 = std::max(0.5 * s2 * s2, m4 - 4 * s2 * nz - 2 * nz * nz);
    const double kurt = s4 / (s2 * s2);   // 1 PSK, 1.32 16-QAM, 1.38 64-QAM
    const size_t Nf = pow2At(nsym * 4);
    Fft F((int)Nf);
    std::vector<cf32> z(Nf);
    const int Ms[3] = {2, 4, 8};
    for (int j = 0; j < 3; j++) {
        std::fill(z.begin(), z.end(), cf32(0, 0));
        for (size_t n = 0; n < nsym; n++) {
            const float am = std::abs(s[n]);
            if (am <= 0) continue;
            cf32 u = s[n] / am, r = u;
            for (int q = 1; q < Ms[j]; q++) r *= u;
            z[n] = r;
        }
        F.forward(z.data());
        double mx = 0;
        for (const cf32& v : z) mx = std::max(mx, (double)std::abs(v));
        f.phaseCoh[j] = mx / (double)nsym;
    }
    std::vector<float> amp(nsym);
    for (size_t n = 0; n < nsym; n++) amp[n] = (float)(std::abs(s[n]) / std::sqrt(m2));
    const KM k3 = kmeans(amp, 3), k2 = kmeans(amp, 2);
    auto apart = [&](const KM& k) {
        for (int j = 1; j < k.k; j++) if (k.c[j] - k.c[j - 1] < 2.5 * k.spread || k.cnt[j] < (int)(0.08 * nsym) || k.cnt[j - 1] < (int)(0.08 * nsym)) return false;
        return true;
    };
    f.ampRings = apart(k3) ? 3 : apart(k2) ? 2 : 1;
    if (kurt > 1.18 || f.ampRings >= 3) f.constOrder = kurt < 1.35 ? 16 : 64;
    else if (f.phaseCoh[0] > 0.5) f.constOrder = 2;
    else if (f.phaseCoh[1] > 0.45) f.constOrder = 4;
    else if (f.phaseCoh[2] > 0.4) f.constOrder = 8;
}

std::string hzText(double hz) {
    char b[48];
    const double a = std::fabs(hz);
    if (a >= 1e6) snprintf(b, sizeof b, "%.3g MHz", hz / 1e6);
    else if (a >= 1e3) snprintf(b, sizeof b, "%.3g kHz", hz / 1e3);
    else snprintf(b, sizeof b, "%.0f Hz", hz);
    return b;
}
std::string rateText(double rs) {
    char b[48];
    if (rs >= 1e6) snprintf(b, sizeof b, "%.4g MBd", rs / 1e6);
    else if (rs >= 1e4) snprintf(b, sizeof b, "%.4g kBd", rs / 1e3);
    else snprintf(b, sizeof b, "%.0f Bd", rs);
    return b;
}

} // namespace

const char* modName(int m) {
    static const char* const k[ModCount] = {"unknown", "noise", "carrier", "AM", "FM", "2FSK", "4FSK", "GMSK / MSK", "BPSK", "QPSK",
                                            "pi/4-DQPSK", "8PSK", "QAM", "OFDM", "chirp (LoRa)", "pulses (OOK / PPM)"};
    return m >= 0 && m < ModCount ? k[m] : "?";
}

const char* shapeName(int s) {
    static const char* const k[ShapeCount] = {"not measured", "flat top", "rounded (filtered)", "discrete tones", "a line"};
    return s >= 0 && s < ShapeCount ? k[s] : "?";
}

// ---------------------------------------------------------------------------------------------------- confidence and priors

// What the evidence is worth. The raw score of a candidate (0 .. 100) is turned into the probability that this candidate is the right one by a
// logistic regression fitted on the app's own test signals (tests/specx/test_specx_idmatrix.cpp writes every candidate with its score when the
// environment variable CSV names a file; the fit is a plain logistic regression of "is the truth" on the features below, over all modes, SNRs
// and frequency settings, checked by leaving whole modes out):
//   z = b0 + b1 s/100 - b2 (s1 - s)/100 + b3 (s1 - s2)/100 [best candidate only]      p = 1 / (1 + e^-z)
// s the candidate's score, s1 the best score, s2 the runner-up's. A high score with a close runner-up is not a confident answer, and a middling
// score with nothing near it is not either. The reliability table of the test shows whether the numbers are honest (they are, to about 6 points).
constexpr double kB0 = -3.42, kB1 = 3.23, kB2 = 5.09, kB3 = 7.64;
constexpr double kUnknownBelow = 0.30;   // the best candidate is not credible enough: "I do not know" (of the answers given at that level and above, two thirds were right on the test signals)
constexpr double kLikelyFrom = 0.60;     // and a runner-up under half as likely

static void calibrate(Result& R) {
    R.verdict = VerdictUnknown;
    if (R.cands.empty()) return;
    const double s1 = R.cands[0].score, s2 = R.cands.size() > 1 ? R.cands[1].score : 0.0;
    double sum = 0;
    for (size_t i = 0; i < R.cands.size(); i++) {
        Candidate& c = R.cands[i];
        const double z = kB0 + kB1 * c.score / 100 - kB2 * (s1 - c.score) / 100 + (i == 0 ? kB3 * (s1 - s2) / 100 : 0.0);
        c.confidence = 1.0 / (1.0 + std::exp(-z));
        if (c.source == 2) c.confidence = std::min(c.confidence, 0.2);   // known by its frequency only: never more than a hint
        sum += c.confidence;
    }
    if (sum > 1) for (Candidate& c : R.cands) c.confidence /= sum;   // events that exclude each other cannot add up to more than one
    const Candidate& t = R.cands[0];
    const double p2 = R.cands.size() > 1 ? R.cands[1].confidence : 0.0;
    // modes that look the same (LoRa and the networks on it, NXDN48 and dPMR without a sync) tie: each is unlikely, the set of them is not,
    // and the answer is "one of these", not "I do not know"
    double tied = 0;
    for (const Candidate& c : R.cands) if (c.score >= t.score - 3 && c.source != 2) tied += c.confidence;
    if (t.confidence < kUnknownBelow && tied >= 0.55 && t.score >= 50 && R.cands.size() > 1 && R.cands[1].score >= t.score - 3) { R.verdict = VerdictAmbiguous; return; }
    if (t.confidence < kUnknownBelow) return;
    R.verdict = t.confidence >= kLikelyFrom && p2 < 0.5 * t.confidence ? VerdictLikely : VerdictAmbiguous;
}

static void applyTvPlanPrior(Result& R, const Features& f);
static void applyPriors(Result& R, const Features& f) {
    const double hz = f.rfHz, tol = std::max(6.25e3, 0.5 * f.obwHz);
    std::vector<const FreqEntry*> nearBy;
    for (const FreqEntry& e : builtinFrequencies()) {
        const double d = std::fabs((double)e.hz - hz);
        if (d <= tol || (e.bwHz > 0 && d <= 0.5 * e.bwHz)) nearBy.push_back(&e);
        else if (e.hz > hz + 2e6) continue;
    }
    for (const FreqEntry* e : nearBy) R.priors.push_back(e->name + " (" + freqModeName(e->mode) + ", " + e->category + ")");
    if (R.priors.size() > 8) R.priors.resize(8);
    for (Candidate& c : R.cands) {
        bool exact = false, family = false;
        for (const FreqEntry* e : nearBy) {
            if (e->mode == c.subId || (c.subId.empty() && e->mode == c.modeId) || (e->mode == "dvb" && c.modeId == "dvbt")) exact = true;
            else if (!c.modeId.empty() && e->mode == c.modeId) family = true;
        }
        if (exact) { c.score = std::min(100.0, c.score + 10); c.evidence.push_back("the frequency database lists this mode here (+10)"); }
        else if (family) { c.score = std::min(100.0, c.score + 4); c.evidence.push_back("the frequency database lists the same receiver here (+4)"); }
    }
    // the band plans: the bands that contain the frequency name the modes they suggest ("DVB-T2", "Airband", "FM" ...)
    auto norm = [](const std::string& t) { std::string o; for (char ch : t) if (std::isalnum((unsigned char)ch)) o += (char)std::tolower((unsigned char)ch); return o; };
    std::set<std::string> bandModes;
    std::string bandName;
    for (const bandplan::Plan& pl : bandplan::builtinPlans())
        for (const bandplan::Band& b : pl.bands)
            if (b.contains(hz)) {
                if (bandName.empty()) bandName = b.name + " (" + b.service + ")";
                for (const std::string& m : b.modes) bandModes.insert(norm(m));
            }
    if (!bandName.empty()) {
        R.priors.push_back("band plan: " + bandName);
        for (Candidate& c : R.cands) {
            const std::string want[] = {norm(c.name), norm(c.subId.empty() ? c.modeId : c.subId), norm(freqModeName(c.subId.empty() ? c.modeId : c.subId))};
            bool hit = false;
            for (const std::string& w : want) if (!w.empty() && bandModes.count(w)) hit = true;
            if (hit) { c.score = std::min(100.0, c.score + 4); c.evidence.push_back("the band plan suggests this mode in " + bandName + " (+4)"); }
        }
    }
    applyTvPlanPrior(R, f);
}

// TV channel plans as a prior: a flat block 6, 7 or 8 MHz wide whose centre sits on a channel of a TV plan (ISDB-T with its 1/7 MHz offset) is
// a television channel of that width, whatever the country. The standards the plans carry say which modes can sit there (ATSC on 6 MHz plans,
// DTMB on 8 and 6, DVB-T/T2 and ISDB-T on all three, DVB-C on 8 and 6). Only used when the RF frequency is known; it never creates a candidate.
static void applyTvPlanPrior(Result& R, const Features& f) {
    if (f.rfHz <= 0 || f.obwHz < 4.6e6 || f.obwHz > 8.6e6) return;
    if (!(f.flatTop || f.shape == ShapeFlat || f.shape == ShapeRounded)) return;   // a block, not a few tones
    struct Hit { const tvplan::Plan* plan; const tvplan::Channel* ch; };
    std::vector<Hit> hits;
    unsigned stds = 0;
    double widthMhz = 0;
    for (const tvplan::Plan& pl : tvplan::plans()) {
        if (!(pl.stds & (tvplan::DvbAny | tvplan::Atsc | tvplan::Atsc3 | tvplan::IsdbT | tvplan::Dtmb | tvplan::DvbC))) continue;
        const tvplan::Channel* c = pl.at(f.rfHz, 0.35e6);
        if (!c || c->bwHz < 5.9e6 || c->bwHz > 8.1e6) continue;
        if (f.obwHz < 0.68 * c->bwHz || f.obwHz > 1.04 * c->bwHz) continue;   // the measured 99 % width fits the channel
        hits.push_back({&pl, c});
        stds |= pl.stds;
        widthMhz = c->bwHz / 1e6;
    }
    if (hits.empty()) return;
    const Hit& h = hits[0];
    char b[200];
    snprintf(b, sizeof b, "TV channel plan: channel %s of %s (%.0f MHz wide)", h.ch->label.c_str(), h.plan->name.c_str(), h.ch->bwHz / 1e6);
    R.priors.push_back(b);
    (void)widthMhz;
    struct Map { const char* mode; unsigned std; };
    static const Map maps[] = {{"dvbt", tvplan::DvbAny}, {"atsc", tvplan::Atsc}, {"atsc3", tvplan::Atsc3}, {"isdbt", tvplan::IsdbT}, {"dtmb", tvplan::Dtmb}, {"dvbc", tvplan::DvbC}};
    for (Candidate& c : R.cands) {
        for (const Map& m : maps) {
            if (c.modeId != m.mode || !(stds & m.std)) continue;
            c.score = std::min(100.0, c.score + 4);
            c.evidence.push_back(std::string("the frequency is on a ") + (h.ch->bwHz / 1e6 > 7.5 ? "8" : h.ch->bwHz / 1e6 > 6.5 ? "7" : "6") + " MHz TV channel raster (" + h.ch->label + " of " + h.plan->name + ") (+4)");
            break;
        }
    }
}

// The stretch of `want` samples of x[0 .. n) with the most power (a sparse signal is where it is; a continuous one is the same everywhere)
static void denseWindow(const cf32*& x, size_t& n, size_t want) {
    if (n <= want) return;
    const size_t B = 4096, nb = n / B, wb = std::max<size_t>(1, want / B);
    std::vector<double> pre(nb + 1, 0.0);
    for (size_t b = 0; b < nb; b++) { double a = 0; for (size_t i = b * B; i < (b + 1) * B; i += 4) a += std::norm(x[i]); pre[b + 1] = pre[b] + a; }
    size_t bs = 0; double bv = -1;
    for (size_t b = 0; b + wb <= nb; b += std::max<size_t>(1, wb / 16)) if (pre[b + wb] - pre[b] > bv) { bv = pre[b + wb] - pre[b]; bs = b; }
    x += bs * B; n = wb * B;
}

static std::vector<Region> detectRegions(const cf32* x, size_t n, double fs, double thrDb);

static Result identifyImpl(const cf32* x, size_t n, double fs, const Options& o) {
    Result R;
    Features& f = R.f;
    const std::vector<Known>& sigs = signatures();
    n = std::min<size_t>(n, (size_t)4 << 20);
    if (!x || n < 4096 || fs <= 0) { f.why = "too few samples"; return R; }
    f.captureSec = n / fs;
    const bool region = o.hiHz > o.loHz;
    // ---- spectrum (Blackman-Harris, power average) and specx::measure on it
    const double want = region ? (o.hiHz - o.loHz) / 256 : fs / 4096;
    size_t N = pow2At((size_t)std::ceil(fs / std::max(1.0, want)));
    N = std::max<size_t>(512, std::min<size_t>(N, 65536));
    while (N > 256 && N * 8 > n) N >>= 1;
    std::vector<float> win(N);
    double ws = 0, ws2 = 0;
    for (size_t i = 0; i < N; i++) {
        const double t = 2 * kPi * i / N;
        win[i] = (float)(0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t));
        ws += win[i]; ws2 += (double)win[i] * win[i];
    }
    const double enbw = N * ws2 / (ws * ws);
    std::vector<double> acc(N, 0.0);
    const size_t segs = std::min<size_t>(512, (n - N) / (N / 2) + 1);
    const double stride = segs > 1 ? (double)(n - N) / (segs - 1) : 0;
    Fft F((int)N);
    std::vector<cf32> b(N);
    for (size_t s = 0; s < segs; s++) {
        const size_t o0 = (size_t)(s * stride);
        for (size_t i = 0; i < N; i++) b[i] = x[o0 + i] * win[i];
        F.forward(b.data());
        for (size_t i = 0; i < N; i++) acc[i] += std::norm(b[i]);
    }
    std::vector<float> db(N);
    for (size_t i = 0; i < N; i++) db[i] = (float)dB(acc[(i + N / 2) % N] / segs / (ws * ws));
    specx::SpecView v;
    v.db = db.data(); v.n = (int)N; v.binHz = fs / N; v.f0Hz = -fs / 2; v.enbw = enbw;
    specx::Measure ms;
    if (region) ms = specx::measure(v, o.loHz, o.hiHz);
    else {
        const auto found = specx::detect(v, 6, 3);
        for (const auto& c : found) if (!ms.valid || c.powerDb > ms.powerDb) ms = c;
    }
    if (!ms.valid || ms.snrDb < 3) { f.why = "no signal above the noise floor here"; f.mod = ModNoise; R.summary = "nothing but noise"; return R; }
    f.centreHz = ms.obwHz > 0 ? ms.obwCentreHz : ms.centreHz;
    f.bw3Hz = ms.bw3Hz; f.bw20Hz = ms.bw20Hz; f.bw26Hz = ms.bw26Hz; f.obwHz = ms.obwHz;
    f.snrDb = ms.snrDb; f.powerDb = ms.powerDb; f.flatTop = ms.flatTop;
    // ---- down to the signal: centre at 0, filtered to its width with some room, about 6 samples per half width
    const double sigW = std::max(std::max(ms.obwHz, ms.bw20AtFloor ? 0.0 : ms.bw20Hz), 4 * v.binHz);
    double half = 0.5 * sigW * 1.15 + 2 * v.binHz;
    if (region) half = std::min(half, std::max(0.5 * (o.hiHz - o.loHz), 4 * v.binHz));
    half = std::min(half, fs / 2);
    Work w;
    const double cut = std::min(half * 1.2, fs / 2);
    int D = std::max(1, (int)std::floor(fs / std::max(4e3, 6 * half)));
    double noiseBw = fs;
    if (2 * cut > 0.8 * fs) {   // the signal fills the band: only move it to the centre
        D = 1;
        w.y.resize(n);
        double ph = 0; const double dph = -2 * kPi * f.centreHz / fs;
        for (size_t i = 0; i < n; i++) { w.y[i] = x[i] * cf32((float)std::cos(ph), (float)std::sin(ph)); ph += dph; if (ph < -kPi) ph += 2 * kPi; else if (ph > kPi) ph -= 2 * kPi; }
    } else {
        const int L = (int)std::max(31.0, std::min(8191.0, std::ceil(5.5 * fs / std::max(1.0, 0.5 * half)))) | 1;
        // the filter costs n / D * L multiply-adds: past 6e7 (about 50 ms) only the stretch of the capture where the signal is on most is used
        { const bool twoStage = std::min(D, (int)std::floor(fs / (4 * cut))) >= 24 && L >= 300; const double cost = twoStage ? (double)n * 12 : (double)n / D * L; if (cost > 6e7) { const size_t want = std::max<size_t>((size_t)1 << 19, (size_t)(6e7 * D / L)); denseWindow(x, n, want); } }
        w.y = downconvert(x, n, fs, f.centreHz, cut, D, L);
        noiseBw = 2 * cut;
    }
    w.fsd = fs / D;
    if (w.y.size() < 2048) { f.why = "too short a capture for this signal"; return R; }
    w.noise = lin(ms.floorDb) * N / enbw * std::min(1.0, noiseBw / fs);
    gate(w, f, sigW);
    // The measurements that follow cost more than linearly with the number of samples; half a million are plenty (and the duty and the bursts
    // have been counted over the whole capture). A longer capture is cropped to the stretch of it where the signal is on most.
    {
        const size_t cap = (size_t)1 << 19;
        if (w.y.size() > cap) {
            std::vector<size_t> pre(w.y.size() + 1, 0);
            for (size_t i = 0; i < w.y.size(); i++) pre[i + 1] = pre[i] + (w.on[i] ? 1 : 0);
            size_t bs = 0, bv = 0;
            for (size_t s0 = 0; s0 + cap <= w.y.size(); s0 += cap / 8) if (pre[s0 + cap] - pre[s0] > bv) { bv = pre[s0 + cap] - pre[s0]; bs = s0; }
            w.y.erase(w.y.begin() + (long)(bs + cap), w.y.end()); w.y.erase(w.y.begin(), w.y.begin() + (long)bs);
            w.on.erase(w.on.begin() + (long)(bs + cap), w.on.end()); w.on.erase(w.on.begin(), w.on.begin() + (long)bs);
            w.core.erase(w.core.begin() + (long)(bs + cap), w.core.end()); w.core.erase(w.core.begin(), w.core.begin() + (long)bs);
            std::vector<std::pair<size_t, size_t>> runs;
            for (const auto& r : w.runs) {
                if (r.second <= bs || r.first >= bs + cap) continue;
                runs.push_back({r.first > bs ? r.first - bs : 0, std::min(r.second, bs + cap) - bs});
            }
            if (runs.empty()) runs.push_back({0, w.y.size()});
            w.runs = runs;
        }
    }
    // ---- envelope
    double m2 = 0, m4 = 0; size_t non = 0;
    for (size_t i = 0; i < w.y.size(); i++) if (w.core[i]) { const double p = std::norm(w.y[i]); m2 += p; m4 += p * p; non++; }
    m2 /= std::max<size_t>(1, non); m4 /= std::max<size_t>(1, non);
    const double s2 = std::max(0.1 * m2, m2 - w.noise), s4 = std::max(0.5 * s2 * s2, m4 - 4 * s2 * w.noise - 2 * w.noise * w.noise);
    f.envKurt = s4 / (s2 * s2);
    // ---- lines: the carrier, (x/|x|)^n
    powerLines(w, f);
    {   // carrier: a line within a few bins of the centre of y's spectrum
        const size_t m = std::min<size_t>(w.y.size(), (size_t)1 << 20);
        std::vector<cf32> z(m);
        for (size_t i = 0; i < m; i++) z[i] = w.core[i] ? w.y[i] : cf32(0, 0);
        const auto P = powerSpec(z.data(), m);
        // the middle of the 99 % band of an AM signal is a little off its carrier (the audio is not the same on both sides): 4 % of the width
        const double tol = std::max(4 * w.fsd / P.size(), std::min(0.04 * sigW, 400.0));
        const Line c = findLine(P, w.fsd, -tol, tol);
        f.carrierDb = c.ok ? c.db : 0;
    }
    // ---- discriminator
    w.d.assign(w.y.size(), 0.f);
    for (size_t i = 1; i < w.y.size(); i++) if (w.on[i] && w.on[i - 1]) w.d[i] = (float)(std::arg(w.y[i] * std::conj(w.y[i - 1])) * w.fsd / (2 * kPi));
    // ---- symbol rate candidates: envelope and discriminator lines
    const double fmin = std::max(30.0, 0.025 * sigW), fmax = std::min(0.45 * w.fsd, 1.6 * sigW);
    std::vector<float> env(w.y.size()), dsq(w.y.size());
    for (size_t i = 0; i < w.y.size(); i++) env[i] = w.core[i] ? std::norm(w.y[i]) : 0.f;
    double dm = 0; size_t dn = 0;
    for (size_t i = 0; i < w.d.size(); i++) if (w.on[i]) { dm += w.d[i]; dn++; }
    dm /= std::max<size_t>(1, dn);
    for (size_t i = 0; i < w.d.size(); i++) dsq[i] = w.core[i] ? (float)((w.d[i] - dm) * (w.d[i] - dm)) : 0.f;
    std::vector<float> Pe, Pd;
    const Line le = rateLine(env, w.fsd, std::max(fmin, 0.25 * sigW), fmax, &Pe), ld = rateLine(dsq, w.fsd, fmin, fmax, &Pd);
    // The strongest line is not always the symbol rate (a frame structure, a pattern); the rates of the table are looked at one by one, and one of
    // them with a line of its own wins over a stronger line anywhere else. The higher of two rates with about the same line is taken (a line at half the rate is a harmonic of the data).
    auto tableLines = [&]() {
        std::vector<std::pair<double, double>> out;   // (dB, rate)
        std::vector<double> seen;
        for (const Known& k : sigs) for (double r : k.rates) {
            if (k.source != 0) break;   // the rates of the hand-written rows only (the measured ones are measurements themselves)
            if (r < 30 || r < fmin || r > fmax || std::find(seen.begin(), seen.end(), r) != seen.end()) continue;
            seen.push_back(r);
            double db = 0;
            if (!Pd.empty()) { const Line a = findLine(Pd, w.fsd, r * 0.99, r * 1.01); if (a.ok) db = a.db; }
            if (!Pe.empty() && r >= 0.25 * sigW) { const Line b = findLine(Pe, w.fsd, r * 0.99, r * 1.01); if (b.ok) db = std::max(db, b.db - 4); }
            if (db >= 9) out.push_back({db, r});
        }
        std::sort(out.begin(), out.end(), [](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first > b.first; });
        if (out.size() > 4) out.resize(4);
        return out;
    };
    // tones in the discriminator itself: analog FM with a tone (or a dotting preamble)
    double toneFrac = 0;
    {
        std::vector<cf32> z(w.d.size());
        for (size_t i = 0; i < w.d.size(); i++) z[i] = cf32(w.on[i] ? (float)(w.d[i] - dm) : 0.f, 0);
        const auto P = powerSpec(z.data(), z.size());
        const Line t = findLine(P, w.fsd, 20, 0.45 * w.fsd);
        if (t.ok) {
            const size_t Nn = P.size();
            const int k = (int)std::lround(t.hz / (w.fsd / Nn));
            double pl = 0, pt = 0;
            for (size_t i = 1; i < Nn / 2; i++) pt += P[i];
            for (int j = -3; j <= 3; j++) if (k + j > 0 && k + j < (int)Nn / 2) pl += P[(size_t)(k + j)];
            toneFrac = pt > 0 ? pl / pt : 0;
        }
        if (sigW > 80e3 && w.fsd > 45e3) { const Line p = findLine(P, w.fsd, 18900, 19100); f.fmPilot = p.ok && p.db > 15; }
    }
    // ---- smoothed discriminator for the FSK levels
    auto smooth = [&](double rs) {
        const int k = std::max(1, (int)std::lround(0.4 * w.fsd / std::max(1.0, rs)));
        w.ds.assign(w.d.size(), 0.f);
        double acc2 = 0;
        for (size_t i = 0; i < w.d.size(); i++) {
            acc2 += w.d[i];
            if (i >= (size_t)k) acc2 -= w.d[i - (size_t)k];
            const size_t c = i >= (size_t)(k / 2) ? i - (size_t)(k / 2) : 0;
            w.ds[c] = (float)(acc2 / k);
        }
    };
    auto snap = [&](double rs) {
        double best = rs, be = 0.012;
        for (const Known& k : sigs) if (k.source == 0) for (double r : k.rates) if (r > 1 && std::fabs(rs / r - 1) < be) { be = std::fabs(rs / r - 1); best = r; }
        return best;
    };
    auto inTable = [&](double rs) { return snap(rs) != rs; };
    // ---- classification
    std::vector<float> syms; double mid = 0, unit = 1;
    OfdmCorr oc;
    double carrierOff = 0;   // linear modulation: the carrier's own offset from the spectrum's centre (from the x^n lines)
    const bool constEnv = f.envKurt < 1.12;
    if (sigW < 400 && f.carrierDb > 15) f.mod = ModCarrier;
    else if (f.envKurt > 1.6 || (f.flatTop && f.envKurt > 1.45)) {
        if (f.envKurt < 3.5 && ofdmTest(w, f, &oc)) f.mod = ModOfdm;   // Gaussian samples have a kurtosis of 2: spikier than that is a pulse train
        else if (f.envKurt < 2.2 && f.carrierDb < 10 && std::max(f.lineDb[1], f.lineDb[2]) > 20) {}   // a PSK with a small roll-off (its envelope dips deep): the x^n analysis below
        else if (f.duty < 0.97 || le.db > 12) { f.mod = ModPulse; if (le.ok && le.db > 10) { f.symRate = le.hz; f.symLineDb = le.db; f.symSrc = 1; } }
        else f.mod = ModQam;
    } else if (f.carrierDb > 12 && f.envKurt > 1.04 && std::max(f.lineDb[2], f.lineDb[3]) < f.carrierDb + 6) f.mod = ModAm;
    if (f.mod == ModUnknown && constEnv) {
        if (sigW >= 20e3 && chirpTest(w, f)) f.mod = ModChirp;
        else if (f.fmPilot) f.mod = ModFm;
        else {
            // FSK at the discriminator's rate (or the envelope's)
            const double lineMin = f.duty < 0.5 ? 7.0 : 9.0;   // a few short bursts give a weak line
            Line r = ld.ok && ld.db > lineMin ? ld : (le.ok && le.db > lineMin ? le : Line());
            if (!r.ok) { const auto tl = tableLines(); if (!tl.empty()) { r.ok = true; r.db = tl[0].first; r.hz = tl[0].second; } }   // no strong line: a rate of the table with a line of its own
            if (r.ok && r.hz >= sigW / 30 && toneFrac < 0.85) {
                // the rate whose symbols fall into the cleanest levels (the strongest line, or one of the table's rates with a line of its own)
                double rs = snap(r.hz), bestQ = 1e9;
                std::vector<double> cands{rs};
                for (const auto& t : tableLines()) if (std::fabs(t.second / rs - 1) > 0.02) cands.push_back(t.second);
                for (double c : cands) {
                    smooth(c);
                    Features g = f; std::vector<float> sy; double m2 = 0, u2 = 1;
                    const bool okT = fskTest(w, g, c, sy, m2, u2);
                                    if (!okT) continue;
                    const double gap = g.fskLevels == 4 ? g.devHz[2] - g.devHz[1] : g.devHz[1] - g.devHz[0];
                    const double q = g.devSpread / std::max(1e-9, gap) * (c == cands[0] ? 1.0 : 1.15);   // the strongest line is preferred a little
                    if (q < bestQ) { bestQ = q; rs = c; }
                }
                // The line of a sharply keyed FSK can be the second harmonic of its rate. Sampled at twice the rate, the symbols come in equal
                // pairs (in one of the two alignments of each block of sampleSymbols); at the rate, neighbours agree about half the time.
                for (int pass = 0; pass < 2 && rs / 2 >= std::max(fmin, sigW / 30); pass++) {
                    smooth(rs);
                    Features g = f; std::vector<float> sy; double m2 = 0, u2 = 1;
                    if (!fskTest(w, g, rs, sy, m2, u2)) break;
                    std::vector<int> lv;
                    for (float s : sy) {
                        if (std::isnan(s)) { lv.push_back(-1); continue; }
                        int b = 0;
                        for (int j = 1; j < g.fskLevels; j++) if (std::fabs(s - m2 - g.devHz[j]) < std::fabs(s - m2 - g.devHz[b])) b = j;
                        lv.push_back(b);
                    }
                    size_t agree = 0, pairs = 0;
                    for (size_t a = 0; a < lv.size();) {   // the blocks: up to 128 symbols between NaNs
                        size_t e = a;
                        while (e < lv.size() && lv[e] >= 0 && e - a < 128) e++;
                        size_t best = 0, np = (e - a) / 2;
                        for (size_t al = 0; al < 2; al++) { size_t c = 0; for (size_t i = a + al; i + 1 < e; i += 2) c += lv[i] == lv[i + 1]; best = std::max(best, c); }
                        agree += best; pairs += np;
                        a = e < lv.size() && lv[e] < 0 ? e + 1 : std::max(e, a + 1);
                    }
                    if (pairs < 32 || agree < 0.88 * (double)pairs) break;
                    rs = snap(rs / 2);
                }
                smooth(rs);
                if (fskTest(w, f, rs, syms, mid, unit)) {
                    f.symRate = rs; f.symLineDb = r.db; f.symSrc = r.hz == ld.hz || !ld.ok || std::fabs(r.hz / ld.hz - 1) > 0.02 ? 2 : 1;
                    if (f.fskLevels == 4) f.mod = ModFsk4;
                    else {
                        const double h = 2 * f.devHz[1] / rs;   // modulation index
                        f.mod = (h > 0.3 && h < 0.7 && f.lineDb[1] > 10) || (h > 0.35 && h < 0.65) ? ModGmsk : ModFsk2;
                    }
                }
            }
            bool linearNext = false;
            if (f.mod == ModUnknown && f.lineDb[1] > 14) {
                // MSK-like: (x)^2 shows two lines Rs apart, about as strong; one line alone is a BPSK whose envelope dips at the phase
                // reversals (PSK31), which the x^n analysis below takes
                const Line l2 = findLine(w.P[1], w.fsd, -0.48 * w.fsd, 0.48 * w.fsd, f.lineHz[1], std::max(4 * w.fsd / std::max<size_t>(1, w.P[1].size()), 0.03 * sigW));
                if ((!l2.ok || l2.db < f.lineDb[1] - 6) && f.lineDb[1] > f.lineDb[0] + 6) linearNext = true;   // (a carrier of its own in x: not a BPSK)
                else {
                    f.mod = ModGmsk;
                    if (ld.ok && ld.db > 8) { f.symRate = snap(ld.hz); f.symLineDb = ld.db; f.symSrc = 2; }
                }
            }
            if (f.mod == ModUnknown && !linearNext) f.mod = f.carrierDb > 25 && sigW < 400 ? ModCarrier : ModFm;
        }
    }
    if (f.mod == ModUnknown) {
        // linear modulation: the lines of x^2, x^4, x^8 sit at 2, 4, 8 times the carrier's offset D (and Rs apart); pi/4-DQPSK puts its x^4
        // lines Rs/2 beside 4 D, MSK its x^2 lines Rs/2 beside 2 D. Each offset the strongest lines suggest is tried against each class,
        // the simplest that holds wins.
        double rs = le.ok && le.db > 9 ? snap(le.hz) : 0;
        if (rs > 0 && !inTable(rs) && inTable(rs / 2)) rs = snap(rs / 2);   // the line of the envelope is often at twice the rate (the transitions of a filtered PSK)
        if (rs > 0) { f.symRate = rs; f.symLineDb = le.db; f.symSrc = 1; }
        const double maxOff = rs > 0 ? std::min(0.15 * sigW, rs / 16) : 0.05 * sigW;   // pi/4-DQPSK is QPSK Rs/8 off: the spectrum says where the centre is
        auto at = [&](int j, double hz) {
            while (hz >= w.fsd / 2) hz -= w.fsd;
            while (hz < -w.fsd / 2) hz += w.fsd;
            const double t = std::max(3 * w.fsd / std::max<size_t>(1, w.P[j].size()), 0.002 * sigW);
            const Line l = findLine(w.P[j], w.fsd, hz - t, hz + t);
            return l.ok ? l.db : 0.0;
        };
        std::vector<double> offs, rates;
        auto addOff = [&](double d) { if (std::fabs(d) < maxOff) offs.push_back(d); };
        addOff(f.lineHz[3] / 8); addOff(f.lineHz[2] / 4); addOff(f.lineHz[1] / 2);
        if (rs > 0) { addOff((f.lineHz[3] - rs) / 8); addOff((f.lineHz[3] + rs) / 8); addOff((f.lineHz[2] - rs / 2) / 4); addOff((f.lineHz[2] + rs / 2) / 4); }
        int bestMod = ModUnknown, bestRank = 99; double bestOff = 0, bestRs = 0;
        for (double d : offs) {
            rates.clear();
            if (rs > 0) rates.push_back(rs);
            for (double g : {2 * std::fabs(f.lineHz[1] - 2 * d), 2 * std::fabs(f.lineHz[2] - 4 * d)})
                if (g > 0.2 * sigW && g < 1.5 * sigW) rates.push_back(g);
            auto consider = [&](int mod, int rank, double r) { if (rank < bestRank) { bestRank = rank; bestMod = mod; bestOff = d; bestRs = r; } };
            // a line holds when it stands out and is about as strong as the strongest of its spectrum
            auto holds = [&](int j, double hz) { const double v = at(j, hz); return v > 18 && v > f.lineDb[j] - 4; };
            if (holds(1, 2 * d) && at(0, d) < at(1, 2 * d) - 6) consider(ModBpsk, 0, 0);
            for (double r : rates) if (holds(1, 2 * d + r / 2) && holds(1, 2 * d - r / 2) && f.envKurt < 1.3) consider(ModGmsk, 1, r);
            if (holds(2, 4 * d)) consider(f.envKurt > 1.5 ? ModQam : ModQpsk, 2, 0);
            for (double r : rates) if (holds(2, 4 * d + r / 2) && holds(2, 4 * d - r / 2) && holds(3, 8 * d)) consider(ModPi4Dqpsk, 3, r);
            if (holds(3, 8 * d)) consider(ModPsk8, 4, 0);
        }
        double off = 0;
        if (bestMod != ModUnknown) {
            f.mod = bestMod; off = bestOff;
            if (bestRs > 0 && (rs <= 0 || std::fabs(bestRs / rs - 1) > 0.05)) { f.symRate = snap(bestRs); f.symSrc = 3; f.symLineDb = 0; }
        } else if (f.carrierDb > 12) f.mod = ModAm;
        else f.mod = f.envKurt > 1.25 ? ModQam : ModFm;
        if (f.mod != ModAm && f.mod != ModFm) { f.centreHz += off; carrierOff = off; }   // the carrier's own offset
        if (f.mod == ModGmsk && f.symRate > 0) {   // the bits for the AIS flags
            smooth(f.symRate);
            if (!fskTest(w, f, f.symRate, syms, mid, unit)) syms.clear();
        }
    }
    f.rfHz = o.rfHz + f.centreHz;
    // ---- spectral shape, constellation, OFDM carrier spacing
    spectralShape(w, f, sigW);
    if (f.envKurt < 1.15 && sigW < 20e3 && f.mod != ModChirp && f.mod != ModCarrier) mfskTones(w, f, sigW);
    if ((f.mod == ModBpsk || f.mod == ModQpsk || f.mod == ModPi4Dqpsk || f.mod == ModPsk8 || f.mod == ModQam) && f.symRate > 0) {
        constellation(w, f, carrierOff);
        f.rolloff = std::max(-0.2, std::min(2.0, f.obwHz / f.symRate - 1));
    }
    if (f.mod == ModOfdm && f.ofdmTuSec > 0) { f.ofdmSpacingHz = 1 / f.ofdmTuSec; f.ofdmGuard = f.ofdmTgSec / f.ofdmTuSec; }
    // ---- frame syncs (cheap, where the measurement fits)
    int sync[SyncCount] = {};
    bool tried[SyncCount] = {};
    if (f.mod == ModFsk4 && std::fabs(f.symRate - 4800) > 50 && std::fabs(f.symRate - 2400) > 25) {
        // the line that gave the rate may have been a frame pattern: at 4800 and 2400 Bd the synchronisation words of the land-mobile systems decide
        for (double rs : {4800.0, 2400.0}) {
            smooth(rs);
            Features g = f; std::vector<float> sy; double m2 = 0, u2 = 1;
            if (!fskTest(w, g, rs, sy, m2, u2) || g.fskLevels != 4) continue;
            std::vector<float> sf(sy.size());
            for (size_t i = 0; i < sy.size(); i++) sf[i] = std::isnan(sy[i]) ? NAN : (float)((sy[i] - m2) / u2);
            int hits = countSync(sf, p25::kSync, p25::kSyncSym, 0.88) + countSync(sf, nxdn::kFsw, nxdn::kFswSym, 0.94, nxdn::kFrameSym);
            for (int k = 0; k < dmr::kSyncCount; k++) {
                int8_t p8[24]; int p[24];
                dmr::syncSymbols(k, p8);
                for (int j = 0; j < 24; j++) p[j] = p8[j];
                hits += countSync(sf, p, 24, 0.88);
            }
            if (hits >= 2) {
                syms = sy; mid = m2; unit = u2; f.symRate = rs; f.symSrc = 2;
                f.fskLevels = g.fskLevels; for (int j = 0; j < 4; j++) f.devHz[j] = g.devHz[j];
                f.devSpread = g.devSpread;
                break;
            }
        }
    }
    auto soft = [&]() { std::vector<float> s(syms.size()); for (size_t i = 0; i < syms.size(); i++) s[i] = std::isnan(syms[i]) ? NAN : (float)((syms[i] - mid) / unit); return s; };
    auto bits = [&]() { std::vector<uint8_t> o2(syms.size()); for (size_t i = 0; i < syms.size(); i++) o2[i] = std::isnan(syms[i]) ? 255 : syms[i] > mid ? 1 : 0; return o2; };
    if (f.mod == ModFsk4 && !syms.empty()) {
        const auto s = soft();
        if (std::fabs(f.symRate - 4800) < 50) {
            tried[SyncDmr] = tried[SyncP25] = true;
            for (int k = 0; k < dmr::kSyncCount; k++) {
                int8_t p8[24]; int p[24];
                dmr::syncSymbols(k, p8);
                for (int j = 0; j < 24; j++) p[j] = p8[j];
                sync[SyncDmr] += countSync(s, p, 24, 0.88);
            }
            sync[SyncP25] = countSync(s, p25::kSync, p25::kSyncSym, 0.88);
        }
        if (std::fabs(f.symRate - 4800) < 50 || std::fabs(f.symRate - 2400) < 25) {
            tried[SyncNxdn] = true;
            sync[SyncNxdn] = countSync(s, nxdn::kFsw, nxdn::kFswSym, 0.94, nxdn::kFrameSym);
        }
        if (std::fabs(f.symRate - 2400) < 25) {
            tried[SyncDpmr] = true;
            const int* fs4[3] = {dpmr::kFs1, dpmr::kFs3, dpmr::kFs4};
            for (const int* p : fs4) sync[SyncDpmr] += countSync(s, p, dpmr::kFsLong, 0.88);
            sync[SyncDpmr] += countSync(s, dpmr::kFs2, dpmr::kFsShort, 0.92, 2 * dpmr::kFrameSym);   // frames 0 and 2 of a superframe
        }
    }
    if ((f.mod == ModFsk2 || f.mod == ModGmsk) && !syms.empty()) {
        const auto bt = bits();
        tried[SyncPocsag] = true;
        sync[SyncPocsag] = countWord(bt, pager::kPocsagSync, 32, 2);
        if (std::fabs(f.symRate - 9600) < 100) {
            tried[SyncAis] = true;
            std::vector<uint8_t> nr(bt.size(), 255);   // NRZI: a 1 is no change
            for (size_t i = 1; i < bt.size(); i++) if (bt[i] <= 1 && bt[i - 1] <= 1) nr[i] = bt[i] == bt[i - 1];
            sync[SyncAis] = countWord(nr, 0x007E, 16, 1);   // the training alternates on the line: NRZI zeros, then the flag
        }
    }
    // ROS (proprietary, identification only): one tone at a time on a ~15.6 Hz grid, about 2.25 kHz wide, 4 / 8 / 16 Bd
    ros::Detection rosDet;
    if (f.obwHz > 1200 && f.obwHz < 3600) {
        tried[SyncRos] = true;
        ros::DetectOptions ro;
        ro.centreHz = f.centreHz; ro.halfWidthHz = 1500;
        rosDet = ros::detect(x, n, fs, ro);
        sync[SyncRos] = rosDet.ros ? (rosDet.weak ? 1 : 2) : 0;
    }
    // SSB voice (identification only): 2 to 3.8 kHz of one sideband with the heavy-tailed, syllabic envelope of speech (the power of 5 ms blocks
    // swings a lot, the envelope is far from constant) and no symbol clock. USB or LSB is not in the samples (no carrier): toMemory takes the usual one for the band.
    bool ssbOk = false; double ssbCv = 0;
    if (f.obwHz > 1600 && f.obwHz < 3800 && f.envKurt > 2.6 && f.symRate <= 0 && !w.y.empty()) {
        const size_t B = std::max<size_t>(8, (size_t)(0.005 * w.fsd)), nb = w.y.size() / B;
        if (nb >= 60) {
            std::vector<double> pb(nb);
            double mean = 0;
            for (size_t b = 0; b < nb; b++) { double s = 0; for (size_t i = b * B; i < (b + 1) * B; i++) s += std::norm(w.y[i]); pb[b] = std::max(0.0, s / B - w.noise); mean += pb[b]; }
            mean /= nb;
            double var = 0; for (double v : pb) var += (v - mean) * (v - mean);
            var /= nb;
            ssbCv = mean > 0 ? std::sqrt(var) / mean : 0;
            ssbOk = ssbCv > 0.7;
        }
    }
    sync[SyncSsb] = ssbOk ? 2 : 0; tried[SyncSsb] = true;
    // ---- rank the table: every rule that can be applied adds what it earns to `sc` and what it could have earned to `mx`; the score is the
    // share (0 .. 100) of the evidence that holds, so a mode with few measurable traits is not held back by the traits it does not have
    const double rf = f.rfHz / 1e6;
    auto atUsual = [&](const Known& k) { return o.rfHz > 0 && k.defMhz > 0 && std::fabs(rf - k.defMhz) <= std::max(0.025, 0.003 * k.defMhz); };
    for (size_t i = 0; i < sigs.size(); i++) {
        const Known& k = sigs[i];
        if (k.sync == SyncRos && !sync[SyncRos]) continue;   // only proposed when its tone grid is found: its width alone fits too much
        if (k.sync == SyncSsb && !sync[SyncSsb]) continue;   // only proposed when the envelope is speech-like
        if (k.source == 2) {   // a mode known by its name and usual frequency only: a candidate there, with little weight
            if (!atUsual(k)) continue;
            Candidate c;
            c.name = k.name; c.modeId = k.modeId; c.subId = k.subId; c.why = k.note; c.family = k.family; c.source = 2; c.sig = (int)i;
            char t[160];
            snprintf(t, sizeof t, "%.3f MHz is its usual frequency; nothing is known about its signal (30 of 100)", k.defMhz);
            c.evidence.push_back(t);
            c.score = 30;
            R.cands.push_back(c);
            continue;
        }
        double sc = 0, mx = 0;
        Candidate c;
        auto rule = [&](double got, double best, const std::string& text) {
            sc += got; mx += best;
            char t[48];
            snprintf(t, sizeof t, " (%+.0f of %.0f)", got, best);
            c.evidence.push_back(text + t);
        };
        auto F = [](const char* fmt, double a1 = 0, double a2 = 0) { char u[128]; snprintf(u, sizeof u, fmt, a1, a2); return std::string(u); };
        const std::string mn = modName(f.mod);
        const bool primary = f.mod == k.mod, accepted = (k.mods >> f.mod) & 1u;
        if (primary) rule(35, 35, "modulation " + mn + " fits");
        else if (accepted) rule(25, 35, "modulation " + mn + " is one it can show as");
        else if (related(f.mod, k.mod)) rule(8, 35, "modulation " + mn + " is related to its own");
        else rule(-25, 35, "modulation " + mn + " is not what it uses");
        if (k.rates[0] > 0) {
            if (k.mod == ModOfdm) {
                if (f.mod == ModOfdm && f.ofdmTuSec > 0) {
                    // the useful symbol length the mode uses: the measured one, or a correlation peak at one of the lengths the mode has
                    double e = 1e9, best = 0, bestTu = 0;
                    for (double r : k.rates) if (r > 0) { e = std::min(e, std::fabs(f.ofdmTuSec / r - 1)); const double q = oc.at(r); if (q > best) { best = q; bestTu = r; } }
                    if (e < 0.03) rule(30, 30, F("OFDM useful symbol %.4g us matches", f.ofdmTuSec * 1e6));
                    else if (best > 12) rule(26, 30, F("OFDM correlation peak at %.4g us, one of its symbol lengths", bestTu * 1e6));
                    else if (e < 0.08) rule(12, 30, F("OFDM useful symbol %.4g us is close", f.ofdmTuSec * 1e6));
                    else rule(-12, 30, F("OFDM useful symbol %.4g us does not match", f.ofdmTuSec * 1e6));
                }
            } else if (f.symRate > 0) {
                double e = 1e9;
                for (double r : k.rates) if (r > 0) e = std::min(e, std::fabs(f.symRate / r - 1));
                if (e < 0.02) rule(25, 25, F("symbol rate %.0f Bd matches", f.symRate));
                else if (e < 0.06) rule(12, 25, F("symbol rate %.0f Bd is close", f.symRate));
                else rule(-15, 25, F("symbol rate %.0f Bd does not match", f.symRate));
            } else rule(0, 25, F("no symbol rate found, one was expected", 0));
        }
        if (k.bw[0] > 0 && f.obwHz > 0) {
            const bool analog = k.rates[0] <= 0 && (k.mod == ModAm || k.mod == ModFm || k.mod == ModCarrier);   // the audio sets the width: a limit
            double lr = 1e9;   // |ln(measured / nominal)| of the nearest nominal width
            for (double b : k.bw) if (b > 0) lr = std::min(lr, std::fabs(std::log(f.obwHz / b)));
            if (analog) {   // a limit (the audio sets the width), but a carrier far narrower than a wideband mode's channel is not that mode
                if (f.obwHz > 1.5 * k.bw[0]) rule(-10, 10, F("width %.0f Hz is too wide", f.obwHz));
                else if (f.obwHz < 0.15 * k.bw[0] && k.mod == ModFm) rule(-10, 10, F("width %.0f Hz is far narrower than this mode", f.obwHz));
                else rule(10, 10, F("width %.0f Hz fits", f.obwHz));
            }
            else if (lr < 0.07) rule(15, 15, F("width %.0f Hz fits", f.obwHz));
            else if (lr < 0.18) rule(8, 15, F("width %.0f Hz is close", f.obwHz));
            else if (lr < 0.4) rule(0, 15, F("width %.0f Hz is roughly right", f.obwHz));
            else if (lr < 1.0) rule(-8, 15, F("width %.0f Hz is off", f.obwHz));
            else rule(-18, 15, F("width %.0f Hz does not fit", f.obwHz));
        }
        if (f.mod == k.mod && (k.mod == ModChirp || (k.mod == ModFm && f.fmPilot))) rule(20, 20, k.mod == ModChirp ? "chirps: nothing else looks like this" : "stereo pilot at 19 kHz");
        if (k.devOuter > 0 && f.fskLevels) {
            const double outer = std::max(std::fabs(f.devHz[0]), std::fabs(f.devHz[f.fskLevels - 1]));
            const double e = std::fabs(outer / k.devOuter - 1);
            if (e < 0.15) rule(12, 12, F("deviation +-%.0f Hz matches", outer));
            else if (e < 0.3) rule(5, 12, F("deviation +-%.0f Hz is close", outer));
            else rule(-8, 12, F("deviation +-%.0f Hz does not match", outer));
        }
        if (k.duty == 1) rule(f.duty < 0.7 ? -8 : 6, 6, f.duty < 0.7 ? "bursty, the mode is continuous" : "continuous like the mode");
        else if (k.duty == 2) rule(f.duty > 0.97 ? -6 : 6, 6, f.duty > 0.97 ? "continuous, the mode is bursty" : "bursty like the mode");
        if (k.burstMs > 0 && f.bursts >= 3) {
            const double e = std::fabs(std::log(f.burstMs / k.burstMs));
            if (e < 0.35) rule(10, 10, F("bursts of %.3g ms match", f.burstMs)); else rule(-6, 10, F("bursts of %.3g ms do not match", f.burstMs));
        }
        if (k.periodMs > 0 && f.burstPeriodMs > 0) {
            const double e = std::fabs(std::log(f.burstPeriodMs / k.periodMs));
            if (e < 0.1) rule(10, 10, F("burst period %.3g ms matches", f.burstPeriodMs)); else rule(-6, 10, F("burst period %.3g ms does not match", f.burstPeriodMs));
        }
        if (k.bands[0] > 0 && o.rfHz > 0) {
            bool in = false;
            for (int j = 0; j + 1 < 10 && k.bands[j] > 0; j += 2) if (rf >= k.bands[j] && rf <= k.bands[j + 1]) in = true;
            if (in) rule(12, 12, F("%.3f MHz is in its band", rf)); else rule(-3, 12, F("%.3f MHz is outside its band", rf));   // a table of bands is never complete: a small penalty
        }
        if (k.bands[0] <= 0 && atUsual(k)) rule(12, 12, F("%.3f MHz is the mode's usual frequency", rf));
        if (k.toneHz > 0 && f.toneSpacingHz > 0) {   // an MFSK tone set
            const double e = std::fabs(f.toneSpacingHz / k.toneHz - 1);
            if (e < 0.08) rule(10, 10, F("tones %.4g Hz apart, its tone spacing", f.toneSpacingHz));
            else rule(-4, 10, F("tones %.4g Hz apart, not its tone spacing", f.toneSpacingHz));
        }
        if (f.constOrder > 0) {   // the constellation guess against the mode's own (pi/4-DQPSK: 4 or 8 points, as its two sets alternate)
            const int want = k.mod == ModBpsk ? 2 : k.mod == ModQpsk ? 4 : k.mod == ModPsk8 ? 8 : k.mod == ModPi4Dqpsk ? 48 : k.mod == ModQam ? 16 : 0;
            if (want) {
                const bool ok = f.constOrder == want || (want == 16 && f.constOrder == 64) || (want == 48 && (f.constOrder == 4 || f.constOrder == 8));
                if (ok) rule(6, 6, F("constellation looks %.0f-point, like its own", f.constOrder));
                else rule(-4, 6, F("constellation looks %.0f-point, not like its own", f.constOrder));
            }
        }
        if (k.mod == ModOfdm && f.combHz > 0 && f.ofdmSpacingHz > 0) {   // pilots: a comb every whole number of carriers
            const double q = f.combHz / f.ofdmSpacingHz;
            if (q > 0.9 && std::fabs(q - std::round(q)) < 0.06) rule(5, 5, F("spectral comb every %.0f carriers (pilots)", std::round(q)));
        }
        c.name = k.name; c.modeId = k.modeId; c.subId = k.subId; c.why = k.note; c.family = k.family; c.source = k.source; c.sig = (int)i;
        if (k.sync == SyncRos) {
            c.syncTried = true; c.syncHits = sync[k.sync];
            rule(c.syncHits >= 2 ? 40 : 15, 40, (c.syncHits >= 2 ? "ROS tone grid: " : "ROS tone grid, few symbols: ") + rosDet.why);
            c.why = rosDet.name + ", " + k.note;
        } else if (k.sync == SyncSsb) {
            c.syncTried = true; c.syncHits = sync[k.sync];
            rule(40, 40, F("speech-like envelope (power of 5 ms blocks varies %.1f x its mean, kurtosis %.1f)", ssbCv, f.envKurt));
        } else if (k.sync && tried[k.sync]) {
            c.syncTried = true; c.syncHits = sync[k.sync];
            if (c.syncHits >= 2) rule(40, 40, F("frame sync found %.0f times", c.syncHits));
            else if (c.syncHits == 1) rule(15, 40, F("frame sync found once", 0));
            else rule(-10, 40, F("frame sync not found", 0));
        }
        c.score = std::max(0.0, std::min(100.0, 100.0 * sc / std::max(mx, 70.0)));   // a mode with few measurable traits cannot reach the top: little evidence is little evidence
        if (c.score >= 20) R.cands.push_back(c);
    }
    if (o.usePriors && o.rfHz > 0) applyPriors(R, f);
    auto byScore = [](const Candidate& a, const Candidate& b) { return a.score > b.score; };
    std::stable_sort(R.cands.begin(), R.cands.end(), byScore);
    // ---- verify: the detectors of the best few candidates, each at that mode's own parameters. A frame sync, the burst timing and the
    // deviation have counted already (they only set the verdict of the check); the symbol clock at the mode's exact rate, the cyclic prefix at
    // its exact symbol length and the chirp width are finer than the scoring's tolerances and move the score.
    if (o.verify && !R.cands.empty()) {
        const int K = std::min<int>(std::max(0, o.verifyTop), (int)R.cands.size());
        auto lineAt = [&](double r, double tol) {   // the strongest symbol-clock line within r (1 -+ tol): discriminator, envelope
            double db = 0;
            if (!Pd.empty()) { const Line a = findLine(Pd, w.fsd, r * (1 - tol), r * (1 + tol)); if (a.ok) db = a.db; }
            if (!Pe.empty()) { const Line b = findLine(Pe, w.fsd, r * (1 - tol), r * (1 + tol)); if (b.ok) db = std::max(db, b.db); }
            return db;
        };
        for (int i = 0; i < K; i++) {
            Candidate& c = R.cands[(size_t)i];
            if (c.sig < 0 || c.source == 2) continue;
            const Known& k = sigs[(size_t)c.sig];
            int ok = 0, bad = 0, okScore = 0, badScore = 0;
            auto note = [&](bool pass, bool scores, const std::string& t) {
                (pass ? ok : bad)++;
                if (scores) (pass ? okScore : badScore)++;
                c.verifyNotes.push_back((pass ? "confirmed: " : "not confirmed: ") + t);
            };
            char t[160];
            if (k.sync && tried[k.sync]) { snprintf(t, sizeof t, "frame sync found %d times", sync[k.sync]); note(sync[k.sync] >= 2, false, t); }
            if (k.mod == ModOfdm && k.rates[0] > 0 && f.mod == ModOfdm && !oc.mag.empty()) {
                double best = 0, bt = 0;
                for (double r : k.rates) if (r > 0) { const double q = oc.at(r); if (q > best) { best = q; bt = r; } }
                snprintf(t, sizeof t, "cyclic prefix at its symbol length %.4g us (%.1f x the median correlation)", bt * 1e6, best);
                if (best > 12) note(true, true, t); else if (best < 5) note(false, true, t);
            }
            if (k.mod != ModOfdm && k.mod != ModChirp && k.rates[0] > 0 && f.symRate > 0) {
                double bestDb = 0, bestR = 0;
                for (double r : k.rates) if (r > fmin && r < fmax) { const double db = lineAt(r, 0.004); if (db > bestDb) { bestDb = db; bestR = r; } }
                if (bestR > 0) {
                    snprintf(t, sizeof t, "symbol clock at %s (%.0f dB)", rateText(bestR).c_str(), bestDb);
                    if (bestDb >= 12 && std::fabs(f.symRate / bestR - 1) < 0.01) note(true, true, t);
                    else if (bestDb < 6) note(false, true, std::string(t) + ", no clock there");
                }
            }
            if (k.mod == ModChirp) {
                if (f.mod != ModChirp) note(false, true, "no chirps");
                else {
                    bool fit = false;
                    for (double b : k.bw) if (b > 0 && std::fabs(f.chirpBwHz / b - 1) < 0.1) fit = true;
                    snprintf(t, sizeof t, "chirps of SF%d, %s wide", f.loraSf, hzText(f.chirpBwHz).c_str());
                    note(fit, true, std::string(t) + (fit ? "" : ", not its width"));
                }
            }
            if (k.mod == ModFm && f.fmPilot) note(true, false, "19 kHz stereo pilot");
            if (k.burstMs > 0 && f.bursts >= 3) { snprintf(t, sizeof t, "bursts of %.3g ms (its own %.3g ms)", f.burstMs, k.burstMs); note(std::fabs(std::log(f.burstMs / k.burstMs)) < 0.25, false, t); }
            if (k.periodMs > 0 && f.burstPeriodMs > 0) { snprintf(t, sizeof t, "frame period %.3g ms (its own %.3g ms)", f.burstPeriodMs, k.periodMs); note(std::fabs(std::log(f.burstPeriodMs / k.periodMs)) < 0.06, false, t); }
            if (k.devOuter > 0 && f.fskLevels) {
                const double outer = std::max(std::fabs(f.devHz[0]), std::fabs(f.devHz[f.fskLevels - 1])), e = std::fabs(outer / k.devOuter - 1);
                snprintf(t, sizeof t, "deviation +-%.0f Hz (its own +-%.0f Hz)", outer, k.devOuter);
                if (e < 0.1) note(true, false, t); else if (e > 0.4) note(false, false, t);
            }
            c.verify = bad ? VerifyFailed : ok ? VerifyConfirmed : c.verifyNotes.empty() ? VerifyNotTried : VerifyInconclusive;
            const double adj = 4.0 * std::min(okScore, 2) - 8.0 * std::min(badScore, 2);
            if (adj != 0) {
                c.score = std::max(0.0, std::min(100.0, c.score + adj));
                snprintf(t, sizeof t, "verify: %d of its detectors confirmed it, %d did not (%+.0f)", okScore, badScore, adj);
                c.evidence.push_back(t);
            }
        }
        std::stable_sort(R.cands.begin(), R.cands.end(), byScore);
    }
    if (R.cands.size() > 6) R.cands.resize(6);
    calibrate(R);
    // ---- the summary line
    std::string s = modName(f.mod);
    if (f.mod == ModOfdm && f.ofdmTuSec > 0) {
        char t[96]; snprintf(t, sizeof t, ", Tu %.4g us", f.ofdmTuSec * 1e6); s += t;
        if (f.ofdmTgSec > 0) { snprintf(t, sizeof t, ", guard 1/%.0f", f.ofdmTuSec / f.ofdmTgSec); s += t; }
    } else if (f.mod == ModChirp) {
        char t[64]; snprintf(t, sizeof t, ", SF%d, %s", f.loraSf, hzText(f.chirpBwHz).c_str()); s += t;
    } else if (f.symRate > 0) s += ", " + rateText(f.symRate);
    if (f.fskLevels == 4) { char t[64]; snprintf(t, sizeof t, ", +-%.0f / +-%.0f Hz", (f.devHz[2] - f.devHz[1]) / 2, (f.devHz[3] - f.devHz[0]) / 2); s += t; }
    else if (f.fskLevels == 2) { char t[48]; snprintf(t, sizeof t, ", +-%.0f Hz", (f.devHz[1] - f.devHz[0]) / 2); s += t; }
    if (f.fmPilot) s += ", stereo pilot";
    s += ", " + hzText(f.obwHz) + " wide";
    if (f.duty < 0.97) { char t[64]; snprintf(t, sizeof t, ", bursts (%.0f %% on, %.3g ms)", f.duty * 100, f.burstMs); s += t; }
    else s += ", continuous";
    R.summary = s;
    f.valid = true;
    return R;
}

static std::string headlineOf(const Result& R) {
    const Features& f = R.f;
    if (!f.valid) return f.why.empty() ? "Nothing to identify here." : f.why;
    std::string h = modName(f.mod);
    if (f.mod == ModOfdm && f.ofdmTuSec > 0) { char t[48]; snprintf(t, sizeof t, ", Tu %.4g us", f.ofdmTuSec * 1e6); h += t; }
    else if (f.mod == ModChirp) { char t[48]; snprintf(t, sizeof t, ", SF%d", f.loraSf); h += t; }
    else if (f.symRate > 0) { char t[48]; snprintf(t, sizeof t, ", %.0f baud", f.symRate); h += t; }
    h += ", " + hzText(f.obwHz) + " wide. ";
    char t[160];
    if (R.verdict == VerdictUnknown || R.cands.empty()) h += "I do not know what this is.";
    else if (R.verdict == VerdictLikely) { snprintf(t, sizeof t, "Most likely %s, %.0f %% sure.", R.cands[0].name.c_str(), R.cands[0].confidence * 100); h += t; }
    else if (R.cands.size() > 1) { snprintf(t, sizeof t, "Could be %s (%.0f %%) or %s (%.0f %%).", R.cands[0].name.c_str(), R.cands[0].confidence * 100, R.cands[1].name.c_str(), R.cands[1].confidence * 100); h += t; }
    else { snprintf(t, sizeof t, "Perhaps %s (%.0f %%).", R.cands[0].name.c_str(), R.cands[0].confidence * 100); h += t; }
    return h;
}

Result identify(const cf32* x, size_t n, double fs, const Options& o) {
    const auto t0 = std::chrono::steady_clock::now();
    Options o2 = o;
    if (!(o.hiHz > o.loHz)) {   // no region given: the strongest signal, as the detector widens and merges the runs of its spectrum (an FM signal is a comb of lines)
        const std::vector<Region> regs = detectRegions(x, n, fs, 6);
        const Region* best = nullptr;
        for (const Region& r : regs) if (!best || r.powerDb > best->powerDb) best = &r;
        if (best) { o2.loHz = best->loHz; o2.hiHz = best->hiHz; }
    }
    Result R = identifyImpl(x, n, fs, o2);
    R.headline = headlineOf(R);
    R.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return R;
}

Memory toMemory(const Candidate& c, const Features& f) {
    Memory m;
    char b[96];
    snprintf(b, sizeof b, "%s %.4f MHz", c.name.c_str(), f.rfHz / 1e6);
    m.name = b;
    m.freqHz = (uint64_t)std::llround(f.rfHz);
    m.mode = !c.subId.empty() ? c.subId : c.modeId == "dvbt" ? "dvb" : c.modeId;
    if (f.obwHz > 0) m.bandwidthHz = f.obwHz;
    // the measured width reaches the mode: DVB-T / T2 takes the channel width as an index (8, 7, 6, 5, 1.7 MHz), left on auto only when nothing was measured
    if (c.modeId == "dvbt") m.options["dvbBw"] = f.obwHz > 7.2e6 ? "0" : f.obwHz > 6.2e6 ? "1" : f.obwHz > 5.2e6 ? "2" : f.obwHz > 3.0e6 ? "3" : f.obwHz > 0 ? "4" : "auto";
    if (c.name == "NFM voice") m.options["genDemod"] = "0";   // GenNfm
    if (c.name == "SSB voice") {   // the sideband is not in the samples (no carrier): LSB below 10 MHz, USB above, the dial at the missing carrier (about 300 Hz below / above the audio)
        const bool lsb = f.rfHz < 10e6 && f.rfHz > 0;
        m.options["genDemod"] = lsb ? "4" : "3";   // GenUsb, GenLsb
        const double edge = (f.obwHz > 0 ? f.obwHz / 2 : 1200) + 300;
        m.freqHz = (uint64_t)std::llround(f.rfHz + (lsb ? edge : -edge));
        m.name = "SSB " + std::string(lsb ? "LSB " : "USB ") + [&] { char t[32]; snprintf(t, sizeof t, "%.4f MHz", m.freqHz / 1e6); return std::string(t); }();
        if (f.obwHz > 0) m.options["genWidthHz"] = "2700";
    }
    if (c.modeId == "general" && f.obwHz > 0) { char w[32]; snprintf(w, sizeof w, "%.0f", f.obwHz); m.options["genWidthHz"] = w; }   // the General receiver's filter width (it clamps to what the demodulator allows)
    if (c.modeId == "dtmb") m.options["dtmbBwMhz"] = f.obwHz > 6.8e6 ? "8" : "6";
    if (c.modeId == "isdbt") m.options["isdbtBwMhz"] = f.obwHz > 7.0e6 ? "8" : f.obwHz > 6.0e6 ? "7" : "6";
    m.tags = "identified";
    m.notes = "Identified from the signal: " + std::string(c.why);
    return m;
}

std::vector<std::pair<std::string, std::string>> featureRows(const Features& f) {
    std::vector<std::pair<std::string, std::string>> v;
    if (!f.valid) return v;
    char b[160];
    auto add = [&](const char* k, const std::string& s) { v.push_back({k, s}); };
    add("Centre", hzText(f.rfHz));
    snprintf(b, sizeof b, "%s (99 %%); -3 dB %s, -20 dB %s, -26 dB %s", hzText(f.obwHz).c_str(), hzText(f.bw3Hz).c_str(), hzText(f.bw20Hz).c_str(), hzText(f.bw26Hz).c_str()); add("Width", b);
    snprintf(b, sizeof b, "%.1f dB", f.snrDb); add("SNR", b);
    add("Spectrum", shapeName(f.shape) + std::string(f.flatTop ? " (flat top)" : ""));
    if (f.tones > 0) { snprintf(b, sizeof b, "%d", f.tones); std::string s = b; if (f.toneSpacingHz > 0) s += ", " + hzText(f.toneSpacingHz) + " apart"; add("Tones", s); }
    if (f.combHz > 0) { snprintf(b, sizeof b, "every %s (strength %.2f)", hzText(f.combHz).c_str(), f.combStrength); add("Spectral comb", b); }
    if (f.duty < 0.97) {
        snprintf(b, sizeof b, "bursts, %.0f %% on, %d of about %.3g ms", f.duty * 100, f.bursts, f.burstMs);
        std::string s = b;
        if (f.burstPeriodMs > 0) { snprintf(b, sizeof b, ", every %.3g ms", f.burstPeriodMs); s += b; }
        add("On the air", s);
    } else add("On the air", "continuous");
    add("Modulation", modName(f.mod));
    snprintf(b, sizeof b, "%.2f (1 constant, 2 noise-like)", f.envKurt); add("Envelope", b);
    if (f.carrierDb > 0) { snprintf(b, sizeof b, "%.1f dB over the spectrum", f.carrierDb); add("Carrier line", b); }
    snprintf(b, sizeof b, "x %.0f, x^2 %.0f, x^4 %.0f, x^8 %.0f dB", f.lineDb[0], f.lineDb[1], f.lineDb[2], f.lineDb[3]); add("Power lines", b);
    if (f.symRate > 0) {
        static const char* const kSrc[] = {"", "|x|^2 line", "FM discriminator line", "x^n lines", "chirp length", "OFDM symbol"};
        snprintf(b, sizeof b, "%s (%s, %.0f dB)", rateText(f.symRate).c_str(), kSrc[f.symSrc >= 0 && f.symSrc < 6 ? f.symSrc : 0], f.symLineDb); add("Symbol rate", b);
    }
    if (f.rolloff >= 0) { snprintf(b, sizeof b, "%.2f (99 %% width / symbol rate - 1)", f.rolloff); add("Roll-off", b); }
    if (f.constOrder > 0 || f.ampRings > 0) {
        snprintf(b, sizeof b, "%d-point guess; %d amplitude level%s; u^2 %.2f, u^4 %.2f, u^8 %.2f", f.constOrder, f.ampRings, f.ampRings == 1 ? "" : "s", f.phaseCoh[0], f.phaseCoh[1], f.phaseCoh[2]);
        add("Constellation", b);
    }
    if (f.fskLevels == 4) { snprintf(b, sizeof b, "%+.0f / %+.0f / %+.0f / %+.0f Hz", f.devHz[0], f.devHz[1], f.devHz[2], f.devHz[3]); add("FSK levels", b); }
    if (f.fskLevels == 2) { snprintf(b, sizeof b, "%+.0f / %+.0f Hz", f.devHz[0], f.devHz[1]); add("FSK levels", b); }
    if (f.mod == ModOfdm && f.ofdmTuSec > 0) {
        snprintf(b, sizeof b, "Tu %.4g us (carriers %s apart), guard %.4g us (1/%.0f), correlation %.2f", f.ofdmTuSec * 1e6, hzText(f.ofdmSpacingHz).c_str(), f.ofdmTgSec * 1e6, f.ofdmGuard > 0 ? 1 / f.ofdmGuard : 0.0, f.ofdmCorr);
        add("OFDM", b);
    }
    if (f.mod == ModChirp) { snprintf(b, sizeof b, "SF%d, %s", f.loraSf, hzText(f.chirpBwHz).c_str()); add("LoRa", b); }
    if (f.fmPilot) add("FM", "19 kHz stereo pilot");
    snprintf(b, sizeof b, "%.3g s", f.captureSec); add("Capture", b);
    return v;
}

std::string toText(const Result& r) {
    const Features& f = r.f;
    std::string t = r.headline.empty() ? r.summary : r.headline;
    t += "\n";
    if (!f.valid) return t;
    char b[256];
    for (const auto& kv : featureRows(f)) t += kv.first + ": " + kv.second + "\n";
    for (const std::string& p : r.priors) t += "Frequency list: " + p + "\n";
    for (size_t i = 0; i < r.cands.size(); i++) {
        const Candidate& c = r.cands[i];
        static const char* const kVer[] = {"failed", "not verified", "verified", "inconclusive"};
        snprintf(b, sizeof b, "%zu. %s (%s%s%s): %.0f %% sure, score %.0f, %s\n", i + 1, c.name.c_str(), c.modeId.c_str(), c.subId.empty() ? "" : "/", c.subId.c_str(), c.confidence * 100, c.score,
                 kVer[std::max(-1, std::min(2, c.verify)) + 1]);
        t += b;
        if (i == 0) for (const std::string& e : c.evidence) t += "     " + e + "\n";
        for (const std::string& e : c.verifyNotes) t += "     " + e + "\n";
    }
    t += "Identified by OnAir (classical DSP, calibrated on its own test signals; not verified on every real signal).\n";
    return t;
}

// ---------------------------------------------------------------------------------------------------- regions

static std::vector<Region> detectRegions(const cf32* x, size_t n, double fs, double thrDb) {
    std::vector<Region> out;
    if (!x || n < 4096 || fs <= 0) return out;
    n = std::min<size_t>(n, (size_t)4 << 20);
    size_t N = pow2At((size_t)std::ceil(fs / 2000.0));
    N = std::max<size_t>(4096, std::min<size_t>(N, 65536));
    while (N > 1024 && N * 8 > n) N >>= 1;
    std::vector<float> win(N);
    double ws = 0, ws2 = 0;
    for (size_t i = 0; i < N; i++) {
        const double t = 2 * kPi * i / N;
        win[i] = (float)(0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t));
        ws += win[i]; ws2 += (double)win[i] * win[i];
    }
    const double enbw = N * ws2 / (ws * ws);
    std::vector<double> acc(N, 0.0);
    const size_t segs = std::min<size_t>(256, (n - N) / (N / 2) + 1);
    const double stride = segs > 1 ? (double)(n - N) / (segs - 1) : 0;
    Fft F((int)N);
    std::vector<cf32> b(N);
    for (size_t s = 0; s < segs; s++) {
        const size_t o0 = (size_t)(s * stride);
        for (size_t i = 0; i < N; i++) b[i] = x[o0 + i] * win[i];
        F.forward(b.data());
        for (size_t i = 0; i < N; i++) acc[i] += std::norm(b[i]);
    }
    std::vector<float> db(N);
    for (size_t i = 0; i < N; i++) db[i] = (float)dB(acc[(i + N / 2) % N] / segs / (ws * ws));
    specx::SpecView v;
    v.db = db.data(); v.n = (int)N; v.binHz = fs / N; v.f0Hz = -fs / 2; v.enbw = enbw;
    // the runs above the floor, then each one widened to where the smoothed level meets the floor again (the skirts of a wide signal are below the
    // detection threshold at modest SNR) and runs that touch merged (FM: the stereo and RDS parts beside the carrier)
    const specx::Floor fl = specx::fitFloor(v);
    std::vector<float> sm(N);
    for (size_t i = 0; i < N; i++) {
        double a = 0; int c = 0;
        for (int j = -3; j <= 3; j++) { const long k = (long)i + j; if (k >= 0 && k < (long)N) { a += lin(db[(size_t)k]); c++; } }
        sm[i] = (float)dB(a / c);
    }
    struct Run { long lo, hi; };
    std::vector<Run> runs;
    for (const auto& m : specx::detect(v, thrDb, 3)) {
        if (!m.valid) continue;
        long lo = (long)std::floor(v.bin(m.loHz)), hi = (long)std::ceil(v.bin(m.hiHz));
        const long w = hi - lo, limit = std::max<long>(8, 2 * w);
        long k = lo;
        while (k > 0 && lo - k < limit && sm[(size_t)(k - 1)] > fl.at((double)(k - 1)) + 2.5) k--;
        lo = k;
        k = hi;
        while (k + 1 < (long)N && k - hi < limit && sm[(size_t)(k + 1)] > fl.at((double)(k + 1)) + 2.5) k++;
        hi = k;
        runs.push_back({lo, hi});
    }
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.lo < b.lo; });
    std::vector<Run> merged;
    for (const Run& r : runs) {
        if (!merged.empty() && r.lo <= merged.back().hi + std::max<long>(std::max<long>(4, (long)(1500.0 / v.binHz)), (merged.back().hi - merged.back().lo) / 8)) merged.back().hi = std::max(merged.back().hi, r.hi);   // lines up to 1.5 kHz apart are one signal (a tone-modulated FM carrier is a comb)
        else merged.push_back(r);
    }
    for (const Run& r : merged) {
        const specx::Measure m = specx::measure(v, v.freq((double)r.lo), v.freq((double)r.hi));
        if (!m.valid) continue;
        Region g;
        g.loHz = v.freq((double)r.lo); g.hiHz = v.freq((double)r.hi); g.centreHz = m.obwHz > 0 ? m.obwCentreHz : m.centreHz;
        g.bwHz = m.obwHz > 0 ? m.obwHz : m.bw3Hz; g.snrDb = m.snrDb; g.powerDb = m.powerDb;
        out.push_back(g);
    }
    return out;
}

Region regionAt(const cf32* x, size_t n, double fs, double clickOffHz, double fallbackWidthHz) {
    Region best;
    bool found = false;
    for (const Region& r : detectRegions(x, n, fs, 6)) if (clickOffHz >= r.loHz && clickOffHz <= r.hiHz && (!found || r.powerDb > best.powerDb)) { best = r; found = true; }
    if (found) return best;
    best.loHz = clickOffHz - fallbackWidthHz / 2; best.hiHz = clickOffHz + fallbackWidthHz / 2; best.centreHz = clickOffHz; best.bwHz = fallbackWidthHz;
    return best;
}

std::vector<Region> findRegions(const cf32* x, size_t n, double fs, size_t maxN, double thresholdDb) {
    std::vector<Region> r = detectRegions(x, n, fs, thresholdDb);
    std::sort(r.begin(), r.end(), [](const Region& a, const Region& b) { return a.powerDb > b.powerDb; });
    if (r.size() > maxN) r.resize(maxN);
    return r;
}

size_t signatureCount() { return signatures().size(); }
const SigInfo& signatureInfo(size_t i) {
    static const std::vector<SigInfo> v = [] {
        std::vector<SigInfo> o;
        for (const Known& k : signatures()) o.push_back({k.name, k.modeId, k.subId, k.family, k.source});
        return o;
    }();
    return v[i < v.size() ? i : 0];
}

// ---------------------------------------------------------------------------------------------------- Capture

struct Capture::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    std::thread th;
    bool stop = false, work = false;
    std::vector<cf32> buf;
    size_t want = 0;
    double rate = 0, cOff = 0, width = 0, rf = 0;
    int state = 0;
    Result res;
    uint64_t seq = 0;
    uint64_t gen = 0;              // start() count: a result of an older start is thrown away
    bool all = false;              // startAll(): every signal in the capture
    size_t maxN = 12;
    std::vector<Found> found;
    uint64_t seqAll = 0;
    bool keep = false;             // keepSamples(): the last single capture stays for Signal Lab
    Kept kept;
};

Capture::Capture() : p_(new Impl) {
    p_->th = dect2::namedThread("onair-sigid", [this] {
        Impl& m = *p_;
        (void)builtinFrequencies(); (void)bandplan::builtinPlans();   // parsed once, on first use: not while someone waits for an answer
        (void)signatureCount();
        std::unique_lock<std::mutex> lk(m.mu);
        for (;;) {
            m.cv.wait(lk, [&] { return m.stop || m.work; });
            if (m.stop) return;
            m.work = false;
            std::vector<cf32> x;
            x.swap(m.buf);
            const double rate = m.rate, c = m.cOff, wd = m.width, rf = m.rf;
            const bool all = m.all;
            const size_t maxN = m.maxN;
            const uint64_t g = m.gen;
            lk.unlock();
            Options o;
            o.rfHz = rf;
            if (wd > 0) { o.loHz = c - wd / 2; o.hiHz = c + wd / 2; }
            Result r;
            std::vector<Found> fl;
            if (all) {
                for (const Region& rg : findRegions(x.data(), x.size(), rate, maxN)) {
                    Options oo;
                    oo.rfHz = rf; oo.loHz = rg.loHz; oo.hiHz = rg.hiHz;
                    Found fd; fd.region = rg; fd.res = identify(x.data(), x.size(), rate, oo);
                    fl.push_back(std::move(fd));
                }
            } else r = identify(x.data(), x.size(), rate, o);
            lk.lock();
            if (g != m.gen) continue;   // restarted meanwhile
            if (!all && m.keep) { m.kept.x = std::move(x); m.kept.rateHz = rate; m.kept.centreOffHz = c; m.kept.widthHz = wd; m.kept.rfHz = rf; m.kept.seq++; }
            x.clear(); x.shrink_to_fit();
            if (all) { m.found = std::move(fl); m.seqAll++; }
            else { m.res = std::move(r); m.seq++; }
            m.state = 3;
        }
    });
}

Capture::~Capture() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->stop = true; }
    p_->cv.notify_all();
    if (p_->th.joinable()) p_->th.join();
}

void Capture::start(double rate, double centreOffHz, double widthHz, double rfHz, double seconds) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->gen++;
    p_->all = false;
    p_->rate = rate; p_->cOff = centreOffHz; p_->width = widthHz; p_->rf = rfHz;
    p_->want = (size_t)std::min(rate * seconds, 4.0 * (1 << 20));
    p_->buf.clear();
    p_->buf.reserve(p_->want);
    p_->state = rate > 0 ? 1 : 0;
    armed_ = rate > 0;
}

void Capture::startAll(double rate, double rfHz, double seconds, size_t maxN) {
    start(rate, 0, 0, rfHz, seconds);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->all = true; p_->maxN = maxN;
}

bool Capture::resultAll(std::vector<Found>& out, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seqAll == seq || p_->state != 3 || !p_->all) return false;
    out = p_->found; seq = p_->seqAll;
    return true;
}

void Capture::cancel() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->gen++;
    armed_ = false;
    p_->buf.clear(); p_->buf.shrink_to_fit();
    p_->state = 0;
}

void Capture::push(const cf32* x, size_t n) {
    if (!armed()) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->state != 1) return;
    const size_t k = std::min(n, p_->want - p_->buf.size());
    p_->buf.insert(p_->buf.end(), x, x + k);
    if (p_->buf.size() >= p_->want) { armed_ = false; p_->state = 2; p_->work = true; p_->cv.notify_one(); }
}

int Capture::state() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->state; }
double Capture::progress() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->state == 1 && p_->want ? (double)p_->buf.size() / p_->want : p_->state >= 2 ? 1.0 : 0.0;
}
bool Capture::result(Result& out, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq == seq || p_->state != 3 || p_->all) return false;
    out = p_->res; seq = p_->seq;
    return true;
}
double Capture::regionCentre() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cOff; }
void Capture::keepSamples(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->keep = on;
    if (!on) { p_->kept.x.clear(); p_->kept.x.shrink_to_fit(); }
}
uint64_t Capture::keptSeq() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->kept.x.empty() ? 0 : p_->kept.seq; }
bool Capture::keptSamples(Kept& out, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->kept.x.empty() || p_->kept.seq == seq) return false;
    out = p_->kept; seq = p_->kept.seq;
    return true;
}

} // namespace sigid
} // namespace dect2
