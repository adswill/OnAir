// Iridium Doppler identification (iridium_doppler.h) against a synthetic 66-satellite constellation: which satellite sent a burst, the receiver's frequency
// error, and the beam circle. Noise, a frequency error of the receiver, time stamp errors and TLEs that are a little wrong are swept.
#include "dect2/iridium/iridium_doppler.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const double kT0 = 1.79e9;   // some day in November 2026
static std::vector<Tle> constellation(double dM = 0, double dN = 0, double dRaan = 0) {
    std::vector<Tle> v;
    int norad = 60000;
    for (int p = 0; p < 6; p++)
        for (int k = 0; k < 11; k++) {
            Tle t;
            t.name = "IRIDIUM " + std::to_string(100 + p * 11 + k);
            t.norad = norad++;
            t.epochJd = unixToJd(kT0);
            t.inclDeg = 86.4; t.raanDeg = std::fmod(p * 31.6 + dRaan, 360.0); t.ecc = 0.0002; t.argpDeg = 90;
            t.meanAnomDeg = std::fmod(k * (360.0 / 11) + p * 16.36 + dM, 360.0);
            t.meanMotion = 14.3426 + dN;
            v.push_back(t);
        }
    return v;
}

struct Case { double sigma, offset, timeErr, dM, dN; };

struct Out { int right = 0, wrong = 0, none = 0; double offErr = 0; int offN = 0; };

static Out trial(const Case& c, int seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, c.sigma), tn(0, c.timeErr);
    std::uniform_real_distribution<double> u(0, 1);
    const Observer ob{55.0, 24.0, 0.1};
    const auto truthT = constellation(c.dM, c.dN, 0), known = constellation();
    std::vector<Sgp4> truth(truthT.size());
    for (size_t i = 0; i < truth.size(); i++) truth[i].init(truthT[i]);
    IridiumDoppler d;
    d.setObserver(ob);
    d.setTles(known);
    const double t0 = kT0 + 3600 * 5 + seed * 977.0;
    // the satellites above 10 degrees at t0 + 150 s; the first four get header numbers
    std::vector<size_t> vis;
    for (size_t i = 0; i < truth.size(); i++) { Look lk; if (lookAt(truth[i], ob, t0 + 150, lk) && lk.elDeg > 10) vis.push_back(i); }
    std::vector<std::pair<int, size_t>> assign;
    for (size_t k = 0; k < vis.size() && k < 5; k++) assign.push_back({20 + (int)k * 7, vis[k]});
    for (double t = t0; t < t0 + 300; t += 1.0)
        for (const auto& a : assign) {
            if (u(rng) > 0.1) continue;   // about one burst in ten seconds per satellite
            Look lk;
            if (!lookAt(truth[a.second], ob, t, lk) || lk.elDeg < 8) continue;
            d.addBurst(t + tn(rng), a.first, kIridiumRingHz + dopplerHz(kIridiumRingHz, lk.rangeRateKmS) + c.offset + noise(rng));
        }
    Out o;
    const auto id = d.identify(t0 + 300);
    for (const auto& a : assign) {
        bool found = false;
        for (const auto& i : id) if (i.satId == a.first) { found = true; if (i.norad == truthT[a.second].norad) o.right++; else o.wrong++; }
        if (!found) o.none++;
    }
    if (d.offsetKnown()) { o.offErr = std::fabs(d.offsetHz() - c.offset); o.offN = 1; }
    return o;
}

