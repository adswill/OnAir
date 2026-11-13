// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The atmosphere models against the ITU-R Study Group 3 validation workbook CG-3M3J-13-ValEx-Rev8.3.0 (the published examples for
// P.676-13, P.838-3 and P.618-14) and a few textbook values for P.453, P.530 and P.835.
#include "dect2/prop/itu_atmos.h"
#include <cmath>
#include <cstdio>
using namespace dect2::prop::itu;

static int fails = 0;
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { printf("FAIL line %d: %s = %.6g, want %.6g +- %g\n", __LINE__, #a, _a, _b, (double)(tol)); fails++; } } while (0)
#define REL(a, b, r) NEAR(a, b, std::fabs(b) * (r))
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

int main() {
    // P.676-13 specific attenuation (sheet "P.676-13 SpAtt"): p = 1013.25 hPa dry, e = 9.97288878634056 hPa, T = 288.15 K
    {
        const double p = 1013.25, e = 9.97288878634056, T = 288.15;
        const double f[5] = {12, 20, 60, 90, 130};
        const double go[5] = {8.6982640687735709E-3, 1.18835504778076E-2, 14.6234747964861, 3.8869711072423499E-2, 4.1509083599522802E-2};
        const double gw[5] = {9.5353882202459297E-3, 9.7047304815111701E-2, 0.15484184063624701, 0.341973394422181, 0.75184470364612899};
        for (int i = 0; i < 5; i++) {
            REL(gammaOxygenDbKm(f[i], p, e, T), go[i], 1e-5);
            REL(gammaWaterDbKm(f[i], p, e, T), gw[i], 1e-5);
        }
    }
    // P.676-13 Annex 1 slant path (sheet "A_Gas_A1_2.2.1a", example 1): 28 GHz, station at sea level, 30 degrees -> 0.47081 dB
    NEAR(slantGasDb(28, 0, 30), 0.47081173472870474, 0.003);
    // and the zenith path is shorter than the 30 degree one, roughly by sin(30)
    { const double z = slantGasDb(28, 0, 90); CHECK(z < 0.47 && z > 0.47 * 0.45); }
    // P.838-3 (sheet "P.838-3 Sp.Att"): 14.25 GHz, horizontal (tau 0); kH, kV, aH, aV and gamma at the three sites
    {
        double k, a;
        rainCoefficients(14.25, 0, 0, k, a);
        REL(k, 3.9186742018373702E-2, 1e-6); REL(a, 1.1352800825355001, 1e-6);
        rainCoefficients(14.25, 0, 90, k, a);
        REL(k, 4.3451215355383301E-2, 1e-6); REL(a, 1.05905291969976, 1e-6);
        rainCoefficients(14.25, 31.076991235657001, 0, k, a);
        REL(k, 3.9754879732931299E-2, 1e-6); REL(a, 1.12418042813791, 1e-6);
        REL(rainGammaDbKm(14.25, 26.480519999999999, 31.076991235657001, 0), 1.58130839366869, 1e-6);
        REL(rainGammaDbKm(14.25, 33.936231999999997, 40.232035996361603, 0), 2.0617321312636201, 1e-6);
    }
    // P.618-14 rain (sheet "P.618-14 A_Rain"), the published rain height used as input
    {
        struct C { double lat, hs, el, hr, R, p, A; };
        const C cs[] = {
            {51.5, 3.1382983999999003E-2, 31.076991235657001, 2.4527333333333301, 26.480519999999999, 1, 0.495317069022985},
            {51.5, 3.1382983999999003E-2, 31.076991235657001, 2.4527333333333301, 26.480519999999999, 0.1, 2.1858474220521602},
            {51.5, 3.1382983999999003E-2, 31.076991235657001, 2.4527333333333301, 26.480519999999999, 0.01, 6.7980722665477398},
            {51.5, 3.1382983999999003E-2, 31.076991235657001, 2.4527333333333301, 26.480519999999999, 0.001, 14.8998224790914},
            {41.9, 4.6122988010001503E-2, 40.232035996361603, 3.0474933333333301, 33.936231999999997, 0.01, 8.2232650085818104},
            {41.9, 4.6122988010001503E-2, 40.232035996361603, 3.0474933333333301, 33.936231999999997, 0.001, 17.6715576625202},
            {33.94, -1, 46.359692611863402, 2.5633027555555601, 27.13586832, 0.1, 1.91338757191163},
        };
        for (const C& c : cs) {
            const double hs = c.hs < 0 ? 0.0 : c.hs;
            if (c.hs < 0) continue;   // the third site's station height is not repeated here
            REL(rainSlantDb(14.25, c.el, c.lat, hs, c.R, c.p, 0, c.hr), c.A, 1e-4);
        }
    }
    // P.618-14 scintillation (sheet "P.618-14 A_Scint"): 14.25 GHz, 1 m dish, efficiency 0.65, published median Nwet
    REL(scintillationDb(14.25, 31.076991235657001, 1, 1, 50.3892622222222, 0.65), 0.261931888971004, 1e-4);
    REL(scintillationDb(14.25, 40.232035996361603, 1, 1, 61.2189004444445, 0.65), 0.22405219480480501, 1e-4);
    REL(scintillationDb(14.25, 31.076991235657001, 1, 0.1, 50.3892622222222, 0.65), 0.42284537942885703, 1e-4);
    // P.835-7 reference atmosphere: 1013.25 hPa / 288.15 K at sea level, 226.32 hPa / 216.65 K at 11 km
    NEAR(refAtmosphere(0).pHpa, 1013.25, 1e-6); NEAR(refAtmosphere(0).tK, 288.15, 1e-9);
    NEAR(refAtmosphere(11.019).tK, 216.65, 0.05); NEAR(refAtmosphere(11.019).pHpa, 226.32, 0.5);
    // P.453: N about 315..320 at sea level for the standard atmosphere; k50 = 4/3 for dN = 39.25
    { const Atmos a = refAtmosphere(0); NEAR(refractivityN(a.pHpa, a.tK, vapourPressureHpa(a.rhoGm3, a.tK)), 318.0, 6.0); }
    NEAR(k50FromDeltaN(39.25), 4.0 / 3.0, 1e-3);
    // P.530-19 rain on a terrestrial link: grows with distance and rain rate, falls with the time percentage; zero below 1 GHz
    {
        const double a = rainTerrestrialDb(23, 10, 42, 0.01), b = rainTerrestrialDb(23, 20, 42, 0.01), c = rainTerrestrialDb(23, 10, 42, 0.1);
        CHECK(a > 5 && a < 40); CHECK(b > a); CHECK(c < a);
        CHECK(rainTerrestrialDb(0.5, 10, 42, 0.01) == 0);
    }
    // I(x) of P.1812 Attachment 2
    NEAR(invQ(0.1), 1.2816, 0.001); NEAR(invQ(0.5), 0.0, 0.001); NEAR(invQ(0.9), -1.2816, 0.001);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("prop_itu: all passed\n");
    return 0;
}
