// Synthetic wideband (core/include/dect2/dsp/synth_wide.h): pure helpers, then three narrow "radios" hearing the same many-carrier wideband signal
// through different gain/phase, sample delay, sample clock error (ppm) and frequency error are stitched into one wide stream; checks that the errors
// are measured (offset Hz, drift ppm, overlap mismatch dB), that every carrier comes out at its true level across the seams, and that a radio that
// drops out and returns is handled. Synthetic only: no hardware, no real free-running phase noise (see the header on coherence).
#include "dect2/dsp/synth_wide.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::synth;
#include "../common/test_util.h"

static const double kPi = 3.14159265358979323846;

static void testHelpers() {
    // fade is complementary; sqrtHann^2 at hop n/2 sums to 1
    for (int i = 0; i < 50; i++) CHECK(std::fabs(fade(i, 50) + fade(49 - i, 50) - 1.f) < 1e-5, "fade not complementary at %d", i);
    const auto w = sqrtHann(64);
    for (int i = 0; i < 32; i++) CHECK(std::fabs(w[i] * w[i] + w[i + 32] * w[i + 32] - 1.f) < 1e-5, "sqrtHann power sum at %d", i);
    // regression
    std::vector<double> t, y;
    for (int i = 0; i < 10; i++) { t.push_back(i * 0.1); y.push_back(3.0 + 2.5 * i * 0.1); }
    CHECK(std::fabs(regressSlope(t, y) - 2.5) < 1e-9, "slope");
    // shift of a power pattern: pa(w) = pb(w - 7)
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    std::vector<float> pb(300), pa(300);
    for (auto& v : pb) v = U(rng);
    for (int w2 = 0; w2 < 300; w2++) pa[w2] = (w2 - 7 >= 0) ? pb[w2 - 7] : 0.5f;
    float conf = 0;
    CHECK(estimateShiftBins(pa, pb, 20, &conf) == 7 && conf > 3, "shift estimate %d conf %.1f", estimateShiftBins(pa, pb, 20), conf);
    // delay + phase: B(u) = A(u) e^{-j(phi + 2 pi tau u / n)}  =>  cross = A conj B has phase phi + 2 pi tau u / n
    const int n = 1024, W = 300;
    const double tau = -3.37, phi = 0.9;
    std::vector<cf32> cross(W);
    for (int u = 0; u < W; u++) { const double a = phi + 2 * kPi * tau * u / n; cross[u] = std::polar(1.0f + 0.3f * U(rng), (float)a); }
    DelayPhase dp = estimateDelay(cross, n);
    CHECK(std::fabs(dp.delay - tau) < 0.02, "delay %.3f vs %.3f", dp.delay, tau);
    CHECK(std::fabs(std::remainder(dp.phase0 - phi, 2 * kPi)) < 0.05, "phase %.3f vs %.3f", dp.phase0, phi);
    CHECK(dp.strength > 0.9, "strength %.2f", dp.strength);
    // mismatch of a perfect copy is very low
    std::vector<float> pa2(W), pb2(W);
    for (int u = 0; u < W; u++) { pa2[u] = std::abs(cross[u]); pb2[u] = pa2[u]; }
    CHECK(mismatchDb(cross, pa2, pb2, n, dp, 1.0) < -30, "mismatch %.1f", mismatchDb(cross, pa2, pb2, n, dp, 1.0));
    // fine offset: per-hop sums rotate by 2 pi f hop
    std::vector<cf32> hops;
    for (int k = 0; k < 20; k++) hops.push_back(std::polar(1.0f, (float)(2 * kPi * 123.0 * 0.001 * k)));
    CHECK(std::fabs(fineOffsetHz(hops, 0.001) - 123.0) < 0.01, "fine offset %.3f", fineOffsetHz(hops, 0.001));
    // resampler: ppm changes the length, off mixes a tone to DC
    FracResampler rs;
    rs.configure(1e6);
    rs.set(100.0, -50000.0);
    std::vector<cf32> in(100000), out;
    for (size_t i = 0; i < in.size(); i++) in[i] = std::polar(1.0f, (float)std::fmod(2 * kPi * 50000.0 * i / 1e6, 2 * kPi));
    rs.process(in.data(), in.size(), out);
    CHECK(std::fabs((double)out.size() / in.size() - 1.0 / 1.0001) < 2e-4, "resampled length %zu", out.size());
    double ph = 0;
    for (size_t i = 1000; i + 1 < out.size(); i++) ph += std::arg(out[i + 1] * std::conj(out[i]));
    CHECK(std::fabs(ph / (out.size() - 1001)) < 0.01, "mixed tone not at DC: %.4f rad/sample", ph / (out.size() - 1001));
}

