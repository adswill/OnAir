// Mesh (LoRa): the test signal through the receiver at the sample rates radios give (2.4, 8, 10 and 20 Msps; 2 Msps is in mesh_rx),
// with 8-bit samples and odd chunk sizes: every frame decodes, none fails its CRC; the CPU time per second of signal is printed.
// Also the US region at 10 Msps, where Meshtastic (906.875 MHz) and MeshCore (910.525 MHz) are 3.65 MHz apart.
// Default (ctest mesh_rates, also CI): every node at the weakest level of the scene (6 dB in 250 kHz, +-1.5 dB per frame): what decodes
// there decodes stronger too, so 8 s of signal are enough (Meshtastic NodeInfo, text and position, MeshCore adverts and a public text).
// --full (not in ctest, run it by hand): the whole scene, nodes from 20 down to 3 dB, 12 s (20 Msps 8 s), every frame of that time.
//
// STOP: a change to the mesh receiver (core/src/mesh/mesh_rx.cpp, mesh_lora_demod.cpp, mesh_lora_fd.cpp), to the mesh test signal
// (core/src/mesh/mesh_gen.cpp) or to this test also needs a run of the full version: test_mesh_rates --full. It does not run in ctest.
#include "dect2/mesh/mesh_gen.h"
#include "../common/cpu_report.h"
#include "../common/cpu_time.h"
#include "../common/ci_slack.h"
#include "dect2/mesh/mesh_rx.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

struct Case { double rate, secs; int region; size_t chunk; };
static bool gFull = false;
struct Result { uint64_t mt = 0, mc = 0, bad = 0; size_t nodes = 0; double cpu = 0; };

static Result run(const Case& c) {
    const double rate = c.rate, secs = c.secs;
    const int region = c.region;
    const size_t chunk = c.chunk;
    SynthConfig cfg;
    cfg.mode = 22; cfg.modeOpt[1] = region;
    cfg.snrDb = gFull ? 20 : 6; cfg.modeVal[0] = gFull ? 3 : 6;   // the strongest and the weakest node (short: both at 6 dB)
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
        const double a = processCpuSecs();
        rx.feed(buf.data(), chunk);
        cpu += (processCpuSecs() - a);
    }
    MeshTelemetry t;
    rx.telemetry(t, 0);
    Result r;
    for (const auto& d : t.decoders) (d.protocol == 1 ? r.mt : r.mc) += d.frames;
    r.bad = t.blocksBad; r.nodes = t.nodes.size(); r.cpu = cpu;
    return r;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--full")) gFull = true;
    const double s = gFull ? 12 : 8;
    const Case cases[] = {
        {2.4e6, s, 0, 4093}, {2.4e6, s, 0, 65536}, {2e6, s, 0, 65536}, {8e6, s, 0, 65536},
        {10e6, s, 0, 7777}, {20e6, 8, 0, 65536}, {10e6, s, 1, 65536},
    };
    constexpr size_t kCases = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < kCases; i++) {   // one after the other: side by side the CPU time of each grew 1.5 to 2 times (shared cores)
        const Case& c = cases[i];
        const Result r = run(c);
        printf("%s %5.1f Msps, chunks of %5zu: %llu Meshtastic + %llu MeshCore frames, %llu CRC errors, %zu nodes; receiver %.2f s CPU per s of signal\n",
               c.region ? "US" : "EU", c.rate / 1e6, c.chunk, (unsigned long long)r.mt, (unsigned long long)r.mc, (unsigned long long)r.bad, r.nodes, r.cpu / c.secs);
        // 8 Meshtastic and 4 MeshCore frames end in the first 12 s (the last a few ms after: 7 or 8); 5 and 3 in the first 8 s (the full
        // run's 20 Msps case asks less, as it did: its weaker nodes)
        const bool longRun = c.secs >= 12;
        const uint64_t needMt = longRun ? 7 : gFull ? 4 : 5, needMc = longRun ? 4 : gFull ? 2 : 3;
        CHECK(r.mt >= needMt && r.bad == 0, "%.1f Msps: %llu Meshtastic frames, %llu bad", c.rate / 1e6, (unsigned long long)r.mt, (unsigned long long)r.bad);
        CHECK(r.mc >= needMc, "%.1f Msps: %llu MeshCore frames", c.rate / 1e6, (unsigned long long)r.mc);
        cpuCal("mesh_rates", std::string(c.region ? "US " : "EU ") + cpuCalMsps(c.rate) + ", chunks of " + std::to_string(c.chunk) + (gFull ? ", full" : ""), r.cpu / c.secs, "cpu-s/s", '<', 0.5 * ciSlack());
        CHECK(r.cpu / c.secs < 0.5 * ciSlack(), "%.1f Msps: %.2f s of CPU per second", c.rate / 1e6, r.cpu / c.secs);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
