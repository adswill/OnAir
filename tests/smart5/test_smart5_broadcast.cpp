// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 3.9 smart features, broadcast (docs/SMART5-broadcast-3.9.md items 1 - 10). Simulated only: synthetic receiver reports, deviation,
// carriers, envelopes and audio with offsets, noise and drift; no recording of these faults was on hand.
#include "dect2/smart5/am_carriers.h"
#include "dect2/smart5/am_health.h"
#include "dect2/smart5/blend_align.h"
#include "dect2/smart5/drm_besthour.h"
#include "dect2/smart5/ens_changes.h"
#include "dect2/smart5/fault_split.h"
#include "dect2/smart5/fm_txprint.h"
#include "dect2/smart5/fm_watchdog.h"
#include "dect2/smart5/hd_sideband.h"
#include "dect2/smart5/mpx412.h"
#include <cmath>
#include <complex>
#include <random>
#include <vector>
#include "../common/test_util.h"
using namespace dect2::smart5;
static const double kPi = 3.14159265358979323846;

static void testWatchdog() {
    std::mt19937 rng(1);
    std::normal_distribution<float> n(0, 1);
    FmWatchdog w;
    FmWatchInput in; in.carrier = true; in.snrDb = 40; in.stereo = true; in.pilotPct = 9; in.pi = 0x5201; in.ps = "LRT"; in.radioText = "song A";
    double t = 0;
    for (int i = 0; i < 1200; i++, t += 0.1) { in.devKhz = 55 + 7 * n(rng); w.update(in, t); }   // 2 min of normal programme
    CHECK(w.status().summary().empty(), "clean station flagged: %s", w.status().summary().c_str());
    for (int i = 0; i < 200; i++, t += 0.1) { in.devKhz = 10.5f + 0.5f * n(rng); w.update(in, t); }   // 20 s of silence: pilot + RDS only
    CHECK(w.status().deadAir && w.status().deadAirFor >= 12, "dead air %d %.1f", w.status().deadAir, w.status().deadAirFor);
    for (int i = 0; i < 20; i++, t += 0.1) { in.devKhz = 60; w.update(in, t); }
    CHECK(!w.status().deadAir, "dead air should clear");
    // the same silence on a weak station is not judged (noise lifts the deviation, the SNR gate stops it)
    in.snrDb = 12;
    for (int i = 0; i < 200; i++, t += 0.1) { in.devKhz = 10; w.update(in, t); }
    CHECK(!w.status().deadAir, "weak station judged");
    in.snrDb = 40;
    // pilot lost
    in.stereo = false; in.pilotPct = 0;
    for (int i = 0; i < 150; i++, t += 0.1) { in.devKhz = 50; w.update(in, t); }
    CHECK(w.status().pilotLost, "pilot lost");
    in.stereo = true; in.pilotPct = 9; w.update(in, t += 0.1);
    CHECK(!w.status().pilotLost, "pilot back");
    // RDS stuck: the text changed once, then stays 3 h
    in.radioText = "song B"; w.update(in, t += 0.1);
    for (int i = 0; i < 11000; i++) { t += 1; w.update(in, t); }
    CHECK(w.status().rdsStuck, "rds stuck");
    in.radioText = "song C"; w.update(in, t += 1);
    CHECK(!w.status().rdsStuck, "rds unstuck");
    // RDS clock 125 s fast and drifting 2 s per hour -> about 48 s/day
    for (int k = 0; k < 10; k++) {
        const double at = t + k * 3600;
        in.clockAt = at; in.clockUtc = (int64_t)std::llround(at + 125 + 2.0 * k);
        w.update(in, at);
    }
    CHECK(w.status().clockWrong && std::fabs(w.status().clockErrS - 143) < 1.5, "clock err %.1f", w.status().clockErrS);
    CHECK(std::fabs(w.status().clockDriftSPerDay - 48) < 4, "drift %.1f", w.status().clockDriftSPerDay);
    // over-deviation: 3 % of reports above 75 kHz
    FmWatchdog w2; FmWatchInput g = in; g.radioText = ""; g.clockUtc = 0;
    for (int i = 0; i < 1200; i++) { g.devKhz = (i % 33 == 0) ? 82 : 60; w2.update(g, i * 0.1); }
    CHECK(w2.status().overDeviation && w2.status().overDevSharePct > 2.5, "overdev %.1f", w2.status().overDevSharePct);
    CHECK(!w.events().empty(), "events");
}

