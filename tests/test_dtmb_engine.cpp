// DTMB through the engine, as the GUI uses it: the built-in test signal (synthetic source, standard 9) -> Engine -> telemetry and transport stream
// analysis. Real time, no files: the engine must lock, decode without dropping input or codewords, and its analysis must find the test-card
// programme. A second run retunes to another header, modulation, code rate and interleaver with an echo and a carrier offset.
#include "dect2/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool haveVideoAndAudio(Engine& e) {
    auto ts = e.tsSnapshot();
    for (auto& s : ts.services) {
        bool v = false, a = false;
        for (auto& x : s.streams) { v |= x.kind == "video"; a |= x.kind == "audio"; }
        if (v && a) return true;
    }
    return false;
}

int main() {
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "Synthetic";
    TuneSettings tune;
    tune.bandwidthMhz = 8; tune.sampleRate = 10e6; tune.centerHz = 530e6;
    tune.synth.mode = 9;                 // DTMB test signal: modeOpt all zero = PN945, 64QAM, rate 0.6, interleaver mode 1, demo programme
    tune.synth.snrDb = 32; tune.synth.cfoHz = 2400;
    FileOptions fo;
    e.setStandard(9);
    CHECK(e.start(dev, tune, fo), "engine started");
    CHECK(e.activeStandard() == 8, "DTMB is the active standard (%d)", e.activeStandard());

    // ---- default signal
    RxTelemetry t;
    uint64_t last = 0, seqBack = 0;
    bool locked = false, video = false;
    double lockedAt = -1;
    const auto t0 = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    while (since() < 25 && !(locked && video)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (e.latestRx(t, last)) {
            if (t.seq < last) seqBack++;
            last = t.seq;
            if (!locked && t.standard == 8 && t.dtmb.state == 2 && t.dtmb.tsLock) { locked = true; lockedAt = since(); }
        }
        video = haveVideoAndAudio(e);
    }
    printf("  locked after %.1f s, video and audio found: %d\n", lockedAt, (int)video);
    CHECK(locked, "no lock within 25 s");
    CHECK(video, "the transport stream analysis found no service with video and audio");
    // let it run a few seconds more, then look at the statistics
    std::this_thread::sleep_for(std::chrono::seconds(6));
    e.latestRx(t, 0);
    const DtmbTelemetry& d = t.dtmb;
    printf("  %s\n", dtmbSummary(d).c_str());
    printf("  frames %llu, level %.1f dBFS, MER %.1f dB, LDPC %.1f iterations, codewords ok %llu bad %llu dropped %llu, dropped input samples %llu, clock %.1f ppm\n", (unsigned long long)d.frames, d.levelDbfs, d.merDb, d.ldpcIter,
           (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad, (unsigned long long)d.cwDropped, (unsigned long long)e.droppedSamples(), d.clockPpm);
    CHECK(t.standard == 8 && d.siOk && d.header == 2 && d.mapping == 4 && d.rate == 1 && d.interleaver == 1, "parameters read from the system information");
    CHECK(std::fabs(d.cfoHz - 2400.0) < 60.0, "carrier offset %.1f Hz", d.cfoHz);
    CHECK(d.snrPnDb > 27.0f && d.snrPnDb < 36.0f, "C/N %.1f dB", d.snrPnDb);
    // three codewords per frame; the first 170 frames only fill the de-interleaver (relative to the frames processed, so that a slow build still counts)
    CHECK(d.frames > 500 && d.blocksOk + 30 >= (uint64_t)(0.97 * 3 * (double)(d.frames - 180)) && d.blocksBad * 200 <= d.blocksOk, "frames %llu, codewords ok %llu bad %llu", (unsigned long long)d.frames, (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad);
    CHECK(d.cwDropped == 0, "the decoder fell behind: %llu codewords dropped", (unsigned long long)d.cwDropped);
    CHECK(e.droppedSamples() == 0, "the engine dropped %llu input samples", (unsigned long long)e.droppedSamples());
    CHECK(!d.cells.empty() && d.cells.size() <= 2048 && d.cirDb.size() >= 64 && d.cirDb.size() <= 512, "constellation %zu cells, impulse response %zu taps", d.cells.size(), d.cirDb.size());
    CHECK(std::fabs(d.netMbps - 21.658f) < 0.01f, "net bit rate %.3f", d.netMbps);
    CHECK(seqBack == 0, "telemetry sequence went back");

    // ---- retune to another signal: PN420, 16QAM, rate 0.8, interleaver mode 2, an echo and a different carrier offset
    tune.synth.modeOpt[0] = 2; tune.synth.modeOpt[1] = 2; tune.synth.modeOpt[2] = 2; tune.synth.modeOpt[3] = 1;
    tune.synth.echoDb = 8; tune.synth.echoDelay = 70; tune.synth.cfoHz = -6100; tune.synth.snrDb = 30;
    CHECK(e.retuneReset(tune), "retune");
    bool second = false;
    const double t1 = since();
    while (since() - t1 < 25 && !second) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (e.latestRx(t, last)) {
            if (t.seq < last) seqBack++;
            last = t.seq;
            const DtmbTelemetry& x = t.dtmb;
            second = x.state == 2 && x.tsLock && x.header == 0 && x.mapping == 2 && x.rate == 2 && x.interleaver == 2;
        }
    }
    CHECK(second, "no lock on the second signal");
    std::this_thread::sleep_for(std::chrono::seconds(4));
    e.latestRx(t, 0);
    const DtmbTelemetry& d2 = t.dtmb;
    printf("  %s\n", dtmbSummary(d2).c_str());
    printf("  echo span %.1f us, codewords ok %llu bad %llu dropped %llu\n", d2.echoSpanUs, (unsigned long long)d2.blocksOk, (unsigned long long)d2.blocksBad, (unsigned long long)d2.cwDropped);
    CHECK(std::fabs(d2.cfoHz + 6100.0) < 60.0, "carrier offset %.1f Hz", d2.cfoHz);
    CHECK(d2.echoSpanUs > 7.0f && d2.echoSpanUs < 11.0f, "echo span %.1f us (70 samples at 10 Msps = 7 us)", d2.echoSpanUs);
    CHECK(d2.frames > 600 && d2.blocksOk + 30 >= (uint64_t)(0.97 * 2 * (double)(d2.frames - 520)) && d2.blocksBad * 100 <= d2.blocksOk + d2.blocksBad, "frames %llu, codewords ok %llu bad %llu", (unsigned long long)d2.frames, (unsigned long long)d2.blocksOk, (unsigned long long)d2.blocksBad);
    CHECK(d2.cwDropped == 0, "codewords dropped on the second signal");
    CHECK(seqBack == 0, "telemetry sequence went back after the retune");
    e.stop();
    printf(fails ? "dtmb engine: FAILED\n" : "dtmb engine: ok\n");
    return fails ? 1 : 0;
}
