// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR sensitivity: the test signal at 9 dB (carrier to noise in 6.25 kHz) and 8 dB with a 1 kHz offset decodes almost every block; the
// receiver before the low-pass symbol filter and the soft Hamming decoding got nothing at 9 dB. Noise alone gives no block.
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dpmr/dpmr_tel.h"
#include "../common/test_util.h"
#include "../common/voice_sens.h"
using namespace dect2;

int main() {
    const double off = dpmrTuning().tuneOffsetHz;
    const DpmrTelemetry ref = voicesens::run<DpmrReceiver, DpmrTelemetry>(47, 250000, off, 20, 30);
    const DpmrTelemetry a = voicesens::run<DpmrReceiver, DpmrTelemetry>(47, 250000, off, 20, 9);
    const DpmrTelemetry b = voicesens::run<DpmrReceiver, DpmrTelemetry>(47, 250000, off, 20, 8, 1000);
    const DpmrTelemetry n = voicesens::run<DpmrReceiver, DpmrTelemetry>(47, 250000, off, 30, 30, 0, 0, true);
    printf("dpmr sens: 30 dB %llu blocks, 9 dB %llu, 8 dB +1 kHz %llu, noise %llu\n", (unsigned long long)ref.blocksOk, (unsigned long long)a.blocksOk,
           (unsigned long long)b.blocksOk, (unsigned long long)n.blocksOk);
    CHECK(ref.blocksOk >= 140, "30 dB: %llu blocks", (unsigned long long)ref.blocksOk);
    CHECK(a.blocksOk * 100 >= ref.blocksOk * 90, "9 dB: %llu of %llu blocks", (unsigned long long)a.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(b.blocksOk * 100 >= ref.blocksOk * 75, "8 dB, +1 kHz: %llu of %llu blocks", (unsigned long long)b.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(n.blocksOk == 0, "noise: %llu blocks", (unsigned long long)n.blocksOk);
    // a carrier 4 kHz off the channel (a cheap oscillator or a mistuned dial) is captured by the frequency capture (MeanAfc): before it, the narrow filters cut the signal
    for (double cfo : {4000.0, -4000.0}) {
        const DpmrTelemetry o = voicesens::run<DpmrReceiver, DpmrTelemetry>(47, 250000, off, 20, 14, cfo);
        CHECK(o.blocksOk * 100 >= ref.blocksOk * 70, "%+.0f Hz off, 14 dB: %llu of %llu", cfo, (unsigned long long)o.blocksOk, (unsigned long long)ref.blocksOk);
    }
    return testSummary("dpmr sens");
}