static void testMpx() {
    const double fs = 192000;
    // a sine of 19 kHz peak deviation is 0 dBr by definition; one of 26.8 kHz is +3 dBr
    for (double peak : {19000.0, 26840.0, 9500.0}) {
        Mpx412Meter m(fs, 2);
        std::vector<float> d(fs * 2);
        for (size_t i = 0; i < d.size(); i++) d[i] = (float)(peak * std::sin(2 * kPi * 1000.3 * i / fs));
        m.push(d.data(), d.size());
        const double want = 20 * std::log10(peak / 19000);
        CHECK(m.report().valid && std::fabs(m.report().mpxPowerDbr - want) < 0.05, "peak %.0f: %.2f dBr want %.2f", peak, m.report().mpxPowerDbr, want);
    }
    // gaussian programme of 20 kHz rms: 10 log(400/180.5) = +3.46 dBr, 99 % point 2.576 sigma = 51.5 kHz, above 75 kHz 0.02 %
    std::mt19937 rng(2); std::normal_distribution<float> n(0, 20000);
    Mpx412Meter m(fs, 5);
    std::vector<float> d(fs);
    for (int s = 0; s < 5; s++) { for (auto& v : d) v = n(rng); m.push(d.data(), d.size()); }
    const auto& r = m.report();
    CHECK(std::fabs(r.mpxPowerDbr - 3.46) < 0.05, "gauss dBr %.2f", r.mpxPowerDbr);
    CHECK(std::fabs(r.p99Khz - 51.5) < 1.0, "p99 %.1f", r.p99Khz);
    CHECK(std::fabs(r.p50Khz - 13.5) < 1.0, "p50 %.1f", r.p50Khz);
    CHECK(r.over75Pct < 0.05, "over75 %.3f", r.over75Pct);
}

static void testTxPrint() {
    std::mt19937 rng(3); std::normal_distribution<double> n(0, 1);
    FmTxPrint p;
    // transmitter A: +14 Hz, pilot 9.1 %, -0.5 dBr; transmitter B of the same PI: +31 Hz, pilot 8.2 %, +1.8 dBr
    int id = 0;
    for (int i = 0; i < 60; i++) id = p.observe(0x5201, 99.5, {14 + 1.5 * n(rng), 9.1 + 0.1 * n(rng), -0.5 + 0.3 * n(rng)});
    CHECK(id == 1 && !p.changed(0x5201, 99.5), "A learnt id %d", id);
    for (int i = 0; i < 60; i++) id = p.observe(0x5201, 99.5, {31 + 1.5 * n(rng), 8.2 + 0.1 * n(rng), 1.8 + 0.3 * n(rng)});
    CHECK(id == 2 && p.changed(0x5201, 99.5), "B is a second transmitter, id %d", id);
    for (int i = 0; i < 10; i++) id = p.observe(0x5201, 99.5, {14 + 1.5 * n(rng), 9.1 + 0.1 * n(rng), -0.5 + 0.3 * n(rng)});
    CHECK(id == 1, "A again %d", id);
    // fading between the two: alternate reports never settle a third signature
    for (int i = 0; i < 100; i++) p.observe(0x5201, 99.5, i % 2 ? FmPrintSample{22, 8.7, 0.6} : FmPrintSample{40, 7.5, 3});
    CHECK(p.signatures(0x5201, 99.5)->size() == 2, "no third: %zu", p.signatures(0x5201, 99.5)->size());
    p.setLabel(0x5201, 99.5, 1, "Vilnius");
    FmTxPrint q; q.load(p.save());
    CHECK(q.signatures(0x5201, 99.5) && q.signatures(0x5201, 99.5)->size() == 2 && (*q.signatures(0x5201, 99.5))[0].label == "Vilnius", "save/load");
}

