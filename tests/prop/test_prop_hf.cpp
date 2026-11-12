// HF sky-wave prediction (P.533-14 with the P.1239-4 ionosphere). What can be checked without the ITU's own program:
//  - the CCIR map evaluation against an independent evaluation of the same coefficients (scratch Python written from the P.1239 text
//    during development): the port must agree to 0.01 MHz;
//  - the physics the maps are known for: noon above night, the winter anomaly at high solar activity, the dip equator near Huancayo;
//  - foE of P.1239 section 4 against the classic values (about 3.5..4 MHz at a mid-latitude noon, about 0.6 MHz at night);
//  - the P.1240 MUF over 3000 km equal to foF2 x M(3000)F2 (that is what M(3000) means) within a few per cent;
//  - textbook band behaviour (20 m by day, 80 m at night, 10 m closed at night near solar minimum, a skip zone), the long-path method
//    beyond 9000 km and its continuity with the ray-path method across 7000..9000 km.
#include "dect2/prop/hf_p533.h"
#include <cmath>
#include <cstdio>
using namespace dect2::prop::hf;

static int fails = 0;
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { printf("FAIL line %d: %s = %.4f, want %.4f +- %g\n", __LINE__, #a, _a, _b, (double)(tol)); fails++; } } while (0)
#define INRANGE(a, lo, hi) do { const double _a = (a); if (!(_a >= (lo) && _a <= (hi))) { printf("FAIL line %d: %s = %.4f, want %g..%g\n", __LINE__, #a, _a, (double)(lo), (double)(hi)); fails++; } } while (0)
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)

static Path path(double la1, double lo1, double la2, double lo2, int month, double utc, double r12, double f) {
    Path p; p.txLat = la1; p.txLon = lo1; p.rxLat = la2; p.rxLon = lo2; p.month = month; p.utcHour = utc; p.r12 = r12; p.fMHz = f;
    p.txPowerDbw = 20; p.txGainDbi = 2; p.rxGainDbi = 2; p.bandwidthHz = 2500; p.requiredSnrDb = 10; p.noiseEnv = 1;
    return p;
}

int main() {
    // CCIR maps: the port against the independent evaluation
    NEAR(foF2(1, 40, -105, 19, 100), 10.4287, 0.01);  NEAR(m3000F2(1, 40, -105, 19, 100), 3.1362, 0.005);   // Boulder, January noon
    NEAR(foF2(6, 40, -105, 19, 100), 6.4825, 0.01);   NEAR(m3000F2(6, 40, -105, 19, 100), 2.6768, 0.005);   // June noon (winter anomaly)
    NEAR(foF2(1, 51.5, -0.6, 12, 10), 5.8702, 0.01);  NEAR(m3000F2(1, 51.5, -0.6, 12, 10), 3.6399, 0.005);
    NEAR(foF2(6, 54.7, 25.3, 22, 100), 5.9560, 0.01); NEAR(m3000F2(6, 54.7, 25.3, 22, 100), 2.8182, 0.005);
    NEAR(foF2(3, 25.2, 55.3, 8, 150), 15.5275, 0.01);  // Dubai, March noon, high activity (the northern crest of the equatorial anomaly)
    NEAR(foF2(9, -12, -75.3, 17, 100), 10.5368, 0.01);
    // the 24-hour evaluation is the same numbers
    { double fo[24], mm[24]; foF2Day(1, 40, -105, 100, fo, mm); NEAR(fo[19], foF2(1, 40, -105, 19, 100), 1e-6); NEAR(mm[19], m3000F2(1, 40, -105, 19, 100), 1e-6); }
    // physics
    CHECK(foF2(1, 40, -105, 19, 100) > foF2(1, 40, -105, 9, 100) + 4);    // noon well above 02 local
    CHECK(foF2(1, 40, -105, 19, 100) > foF2(6, 40, -105, 19, 100) + 2);   // winter anomaly
    CHECK(foF2(1, 40, -105, 19, 100) > foF2(1, 40, -105, 19, 10) + 3);    // solar activity
    INRANGE(modipDeg(-12, -76), -5, 5);                                     // the dip equator passes near Huancayo
    { double I, F; magField(50, 10, 300, I, F); INRANGE(I, 62, 69); INRANGE(2.8 * F, 1.0, 1.5); }
    // foE
    INRANGE(foE(3, 45, 0, 12, 150), 3.3, 4.2);
    INRANGE(foE(3, 45, 0, 0, 150), 0.4, 0.9);
    INRANGE(foE(6, 45, 0, 12, 70), 2.9, 3.7);
    // MUF over 3000 km equals foF2 x M(3000) at the midpoint within a few per cent
    {
        const Path p = path(40, -30, 40, 5.2, 1, 13, 100, 14);   // about 3000 km east-west, midpoint near 41 N 12 W
        const Prediction q = predict(p);
        CHECK(q.valid);
        INRANGE(q.distKm, 2900, 3100);
        INRANGE(q.basicMufMHz / (q.foF2Mid * q.m3000Mid), 0.94, 1.06);
        CHECK(q.opMufMHz >= q.basicMufMHz);
        CHECK(q.owfMHz < q.opMufMHz && q.hpfMHz > q.opMufMHz);
        CHECK(!q.modes.empty());
    }
    // textbook band behaviour, Vilnius -> London (1700 km), 20 W, dipoles, rural noise, SSB 2.5 kHz
    {
        const Prediction day20 = predict(path(54.69, 25.28, 51.5, -0.12, 4, 11, 100, 14.1));
        CHECK(day20.snrDb > 10 && day20.reliability > 0.5);
        const Prediction nite10 = predict(path(54.69, 25.28, 51.5, -0.12, 12, 1, 5, 28.2));
        CHECK(nite10.reliability < 0.1);   // 10 m dead at night near solar minimum
        const Prediction nite80 = predict(path(54.69, 25.28, 51.5, -0.12, 12, 22, 20, 3.6));
        CHECK(nite80.snrDb > 10);
        const Prediction day80 = predict(path(54.69, 25.28, 51.5, -0.12, 6, 11, 100, 3.6));
        CHECK(day80.snrDb < nite80.snrDb - 10);   // D-layer absorption by day
        // absorption at 7.1 MHz over 1000 km at a summer noon: some 10..35 dB
        const Prediction q7 = predict(path(50, 0, 50, 14.0, 6, 11.5, 100, 7.1));
        CHECK(q7.bestMode >= 0);
        if (q7.bestMode >= 0) INRANGE(q7.modes[q7.bestMode].absorptionDb, 8, 35);
    }
    // skip zone: 14 MHz by day at 300 km is not carried by the F2 layer (above the MUF), at 2000 km it is
    {
        const Prediction near = predict(path(50, 10, 52.7, 10, 1, 12, 60, 14.1));
        const Prediction far = predict(path(50, 10, 50, 38, 1, 12, 60, 14.1));
        CHECK(near.basicMufMHz < 14.1);
        CHECK(far.basicMufMHz > 14.1);
    }
    // long paths: the composite method, finite and continuous with the ray-path method
    {
        const Prediction a = predict(path(51.5, -0.12, -33.9, 151.2, 11, 8, 100, 14.1));   // London - Sydney, ~17000 km
        CHECK(a.valid && a.method == 1);
        INRANGE(a.fieldDbuv, -40, 60);
        CHECK(a.lufLongMHz > 0);
        const Prediction s = predict(path(51.5, -0.12, 40.7, -74.0, 1, 15, 100, 14.1));    // London - New York, ~5570 km
        CHECK(s.valid && s.method == 0);
        INRANGE(s.fieldDbuv, -10, 60);
        const Prediction i7 = predict(path(0, 0, 0, 62.9, 3, 12, 100, 21.1));             // 6990 km and 7010 km (ray-path vs interpolated)
        const Prediction i8 = predict(path(0, 0, 0, 63.1, 3, 12, 100, 21.1));
        CHECK(i8.method == 2);
        CHECK(std::fabs(i7.fieldDbuv - i8.fieldDbuv) < 3);
    }
    // the hour x band chart
    {
        const Chart c = chart(path(54.69, 25.28, 40.7, -74.0, 1, 0, 100, 14), hamBandsMHz());
        CHECK(c.freqMHz.size() == 10 && c.rel.size() == 240);
        double best = 0; for (float r : c.rel) best = std::max(best, (double)r);
        CHECK(best > 0.5);   // something is open at some hour Vilnius - New York in January at R12 = 100
        CHECK(c.muf[15] > c.muf[3]);   // MUF higher in the (transatlantic) afternoon than before dawn
    }
    // P.371
    NEAR(sfiFromR12(r12FromSfi(150)), 150, 1e-6);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("prop_hf: all passed\n");
    return 0;
}
