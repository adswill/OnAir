// COSPAS-SARSAT through the engine at 0.5 Msps (an RTL-SDR / RSPduo style start): which receivers log at start, how much CPU the engine uses next to the
// bare receiver, and that the generated beacons are decoded with the tuner 20 kHz off the dial.
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_rx.h"
#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static double cpuSec() { rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6; }

int main() {
    const ModeTuning* mt = modeTuningById("sarsat");
    CHECK(mt, "tuning");
    if (!mt) return 1;
    const double rate = 500000;
    // bare receiver, free running: CPU seconds per second of signal
    {
        SynthConfig cfg; cfg.snrDb = 30;
        SarsatReceiver rx; rx.configure(rate); rx.setSignalOffset(-mt->tuneOffsetHz);
        auto syn = makeSarsatSynth(cfg, rate);
        std::vector<cf32> buf(32768);
        const double c0 = cpuSec(); double sig = 0;
        while (sig < 20) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); sig += buf.size() / rate; }
        printf("RX-ONLY cpu %.3f s per s of signal\n", (cpuSec() - c0) / sig);
    }
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic";
    TuneSettings tune;
    tune.centerHz = mt->defMhz * 1e6;
    tune.sampleRate = rate;
    tune.bandwidthMhz = mt->bandwidthMhz;
    tune.basebandFilterHz = mt->basebandHz;
    tune.synth.mode = mt->stdMode;
    tune.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(mt->stdMode);
    const double c0 = cpuSec();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(e.start(dev, tune, fo), "engine started");
    RxTelemetry t, last; uint64_t seq = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 25) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (e.latestRx(t, seq)) { seq = t.seq; last = t; }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double cpu = cpuSec() - c0;
    e.stop();
    size_t total = 0;
    printf("ENGINE LOG:\n");
    for (const auto& l : e.logSnapshot(total)) printf("  | %s\n", l.c_str());
    printf("ENGINE cpu %.3f s per s of wall (%.1f s wall), beacons ok %llu, bursts %llu\n", cpu / wall, wall, (unsigned long long)last.sarsat.blocksOk, (unsigned long long)last.sarsat.bursts);
    return fails ? 1 : 0;
}
