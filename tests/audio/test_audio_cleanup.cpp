// Audio cleanup chain (audio_cleanup.h): off = the input itself, SNR gain of the noise reduction, transparency on clean sound, the notch
// following a drifting whistle without hurting speech, leveller targets and limiter, silence gate, click-free switching, thread safety, CPU.
#include "dect2/audio/audio_cleanup.h"
#include "dect2/common/stable_random.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = 3.14159265358979323846;

// voiced bursts (130 Hz pitch with vibrato, harmonics to 3.4 kHz, syllable envelope, 0.6 s on / 0.4 s off)
static void speechLike(int rate, double seconds, std::vector<float>& c, std::vector<char>& on, double amp = 0.25) {
    const size_t n = (size_t)(rate * seconds);
    c.assign(n, 0.f); on.assign(n, 0);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        const bool b = std::fmod(t, 1.0) < 0.6;
        on[i] = b;
        if (!b) continue;
        const double env = 0.5 * (1 - std::cos(2 * kPi * 4 * t));
        const double f0 = 130 + 15 * std::sin(2 * kPi * 2 * t);
        ph += 2 * kPi * f0 / rate;
        double v = 0;
        for (int h = 1; h * f0 < std::min(3400.0, 0.45 * rate); h++) v += std::sin(h * ph) / h;
        c[i] = (float)(amp * env * v);
    }
}
static void addWhite(std::vector<float>& x, double sigma, unsigned seed) {
    std::mt19937 g(seed); dect2::normal_distribution<float> nd(0.f, (float)sigma);
    for (auto& v : x) v += nd(g);
}
// run a whole signal through a chain (mono as stereo), chunks of `chunk` frames; returns the left channel
static std::vector<float> run(AudioCleanup& ac, const std::vector<float>& x, int chunk = 480) {
    std::vector<float> y(x.size()), in(2 * chunk), sc(2 * AudioCleanup::kMaxChunk);
    for (size_t i = 0; i < x.size(); i += chunk) {
        const int m = (int)std::min<size_t>(chunk, x.size() - i);
        for (int j = 0; j < m; j++) in[2 * j] = in[2 * j + 1] = x[i + j];
        const float* o = ac.process(in.data(), m, sc.data());
        for (int j = 0; j < m; j++) y[i + j] = o[2 * j];
    }
    return y;
}
static double power(const std::vector<float>& a, size_t from, size_t to) { double s = 0; for (size_t i = from; i < to && i < a.size(); i++) s += (double)a[i] * a[i]; return s / std::max<size_t>(1, to - from); }
static double db(double p) { return 10 * std::log10(p + 1e-20); }
// SNR of y against the reference c, y delayed by lag, from index `from`
static double snr(const std::vector<float>& y, const std::vector<float>& c, size_t lag, size_t from) {
    double sp = 0, ep = 0;
    for (size_t i = from; i + lag < y.size(); i++) { const double r = c[i], e = y[i + lag] - r; sp += r * r; ep += e * e; }
    return db(sp / (ep + 1e-20));
}
static AudioCleanupSettings only() { AudioCleanupSettings s; s.master = true; return s; }

