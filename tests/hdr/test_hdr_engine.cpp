// HD Radio through the engine, as the app uses it: the synthetic source plays the test signal (FM hybrid, then AM hybrid) at the rate of
// the tuning table with 15 dB SNR and a 1 kHz carrier offset, then AM all-digital (MA3) at the 1 Msps the app asks for in the AM band; the engine must find the waveform, decode the station information, both
// programs, their program data and the picture, report it through latestRx with the mode's standard, and keep up with real time.
// Real time: about 20 s.
#include "dect2/engine/engine.h"
#include "dect2/hdr/hdr_gen.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/engine/modes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void run(int am, double cfo, double maxSec, double rate = 0) {
    const ModeTuning mt = hdrTuning();
    const HdrTestContent& tc = hdrTestContent();
    Engine e;
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    t.centerHz = (am ? 1.03 : mt.defMhz) * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = rate > 0 ? rate : mt.sampleRate;
    t.synth.mode = 23;
    t.synth.modeOpt[0] = am;
    t.synth.snrDb = 15;
    t.synth.cfoHz = cfo;
    FileOptions fo;
    e.setStandard(23);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 22, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    bool back = false;
    double doneAt = -1;
    float load = 0, cpuLoad = 0;   // the wall-clock load (shown) and the CPU-time load (checked: other programs on a shared runner do not raise it)
    std::vector<uint8_t> img;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (e.latestRx(rx, seq)) {
            if (rx.seq < seq) back = true;
            seq = rx.seq;
            reports++;
            if (rx.standard != 22) wrong++;
            const HdrTelemetry& h = rx.hdr;
            int psd = 0;
            for (const auto& p : h.programs) if (p.psdCount) psd++;
            if (doneAt < 0 && !h.callSign.empty() && !h.message.empty() && !h.slogan.empty() && psd == 2 && e.hdr().lotBytes(tc.artPort, tc.artLot, img)) doneAt = el;
        }
        if (el > 3) { const SampleLoss sl = e.sampleLoss(); load = std::max(load, sl.loadPct); cpuLoad = std::max(cpuLoad, sl.cpuPct); }
        if (doneAt > 0 && el > doneAt + 1.0 && el > 3 && cpuLoad > 0) break;   // the load is measured over 2 s windows: wait for one
        if (el > maxSec) break;
    }
    e.stop();
    const HdrTelemetry& h = rx.hdr;
    printf("%s through the engine: %s after %.1f s, %d reports, %s, MER %.1f dB, CFO %+.1f Hz, P1 %llu ok %llu bad, call sign %s, %zu programs, receiver load %.1f%% of real time (CPU %.1f%%)\n",
           am == 3 ? "AM MA3" : am ? "AM" : "FM", doneAt > 0 ? "everything decoded" : "INCOMPLETE", doneAt, reports, hdrSummary(h).c_str(), h.snrDb, h.cfoHz,
           (unsigned long long)h.blocksOk, (unsigned long long)h.blocksBad, h.callSign.c_str(), h.programs.size(), load, cpuLoad);
    CHECK(doneAt > 0, "not everything decoded within %.0f s", maxSec);
    CHECK(wrong == 0 && !back, "%d reports with another standard, sequence back %d", wrong, (int)back);
    CHECK(rx.rateOk && rx.standard == 22 && rx.state == 3 && rx.dataValid, "rate ok %d, standard %d, state %d, data %d", (int)rx.rateOk, rx.standard, rx.state, (int)rx.dataValid);
    CHECK(h.band == (am ? 2 : 1), "band %d", h.band);
    CHECK(h.callSign == tc.callSign && h.stationName == tc.name && h.slogan == tc.slogan && h.message == tc.message, "station information");
    CHECK(img == hdrTestLogoPng(), "picture: %zu bytes", img.size());
    CHECK(std::fabs(rx.cfoHz - cfo) < 10, "CFO %.1f Hz", rx.cfoHz);
    CHECK(h.blocksBad == 0, "%llu bad P1 frames", (unsigned long long)h.blocksBad);
    CHECK(e.droppedSamples() == 0, "%llu samples dropped", (unsigned long long)e.droppedSamples());
    CHECK(cpuLoad > 0 && cpuLoad < 60, "receiver CPU load %.1f%% (wall clock %.1f%%)", cpuLoad, load);
}

int main() {
    const ModeTuning mt = hdrTuning();
    CHECK(mt.stdMode == 23 && modeTuningById("hdr") && modeTuningById("hdr")->stdMode == 23, "tuning table");
    run(0, 1000, 16);
    run(1, -1000, 20);
    CHECK(hdrSampleRate(1.03) == 1e6 && hdrMinSampleRate(1.03) < 1e6 && hdrMinSampleRate(98.5) == hdrTuning().minSampleRate, "AM rates");
    run(3, -60, 20, hdrSampleRate(1.03));
    printf("hdr engine: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
