// CW keying quality meter: Morse sent by a generated keyer or hand (speed 12 to 30 wpm, dah ratio, weight, scatter, character and word gaps, shaped edges, chirp), with a carrier offset and drift, noise,
// fades; the speed, ratios, rise and fall times and chirp are compared with what the generator did; noise alone gives nothing. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char* kMorse[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
struct Iv { double a, b; };
struct Keyer { double wpm = 20, dahRatio = 3.0, weight = 1.0, charGap = 3, wordGap = 7, cv = 0; };
static std::vector<Iv> send(const std::string& text, const Keyer& k, unsigned seed, double start = 1.0) {
    std::vector<Iv> v; std::mt19937 g(seed); const double T = 1.2 / k.wpm; double t = start;
    auto jit = [&]() { return 1.0 + k.cv * (((double)(g() % 2001) / 1000.0) - 1.0); };
    for (char c : text) {
        if (c == ' ') { t += (k.wordGap - k.charGap) * T; continue; }
        const char* m = kMorse[c - 'A'];
        for (const char* e = m; *e; e++) {
            const double d = (*e == '.' ? 1.0 : k.dahRatio) * T * k.weight * jit();
            v.push_back({t, t + d}); t += d + T * (2.0 - k.weight) ;     // element gap: the dit length is weight x T, the gap makes up the rest of the nominal 2 T
        }
        t += (k.charGap - 1.0) * T - T * (1.0 - 1.0) * 0;     // character gap measured from the end of the last element's gap
    }
    return v;
}
struct Src {
    double fs, hz, drift, noise, ramp, chirpHz, chirpTau; std::vector<Iv> iv; Rng rng{9}; double ph = 0, fadeDb = 0;
    double amp(double t) const { double a = 0, ts = 0; for (auto& v : iv) { if (t < v.a - ramp / 2 || t > v.b + ramp / 2) continue; double q; if (t < v.a + ramp / 2) q = 0.5 - 0.5 * std::cos(kPi * (t - (v.a - ramp / 2)) / ramp); else if (t > v.b - ramp / 2) q = 0.5 + 0.5 * std::cos(kPi * (t - (v.b - ramp / 2)) / ramp); else q = 1; a = std::max(a, q); ts = v.a; } (void)ts; return a; }
    double chirpAt(double t) const { for (auto& v : iv) if (t >= v.a && t <= v.b + 0.02) return chirpHz * std::exp(-(t - v.a) / chirpTau); return 0; }
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) { const double t = t0 + i / fs; const double f = hz + drift * t + chirpAt(t); ph += 2 * kPi * f / fs; const double a = amp(t) * std::pow(10.0, fadeDb * std::sin(2 * kPi * 0.2 * t) / 20); x[i] = (float)a * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sg * rng.g(), sg * rng.g()); }
        return x;
    }
};
static double f1(const X40Telemetry& t, const char* key, const char* what) { for (auto& f : t.facts) if (f.first == key) { const char* p = strstr(f.second.c_str(), what); return p ? atof(p + strlen(what)) : atof(f.second.c_str()); } return -999; }

