// APRS / Packet under real-radio faults (REAL_WORLD_CHECKLIST.md): one generator cycle (1200 and 9600 baud) through tests/impair.h, then the
// real receiver. A radio 50 ppm off at 144 MHz is 7.2 kHz out; at 222 MHz 11 kHz. Every frame of the cycle must arrive.
#include "dect2/packet_gen.h"
#include "dect2/packet_rx.h"
#include "impair.h"
#include <cstdio>
#include <functional>
using namespace dect2;
using V = std::vector<cf32>;
static int fails = 0;

static PacketTelemetry run(const V& x, double rate) {
    PacketReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-packetTuning().tuneOffsetHz);
    PacketTelemetry t; uint64_t seq = 0;
    auto take = [&] { PacketTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; } };
    for (size_t i = 0; i < x.size(); i += 65536) { rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i)); take(); }
    V z(65536);
    for (int k = 0; k < 8; k++) { rx.feed(z.data(), z.size()); take(); }
    return t;
}

int main() {
    const double rate = 1.2e6;
    SynthConfig c; c.snrDb = 25;
    auto syn = makePacketSynth(c, rate);
    V base((size_t)(rate * (packetGenCycleSec(0) + 1)));
    for (size_t i = 0; i < base.size(); i += 65536) syn->generate(base.data() + i, std::min<size_t>(65536, base.size() - i));
    const size_t want = packetGenCycle(0).size();
    const std::vector<std::pair<const char*, std::function<void(V&)>>> cases = {
        {"carrier -8 kHz", [&](V& x) { impair::shift(x, -8000, rate); }},
        {"carrier +10 kHz", [&](V& x) { impair::shift(x, 10000, rate); }},
        {"carrier -12 kHz", [&](V& x) { impair::shift(x, -12000, rate); }},
        {"clock +100 ppm", [](V& x) { x = impair::clock(x, 100); }},
        {"clock -100 ppm", [](V& x) { x = impair::clock(x, -100); }},
        {"echo", [](V& x) { impair::echo(x, 37, -6, 2.0); }},
        {"8-bit clipped", [](V& x) { impair::clip8(x, 6); }},
        {"combined: -8 kHz, +80 ppm, echo, 8-bit clipped", [&](V& x) { impair::shift(x, -8000, rate); x = impair::clock(x, 80); impair::echo(x, 37, -6, 2.0); impair::clip8(x, 6); }},
    };
    for (const auto& [name, f] : cases) {
        V x = base; f(x);
        const PacketTelemetry t = run(x, rate);
        printf("%-48s %llu of %zu frames, carrier %.0f Hz\n", name, (unsigned long long)t.blocksOk, want, t.cfoHz);
        if (t.blocksOk != want) { printf("FAIL: %s\n", name); fails++; }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
