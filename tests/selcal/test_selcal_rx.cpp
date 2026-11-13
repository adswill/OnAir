// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SELCAL receiver on the built-in test signal (voice, AM-CP, DH-KS, AM-CP in 22 s) as an FM channel at a radio's rate, with a frequency offset and noise,
// and a second case in the engine's own mode (own log, no other tone decoders).
#include "dect2/selcal/selcal_rx.h"
#include "dect2/selcal/selcal_tel.h"
#include "dect2/tones/tones_gen.h"
#include "dect2/tones/tones_rx.h"
#include <cstdio>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static SelcalTelemetry run(double rate, double cfo, double snr, double dial, const std::string& table) {
    SynthConfig cfg; cfg.mode = 241; cfg.snrDb = snr; cfg.cfoHz = cfo;
    auto syn = makeSelcalSynth(cfg, rate);
    SelcalReceiver rx;
    rx.setSilent(true); rx.setDemod(TonesFm);
    rx.configure(rate); rx.setSignalOffset(-selcalTuning().tuneOffsetHz); rx.setDialHz(dial);
    if (!table.empty()) rx.setAircraftTable(table);
    SelcalTelemetry t, last; uint64_t seq = 0;
    std::vector<cf32> b(65536);
    for (size_t done = 0; done < (size_t)(24 * rate); done += b.size()) {
        syn->generate(b.data(), b.size());
        rx.feed(b.data(), b.size());
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    return last;
}

int main() {
    for (double snr : {40.0, 25.0}) for (double cfo : {0.0, 300.0}) {
        const SelcalTelemetry t = run(2e6, cfo, snr, 8.891e6, "AMCP\tN123AB\n# note\nDH-KS\tD-ABCD\n");
        int amcp = 0, dhks = 0;
        for (const auto& c : t.calls) { if (c.code == "AM-CP") { amcp++; CHECK(c.aircraft == "N123AB" && c.dialHz == 8.891e6 && c.hz[0] > 300 && c.hz[3] < 1500, "fields of %s", c.code.c_str()); } if (c.code == "DH-KS") { dhks++; CHECK(c.aircraft == "D-ABCD", "aircraft"); } }
        printf("snr %.0f cfo %.0f: %zu calls, AM-CP %d, DH-KS %d, %zu codes\n", snr, cfo, t.calls.size(), amcp, dhks, t.codes.size());
        CHECK(amcp == 2 && dhks == 1 && t.calls.size() == 3 && t.codes.size() == 2 && t.codes[0].code == "AM-CP", "calls at snr %.0f cfo %.0f", snr, cfo);
    }
    double hz[4];
    CHECK(selcalCodeTones("AM-CP", hz) && hz[0] > 312 && !selcalCodeTones("AM-C", hz) && !selcalCodeTones("AI-CP", hz), "code to tones");
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
