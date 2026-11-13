// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS / Packet receiver on the test signal, fed directly: one full cycle of the generator with noise and a carrier offset, every frame must come
// out with exactly the contents that were sent. 1200 baud at 12 dB carrier to noise (in 25 kHz), 9600 baud at 18 dB, then both together.
#include "dect2/dsp/gen_util.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/packet/packet_ax25.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static bool same(const PacketFrameInfo& r, const PacketGenFrame& g) {
    return r.from == g.frame.from.str() && r.to == g.frame.to.str() && r.path == g.frame.pathStr() && r.info == g.frame.info && r.baud == g.baud;
}

static void run(const char* name, int mode, double snrDb, double cfoHz, double rate) {
    const std::vector<PacketGenFrame> want = packetGenCycle(mode);
    SynthConfig cfg;
    cfg.snrDb = snrDb;
    cfg.cfoHz = cfoHz;
    cfg.modeOpt[0] = mode;
    auto syn = makePacketSynth(cfg, rate);
    CHECK(syn != nullptr, "%s: no generator", name);
    if (!syn) return;
    PacketReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-packetTuning().tuneOffsetHz);
    CHECK(rx.ready(), "%s: receiver not ready", name);
    // one cycle, and a little more for the last report; the next cycle's first frame starts 0.6 s after the wrap, so stop before it
    const size_t total = (size_t)((packetGenCycleSec(mode) + 0.35) * rate);
    std::vector<cf32> buf(65536);
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(buf.size(), total - done);
        syn->generate(buf.data(), n);
        rx.feed(buf.data(), n);
        done += n;
    }
    PacketTelemetry t;
    CHECK(rx.telemetry(t, 0), "%s: no telemetry", name);
    int found = 0;
    for (const PacketGenFrame& g : want) {
        int hits = 0;
        for (const PacketFrameInfo& r : t.frames) if (same(r, g)) hits++;
        if (hits == 1) found++;
        else printf("  %s: frame %s sent, %d times decoded\n", name, g.frame.tnc2().c_str(), hits);
    }
    printf("%s (%.0f dB, %+.0f Hz): %d of %zu frames, %zu decoded in all, bad 1200/9600: %llu/%llu, stations %zu, snr %.1f dB, state %d\n", name, snrDb, cfoHz, found, want.size(),
           t.frames.size(), (unsigned long long)t.bad1200, (unsigned long long)t.bad9600, t.stations.size(), t.snrDb, t.state);
    CHECK(found == (int)want.size(), "%s: %d of %zu frames", name, found, want.size());
    CHECK(t.frames.size() == want.size(), "%s: %zu frames decoded, %zu sent", name, t.frames.size(), want.size());
    CHECK(t.blocksBad == 0, "%s: %llu failed frames counted", name, (unsigned long long)t.blocksBad);
    CHECK(t.blocksOk == want.size(), "%s: counter %llu", name, (unsigned long long)t.blocksOk);
    CHECK(t.stations.size() >= (mode == 2 ? 3u : 5u), "%s: %zu stations", name, t.stations.size());
}

int main() {
    {   // "Listen": 8000 audio samples a second when on, none when off, the same frames either way
        size_t heard[2] = {0, 0}; PacketTelemetry t[2];
        for (int on = 0; on < 2; on++) {
            SynthConfig cfg; cfg.snrDb = 15; cfg.modeOpt[0] = 1;
            auto syn = makePacketSynth(cfg, 2000000);
            PacketReceiver rx;
            rx.configure(2000000); rx.setSignalOffset(-packetTuning().tuneOffsetHz);
            rx.setSilent(true); rx.setListen(on != 0);
            rx.setListenTap([&heard, on](const float*, size_t n) { heard[on] += n; });
            std::vector<cf32> buf(65536);
            for (size_t done = 0; done < 6 * 2000000; done += buf.size()) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
            rx.telemetry(t[on], 0);
        }
        CHECK(heard[0] == 0 && std::fabs((double)heard[1] - 6 * 8000) < 800, "packet listen: off %zu, on %zu samples", heard[0], heard[1]);
        CHECK(t[0].blocksOk == t[1].blocksOk, "packet listen changes decoding: %llu vs %llu", (unsigned long long)t[0].blocksOk, (unsigned long long)t[1].blocksOk);
    }
    run("1200 baud", 1, 12.0, 700.0, 2000000);
    run("9600 baud", 2, 18.0, -900.0, 2000000);
    run("both", 0, 18.0, 300.0, 2000000);
    run("1200 baud, 10 Msps", 1, 15.0, 0.0, 10000000);
    {   // two 1200 baud frames with no inter-packet gap (dense APRS channel)
        PacketGenFrame f0, f1;
        f0.baud = f1.baud = 1200;
        f0.frame.from.call = "A61AA"; f0.frame.to.call = "APRS"; f0.frame.info = "first";
        f1.frame.from.call = "A61BB"; f1.frame.to.call = "APRS"; f1.frame.info = "second";
        SynthConfig cfg;
        cfg.snrDb = 16.0;
        cfg.cfoHz = 400.0;
        auto syn = makePacketSynthBurst(cfg, 2000000, {f0, f1}, 0.0);
        CHECK(syn != nullptr, "burst synth");
        PacketReceiver rx;
        rx.configure(2000000);
        rx.setSignalOffset(-packetTuning().tuneOffsetHz);
        std::vector<cf32> buf(65536);
        const size_t total = (size_t)(2.0 * 2000000);
        for (size_t done = 0; done < total;) {
            const size_t n = std::min(buf.size(), total - done);
            syn->generate(buf.data(), n);
            rx.feed(buf.data(), n);
            done += n;
        }
        PacketTelemetry t;
        CHECK(rx.telemetry(t, 0), "back-to-back: telemetry");
        int hit0 = 0, hit1 = 0;
        for (const PacketFrameInfo& r : t.frames) {
            if (r.from == f0.frame.from.str() && r.info == f0.frame.info) hit0++;
            if (r.from == f1.frame.from.str() && r.info == f1.frame.info) hit1++;
        }
        printf("back-to-back 1200: %zu decoded, hits %d/%d, bad %llu\n", t.frames.size(), hit0, hit1, (unsigned long long)t.blocksBad);
        CHECK(hit0 == 1 && hit1 == 1, "back-to-back: %d and %d", hit0, hit1);
        CHECK(t.blocksBad == 0, "back-to-back bad %llu", (unsigned long long)t.blocksBad);
    }
    // nothing but noise for 6 s: no frame, no failure counted
    {
        PacketReceiver rx;
        rx.configure(2000000);
        rx.setSignalOffset(-packetTuning().tuneOffsetHz);
        genutil::NoiseSource ns(5);
        std::vector<cf32> buf(65536);
        for (int i = 0; i < 183; i++) {
            for (auto& v : buf) v = cf32(0.f, 0.f);
            ns.add(buf.data(), buf.size(), 0.1f);
            rx.feed(buf.data(), buf.size());
        }
        PacketTelemetry t;
        CHECK(rx.telemetry(t, 0), "noise: no telemetry");
        printf("noise only: %llu ok, %llu bad, state %d\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.state);
        CHECK(t.blocksOk == 0 && t.blocksBad == 0 && t.state == 0, "noise: ok %llu bad %llu state %d", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.state);
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
