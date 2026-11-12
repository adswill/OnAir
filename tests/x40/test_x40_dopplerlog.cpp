// Satellite Doppler curve logger: the pass of a circular-orbit satellite (550 km and 800 km) past an observer is generated from the geometry (range rate of the true orbit, not the fit
// model), with a radio offset and drift, noise and fades; the logger's time of closest approach, no-Doppler carrier and steepest slope are compared with the geometry's. Simulated
// signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// geometry: observer at cross-track angle beta from the orbit plane, satellite at orbit radius Ro; Doppler of carrier f0 (Hz) at time t (TCA at tca)
struct Orbit {
    double hKm, betaDeg, f0, tca; double Re = 6371.0, mu = 398600.4418;
    double Ro() const { return Re + hKm; }
    double omega() const { return std::sqrt(mu / (Ro() * Ro() * Ro())); }
    double rangeRate(double t) const {
        const double th = omega() * (t - tca), b = betaDeg * kPi / 180, Rr = Ro(), Vs = omega() * Rr;
        const double sx = Rr * std::cos(th), sy = Rr * std::sin(th), ox = Re * std::cos(b), oz = Re * std::sin(b);
        const double dx = sx - ox, dy = sy, dz = -oz, rho = std::sqrt(dx * dx + dy * dy + dz * dz);
        return (dx * (-Vs * std::sin(th)) + dy * (Vs * std::cos(th))) / rho;       // km/s
    }
    double rho(double t) const { const double th = omega() * (t - tca), b = betaDeg * kPi / 180; const double dx = Ro() * std::cos(th) - Re * std::cos(b), dy = Ro() * std::sin(th), dz = -Re * std::sin(b); return std::sqrt(dx * dx + dy * dy + dz * dz); }
    double doppler(double t) const { return -f0 * rangeRate(t) * 1000 / 299792458.0; }
    double elevation(double t) const { const double b = betaDeg * kPi / 180; const double th = omega() * (t - tca); const double ox = Re * std::cos(b), oz = Re * std::sin(b); const double sx = Ro() * std::cos(th), sy = Ro() * std::sin(th); const double dx = sx - ox, dy = sy, dz = -oz; const double up = (dx * std::cos(b) + dz * std::sin(b)); return std::asin(up / std::sqrt(dx * dx + dy * dy + dz * dz)) * 180 / kPi; }
};

struct Pass {
    Orbit o; double fs, lo, loDrift, noise, fadeDb, tStart, tEnd; Rng rng{7}; double ph = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs; cf32 v(0, 0);
            if (t >= tStart && t < tEnd && o.elevation(t) > 3.0) {
                const double f = lo + loDrift * t + o.doppler(t);
                ph += 2 * kPi * f / fs; const double amp = 0.1 * std::pow(10.0, fadeDb * std::sin(2 * kPi * 0.31 * t) / 20);
                v = (float)amp * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            x[i] = v + cf32(sg * rng.g(), sg * rng.g());
        }
        return x;
    }
};

int main() {
    const double fs = 48000, f0 = 437.5e6;
    struct Case { double h, beta, lo, drift, cn0, fade; };
    for (Case c : {Case{550, 5, 0, 0, 50, 0}, Case{550, 15, 1200, 0.3, 45, 3}, Case{800, 8, -1000, -0.2, 45, 4}, Case{550, 12, 300, 0, 45, 0}}) {
        Orbit o{c.h, c.beta, f0, 400.0};
        // the pass lasts while the elevation is above 3 degrees: find it
        double ts = 400, te = 400; while (o.elevation(ts) > 3.0) ts -= 1; while (o.elevation(te) > 3.0) te += 1;
        Pass p{o, fs, c.lo, c.drift, fs / std::pow(10.0, c.cn0 / 10), c.fade, ts, te};
        auto t = runChunks(x40::DopplerLog, fs, te + 60.0, std::ref(p), 0, f0, 1.0);
        // the truth: TCA 400 s, the steepest slope from the geometry, the no-Doppler carrier = lo (+ drift at TCA)
        double steep = 0; for (double tt = ts; tt < te; tt += 0.25) steep = std::max(steep, std::fabs(o.doppler(tt + 0.25) - o.doppler(tt)) / 0.25);
        std::string pass; double tca = -1, fc = 0, span = 0, slope = 0, rms = 0;
        for (auto& r : t.rows) if (r.kind == "Pass" && r.id.rfind("TCA", 0) == 0) { pass = r.text; sscanf(r.id.c_str(), "TCA %lf", &tca); const char* q = strstr(r.text.c_str(), "carrier "); if (q) sscanf(q + 8, "%lf Hz; Doppler span +-%lf Hz; steepest %lf Hz/s at TCA; fit %lf", &fc, &span, &slope, &rms); }
        CHECK(!pass.empty(), "h %.0f beta %.0f: no pass summary (%s, rows %zu)", c.h, c.beta, t.headline.c_str(), t.rows.size());
        if (pass.empty()) continue;
        printf("  h %.0f beta %.0f true closest range %.0f km: TCA %.1f (true 400), no-Doppler %+.1f (true %+.1f), steepest %.1f (true %.1f), fit %.1f Hz RMS, %s\n", c.h, c.beta, o.rho(400), tca, fc, c.lo + c.drift * 400, slope, steep, rms, pass.substr(pass.find("speed") == std::string::npos ? 0 : pass.find("speed")).c_str());
        CHECK(std::fabs(tca - 400.0) < 3.0, "TCA %.1f", tca);
        CHECK(std::fabs(fc - (c.lo + c.drift * 400)) < 40.0, "no-Doppler carrier %.1f want %.1f", fc, c.lo + c.drift * 400);
        CHECK(std::fabs(slope - steep) < 0.15 * steep, "steepest slope %.1f want %.1f", slope, steep);
    }
    // a short carrier (30 s) is "short"; noise only logs nothing
    { ToneScene sc(fs, noiseFor(30, 1000, fs), 8); SchedTone a; a.hz = 2000; a.from = 20; a.to = 34; sc.tones.push_back(a); auto t = runChunks(x40::DopplerLog, fs, 100.0, std::ref(sc), 0, f0, 1.0); CHECK(countKind(t, "Pass") == 2, "short pass rows: %d", countKind(t, "Pass")); }
    { ToneScene sc(fs, 1.0, 3); auto t = runChunks(x40::DopplerLog, fs, 900.0, std::ref(sc), 0, f0, 5.0); CHECK(t.rows.empty(), "noise only: %zu rows", t.rows.size()); }
    printf(fails ? "x40 dopplerlog: %d FAILED\n" : "x40 dopplerlog: ok\n", fails);
    return fails ? 1 : 0;
}
