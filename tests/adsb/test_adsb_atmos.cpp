// ADS-B atmosphere sounding and altimetry cross-check (adsb_atmos.h). Simulated aircraft reports from a model atmosphere, with noise.
#include "dect2/adsb/adsb_atmos.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

// model: surface 10 C, 6.5 K/km to 11 km, isothermal above; wind from 270, 10 kt + 11 kt/km up to 10 km (120 kt), weaker above
static double modelT(double km) { return km < 11 ? 10 - 6.5 * km : 10 - 6.5 * 11; }
static double modelW(double km) { return km < 10 ? 10 + 11 * km : std::max(30.0, 120 - 25 * (km - 10)); }

int main() {
    for (double tNoise : {0.5, 1.5, 3.0}) {   // noise sweep on the temperature and the wind
        std::mt19937 r(7); std::normal_distribution<double> g(0, 1); std::uniform_real_distribution<double> u(0, 1);
        AdsbSounding s;
        double now = 0;
        for (int i = 0; i < 1800; i++) {      // one report a second, 60 aircraft at random levels, 30 minutes
            now = i;
            const uint32_t icao = 0x400000 + (uint32_t)(i % 60);
            const int alt = (int)(1000 + 41000 * u(r));
            const double km = alt * 0.3048 / 1000;
            s.addSample(icao, alt, true, 270 + 8 * g(r) * tNoise / 1.5, modelW(km) + 4 * g(r) * tNoise / 1.5, true, modelT(km) + tNoise * g(r), now);
        }
        const AtmosProfile p = s.profile(now);
        CHECK(p.aircraft == 60, "aircraft %d", p.aircraft);
        CHECK(p.layers.size() >= 38, "layers %zu", p.layers.size());
        const double frzFt = 10 / 6.5 * 1000 / 0.3048;   // 5048 ft
        CHECK(p.hasFreezing && std::fabs(p.freezingFt - frzFt) < 900, "noise %.1f: freezing %d ft (model %.0f)", tNoise, p.freezingFt, frzFt);
        CHECK(p.hasTropo && std::fabs(p.tropoFt - 36089) < 2000, "noise %.1f: tropopause %d ft", tNoise, p.tropoFt);
        CHECK(p.hasJet && std::fabs(p.jetFt - 32800) < 2600 && std::fabs(p.jetFromDeg - 270) < 8 && p.jetKt > 100, "noise %.1f: jet %d ft %.0f kt from %.0f", tNoise, p.jetFt, p.jetKt, p.jetFromDeg);
        for (const auto& L : p.layers) if (L.baseFt == 20000) {
            const double km = 20500 * 0.3048 / 1000;
            CHECK(std::fabs(L.windKt - modelW(km)) < 8 && std::fabs(L.windFromDeg - 270) < 6, "FL200 wind %.0f kt from %.0f", L.windKt, L.windFromDeg);
            CHECK(std::fabs(L.isaDevC - (modelT(km) - isaTempC(20500))) < 2, "FL200 ISA deviation %.1f", L.isaDevC);
        }
    }
    {   // wind direction wrap: from 355 and 5 averages to 0, not 180
        AdsbSounding s; s.addSample(1, 30500, true, 355, 50, false, 0, 0); s.addSample(2, 30500, true, 5, 50, false, 0, 1);
        const auto p = s.profile(2);
        CHECK(p.layers.size() == 1 && (p.layers[0].windFromDeg < 1 || p.layers[0].windFromDeg > 359) && p.layers[0].windKt > 49, "wrap %.1f", p.layers[0].windFromDeg);
    }
    {   // a standard-atmosphere sounding with no temperature inversion below 5 km: no false tropopause low down; samples expire
        AdsbSounding s; s.windowSec = 600;
        for (int i = 0; i < 40; i++) s.addSample(10 + i, 1000 + i * 1000, false, 0, 0, true, isaTempC(1500 + i * 1000), i);
        auto p = s.profile(40);
        CHECK(p.hasTropo && std::fabs(p.tropoFt - 36500) < 1500, "ISA tropopause %d", p.tropoFt);
        CHECK(s.profile(40 + 700).layers.empty(), "expired");
        // one aircraft repeating inside a minute adds one sample
        AdsbSounding s2; for (int i = 0; i < 50; i++) s2.addSample(5, 10500, true, 90, 20, false, 0, i);
        CHECK(s2.samples() == 1, "dedup %zu", s2.samples());
    }

    // ---- altimetry
    for (double slope : {0.0, 0.03, -0.02}) {   // fleets in warm, standard and cold air
        std::mt19937 r(3); std::normal_distribution<double> g(0, 1);
        AdsbAltCheck c; std::vector<AltFlag> fl; std::vector<AltShift> sh;
        for (int step = 0; step < 240; step++) {   // 20 minutes, every aircraft every 5 s
            for (int k = 0; k < 12; k++) {
                AdsbAircraft a; a.icao = 0x500000 + k; a.hasAlt = true; a.altFt = 24000 + 300 * k; a.hasGnssDiff = true;
                double d = 150 + slope * a.altFt + 20 * g(r);
                if (k == 4) d += 420;                    // one aircraft off
                a.gnssDiffFt = (int)std::lround(d / 25) * 25;   // 25 ft steps as sent
                if (k == 4) a.callsign = "TEST4";
                c.add(a, step * 5.0 + k * 0.1, &fl, &sh);
            }
        }
        int other = 0, four = 0; for (auto& f : fl) (f.icao == 0x500004 ? four : other)++;
        CHECK(four == 1 && other == 0, "slope %.2f: flags for the faulty %d, others %d", slope, four, other);
        CHECK(sh.empty(), "no shift %zu", sh.size());
        if (!fl.empty()) CHECK(std::fabs(fl[0].offsetFt - 420) < 80, "offset %.0f", fl[0].offsetFt);
    }
    {   // the whole band jumps 600 ft together at t = 600 s: one shift, no single-aircraft flag
        std::mt19937 r(4); std::normal_distribution<double> g(0, 1);
        AdsbAltCheck c; std::vector<AltFlag> fl; std::vector<AltShift> sh;
        for (int step = 0; step < 240; step++)
            for (int k = 0; k < 8; k++) {
                AdsbAircraft a; a.icao = 0x600000 + k; a.hasAlt = true; a.altFt = 33000 + 200 * k; a.hasGnssDiff = true;
                const double t = step * 5.0 + k * 0.2;
                a.gnssDiffFt = (int)std::lround((400 + 15 * g(r) + (t >= 600 ? 600 : 0)) / 25) * 25;
                c.add(a, t, &fl, &sh);
            }
        CHECK(sh.size() == 1 && sh[0].shiftFt > 450, "shift %zu %.0f", sh.size(), sh.empty() ? 0 : sh[0].shiftFt);
        CHECK(fl.empty(), "no aircraft blamed %zu", fl.size());
    }
    {   // too few aircraft to compare: nothing
        AdsbAltCheck c; std::vector<AltFlag> fl;
        for (int step = 0; step < 100; step++) for (int k = 0; k < 3; k++) {
            AdsbAircraft a; a.icao = 0x700000 + k; a.hasAlt = true; a.altFt = 20000; a.hasGnssDiff = true; a.gnssDiffFt = k == 0 ? 1000 : 200;
            c.add(a, step * 5.0 + k, &fl);
        }
        CHECK(fl.empty(), "two others are not a fleet");
    }
    return testSummary("adsb_atmos");
}
