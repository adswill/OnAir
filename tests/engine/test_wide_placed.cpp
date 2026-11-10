// Several modes at once (issue #384, core/include/dect2/engine/wide_placed.h): on ONE 2.4 Msps capture a POCSAG pager carrier and an air-band AM station are
// placed by hand and decoded together, a DMR carrier in the same capture that was not placed is left off, and the placed list follows a retune. Simulated signals only.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/engine/modes.h"
#include "dect2/engine/wide_placed.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/t2/t2rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace dect2;
#include "../common/test_util.h"

static const double kFs = 2.4e6, kCentre = 125.0e6;

static void testPure() {
    std::vector<Placed> v;
    Placed a; a.absHz = 125e6; a.widthHz = 12500; a.kind = WsKind::Pager; v.push_back(a);
    Placed b; b.absHz = 126.5e6; b.widthHz = 25000; b.kind = WsKind::Am; v.push_back(b);
    const std::vector<Placed> r = placedFromText(placedToText(v));
    CHECK(r.size() == 2 && r[0].absHz == 125e6 && r[0].kind == WsKind::Pager && r[1].widthHz == 25000, "text round trip");
    CHECK(placedFromText("nonsense\n1 2\n-5 12500 3\n125e6 12500 999\n").empty(), "bad lines are skipped");
    CHECK(placedInCapture(a, 125e6, 2.4e6), "centre is inside");
    CHECK(!placedInCapture(b, 125e6, 2.4e6), "1.5 MHz off is outside 2.4 Msps");
    CHECK(placedInCapture(b, 125e6, 8e6), "inside a wider capture");
    Placed edge; edge.absHz = 125.9e6; edge.widthHz = 12500;
    CHECK(placedInCapture(edge, 125e6, 2.4e6), "inside the usable 80 percent");
    edge.absHz = 126.1e6;
    CHECK(!placedInCapture(edge, 125e6, 2.4e6), "the rolled-off edge does not count");
    CHECK(!placedInCapture(a, 125e6, 0), "no rate: nothing is inside");
}

static void testMidClick() {
    CHECK(placedIsTap(0, 0) && placedIsTap(3, -3) && !placedIsTap(4, 0) && !placedIsTap(0, 9), "a middle press that moved is a drag, not a click");
    std::vector<Placed> v;
    v.push_back(placedAtClick(125000040, 12500, WsKind::Pager));
    v.push_back(placedAtClick(126500000, 1, WsKind::Unknown));
    CHECK(v[0].absHz == 125000000 && v[1].widthHz == 1000, "click is rounded to 100 Hz, width at least 1 kHz");
    CHECK(placedNearest(v, 125003000, 500) == 0, "inside its own width");
    CHECK(placedNearest(v, 126501000, 100) == -1 && placedNearest(v, 126501000, 1100) == 1, "pixel tolerance");
    CHECK(placedNearest(v, 130e6, 1000) == -1 && placedNearest({}, 1e6, 1000) == -1, "nothing near");
    v.push_back(placedAtClick(125002000, 1000, WsKind::Am));
    CHECK(placedNearest(v, 125001900, 100) == 2, "closest one wins");
}

