// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISDB-T on 7 and 8 MHz channels (ABNT NBR 15601, ISDB-T International: the Maldives): the same signal with every time quantity scaled by
// the channel width / 6. The built-in test signal (as the app makes it) at each width, received with the width set and on automatic, also
// 20 kHz off the centre: the TMCC must be found, the transport stream must flow and carry its PAT.
#include "dect2/isdbt/isdbt.h"
#include "dect2/isdbt/isdbt_rx.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/source/source.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};

static void check(bool c, const char* name, const char* what) { if (!c) { jprintf("FAIL: %s: %s\n", name, what); fails++; } }

static void runCase(const char* name, int txBw, int rxBw, double cfoHz, double seconds = 6.0) {
    const double rate = 10e6;
    SynthConfig sc; sc.snrDb = 28; sc.cfoHz = cfoHz; sc.isdbtBwMhz = txBw;
    auto syn = makeModeSynth(6, sc, rate);
    IsdbtReceiver rx;
    rx.configure(rate, rxBw);
    long good = 0, bad = 0, pat = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = p + i * 188;
            if (q[0] != 0x47 || (q[1] & 0x80)) { bad++; continue; }
            good++;
            const int pid = ((q[1] & 0x1f) << 8) | q[2];
            if (pid == 0 && (q[1] & 0x40) && q[4 + q[4] + 1] == 0x00) pat++;   // payload start, pointer field, table_id 0
        }
    });
    std::vector<cf32> buf(50000);
    const size_t total = (size_t)(seconds * rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    RxTelemetry t;
    rx.telemetry(t, 0);
    jprintf("  %s: tmcc %d, width %d MHz (rate %.4f Msps), mode %d, CFO %.0f Hz, packets good %ld bad %ld, PAT %ld\n", name, (int)t.isdbt.tmccOk, t.isdbt.bwMhz,
            t.nativeRate / 1e6, t.isdbt.mode, t.cfoHz, good, bad, pat);
    check(t.isdbt.tmccOk, name, "TMCC decoded");
    check(t.isdbt.bwMhz == txBw && rx.channelWidth() == txBw, name, "channel width");
    check(std::fabs(t.nativeRate - isdbt::sampleRateFor(txBw)) < 1, name, "IFFT rate");
    check(std::fabs(t.cfoHz - cfoHz) < 500, name, "carrier offset");
    check(t.isdbt.mode == 3 && t.isdbt.partial && t.isdbt.layer[1].segments == 12, name, "parameters from the TMCC");
    check(good > 2000 && bad < good / 20, name, "transport stream flowing");
    check(pat >= 3, name, "PAT received");
}

int main() {
    // the scaling itself: 8 MHz is 10.836 Msps, 7 MHz 9.4815 Msps; a mode 3 frame with GI 1/8 lasts 6/8 and 6/7 of the 6 MHz one
    check(std::fabs(isdbt::sampleRateFor(8) - 10.8360e6) < 100, "rates", "8 MHz IFFT rate");
    check(std::fabs(isdbt::sampleRateFor(7) - 9.4815e6) < 100, "rates", "7 MHz IFFT rate");
    check(std::fabs(isdbt::frameSeconds(3, isdbt::kGi8, 8) / isdbt::frameSeconds(3, isdbt::kGi8) - 0.75) < 1e-9, "rates", "8 MHz frame length");
    testjobs::Jobs jobs;
    int caseNo = 0;
    auto add = [&](std::function<void()> f) { const char* o = getenv("ISDBT_CASE"); if (!o || atoi(o) == caseNo) jobs.add(std::move(f)); caseNo++; };
    add([] { runCase("8 MHz, set", 8, 8, 0); });
    add([] { runCase("7 MHz, set", 7, 7, 0); });
    add([] { runCase("8 MHz, automatic", 8, 0, 0); });
    add([] { runCase("7 MHz, automatic", 7, 0, 0); });
    add([] { runCase("6 MHz, automatic", 6, 0, 0); });
    add([] { runCase("8 MHz, automatic, +20 kHz", 8, 0, 20e3); });
    add([] { runCase("7 MHz, set, +20 kHz", 7, 7, 20e3); });
    jobs.run();
    jprintf(fails ? "isdbt bandwidth: FAILED\n" : "isdbt bandwidth: ok\n");
    return fails ? 1 : 0;
}
