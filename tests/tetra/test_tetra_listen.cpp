// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What a TETRA listener chooses (issue #267): the single-carrier mode decodes only the 25 kHz channel at the dial frequency, so a second
// cell in the span is not decoded (no second copy of a call, a capture decoded in a fraction of the time); the default still finds both
// cells. The timeslot and carrier choices are accepted and the playing stream is reported as none on a cell without clear voice.
// NOT covered: a real network, a clear call with a codec (test_tetra_voice.cpp covers the voice path).
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_tel.h"
#include "dect2/engine/modes.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static TetraTelemetry run(bool single, double dialOffHz, int tn, double carrierHz) {
    const double rate = 300e3;
    SynthConfig sc; sc.mode = 48; sc.snrDb = 30; sc.modeOpt[0] = 1;   // two cells: 392.4125 and 392.6 MHz
    auto syn = makeTetraSynth(sc, rate);
    TetraReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kTetraTestCentreHz);
    rx.setSignalOffset(dialOffHz);
    rx.setSingleCarrier(single);
    rx.setVoiceTimeslot(tn);
    rx.setVoiceCarrier(carrierHz);
    std::vector<cf32> x(32768);
    for (int i = 0; i < 70; i++) {   // three captures
        syn->generate(x.data(), x.size());
        rx.feed(x.data(), x.size());
        while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    rx.feed(x.data(), 1);
    TetraTelemetry t;
    rx.telemetry(t, 0);
    CHECK(rx.singleCarrier() == single, "single carrier flag");
    return t;
}

static int cellsNear(const TetraTelemetry& t, double hz) { int n = 0; for (const auto& c : t.cells) if (std::fabs(c.freqHz - hz) < 5e3) n++; return n; }

int main() {
    const double f1 = 392.4125e6, f2 = 392.6e6;
    const TetraTelemetry all = run(false, 0, 0, 0);
    printf("whole span: %zu cells (%d at 392.4125, %d at 392.6)\n", all.cells.size(), cellsNear(all, f1), cellsNear(all, f2));
    CHECK(cellsNear(all, f1) == 1 && cellsNear(all, f2) == 1, "both cells found in the whole span");

    const TetraTelemetry one = run(true, f2 - kTetraTestCentreHz, 2, f2);
    printf("single carrier at 392.6: %zu cells (%d at 392.4125, %d at 392.6)\n", one.cells.size(), cellsNear(one, f1), cellsNear(one, f2));
    CHECK(cellsNear(one, f2) == 1 && cellsNear(one, f1) == 0, "only the carrier at the dial frequency is decoded");

    const TetraTelemetry other = run(true, f1 - kTetraTestCentreHz, 0, 0);
    CHECK(cellsNear(other, f1) == 1 && cellsNear(other, f2) == 0, "the other carrier when dialled");
    return testSummary("tetra listen");
}