static void testAmCarriers() {
    std::mt19937 rng(4); std::normal_distribution<float> n(0, 1);
    for (double tune : {0.0, 3.3, -11.7}) {   // the radio tuned off the channel: offsets move together
        const double fs = 12000;
        AmCarrierAnalyser a;
        AmCarrierParams p; p.spanHz = 400; p.resolutionHz = 0.2; p.averages = 3;
        a.configure(fs, p);
        a.setBook({{1.37 + tune, 0.3, "Station B"}});
        struct C { double f, amp, drift; } cs[] = {{0.0, 1.0, 0}, {1.37, 0.25, 0}, {-7.9, 0.056, 0.0}, {9.6, 0.1, 0.002}};
        std::vector<std::complex<float>> x(4800);
        double ph[4] = {0, 1, 2, 3}, t = 0;
        int analyses = 0;
        std::vector<AmCarrier> last;
        while (analyses < 3) {
            for (auto& v : x) {
                std::complex<double> s(0, 0);
                for (int k = 0; k < 4; k++) {
                    const double f = cs[k].f + tune + cs[k].drift * t;
                    ph[k] += 2 * kPi * f / fs;
                    s += cs[k].amp * std::polar(1.0, ph[k]);
                }
                v = std::complex<float>((float)s.real() + 0.05f * n(rng), (float)s.imag() + 0.05f * n(rng));
                t += 1 / fs;
            }
            a.push(x.data(), x.size(), t);
            if (a.fresh()) { analyses++; last = a.carriers(); }
        }
        CHECK(last.size() >= 4, "tune %.1f: %zu carriers", tune, last.size());
        if (last.size() >= 4) {
            CHECK(std::fabs(last[0].offsetHz - tune) < 0.05, "main %.3f", last[0].offsetHz);
            bool b = false, c = false, d = false;
            for (auto& k : last) {
                if (std::fabs(k.offsetHz - (1.37 + tune)) < 0.05) { b = true; CHECK(std::fabs(k.levelDb + 12) < 1.5 && k.name == "Station B" && std::fabs(k.hetHz - 1.37) < 0.05, "B level %.1f name %s het %.2f", k.levelDb, k.name.c_str(), k.hetHz); }
                if (std::fabs(k.offsetHz - (-7.9 + tune)) < 0.05) { c = true; CHECK(std::fabs(k.levelDb + 25) < 1.5, "C level %.1f", k.levelDb); }
                if (std::fabs(k.offsetHz - (9.6 + tune)) < 0.6) { d = true; CHECK(k.seen < 2 || std::fabs(k.driftHzPerMin - 0.12) < 0.06, "drift %.3f Hz/min", k.driftHzPerMin); }
            }
            CHECK(b && c && d, "found B %d C %d D %d", b, c, d);
        }
    }
}

static void testAmHealth() {
    const double fs = 8000;
    std::mt19937 rng(5); std::normal_distribution<float> n(0, 1);
    auto run = [&](double depth, double shift, bool silent, int secs) {
        AmHealth h; h.configure(fs);
        std::vector<float> e(fs);
        for (int s = 0; s < secs; s++) {
            const double d = silent ? 0 : (s % 2 ? depth : depth * 0.2);   // loud and quiet seconds
            const double carrier = 1.0 + (s % 2 ? shift : 0);
            for (int i = 0; i < fs; i++) {
                const double m = d * std::sin(2 * kPi * 400 * i / fs);
                e[i] = (float)std::max(0.0, carrier * (1 + m)) + 0.003f * n(rng);
            }
            h.push(e.data(), e.size());
        }
        return h.report();
    };
    auto ok = run(0.85, 0, false, 30);
    CHECK(!ok.overModulated && !ok.carrierShift && !ok.deadAir && std::fabs(ok.posPeakPct - 85) < 3, "clean: %s pos %.1f", ok.summary().c_str(), ok.posPeakPct);
    auto ov = run(1.3, 0, false, 30);
    CHECK(ov.overModulated && ov.negPeakPct > 99, "overmod clip %.2f neg %.1f", ov.clipPct, ov.negPeakPct);
    auto sh = run(0.8, -0.08, false, 30);
    CHECK(sh.carrierShift && std::fabs(sh.carrierShiftPct + 8) < 1.5, "shift %.1f", sh.carrierShiftPct);
    auto da = run(0, 0, true, 15);
    CHECK(da.deadAir, "dead air");
}

