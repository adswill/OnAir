// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Argos receiver on its test signal.
// Uplink: every made-up platform arrives with its ID and data, clean, at 16 dB in 2.4 kHz, with radio offsets of a few kHz, with the carrier
// drifting 40 Hz/s during each burst, at another sample rate; a burst of another modulation is listed as "not A2" and never decoded; the
// per-platform frequency list shows a burst-to-burst step as its drift. Noise alone gives nothing.
// Downlink: HDLC frames with a good FCS through a pass with +-9 kHz of Doppler (150 Hz/s at its fastest), as BPSK, as NRZI, and the
// measured Doppler follows the generated curve; noise alone gives no frame.
#include "dect2/argos/argos_gen.h"
#include "dect2/argos/argos_rx.h"
#include "dect2/argos/argos_tel.h"
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static ArgosTelemetry run(const SynthConfig& cfg, double rate, double seconds, int link, bool noiseOnly = false, std::vector<std::pair<double, double>>* dop = nullptr) {
    ArgosReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-argosTuning().tuneOffsetHz);
    rx.setLink(link);
    auto syn = makeArgosSynth(cfg, rate);
    std::vector<cf32> buf(16384);
    ArgosTelemetry t;
    uint64_t seq = 0;
    unsigned s = 99;
    double done = 0;
    for (; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) {
            for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); }
        } else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        ArgosTelemetry n;
        if (rx.telemetry(n, seq)) {
            seq = n.seq; t = n;
            if (dop && t.dlLocked) dop->push_back({(done + (double)buf.size()) / rate, t.dlCarrierHz});
        }
    }
    return t;
}

static int uplinkFound(const ArgosTelemetry& t, const char* what, double cfo) {
    int got = 0;
    std::set<std::string> want;
    for (const argos::TestPtt& p : argos::testPtts()) {
        char id[16]; snprintf(id, sizeof id, "%07X", p.id);
        want.insert(id);
        bool ok = false;
        for (const ArgosBurst& e : t.log)
            if (e.kind == 1 && e.msg.idHex == id && e.msg.data == p.data && e.msg.dataBits == p.dataBits && std::fabs(e.carrierHz - (p.offsetHz + cfo)) < 60) { ok = true; break; }
        CHECK(ok, "%s: platform %s missing", what, id);
        if (!ok) for (const ArgosBurst& e : t.log) printf("    burst kind %d %s q %.2f carrier %.0f ms, %d bits, %+.0f Hz, %.1f dB\n", e.kind, e.msg.idHex.c_str(), e.quality, e.carrierMs, e.msg.totalBits, e.carrierHz, e.snrDb);
        got += ok;
    }
    int stray = 0;
    for (const ArgosBurst& e : t.log) if (e.kind == 1 && !want.count(e.msg.idHex)) { stray++; printf("  stray %s q %.2f\n", e.msg.idHex.c_str(), e.quality); }
    CHECK(stray == 0, "%s: %d stray messages", what, stray);
    printf("  %-30s %d/3 platforms, %llu bursts, %llu decoded, %llu other\n", what, got, (unsigned long long)t.bursts, (unsigned long long)t.blocksOk, (unsigned long long)t.otherBursts);
    return got;
}

static void downlinkFrames(const ArgosTelemetry& t, const char* what, int coding) {
    std::set<std::string> want;
    for (const auto& f : argos::testDownlinkFrames()) { std::string h; char b[4]; for (uint8_t v : f) { snprintf(b, sizeof b, "%02X", v); h += b; } want.insert(h); }
    std::set<std::string> seen;
    int stray = 0;
    for (const ArgosDlFrame& f : t.frames) { if (!f.fcsOk) continue; if (want.count(f.hex)) seen.insert(f.hex); else stray++; }
    CHECK(seen.size() == want.size(), "%s: %zu of %zu frames", what, seen.size(), want.size());
    CHECK(stray == 0, "%s: %d stray frames", what, stray);
    // NRZ and inverted NRZ are the same signal with the carrier phase half a turn away: either is right for NRZ
    CHECK(coding == 2 ? t.dlCoding == 2 : (t.dlCoding == 0 || t.dlCoding == 1), "%s: coding %d, want %s", what, t.dlCoding, coding == 2 ? "NRZI" : "NRZ");
    printf("  %-30s %zu/%zu frame kinds, %llu good, %llu bad FCS, flags %llu/%llu/%llu, carrier %+.0f Hz\n", what, seen.size(), want.size(), (unsigned long long)t.blocksOk,
           (unsigned long long)t.dlFcsBad, (unsigned long long)t.dlFlags[0], (unsigned long long)t.dlFlags[1], (unsigned long long)t.dlFlags[2], t.dlCarrierHz);
}

