// OOK / ASK pulse analyser: PT2262-style PWM (1:3 pulses, 4 T period, sync 1:31), a PPM remote, a Manchester sensor and a PCM (NRZ) frame, with 3 % timing jitter, a carrier offset, noise and repeats;
// the modulation name, the bits and the timings are compared with what the generator sent; noise alone and a plain carrier give no frame. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// levels over time: a list of (duration seconds, high?) segments, repeated frames separated by a gap
struct Seg { double dur; bool hi; };
static std::vector<Seg> pwm(const std::string& bits, double T, double jit, unsigned seed) { std::vector<Seg> s; std::mt19937 g(seed); auto j = [&]() { return 1.0 + jit * (((double)(g() % 2001) / 1000.0) - 1.0); };
    for (char b : bits) { if (b == '0') { s.push_back({1 * T * j(), true}); s.push_back({3 * T * j(), false}); } else { s.push_back({3 * T * j(), true}); s.push_back({1 * T * j(), false}); } }
    s.push_back({1 * T * j(), true}); s.push_back({31 * T * j(), false}); return s; }
static std::vector<Seg> ppm(const std::string& bits, double pw, double g0, double g1, double jit, unsigned seed) { std::vector<Seg> s; std::mt19937 g(seed); auto j = [&]() { return 1.0 + jit * (((double)(g() % 2001) / 1000.0) - 1.0); };
    s.push_back({pw * j(), true}); for (char b : bits) { s.push_back({(b == '0' ? g0 : g1) * j(), false}); s.push_back({pw * j(), true}); } s.push_back({20e-3, false}); return s; }
static std::vector<Seg> manchester(const std::string& bits, double Th, double jit, unsigned seed) { std::vector<Seg> s; std::mt19937 g(seed); auto j = [&]() { return 1.0 + jit * (((double)(g() % 2001) / 1000.0) - 1.0); };
    std::string half; for (char b : bits) half += b == '1' ? "10" : "01";
    size_t i = 0; while (i < half.size()) { size_t k = i; while (k < half.size() && half[k] == half[i]) k++; s.push_back({(double)(k - i) * Th * j(), half[i] == '1'}); i = k; } s.push_back({20e-3, false}); return s; }
static std::vector<Seg> repeat(const std::vector<Seg>& f, int n, double gap) { std::vector<Seg> s; for (int i = 0; i < n; i++) { s.insert(s.end(), f.begin(), f.end()); s.push_back({gap, false}); } return s; }
struct Ook { double fs, lo, noise; std::vector<Seg> segs; double start = 0.2; Rng rng{3}; double ph = 0; std::vector<double> edges;
    void prep() { double t = start; edges.clear(); for (auto& s : segs) { edges.push_back(t); t += s.dur; } edges.push_back(t); }
    bool level(double t) const { if (t < start || t >= edges.back()) return false; const size_t i = (size_t)(std::upper_bound(edges.begin(), edges.end(), t) - edges.begin()) - 1; return segs[i].hi; }
    std::vector<cf32> operator()(double t0, size_t n) { if (edges.empty()) prep(); std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) { const double t = t0 + i / fs; ph += 2 * kPi * lo / fs; x[i] = (level(t) ? cf32((float)std::cos(ph), (float)std::sin(ph)) : cf32(0, 0)) + cf32(sg * rng.g(), sg * rng.g()); }
        return x; } };
static std::string cleanBits(const std::string& s) { return s.substr(0, s.find_first_of(" ")); }
static std::vector<std::pair<std::string, std::string>> frames(const X40Telemetry& t) { std::vector<std::pair<std::string, std::string>> v; for (auto& r : t.rows) if (r.kind == "Frame") { const size_t b = r.text.find(' '); v.push_back({r.id, r.text.substr(0, b)}); } return v; }

