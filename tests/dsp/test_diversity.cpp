// Two-branch diversity combiner: two noisy copies of one narrowband signal, heard by free-running radios (delay, frequency offset, drifting
// sampling phase, complex gain), are combined by selection and by maximal ratio. The in-band SNR of the output is measured against the known
// clean signal and printed next to the input SNRs. Also: a dead branch A, no common signal, identical branches, config checks.
#include "dect2/dsp/diversity.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = 3.14159265358979323846;

constexpr int kLen = 1 << 18;      // 1.05 s at 250 ksps
constexpr double kFs = 250e3;
constexpr double kBw = 12e3;       // occupied bandwidth of the wanted signal, centred 20 kHz above the tuned frequency
constexpr double kOff = 20e3;

// a periodic, band-limited, noise-like signal of unit power in its band: S[k] random on the bins of the band
static std::vector<cf32> makeSignal(unsigned seed, std::vector<char>& band) {
    std::mt19937 g(seed);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> S(kLen);
    band.assign(kLen, 0);
    for (int k = 0; k < kLen; k++) {
        const double f = (k < kLen / 2 ? k : k - kLen) * kFs / kLen;
        if (std::fabs(f - kOff) < kBw / 2) { S[k] = cf32(nd(g), nd(g)); band[k] = 1; }
    }
    Fft fft(kLen);
    fft.inverse(S.data());
    double p = 0; for (auto& v : S) p += std::norm(v);
    const float sc = (float)std::sqrt(kLen / p);
    for (auto& v : S) v *= sc;
    return S;
}