int main() {
    const std::string text = "CQ CQ DE WIAW WIAW K PARIS PARIS QRZ TEST";
    // 1. speeds and keyer settings
    struct C { double wpm, dah, w, cv, ramp, chirp, off, drift; };
    for (C c : {C{20, 3.0, 1.0, 0.0, 0.006, 0, 0, 0}, C{12, 3.0, 1.0, 0.0, 0.010, 0, 400, 2}, C{30, 3.0, 1.0, 0.0, 0.004, 0, -300, 0}, C{20, 3.3, 1.15, 0.0, 0.006, 0, 100, 0}, C{20, 3.0, 1.0, 0.12, 0.006, 0, 0, 0}, C{20, 3.0, 1.0, 0.0, 0.006, 90, 0, 0}}) {
        const double fs = 48000; Keyer k; k.wpm = c.wpm; k.dahRatio = c.dah; k.weight = c.w; k.cv = c.cv;
        Src s{fs, c.off, c.drift, noiseFor(40, 500, fs), c.ramp, c.chirp, 0.02}; s.iv = send(text, k, 4);
        const double end = s.iv.back().b + 3.0;
        auto t = runChunks(x40::CwKeying, fs, end, std::ref(s), 0, 7.03e6, 1.0);
        const double wpm = f1(t, "speed", ""), dah = f1(t, "dit and dah", "dah / dit "), rise = f1(t, "rise time (10 - 90 %)", ""), fall = f1(t, "fall time (90 - 10 %)", ""), cv = f1(t, "dit scatter", ""), chirp = f1(t, "chirp", "");
        const double wantRise = 0.5904 * c.ramp * 1000;
        printf("  %.0f wpm dah %.1f weight %.2f cv %.0f %% ramp %.0f ms chirp %.0f: read %.1f wpm, dah/dit %.2f, rise %.1f (want %.1f) fall %.1f, scatter %.1f %%, chirp %.1f\n", c.wpm, c.dah, c.w, c.cv * 100, c.ramp * 1000, c.chirp, wpm, dah, rise, wantRise, fall, cv, chirp);
        CHECK(std::fabs(wpm - c.wpm) < 0.04 * c.wpm, "wpm %.2f want %.0f", wpm, c.wpm);
        CHECK(std::fabs(dah - c.dah) < 0.2, "dah ratio %.2f want %.2f", dah, c.dah);
        CHECK(std::fabs(rise - std::max(wantRise, 1.5)) < 0.25 * std::max(wantRise, 1.5) + 0.5, "rise %.2f want %.2f", rise, wantRise);
        CHECK(std::fabs(fall - std::max(wantRise, 1.5)) < 0.25 * std::max(wantRise, 1.5) + 0.5, "fall %.2f want %.2f", fall, wantRise);
        if (c.cv > 0) CHECK(cv > 4 && cv < 12, "scatter %.1f %% for 12 %% uniform jitter (std 6.9 %%)", cv); else CHECK(cv < 3, "scatter %.1f %% for a keyer", cv);
        if (c.chirp > 0) CHECK(chirp > 0.3 * c.chirp && chirp < 1.2 * c.chirp, "chirp %.1f Hz for a %.0f Hz start offset", chirp, c.chirp);
        else if (c.chirp == 0) CHECK(std::fabs(chirp) < 8.0, "chirp %.1f on a clean carrier", chirp);
        const double weight = f1(t, "weight", ""); CHECK(std::fabs(weight - c.w / (2.0 - c.w)) < 0.08 * c.w / (2.0 - c.w) + 0.03, "weight %.2f want %.2f", weight, c.w / (2.0 - c.w));
    }
    // 2. noise sweep (carrier to noise in 500 Hz): the speed read and the elements counted
    printf("noise sweep (carrier to noise in 500 Hz) -> wpm read (true 20.0), scatter %%:");
    for (double cnr : {40.0, 30.0, 20.0, 15.0, 10.0, 6.0}) {
        const double fs = 48000; Keyer k; Src s{fs, 200, 0, noiseFor(cnr, 500, fs), 0.006, 0, 0.02}; s.iv = send(text, k, 4);
        auto t = runChunks(x40::CwKeying, fs, s.iv.back().b + 3.0, std::ref(s), 0, 7.03e6, 1.0);
        printf(" %.0f: %.1f/%.1f", cnr, f1(t, "speed", ""), f1(t, "dit scatter", ""));
        if (cnr >= 20) CHECK(std::fabs(f1(t, "speed", "") - 20.0) < 1.0, "%.0f dB: %.1f wpm", cnr, f1(t, "speed", ""));
    }
    printf("\n");
    { ToneScene sc(48000, 1.0, 5); auto t = runChunks(x40::CwKeying, 48000, 60.0, std::ref(sc), 0, 7.03e6, 1.0); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); }
    { ToneScene sc(48000, noiseFor(40, 500, 48000), 5); SchedTone a; a.hz = 300; sc.tones.push_back(a); auto t = runChunks(x40::CwKeying, 48000, 60.0, std::ref(sc), 0, 7.03e6, 1.0); CHECK(t.rows.empty(), "steady carrier: %zu rows", t.rows.size()); }
    printf(fails ? "x40 cwkeying: %d FAILED\n" : "x40 cwkeying: ok\n", fails);
    return fails ? 1 : 0;
}
