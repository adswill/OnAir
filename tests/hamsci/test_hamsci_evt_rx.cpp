// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The six event monitors through HamsciReceiver on the mode's own test signal (hamsci_gen.h): tuning, kind and family numbers, telemetry (events as lines,
// spectrogram rows, trend), the CSV exports and the summary line. Simulated only.
#include "dect2/hamsci/hamsci_gen.h"
#include "dect2/hamsci/hamsci_rx.h"
#include "hamsci_testutil.h"
using namespace dect2;

static HamsciTelemetry run(int kind, double rate, double seconds, HamsciReceiver& rx) {
    SynthConfig sc;
    auto syn = makeHamsciSynth(sc, rate, kind);
    rx.configure(rate);
    HamsciTelemetry t, last;
    uint64_t seq = 0;
    std::vector<cf32> buf(65536);
    const size_t total = (size_t)(seconds * rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t m = std::min(buf.size(), total - done);
        syn->generate(buf.data(), m);
        rx.feed(buf.data(), m);
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    return last;
}

int main() {
    // numbers: families 820, 823 .. 835, engine standard family + 2, active standard family + 1
    for (int k = kHsSchumann; k < kHamsciKinds; k++) {
        const int fam = 820 + 3 * (k - kHsSchumann);
        CHECK(hamsciFamily(k) == fam && hamsciKindOfFamily(fam) == k && hamsciKindOfStd(fam + 2) == k && hamsciKindOfActive(fam + 1) == k, "numbers of kind %d", k);
        CHECK(hamsciKindOfFamily(fam + 1) == -1 && hamsciKindOfFamily(fam + 2) == -1, "family + 1, + 2 are not families (%d)", fam);
        const ModeTuning mt = hamsciTuning(k);
        CHECK(mt.stdMode == fam + 2 && std::string(mt.id) == hamsciId(k) && mt.minSampleRate > 0, "tuning of kind %d", k);
        CHECK(hamsciEvtKind(k) && !hamsciAudioKind(k), "kind %d is an event monitor", k);
    }
    CHECK(hamsciKindOfFamily(750) == kHsJt6m && hamsciKindOfFamily(502) == kHsPulsar, "older families unchanged");
    {   // Whistlers and tweeks, 48 kHz, the synth loops 60 s
        HamsciReceiver rx(kHsWhistler);
        auto t = run(kHsWhistler, 48000, 58, rx);
        std::printf("whistler synth: %zu lines, %d events, rows %zu, summary '%s'\n", t.lines.size(), t.evt.eventsTotal, t.evt.rows.size(), hamsciSummary(t).c_str());
        CHECK(t.evt.eventsTotal >= 3 && !t.lines.empty() && !t.evt.rows.empty() && t.state >= 3, "whistler events %d", t.evt.eventsTotal);
        const std::string csv = rx.evtExportCsv();
        CHECK(csv.find("whistler") != std::string::npos && csv.find("tweek") != std::string::npos, "CSV has both kinds");
        CHECK(rx.evtExportTrendCsv().find("utc,") == 0, "trend CSV");
        rx.clearResults();
    }
    {   // Schumann from a 48 kHz ELF input: the synth holds 200 Hz samples
        HamsciReceiver rx(kHsSchumann);
        auto t = run(kHsSchumann, 48000, 100, rx);
        std::printf("schumann synth: f1 %.2f Hz, summary '%s'\n", t.evt.peakHz.empty() ? 0.0 : (double)t.evt.peakHz[0], hamsciSummary(t).c_str());
        CHECK(!t.evt.peakHz.empty() && t.evt.peakHz[0] > 6.8 && t.evt.peakHz[0] < 8.9, "schumann fundamental");
        CHECK(t.evt.avgSpec.size() > 100 && !t.evt.rows.empty(), "schumann spectra");
    }
    {   // Moon echo: the synth keys 1.5 s pulses at 2 s and 9 s with an echo 2.55 s later
        HamsciReceiver rx(kHsEme);
        auto t = run(kHsEme, 250000, 30, rx);
        std::printf("eme synth: %d echoes, %d pulses, summary '%s'\n", t.evt.eventsTotal, t.evt.txPulses, hamsciSummary(t).c_str());
        CHECK(t.evt.txPulses >= 1, "own pulses seen %d", t.evt.txPulses);
    }
    {   // Radio Jove: broadband bursts
        HamsciReceiver rx(kHsJove);
        auto t = run(kHsJove, 250000, 31, rx);
        std::printf("jove synth: %d bursts, summary '%s'\n", t.evt.eventsTotal, hamsciSummary(t).c_str());
        CHECK(t.evt.eventsTotal >= 1 && !t.evt.rows.empty(), "jove bursts %d", t.evt.eventsTotal);
    }
    {   // CALLISTO style
        HamsciReceiver rx(kHsCallisto);
        auto t = run(kHsCallisto, 250000, 31, rx);
        std::printf("callisto synth: %d bursts, summary '%s'\n", t.evt.eventsTotal, hamsciSummary(t).c_str());
        CHECK(t.evt.eventsTotal >= 1, "callisto bursts %d", t.evt.eventsTotal);
    }
    {   // beacon openings
        HamsciReceiver rx(kHsAurora);
        auto t = run(kHsAurora, 250000, 31, rx);
        std::printf("beacon synth: %d events, now '%s'\n", t.evt.eventsTotal, t.found.c_str());
        CHECK(!t.evt.rows.empty() && !t.evt.info.empty(), "beacon spectrogram and status");
    }
    std::printf(g_fails ? "hamsci event monitors through the receiver: %d FAILED\n" : "hamsci event monitors through the receiver: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