int main() {
    // sanity: the constellation has satellites in view at all
    {
        const auto t = constellation();
        std::vector<Sgp4> s(t.size());
        int in = 0;
        for (size_t i = 0; i < t.size(); i++) { s[i].init(t[i]); Look lk; if (lookAt(s[i], Observer{55, 24, 0.1}, kT0 + 18000, lk) && lk.elDeg > 8) in++; }
        CHECK(in >= 1, "satellites in view: %d", in);
    }
    printf("which satellite sent it (5 numbers, bursts for 5 min, one per ~10 s each; mean of 12 runs):\n");
    printf("  noise  rx offset  time err  TLE error | right  wrong  not identified | offset error\n");
    const Case cases[] = {
        {100, 0, 0.0, 0, 0}, {300, 3000, 0.5, 0, 0}, {300, -8000, 1.0, 0, 0}, {600, 3000, 1.0, 0, 0}, {1000, 3000, 1.0, 0, 0},
        {300, 3000, 0.5, 0.03, 0.0002}, {300, 3000, 0.5, 0.1, 0.0005}, {300, 3000, 1.0, 0.3, 0.001}};
    double goodRight = 0, goodTotal = 0, sweepWrong = 0;
    for (const Case& c : cases) {
        Out sum; int n = 0; double offE = 0; int offN = 0;
        for (int s = 0; s < 12; s++) { const Out o = trial(c, 100 + s); sum.right += o.right; sum.wrong += o.wrong; sum.none += o.none; n += o.right + o.wrong + o.none; offE += o.offErr; offN += o.offN; }
        printf("  %4.0f  %8.0f  %7.1f  %.2f deg %.4f | %4.0f%%  %4.0f%%  %6.0f%%         | %.0f Hz\n", c.sigma, c.offset, c.timeErr, c.dM, c.dN, 100.0 * sum.right / n, 100.0 * sum.wrong / n, 100.0 * sum.none / n, offN ? offE / offN : -1);
        sweepWrong += sum.wrong;
        if (c.dM <= 0.03 && c.sigma <= 600) { goodRight += sum.right; goodTotal += n; }
    }
    CHECK(goodRight / goodTotal >= 0.85, "identification rate with good TLEs: %.0f%%", 100 * goodRight / goodTotal);
    CHECK(sweepWrong <= 12 * 5 * 0.06, "wrong identifications over the whole sweep: %.0f", sweepWrong);

    // too little data: a point is not a curve
    {
        IridiumDoppler d; d.setObserver({55, 24, 0.1}); d.setTles(constellation());
        d.addBurst(kT0 + 5, 3, kIridiumRingHz + 1000);
        d.addBurst(kT0 + 6, 3, kIridiumRingHz + 1010);
        CHECK(d.identify(kT0 + 10).empty(), "two bursts identify nothing");
        d.addBurst(kT0 + 5, 4, 1620e6);   // not the ring channel
        d.addBurst(kT0 + 5, 200, kIridiumRingHz);
        CHECK(d.identify(kT0 + 10).empty(), "off-channel and bad numbers ignored");
    }
    // no TLEs, no observer: quiet
    {
        IridiumDoppler d;
        d.addBurst(kT0, 1, kIridiumRingHz);
        CHECK(d.identify(kT0).empty() && d.sky(kT0).empty(), "nothing without TLEs");
        std::vector<Tle> deb = constellation();
        for (auto& t : deb) t.name = "IRIDIUM 33 DEB";
        d.setTles(deb);
        CHECK(d.satellites() == 0, "debris is not a satellite");
    }
    // the sky: the satellites above the horizon, and an identified one is marked
    {
        IridiumDoppler d; d.setObserver({55, 24, 0.1}); d.setTles(constellation());
        const auto sk = d.sky(kT0 + 18000);
        CHECK(sk.size() >= 1 && sk.size() <= 25, "sky has %zu satellites", sk.size());
        for (const auto& p : sk) CHECK(p.elDeg >= 0 && p.azDeg >= 0 && p.azDeg < 360, "sky point");
    }
    // the beam circle: carried along with the satellite, and old ones are refused
    {
        const Observer ob{55, 24, 0.1};
        IridiumDoppler d; d.setObserver(ob); d.setTles(constellation());
        const auto t = constellation();
        Sgp4 s; s.init(t[5]);
        const double ta = kT0 + 18000;
        double la, lo, al;
        subPoint(s, ta, la, lo, al);
        d.addBeam(ta, 5, 17, la, lo);
        auto c0 = d.beamCircle(5, 17, ta + 0.5);
        CHECK(c0.valid && std::fabs(c0.lat - la) < 0.01, "beam fresh");
        CHECK(!d.beamCircle(5, 17, ta + 500).valid, "old beam refused");
        CHECK(!d.beamCircle(5, 18, ta).valid, "other beam unknown");
        auto c1 = d.beamCircle(5, 17, ta + 60);
        CHECK(c1.valid && !c1.moved && c1.radiusKm > 600, "unidentified: the circle grows, %.0f km", c1.radiusKm);
    }
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
