// ITU-R P.1812-8 against the ITU-R Study Group 3 validation examples (P_1812_8.zip): 7 published profiles (sea / land, horizontal /
// vertical, 1 to 110 km, line of sight and trans-horizon, with and without clutter), 3 time percentages each, the basic transmission loss
// and the published intermediate values. Plus behaviour checks: location percentages, the flat-profile limit, frequency trend.
#include "dect2/prop/p1812.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2::prop;

static int fails = 0;
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { printf("FAIL line %d (%s): %s = %.4f, want %.4f +- %g\n", __LINE__, c.name, #a, _a, _b, (double)(tol)); fails++; } } while (0)
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

#include "p1812_cases.inc"

// the published results of one profile use a different frequency for each time percentage
static double freqOf(const P1812Case& c, int k) {
    static const double urban[3] = {0.03, 0.09, 0.5};
    return std::string(c.name) == "rburg_urban_with_clutter" ? urban[k] : c.f;
}

static p1812::Input inputOf(const P1812Case& c, double p) {
    p1812::Input in;
    for (int i = 0; i < c.nprof; i++) { in.dKm.push_back(c.prof[3 * i]); in.hM.push_back(c.prof[3 * i + 1]); in.clutterM.push_back(c.prof[3 * i + 2]); }
    in.fGHz = c.f; in.timePct = p; in.locPct = c.pL; in.txHeightM = c.htg; in.rxHeightM = c.hrg; in.vertical = c.pol == 2;
    in.latDeg = c.lat; in.deltaN = c.dN; in.n0 = c.n0; in.seaFraction = c.w; in.dtmKm = c.dtm; in.dlmKm = c.dlm; in.sigmaLocDb = c.sigL;
    return in;
}

int main() {
    int n = 0;
    double worst = 0;
    for (const P1812Case& c : kP1812Cases) {
        for (int k = 0; k < 3; k++) {
            p1812::Input in = inputOf(c, c.p[k]);
            in.fGHz = freqOf(c, k);
            const p1812::Result r = p1812::compute(in);
            CHECK(r.valid);
            NEAR(r.lb, c.lb[k], 0.02);
            worst = std::max(worst, std::fabs(r.lb - c.lb[k]));
            n++;
            if (k == 0) {
                NEAR(r.dlt, c.dlt, 1e-6); NEAR(r.dlr, c.dlr, 1e-6);
                NEAR(r.tht, c.tht, 0.01); NEAR(r.thr, c.thr, 0.01);
                NEAR(r.beta0, c.b0, 0.005);
                NEAR(r.hstd, c.hstd, 0.01); NEAR(r.hsrd, c.hsrd, 0.01);
                NEAR(r.hte, c.hte, 0.01); NEAR(r.hre, c.hre, 0.01); NEAR(r.hm, c.hm, 0.01);
                NEAR(r.lbulla, c.lbulla, 0.01); NEAR(r.ldsph, c.ldsph, 0.01);
                NEAR(r.lbs, c.lbs, 0.01); NEAR(r.lba, c.lba, 0.01);
            }
        }
    }
    printf("P.1812-8 validation: %d cases, worst |Lb - ITU| = %.4f dB\n", n, worst);

    // behaviour: more locations covered costs margin (90 % of locations = +1.28 sigma), 10 % gains it
    {
        const P1812Case& c = kP1812Cases[2];
        p1812::Input in = inputOf(c, 50);
        in.sigmaLocDb = 5.5;
        in.locPct = 50; const double l50 = p1812::compute(in).lb;
        in.locPct = 90; const double l90 = p1812::compute(in).lb;
        in.locPct = 10; const double l10 = p1812::compute(in).lb;
        NEAR(l90 - l50, 1.2816 * 5.5, 0.05);
        CHECK(l10 < l50);
    }
    // a flat sea-level profile: line of sight close in (about free space), trans-horizon far out, loss rising with frequency
    {
        const struct { const char* name; } c{"flat"};
        auto flat = [](double D, double f, double p) {
            p1812::Input in;
            for (int i = 0; i <= 100; i++) { in.dKm.push_back(D * i / 100.0); in.hM.push_back(0); }
            in.fGHz = f; in.timePct = p; in.txHeightM = 30; in.rxHeightM = 10; in.latDeg = 50;
            return p1812::compute(in);
        };
        const p1812::Result a = flat(5, 0.5, 50);
        CHECK(a.los);
        NEAR(a.lb - a.lbfs, 0.0, 6.0);
        const p1812::Result b = flat(80, 0.5, 50);
        CHECK(!b.los);
        CHECK(b.lb > b.lbfs + 15);
        CHECK(flat(80, 2.0, 50).lb > b.lb);
        CHECK(flat(80, 0.5, 1).lb < b.lb);   // 1 % of the time: enhanced (ducting / super-refraction)
        CHECK(p1812::knifeEdgeJ(0) > 6.0 && p1812::knifeEdgeJ(0) < 6.1);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("prop_p1812: all passed\n");
    return 0;
}
