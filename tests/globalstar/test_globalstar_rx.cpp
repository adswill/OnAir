// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Globalstar activity monitor: the test signal (globalstar_gen.h) through the receiver (globalstar_rx.h): a steady channel with Doppler offsets, SNRs and
// a radio clock error; a satellite pass (the Doppler track follows the sweep); bursts on a second channel (the event log); noise alone gives nothing.
#include "dect2/globalstar/globalstar_gen.h"
#include "dect2/globalstar/globalstar_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static GlobalstarTelemetry run(const SynthConfig& sc, double rate, double secs, bool noiseOnly, double rxRatePpm = 0) {
    std::unique_ptr<ModeSynth> g = makeGlobalstarSynth(sc, rate);
    GlobalstarReceiver r;
    r.configure(rate * (1 + rxRatePpm * 1e-6));
    r.setSignalOffset(0);
    r.setDialHz(1618.25e6);
    genutil::NoiseSource ns(3);
    std::vector<cf32> buf(1 << 16);
    GlobalstarTelemetry last;
    uint64_t seq = 0;
    const size_t total = (size_t)(secs * rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        if (noiseOnly) { for (auto& v : buf) v = 0; ns.add(buf.data(), buf.size(), 0.1f); } else g->generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        GlobalstarTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    return last;
}

int main() {
    const double rate = 6e6;
    // which channel the generator uses: the one nearest the centre of the band
    int ch = 0;
    { double best = 1e12; for (int c = 0; c < kGsChannels; c++) { const double d = std::fabs(globalstarChannelHz(c) - 1618.25e6); if (d < best && d + 0.74e6 <= 0.46 * rate) { best = d; ch = c; } } }

    // steady channel: Doppler and SNR sweeps
    for (double snr : {8.0, 15.0, 25.0})
        for (double cfo : {-30000.0, -9000.0, 0.0, 14000.0, 30000.0}) {
            SynthConfig sc; sc.modeOpt[0] = 1; sc.snrDb = snr; sc.cfoHz = cfo;
            const GlobalstarTelemetry t = run(sc, rate, 2.5, false);
            const GsChannel& c = t.chan[(size_t)ch];
            int others = 0;
            for (const GsChannel& o : t.chan) if (o.index != ch && o.active) others++;
            printf("steady SNR %4.1f cfo %+7.0f: active %d, Doppler %+8.0f Hz, SNR %.1f dB, others active %d\n", snr, cfo, c.active, c.dopplerHz, c.snrDb, others);
            CHECK(c.active, "snr %.0f cfo %.0f: channel not active", snr, cfo);
            CHECK(std::fabs(c.dopplerHz - cfo) < 3500.0, "snr %.0f cfo %.0f: Doppler %.0f", snr, cfo, c.dopplerHz);
            CHECK(std::fabs(c.snrDb - snr) < 3.0, "snr %.0f cfo %.0f: SNR %.1f", snr, cfo, c.snrDb);
            CHECK(others == 0, "other channels active: %d", others);
            CHECK(t.state == 2 && t.activeNow == 1 && t.dataValid, "state %d active %d", t.state, t.activeNow);
        }
    // a radio clock off by 100 ppm either way
    for (double ppm : {-100.0, 100.0}) {
        SynthConfig sc; sc.modeOpt[0] = 1; sc.snrDb = 15; sc.cfoHz = 10000;
        const GlobalstarTelemetry t = run(sc, rate, 2.0, false, ppm);
        const GsChannel& c = t.chan[(size_t)ch];
        printf("clock %+4.0f ppm: active %d, Doppler %+.0f Hz\n", ppm, c.active, c.dopplerHz);
        CHECK(c.active && std::fabs(c.dopplerHz - 10000) < 4000, "clock error %.0f ppm: %.0f Hz", ppm, c.dopplerHz);
    }
    // noise alone, and silence from the generator: nothing active, no event, no occupancy
    {
        SynthConfig sc;
        const GlobalstarTelemetry n1 = run(sc, rate, 6.0, true);
        sc.modeOpt[0] = 2; sc.snrDb = 12;
        const GlobalstarTelemetry n2 = run(sc, rate, 6.0, false);
        for (const GlobalstarTelemetry* t : {&n1, &n2}) {
            int act = 0; float occ = 0;
            for (const GsChannel& c : t->chan) { act += c.active; occ = std::max(occ, c.occupancy); }
            printf("noise: active %d, events %zu, max occupancy %.3f, state %d\n", act, t->events.size(), occ, t->state);
            CHECK(act == 0 && t->events.empty() && t->state == 0 && t->blocksOk == 0, "noise gave activity (%d, %zu events)", act, t->events.size());
            CHECK(t->track.empty(), "noise gave a Doppler track");
        }
    }
    // a pass played ten times faster: the Doppler track follows the sweep (the average of the interval before each point)
    {
        SynthConfig sc; sc.modeOpt[0] = 0; sc.snrDb = 14; sc.modeVal[0] = 10;
        const GlobalstarTelemetry t = run(sc, rate, 4.0, false);
        int n = 0; double worst = 0, sq = 0;
        for (const GsDopplerPoint& p : t.track) {
            if (p.chan != ch) continue;
            const double want = globalstarGenDoppler((p.timeSec - 0.125) * 10.0);
            const double e = std::fabs(p.hz - want);
            worst = std::max(worst, e); sq += e * e; n++;
        }
        printf("pass: %d points on channel %d, Doppler error rms %.0f Hz, worst %.0f Hz\n", n, ch + 1, n ? std::sqrt(sq / n) : 0.0, worst);
        CHECK(n >= 12, "only %d Doppler points", n);
        CHECK(n && std::sqrt(sq / n) < 2500.0 && worst < 5000.0, "track error rms %.0f worst %.0f", n ? std::sqrt(sq / n) : 0.0, worst);
    }
    // bursts on the second channel at real speed: 300 ms on, 500 ms off
    {
        SynthConfig sc; sc.modeOpt[0] = 0; sc.snrDb = 14;
        const GlobalstarTelemetry t = run(sc, rate, 7.0, false);
        int onB = 0; double dsum = 0;
        for (const GsEvent& e : t.events) if (e.chan != ch) { onB++; dsum += e.durationSec; }
        printf("bursts: %zu events, %d on the second channel, mean duration %.3f s, occupancy of channel %d: %.2f\n", t.events.size(), onB, onB ? dsum / onB : 0.0, ch + 1, t.chan[(size_t)ch].occupancy);
        CHECK(onB >= 6 && onB <= 9, "%d burst events (6 to 8 expected)", onB);
        CHECK(onB && std::fabs(dsum / onB - 0.3) < 0.08, "burst duration %.3f", onB ? dsum / onB : 0.0);
        CHECK(t.chan[(size_t)ch].occupancy > 0.05, "occupancy %.3f", t.chan[(size_t)ch].occupancy);
    }
    // the summary and tuning
    CHECK(globalstarTuning().stdMode == 64 && modeTuningById("globalstar") && modeTuningById("globalstar")->stdMode == 64, "tuning table");
    printf(fails ? "FAILED: %d\n" : "globalstar rx ok\n", fails);
    return fails ? 1 : 0;
}