int main() {
    testPure();
    testMidClick();
    const double offDmr = -603000, offPag = -996000, offAm = +212300;
    struct Src { std::unique_ptr<ModeSynth> s; float gain; double shiftHz; std::complex<double> ph{1, 0}; };
    std::vector<Src> src;
    SynthConfig cd; cd.snrDb = 28;
    src.push_back({makeDmrSynth(cd, kFs), 0.6f, offDmr});
    SynthConfig cg; cg.snrDb = 30; cg.modeOpt[2] = kPocsag1200 + 1; cg.modeOpt[3] = 21;
    src.push_back({makePagerSynth(cg, kFs), 0.6f, offPag + 50000});   // the pager synth makes its signal 50 kHz below 0

    WideSession ws;
    WsConfig cfg; cfg.spanRate = kFs; cfg.centerHz = kCentre; cfg.cpuBudget = 0; cfg.tv = false; cfg.identSec = 0.8;
    ws.configure(cfg);
    ws.useFrequencyDatabase(false);
    ws.setOnlyManual(true);
    std::vector<float> snd;
    ws.setAudioSink([&](const float* s, size_t n) { snd.insert(snd.end(), s, s + 2 * n); });

    std::vector<Placed> placed;
    Placed pg; pg.absHz = kCentre + offPag; pg.widthHz = 12500; pg.kind = WsKind::Pager; placed.push_back(pg);
    Placed am; am.absHz = kCentre + offAm; am.widthHz = 8000; am.kind = WsKind::Am; placed.push_back(am);
    Placed outside; outside.absHz = kCentre + 5e6; outside.widthHz = 12500; outside.kind = WsKind::Nfm; placed.push_back(outside);   // not in this capture ("far" is a macro on Windows)
    PlacedSync sync;
    sync.apply(ws, placed, kCentre, kFs);
    CHECK(sync.ids().size() == 2, "two of the three placed signals are in the capture (%zu)", sync.ids().size());

    const size_t blk = (size_t)(kFs * 0.05);
    std::vector<cf32> sum(blk), tmp(blk);
    uint32_t rng = 321;
    double amPh = 0, t = 0;
    auto run = [&](double secs) {
        const double end = t + secs;
        for (; t < end; t += 0.05) {
            std::fill(sum.begin(), sum.end(), cf32(0, 0));
            for (Src& s : src) {
                s.s->generate(tmp.data(), blk);
                const std::complex<double> st = std::polar(1.0, 2 * M_PI * s.shiftHz / kFs);
                std::complex<double> ph = s.ph;
                for (size_t i = 0; i < blk; i++) { sum[i] += tmp[i] * cf32((float)ph.real(), (float)ph.imag()) * s.gain; ph *= st; }
                s.ph = ph / std::abs(ph);
            }
            for (size_t i = 0; i < blk; i++) {
                const double m = 1.0 + 0.5 * std::sin(amPh);
                const double ph = 2 * M_PI * offAm * (t * kFs + (double)i) / kFs;
                sum[i] += cf32((float)(0.12 * m * std::cos(ph)), (float)(0.12 * m * std::sin(ph)));
                amPh += 2 * M_PI * 700.0 / kFs;
                rng = rng * 1664525u + 1013904223u;
                sum[i] += cf32(((rng >> 8) & 0xffff) / 65536.f - .5f, ((rng >> 12) & 0xffff) / 65536.f - .5f) * 0.0004f;
            }
            ws.feed(sum.data(), blk);
        }
    };
    run(12.0);
    ws.wait();
    run(1.0);
    bool sawDmrOff = false;
    int running = 0;
    for (const WsEntry& e : ws.entries()) {
        printf("  id %d %-8s %-12s %+9.1f kHz items %llu\n", e.id, wsKindName(e.kind), wsStateName(e.state), e.offsetHz / 1e3, (unsigned long long)e.items);
        if (e.id >= kFirstManualId) { CHECK(e.state == WsState::Running, "placed signal %d is %s", e.id, wsStateName(e.state)); running++; }
        else if (std::fabs(e.offsetHz - offDmr) < 5000) { sawDmrOff = true; CHECK(e.state == WsState::Off, "the DMR carrier that was not placed is %s", wsStateName(e.state)); }
    }
    CHECK(running == 2, "%d placed signals running", running);
    CHECK(sawDmrOff, "the DMR carrier is listed");
    {
        RxTelemetry rg; uint64_t seq = 0;
        ws.modes().telemetry(24, rg, seq);
        CHECK(rg.pager.messagesTotal >= 1, "pager decoder: %llu pages", (unsigned long long)rg.pager.messagesTotal);
        printf("  pager pages: %llu\n", (unsigned long long)rg.pager.messagesTotal);
    }
    {   // the AM station's 700 Hz tone is in the sound
        std::vector<float> mono;
        for (size_t i = snd.size() / 2; i + 1 < snd.size(); i += 2) mono.push_back(snd[i] + snd[i + 1]);
        double on = 0, off = 0;
        CHECK(mono.size() > 48000, "AM sound arrived (%zu samples)", mono.size());
        const auto level = [&](double f) { double re = 0, im = 0; for (size_t i = 0; i < mono.size(); i++) { const double p = 2 * M_PI * f * (double)i / 48000; re += mono[i] * std::cos(p); im += mono[i] * std::sin(p); } return std::hypot(re, im) / (double)std::max<size_t>(1, mono.size()); };
        on = level(700); off = level(1900);
        printf("  AM tone %.4f, off tone %.4f\n", on, off);
        CHECK(on > 5 * off && on > 1e-4, "the AM tone is in the sound (%.4f vs %.4f)", on, off);
    }
    // retune so that the pager falls outside: it stops, the far one is still outside; the placed list is unchanged
    const double newCentre = kCentre + 2e6;
    ws.setCenterHz(newCentre);
    sync.apply(ws, placed, newCentre, kFs);
    CHECK(sync.ids().count(pg.absHz) == 0, "the pager left the capture");
    CHECK(sync.ids().count(am.absHz) == 0 || placedInCapture(am, newCentre, kFs), "the AM station follows the same rule");
    // and back
    ws.setCenterHz(kCentre);
    sync.apply(ws, placed, kCentre, kFs);
    CHECK(sync.ids().count(pg.absHz) == 1, "the pager is back after the retune");
    // no change: no churn
    const auto before = sync.ids();
    sync.apply(ws, placed, kCentre, kFs);
    CHECK(sync.ids() == before, "a repeated apply changes nothing");
    // taken off the list
    placed.erase(placed.begin());
    sync.apply(ws, placed, kCentre, kFs);
    CHECK(sync.ids().count(pg.absHz) == 0 && sync.ids().size() == 1, "a removed signal goes");
    return testSummary("wide_placed");
}