// ---- the multi-radio scenario ----
struct Tone { double f, a, ph; };

struct Radio {
    double fc, off, q, d;     // LO centre (Hz from the wide centre), frequency error (Hz), clock error (ppm as a ratio 1e-6), delay (samples)
    std::complex<double> g;   // gain and phase
    std::vector<std::complex<double>> z, rot;
    std::vector<int> idx;
    long long n = 0;
    double lwHz = 0, wph = 0;   // free-running LO phase noise: Wiener phase of this Lorentzian linewidth (0 = none)
    double fsHz = 1;
    void init(const std::vector<Tone>& tones, double fs) {
        fsHz = fs;
        for (size_t m = 0; m < tones.size(); m++) {
            const double fbb = tones[m].f - fc - off;
            if (std::fabs(fbb) > 0.48 * fs) continue;
            idx.push_back((int)m);
            z.push_back(std::polar(tones[m].a, 2 * kPi * fbb * d / fs + tones[m].ph) * g);
            rot.push_back(std::polar(1.0, 2 * kPi * fbb * (1 + q) / fs));
        }
    }
    void block(std::vector<cf32>& o, size_t len, std::mt19937& rng) {
        std::normal_distribution<float> nd(0.f, 0.003f);
        o.assign(len, cf32(0, 0));
        std::vector<std::complex<double>> acc(len, 0.0);
        for (size_t k = 0; k < z.size(); k++) {
            std::complex<double> zz = z[k]; const std::complex<double> r = rot[k];
            for (size_t i = 0; i < len; i++) { zz *= r; acc[i] += zz; }
            z[k] = zz / std::abs(zz) * std::abs(z[k]);
        }
        std::normal_distribution<double> pn(0.0, 1.0);
        const double ws = std::sqrt(2 * kPi * lwHz / fsHz);
        for (size_t i = 0; i < len; i++) {
            std::complex<double> v = acc[i];
            if (lwHz > 0) { wph += ws * pn(rng); v *= std::polar(1.0, wph); }
            o[i] = cf32((float)v.real() + nd(rng), (float)v.imag() + nd(rng));
        }
        n += (long long)len;
    }
};

