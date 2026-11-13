// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The ham science event monitors (hamsci_evt.h) on simulated signals: Schumann resonances (sound card rate and 200 Hz, frequency error and drift, noise
// sweep, 50 Hz mains, transients), whistlers and tweeks (dispersion, noise-only control, 24 and 48 kHz), the Moon echo test (carrier drift, noise, a
// control without an echo), Radio Jove and CALLISTO-style bursts (narrowband interference keyed in, a gain drift, a drifting burst) and the beacon event log
// (clean, Doppler-spread and fluttering openings, a noise-only control). Simulated only: no recording of a real event was available.
// Long sweeps (more noise levels) run when ONAIR_TEST_LONG=1.
#include "dect2/hamsci/hamsci_evt.h"
#include "hamsci_testutil.h"
#include <cstdlib>
using namespace dect2::hamsci;
using cf = std::complex<float>;

static bool longRun() { const char* e = std::getenv("ONAIR_TEST_LONG"); return e && e[0] == '1'; }

static std::vector<EvtEvent> run(int kind, const std::vector<cf>& x, double rate, double offsetHz, EvtState* last = nullptr, double thr = -1) {
    EvtMonitor m(kind);
    m.configure(rate);
    m.setSignalOffset(offsetHz);
    if (thr > 0) m.setThresholdDb(thr);
    std::vector<EvtEvent> all;
    for (size_t i = 0; i < x.size(); i += 65536) {
        m.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        for (auto& e : m.take()) all.push_back(e);
    }
    if (last) *last = m.state();
    return all;
}
static int countType(const std::vector<EvtEvent>& v, const char* t) { int n = 0; for (auto& e : v) if (e.type == t) n++; return n; }