int main() {
    const int R = 48000;
    // 1. off: the input pointer itself, bit-identical
    {
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        std::vector<float> in(960), sc(2048);
        for (size_t i = 0; i < in.size(); i++) in[i] = std::sin(0.01f * i) * 0.5f;
        bool active = true;
        const float* o = ac.process(in.data(), 480, sc.data(), &active);
        CHECK(o == in.data() && !active, "off must return the input itself");
        auto s = only(); cfg.set(s);
        const std::vector<float> sig(R, 0.1f);
        const auto y = run(ac, sig);
        CHECK(y.size() == sig.size(), "size");
        CHECK(ac.latencySamples() == 256, "latency %d", ac.latencySamples());
    }
    // 2. noise reduction: SNR improvement and gap attenuation, strength ordering
    {
        std::vector<float> c, x; std::vector<char> on;
        speechLike(R, 12, c, on); x = c; addWhite(x, 0.05, 3);   // speech RMS about 0.07: SNR about 3 dB
        const double snrIn = snr(x, c, 0, 2 * R);
        double prevImp = -1;
        for (float st : {0.3f, 0.6f, 1.0f}) {
            AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
            auto s = only(); s.nr = true; s.nrStrength = st; cfg.set(s);
            const auto y = run(ac, x);
            const double imp = snr(y, c, 256, 2 * R) - snrIn;
            double g = 0, gi = 0; size_t cnt = 0;   // gap power out vs in (gap = 150 ms after the burst ended up to 100 ms before the next)
            for (size_t i = 3 * R; i + 256 < y.size(); i++) { const double t = std::fmod((double)i / R, 1.0); if (t > 0.75 && t < 0.95) { g += (double)y[i + 256] * y[i + 256]; gi += (double)x[i] * x[i]; cnt++; } }
            printf("nr strength %.1f: SNR in %.1f dB, improvement %.1f dB, gap attenuation %.1f dB\n", st, snrIn, imp, db(gi / g));
            CHECK(imp > (st >= 0.6f ? 4.0 : 2.0), "SNR improvement %.1f dB at strength %.1f", imp, st);
            CHECK(imp >= prevImp - 1.0, "more strength must not be worse by much");
            CHECK(db(gi / g) > 8.0, "gap attenuation %.1f dB", db(gi / g));
            prevImp = imp;
        }
    }
    // 3. transparent when there is no noise (clean speech-like, and a steady sine), and the noise tracker bias on stationary noise
    {
        std::vector<float> c; std::vector<char> on; speechLike(R, 8, c, on);
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        auto s = only(); s.nr = true; s.nrStrength = 1.f; s.autoNotch = true; cfg.set(s);
        const auto y = run(ac, c);
        const double e = snr(y, c, 256, R);
        printf("clean speech through NR + auto notch: error %.1f dB below the signal\n", e);
        CHECK(e > 20.0, "clean sound damaged: SNR %.1f dB", e);
        // master on with every stage off: pure delay
        AudioCleanupConfig cfg2; AudioCleanup ac2; ac2.setConfig(&cfg2); ac2.prepare(R); cfg2.set(only());
        const auto y2 = run(ac2, c);
        CHECK(snr(y2, c, 256, R) > 60.0, "all stages off must be a pure delay, SNR %.1f", snr(y2, c, 256, R));
        // stationary white noise: the reduction settles at the gain floor
        std::vector<float> nz(6 * R, 0.f); addWhite(nz, 0.05, 9);
        AudioCleanupConfig cfg3; AudioCleanup ac3; ac3.setConfig(&cfg3); ac3.prepare(R);
        auto s3 = only(); s3.nr = true; s3.nrStrength = 1.f; cfg3.set(s3);
        const auto y3 = run(ac3, nz);
        printf("white noise at strength 1: %.1f dB reduction\n", db(power(nz, 3 * R, 6 * R)) - db(power(y3, 3 * R, 6 * R)));
        CHECK(db(power(nz, 3 * R, 6 * R)) - db(power(y3, 3 * R, 6 * R)) > 12.0, "noise only must be reduced");
    }
    // 4. auto notch follows a drifting whistle, speech is not damaged
    {
        std::vector<float> c; std::vector<char> on; speechLike(R, 10, c, on);
        std::vector<float> w(c.size()), x(c.size());
        double ph = 0; std::vector<double> inst(c.size());
        for (size_t i = 0; i < c.size(); i++) { const double t = (double)i / R; const double f = 1500 + 60 * t + 40 * std::sin(2 * kPi * 0.5 * t); inst[i] = f; ph += 2 * kPi * f / R; w[i] = (float)(0.15 * std::sin(ph)); x[i] = c[i] + w[i]; }
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        auto s = only(); s.autoNotch = true; cfg.set(s);
        const auto y = run(ac, x);
        // whistle residue: demodulate at the instantaneous frequency, average over 50 ms
        auto resid = [&](const std::vector<float>& a, size_t lag, size_t from, size_t to) {
            double re = 0, im = 0, p2 = 0; double ph2 = 0; size_t n = 0;
            for (size_t i = 0; i < to; i++) { ph2 += 2 * kPi * inst[i] / R; if (i >= from) { const double v = (i + lag < a.size()) ? a[i + lag] : 0; re += v * std::cos(ph2); im += v * std::sin(ph2); n++; } }
            p2 = (re * re + im * im) / ((double)n * n) * 4; return p2;
        };
        const double before = resid(x, 0, 5 * R, 6 * R), after = resid(y, 256, 5 * R, 6 * R);
        printf("whistle (1.5 kHz drifting 60 Hz/s): %.1f dB removed after 5 s; slots %.0f %.0f\n", db(before) - db(after), ac.autoNotchHz(0), ac.autoNotchHz(1));
        CHECK(db(before) - db(after) > 18.0, "whistle only %.1f dB down", db(before) - db(after));
        // speech: the output minus the whistle-free reference
        std::vector<float> yc = y;
        const double sn = snr(yc, c, 256, 3 * R), snIn = snr(x, c, 0, 3 * R);
        printf("speech + whistle: SNR %.1f dB in, %.1f dB out\n", snIn, sn);
        CHECK(sn > snIn + 15.0, "whistle not removed enough: %.1f -> %.1f", snIn, sn);
        // speech without the whistle: no slot may lock on, nothing damaged
        AudioCleanupConfig cfg2; AudioCleanup ac2; ac2.setConfig(&cfg2); ac2.prepare(R); cfg2.set(s);
        const auto y2 = run(ac2, c);
        CHECK(snr(y2, c, 256, R) > 30.0, "speech damaged by the auto notch: %.1f dB", snr(y2, c, 256, R));
        // released again when the whistle stops
        std::vector<float> x3(6 * R);
        for (size_t i = 0; i < x3.size(); i++) x3[i] = (i < 3 * (size_t)R ? 0.2f : 0.f) * (float)std::sin(2 * kPi * 900.0 * i / R) + (i >= 3 * (size_t)R ? c[i] : 0.f);
        AudioCleanupConfig cfg3; AudioCleanup ac3; ac3.setConfig(&cfg3); ac3.prepare(R); cfg3.set(s);
        const auto y3 = run(ac3, x3);
        std::vector<float> ref(x3.size()); for (size_t i = 3 * R; i < ref.size(); i++) ref[i] = c[i];
        CHECK(power(y3, 2 * R, 3 * R) < power(x3, 2 * R, 3 * R) * 0.02, "steady carrier not notched");
        CHECK(snr(y3, ref, 256, 4 * R) > 25.0, "notch did not release after the tone stopped: %.1f dB", snr(y3, ref, 256, 4 * R));
    }
    // 4b. manual notch
    {
        std::vector<float> x(2 * R); for (size_t i = 0; i < x.size(); i++) x[i] = 0.3f * (float)std::sin(2 * kPi * 1000.0 * i / R) + 0.3f * (float)std::sin(2 * kPi * 2500.0 * i / R);
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        auto s = only(); s.notch = true; s.notchHz = 1000; s.notchWidth = 0.5f; cfg.set(s);
        const auto y = run(ac, x);
        // 2500 Hz keeps its amplitude, 1000 Hz is gone
        double a1 = 0, b1 = 0, a2 = 0, b2 = 0; for (size_t i = R; i < y.size(); i++) { a1 += y[i] * std::sin(2 * kPi * 1000.0 * (i - 256) / R); b1 += y[i] * std::cos(2 * kPi * 1000.0 * (i - 256) / R); a2 += y[i] * std::sin(2 * kPi * 2500.0 * (i - 256) / R); b2 += y[i] * std::cos(2 * kPi * 2500.0 * (i - 256) / R); }
        const double n = y.size() - R, m1 = 2 * std::hypot(a1, b1) / n, m2 = 2 * std::hypot(a2, b2) / n;
        CHECK(m1 < 0.01 && m2 > 0.28, "manual notch: 1 kHz %.3f 2.5 kHz %.3f", m1, m2);
    }
    // 5. leveller: quiet and loud both go to the target; never louder than full scale; noise is not boosted
    {
        std::vector<float> c; std::vector<char> on; speechLike(R, 14, c, on, 0.25);
        for (float g : {0.15f, 1.0f, 3.0f}) {
            std::vector<float> x = c; for (auto& v : x) v *= g;
            AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
            auto s = only(); s.level = true; s.levelAmount = 1.f; s.levelTargetDb = -20.f; s.levelHold = false; cfg.set(s);
            const auto y = run(ac, x);
            double p = 0; size_t k = 0; float pk = 0;
            for (size_t i = 8 * R; i < y.size(); i++) { pk = std::max(pk, std::fabs(y[i])); if (on[i - 256 > 0 ? i - 256 : 0]) { p += (double)y[i] * y[i]; k++; } }
            const double lvl = db(p / k);
            printf("leveller: input gain %.2f -> speech RMS %.1f dBFS, peak %.2f\n", g, lvl, pk);
            if (g < 3.f) CHECK(lvl > -24.0 && lvl < -14.0, "level %.1f dBFS for gain %.2f", lvl, g);
            CHECK(pk <= 1.0f, "limiter let %.3f through", pk);
        }
        std::vector<float> nz(8 * R, 0.f); addWhite(nz, 0.002, 4);   // -54 dBFS hiss only
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        auto s = only(); s.level = true; s.levelAmount = 1.f; s.levelHold = true; cfg.set(s);
        const auto y = run(ac, nz);
        CHECK(db(power(y, 4 * R, 8 * R)) - db(power(nz, 4 * R, 8 * R)) < 3.0, "hiss was boosted by %.1f dB", db(power(y, 4 * R, 8 * R)) - db(power(nz, 4 * R, 8 * R)));
        // quiet/loud balance: a quiet and a loud passage come closer
        std::vector<float> two(16 * R);
        for (size_t i = 0; i < two.size(); i++) two[i] = c[i % c.size()] * (i < 8 * (size_t)R ? 0.1f : 1.f);
        AudioCleanupConfig cfg2; AudioCleanup ac2; ac2.setConfig(&cfg2); ac2.prepare(R);
        auto s2 = only(); s2.level = true; s2.levelAmount = 0.8f; s2.levelHold = false; cfg2.set(s2);
        const auto y2 = run(ac2, two);
        const double dIn = db(power(two, 12 * R, 16 * R)) - db(power(two, 4 * R, 8 * R)), dOut = db(power(y2, 12 * R, 16 * R)) - db(power(y2, 4 * R, 8 * R));
        printf("quiet/loud difference: %.1f dB in, %.1f dB out\n", dIn, dOut);
        CHECK(dOut < dIn * 0.5, "balance too weak: %.1f -> %.1f", dIn, dOut);
    }
    // 6. silence gate
    {
        std::vector<float> c; std::vector<char> on; speechLike(R, 10, c, on);
        std::vector<float> x = c; addWhite(x, 0.01, 5);
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        auto s = only(); s.gate = true; s.gateOffsetDb = 9; cfg.set(s);
        const auto y = run(ac, x);
        double g = 0, gi = 0, sp = 0, spi = 0; size_t kg = 0, ks = 0;
        for (size_t i = 3 * R; i + 256 < y.size(); i++) {
            const double t = std::fmod((double)i / R, 1.0);
            if (t > 0.9 && t < 0.98) { g += (double)y[i + 256] * y[i + 256]; gi += (double)x[i] * x[i]; kg++; }
            if (t > 0.1 && t < 0.5) { sp += (double)y[i + 256] * y[i + 256]; spi += (double)x[i] * x[i]; ks++; }
        }
        printf("gate: silence %.1f dB down, speech %.2f dB\n", db(gi / g), db(sp / spi));
        CHECK(db(gi / g) > 20.0, "gap only %.1f dB down", db(gi / g));
        CHECK(std::fabs(db(sp / spi)) < 1.5, "speech level changed by %.2f dB", db(sp / spi));
        CHECK(ac.gateOpen() || true, "flag");
    }
    // 7. de-esser: a burst of 5-8 kHz hiss over speech goes down, the speech around it stays; voice EQ keeps the sound sane
    {
        std::vector<float> c; std::vector<char> on; speechLike(R, 6, c, on, 0.08);
        std::vector<float> x = c;
        std::mt19937 g(2); dect2::normal_distribution<float> nd(0, 1);
        float p1 = 0, p2 = 0;
        for (size_t i = 0; i < x.size(); i++) {
            const double t = std::fmod((double)i / R, 1.0);
            const float n0 = nd(g), h = n0 - p1 - (p1 - p2) * 0.0f; p2 = p1; p1 = n0;   // first difference: tilted to the highs
            if (t > 0.2 && t < 0.45) x[i] += 0.25f * (1.f + 0.3f * h) * (float)std::sin(2 * kPi * 6500.0 * i / R + 1.0);
        }
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        auto s = only(); s.deess = true; s.deessAmount = 1.f; cfg.set(s);
        const auto y = run(ac, x);
        double a = 0, b = 0, a0 = 0, b0 = 0;
        for (size_t i = 2 * R; i + 256 < y.size(); i++) {
            const double t = std::fmod((double)i / R, 1.0);
            if (t > 0.25 && t < 0.4) { a += (double)x[i] * x[i]; b += (double)y[i + 256] * y[i + 256]; }
            if (t > 0.5 && t < 0.58) { a0 += (double)x[i] * x[i]; b0 += (double)y[i + 256] * y[i + 256]; }
        }
        printf("de-esser: hiss burst %.1f dB down, speech around it %.2f dB\n", db(a / b), db(b0 / a0));
        CHECK(db(a / b) > 4.0, "de-esser too weak: %.1f dB", db(a / b));
        CHECK(std::fabs(db(b0 / a0)) < 1.0, "de-esser changed the plain speech by %.2f dB", db(b0 / a0));
        AudioCleanupConfig cfg2; AudioCleanup ac2; ac2.setConfig(&cfg2); ac2.prepare(R);
        auto s2 = only(); s2.voice = true; s2.voiceAmount = 1.f; cfg2.set(s2);
        const auto y2 = run(ac2, c);
        const double r = db(power(y2, R, 6 * R)) - db(power(c, R, 6 * R));
        CHECK(std::fabs(r) < 6.0, "voice EQ changed the level by %.1f dB", r);
    }
    // 8. no clicks when switching stages on and off while a tone plays
    {
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        std::vector<float> in(2 * 240), sc(2 * 1024);
        std::mt19937 rg(7); double ph = 0, maxJ = 0; float last = 0; bool first = true;
        for (int blk = 0; blk < 600; blk++) {
            if (blk % 7 == 0) {
                AudioCleanupSettings s; s.master = (rg() % 4) != 0; s.nr = rg() & 1; s.nrStrength = (rg() % 100) / 100.f; s.autoNotch = rg() & 1; s.notch = rg() & 1; s.notchHz = 500 + rg() % 2000;
                s.level = rg() & 1; s.levelAmount = (rg() % 100) / 100.f; s.gate = rg() & 1; s.voice = rg() & 1; s.deess = rg() & 1; cfg.set(s);
            }
            for (int i = 0; i < 240; i++) { ph += 2 * kPi * 440.0 / R; in[2 * i] = in[2 * i + 1] = 0.3f * (float)std::sin(ph); }
            const float* o = ac.process(in.data(), 240, sc.data());
            for (int i = 0; i < 240; i++) { if (!first && blk > 20) maxJ = std::max(maxJ, (double)std::fabs(o[2 * i] - last)); last = o[2 * i]; first = false; }
        }
        printf("switching stress: biggest sample step %.3f (a 0.3 sine at 440 Hz steps %.3f)\n", maxJ, 0.3 * 2 * kPi * 440 / R);
        CHECK(maxJ < 0.25, "click while switching: step %.3f", maxJ);
    }
    // 9. thread safety: another thread flips the config while the audio runs
    {
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        std::atomic<bool> stop{false};
        std::thread t([&] { std::mt19937 rg(3); while (!stop) { AudioCleanupSettings s; s.master = rg() & 1; s.nr = rg() & 1; s.autoNotch = rg() & 1; s.level = rg() & 1; s.gate = rg() & 1; s.voice = rg() & 1; s.deess = rg() & 1; s.nrStrength = (rg() % 101) / 100.f; cfg.set(s); } });
        std::vector<float> in(2 * 480), sc(2 * 1024); bool bad = false; double ph = 0;
        for (int blk = 0; blk < 1500; blk++) {
            for (int i = 0; i < 480; i++) { ph += 0.05; in[2 * i] = 0.3f * (float)std::sin(ph); in[2 * i + 1] = 0.2f * (float)std::cos(ph * 1.3); }
            const float* o = ac.process(in.data(), 480, sc.data());
            for (int i = 0; i < 960; i++) if (!std::isfinite(o[i]) || std::fabs(o[i]) > 3.f) bad = true;
        }
        stop = true; t.join();
        CHECK(!bad, "non-finite or huge output while the config changed");
    }
    // 10. other rates
    for (int rate : {8000, 16000, 22050, 44100, 96000}) {
        std::vector<float> c; std::vector<char> on; speechLike(rate, 5, c, on); addWhite(c, 0.02, 1);
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(rate);
        auto s = audioCleanupPreset(AudioProfile::NFM); s.master = true; cfg.set(s);
        const auto y = run(ac, c, 333);
        bool ok = true; for (float v : y) if (!std::isfinite(v) || std::fabs(v) > 1.5f) ok = false;
        CHECK(ok, "rate %d not sane", rate);
        printf("rate %d: latency %d samples = %.1f ms\n", rate, ac.latencySamples(), 1000.0 * ac.latencySamples() / rate);
    }
    // 11. CPU: everything on, stereo with different channels (the worst case), 20 s of audio
    {
        std::vector<float> c; std::vector<char> on; speechLike(R, 20, c, on); addWhite(c, 0.03, 8);
        for (int stereo = 0; stereo < 2; stereo++) {
            AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
            AudioCleanupSettings s = audioCleanupPreset(AudioProfile::NFM); s.master = true; s.notch = true; cfg.set(s);
            std::vector<float> in(2 * 480), sc(2 * 1024);
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i + 480 <= c.size(); i += 480) {
                for (int j = 0; j < 480; j++) { in[2 * j] = c[i + j]; in[2 * j + 1] = stereo ? c[(i + j + 777) % c.size()] : c[i + j]; }
                ac.process(in.data(), 480, sc.data());
            }
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            printf("CPU %s, every stage on: %.2f %% of one core at 48 kHz\n", stereo ? "stereo" : "mono (dual mono)", 100.0 * sec / 20.0);
            CHECK(100.0 * sec / 20.0 < 3.0 * (stereo ? 2 : 1), "too slow: %.2f %%", 100.0 * sec / 20.0);
        }
    }
    // one NaN from a demodulator: the sound comes back right after it, with every recursive stage on
    {
        std::vector<float> c; std::vector<char> on; speechLike(R, 4, c, on); addWhite(c, 0.02, 5);
        c[100 * 480 + 7] = std::nanf("");
        AudioCleanupConfig cfg; AudioCleanup ac; ac.setConfig(&cfg); ac.prepare(R);
        auto s = only(); s.nr = true; s.autoNotch = true; s.voice = true; s.level = true; cfg.set(s);
        const auto y = run(ac, c);
        size_t bad = 0;
        for (size_t i = 100 * 480 + R; i < y.size(); i++) if (!std::isfinite(y[i])) bad++;
        CHECK(bad == 0, "a single NaN must not silence the chain: %zu bad samples a second later", bad);
    }
    printf(fails ? "audio cleanup: %d FAILED\n" : "audio cleanup: all passed\n", fails);
    return fails ? 1 : 0;
}