static void testEnsChanges() {
    EnsChangeLog l;
    std::vector<EnsService> a = {{0xF201, "LRT Radijas", 96, 1, "EEP 3-A", "DAB+"}, {0xF202, "Klasika", 72, 2, "EEP 3-A", "DAB+"}, {0xF203, "Opus", 64, 3, "EEP 3-A", "DAB+"}};
    l.update(a, 0);
    CHECK(l.events().empty(), "first list is the baseline");
    auto b = a; b[1].bitrateKbps = 88; b[2].label = "Opus 3"; b.pop_back(); b.push_back({0xF204, "Pop", 48, 4, "EEP 2-A", "DAB+"});
    l.update(b, 10);   // Opus missing once: not removed yet; bitrate, new service
    CHECK(l.events().size() == 2, "%zu events", l.events().size());
    l.update(b, 11); l.update(b, 12);
    bool removed = false;
    for (auto& e : l.events()) if (e.what == "removed" && e.id == 0xF203) removed = true;
    CHECK(removed, "removed after 3 lists");
    auto c = b; c[0].subch = 7; c[0].protection = "EEP 2-A";
    l.update(c, 20);
    CHECK(l.events().size() == 5 && l.events().back().what == "moved", "%zu %s", l.events().size(), l.events().back().what.c_str());
    CHECK(l.csv().find("bitrate 72 -> 88") != std::string::npos, "csv");
}

static void testFaultSplit() {
    std::mt19937 rng(6); std::uniform_real_distribution<double> u(0, 1);
    // service 2's encoder fails at t = 30 while the FIC and services 1, 3 stay clean
    FaultSplit f;
    for (int t = 0; t < 60; t++) {
        f.common(t, 40, u(rng) < 0.05 ? 1 : 0);
        f.service(1, 40, 0); f.service(3, 40, u(rng) < 0.1 ? 1 : 0);
        f.service(2, t < 30 ? 40 : 10, t < 30 ? 0 : 30);
        f.tick(t);
    }
    auto v = f.verdict();
    CHECK(v.size() == 3 && v[1].kind == FaultKind::ServiceFeed && v[0].kind == FaultKind::Ok && v[2].kind == FaultKind::Ok, "feed: %s %s %s", FaultSplit::name(v[0].kind), FaultSplit::name(v[1].kind), FaultSplit::name(v[2].kind));
    CHECK(f.text(2, "Klasika").find("feed") != std::string::npos, "%s", f.text(2, "Klasika").c_str());
    // fading: everything fails together
    FaultSplit g;
    for (int t = 0; t < 30; t++) {
        const bool fade = t >= 15;
        g.common(t, fade ? 20 : 40, fade ? 20 : 0);
        for (unsigned s = 1; s <= 3; s++) g.service(s, fade ? 15 : 40, fade ? 25 : 0);
        g.tick(t);
    }
    CHECK(g.overall() == FaultKind::Reception && g.verdict()[1].kind == FaultKind::Reception, "reception %s", FaultSplit::name(g.overall()));
    // a service stops sending at all (off air at the source)
    FaultSplit h;
    for (int t = 0; t < 30; t++) { h.common(t, 40, 0); h.service(1, 40, 0); if (t < 10) h.service(2, 40, 0); h.tick(t); }
    CHECK(h.verdict()[1].kind == FaultKind::NoData, "no data %s", FaultSplit::name(h.verdict()[1].kind));
}