int main() {
    const double rate = 256000;
    // uplink: three platforms, bursts of up to 0.92 s, 2 s apart
    { SynthConfig c; c.snrDb = 40; uplinkFound(run(c, rate, 9.5, 1), "clean", 0); }
    { SynthConfig c; c.snrDb = 16; c.modeOpt[3] = 3; uplinkFound(run(c, rate, 9.5, 1), "16 dB", 0); }
    { SynthConfig c; c.snrDb = 22; c.cfoHz = 4000; uplinkFound(run(c, rate, 9.5, 1), "+4 kHz", 4000); }
    { SynthConfig c; c.snrDb = 22; c.cfoHz = -6000; c.modeVal[0] = 40; uplinkFound(run(c, rate, 9.5, 1), "-6 kHz, drift 40 Hz/s", -6000); }
    { SynthConfig c; c.snrDb = 25; uplinkFound(run(c, 500000, 9.5, 0), "500 ksps, automatic link", 0); }
    {   // another modulation each cycle
        SynthConfig c; c.snrDb = 30; c.modeOpt[2] = 1;
        const ArgosTelemetry t = run(c, rate, 12.5, 1);
        uplinkFound(t, "with a QPSK burst", 0);
        CHECK(t.otherBursts >= 1, "the QPSK burst was not listed as other");
        for (const ArgosBurst& e : t.log) if (e.kind == 1) CHECK(std::fabs(e.carrierHz + 20000) > 500, "the QPSK burst was decoded as A2");
    }
    {   // the frequency list: each platform 25 Hz higher every burst
        SynthConfig c; c.snrDb = 30; c.modeVal[2] = 25;
        const ArgosTelemetry t = run(c, rate, 4 * 7.72, 1);
        CHECK(t.ptts.size() == 3, "%zu platforms", t.ptts.size());
        for (const ArgosPtt& p : t.ptts) {
            const double perMin = 25.0 * 60.0 / 7.72;   // one cycle of three bursts takes 7.72 s
            CHECK(p.count >= 3 && std::fabs(p.driftHzPerMin - perMin) < 0.25 * perMin, "platform %s: %d bursts, drift %.0f Hz/min (about %.0f)", p.idHex.c_str(), p.count, p.driftHzPerMin, perMin);
        }
    }
    {
        SynthConfig c;
        const ArgosTelemetry t = run(c, rate, 8, 1, true);
        CHECK(t.log.empty(), "uplink noise: %zu bursts listed", t.log.size());
    }
    // downlink
    { SynthConfig c; c.snrDb = 30; c.modeOpt[0] = 1; downlinkFrames(run(c, rate, 6, 2), "downlink clean", 0); }
    { SynthConfig c; c.snrDb = 20; c.modeOpt[0] = 1; c.cfoHz = 3000; c.modeOpt[3] = 4; downlinkFrames(run(c, rate, 6, 2), "downlink 20 dB, +3 kHz", 0); }
    { SynthConfig c; c.snrDb = 25; c.modeOpt[0] = 1; c.modeOpt[1] = 1; downlinkFrames(run(c, rate, 6, 2), "downlink BPSK", 0); }
    { SynthConfig c; c.snrDb = 25; c.modeOpt[0] = 1; c.modeOpt[2] = 1; downlinkFrames(run(c, rate, 6, 2), "downlink NRZI", 2); }
    {   // a pass: +-9 kHz, 60 s time constant, closest approach 10 s in: the fastest part of the curve
        SynthConfig c; c.snrDb = 25; c.modeOpt[0] = 1; c.modeVal[0] = 9000; c.modeVal[1] = 60; c.modeVal[3] = 10;
        std::vector<std::pair<double, double>> dop;
        const ArgosTelemetry t = run(c, rate, 20, 2, false, &dop);
        downlinkFrames(t, "downlink pass, 150 Hz/s", 0);
        double worst = 0;
        int n = 0;
        for (const auto& [ts, hz] : dop) if (ts > 3) { worst = std::max(worst, std::fabs(hz - argos::testDoppler(ts, 9000, 60, 10))); n++; }
        CHECK(n > 20 && worst < 40, "Doppler follow: %d points, worst %.1f Hz", n, worst);
        printf("  Doppler follow: %d points, worst %.1f Hz\n", n, worst);
    }
    {
        SynthConfig c;
        const ArgosTelemetry t = run(c, rate, 6, 2, true);
        int good = 0;
        for (const ArgosDlFrame& f : t.frames) good += f.fcsOk;
        CHECK(good == 0, "downlink noise: %d frames", good);
    }
    return testSummary("argos rx");
}
