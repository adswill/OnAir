// Voice inversion descrambler (voice_inversion.h): frequency mapping, invert twice = identity, split-band round trip, the auto-search on
// speech-like signals across noise and levels, thread safety, near-zero CPU while off.
#include "dect2/audio/voice_inversion.h"
#include "dect2/common/stable_random.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
#include "../common/cpu_report.h"

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = 3.14159265358979323846;
static const int kRate = 48000;

// voiced bursts (pitch ~130 Hz with vibrato, telephone band 300 .. top, a moving first and second formant, syllable envelope 4 Hz, 0.6 s on / 0.4 s off)
static std::vector<float> speechLike(double seconds, double amp, double top = 3000, double pitch = 130) {
    const size_t n = (size_t)(kRate * seconds);
    std::vector<float> c(n, 0.f);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / kRate;
        if (std::fmod(t, 1.0) >= 0.6) continue;
        const double env = 0.5 * (1 - std::cos(2 * kPi * 4 * t));
        const double f0 = pitch + 15 * std::sin(2 * kPi * 2 * t);
        const double fm1 = 500 + 150 * std::sin(2 * kPi * 0.7 * t), fm2 = 1500 + 300 * std::sin(2 * kPi * 0.45 * t);
        ph += 2 * kPi * f0 / kRate;
        double v = 0;
        for (int h = 1; h * f0 < top; h++) {
            const double f = h * f0;
            if (f < 300) continue;
            const double a = (0.15 + std::exp(-std::pow((f - fm1) / 200, 2)) + 0.5 * std::exp(-std::pow((f - fm2) / 300, 2))) / (1 + f / 700);
            v += a * std::sin(h * ph);
        }
        c[i] = (float)(amp * env * v);
    }
    return c;
}
static void addWhite(std::vector<float>& x, double sigma, unsigned seed) {
    std::mt19937 g(seed); dect2::normal_distribution<float> nd(0.f, (float)sigma);
    for (auto& v : x) v += nd(g);
}
static double rms(const std::vector<float>& x, size_t from = 0) { double s = 0; for (size_t i = from; i < x.size(); i++) s += (double)x[i] * x[i]; return std::sqrt(s / std::max<size_t>(1, x.size() - from)); }
// run a mono signal through an instance (stereo in, left out)
static std::vector<float> run(VoiceInversion& v, const std::vector<float>& x, int chunk = 480) {
    std::vector<float> y(x.size()), in(2 * chunk), sc(2 * VoiceInversion::kMaxChunk);
    for (size_t i = 0; i < x.size(); i += chunk) {
        const int m = (int)std::min<size_t>(chunk, x.size() - i);
        for (int j = 0; j < m; j++) in[2 * j] = in[2 * j + 1] = x[i + j];
        const float* o = v.process(in.data(), m, sc.data());
        for (int j = 0; j < m; j++) y[i + j] = o[2 * j];
    }
    return y;
}
// scrambles / descrambles with a private config
struct Inv {
    VoiceInversionConfig cfg; VoiceInversion v;
    Inv(const VoiceInversionSettings& s) { cfg.set(s); v.setConfig(&cfg); v.prepare(kRate); }
};
static std::vector<float> through(const std::vector<float>& x, VoiceInversionSettings s) { s.on = true; Inv i(s); return run(i.v, x); }
static double tone(const std::vector<float>& x, double f, size_t from) {   // Goertzel-like single bin amplitude
    double re = 0, im = 0; const size_t n = x.size() - from;
    for (size_t i = 0; i < n; i++) { const double w = 2 * kPi * f * i / kRate; re += x[from + i] * std::cos(w); im += x[from + i] * std::sin(w); }
    return 2 * std::hypot(re, im) / n;
}
static std::vector<float> sine(double f, double amp, double seconds) {
    std::vector<float> x((size_t)(kRate * seconds));
    for (size_t i = 0; i < x.size(); i++) x[i] = (float)(amp * std::sin(2 * kPi * f * i / kRate));
    return x;
}
// level of band [a, b) of a signal in dB (Welch-free: sum of tone bins every 25 Hz)
static double bandDb(const std::vector<float>& x, double a, double b, size_t from) {
    double e = 0;
    for (double f = a; f < b; f += 25) { const double t = tone(x, f, from); e += t * t; }
    return 10 * std::log10(e + 1e-20);
}

