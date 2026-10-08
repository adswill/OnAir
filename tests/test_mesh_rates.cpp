// Mesh (LoRa): the test signal through the receiver at the sample rates radios give (2.4, 8, 10 and 20 Msps; 2 Msps is in mesh_rx),
// with 8-bit samples and odd chunk sizes: every frame decodes, none fails its CRC; the CPU time per second of signal is printed.
// Also the US region at 10 Msps, where Meshtastic (906.875 MHz) and MeshCore (910.525 MHz) are 3.65 MHz apart.
#include "dect2/mesh_gen.h"
#include "dect2/mesh_rx.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run(double rate, double secs, int region, size_t chunk) {
    SynthConfig cfg;
    cfg.mode = 22; cfg.snrDb = 20; cfg.modeVal[0] = 3; cfg.modeOpt[1] = region;
    auto syn = makeMeshSynth(cfg, rate);
    MeshReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-meshTuning().tuneOffsetHz);
    rx.setRegion(region);
    if (region == 1) rx.setTunedHz(906.875e6);
    std::vector<cf32> buf(chunk);
    double cpu = 0;
    for (size_t done = 0; done < (size_t)(secs * rate); done += chunk) {
        syn->generate(buf.data(), chunk);
        for (auto& v : buf) v = cf32(std::round(v.real() * 127.f) / 127.f, std::round(v.imag() * 127.f) / 127.f);
        const std::clock_t a = std::clock();
        rx.feed(buf.data(), chunk);
        cpu += (double)(std::clock() - a) / CLOCKS_PER_SEC;
    }
    MeshTelemetry t;
    rx.telemetry(t, 0);
    uint64_t mt = 0, mc = 0;
    for (const auto& d : t.decoders) (d.protocol == 1 ? mt : mc) += d.frames;
    printf("%s %5.1f Msps, chunks of %5zu: %llu Meshtastic + %llu MeshCore frames, %llu CRC errors, %zu nodes; receiver %.2f s CPU per s of signal\n",
           region ? "US" : "EU", rate / 1e6, chunk, (unsigned long long)mt, (unsigned long long)mc, (unsigned long long)t.blocksBad, t.nodes.size(), cpu / secs);
    // 8 Meshtastic and 4 MeshCore frames end in the first 12 s (the last a few ms after: 7 or 8)
    CHECK(mt >= (secs >= 12 ? 7 : 4) && t.blocksBad == 0, "%.1f Msps: %llu Meshtastic frames, %llu bad", rate / 1e6, (unsigned long long)mt, (unsigned long long)t.blocksBad);
    CHECK(mc >= (secs >= 12 ? 4 : 2), "%.1f Msps: %llu MeshCore frames", rate / 1e6, (unsigned long long)mc);
    CHECK(cpu / secs < 0.5, "%.1f Msps: %.2f s of CPU per second", rate / 1e6, cpu / secs);
}

int main() {
    run(2.4e6, 12, 0, 4093);
    run(2.4e6, 12, 0, 65536);
    run(2e6, 12, 0, 65536);
    run(8e6, 12, 0, 65536);
    run(10e6, 12, 0, 7777);
    run(20e6, 8, 0, 65536);
    run(10e6, 12, 1, 65536);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
