// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Whole-span APRS / packet: three channels in one 2 Msps capture, each with its own station (1200 baud on the raster, a weak 9600 baud one off the raster,
// a 1200 baud one that starts late). Every channel needs its own receiver; each station's frame must come back with the frequency of its own channel
// and no other; the stations table lists all three; a retune keeps the frequencies; and the single receiver (switch off) is unchanged.
#include "dect2/engine/modes.h"
#include "dect2/packet/packet_ax25.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/t2/t2rx.h"
#include "dect2/engine/wide_modes.h"
#include <cmath>
#include <cstdio>
#include <memory>
using namespace dect2;
#include "../common/test_util.h"

static const double kFs = 2.0e6;

static ax25::Frame frameOf(const char* from, const char* info) {
    ax25::Frame f;
    f.from.call = from; f.to.call = "APRS"; f.info = info;
    return f;
}

int main() {
    const double off = -packetTuning().tuneOffsetHz;
    struct Car { const char* call; const char* info; int baud; double hz, amp, startSec; };
    const Car car[3] = {{"N0AAA", ">alpha station", 1200, off - 125000, 1.0, 0},
                        {"N0BBB", ">bravo station", 9600, off + 75000 + 1500, 0.5, 0},
                        {"N0CCC", ">charlie station", 1200, off + 200000 - 1000, 0.8, 4.0}};
    std::unique_ptr<ModeSynth> g[3];
    for (int k = 0; k < 3; k++) {
        SynthConfig c; c.snrDb = 30; c.cfoHz = car[k].hz - off; c.modeOpt[3] = 21 + k;
        g[k] = makePacketSynthFrame(c, kFs, frameOf(car[k].call, car[k].info), car[k].baud);
        if (!g[k]) { printf("FAIL: no generator\n"); return 1; }
    }
    WideModes wm;
    wm.configure(25, kFs, off);
    wm.setCenterHz(144.0e6);
    RxTelemetry t; uint64_t seq = 0;
    const size_t blk = (size_t)(kFs * 0.1);
    std::vector<cf32> sum(blk), tmp(blk);
    for (double s = 0; s < 30.0; s += 0.1) {
        std::fill(sum.begin(), sum.end(), cf32(0, 0));
        for (int k = 0; k < 3; k++) {
            g[k]->generate(tmp.data(), blk);
            if (s < car[k].startSec) continue;
            for (size_t i = 0; i < blk; i++) sum[i] += tmp[i] * (float)car[k].amp;
        }
        wm.feed(25, sum.data(), blk);
        wm.telemetry(25, t, seq);
    }
    printf("%zu rows, %zu frames, %zu stations\n", t.wide.size(), t.packet.frames.size(), t.packet.stations.size());
    for (auto& r : t.wide) printf("  %+9.0f Hz  level %.1f  snr %.1f  state %d  %s\n", r.hz, r.levelDb, r.snrDb, r.state, r.info.c_str());
    CHECK(t.wide.size() == 3, "%zu carriers, expected 3", t.wide.size());
    for (int k = 0; k < 3; k++) {
        int n = 0, wrong = 0;
        for (const auto& f : t.packet.frames) {
            if (f.from == car[k].call) { n++; if (f.carrierId <= 0 || std::fabs(f.carrierHz - car[k].hz) > 2000) wrong++; }
            else if (f.carrierId > 0 && std::fabs(f.carrierHz - car[k].hz) < 2000) wrong++;   // somebody else's frame on this channel
        }
        printf("  %s: %d frames, %d at a wrong frequency\n", car[k].call, n, wrong);
        CHECK(n > 0, "%s: no frame", car[k].call);
        CHECK(wrong == 0, "%s: %d frames at a wrong frequency", car[k].call, wrong);
        bool st = false;
        for (const auto& s : t.packet.stations) if (s.call == car[k].call) st = s.carrierId > 0 && std::fabs(s.carrierHz - car[k].hz) < 2000;
        CHECK(st, "%s: not in the stations table at its frequency", car[k].call);
    }
    // retune by +500 kHz: the same stations at the same absolute frequencies
    wm.setCenterHz(144.5e6);
    for (int i = 0; i < 5; i++) { std::fill(sum.begin(), sum.end(), cf32(0, 0)); wm.feed(25, sum.data(), blk); }
    wm.telemetry(25, t, seq);
    int ok = 0;
    for (const auto& f : t.packet.frames) for (int k = 0; k < 3; k++) if (f.from == car[k].call && std::fabs(f.carrierHz - (car[k].hz - 500000.0)) < 2000) { ok++; break; }
    CHECK(ok == (int)t.packet.frames.size() && ok > 0, "%d of %zu frames kept their absolute frequency over the retune", ok, t.packet.frames.size());
    {   // the single receiver, no whole span: all frames of its channel, no stamp
        auto syn = makePacketSynthFrame([] { SynthConfig c; c.snrDb = 25; return c; }(), kFs, frameOf("N0ZZZ", ">solo"), 1200);
        PacketReceiver rx; rx.configure(kFs); rx.setSilent(true); rx.setSignalOffset(off);
        std::vector<cf32> b(65536);
        for (double d = 0; d < 12.0 * kFs; d += (double)b.size()) { syn->generate(b.data(), b.size()); rx.feed(b.data(), b.size()); }
        PacketTelemetry pt; rx.telemetry(pt, 0);
        int n = 0; for (const auto& f : pt.frames) if (f.from == "N0ZZZ" && f.carrierId == 0) n++;
        CHECK(n > 0 && pt.stations.size() == 1, "single receiver: %d frames, %zu stations", n, pt.stations.size());
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
