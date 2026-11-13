// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 phase 1 sensitivity: the test signal (carrier to noise in 12.5 kHz) at 7 dB decodes nine blocks in ten, also 3 kHz off tune; the
// receiver with the +-8.5 kHz channel filter and hard Hamming / Golay decoding got half at 7 dB. Noise alone gives nothing.
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "../common/test_util.h"
#include "../common/voice_sens.h"
using namespace dect2;

int main() {
    const double off = p25Tuning().tuneOffsetHz;
    const P25Telemetry ref = voicesens::run<P25Receiver, P25Telemetry>(52, 250000, off, 20, 30);
    const P25Telemetry a = voicesens::run<P25Receiver, P25Telemetry>(52, 250000, off, 20, 7);
    const P25Telemetry b = voicesens::run<P25Receiver, P25Telemetry>(52, 250000, off, 20, 8, 3000);
    const P25Telemetry c = voicesens::run<P25Receiver, P25Telemetry>(52, 250000, off, 20, 8, -3000);
    const P25Telemetry n = voicesens::run<P25Receiver, P25Telemetry>(52, 250000, off, 30, 30, 0, 0, true);
    printf("p25 sens: 30 dB %llu blocks, 7 dB %llu, 8 dB +3 kHz %llu, -3 kHz %llu, noise %llu\n", (unsigned long long)ref.blocksOk, (unsigned long long)a.blocksOk,
           (unsigned long long)b.blocksOk, (unsigned long long)c.blocksOk, (unsigned long long)n.blocksOk);
    CHECK(ref.blocksOk >= 300, "30 dB: %llu blocks", (unsigned long long)ref.blocksOk);
    CHECK(a.blocksOk * 100 >= ref.blocksOk * 80, "7 dB: %llu of %llu blocks", (unsigned long long)a.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(b.blocksOk * 100 >= ref.blocksOk * 75 && c.blocksOk * 100 >= ref.blocksOk * 75, "8 dB, +-3 kHz: %llu and %llu of %llu blocks",
          (unsigned long long)b.blocksOk, (unsigned long long)c.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(n.blocksOk == 0 && n.log.empty(), "noise: %llu blocks", (unsigned long long)n.blocksOk);
    // a carrier 4 kHz off the channel (a cheap oscillator or a mistuned dial) is captured by the frequency capture (MeanAfc): before it, the narrow filters cut the signal
    for (double cfo : {4000.0, -4000.0}) {
        const P25Telemetry o = voicesens::run<P25Receiver, P25Telemetry>(52, 250000, off, 20, 14, cfo);
        CHECK(o.blocksOk * 100 >= ref.blocksOk * 70, "%+.0f Hz off, 14 dB: %llu of %llu", cfo, (unsigned long long)o.blocksOk, (unsigned long long)ref.blocksOk);
    }
    return testSummary("p25 sens");
}
