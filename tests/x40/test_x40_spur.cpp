// Spur and image finder: a radio with an IQ mismatch (image at -f), a DC offset, second and third harmonics and a constant spur at fs/4, with fading signals; a real signal at -f that
// does not follow must not be called an image. Image rejection and the mismatch are checked against the generator. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Scene {
    double fs, f1, f2, swing, imgAmp, imgPhase, dc, h2, h3, spurHz, spurAmp, noise, realHz, realAmp; Rng rng{5}; double p1 = 0, p2 = 0, ps = 0, pr = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs;
            const double a1 = 0.2 * std::pow(10.0, swing * std::sin(2 * kPi * 0.067 * t) / 20), a2 = 0.2 * std::pow(10.0, swing * std::sin(2 * kPi * 0.101 * t + 2.0) / 20);
            p1 += 2 * kPi * f1 / fs; p2 += 2 * kPi * f2 / fs; ps += 2 * kPi * spurHz / fs; pr += 2 * kPi * realHz / fs;
            const cf32 s = (float)a1 * cf32((float)std::cos(p1), (float)std::sin(p1)) + (float)a2 * cf32((float)std::cos(p2), (float)std::sin(p2));
            cf32 v = s + (float)imgAmp * cf32((float)std::cos(imgPhase), (float)std::sin(imgPhase)) * std::conj(s);     // IQ mismatch: image at -f
            // harmonics of the first signal only: a1^2 at 2 f1, a1^3 at 3 f1
            const cf32 e1 = (float)a1 * cf32((float)std::cos(p1), (float)std::sin(p1));
            v += (float)h2 * e1 * e1 + (float)h3 * e1 * e1 * e1;
            v += cf32((float)dc, 0) + (float)spurAmp * cf32((float)std::cos(ps), (float)std::sin(ps)) + (float)realAmp * cf32((float)std::cos(pr), (float)std::sin(pr));
            x[i] = v + cf32(sg * rng.g(), sg * rng.g());
        }
        return x;
    }
};
static bool hasRow(const X40Telemetry& t, const char* kind, double hzOff, double centre, const char* what) {
    for (auto& r : t.rows) if (r.kind == kind && std::fabs(atof(r.id.c_str()) * 1e6 - (centre + hzOff)) < 500 && r.text.find(what) != std::string::npos) return true; return false;
}

int main() {
    const double fs = 1000000, centre = 145e6, f1 = 130000, f2 = -210000, noise = 2e-6;
    // 1. IQ mismatch of -34 dB (|beta| = 0.02), a DC spike, a 2nd (0.5) and 3rd (1.0) harmonic of signal 1, a constant spur at fs/4
    for (double shift : {0.0, 555.0}) {
        Scene sc{fs, f1 + shift, f2 + shift, 5.0, 0.02, 0.7, 0.01, 0.5, 1.5, 250000, 0.005, noise, 0, 0};
        auto t = runChunks(x40::Spur, fs, 70.0, std::ref(sc), 0, centre, 0.5);
        CHECK(hasRow(t, "Image", -(f1 + shift), centre, "follows the signal"), "shift %.0f: image of signal 1 not found (%s)", shift, t.headline.c_str());
        CHECK(hasRow(t, "Image", -(f2 + shift), centre, "follows the signal"), "shift %.0f: image of signal 2 not found", shift);
        CHECK(hasRow(t, "Harmonic", 2 * (f1 + shift), centre, "2nd harmonic"), "shift %.0f: 2nd harmonic not found", shift);
        CHECK(hasRow(t, "Harmonic", 3 * (f1 + shift) - 1e6 * 0, centre, "3rd harmonic") || hasRow(t, "Harmonic", 3 * (f1 + shift) - fs, centre, "3rd harmonic") || hasRow(t, "Harmonic", 3 * (f1 + shift) + fs, centre, "3rd harmonic"), "shift %.0f: 3rd harmonic not found", shift);
        CHECK(hasRow(t, "Spur", 250000, centre, "fixed spur"), "shift %.0f: fs/4 spur not found", shift);
        CHECK(countKind(t, "DC") == 1, "DC rows %d", countKind(t, "DC"));
        // the image rejection in the facts: 34 dB +- 2
        for (auto& f : t.facts) if (f.first.rfind("finding", 0) == 0 && f.second.rfind("image of", 0) == 0) { double dB = 0; const char* p = strstr(f.second.c_str(), "MHz, "); if (p && sscanf(p + 5, "%lf dB below it", &dB) == 1) CHECK(std::fabs(dB - 34.0) < 2.5, "image %.1f dB below, want 34", dB); }
    }
    // 2. a real constant signal at -f1: not an image
    { Scene sc{fs, f1, f2, 5.0, 0.0, 0, 0, 0, 0, 0, 0, noise, -f1, 0.004};
      auto t = runChunks(x40::Spur, fs, 70.0, std::ref(sc), 0, centre, 0.5);
      CHECK(!hasRow(t, "Image", -f1, centre, "follows the signal"), "a real signal at -f called an image");
      CHECK(hasRow(t, "Image", -f1, centre, "does not follow"), "a real signal at -f not called independent"); }
    // 3. steady signals: the image is listed as "cannot tell"
    { Scene sc{fs, f1, f2, 0.0, 0.02, 0.7, 0, 0, 0, 0, 0, noise, 0, 0}; auto t = runChunks(x40::Spur, fs, 40.0, std::ref(sc), 0, centre, 0.5);
      CHECK(hasRow(t, "Image", -f1, centre, "cannot tell"), "steady: not 'cannot tell' (%s)", t.headline.c_str()); }
    // 4. image rejection sweep (the finder's reading against the generator's)
    printf("image rejection put in -> read (dB):");
    for (double irr : {20.0, 30.0, 40.0, 50.0, 55.0}) {
        Scene sc{fs, f1, f2, 5.0, std::pow(10.0, -irr / 20), 0.3, 0, 0, 0, 0, 0, noise, 0, 0};
        auto t = runChunks(x40::Spur, fs, 70.0, std::ref(sc), 0, centre, 0.5); double rd = -1;
        for (auto& f : t.facts) if (f.first.rfind("finding", 0) == 0 && f.second.rfind("image of", 0) == 0) { const char* p = strstr(f.second.c_str(), "MHz, "); double v; if (p && sscanf(p + 5, "%lf dB below it", &v) == 1) { rd = v; break; } }
        printf(" %.0f->%.0f", irr, rd);
        if (irr <= 50) CHECK(std::fabs(rd - irr) < 2.5, "irr %.0f read %.1f", irr, rd);
    }
    printf("\n");
    // 5. no mismatch, no spurs: nothing is reported; noise only: nothing
    { Scene sc{fs, f1, f2, 5.0, 0, 0, 0, 0, 0, 0, 0, noise, 0, 0}; auto t = runChunks(x40::Spur, fs, 60.0, std::ref(sc), 0, centre, 0.5); CHECK(t.rows.empty(), "clean radio: %zu rows (%s)", t.rows.size(), t.headline.c_str()); }
    { ToneScene sc(fs, 1.0, 5); auto t = runChunks(x40::Spur, fs, 40.0, std::ref(sc), 0, centre, 0.5); for (auto& r : t.rows) printf("noise row: %s %s %s\n", r.kind.c_str(), r.id.c_str(), r.text.c_str()); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); }
    printf(fails ? "x40 spur: %d FAILED\n" : "x40 spur: ok\n", fails);
    return fails ? 1 : 0;
}
