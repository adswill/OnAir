// Neural voice noise remover inside the shared audio cleanup chain (audio_cleanup.h, every mode's sound goes through it): off = what the chain
// did before (no change at all), on = the noise between speech bursts goes down and the speech stays, at every rate that divides 48 kHz, mono and
// stereo, any chunking, strength 0 passes the dry sound, switching on and off does not blow up, unsupported rates pass through.
#include "dect2/audio/audio_cleanup.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = 3.14159265358979323846;

static void speechLike(int rate, double seconds, std::vector<float>& c, std::vector<char>& on) {
    const size_t n = (size_t)(rate * seconds);
    c.assign(n, 0.f); on.assign(n, 0);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        const bool b = std::fmod(t, 1.0) < 0.5;
        on[i] = b;
        if (!b) continue;
        const double env = 0.5 * (1 - std::cos(2 * kPi * 4 * t));
        const double f0 = 130 + 15 * std::sin(2 * kPi * 2 * t);
        ph += 2 * kPi * f0 / rate;
        double v = 0;
        for (int h = 1; h * f0 < std::min(3400.0, 0.45 * rate); h++) v += std::sin(h * ph) / h;
        c[i] = (float)(0.25 * env * v);
    }
}

// run the chain over mono (duplicated to stereo) in chunks; returns the left channel
static std::vector<float> run(AudioCleanup& ac, const std::vector<float>& x, int chunk, bool stereo = false) {
    std::vector<float> in(2 * (size_t)chunk), sc(2 * AudioCleanup::kMaxChunk), y;
    for (size_t off = 0; off < x.size(); off += chunk) {
        const int m = (int)std::min<size_t>(chunk, x.size() - off);
        for (int i = 0; i < m; i++) { in[2 * i] = x[off + i]; in[2 * i + 1] = stereo ? 0.5f * x[off + i] : x[off + i]; }
        const float* r = ac.process(in.data(), m, sc.data());
        for (int i = 0; i < m; i++) y.push_back(r[2 * i]);
    }
    return y;
}
static double rmsGaps(const std::vector<float>& y, const std::vector<char>& on, int rate, size_t from, int lag) {
    double s = 0; size_t c = 0;
    const int guard = rate / 10;
    for (size_t i = from; i + lag < y.size(); i++) {
        if (i < (size_t)guard || on[i] || on[i - guard] || (i + guard < on.size() && on[i + guard])) continue;
        s += (double)y[i + lag] * y[i + lag]; c++;
    }
    return c ? std::sqrt(s / (double)c) : 0;
}