int main() {
    // ---- Schumann
    {
        EvtGenOpt o; o.seconds = 420; o.seed = 5;
        auto x = evtGenSchumann(200, o);
        EvtState st;
        auto ev = run(kEvtSchumann, x, 200, 0, &st);
        int found = 0;
        for (float f : st.peakHz) if (f > 0) found++;
        std::printf("schumann clean: %d peaks, f1 %.2f Hz (%.1f dB, Q %.1f), %zu transient events\n", found, st.peakHz.empty() ? 0.0 : (double)st.peakHz[0], st.peakDb.empty() ? 0.0 : (double)st.peakDb[0], st.peakQ.empty() ? 0.0 : (double)st.peakQ[0], ev.size());
        CHECK(found >= 4, "peaks found %d", found);
        CHECK(!st.peakHz.empty() && std::fabs(st.peakHz[0] - 7.83) < 0.8, "fundamental %.2f", st.peakHz.empty() ? 0.0 : (double)st.peakHz[0]);
        CHECK(st.peakHz.size() == 5 && st.peakHz[1] > 12.5 && st.peakHz[1] < 15.7, "second mode %.2f", st.peakHz.size() > 1 ? (double)st.peakHz[1] : 0.0);
        CHECK(ev.empty(), "no transient expected with 50 Hz mains and noise only, got %zu", ev.size());
        CHECK(!st.trend.empty() && st.trend.size() >= 5, "trend per minute %zu", st.trend.size());
        // sound card clock error (+0.4 Hz on every resonance) and a drift: the peaks follow
        EvtGenOpt o2 = o; o2.offsetHz = 0.4; o2.driftHz = 0.2; o2.seed = 6;
        EvtState st2;
        run(kEvtSchumann, evtGenSchumann(200, o2), 200, 0, &st2);
        CHECK(!st2.peakHz.empty() && std::fabs(st2.peakHz[0] - 8.3) < 0.9, "offset fundamental %.2f", st2.peakHz.empty() ? 0.0 : (double)st2.peakHz[0]);
        // noise sweep: the fundamental survives +6 dB of background, the number of peaks falls
        std::printf("schumann noise sweep (peaks found):");
        for (double nd : (longRun() ? std::vector<double>{0, 3, 6, 9, 12} : std::vector<double>{0, 6})) {
            EvtGenOpt o3 = o; o3.noiseDb = nd; o3.seed = 7;
            EvtState s3;
            run(kEvtSchumann, evtGenSchumann(200, o3), 200, 0, &s3);
            int f3 = 0;
            for (float f : s3.peakHz) if (f > 0) f3++;
            std::printf(" %+.0f dB:%d", nd, f3);
            if (nd <= 6) CHECK(!s3.peakHz.empty() && s3.peakHz[0] > 6.5 && s3.peakHz[0] < 9.2, "fundamental at noise %+.0f dB: %.2f", nd, s3.peakHz.empty() ? 0.0 : (double)s3.peakHz[0]);
        }
        std::printf("\n");
        // transients every 60 s from 70 s
        EvtGenOpt o4 = o; o4.level = 2.0; o4.seed = 8; o4.seconds = 330;
        auto evt = run(kEvtSchumann, evtGenSchumann(200, o4), 200, 0);
        int hit = 0;
        for (double t0 : {70.0, 130.0, 190.0, 250.0, 310.0})
            for (auto& e : evt) if (e.type == "transient" && std::fabs(e.timeSec + 0.5 - t0) < 3.0) { hit++; break; }
        std::printf("schumann transients: %zu events, %d of 5 at the expected times\n", evt.size(), hit);
        CHECK(hit >= 4, "transients found %d of 5", hit);
        // the sound card rate: 48 kHz, held 200 Hz signal averaged back
        EvtGenOpt o5; o5.seconds = 150; o5.seed = 9;
        EvtState s5;
        run(kEvtSchumann, evtGenSchumann(48000, o5), 48000, 0, &s5);
        CHECK(!s5.peakHz.empty() && std::fabs(s5.peakHz[0] - 7.83) < 0.9, "48 kHz fundamental %.2f", s5.peakHz.empty() ? 0.0 : (double)s5.peakHz[0]);
    }
    // ---- whistler and tweek
    {
        EvtGenOpt o; o.seconds = 45; o.seed = 11;
        const std::vector<double> wh = {8.0, 24.0}, tw = {5.0, 16.0, 33.0, 40.0};
        auto x = evtGenWhistler(24000, o, wh, tw, 60.0);
        EvtState st;
        auto ev = run(kEvtWhistler, x, 24000, 0, &st);
        // the generator's second whistler has D = 60 as well; a different one is tested below
        int w = 0, tk = 0;
        for (auto& e : ev) {
            if (e.type == "whistler") { for (double t0 : wh) if (std::fabs(e.timeSec - (t0 + 60.0 / std::sqrt(4000.0))) < 0.2 && std::fabs(e.aux1 / 60.0 - 1.0) < 0.2) w++; }
            if (e.type == "tweek") { for (double t0 : tw) if (std::fabs(e.timeSec - t0) < 0.08 && e.freqHz > 1650 && e.freqHz < 2150) tk++; }
        }
        std::printf("whistler: %zu events, whistlers right %d of 2, tweeks right %d of 4, sferics/min trend %.0f\n", ev.size(), w, tk, st.trend2.empty() ? 0.0 : (double)st.trend2.back());
        for (auto& e : ev) std::printf("  %s at %.3f s, %.0f Hz, %.1f dB, aux %.1f\n", e.type.c_str(), e.timeSec, e.freqHz, e.peakDb, e.aux1);
        CHECK(w >= 2, "whistlers %d of 2", w);
        CHECK(tk >= 3, "tweeks %d of 4", tk);
        CHECK((int)ev.size() <= 7, "no more than a few extra events: %zu", ev.size());
        // other dispersions
        for (double D : {35.0, 110.0}) {
            EvtGenOpt o2 = o; o2.seed = 12; o2.seconds = 25;
            auto e2 = run(kEvtWhistler, evtGenWhistler(24000, o2, {6.0}, {}, D), 24000, 0);
            bool ok = false;
            for (auto& e : e2) if (e.type == "whistler" && std::fabs(e.aux1 / D - 1.0) < 0.2) ok = true;
            CHECK(ok, "whistler with D = %.0f found", D);
        }
        // noise only (sferics only): nothing
        auto e0 = run(kEvtWhistler, evtGenWhistler(24000, o, {}, {}), 24000, 0);
        CHECK(countType(e0, "whistler") == 0 && countType(e0, "tweek") == 0, "noise and sferics only: %zu events", e0.size());
        // 48 kHz input rate (the front end averages pairs)
        EvtGenOpt o3 = o; o3.seed = 13; o3.seconds = 25;
        auto e3 = run(kEvtWhistler, evtGenWhistler(48000, o3, {7.0}, {15.0}), 48000, 0);
        CHECK(countType(e3, "whistler") >= 1 && countType(e3, "tweek") >= 1, "48 kHz: %d whistlers %d tweeks", countType(e3, "whistler"), countType(e3, "tweek"));
    }
    // ---- Moon echo
    {
        const std::vector<double> tx = {10, 20, 30, 40};
        std::printf("eme echo SNR sweep (echoes found of 4, mean delay error ms):");
        for (double snr : (longRun() ? std::vector<double>{16, 12, 10, 8, 6} : std::vector<double>{12, 8})) {
            EvtGenOpt o; o.seconds = 56; o.seed = 21; o.offsetHz = 15.0; o.driftHz = 10.0;
            auto x = evtGenEme(8000, o, tx, 1.5, 2.55, 120.0, 4.0, snr);
            auto ev = run(kEvtEme, x, 8000, 0);
            int n = 0; double err = 0;
            for (auto& e : ev) if (e.type == "echo") { n++; err += std::fabs(e.aux1 - 2.55); CHECK(std::fabs(e.freqHz - 120.0) < 12.0, "Doppler %.1f", e.freqHz); CHECK(e.aux2 <= 40.0 && e.aux2 >= 3.0, "spread %.1f", e.aux2); }
            std::printf(" %.0f dB:%d/%.0f", snr, n, n ? 1000 * err / n : 0.0);
            if (snr >= 12) { CHECK(n >= 3, "echoes found at %.0f dB: %d", snr, n); CHECK(n == 0 || err / n < 0.08, "delay error %.3f s", n ? err / n : 0.0); }
            CHECK(n <= 4, "no extra events");
        }
        std::printf("\n");
        // no echo (the echo level is 60 dB under the noise): no event
        EvtGenOpt o; o.seconds = 56; o.seed = 22;
        auto ev0 = run(kEvtEme, evtGenEme(8000, o, tx, 1.5, 2.55, 120.0, 4.0, -60.0), 8000, 0);
        CHECK(ev0.empty(), "no echo present but %zu events", ev0.size());
        // too short a pulse (a click) is no pulse; no carrier at all: nothing
        auto ev1 = run(kEvtEme, evtGenEme(8000, o, {}, 1.5, 2.55, 120.0, 4.0, 12.0), 8000, 0);
        CHECK(ev1.empty(), "no transmission, %zu events", ev1.size());
    }
    // ---- Radio Jove and CALLISTO style bursts
    {
        const double R = 250000;
        EvtGenOpt o; o.seconds = 45; o.seed = 31; o.driftHz = 1.0;   // 1 dB gain drift over the run
        auto x = evtGenBursts(R, o, {12.0, 28.0}, 1.5, 0.0, 200e3);
        // a keyed narrowband interferer at +60 kHz (one bin), 1 s on every 7 s
        for (size_t i = 0; i < x.size(); i++) {
            const double t = (double)i / R;
            if (std::fmod(t, 7.0) < 1.0) x[i] += cf((float)(0.4 * std::cos(2 * 3.14159265 * 60e3 * t)), (float)(0.4 * std::sin(2 * 3.14159265 * 60e3 * t)));
        }
        EvtState st;
        auto ev = run(kEvtJove, x, R, 0, &st);
        std::printf("jove: %zu bursts (expected 2), rows %zu, bins %zu\n", ev.size(), st.rows.size(), st.rows.empty() ? 0 : st.rows[0].size());
        CHECK(ev.size() == 2, "bursts %zu", ev.size());
        for (size_t i = 0; i < ev.size() && i < 2; i++) {
            CHECK(std::fabs(ev[i].timeSec - (i ? 28.0 : 12.0)) < 0.6, "burst %zu at %.2f", i, ev[i].timeSec);
            CHECK(std::fabs(ev[i].durSec - 1.5) < 0.5, "burst %zu duration %.2f", i, ev[i].durSec);
            CHECK(ev[i].peakDb > 3.0, "burst %zu peak %.1f dB", i, ev[i].peakDb);
        }
        // fainter bursts (3 dB) and a noise-only control
        EvtGenOpt o2 = o; o2.level = 3.0; o2.seed = 32;
        auto e2 = run(kEvtJove, evtGenBursts(R, o2, {12.0, 28.0}, 1.5, 0.0, 200e3), R, 0);
        CHECK(e2.size() == 2, "3 dB bursts %zu", e2.size());
        auto e3 = run(kEvtJove, evtGenBursts(R, o, {}, 1.5, 0.0, 200e3), R, 0);
        CHECK(e3.empty(), "noise only with a gain drift: %zu bursts", e3.size());
        // CALLISTO style: a burst that drifts up by 40 kHz per second, 60 kHz wide, 2.5 s long, 8 dB
        EvtGenOpt o3 = o; o3.level = 8.0; o3.seed = 33; o3.offsetHz = -50e3; o3.driftHz = 0;
        auto e4 = run(kEvtCallisto, evtGenBursts(R, o3, {15.0}, 2.5, 40e3, 60e3), R, 0);
        std::printf("callisto: %zu events", e4.size());
        for (auto& e : e4) std::printf(", %s drift %.1f kHz/s, %.1f s", e.type.c_str(), e.aux1 / 1e3, e.durSec);
        std::printf("\n");
        CHECK(e4.size() == 1, "callisto events %zu", e4.size());
        if (!e4.empty()) { CHECK(e4[0].aux1 > 15e3 && e4[0].aux1 < 70e3, "drift %.1f kHz/s", e4[0].aux1 / 1e3); CHECK(std::fabs(e4[0].timeSec - 15.0) < 0.8, "callisto start %.2f", e4[0].timeSec); }
        CHECK(!e4.empty() && e4[0].type == "drifting burst", "drift recognised");
    }
    // ---- beacon events
    {
        struct Case { const char* name; double spread, flutter, level; const char* want; };
        const Case cs[] = {{"steady", 0, 0, 20, "steady carrier"}, {"spread", 150, 0, 28, "Doppler-spread"}, {"flutter", 0, 6, 28, "fluttering"}};
        for (const Case& c : cs) {
            EvtGenOpt o; o.seconds = 90; o.seed = 41; o.offsetHz = 60.0; o.driftHz = 8.0; o.level = c.level;
            auto x = evtGenBeacon(8000, o, {20.0}, 30.0, c.spread, c.flutter);
            {   // diagnostics: the status line every 10 s
                EvtMonitor dm(kEvtAurora); dm.configure(8000);
                for (size_t i = 0; i < x.size(); i += 80000) { dm.feed(x.data() + i, std::min<size_t>(80000, x.size() - i)); std::printf("  %s t=%zu s: %s\n", c.name, (i + 80000) / 8000, dm.state().info.empty() ? "-" : dm.state().info[0].c_str()); }
            }
            auto ev = run(kEvtAurora, x, 8000, 0);
            std::printf("beacon %s: %zu events", c.name, ev.size());
            for (auto& e : ev) std::printf(", at %.1f s for %.1f s, %.1f dB, %s", e.timeSec, e.durSec, e.peakDb, e.note.c_str());
            std::printf("\n");
            CHECK(ev.size() == 1, "%s: %zu openings", c.name, ev.size());
            if (ev.size() == 1) {
                CHECK(std::fabs(ev[0].timeSec - 20.0) < 4.0 && std::fabs(ev[0].durSec - 30.0) < 8.0, "%s: time %.1f duration %.1f", c.name, ev[0].timeSec, ev[0].durSec);
                CHECK(ev[0].note.find(c.want) != std::string::npos, "%s: class '%s'", c.name, ev[0].note.c_str());
            }
        }
        // noise only for five minutes: no opening
        EvtGenOpt o; o.seconds = 300; o.seed = 42;
        auto e0 = run(kEvtAurora, evtGenBeacon(8000, o, {}, 30.0, 0, 0), 8000, 0);
        CHECK(e0.empty(), "noise only: %zu openings", e0.size());
        // CSV exports
        EvtEvent e; e.timeSec = 12.5; e.type = "burst"; e.note = "a \"quoted\" note";
        const std::string csv = evtCsv({e}, 1700000000);
        CHECK(csv.find("utc,signal_time_s") == 0 && csv.find("2023-11-14T22:13:32Z") != std::string::npos && csv.find("'quoted'") != std::string::npos, "events CSV: %s", csv.c_str());
        EvtState s; s.trend = {1.f, 2.f}; s.trend2 = {3.f, 4.f}; s.trendName = "a, b"; s.trend2Name = "c";
        const std::string tc = evtTrendCsv(s, 1700000000);
        CHECK(tc.find("\"a, b\"") != std::string::npos && tc.find("2023-11-14T22:14:20Z") != std::string::npos, "trend CSV: %s", tc.c_str());
    }
    std::printf(g_fails ? "hamsci event monitors: %d FAILED\n" : "hamsci event monitors: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
