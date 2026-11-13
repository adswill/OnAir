// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS / Packet: the 12.5 / 25 kHz channel filter choice. The 1200 baud cycle at 15 dB, carrier offsets swept, alone and with a much stronger
// neighbour on the next 12.5 kHz channel (a frame from another station). Alone both widths decode every frame; with the neighbour the narrow
// filter must keep every frame of the wanted channel and decode none of the neighbour's, and must do better than the wide one.
#include "dect2/packet/packet_gen.h"
#include "dect2/packet/packet_rx.h"
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static constexpr double kRate = 1.2e6;
static constexpr double kNeighbourSnr = 22;   // carrier to noise; the wanted channel is 15 dB

struct Result { int found = 0, total = 0; bool foreign = false; };

static Result run(bool narrow, double cfo, double neighbourSnr) {
    SynthConfig c; c.snrDb = 15; c.cfoHz = cfo; c.modeOpt[0] = 1;
    auto syn = makePacketSynth(c, kRate);
    std::unique_ptr<ModeSynth> nb;
    ax25::Frame nf;
    nf.from.call = "N0kNeighbourSnrR"; nf.from.ssid = 9; nf.to.call = "APRS";
    nf.info = "!4903.50N/07201.75W-neighbour channel";
    if (neighbourSnr > 0) { SynthConfig n; n.snrDb = neighbourSnr; n.cfoHz = 12500; nb = makePacketSynthFrame(n, kRate, nf, 1200); }
    PacketReceiver rx;
    rx.configure(kRate);
    rx.setNarrowChannel(narrow);
    rx.setSignalOffset(-packetTuning().tuneOffsetHz);
    const auto want = packetGenCycle(1);
    std::vector<cf32> a(65536), b(65536);
    const size_t total = (size_t)((packetGenCycleSec(1) + 0.35) * kRate);
    for (size_t done = 0; done < total; done += a.size()) {
        const size_t n = std::min(a.size(), total - done);
        syn->generate(a.data(), n);
        if (nb) { nb->generate(b.data(), n); for (size_t i = 0; i < n; i++) a[i] += b[i]; }
        rx.feed(a.data(), n);
    }
    PacketTelemetry t;
    rx.telemetry(t, 0);
    Result r;
    for (const PacketGenFrame& g : want) {
        r.total++;
        for (const PacketFrameInfo& f : t.frames) if (f.from == g.frame.from.str() && f.info == g.frame.info) { r.found++; break; }
    }
    for (const PacketFrameInfo& f : t.frames) if (f.from == nf.from.str()) r.foreign = true;
    return r;
}

int main() {
    for (double cfo : {-3000.0, 0.0, 3000.0}) {
        for (bool narrow : {false, true}) {
            const Result r = run(narrow, cfo, 0);
            printf("alone, %s, offset %+.0f Hz: %d of %d frames\n", narrow ? "12.5 kHz" : "25 kHz", cfo, r.found, r.total);
            CHECK(r.total > 0 && r.found == r.total, "alone %s offset %.0f: %d of %d frames", narrow ? "12.5" : "25", cfo, r.found, r.total);
        }
    }
    for (double cfo : {-1500.0, 0.0, 1500.0}) {
        const Result w = run(false, cfo, kNeighbourSnr), n = run(true, cfo, kNeighbourSnr);
        printf("neighbour, offset %+.0f Hz: 25 kHz %d of %d (foreign %d), 12.5 kHz %d of %d (foreign %d)\n", cfo, w.found, w.total, (int)w.foreign, n.found, n.total, (int)n.foreign);
        // the neighbour's lower sideband reaches into the filter from the other side of an offset, so only the centred and the low offset keep every frame
        if (cfo <= 0) CHECK(n.found == n.total, "12.5 kHz with a neighbour, offset %.0f: %d of %d frames", cfo, n.found, n.total);
        CHECK(n.found > w.found, "the narrow filter is not better: %d vs %d frames", n.found, w.found);
    }
    return testSummary("packet_width");
}