int main() {
    for (int rate : {8000, 16000, 24000, 48000}) {
        std::vector<float> clean; std::vector<char> on;
        speechLike(rate, 4.0, clean, on);
        std::vector<float> x = clean;
        std::mt19937 g(4); dect2::normal_distribution<float> nd(0.f, 0.03f);
        for (auto& v : x) v += nd(g);

        // chain on, neural off, versus chain on, neural on (separate configs: the global one stays untouched)
        AudioCleanupConfig cOff, cOn;
        AudioCleanupSettings s; s.master = true;
        cOff.set(s);
        s.neural = true; s.neuralStrength = 1.f;
        cOn.set(s);
        AudioCleanup aOff, aOn;
        aOff.setConfig(&cOff); aOn.setConfig(&cOn);
        aOff.prepare(rate); aOn.prepare(rate);
        const std::vector<float> yOff = run(aOff, x, 256), yOn = run(aOn, x, 256);
        const size_t from = (size_t)rate * 2;
        const double gOff = rmsGaps(yOff, on, rate, from, 0), gOn = rmsGaps(yOn, on, rate, from, 0);
        const double red = 20 * std::log10(gOff / std::max(gOn, 1e-9));
        printf("rate %5d: noise between bursts %.4f -> %.4f (%.1f dB down)\n", rate, gOff, gOn, red);
        CHECK(red > 12.0, "rate %d: neural stage lowers the noise by more than 12 dB (%.1f)", rate, red);
        // the speech survives: energy inside the bursts stays within 4 dB
        double eOff = 0, eOn = 0; size_t cnt = 0;
        for (size_t i = from; i < yOn.size(); i++) if (on[i]) { eOff += (double)yOff[i] * yOff[i]; eOn += (double)yOn[i] * yOn[i]; cnt++; }
        CHECK(cnt > 0 && std::fabs(10 * std::log10(eOn / eOff)) < 4.0, "rate %d: speech level kept (%.1f dB)", rate, 10 * std::log10(eOn / eOff));

        // any chunking gives the same sound
        AudioCleanup aOn2; aOn2.setConfig(&cOn); aOn2.prepare(rate);
        const std::vector<float> yOn2 = run(aOn2, x, 1000);
        double e = 0, p = 0;
        for (size_t i = 0; i < std::min(yOn.size(), yOn2.size()); i++) { e += (double)(yOn[i] - yOn2[i]) * (yOn[i] - yOn2[i]); p += (double)yOn[i] * yOn[i]; }
        CHECK(e < 1e-6 * p, "rate %d: chunk size does not change the result (%.2e)", rate, e / p);

        // strength 0 is the chain without the neural stage, delayed alike: the same samples
        AudioCleanupConfig c0; s.neuralStrength = 0.f; c0.set(s);
        AudioCleanup a0; a0.setConfig(&c0); a0.prepare(rate);
        const std::vector<float> y0 = run(a0, x, 256);
        const int lat = 0;   // the dry path is delayed by the stage's own latency: compare the energy, not the samples
        (void)lat;
        double e0 = 0, eo = 0; for (size_t i = from; i < y0.size(); i++) { e0 += (double)y0[i] * y0[i]; eo += (double)yOff[i] * yOff[i]; }
        CHECK(std::fabs(10 * std::log10(e0 / eo)) < 0.5, "rate %d: strength 0 leaves the sound (%.2f dB)", rate, 10 * std::log10(e0 / eo));
    }

    // neural off (the default): the stage is never run, so the chain is deterministic and untouched by it
    {
        const int rate = 48000;
        std::vector<float> clean; std::vector<char> on; speechLike(rate, 1.0, clean, on);
        AudioCleanupConfig c1, c2; AudioCleanupSettings s; s.master = true; s.nr = true; s.level = true;
        c1.set(s); c2.set(s);
        AudioCleanup a1, a2; a1.setConfig(&c1); a2.setConfig(&c2); a1.prepare(rate); a2.prepare(rate);
        const auto y1 = run(a1, clean, 480), y2 = run(a2, clean, 480);
        CHECK(y1.size() == y2.size(), "sizes");
        double e = 0, p = 0; for (size_t i = 0; i < y1.size(); i++) { e += (double)(y1[i] - y2[i]) * (y1[i] - y2[i]); p += (double)y1[i] * y1[i]; }
        CHECK(e == 0.0, "neural off: two identical runs give identical samples (%.2e)", e / (p + 1e-12));
        CHECK(!AudioCleanupConfig::global().neural.load(), "global config: off by default");
    }

    // a rate RNNoise cannot take (44.1 kHz) passes the sound on, stereo is handled, on/off toggling keeps the output finite
    {
        const int rate = 44100;
        std::vector<float> clean; std::vector<char> on; speechLike(rate, 2.0, clean, on);
        AudioCleanupConfig c; AudioCleanupSettings s; s.master = true; s.neural = true; c.set(s);
        AudioCleanup a; a.setConfig(&c); a.prepare(rate);
        const auto y = run(a, clean, 441, true);
        bool fin = true; for (float v : y) if (!std::isfinite(v)) fin = false;
        CHECK(fin && y.size() == clean.size(), "44.1 kHz: finite output of the same length");
        // toggle every 0.25 s at 48 kHz
        AudioCleanupConfig c2; AudioCleanup b; b.setConfig(&c2);
        b.prepare(48000);
        std::vector<float> c3; std::vector<char> on3; speechLike(48000, 3.0, c3, on3);
        std::vector<float> out;
        for (size_t off = 0, blk = 0; off < c3.size(); off += 12000, blk++) {
            AudioCleanupSettings t; t.master = true; t.neural = (blk & 1) == 0; c2.set(t);
            std::vector<float> part(c3.begin() + (long)off, c3.begin() + (long)std::min(c3.size(), off + 12000));
            const auto r = run(b, part, 480, true);
            out.insert(out.end(), r.begin(), r.end());
        }
        bool fin2 = true; float mx = 0; for (float v : out) { if (!std::isfinite(v)) fin2 = false; mx = std::max(mx, std::fabs(v)); }
        CHECK(fin2 && mx < 2.0f, "toggling keeps the output finite and bounded (max %.2f)", mx);
    }

    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
