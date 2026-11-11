// Occupied bandwidth meter: PSK signals with root-raised-cosine pulses (known raised-cosine spectra: the 99 % and the x dB bandwidths are integrated / solved in the test), with a carrier offset and a
// neighbour at a known level; a noise sweep; no signal. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// PSD of an RRC-shaped PSK: the raised cosine |H(f)|^2 (rs symbol rate, alpha roll-off), per Hz
static double rc(double f, double rs, double a) { f = std::fabs(f); const double f1 = (1 - a) * rs / 2, f2 = (1 + a) * rs / 2; if (f <= f1) return 1; if (f >= f2) return 0; return 0.5 * (1 + std::cos(kPi / (a * rs) * (f - f1))); }
static double obw99(double rs, double a) { double tot = 0; const double df = rs / 20000; for (double f = -rs; f < rs; f += df) tot += rc(f, rs, a) * df; double c = 0, lo = 0; for (double f = -rs; f < rs; f += df) { c += rc(f, rs, a) * df; if (c >= 0.005 * tot) { lo = f; break; } } return -2 * lo; }
static double bwDb(double rs, double a, double dB) { double hi = 0; const double th = std::pow(10.0, -dB / 10); for (double f = 0; f < rs; f += rs / 20000) if (rc(f, rs, a) >= th) hi = f; return 2 * hi; }
static double fnum(const X40Telemetry& t, const char* key) { return factNum(t, key); }
static double nth(const std::string& s, int k) { const char* p = s.c_str(); for (int i = 0; i < k; i++) { p = strchr(p, '/'); if (!p) return -999; p++; } return atof(p); }

int main() {
    const double fs = 1e6, centre = 433.92e6;
    for (double rs : {25000.0, 100000.0}) for (double a : {0.35, 0.2}) for (double off : {0.0, 37500.0, -81234.0}) {
        auto x = mpsk(4, rs, a, fs, 6.0, off, 0, 0, 3); addNoise(x, noiseEsN0(40, rs, fs), 5);
        auto t = run(x40::OccBw, x, fs);
        const double obw = fnum(t, "occupied bandwidth (99 %)") * 1000, b26 = nth(factStr(t, "bandwidth at -20 / -26 dB"), 1), cen = factNum(t, "centre of the signal");
        const double wantObw = obw99(rs, a), want26 = bwDb(rs, a, 26);
        const std::string cs = factStr(t, "centre of the signal"); const double cHz = atof(strstr(cs.c_str(), "(") + 1);
        if (off == 0 && a == 0.35) printf("  Rs %.0f a %.2f: 99 %% %.2f kHz (theory %.2f), -26 dB %.2f kHz (%.2f), centre %+.0f Hz\n", rs, a, obw / 1000, wantObw / 1000, b26, want26 / 1000, cHz);
        CHECK(std::fabs(obw - wantObw) < 0.03 * wantObw + 300, "Rs %.0f a %.2f off %.0f: 99 %% %.0f Hz want %.0f", rs, a, off, obw, wantObw);
        CHECK(std::fabs(b26 * 1000 - want26) < 0.06 * want26 + 500, "-26 dB %.0f want %.0f", b26 * 1000, want26);
        CHECK(std::fabs(cHz - off) < 0.02 * rs + 100, "centre %.0f want %.0f", cHz, off); (void)cen;
    }
    // a neighbour of the same shape at -40 dB, centred at +1.25 occupied bandwidths: the adjacent power read -40 dB (upper), the lower side the floor
    { const double rs = 25000, a = 0.35; auto x = mpsk(4, rs, a, fs, 6.0, 0, 0, 0, 3); const double wb = obw99(rs, a); auto y = mpsk(4, rs, a, fs, 6.0, 1.25 * wb, 0, 0, 9); for (size_t i = 0; i < x.size(); i++) x[i] += (float)std::pow(10.0, -40.0 / 20) * y[i]; addNoise(x, noiseEsN0(55, rs, fs), 5);
      auto t = run(x40::OccBw, x, fs); const std::string s = factStr(t, "adjacent power, lower / upper"); const double lo = nth(s, 0), up = atof(strchr(s.c_str(), '/') + 1);
      printf("  neighbour at -40 dBc: adjacent lower / upper read %.1f / %.1f dB\n", lo, up); CHECK(std::fabs(up - 40.0) < 1.5, "upper adjacent %.1f want 40", up); CHECK(lo > 50.0, "lower adjacent %.1f (clean side)", lo); }
    // noise sweep: Es/N0 -> the 99 % bandwidth read against theory
    printf("noise sweep (Es/N0 -> 99 %% bandwidth kHz, true %.2f):", obw99(25000, 0.35) / 1000);
    for (double en : {40.0, 30.0, 20.0, 14.0, 10.0, 6.0}) { auto x = mpsk(4, 25000, 0.35, fs, 6.0, 5000, 0, 0, 3); addNoise(x, noiseEsN0(en, 25000, fs), 5); auto t = run(x40::OccBw, x, fs); const double v = fnum(t, "occupied bandwidth (99 %)");
        printf(" %.0f: %s", en, v < -900 ? "none" : (std::to_string(v).substr(0, 5)).c_str()); if (en >= 14) CHECK(std::fabs(v * 1000 - obw99(25000, 0.35)) < 0.05 * obw99(25000, 0.35), "%.0f dB: %.2f", en, v); }
    printf("\n");
    { ToneScene sc(fs, 1.0, 3); auto t = runChunks(x40::OccBw, fs, 8.0, std::ref(sc), 0, centre, 1.0); CHECK(t.headline.find("no signal") != std::string::npos, "noise: %s", t.headline.c_str()); }
    printf(fails ? "x40 occbw: %d FAILED\n" : "x40 occbw: ok\n", fails);
    return fails ? 1 : 0;
}
