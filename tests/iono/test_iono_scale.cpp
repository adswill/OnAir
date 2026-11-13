// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ionogram scaling on synthetic layered ionospheres (makeIonoLayerIonogram): a local ionosonde (E and F2, then E, F1 and F2) gives foE,
// foF1, foF2, fxF2, h'E, h'F, M(3000)F2 and MUF(3000)F2 within tolerance of the model; an oblique path gives its distance from the
// delay, the 1F / 2F modes and the path MUF; a known sounder (rate and schedule) and the receiver location give the distance from
// the sites; noise alone (with interfering carriers) gives nothing. Last, the real processor on the IQ test signal: its echo traces.
#include "dect2/iono/iono_dsp.h"
#include "dect2/iono/iono_gen.h"
#include "dect2/iono/iono_rx.h"
#include "dect2/iono/iono_scale.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static bool nearBy(double v, double want, double tol) { return std::isfinite(v) && std::fabs(v - want) <= tol; }

static void print(const char* what, const IonoScaling& s, const std::vector<IonoTrace>& tr) {
    printf("  %s: geometry %d, %zu traces, foE %.2f%s h'E %.0f, foF1 %.2f, foF2 %.2f%s fxF2 %.2f, h'F %.0f h'F2 %.0f hmF2 %.0f, M3000 %.3f MUF3000 %.2f, "
           "fmin %.2f | dist %.0f (%d) pathMUF %.2f%s, known %d (%d of %d)\n",
           what, s.geometry, tr.size(), s.foE, s.foEAbove ? "+" : "", s.hE, s.foF1, s.foF2, s.foF2Above ? "+" : "", s.fxF2, s.hF, s.hF2, s.hmF2, s.m3000,
           s.muf3000, s.fmin, s.distKm, s.distFrom, s.pathMuf, s.pathMufAbove ? "+" : "", s.known, s.knownMatch, s.candidates);
    for (const auto& t : tr)
        printf("    trace layer %d hops %d%s%s: %.2f - %.2f MHz, %.0f - %.0f km, %zu points\n", t.layer, t.hops, t.x ? " X" : "", t.highRay ? " high" : "",
               t.pts.front().fMhz, t.pts.back().fMhz, t.pts.front().rangeKm, t.pts.back().rangeKm, t.pts.size());
    for (const auto& m : s.modes) printf("    mode %d%s: MUF %.2f%s, min %.0f km\n", m.hops, m.layer == IonoTrace::E ? "E" : "F", m.mufMhz, m.mufAbove ? "+" : "", m.minRangeKm);
}

// the lowest virtual height of the model over [f0, f1)
static double minH(const IonoLayerModel& m, double f0, double f1) {
    double h = 1e300;
    for (double f = f0; f < f1; f += 0.01) { const double v = ionoVirtualHeightKm(m, f); if (std::isfinite(v)) h = std::min(h, v); }
    return h;
}

static bool empty(const IonoScaling& s) {
    return s.geometry == 0 && std::isnan(s.foF2) && std::isnan(s.foE) && std::isnan(s.pathMuf) && std::isnan(s.muf3000) && s.modes.empty();
}

static IonoSounder sounder(double rate, double period = 0, double offset = 0) {
    IonoSounder s; s.id = 1; s.rateHz = rate; s.periodSec = period; s.offsetSec = offset; return s;
}

