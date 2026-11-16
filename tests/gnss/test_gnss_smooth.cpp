// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Carrier smoothing of the pseudoranges (GnssReceiver::setCarrierSmoothing, gnss_rx_measure.cpp). The same sky is run with the smoothing off and on:
// the position must be steadier with it, a second system joining filters that are already running must not stand off from them, and GLONASS, whose
// satellites each have a carrier of their own, must be left alone.
#include "dect2/gnss/gnss_testkit.h"
#include "dect2/engine/test_parallel.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::gnsstest;
using dect2::testpar::CaseOut;
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

namespace {
struct Result {
    bool valid = false;
    double hzEnd = 0, vtEnd = 0;      // the last fix against the truth
    double hzRms = 0, vtRms = 0;      // over the reports of the last 30 s
    int reports = 0;
    int smoothedGps = 0, smoothedGlo = 0, gps = 0, glo = 0;   // the last measurement epoch: satellites in the fix, and the longest filter among them
};

Result runSky(unsigned systems, double centerMhz, double rate, double secs, int smoothing) {
    GnssSimConfig cfg;
    cfg.systems = systems; cfg.centerHz = centerMhz * 1e6;
    GnssSim sim(cfg, rate);
    Options o;
    o.rate = rate; o.secs = secs; o.centerMhz = centerMhz;
    o.setup = [smoothing](GnssReceiver& rx) { rx.setCarrierSmoothing(smoothing); };
    Run r = run(sim, o);
    Result q;
    q.valid = r.tel.fix.valid;
    if (q.valid) fixError(sim, r.tel.fix, &q.hzEnd, &q.vtEnd);
    double sh = 0, sv = 0;
    for (const GnssTelemetry& t : r.reports) {
        if (!t.fix.valid || t.signalSecs < secs - 30.0) continue;
        double hz = 0, vt = 0;
        fixError(sim, t.fix, &hz, &vt);
        sh += hz * hz; sv += vt * vt; q.reports++;
    }
    if (q.reports) { q.hzRms = std::sqrt(sh / q.reports); q.vtRms = std::sqrt(sv / q.reports); }
    if (!r.meas.empty())
        for (const GnssMeasurement& m : r.meas.back()) {
            if (!m.inFix) continue;
            if (m.sys == GnssGlonass) { q.glo++; q.smoothedGlo = std::max(q.smoothedGlo, m.smoothed); }
            else if (m.sys == GnssGps) { q.gps++; q.smoothedGps = std::max(q.smoothedGps, m.smoothed); }
        }
    return q;
}

void runCase(int k, CaseOut& out) {
    if (k == 0) {
        // GPS alone, 75 s: the fixes of the last 30 s against the truth
        const Result off = runSky(gnssSystemBit(GnssGps), 1575.42, 4e6, 75, 0), on = runSky(gnssSystemBit(GnssGps), 1575.42, 4e6, 75, 100);
        out.print("GPS: smoothing off %.2f / %.2f m rms (horizontal / vertical, %d reports), on %.2f / %.2f m; filters %d epochs long\n", off.hzRms, off.vtRms,
                  off.reports, on.hzRms, on.vtRms, on.smoothedGps);
        CHECK(off.valid && on.valid && on.reports >= 8, "no fix (%d reports)", on.reports);
        CHECK(off.smoothedGps <= 1, "smoothing off, but a filter is %d epochs long", off.smoothedGps);
        CHECK(on.smoothedGps >= 30, "the longest filter is %d epochs after 75 s", on.smoothedGps);
        const double a = std::hypot(off.hzRms, off.vtRms), b = std::hypot(on.hzRms, on.vtRms);
        CHECK(b < 0.6 * a, "position error %.2f m with the smoothing, %.2f m without", b, a);
        CHECK(on.hzRms < 2.5 && on.vtRms < 4.0, "position error %.2f / %.2f m with the smoothing", on.hzRms, on.vtRms);
    } else if (k == 1) {
        // Galileo joins GPS filters that have run for half a minute, and its ranges move to the GPS clock when word type 10 arrives: neither may
        // leave the Galileo satellites standing off (7 - 15 m in height before the filters were re-anchored and restarted on a change of clock)
        const unsigned sys = gnssSystemBit(GnssGps) | gnssSystemBit(GnssGalileo);
        const Result off = runSky(sys, 1575.42, 4e6, 80, 0), on = runSky(sys, 1575.42, 4e6, 80, 100);
        out.print("GPS + Galileo: smoothing off %.2f / %.2f m at the end, on %.2f / %.2f m\n", off.hzEnd, off.vtEnd, on.hzEnd, on.vtEnd);
        CHECK(off.valid && on.valid, "no fix");
        CHECK(on.hzEnd < 3.0 && std::fabs(on.vtEnd) < 4.0, "position error %.2f / %.2f m with the smoothing", on.hzEnd, on.vtEnd);
    } else {
        // GLONASS with GPS: the GPS satellites are smoothed, the GLONASS ones are not
        const unsigned sys = gnssSystemBit(GnssGps) | gnssSystemBit(GnssGlonass);
        const Result on = runSky(sys, 1590.4, 40e6, 60, 100);
        out.print("GPS + GLONASS: %.2f / %.2f m at the end, %d GPS (filters up to %d epochs) + %d GLONASS (%d) in the fix\n", on.hzEnd, on.vtEnd, on.gps,
                  on.smoothedGps, on.glo, on.smoothedGlo);
        CHECK(on.valid, "no fix");
        CHECK(on.gps >= 4 && on.smoothedGps >= 10, "%d GPS satellites, filters up to %d epochs", on.gps, on.smoothedGps);
        CHECK(on.glo >= 4 && on.smoothedGlo <= 1, "%d GLONASS satellites, filters up to %d epochs (GLONASS is not smoothed)", on.glo, on.smoothedGlo);
        CHECK(on.hzEnd < 15.0 && std::fabs(on.vtEnd) < 25.0, "position error %.2f / %.2f m", on.hzEnd, on.vtEnd);
    }
}
} // namespace

int main() {
    const int fails = dect2::testpar::runCases(3, [](size_t i, CaseOut& out) { runCase((int)i, out); });
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