int main() {
    const double fs = 250000;
    const std::string bitsPwm = "101100101001110010100110", bitsPpm = "1100101001101011", bitsMan = "10110010111000101101";
    for (double lo : {0.0, 12000.0, -30000.0}) for (double jit : {0.0, 0.03}) {
        // PT2262-like: T = 350 us, 4 frames
        { Ook o{fs, lo, noiseFor(25, 100000, fs), repeat(pwm(bitsPwm, 350e-6, jit, 3), 4, 0.0)}; auto x = runChunks(x40::OokPulse, fs, 0.6 + 4 * 0.0165, std::ref(o), 0, 433.92e6, 0.2); auto f = frames(x);
          CHECK(f.size() >= 3, "PWM lo %.0f jit %.2f: %zu frames", lo, jit, f.size());
          for (auto& fr : f) { CHECK(fr.first.rfind("PWM 24 bits", 0) == 0, "PWM id '%s'", fr.first.c_str()); CHECK(fr.second == bitsPwm, "PWM bits %s want %s", fr.second.c_str(), bitsPwm.c_str()); }
          const std::string tm = factStr(x, "timing"); const double sp = atof(strstr(tm.c_str(), "short pulse ") + 12), lp = atof(strstr(tm.c_str(), "long pulse ") + 11);
          CHECK(std::fabs(sp - 350) < 40 && std::fabs(lp - 1050) < 90, "PWM timing %s", tm.c_str());
          CHECK(atoi(factStr(x, "repeats of this frame").c_str()) >= 3, "repeats %s", factStr(x, "repeats of this frame").c_str()); }
        // PPM: pulse 300 us, gaps 500 / 1000 us
        { Ook o{fs, lo, noiseFor(25, 100000, fs), repeat(ppm(bitsPpm, 300e-6, 500e-6, 1000e-6, jit, 4), 3, 0.015)}; auto x = runChunks(x40::OokPulse, fs, 0.5 + 3 * 0.06, std::ref(o), 0, 433.92e6, 0.2); auto f = frames(x);
          CHECK(f.size() >= 2, "PPM: %zu frames", f.size());
          for (auto& fr : f) { CHECK(fr.first.rfind("PPM", 0) == 0, "PPM id '%s'", fr.first.c_str()); CHECK(fr.second == bitsPpm, "PPM bits %s", fr.second.c_str()); } }
        // Manchester at 2 kbit/s (half bit 250 us)
        { Ook o{fs, lo, noiseFor(25, 100000, fs), repeat(manchester(bitsMan, 250e-6, jit, 5), 3, 0.015)}; auto x = runChunks(x40::OokPulse, fs, 0.5 + 3 * 0.06, std::ref(o), 0, 433.92e6, 0.2); auto f = frames(x);
          CHECK(f.size() >= 2, "Manchester: %zu frames", f.size());
          for (auto& fr : f) { CHECK(fr.first.rfind("Manchester", 0) == 0, "Manchester id '%s'", fr.first.c_str()); CHECK(fr.second.size() >= bitsMan.size() - 1 && fr.second.compare(0, bitsMan.size() - 1, bitsMan, 0, bitsMan.size() - 1) == 0, "Manchester bits %s want %s", fr.second.c_str(), bitsMan.c_str()); } }
    }
    // noise sweep: PWM frames found of 4 and bits right (carrier to noise in 100 kHz)
    printf("noise sweep (carrier to noise in 100 kHz): PWM frames found of 4 / bit errors\n");
    for (double cnr : {30.0, 20.0, 15.0, 12.0, 9.0}) { Ook o{fs, 0, noiseFor(cnr, 100000, fs), repeat(pwm(bitsPwm, 350e-6, 0.0, 3), 4, 0.0)}; auto x = runChunks(x40::OokPulse, fs, 0.6 + 4 * 0.0165, std::ref(o), 0, 433.92e6, 0.2); auto f = frames(x);
        int good = 0, bad = 0; for (auto& fr : f) { if (fr.second == bitsPwm) good++; else bad++; } printf("  %.0f dB: %d / %d wrong\n", cnr, good, bad); if (cnr >= 15) CHECK(good >= 3 && bad == 0, "%.0f dB: %d good %d bad", cnr, good, bad); }
    // noise alone and a plain carrier
    { Ook o{fs, 0, 1.0, {{1000.0, false}}}; auto x = runChunks(x40::OokPulse, fs, 20.0, std::ref(o), 0, 433.92e6, 1.0); CHECK(countKind(x, "Frame") == 0, "noise: %d frames", countKind(x, "Frame")); }
    { Ook o{fs, 5000, noiseFor(30, 100000, fs), {{100.0, true}}}; auto x = runChunks(x40::OokPulse, fs, 10.0, std::ref(o), 0, 433.92e6, 1.0); CHECK(countKind(x, "Frame") == 0, "carrier: %d frames", countKind(x, "Frame")); }
    printf(fails ? "x40 ookpulse: %d FAILED\n" : "x40 ookpulse: ok\n", fails);
    return fails ? 1 : 0;
}