static void testScenario() {
    SynthCfg c;
    c.slices = 3; c.fs = 1e6; c.n = 1024; c.stepBins = 600; c.alignFrames = 24;
    SynthWideband sw;
    CHECK(sw.configure(c), "configure");
    const double df = c.fs / c.n, rate = sw.outRate();
    CHECK(std::fabs(rate - 2.048e6 * 1e6 / 1.024e6 / 1.0) < 1e9 && rate > 1.9e6, "rate %.0f", rate);
    // the carriers
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<Tone> tones;
    const double lo = -0.5 * sw.spanHz() + 20e3, hi = 0.5 * sw.spanHz() - 20e3;
    for (double f = lo; f < hi; f += (5.2 + 2.0 * U(rng)) * df) tones.push_back({f, 0.006 + 0.008 * U(rng), 2 * kPi * U(rng)});
    Radio rad[3];
    const double stepHz = c.stepBins * df;
    const double off[3] = {0.0, 4500.0, -6200.0}, q[3] = {0, 6e-6, -9e-6}, d[3] = {0, 37.3, -11.6};
    const std::complex<double> g[3] = {1.0, std::polar(0.7, 1.1), std::polar(1.4, -2.0)};
    for (int i = 0; i < 3; i++) { rad[i].fc = (i - 1) * stepHz; rad[i].off = off[i]; rad[i].q = q[i]; rad[i].d = d[i]; rad[i].g = g[i]; rad[i].init(tones, c.fs); }
    std::vector<cf32> blk, wide;
    const size_t B = 4096;
    auto run = [&](double seconds, bool dropMid) {
        const size_t blocks = (size_t)(seconds * c.fs / B);
        for (size_t b = 0; b < blocks; b++)
            for (int i = 0; i < 3; i++) {
                rad[i].block(blk, B, rng);
                if (dropMid && i == 1) continue;   // the radio is gone (its clock goes on)
                sw.push(i, blk.data(), B);
            }
    };
    run(1.6, false);
    sw.pull(wide);
    for (int p = 0; p < 2; p++) {
        const PairStatus ps = sw.pair(p);
        CHECK(ps.valid && ps.mismatchDb < -15.0, "pair %d mismatch %.1f dB valid %d", p, ps.mismatchDb, (int)ps.valid);
        printf("pair %d: mismatch %.1f dB gain %.2f dB delay %.2f conf %.2f\n", p, ps.mismatchDb, ps.gainDb, ps.delaySamples, ps.confidence);
    }
    for (int i = 1; i < 3; i++) {
        const SliceStatus s = sw.slice(i);
        printf("slice %d: offset %.1f Hz (true %.1f) drift %.2f ppm (true %.1f) locked %d\n", i, s.offsetHz, off[i], s.driftPpm, -q[i] * 1e6, (int)s.locked);
        CHECK(std::fabs(s.offsetHz - off[i]) < 40.0, "slice %d offset %.1f vs %.1f", i, s.offsetHz, off[i]);
        CHECK(std::fabs(s.driftPpm - (-q[i] * 1e6)) < 2.0, "slice %d drift %.2f vs %.2f", i, s.driftPpm, -q[i] * 1e6);
        CHECK(s.locked, "slice %d not locked", i);
    }
    // carrier levels in the last 0.2 s of the wide stream
    auto levels = [&](const std::vector<cf32>& x, size_t from, std::vector<double>& ratioDb) {
        const int L = 2048;
        Fft f(L);
        std::vector<double> pw(L, 0.0);
        int frames = 0;
        for (size_t s = from; s + L <= x.size(); s += L) {
            std::vector<cf32> t(L);
            for (int k = 0; k < L; k++) t[k] = x[s + k] * (float)(0.5 - 0.5 * std::cos(2 * kPi * k / L));
            f.forward(t.data());
            for (int k = 0; k < L; k++) pw[k] += std::norm(t[k]);
            frames++;
        }
        ratioDb.clear();
        if (!frames) return;
        for (auto& v : pw) v /= frames;
        for (const Tone& tn : tones) {
            const int bin = (int)std::lround(tn.f / (rate / L));
            double e = 0;
            for (int k = bin - 2; k <= bin + 2; k++) e += pw[((k % L) + L) % L];
            const double a = std::sqrt(e / ((double)L * 3.0 * L / 8.0));
            ratioDb.push_back(20 * std::log10(a / tn.a));
        }
    };
    std::vector<double> rd;
    levels(wide, wide.size() > 400000 ? wide.size() - 400000 : 0, rd);
    {
        int bad = 0; double worst = 0, mean = 0;
        for (double v : rd) { mean += v; if (std::fabs(v) > 1.5) bad++; worst = std::max(worst, std::fabs(v)); }
        mean /= std::max<size_t>(1, rd.size());
        printf("carriers: %zu, mean level error %.2f dB, worst %.2f dB, %d beyond 1.5 dB\n", rd.size(), mean, worst, bad);
        CHECK(rd.size() > 100 && std::fabs(mean) < 0.7, "mean level error %.2f dB", mean);
        CHECK(bad <= (int)(rd.size() / 20), "%d of %zu carriers off by more than 1.5 dB", bad, rd.size());
    }
    // a radio drops out: the others go on, the pair is invalid, the output keeps coming
    wide.clear();
    sw.setAlive(1, false);
    run(0.4, true);
    sw.pull(wide);
    CHECK(wide.size() > 500000, "output stopped after a dropout: %zu samples", wide.size());
    CHECK(!sw.slice(1).alive && sw.slice(1).dropouts >= 1, "slice 1 should be down");
    CHECK(!sw.pair(0).valid && !sw.pair(1).valid, "pairs should be invalid while a middle radio is down");
    bool finite = true;
    for (auto& v : wide) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) { finite = false; break; }
    CHECK(finite, "non-finite output after the dropout");
    // slice 0's own carriers (left third of the span) are still there
    levels(wide, wide.size() - 200000, rd);
    int ok = 0, tot = 0;
    for (size_t m = 0; m < tones.size(); m++) if (tones[m].f < -0.5 * c.stepBins * df - 0.0 && tones[m].f > lo + 30e3) { tot++; if (std::fabs(rd[m]) < 1.5) ok++; }
    CHECK(tot > 10 && ok >= tot * 9 / 10, "slice 0 carriers during the dropout: %d of %d", ok, tot);
    // it comes back and is aligned again
    wide.clear();
    sw.setAlive(1, true);
    run(1.6, false);
    sw.pull(wide);
    CHECK(sw.slice(1).alive, "slice 1 should be back");
    printf("after return: pair0 %.1f dB pair1 %.1f dB, slice1 offset %.1f Hz drift %.2f ppm\n", sw.pair(0).mismatchDb, sw.pair(1).mismatchDb, sw.slice(1).offsetHz, sw.slice(1).driftPpm);
    CHECK(sw.pair(0).valid && sw.pair(0).mismatchDb < -15.0, "pair 0 after return %.1f dB", sw.pair(0).mismatchDb);
    CHECK(sw.pair(1).valid && sw.pair(1).mismatchDb < -15.0, "pair 1 after return %.1f dB", sw.pair(1).mismatchDb);
    // the display spectrum is there
    const auto sp = sw.spectrumDb();
    CHECK((int)sp.size() == (int)(rate / df), "spectrum size %zu", sp.size());
}

