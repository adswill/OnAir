// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR sensitivity: the test signal (carrier to noise in 6.25 kHz) at 8 dB keeps every voice frame and at 7 dB most, also 2 kHz off
// tune; the receiver with the +-7 kHz channel filter kept 311 of 764 at 8 dB and 35 at 7 dB. Noise alone gives nothing.
#include "dect2/dstar/dstar_rx.h"
#include "dect2/dstar/dstar_tel.h"
#include "../common/test_util.h"
#include "../common/voice_sens.h"
using namespace dect2;

int main() {
    const double off = dstarTuning().tuneOffsetHz;
    const DstarTelemetry ref = voicesens::run<DstarReceiver, DstarTelemetry>(35, 250000, off, 20, 30);
    const DstarTelemetry a = voicesens::run<DstarReceiver, DstarTelemetry>(35, 250000, off, 20, 8);
    const DstarTelemetry b = voicesens::run<DstarReceiver, DstarTelemetry>(35, 250000, off, 20, 7);
    const DstarTelemetry c = voicesens::run<DstarReceiver, DstarTelemetry>(35, 250000, off, 20, 9, 2000);
    const DstarTelemetry n = voicesens::run<DstarReceiver, DstarTelemetry>(35, 250000, off, 30, 30, 0, 0, true);
    printf("dstar sens: 30 dB %llu frames, 8 dB %llu, 7 dB %llu, 9 dB +2 kHz %llu, noise %llu\n", (unsigned long long)ref.blocksOk, (unsigned long long)a.blocksOk,
           (unsigned long long)b.blocksOk, (unsigned long long)c.blocksOk, (unsigned long long)n.blocksOk);
    CHECK(ref.blocksOk >= 750, "30 dB: %llu frames", (unsigned long long)ref.blocksOk);
    CHECK(a.blocksOk * 100 >= ref.blocksOk * 90, "8 dB: %llu of %llu frames", (unsigned long long)a.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(b.blocksOk * 100 >= ref.blocksOk * 70, "7 dB: %llu of %llu frames", (unsigned long long)b.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(c.blocksOk * 100 >= ref.blocksOk * 85, "9 dB, +2 kHz: %llu of %llu frames", (unsigned long long)c.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(n.blocksOk == 0 && n.log.empty(), "noise: %llu frames", (unsigned long long)n.blocksOk);
    // a carrier 4 kHz off the channel (a cheap oscillator or a mistuned dial) is captured by the frequency capture (MeanAfc): before it, the narrow filters cut the signal
    for (double cfo : {4000.0, -4000.0}) {
        const DstarTelemetry o = voicesens::run<DstarReceiver, DstarTelemetry>(35, 250000, off, 20, 14, cfo);
        CHECK(o.blocksOk * 100 >= ref.blocksOk * 70, "%+.0f Hz off, 14 dB: %llu of %llu", cfo, (unsigned long long)o.blocksOk, (unsigned long long)ref.blocksOk);
    }
    return testSummary("dstar sens");
}