int main() {
    VoiceInversionSettings S;
    // 1. off: the input itself, no work
    {
        VoiceInversionConfig cfg; VoiceInversion v; v.setConfig(&cfg); v.prepare(kRate);
        std::vector<float> in(2 * 480, 0.3f), sc(2 * 1024); bool act = true;
        CHECK(v.process(in.data(), 480, sc.data(), &act) == in.data() && !act, "off returns the input");
    }
    // 2. frequency mapping: simple 1000 Hz -> 2000 Hz at carrier 3000; split-band 600 -> 1400 and 2000 -> 2700 (lo 300, split 1700, hi 3000)
    {
        S.mode = VoiceInvMode::Simple; S.carrierHz = 3000;
        const auto y = through(sine(1000, 0.5, 1.0), S);
        CHECK(tone(y, 2000, 24000) > 0.4 && tone(y, 2000, 24000) < 0.6 && tone(y, 1000, 24000) < 0.03, "simple 1000->2000: %.3f / %.3f", tone(y, 2000, 24000), tone(y, 1000, 24000));
        S.mode = VoiceInvMode::Split; S.splitHz = 1700; S.loHz = 300; S.hiHz = 3000;
        const auto a = through(sine(600, 0.5, 1.0), S), b = through(sine(2000, 0.5, 1.0), S);
        CHECK(tone(a, 1400, 24000) > 0.35 && tone(a, 600, 24000) < 0.05, "split 600->1400: %.3f / %.3f", tone(a, 1400, 24000), tone(a, 600, 24000));
        CHECK(tone(b, 2700, 24000) > 0.25 && tone(b, 2000, 24000) < 0.05, "split 2000->2700: %.3f / %.3f", tone(b, 2700, 24000), tone(b, 2000, 24000));
    }
    // 3. invert twice = identity (spectrum per band within 1.5 dB, total level), simple at every preset and split-band
    {
        for (int pi = 0; pi < 5; pi++) {
            S.mode = VoiceInvMode::Simple; S.carrierHz = kVoiceInvPresets[pi];
            const double top = 0.8 * S.carrierHz;
            const auto x = speechLike(3.0, 0.2, top);
            const auto z = through(through(x, S), S);
            for (double a = 300; a + 400 < top; a += 400) {
                const double d = bandDb(z, a, a + 400, 24000) - bandDb(x, a, a + 400, 24000);
                CHECK(std::fabs(d) < 1.5, "simple %.0f twice, band %.0f: %.2f dB", S.carrierHz, a, d);
            }
            CHECK(std::fabs(20 * std::log10(rms(z, 24000) / rms(x, 24000))) < 1.0, "simple %.0f level", S.carrierHz);
        }
        // split-band: the two halves meet again at the split, where the crossover and the mirrored edges add with whatever phase: a few dB there
        S.mode = VoiceInvMode::Split; S.splitHz = 1700; S.loHz = 300; S.hiHz = 3000;
        const auto x = speechLike(3.0, 0.2, 2800);
        const auto z = through(through(x, S), S);
        for (double a = 400; a < 2800; a += 600) {
            if (a + 600 > S.splitHz - 150 && a < S.splitHz + 450) continue;   // the band around the split itself
            const double d = bandDb(z, a, a + 600, 24000) - bandDb(x, a, a + 600, 24000);
            CHECK(std::fabs(d) < 5.0, "split twice, band %.0f: %.2f dB", a, d);
        }
    }
    // 4. the search: carriers x noise x levels
    {
        const float carriers[] = {2550, 2700, 3000, 3300, 3400};
        const double sigmas[] = {0.0, 0.01, 0.03}, amps[] = {0.05, 0.3};
        int n = 0;
        for (float c : carriers) for (double sg : sigmas) for (double am : amps) {
            VoiceInversionSettings s; s.mode = VoiceInvMode::Simple; s.carrierHz = c;
            auto sc = through(speechLike(4.0, am, 0.8 * c, 120 + 10 * (n % 4)), s);
            addWhite(sc, sg * am / 0.3, 100 + n);
            n++;
            VoiceInversionSettings base;
            const auto r = voiceInversionSearch(sc.data(), (int)sc.size(), kRate, VoiceInvMode::Simple, base);
            CHECK(r.found && std::fabs(r.value - c) <= 150, "search carrier %.0f noise %.3f amp %.2f: found=%d value %.0f score %.3f base %.3f", c, sg, am, r.found, r.value, r.score, r.baseline);
        }
        // split-band
        for (float sp : {1300.f, 1700.f, 2000.f}) {
            VoiceInversionSettings s; s.mode = VoiceInvMode::Split; s.splitHz = sp; s.loHz = 300; s.hiHz = 3000;
            auto sc = through(speechLike(4.0, 0.2, 2900), s);
            addWhite(sc, 0.005, 7);
            const auto r = voiceInversionSearch(sc.data(), (int)sc.size(), kRate, VoiceInvMode::Split, s);
            CHECK(r.found && std::fabs(r.value - sp) <= 200, "search split %.0f: found=%d value %.0f score %.3f base %.3f", sp, r.found, r.value, r.score, r.baseline);
        }
        // plain noise and silence: nothing found
        std::vector<float> nz(4 * kRate, 0.f);
        addWhite(nz, 0.1, 5);
        VoiceInversionSettings base;
        CHECK(!voiceInversionSearch(nz.data(), (int)nz.size(), kRate, VoiceInvMode::Simple, base).found, "noise is not speech");
        std::vector<float> sil(4 * kRate, 0.f);
        CHECK(!voiceInversionSearch(sil.data(), (int)sil.size(), kRate, VoiceInvMode::Simple, base).found, "silence is not speech");
        // untouched speech scores above its inversion
        const auto clean = speechLike(4.0, 0.3, 2800);
        S.mode = VoiceInvMode::Simple; S.carrierHz = 3000;
        const auto inv = through(clean, S);
        CHECK(voiceSpeechScore(clean.data(), (int)clean.size(), kRate) > voiceSpeechScore(inv.data(), (int)inv.size(), kRate), "clean speech scores above inverted");
    }
    // 5. enabling fades in without a click
    {
        VoiceInversionConfig cfg; VoiceInversion v; v.setConfig(&cfg); v.prepare(kRate);
        const auto x = sine(500, 0.3, 0.5);
        run(v, std::vector<float>(2400, 0.f));
        cfg.on = true;
        const auto y = run(v, x);
        float mx = 0; for (size_t i = 1; i < y.size(); i++) mx = std::max(mx, std::fabs(y[i] - y[i - 1]));
        CHECK(mx < 0.25f, "click on enable: step %.3f", mx);
        cfg.on = false;
        const auto y2 = run(v, x);
        mx = 0; for (size_t i = 1; i < y2.size(); i++) mx = std::max(mx, std::fabs(y2[i] - y2[i - 1]));
        CHECK(mx < 0.25f, "click on disable: step %.3f", mx);
        CHECK(!std::isnan(y2.back()), "finite");
    }
    // 6. thread safety: settings and capture reads from other threads while the audio thread runs
    {
        VoiceInversionConfig cfg; VoiceInversion v; v.setConfig(&cfg); v.prepare(kRate);
        std::atomic<bool> stop{false};
        std::thread w([&] { int i = 0; while (!stop) { VoiceInversionSettings s; s.on = (i / 7) % 3 != 0; s.mode = (i & 1) ? VoiceInvMode::Split : VoiceInvMode::Simple; s.carrierHz = 2500.f + (i % 11) * 90; s.splitHz = 1000.f + (i % 13) * 90; cfg.set(s); cfg.capture = (i & 2); i++; } });
        std::thread r([&] { std::vector<float> o; while (!stop) cfg.snapshot(o, 2.0); });
        const auto x = speechLike(6.0, 0.3, 2800);
        bool finite = true;
        const auto y = run(v, x);
        for (float f : y) if (!std::isfinite(f) || std::fabs(f) > 20) { finite = false; break; }
        stop = true; w.join(); r.join();
        CHECK(finite, "output stays finite while the settings change");
    }
    // 7. CPU: off is near zero, on is small (seconds of audio processed per second of CPU)
    {
        const auto x = speechLike(10.0, 0.3, 2800);
        VoiceInversionConfig c1, c2; VoiceInversion off, on; off.setConfig(&c1); on.setConfig(&c2); off.prepare(kRate); on.prepare(kRate);
        VoiceInversionSettings s; s.on = true; s.mode = VoiceInvMode::Split; c2.set(s);
        auto t0 = std::chrono::steady_clock::now(); run(off, x);
        auto t1 = std::chrono::steady_clock::now(); run(on, x);
        auto t2 = std::chrono::steady_clock::now();
        const double toff = std::chrono::duration<double>(t1 - t0).count() / 10.0, ton = std::chrono::duration<double>(t2 - t1).count() / 10.0;
        printf("CPU per second of audio: off %.5f s (includes the test's own copying), on (split-band) %.4f s\n", toff, ton);
        cpuCal("voice_inversion", "off", toff, "wall-s/s", '<', 0.003);
        CHECK(toff < 0.003, "off costs %.5f", toff);
        cpuCal("voice_inversion", "on (split-band)", ton, "wall-s/s", '<', 0.05);
        CHECK(ton < 0.05, "on costs %.4f", ton);
    }
    printf(fails ? "FAILED (%d)\n" : "voice inversion: all passed\n", fails);
    return fails ? 1 : 0;
}
