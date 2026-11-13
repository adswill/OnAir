// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Navaid signal check (navaid_check.h): the real VOR / ILS generator through the real receiver (carrier offset, noise sweep), and figures
// pushed out of the Annex 10 limits. The generator always sends the nominal depths, so out-of-limit depths are tested on telemetry.
#include "dect2/vor/navaid_check.h"
#include "dect2/vor/vor_gen.h"
#include "dect2/vor/vor_rx.h"
#include <cmath>
#include <cstdlib>
#include <vector>
#include "../common/test_util.h"
using namespace dect2;

constexpr double kRate = 250000, kOff = -40000;

static NavaidCheck run(VorGenConfig g, int function, double secs = 14) {
    g.rate = kRate; g.dialOffsetHz = kOff;
    VorGenerator gen(g);
    VorReceiver rx; rx.setSilent(true); rx.setFunction(function); rx.setSignalOffset(kOff); rx.configure(kRate);
    NavaidChecker ck;
    std::vector<cf32> x((size_t)(0.01 * kRate));
    uint64_t seq = 0;
    for (double t = 0; t < secs; t += 0.01) {
        gen.generate(x.data(), x.size());
        rx.feed(x.data(), x.size());
        VorTelemetry tel; if (rx.telemetry(tel, seq)) { seq = tel.seq; ck.add(tel); }
    }
    return ck.result();
}

static const NavaidCheckItem* find(const NavaidCheck& r, const char* name) { for (const auto& i : r.items) if (i.name == name) return &i; return nullptr; }

int main() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;   // the full sweep; the default run keeps two points
    const std::vector<double> snrs = longRun ? std::vector<double>{40, 25, 15} : std::vector<double>{15}, cfos = longRun ? std::vector<double>{0, 3000} : std::vector<double>{3000};
    for (double snr : snrs) for (double cfo : cfos) {   // nominal beacons: everything within
        VorGenConfig g; g.kind = 0; g.radialDeg = 123; g.snrDb = snr; g.cfoHz = cfo;
        NavaidCheck r = run(g, 1);
        CHECK(r.function == NavFunction::Vor && r.items.size() >= 5, "VOR snr %.0f cfo %.0f: %zu items", snr, cfo, r.items.size());
        CHECK(r.worst <= 1, "VOR snr %.0f cfo %.0f: %s", snr, cfo, r.summary.c_str());   // the 10 % ident sits on its limit: at most marginal
        for (const char* n : {"30 Hz AM depth", "9960 Hz subcarrier depth", "FM deviation", "subcarrier frequency"}) {
            const NavaidCheckItem* it = find(r, n);
            CHECK(it && (it->verdict == 0 || (snr < 20 && it->verdict == 1)), "VOR snr %.0f cfo %.0f: %s %.2f", snr, cfo, n, it ? it->value : -1);
        }
        for (const auto& i : r.items) if (i.verdict) printf("  snr %.0f %s %.2f %s [%.1f %.1f]\n", snr, i.name.c_str(), i.value, i.unit.c_str(), i.lo, i.hi);
        VorGenConfig l; l.kind = 2; l.ddm = 0; l.snrDb = snr; l.cfoHz = cfo;
        NavaidCheck rl = run(l, 2);
        const NavaidCheckItem* m90 = find(rl, "90 Hz depth");
        CHECK(m90 && std::fabs(m90->value - 20) < 1.5 && rl.worst == 0, "LOC snr %.0f: %s", snr, rl.summary.c_str());
        l.ddm = 0.155;   // full scale, off the course line: SDM checked instead
        rl = run(l, 2);
        const NavaidCheckItem* sdm = find(rl, "SDM");
        CHECK(sdm && sdm->verdict == 0 && std::fabs(sdm->value - 40) < 2, "LOC off course SDM %.1f", sdm ? sdm->value : -1);
    }
    {   // a subcarrier 1.9 % high: beyond the 1 % limit, within the 2 % measurement margin = marginal; 3.5 % high: out
        VorGenConfig g; g.kind = 0; g.radialDeg = 40; g.snrDb = 35; g.sub9960Hz = 10150;
        NavaidCheck r = run(g, 1);
        const NavaidCheckItem* sc = find(r, "subcarrier frequency");
        CHECK(sc && sc->verdict == 1 && std::fabs(sc->value - 10150) < 20, "subcarrier %.0f verdict %d", sc ? sc->value : 0, sc ? sc->verdict : -1);
        g.sub9960Hz = 10310; r = run(g, 1); sc = find(r, "subcarrier frequency");
        CHECK(sc && sc->verdict == 2, "subcarrier %.0f verdict %d", sc ? sc->value : 0, sc ? sc->verdict : -1);
    }
    {   // depths on telemetry: 30 Hz AM at 24 % is out, 31 % within, 33 % marginal; glide path tones at 36 % out; a marker at 95 % within
        auto feed = [](NavFunction fn, auto set) { NavaidChecker c; for (int i = 0; i < 80; i++) { VorTelemetry t; t.dataValid = true; t.function = fn; t.timeSec = i * 0.25;
            t.am30 = 0.3f; t.sub9960 = 0.3f; t.fmDevHz = 480; t.subcarrierHz = 9960; t.tone30Hz = 30; set(t, i); c.add(t); } return c.result(); };
        CHECK(find(feed(NavFunction::Vor, [](VorTelemetry& t, int) { t.am30 = 0.24f; }), "30 Hz AM depth")->verdict == 2, "24 %% out");
        CHECK(find(feed(NavFunction::Vor, [](VorTelemetry& t, int) { t.am30 = 0.31f; }), "30 Hz AM depth")->verdict == 0, "31 %% within");
        CHECK(find(feed(NavFunction::Vor, [](VorTelemetry& t, int) { t.am30 = 0.33f; }), "30 Hz AM depth")->verdict == 1, "33 %% marginal");
        CHECK(find(feed(NavFunction::Vor, [](VorTelemetry& t, int i) { t.identKey = i % 3 == 0; t.identDepth = 0.18f; }), "identification depth")->verdict == 2, "ident 18 %%");
        auto gp = feed(NavFunction::Glideslope, [](VorTelemetry& t, int) { t.m90 = 0.36; t.m150 = 0.36; t.ddm = 0; });
        CHECK(find(gp, "90 Hz depth") && find(gp, "90 Hz depth")->verdict == 1 && gp.worst >= 1, "GP 36 %%: %s", gp.summary.c_str());
        auto gp2 = feed(NavFunction::Glideslope, [](VorTelemetry& t, int) { t.m90 = 0.33; t.m150 = 0.33; t.ddm = 0; });
        CHECK(gp2.worst == 2, "GP 33 %% out");
        auto mk = feed(NavFunction::Marker, [](VorTelemetry& t, int i) { t.markerOn = i % 2; t.markerDepth = 0.95f; });
        CHECK(mk.worst == 0 && mk.items.size() == 1, "marker");
        NavaidChecker c; CHECK(c.result().summary == "measuring", "empty");
        // a change of function starts over
        VorTelemetry t; t.dataValid = true; t.function = NavFunction::Vor;
        for (int i = 0; i < 20; i++) { t.timeSec = i; c.add(t); }
        t.function = NavFunction::Localizer; t.timeSec = 21; c.add(t);
        CHECK(c.result().samples == 1, "restart on a new function");
    }
    return testSummary("vor_navcheck");
}
