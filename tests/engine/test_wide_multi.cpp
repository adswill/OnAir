// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wide multi-signal decode, part 2 (core/include/dect2/engine/wide_session.h): TETRA carriers beside digital voice in one capture, signals marked by
// hand (addManual / remove), and an impairment sweep: carriers at the edge of the span, next to DC, off the raster, at 2.4 and 4 Msps. Synthetic IQ only.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/engine/modes.h"
#include "dect2/engine/wide_session.h"
#include "dect2/inmsat/inmsat_gen.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/tetra/tetra_gen.h"
#include "dect2/t2/t2rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

namespace {

struct Src { std::unique_ptr<ModeSynth> s; float gain; double shiftHz = 0; std::complex<double> ph{1, 0}; };

// where a synth puts its signal with cfo = 0: the power centroid within +-120 kHz
double baseOf(std::unique_ptr<ModeSynth> s, double fs) {
    std::vector<cf32> v((size_t)(fs * 0.1));
    s->generate(v.data(), v.size());
    double best = 0, bf = 0;
    for (double f = -120000; f <= 120000; f += 2000) {
        const double w = 2 * M_PI * f / fs; std::complex<double> a = 0;
        for (size_t i = 0; i < v.size(); i += 8) a += std::complex<double>(v[i]) * std::polar(1.0, -w * (double)i);
        if (std::abs(a) > best) { best = std::abs(a); bf = f; }
    }
    return bf;
}

struct Rig {
    double fs, centre;
    std::vector<Src> src;
    WideSession ws;
    std::vector<cf32> sum, tmp;
    std::mt19937 rng{5};
    std::normal_distribution<float> nd{0.f, 0.0004f};
    double t = 0;
    Rig(double f, double c) : fs(f), centre(c) { sum.resize((size_t)(fs * 0.05)); tmp.resize(sum.size()); }
    void addSynth(std::unique_ptr<ModeSynth> s, double wantOff, float gain, double base) { src.push_back({std::move(s), gain, wantOff - base}); }
    void run(double secs) {
        const size_t blk = sum.size();
        const double end = t + secs;
        for (; t < end; t += 0.05) {
            std::fill(sum.begin(), sum.end(), cf32(0, 0));
            for (Src& q : src) {
                q.s->generate(tmp.data(), blk);
                const std::complex<double> st = std::polar(1.0, 2 * M_PI * q.shiftHz / fs);
                std::complex<double> ph = q.ph;
                for (size_t i = 0; i < blk; i++) { sum[i] += tmp[i] * cf32((float)ph.real(), (float)ph.imag()) * q.gain; ph *= st; }
                q.ph = ph / std::abs(ph);
            }
            for (size_t i = 0; i < blk; i++) sum[i] += cf32(nd(rng), nd(rng));
            ws.feed(sum.data(), blk);
        }
    }
    WsEntry find(WsKind k, double nearOff = 1e18) {
        WsEntry best; double bd = 1e18;
        for (const WsEntry& e : ws.entries()) if (e.kind == k) { const double d = nearOff > 1e17 ? 0 : std::fabs(e.offsetHz - nearOff); if (d < bd) { bd = d; best = e; } }
        return best;
    }
    int count(WsKind k) { int n = 0; for (const WsEntry& e : ws.entries()) n += e.kind == k; return n; }
    void dump(const char* title) {
        printf("%s (%.0f s):\n", title, t);
        for (const WsEntry& e : ws.entries())
            printf("  id %d %-10s %-14s %+9.1f kHz %5.1f dB cost %.3f [%s] %s | %s\n", e.id, wsKindName(e.kind), wsStateName(e.state), e.offsetHz / 1e3, e.snrDb, e.cost, e.locked ? "locked" : "-", e.info.c_str(), e.identity.c_str());
    }
};

}  // namespace

