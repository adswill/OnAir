// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Temporary diagnostic (removed before merge): the 802.11b chain at several radio rates and clock errors.
#include "dect2/espnow/espnow_proto.h"
#include "dect2/wifi/wifi_gen.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
#include <cstdio>
#include <vector>
using namespace dect2;

int main() {
    for (double rate : {20e6, 24e6, 25e6, 26e6, 30.72e6})
        for (double ppm : {0.0, 1.0, 10.0, -10.0})
            for (double cfo : {0.0, 20e3}) {
                WifiGenConfig c; c.rate = rate; c.clockPpm = ppm; c.cfoHz = cfo; c.snrDb = 99;
                espnow::Build b; b.body.assign(20, 0x5A);
                WifiGenFrame f; f.mpdu = espnow::makeFrame(b); f.gapSec = 0.001;
                c.frames = {f};
                WifiGenerator g(c);
                WifiReceiver r; r.configure(rate); r.setReportInterval(0.01);
                int bad = 0, ok = 0;
                r.setFrameMetaCallback([&](const uint8_t*, size_t, const WifiFrameMeta& m) { (m.fcsOk ? ok : bad)++; });
                std::vector<cf32> buf(16384);
                for (size_t d = 0; d < (size_t)(0.1 * rate); d += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
                WifiTelemetry t; r.telemetry(t, 0);
                printf("DIAG rate %.2f ppm %+.0f cfo %.0f: sfd %llu hdr ok %llu bad %llu other %llu, frames ok %d bad %d, snr %.1f\n", rate / 1e6, ppm, cfo,
                       (unsigned long long)t.sfdHits, (unsigned long long)t.headersOk, (unsigned long long)t.headersBad, (unsigned long long)t.otherRate, ok, bad, t.snrDb);
            }
    return 0;
}
