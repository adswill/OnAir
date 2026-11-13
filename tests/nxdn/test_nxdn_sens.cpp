// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NXDN sensitivity: the test signal (NXDN48 and NXDN96, carrier to noise in 12.5 kHz) at 7 dB decodes nine blocks in ten, at 5 dB about
// half; the receiver before the low-pass symbol filter got 41 of 247 at 7 dB and none at 5 dB. Noise alone gives at most one block (the
// 6-bit SACCH CRC passes noise now and then; nothing reaches the call log from it).
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "../common/test_util.h"
#include "../common/voice_sens.h"
using namespace dect2;

int main() {
    const double off = nxdnTuning().tuneOffsetHz;
    const NxdnTelemetry ref = voicesens::run<NxdnReceiver, NxdnTelemetry>(40, 250000, off, 20, 30);
    const NxdnTelemetry a = voicesens::run<NxdnReceiver, NxdnTelemetry>(40, 250000, off, 20, 7);
    const NxdnTelemetry b = voicesens::run<NxdnReceiver, NxdnTelemetry>(40, 250000, off, 20, 5);
    const NxdnTelemetry n = voicesens::run<NxdnReceiver, NxdnTelemetry>(40, 250000, off, 30, 30, 0, 0, true);
    printf("nxdn sens: 30 dB %llu blocks, 7 dB %llu, 5 dB %llu, noise %llu\n", (unsigned long long)ref.blocksOk, (unsigned long long)a.blocksOk,
           (unsigned long long)b.blocksOk, (unsigned long long)n.blocksOk);
    CHECK(ref.blocksOk >= 240, "30 dB: %llu blocks", (unsigned long long)ref.blocksOk);
    CHECK(a.blocksOk * 100 >= ref.blocksOk * 85, "7 dB: %llu of %llu blocks", (unsigned long long)a.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(b.blocksOk * 100 >= ref.blocksOk * 40, "5 dB: %llu of %llu blocks", (unsigned long long)b.blocksOk, (unsigned long long)ref.blocksOk);
    CHECK(n.blocksOk <= 1 && n.log.empty(), "noise: %llu blocks, %zu calls", (unsigned long long)n.blocksOk, n.log.size());
    // a carrier 4 kHz off the channel (a cheap oscillator or a mistuned dial) is captured by the frequency capture (MeanAfc): before it, the narrow filters cut the signal
    for (double cfo : {4000.0, -4000.0}) {
        const NxdnTelemetry o = voicesens::run<NxdnReceiver, NxdnTelemetry>(40, 250000, off, 20, 14, cfo);
        CHECK(o.blocksOk * 100 >= ref.blocksOk * 70, "%+.0f Hz off, 14 dB: %llu of %llu", cfo, (unsigned long long)o.blocksOk, (unsigned long long)ref.blocksOk);
    }
    return testSummary("nxdn sens");
}
