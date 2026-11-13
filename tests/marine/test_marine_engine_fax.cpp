// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather fax through the engine exactly as the app runs it: the "Weather fax" entry is picked (applySubModeSynth for the test signal,
// applySubMode for the decoder, the service and the frequency the marine tab pushes), the synthetic source plays, and the picture that
// comes out of MarineReceiver::latestImage() must be the test chart. Also with a SynthConfig left over from other modes (GNSS, mesh,
// ADS-B and the spectrum analyser write into the same modeOpt / modeVal slots).
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/marine/marine_gen.h"
#include "../data/marine/fax/fax_util.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
using namespace dect2::enginetest;

#include "../common/test_util.h"

static void session(const char* name, bool dirty, double secs) {
    const SubMode* sm = subModeById("wefax");
    CHECK(sm && sm->stdMode == 17 && sm->sel == 3, "%s: the wefax entry", name);
    if (!sm) return;
    Engine e;
    DeviceInfo dev;                                    // the synthetic source
    TuneSettings t;
    const ModeTuning* mt = modeTuning(sm->stdMode);
    t.centerHz = sm->defMhz * 1e6 + mt->tuneOffsetHz;   // App: the radio sits tuneOffsetHz above the dial
    t.bandwidthMhz = mt->bandwidthMhz;
    t.sampleRate = mt->sampleRate;
    t.basebandFilterHz = mt->basebandHz;
    t.synth.mode = 17;
    t.synth.pace = 20.0;
    t.synth.snrDb = 30;
    if (dirty) {      // what other modes leave in the shared test-signal settings
        t.synth.modeOpt[0] = 1;        // NAVTEX picked earlier in the family
        t.synth.modeOpt[1] = 2;        // mesh region
        t.synth.modeOpt[2] = 4;
        t.synth.modeOpt[3] = 7;
        t.synth.modeOpt[4] = 1;
        t.synth.modeOpt[5] = 1575420;  // GNSS: the tune in kHz
        t.synth.modeVal[0] = 87.5;     // spectrum analyser: low edge in MHz
        t.synth.modeVal[1] = 108;
        t.synth.modeVal[2] = 54.7;     // ADS-B reference latitude
        t.synth.modeVal[3] = 25.3;
    }
    applySubModeSynth(t.synth, *sm);
    e.setStandard(sm->stdMode);
    applySubMode(e, sm);
    e.marine().setFrequencyHz(0);       // the marine tab: a test signal has no dial frequency
    FileOptions fo;
    CHECK(e.start(dev, t, fo), "%s: engine start", name);
    const double s0 = signalSecs(e);
    RxTelemetry rx;
    uint64_t seq = 0;
    int maxLines = 0, sawPhasing = 0, published = 0;
    uint64_t imgSeq = 0;
    FaxImage img;
    while (signalSecs(e) - s0 < secs) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            maxLines = std::max(maxLines, rx.marine.fax.lines);
            if (rx.marine.fax.state == 1) sawPhasing++;
            FaxImage im;
            if (e.marine().latestImage(im, imgSeq) && im.height > 0) { published++; img = std::move(im); }   // the viewer copies the picture as it builds
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    waitFor(5.0, [&] { return e.latestRx(rx, seq); });
    e.latestRx(rx, 0);
    { FaxImage im; uint64_t s = 0; if (e.marine().latestImage(im, s)) img = std::move(im); }
    const MarineFaxInfo& f = rx.marine.fax;
    const int w = faxImageWidth(576);
    // the chart the source sends (800 lines); the first decoded row is the white line after the phasing
    const FaxImage ref = faxTestChart(w, 800, 5);
    FaxImage refCut = ref; refCut.height = std::max(0, std::min(800, img.height - 2)); refCut.pix.resize((size_t)refCut.height * w);
    const faxt::Quality q = faxt::compare(img, refCut, 1);
    printf("%-26s state %d, lpm %d, ioc %d, lines %d, phasing lines %d, picture %dx%d (%d copies while it built), row correlation %.3f, drift %.2f px, service %d\n",
           name, f.state, f.lpm, f.ioc, f.lines, f.phasingLines, img.width, img.height, published, q.meanCorr, q.drift, rx.marine.serviceActive);
    CHECK(sawPhasing > 0 && f.phasingLines >= 12, "%s: phasing not seen (%d reports, %d lines)", name, sawPhasing, f.phasingLines);
    CHECK(f.state == 2 && f.lpm == 120 && f.ioc == 576, "%s: state %d lpm %d ioc %d", name, f.state, f.lpm, f.ioc);
    CHECK(f.lines >= 40 && img.width == w && img.height >= f.lines, "%s: %d lines, picture %dx%d", name, f.lines, img.width, img.height);
    CHECK(published >= 3, "%s: the picture was published %d times while it built", name, published);
    CHECK(q.ok && q.meanCorr > 0.9 && std::fabs(q.drift) < 1.5, "%s: correlation %.3f drift %.2f", name, q.meanCorr, q.drift);
    CHECK(rx.marine.serviceActive == 3, "%s: service %d", name, rx.marine.serviceActive);
    CHECK(e.droppedSamples() == 0, "%s: dropped %llu samples", name, (unsigned long long)e.droppedSamples());
    {   // the "clear picture" button: the picture is gone at once, and the change counter moves so the viewer drops its copy
        FaxImage keep; uint64_t s0 = 0;
        CHECK(e.marine().latestImage(keep, s0) || s0 > 0, "%s: a picture before the clear", name);
        e.marine().clearFax();
        FaxImage after; uint64_t s1 = s0;
        CHECK(e.marine().latestImage(after, s1) && s1 != s0, "%s: clearFax did not move the picture's change counter", name);
        CHECK(after.height == 0 && after.pix.empty(), "%s: picture still %dx%d after clearFax", name, after.width, after.height);
        e.marine().clearResults();      // the top bar's Clear all goes through this
        FaxImage after2; uint64_t s2 = s1;
        e.marine().latestImage(after2, s2);
        CHECK(after2.height == 0, "%s: picture %dx%d after clearResults", name, after2.width, after2.height);
    }
    e.stop();
}

int main() {
    // 5 s start tone, 10 s phasing (the test signal; stations send 30 s), one white line, then the chart at 2 lines a second
    session("wefax entry", false, 45);
    session("wefax, stale test settings", true, 45);
    if (fails || faxt::fails) { printf("marine engine fax: %d FAILED\n", fails + faxt::fails); return 1; }
    printf("marine engine fax: all passed\n");
    return 0;
}
