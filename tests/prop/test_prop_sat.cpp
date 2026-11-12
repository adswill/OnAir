// Satellite links: GEO look angles against the closed-form spherical result, LEO range rate / Doppler at the horizon (ISS, 2 m),
// a Ku-band DVB-S2 downlink budget against a hand calculation, and the measured mode thresholds table.
#include "dect2/prop/mode_snr.h"
#include "dect2/prop/prop_scenario.h"
#include "dect2/prop/sat_link.h"
#include <cmath>
#include <cstdio>
using namespace dect2::prop;

static int fails = 0;
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { printf("FAIL line %d: %s = %.4f, want %.4f +- %g\n", __LINE__, #a, _a, _b, (double)(tol)); fails++; } } while (0)
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

int main() {
    // directly under a GEO satellite: zenith, 35786 km
    { const sat::Look k = sat::look(0, 0, 0, 0, 0, 35786); NEAR(k.elevDeg, 90, 0.01); NEAR(k.rangeKm, 35786, 0.5); }
    // London to 28.2 E (spherical closed form: elevation 25.4, azimuth 145.6, range 39031 km; WGS 84 moves them slightly)
    { const sat::Look k = sat::look(51.5, 0, 0, 0, 28.2, 35786); NEAR(k.elevDeg, 25.4, 0.4); NEAR(k.azDeg, 145.6, 0.6); NEAR(k.rangeKm, 39031, 40); }
    // a satellite on the other side of the earth is below the horizon
    CHECK(sat::look(51.5, 0, 0, 0, 180, 35786).elevDeg < 0);
    // ISS (420 km): 7.66 km/s; at the horizon 7.20 km/s toward the station, overhead 0; 3.5 kHz Doppler on 145.8 MHz
    NEAR(sat::orbitalSpeedKmS(420), 7.66, 0.01);
    NEAR(sat::rangeRateKmS(420, 0), 7.20, 0.05);
    NEAR(sat::rangeRateKmS(420, 90), 0.0, 1e-3);
    NEAR(sat::dopplerHz(145.8, sat::rangeRateKmS(420, 0)), 3500, 50);
    NEAR(sat::slantRangeKm(420, 90), 420, 1e-6);
    NEAR(sat::slantRangeKm(420, 0), 2351, 2);
    // Ku-band TV downlink: 52 dBW EIRP, 0.8 m dish (37.6 dBi), 1 dB LNB, 36 MHz, London, 28.2 E: clear sky C/N about 16.7 dB
    {
        sat::Link l;
        l.fGHz = 11.7; l.eirpDbw = 52; l.rxGainDbi = 37.6; l.rxNoiseFigureDb = 1; l.antennaM = 0.8; l.stLat = 51.5;
        l.elevDeg = 25.4; l.rangeKm = 39031; l.bandwidthHz = 36e6; l.rainR001 = 28; l.availabilityPct = 99.9; l.requiredSnrDb = 3;
        const sat::Budget b = sat::budget(l);
        NEAR(b.fsplDb, 205.64, 0.05);
        CHECK(b.gasDb > 0.05 && b.gasDb < 0.4);
        NEAR(b.clearSkyCnDb, 16.7, 1.5);
        CHECK(b.rainDb > 0.5 && b.rainDb < 6);            // P.618 at 0.1 % of the year, R0.01 = 28 mm/h
        CHECK(b.cnDb < b.clearSkyCnDb);
        l.availabilityPct = 99.99;
        CHECK(sat::budget(l).rainDb > b.rainDb);           // a stricter availability needs a bigger rain margin
    }
    // the measured thresholds
    {
        const ModeSnr* m = findModeThreshold("dvbs2");
        CHECK(m && m->awgnDb == 3 && m->bwHz == 5e6);
        const ModeSnr* f = findModeThreshold("ft8");
        CHECK(f && f->awgnDb == -21);
        CHECK(!findModeThreshold("nope"));
        const ModeSnr* d = findModeThreshold("dmr");
        CHECK(d && modeRequiredSnrDb(*d, 0) == 5 && modeRequiredSnrDb(*d, 1) == 3 && modeRequiredSnrDb(*d, 2) == 9);
        CHECK(m && modeRequiredSnrDb(*m, 2) == 3);       // not measured with fading: falls back
        CHECK(modeThresholds().size() >= 40);
    }
    // antenna and cable presets (typical datasheet values: RG-58 16 dB / 100 m at 100 MHz, LMR-400 13.4 dB / 100 m at 1 GHz)
    {
        const auto& cs = cablePresets();
        const CablePreset* rg58 = nullptr; const CablePreset* lmr = nullptr;
        for (const auto& c : cs) { if (std::string(c.name) == "RG-58") rg58 = &c; if (std::string(c.name).find("LMR-400") == 0) lmr = &c; }
        CHECK(rg58 && lmr);
        if (rg58) { NEAR(cableLossDb(*rg58, 100, 100), 16, 0.3); NEAR(cableLossDb(*rg58, 145, 20), 3.9, 0.3); }
        if (lmr) NEAR(cableLossDb(*lmr, 1000, 100), 13.4, 0.3);
        CHECK(antennaPresets().size() >= 10);
        for (const auto& a : antennaPresets()) CHECK(a.gainDbi > -30 && a.gainDbi < 30);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("prop_sat: all passed\n");
    return 0;
}