// Five radios with free-running oscillators (30 Hz Wiener linewidth each): the seams with per-hop phase tracking against without
static double phaseNoiseMismatch(bool track, double* rmsDeg) {
    SynthCfg c;
    c.slices = 5; c.fs = 1e6; c.n = 512; c.stepBins = 300; c.alignFrames = 48; c.trackPhase = track;
    SynthWideband sw;
    if (!sw.configure(c)) return 0;
    const double df = c.fs / c.n;
    std::mt19937 rng(23);
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<Tone> tones;
    const double lo = -0.5 * sw.spanHz() + 10e3, hi = 0.5 * sw.spanHz() - 10e3;
    for (double f = lo; f < hi; f += (2.2 + 1.5 * U(rng)) * df) tones.push_back({f, 0.004 + 0.006 * U(rng), 2 * kPi * U(rng)});
    Radio rad[5];
    const double off[5] = {0, 2100, -3300, 900, 4100}, q[5] = {0, 3e-6, -4e-6, 2e-6, -1e-6}, d[5] = {0, 12.2, -7.6, 25.1, 3.3};
    for (int i = 0; i < 5; i++) {
        rad[i].fc = (i - 2) * c.stepBins * df; rad[i].off = off[i]; rad[i].q = q[i]; rad[i].d = d[i]; rad[i].g = std::polar(1.0 + 0.1 * i, 0.5 * i);
        rad[i].lwHz = 30; rad[i].init(tones, c.fs);
    }
    std::vector<cf32> blk, wide;
    const size_t B = 4096;
    double sum = 0; int cnt = 0;
    for (size_t b = 0; b < (size_t)(2.4 * c.fs / B); b++) {
        for (int i = 0; i < 5; i++) { rad[i].block(blk, B, rng); sw.push(i, blk.data(), B); }
        sw.pull(wide); wide.clear();
        if (b * B > 1.4 * c.fs && b % 8 == 0)
            for (int p = 0; p < 4; p++) if (sw.pair(p).valid) { sum += sw.pair(p).mismatchDb; cnt++; }
    }
    if (rmsDeg) *rmsDeg = sw.pair(1).trackRmsDeg;
    return cnt ? sum / cnt : 0;
}

static void testPhaseNoise() {
    double rmsT = 0;
    const double off = phaseNoiseMismatch(false, nullptr), on = phaseNoiseMismatch(true, &rmsT);
    printf("5 radios, 30 Hz linewidth each: mean seam mismatch %.1f dB without phase tracking, %.1f dB with (%.1f deg per hop moved)\n", off, on, rmsT);
    CHECK(on < off - 6.0, "phase tracking: %.1f dB vs %.1f dB without", on, off);
    CHECK(on < -15.0, "tracked seams %.1f dB", on);
}

static void testBadConfig() {
    SynthWideband sw;
    SynthCfg c;
    c.slices = 1; CHECK(!sw.configure(c), "1 slice accepted");
    c = SynthCfg(); c.stepBins = 2401; CHECK(!sw.configure(c), "odd step accepted");
    c = SynthCfg(); c.stepBins = 3600; CHECK(!sw.configure(c), "no overlap accepted");
    c = SynthCfg(); c.n = 3000; CHECK(!sw.configure(c), "non power of 2 accepted");
    c = SynthCfg(); c.slices = 6; CHECK(!sw.configure(c), "6 slices accepted");
    c = SynthCfg(); c.slices = 5; c.stepBins = 2000; CHECK(sw.configure(c), "5 slices refused");
}

int main() {
    testHelpers();
    testBadConfig();
    testScenario();
    testPhaseNoise();
    return testSummary("synth_wide");
}