int main() {
    iono::ScaleOptions opt;
    {   // vertical: E and F2, O and X
        IonoLayerModel m;
        m.fLoMhz = 1.0; m.fHiMhz = 16;
        std::vector<IonoTrace> tr;
        const IonoScaling s = iono::scaleIonogram(makeIonoLayerIonogram(m), sounder(100e3), opt, &tr);
        print("vertical E+F2", s, tr);
        const double fx = m.fH / 2 + std::sqrt(m.fH * m.fH / 4 + m.foF2 * m.foF2);
        const double hE = minH(m, m.fLoMhz, m.foE), hF = minH(m, m.foE + 0.01, m.foF2);
        const double M = 1490 / (m.hmF2 + 176);
        printf("    want foE %.2f h'E %.0f foF2 %.2f fxF2 %.2f h'F %.0f hmF2 %.0f M3000 %.3f MUF3000 %.2f\n", m.foE, hE, m.foF2, fx, hF, m.hmF2, M, m.foF2 * M);
        CHECK(s.geometry == 1, "geometry %d", s.geometry);
        CHECK(nearBy(s.foE, m.foE, 0.2) && !s.foEAbove, "foE %.2f", s.foE);
        CHECK(nearBy(s.foF2, m.foF2, 0.2) && !s.foF2Above, "foF2 %.2f", s.foF2);
        CHECK(nearBy(s.fxF2, fx, 0.25), "fxF2 %.2f want %.2f", s.fxF2, fx);
        CHECK(nearBy(s.hE, hE, 15), "h'E %.0f want %.0f", s.hE, hE);
        CHECK(nearBy(s.hF, hF, 20), "h'F %.0f want %.0f", s.hF, hF);
        CHECK(nearBy(s.m3000, M, 0.15), "M3000 %.3f want %.3f", s.m3000, M);
        CHECK(nearBy(s.muf3000, m.foF2 * M, 0.8), "MUF3000 %.2f", s.muf3000);
        CHECK(std::isnan(s.foF1), "foF1 %.2f without an F1 layer", s.foF1);
        CHECK(nearBy(s.fmin, m.fLoMhz, 0.3), "fmin %.2f", s.fmin);
        int multi = 0; for (const auto& t : tr) if (t.hops == 2) multi++;
        CHECK(multi >= 1, "no multiple reflection found");
        // MUF(3000) puts 20 m open, 10 m closed; 160 m below fmin (1.0) is open, nothing absorbed
        for (const auto& b : iono::hfBands()) {
            const int st = iono::bandState(b, s.muf3000, s.fmin);
            if (std::string(b.name) == "20 m") CHECK(st == 1, "20 m %d", st);
            if (std::string(b.name) == "10 m") CHECK(st == 0, "10 m %d", st);
        }
    }
    {   // vertical: E, F1 and F2
        IonoLayerModel m;
        m.foE = 3.2; m.foF1 = 4.6; m.hmF1 = 200; m.ymF1 = 50; m.foF2 = 8.5; m.hmF2 = 340; m.ymF2 = 110;
        m.fLoMhz = 1.5; m.fHiMhz = 18; m.seed = 7;
        std::vector<IonoTrace> tr;
        const IonoScaling s = iono::scaleIonogram(makeIonoLayerIonogram(m), sounder(100e3), opt, &tr);
        print("vertical E+F1+F2", s, tr);
        const double M = 1490 / (m.hmF2 + 176);
        CHECK(nearBy(s.foE, m.foE, 0.2), "foE %.2f", s.foE);
        CHECK(nearBy(s.foF1, m.foF1, 0.3), "foF1 %.2f", s.foF1);
        CHECK(nearBy(s.foF2, m.foF2, 0.2), "foF2 %.2f", s.foF2);
        CHECK(nearBy(s.m3000, M, 0.2), "M3000 %.3f want %.3f", s.m3000, M);
        const double hF = minH(m, m.foE + 0.01, m.foF2);
        CHECK(nearBy(s.hF, hF, 20), "h'F %.0f want %.0f", s.hF, hF);
    }
    {   // vertical, the sweep stops below foF2: a lower bound only, no M(3000)
        IonoLayerModel m;
        m.fLoMhz = 1.0; m.fHiMhz = 5.5; m.seed = 3;
        std::vector<IonoTrace> tr;
        const IonoScaling s = iono::scaleIonogram(makeIonoLayerIonogram(m), sounder(100e3), opt, &tr);
        print("vertical cut at 5.5 MHz", s, tr);
        CHECK(s.foF2Above && s.foF2 < 5.6 && std::isnan(s.muf3000), "foF2 %.2f above %d", s.foF2, s.foF2Above);
    }
    {   // oblique 1500 km, distance from the delay
        IonoLayerModel m;
        m.distKm = 1500; m.fLoMhz = 2; m.fHiMhz = 30; m.fH = 0; m.seed = 11;
        std::vector<IonoTrace> tr;
        const IonoScaling s = iono::scaleIonogram(makeIonoLayerIonogram(m), sounder(100e3), opt, &tr);
        print("oblique 1500 km", s, tr);
        const double muf1F = ionoPathMufMhz(m, 3, 1), muf2F = ionoPathMufMhz(m, 3, 2), muf1E = ionoPathMufMhz(m, 1, 1);
        printf("    want MUF 1E %.2f 1F %.2f 2F %.2f\n", muf1E, muf1F, muf2F);
        CHECK(s.geometry == 2 && s.distFrom == 1 && nearBy(s.distKm, m.distKm, 0.08 * m.distKm), "distance %.0f", s.distKm);
        CHECK(nearBy(s.pathMuf, muf1F, 0.4) && !s.pathMufAbove, "path MUF %.2f want %.2f", s.pathMuf, muf1F);
        bool f1 = false, f2 = false;
        for (const auto& md : s.modes) {
            if (md.layer == IonoTrace::F2 && md.hops == 1) { f1 = true; CHECK(nearBy(md.mufMhz, muf1F, 0.4), "1F MUF %.2f", md.mufMhz); }
            if (md.layer == IonoTrace::F2 && md.hops == 2) { f2 = true; CHECK(nearBy(md.mufMhz, muf2F, 0.4), "2F MUF %.2f", md.mufMhz); }
        }
        CHECK(f1 && f2, "modes 1F %d 2F %d", f1, f2);
    }
    {   // oblique 800 km, F only
        IonoLayerModel m;
        m.distKm = 800; m.foE = 0; m.foF2 = 6; m.fLoMhz = 2; m.fHiMhz = 30; m.fH = 0; m.hops = 1; m.seed = 5;
        std::vector<IonoTrace> tr;
        const IonoScaling s = iono::scaleIonogram(makeIonoLayerIonogram(m), sounder(125e3), opt, &tr);
        print("oblique 800 km", s, tr);
        const double muf = ionoPathMufMhz(m, 3, 1);
        CHECK(s.geometry == 2 && nearBy(s.distKm, m.distKm, 0.1 * m.distKm), "distance %.0f", s.distKm);
        CHECK(nearBy(s.pathMuf, muf, 0.4), "path MUF %.2f want %.2f", s.pathMuf, muf);
    }
    {   // a known sounder (rate and schedule) and the receiver location: the distance from the sites
        const std::vector<iono::KnownSounder> list = {
            {"Test A", 100e3, 300, 12, 52.0, 13.0, true, ""}, {"Test B", 100e3, 300, 40, 60.0, 10.0, true, ""}, {"Test C", 500e3, 0, 0, 55.0, 25.0, false, ""}};
        iono::ScaleOptions o;
        o.obs.have = true; o.obs.lat = 54.7; o.obs.lon = 25.3;
        const double d = iono::greatCircleKm(o.obs.lat, o.obs.lon, 52.0, 13.0);
        IonoLayerModel m;
        m.distKm = d; m.fLoMhz = 2; m.fHiMhz = 30; m.fH = 0; m.seed = 9;
        std::vector<IonoTrace> tr;
        const IonoScaling s = iono::scaleIonogram(makeIonoLayerIonogram(m), sounder(100e3, 300, 12), o, &tr, list);
        print("known sounder", s, tr);
        CHECK(s.known == 0 && s.knownMatch == 2 && s.candidates == 2, "known %d match %d of %d", s.known, s.knownMatch, s.candidates);
        CHECK(s.geometry == 2 && s.distFrom == 2 && nearBy(s.distKm, d, 1), "distance %.0f want %.0f (%d)", s.distKm, d, s.distFrom);
        CHECK(nearBy(s.pathMuf, ionoPathMufMhz(m, 3, 1), 0.4), "path MUF %.2f", s.pathMuf);
        // the rate alone: two 100 kHz/s entries, no distance to choose by -> not identified
        int match = 0, cands = 0;
        CHECK(iono::identify(sounder(100e3), iono::Observer{}, NAN, match, cands, list) < 0 && cands == 2, "rate only");
        // a nearby 500 kHz/s sounder: vertical
        iono::ScaleOptions o2 = o;
        IonoLayerModel v; v.fLoMhz = 1; v.fHiMhz = 16;
        const IonoScaling s2 = iono::scaleIonogram(makeIonoLayerIonogram(v), sounder(500e3), o2, nullptr, list);
        CHECK(s2.known == 2 && s2.knownMatch == 1 && s2.geometry == 1, "nearby known %d match %d geometry %d", s2.known, s2.knownMatch, s2.geometry);
    }
    {   // geometry checks
        CHECK(nearBy(iono::obliqueDistanceKm(iono::obliqueGroupPathKm(2000, 300, 2), 300, 2), 2000, 0.01), "geometry inverse");
        CHECK(std::isnan(iono::obliqueDistanceKm(100, 300, 1)), "path shorter than the height");
    }
    for (int k = 0; k < 6; k++) {   // noise only, with interfering carriers: nothing
        IonoLayerModel m;
        m.noiseOnly = true; m.fLoMhz = 1; m.fHiMhz = 30; m.seed = 100 + k; m.blocks = k % 3 + 1;
        if (k >= 3) m.carriersMhz = {5.0, 6.1, 9.5, 9.55, 9.6, 15.2};
        std::vector<IonoTrace> tr;
        const IonoScaling s = iono::scaleIonogram(makeIonoLayerIonogram(m), sounder(100e3), opt, &tr);
        if (!empty(s) || !tr.empty()) print("noise", s, tr);
        CHECK(empty(s) && tr.empty(), "noise %d: %zu traces, geometry %d", k, tr.size(), s.geometry);
    }
    {   // interfering carriers over a real ionogram do not change the values
        IonoLayerModel m;
        m.fLoMhz = 1.0; m.fHiMhz = 16; m.carriersMhz = {4.0, 5.0, 6.0, 9.4}; m.seed = 21;
        const IonoScaling s = iono::scaleIonogram(makeIonoLayerIonogram(m), sounder(100e3), opt);
        CHECK(nearBy(s.foF2, m.foF2, 0.2) && nearBy(s.foE, m.foE, 0.2), "with carriers foE %.2f foF2 %.2f", s.foE, s.foF2);
    }
    {   // the processor on the IQ test signal (constant echo delays, 1 Msps at 0.5 MHz): traces at the two echo ranges, scaled per sweep
        const double fs = 1e6, fc = 0.5e6;
        SynthConfig sc;
        sc.modeVal[0] = fc / 1e6;
        auto syn = makeIonoSynth(sc, fs);
        iono::Processor p;
        p.configure(fs, fc, 0);
        std::vector<cf32> b(65536);
        for (double t = 0; t < 36; t += b.size() / fs) { syn->generate(b.data(), b.size()); p.feed(b.data(), b.size()); }
        IonoTelemetry t;
        p.fill(t);
        int idx = -1;
        for (size_t i = 0; i < t.sounders.size(); i++) if (t.sounders[i].rateHz == kIonoSynth[0].rateHz) idx = (int)i;
        CHECK(idx >= 0, "no 100 kHz/s sounder");
        if (idx >= 0) {
            p.setSelected(idx);
            p.fill(t);
            print("processor", t.sounders[(size_t)idx].scale, t.traces);
            bool a = false, c = false;
            for (const auto& tr : t.traces) {
                const double r = tr.pts.front().rangeKm;
                if (std::fabs(r - iono::groupRangeKm(kIonoSynth[0].delaySec[0])) < 20) a = true;
                if (std::fabs(r - iono::groupRangeKm(kIonoSynth[0].delaySec[1])) < 20) c = true;
            }
            CHECK(a && c, "processor traces at %d / %d", a, c);
            CHECK(t.sounders[(size_t)idx].scale.geometry != 0 && !t.history.empty(), "processor scaling %d, history %zu", t.sounders[(size_t)idx].scale.geometry,
                  t.history.size());
            // the test signal stops at the edge of the narrow span: any end frequency is a lower bound
            const IonoScaling& s = t.sounders[(size_t)idx].scale;
            CHECK(std::isnan(s.foF2) || s.foF2Above, "foF2 %.2f from a span edge", s.foF2);
            CHECK(std::isnan(s.pathMuf) || s.pathMufAbove, "MUF %.2f from a span edge", s.pathMuf);
            const std::string csv = iono::historyCsv(t.history, t.sounders[(size_t)idx], nullptr);
            CHECK(std::count(csv.begin(), csv.end(), '\n') == 1 + (long)t.history.size(), "history csv");
        }
    }
    printf(fails ? "iono scale: %d FAILED\n" : "iono scale: all passed\n", fails);
    return fails ? 1 : 0;
}