int main() {
    // ---- 1. TETRA carriers (the span decoder) beside DMR in one capture
    {
        const double fs = 2.4e6, centre = kTetraTestCentreHz;
        Rig r(fs, centre);
        SynthConfig ct; ct.mode = 48; ct.snrDb = 28; ct.modeOpt[0] = 1;   // two cells: 392.4125 and 392.6 MHz
        r.src.push_back({makeTetraSynth(ct, fs), 0.5f, 0});
        SynthConfig cd; cd.snrDb = 28;
        const double offDmr = +903000;
        r.addSynth(makeDmrSynth(cd, fs), offDmr, 0.5f, baseOf(makeDmrSynth(cd, fs), fs));
        WsConfig cfg; cfg.spanRate = fs; cfg.centerHz = centre; cfg.cpuBudget = 0; cfg.tv = false;
        r.ws.configure(cfg);
        r.ws.useFrequencyDatabase(false);
        r.run(30.0);
        r.ws.wait();
        r.run(2.0);
        r.dump("TETRA + DMR");
        const int nt = r.count(WsKind::Tetra);
        CHECK(nt >= 2, "%d TETRA entries (want the 2 carriers)", nt);
        for (const WsEntry& e : r.ws.entries()) if (e.kind == WsKind::Tetra) {
            CHECK(e.state == WsState::Running, "TETRA carrier at %+.1f kHz is %s", e.offsetHz / 1e3, wsStateName(e.state));
            const bool a = std::fabs(e.offsetHz - (392.4125e6 - centre)) < 5e3, b = std::fabs(e.offsetHz - (392.6e6 - centre)) < 5e3;
            CHECK(a || b, "TETRA entry at %+.1f kHz is on neither cell", e.offsetHz / 1e3);
            CHECK(e.info.find("cells") != std::string::npos, "TETRA entry shows nothing from its decoder ('%s')", e.info.c_str());
        }
        const WsEntry d = r.find(WsKind::Dmr);
        CHECK(d.kind == WsKind::Dmr && std::fabs(d.offsetHz - offDmr) < 3000 && d.state == WsState::Running, "DMR beside TETRA: %s %+.1f kHz", wsKindName(d.kind), d.offsetHz / 1e3);
        // the TETRA decoder found both cells
        const WsEntry t0 = r.find(WsKind::Tetra);
        int cells = 0; sscanf(t0.info.c_str(), "%d cells", &cells);
        CHECK(cells >= 2, "TETRA decoder reports %d cells", cells);
        // switching one TETRA entry off leaves the decoder running for the other
        const int idOff = t0.id;
        r.ws.setEnabled(idOff, false);
        r.run(6.0);
        CHECK(r.find(WsKind::Tetra).state == WsState::Running || r.count(WsKind::Tetra) == 2, "switching one TETRA entry off");
        int off = 0, run = 0;
        for (const WsEntry& e : r.ws.entries()) if (e.kind == WsKind::Tetra) { off += e.state == WsState::Off; run += e.state == WsState::Running; }
        CHECK(off == 1 && run >= 1, "TETRA entries off %d running %d", off, run);
    }

    // ---- 1b. Inmarsat (L band): all the carriers of the capture, taken by one span decoder
    {
        const double fs = 2.4e6, centre = 1545.0e6;
        Rig r(fs, centre);
        SynthConfig ci; ci.mode = 210; ci.snrDb = 28;
        r.src.push_back({makeInmsatSynth(ci, fs), 0.5f, 0});
        WsConfig cfg; cfg.spanRate = fs; cfg.centerHz = centre; cfg.cpuBudget = 0; cfg.tv = false;
        r.ws.configure(cfg);
        r.ws.useFrequencyDatabase(false);
        r.run(24.0);
        r.ws.wait();
        r.run(2.0);
        r.dump("Inmarsat");
        const int ni = r.count(WsKind::Inmarsat);
        CHECK(ni >= 1, "no Inmarsat entry in the L band capture");
        const WsEntry e = r.find(WsKind::Inmarsat);
        CHECK(e.state == WsState::Running && e.info.find("carriers") != std::string::npos, "Inmarsat entry %s '%s'", wsStateName(e.state), e.info.c_str());
        int carriers = 0; sscanf(e.info.c_str(), "%d carriers", &carriers);
        CHECK(carriers >= 2, "the Inmarsat decoder found %d carriers", carriers);
    }

    // ---- 2. a signal marked by hand
    {
        const double fs = 2.4e6, centre = 440.0e6;
        Rig r(fs, centre);
        SynthConfig cd; cd.snrDb = 28;
        const double offDmr = -603000;
        r.addSynth(makeDmrSynth(cd, fs), offDmr, 0.6f, baseOf(makeDmrSynth(cd, fs), fs));
        WsConfig cfg; cfg.spanRate = fs; cfg.centerHz = centre; cfg.cpuBudget = 0; cfg.tv = false;
        r.ws.configure(cfg);
        r.ws.useFrequencyDatabase(false);
        r.run(8.0);
        // a hand mark on nothing but noise: listed, not identified, then given a kind by hand
        const int idNoise = r.ws.addManual(+700000, 12500);
        const int idDmr = r.ws.addManual(offDmr, 12500, WsKind::Dmr);
        CHECK(idNoise != idDmr && idNoise > 0, "manual ids %d %d", idNoise, idDmr);
        r.run(10.0);
        r.ws.wait();
        r.run(2.0);
        r.dump("marked by hand");
        WsEntry en, ed;
        for (const WsEntry& e : r.ws.entries()) { if (e.id == idNoise) en = e; if (e.id == idDmr) ed = e; }
        CHECK(en.id == idNoise && std::fabs(en.offsetHz - 700000) < 1 && std::fabs(en.absHz - (centre + 700000)) < 1, "the mark on noise is not listed at its place");
        CHECK(en.state == WsState::Unidentified, "the mark on noise is %s, not unidentified", wsStateName(en.state));
        CHECK(ed.id == idDmr && ed.kind == WsKind::Dmr && ed.state == WsState::Running, "the forced DMR mark: %s %s", wsKindName(ed.kind), wsStateName(ed.state));
        r.ws.setKind(idNoise, WsKind::Nfm);
        r.run(3.0);
        for (const WsEntry& e : r.ws.entries()) if (e.id == idNoise) en = e;
        CHECK(en.kind == WsKind::Nfm && en.state == WsState::Running, "the mark given a kind by hand: %s %s", wsKindName(en.kind), wsStateName(en.state));
        r.ws.remove(idNoise);
        r.run(1.0);
        bool gone = true;
        for (const WsEntry& e : r.ws.entries()) if (e.id == idNoise) gone = false;
        CHECK(gone, "a removed mark is still listed");
        // removing a found carrier switches it off (it would be found again)
        const WsEntry found = [&] { WsEntry b; for (const WsEntry& e : r.ws.entries()) if (e.id != idDmr && e.kind == WsKind::Dmr) b = e; return b; }();
        if (found.id) { r.ws.remove(found.id); r.run(1.0); WsEntry f2; for (const WsEntry& e : r.ws.entries()) if (e.id == found.id) f2 = e; CHECK(f2.state == WsState::Off, "a removed found carrier is %s", wsStateName(f2.state)); }
    }

    // ---- 3. impairment sweep: edge of the span, next to DC, off the raster, two rates
    struct Sc { const char* name; double fs; double dmrOff, p25Off; };
    const Sc scs[] = {
        {"2.4 Msps, both near the edges (+-1.0 MHz), off the raster", 2.4e6, -1.0e6 + 3100, +1.0e6 - 4700},
        {"2.4 Msps, DMR next to DC, P25 at -150 kHz", 2.4e6, +4000, -150300},
        {"4 Msps, carriers at +-1.7 MHz", 4.0e6, +1.7e6 + 2200, -1.7e6 - 1800},
    };
    for (const Sc& sc : scs) {
        // quick run: the 4 Msps case already has both carriers near the span edges and off the raster; ONAIR_TEST_LONG=1 adds the 2.4 Msps edge case
        if (!testLong() && &sc == &scs[0]) continue;
        Rig r(sc.fs, 440.0e6);
        SynthConfig cd; cd.snrDb = 28; SynthConfig cp; cp.snrDb = 28;
        r.addSynth(makeDmrSynth(cd, sc.fs), sc.dmrOff, 0.6f, baseOf(makeDmrSynth(cd, sc.fs), sc.fs));
        r.addSynth(makeP25Synth(cp, sc.fs), sc.p25Off, 0.6f, -50000);   // the receivers' tuning offset: P25 makes its signal 50 kHz below 0
        WsConfig cfg; cfg.spanRate = sc.fs; cfg.centerHz = 440.0e6; cfg.cpuBudget = 0; cfg.tv = false;
        r.ws.configure(cfg);
        r.ws.useFrequencyDatabase(false);
        r.run(14.0);
        r.ws.wait();
        r.run(2.0);
        r.dump(sc.name);
        const WsEntry d = r.find(WsKind::Dmr, sc.dmrOff), p = r.find(WsKind::P25, sc.p25Off);
        CHECK(d.kind == WsKind::Dmr && std::fabs(d.offsetHz - sc.dmrOff) < 3500 && d.state == WsState::Running, "%s: DMR %s %+.1f kHz %s", sc.name, wsKindName(d.kind), d.offsetHz / 1e3, wsStateName(d.state));
        CHECK(p.kind == WsKind::P25 && std::fabs(p.offsetHz - sc.p25Off) < 3500 && p.state == WsState::Running, "%s: P25 %s %+.1f kHz %s", sc.name, wsKindName(p.kind), p.offsetHz / 1e3, wsStateName(p.state));
    }
    return testSummary("wide_multi");
}