// signal delayed by tau samples (may be fractional, exact for the periodic band-limited signal) via the frequency domain
static std::vector<cf32> delayed(const std::vector<cf32>& s, double tau) {
    std::vector<cf32> S = s;
    Fft fft(kLen);
    fft.forward(S.data());
    for (int k = 0; k < kLen; k++) {
        const int kk = k < kLen / 2 ? k : k - kLen;
        const double ph = -2 * kPi * kk * tau / kLen;
        S[k] *= cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    fft.inverse(S.data());
    for (auto& v : S) v /= (float)kLen;
    return S;
}

// power of x inside the band (sum over the band's bins of |FFT|^2)
static double bandPower(std::vector<cf32> x, const std::vector<char>& band) {
    Fft fft(kLen);
    fft.forward(x.data());
    double p = 0;
    for (int k = 0; k < kLen; k++) if (band[k]) p += std::norm(x[k]);
    return p;
}

struct Case {
    const char* name;
    double snrA, snrB;       // dB, in the signal's band (noise is white over the whole 250 kHz)
    double cfoHz, delay, driftPpm;
    double phaseRad, gainB;
};

// noise sigma per complex sample so that the SNR inside the band (bandwidth fraction kBw/kFs of the noise) is snrDb
static double sigmaFor(double snrDb) { return std::sqrt(std::pow(10.0, -snrDb / 10.0) * (kFs / kBw)); }

struct Result { double inA, inB, out; int sel; DiversityStatus st; bool ok; };

static Result run(const Case& c, DiversityConfig::Mode mode, bool deadA = false, unsigned seed = 7) {
    std::vector<char> band;
    const std::vector<cf32> s = makeSignal(seed, band);
    std::vector<cf32> sb = delayed(s, c.delay);
    std::mt19937 g(seed + 1000);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    const double sa = sigmaFor(c.snrA) / std::sqrt(2.0), sbn = sigmaFor(c.snrB) / std::sqrt(2.0);
    std::vector<cf32> a(kLen), b(kLen), na(kLen), nb(kLen);
    const double drift = c.driftPpm * 1e-6;
    for (int i = 0; i < kLen; i++) {
        na[i] = cf32((float)(sa * nd(g)), (float)(sa * nd(g)));
        nb[i] = cf32((float)(sbn * nd(g)), (float)(sbn * nd(g)));
        a[i] = (deadA ? cf32() : s[i]) + na[i];
        // branch B: sampling phase drifting (the signal sampled at i*(1+drift), linear interpolation of the oversampled signal)
        double t = i * (1.0 + drift); int i0 = (int)std::floor(t); double fr = t - i0;
        cf32 v = sb[((i0 % kLen) + kLen) % kLen] * (float)(1 - fr) + sb[(((i0 + 1) % kLen) + kLen) % kLen] * (float)fr;
        const double ph = 2 * kPi * c.cfoHz * i / kFs + c.phaseRad;
        b[i] = (v * (float)c.gainB) * cf32((float)std::cos(ph), (float)std::sin(ph)) + nb[i] * (float)1.0;
    }
    Result r{};
    r.inA = 10 * std::log10(bandPower(s, band) / bandPower(na, band));
    r.inB = 10 * std::log10(bandPower(sb, band) * c.gainB * c.gainB / bandPower(nb, band));
    DiversityCombiner dc;
    DiversityConfig cfg; cfg.rateHz = kFs; cfg.mode = mode;
    r.ok = dc.configure(cfg);
    std::vector<cf32> y;
    for (int i = 0; i < kLen; i += 4096) { const size_t n = std::min<size_t>(4096, kLen - i); dc.push(a.data() + i, b.data() + i, n); dc.pull(y); }
    r.st = dc.status();
    // the output is aligned with branch A: y[i] ~ s[i] times a constant. Fit the constant and measure the in-band error on the second half
    // (the first part is spent acquiring); both over whole-record-periodic windows of the same length is not possible, so work on the half.
    const int h = kLen / 2, n = (int)std::min<size_t>(y.size(), (size_t)kLen) - h;
    if (n < kLen / 16) { r.out = -99; return r; }
    cf32 num{0, 0}; double den = 0;
    for (int i = 0; i < n; i++) { num += y[h + i] * std::conj(s[h + i]); den += std::norm(s[h + i]); }
    const cf32 al = num / (float)den;
    // error and signal restricted to the band: filter the half-record by an FFT of length n (a power of two) on the band of that size
    int m = 1; while (m * 2 <= n) m *= 2;
    std::vector<cf32> e(m), sg(m);
    for (int i = 0; i < m; i++) { e[i] = y[h + i] - al * s[h + i]; sg[i] = al * s[h + i]; }
    Fft f2(m);
    f2.forward(e.data()); f2.forward(sg.data());
    double pe = 0, ps = 0;
    for (int k = 0; k < m; k++) {
        const double f = (k < m / 2 ? k : k - m) * kFs / m;
        if (std::fabs(f - kOff) < kBw / 2 - 500) { pe += std::norm(e[k]); ps += std::norm(sg[k]); }
    }
    r.out = 10 * std::log10(ps / pe);
    r.sel = r.st.selected;
    return r;
}

int main(int argc, char** argv) {
    const int only = argc > 1 ? atoi(argv[1]) : -1;
    // the cases: equal branches, unequal, offset/delay/drift/phase, off-centre signal (kOff), weak signals
    const Case cases[] = {
        {"equal 0 dB, no impairments", 0, 0, 0, 0, 0, 0, 1.0},
        {"equal 0 dB, delay 7.3, cfo 1.3 kHz", 0, 0, 1300, 7.3, 0, 1.1, 1.0},
        {"equal 0 dB, delay -12.6, cfo -4.2 kHz, phase", 0, 0, -4200, -12.6, 0, -2.5, 0.8},
        {"equal -3 dB, all impairments + drift 15 ppm", -3, -3, 2700, 5.4, 15, 0.7, 1.2},
        {"unequal 0 / -6 dB", 0, -6, -900, 3.1, 0, 2.0, 1.0},
        {"unequal -6 / 1 dB (B better)", -6, 1, 6100, -9.7, 0, -1.0, 0.9},
        {"equal 6 dB, cfo 12 kHz", 6, 6, 12000, 20.4, 0, 0.3, 1.0},
        {"unequal -3 / 6 dB (B better)", -3, 6, 6100, -9.7, 0, -1.0, 0.9},
        {"unequal -6 / 6 dB (B much better)", -6, 6, 6100, -9.7, 0, -1.0, 0.9},
        {"unequal 6 / -3 dB", 6, -3, 6100, -9.7, 0, -1.0, 0.9},
    };
    int ci = -1;
    for (const Case& c : cases) {
        if (only >= 0 && ++ci != only) continue;
        if (only < 0) ++ci;
        const Result m = run(c, DiversityConfig::Mrc), s = run(c, DiversityConfig::Selection);
        const double best = std::max(m.inA, m.inB);
        printf("%-48s in A %5.1f B %5.1f dB | MRC %5.1f (%+.1f vs best) lock=%d cfo=%.0f Hz lag=%d | Sel %5.1f (%+.1f)\n", c.name, m.inA, m.inB,
               m.out, m.out - best, m.st.locked, m.st.cfoHz, m.st.lagSamples, s.out, s.out - best);
        CHECK(m.ok, "configure");
        CHECK(m.st.locked, "%s: MRC locks", c.name);
        CHECK(std::fabs(m.st.cfoHz - c.cfoHz) < 60.0, "%s: cfo estimate %.0f vs %.0f", c.name, m.st.cfoHz, c.cfoHz);
        // MRC: never worse than the better branch (0.5 dB estimation loss allowed), and about the sum of the two branch SNRs in power
        const double want = 10 * std::log10(std::pow(10, m.inA / 10) + std::pow(10, m.inB / 10));
        const bool weak = std::min(m.inA, m.inB) <= -3.5;   // a branch below -3 dB per bin: the phase estimate is noisy, so the tolerances are wider
        CHECK(m.out > best - (weak ? 1.0 : 0.5), "%s: MRC %.2f not below best branch %.2f", c.name, m.out, best);
        CHECK(m.out > want - (weak ? 1.7 : 1.2), "%s: MRC %.2f within %.1f dB of the ideal %.2f", c.name, m.out, weak ? 1.7 : 1.2, want);
        CHECK(s.out > best - (weak ? 1.7 : 1.0), "%s: selection %.2f not far below best branch %.2f", c.name, s.out, best);
        CHECK(m.out >= s.out - 0.3, "%s: MRC (%.2f) at least selection (%.2f)", c.name, m.out, s.out);
        if (std::fabs(m.inA - m.inB) < 0.5 && !weak) CHECK(m.out - best > 2.2, "%s: equal branches gain about 3 dB, got %.2f", c.name, m.out - best);
        if (std::fabs(m.inA - m.inB) < 0.5) CHECK(m.out - best > 1.0, "%s: equal branches gain, got %.2f", c.name, m.out - best);
    }

    // other noise / signal realisations: the same tolerances (the algorithm is not tuned to one seed)
    {
        const int pick[] = {1, 3, 4};
        for (unsigned seed : {21u, 22u, 23u}) for (int pi : pick) {
            const Case& c = cases[pi];
            const Result m = run(c, DiversityConfig::Mrc, false, seed);
            const double best = std::max(m.inA, m.inB), want = 10 * std::log10(std::pow(10, m.inA / 10) + std::pow(10, m.inB / 10));
            printf("seed %u %-40s MRC %5.1f (ideal %5.1f)\n", seed, c.name, m.out, want);
            CHECK(m.st.locked && m.out > best - 0.5 && m.out > want - 1.5, "seed %u %s: MRC %.2f ideal %.2f", seed, c.name, m.out, want);
        }
    }

    // the result does not depend on how the samples are cut into pushes
    {
        std::vector<char> band; const std::vector<cf32> s = makeSignal(3, band), sb = delayed(s, 4.0);
        std::mt19937 g(9); dect2::normal_distribution<float> nd(0.f, 3.f);
        std::vector<cf32> a(60000), b(60000);
        for (int i = 0; i < 60000; i++) {
            const double ph = 2 * kPi * 800.0 * i / kFs;
            a[i] = s[i] + cf32(nd(g), nd(g)); b[i] = sb[i] * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(nd(g), nd(g));
        }
        std::vector<cf32> y1, y2;
        DiversityCombiner d1, d2; DiversityConfig cfg; cfg.rateHz = kFs; d1.configure(cfg); d2.configure(cfg);
        for (size_t i = 0; i < a.size(); i += 4096) { const size_t n = std::min<size_t>(4096, a.size() - i); d1.push(a.data() + i, b.data() + i, n); d1.pull(y1); }
        for (size_t i = 0; i < a.size(); i += 777) { const size_t n = std::min<size_t>(777, a.size() - i); d2.push(a.data() + i, b.data() + i, n); d2.pull(y2); }
        CHECK(y1.size() == y2.size(), "same output length for any chunking (%zu vs %zu)", y1.size(), y2.size());
        double e = 0, p = 0;
        for (size_t i = 0; i < std::min(y1.size(), y2.size()); i++) { e += std::norm(y1[i] - y2[i]); p += std::norm(y1[i]); }
        CHECK(e < 1e-6 * p, "same output for any chunking (rel err %.2e)", e / p);
        // reset forgets the alignment
        d1.reset(); CHECK(!d1.status().locked && d1.status().frames == 0, "reset");
    }

    // coarse alignment at the start: two radios whose first samples are tens of milliseconds apart, with a frequency offset
    {
        const int P = 1 << 17;
        const struct { int skew; double cfo; double snr; } as[] = {{5000, 3300, 0}, {-12000, -7200, 0}, {20, 150, 3}, {9000, 1000, -1.5}, {9000, 1000, -3}};
        for (const auto& c : as) {
            std::vector<char> band; const std::vector<cf32> s = makeSignal(31, band);
            std::mt19937 g(77); dect2::normal_distribution<float> nd(0.f, 1.f);
            const double sg = sigmaFor(c.snr) / std::sqrt(2.0);
            // A starts at sample 20000, B starts `skew` samples earlier or later: B[n] = A content delayed by skew
            std::vector<cf32> a(P), b(P);
            for (int i = 0; i < P; i++) {
                a[i] = s[20000 + i] + cf32((float)(sg * nd(g)), (float)(sg * nd(g)));
                const int j = 20000 + i - c.skew;
                const double ph = 2 * kPi * c.cfo * i / kFs;
                b[i] = s[((j % kLen) + kLen) % kLen] * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32((float)(sg * nd(g)), (float)(sg * nd(g)));
            }
            const DiversityAlign al = diversityCoarseAlign(a.data(), b.data(), P, kFs, 40e3, 25000);
            printf("coarse align skew %6d cfo %6.0f snr %4.1f: ok=%d skew=%d cfo=%.0f conf=%.1f\n", c.skew, c.cfo, c.snr, al.ok, al.skewSamples, al.cfoHz, al.confidence);
            if (c.snr <= -2.5) {   // too weak for the coarse step: it must say so rather than guess wrong (the combiner then starts without it)
                CHECK(!al.ok || (std::abs(al.skewSamples - c.skew) <= 12), "weak: refused or right");
                continue;
            }
            CHECK(al.ok, "coarse align finds the signal (conf %.1f)", al.confidence);
            CHECK(std::abs(al.skewSamples - c.skew) <= 12, "skew %d vs %d", al.skewSamples, c.skew);
            CHECK(std::fabs(al.cfoHz - c.cfo) < 3 * kFs / P + 20, "cfo %.0f vs %.0f", al.cfoHz, c.cfo);
        }
        // nothing in common: refused
        std::mt19937 g(3); dect2::normal_distribution<float> nd(0.f, 1.f);
        std::vector<cf32> a(P), b(P);
        for (auto& v : a) v = cf32(nd(g), nd(g));
        for (auto& v : b) v = cf32(nd(g), nd(g));
        CHECK(!diversityCoarseAlign(a.data(), b.data(), P, kFs, 40e3, 25000).ok, "independent noise: no alignment");
        CHECK(!diversityCoarseAlign(a.data(), b.data(), 1000, kFs, 40e3, 100).ok, "too short is refused");
    }

    // a dead antenna on A (only noise): selection takes B; MRC must not be worse than A's noise and must not crash
    {
        Case c{"A dead, B 3 dB", 0, 3, 800, 4.0, 0, 0.5, 1.0};
        const Result s = run(c, DiversityConfig::Selection, true);
        printf("%-48s Sel status: selected=%d\n", c.name, s.st.selected);
        CHECK(s.st.selected == 1, "dead A: selection picks B (selected=%d)", s.st.selected);
        const Result m = run(c, DiversityConfig::Mrc, true);
        CHECK(m.ok, "dead A: mrc runs");
    }

    // no common signal: two independent noises -> not locked, output is branch A (no gain, no harm)
    {
        std::mt19937 g(5); dect2::normal_distribution<float> nd(0.f, 1.f);
        std::vector<cf32> a(100000), b(100000), y;
        for (auto& v : a) v = cf32(nd(g), nd(g));
        for (auto& v : b) v = cf32(nd(g), nd(g));
        DiversityCombiner dc; DiversityConfig cfg; cfg.rateHz = kFs;
        dc.configure(cfg);
        for (size_t i = 0; i < a.size(); i += 5000) { dc.push(a.data() + i, b.data() + i, 5000); dc.pull(y); }
        printf("noise-only: locked=%d frames=%llu bins=%d\n", dc.status().locked, (unsigned long long)dc.status().frames, dc.status().bins);
        CHECK(!dc.status().locked, "independent noise does not lock");
        CHECK(y.size() > 80000, "output produced (%zu)", y.size());
        double e = 0, p = 0;
        for (size_t i = 20000; i < y.size() && i < a.size(); i++) { e += std::norm(y[i] - a[i]); p += std::norm(a[i]); }
        CHECK(e / p < 1e-4, "unlocked passes branch A unchanged (err %.2e)", e / p);
    }

    // identical branches (same radio twice, or a splitter): output equals the input signal, gain 3 dB on noise that is not shared
    {
        Case c{"identical signal, independent noise", 3, 3, 0, 0, 0, 0, 1.0};
        const Result m = run(c, DiversityConfig::Mrc);
        CHECK(m.st.locked && m.out > c.snrA + 2.2, "identical branches combine (%.2f)", m.out);
    }

    // bad configurations
    {
        DiversityCombiner dc; DiversityConfig bad; bad.rateHz = 100; CHECK(!dc.configure(bad), "rate too low is refused");
        bad.rateHz = 250e3; bad.fftSize = 1000; CHECK(!dc.configure(bad), "fft size must be a power of two");
        bad.fftSize = 0; CHECK(dc.configure(bad) && dc.fftSize() == 512, "default fft size at 250 ksps is 512 (%d)", dc.fftSize());
        std::vector<cf32> y; dc.push(nullptr, nullptr, 0); CHECK(dc.pull(y) == 0, "empty push");
    }

    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