static void testBlend() {
    const double fs = 44100;
    std::mt19937 rng(7); std::normal_distribution<float> n(0, 1);
    // programme: filtered noise with a syllable-like envelope
    std::vector<float> prog(fs * 20);
    double lp = 0;
    for (size_t i = 0; i < prog.size(); i++) { lp += (n(rng) - lp) * 0.2; prog[i] = (float)(lp * (0.6 + 0.4 * std::sin(2 * kPi * 3.1 * i / fs))); }
    for (double offMs : {0.0, 37.3, -210.0, 640.0}) {
        for (double snrDb : {30.0, 10.0}) {
            BlendAlign b; b.configure(fs);
            const int off = (int)std::lround(offMs / 1000 * fs);
            const double gl = std::pow(10, -2.0 / 20), ns = std::pow(10, -snrDb / 20) * 0.3;
            std::vector<float> A(4410), D(4410);
            bool any = false;
            for (size_t s = 2 * fs; s + 4410 + 2 * fs < prog.size(); s += 4410) {
                for (int i = 0; i < 4410; i++) {
                    A[i] = prog[s + i] + (float)(ns * n(rng));
                    D[i] = (float)(gl * prog[s + i - off]) + (float)(ns * n(rng) * 0.3);
                }
                b.pushAnalog(A.data(), A.size()); b.pushDigital(D.data(), D.size());
                any |= b.analyse();
            }
            const auto& r = b.report();
            CHECK(any && r.valid && std::fabs(r.offsetMs - offMs) < 0.5, "off %.1f snr %.0f: got %.2f ms conf %.2f", offMs, snrDb, r.offsetMs, r.confidence);
            CHECK(r.misaligned == (std::fabs(offMs) > 20), "misaligned flag %.1f", offMs);
            if (snrDb > 20) CHECK(std::fabs(r.levelDb + 2) < 0.6, "level %.2f", r.levelDb);
        }
    }
}

static void testHdSideband() {
    const double bin = 1000;
    for (int step : {-20, -14, -10}) {
        for (double imb : {0.0, 6.0}) {
            std::vector<double> p;
            const double noise = 1e-7;
            double an = 0;
            for (double f = -250e3; f < 250e3; f += bin) { double v = noise; if (std::fabs(f) < 100e3) v += 1e-3 * std::exp(-f * f / (2 * 40e3 * 40e3)); p.push_back(v); if (std::fabs(f) < 100e3) an += v - noise; }
            // sidebands: total = an * 10^(step/10), split by the imbalance
            const double tot = an * std::pow(10, step / 10.0), up = tot / (1 + std::pow(10, -imb / 10)), lo = tot - up;
            const int nb = 69;
            for (size_t k = 0; k < p.size(); k++) {
                const double f = -250e3 + k * bin;
                if (f >= 129.4e3 && f < 198.4e3) p[k] += up / nb;
                if (f >= -198.4e3 && f < -129.4e3) p[k] += lo / nb;
            }
            auto r = hdSidebandLevels(p, -250e3, bin);
            CHECK(r.valid && r.stepDbc == step && std::fabs(r.totalDbc - step) < 0.3, "step %d imb %.0f: total %.2f step %d", step, imb, r.totalDbc, r.stepDbc);
            CHECK(std::fabs(r.imbalanceDb - imb) < 0.3 && r.asymmetric == (imb > 3), "imbalance %.2f", r.imbalanceDb);
        }
    }
    std::vector<double> p(500, 1e-7);
    for (int k = 150; k < 350; k++) p[k] += 1e-3;
    CHECK(hdSidebandLevels(p, -250e3, 1000).stepDbc == 0, "analogue only");
}

static void testDrmBestHour() {
    DrmBestHour b;
    // decodes well 18 - 21 UTC, badly at 12 UTC, over a week of listening
    for (int day = 0; day < 7; day++)
        for (int h : {12, 18, 19, 20, 21}) {
            const double t0 = day * 86400.0 + h * 3600.0;
            for (int m = 0; m < 20; m++) b.add("RNZ Pacific", 9700, t0 + m * 60, 60, h == 12 ? (m % 5 == 0) : (m % 10 != 0));
        }
    auto best = b.bestHours("RNZ Pacific", 9700);
    CHECK(best.size() == 5 && best.back() == 12, "best hours %zu last %d", best.size(), best.empty() ? -1 : best.back());
    const double now = 7 * 86400.0 + 10 * 3600 + 1234;   // 10:20 UTC
    CHECK(std::fabs(b.nextGood("RNZ Pacific", 9700, now) - (7 * 86400.0 + 18 * 3600)) < 1, "next good %.0f", b.nextGood("RNZ Pacific", 9700, now));
    DrmBestHour c; c.load(b.save());
    CHECK(c.entries() == 1 && std::fabs((*c.hours("RNZ Pacific", 9700))[19].rate() - 0.9) < 0.01, "save/load");
}

int main() {
    testWatchdog(); testMpx(); testTxPrint(); testAmCarriers(); testAmHealth();
    testEnsChanges(); testFaultSplit(); testBlend(); testHdSideband(); testDrmBestHour();
    return testSummary("smart5_broadcast");
}
