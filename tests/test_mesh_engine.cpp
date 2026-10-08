// Mesh (LoRa) through the engine as the app and dect2cli use it: the synthetic source plays the test signal (8-bit samples, paced
// faster than a radio when it can), the engine keeps every sample and reports the mesh tables through latestRx().
#include "dect2/engine.h"
#include "dect2/mesh_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace dect2;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double rate = 2e6;
    const double pace = SANITIZED ? 0.25 : std::getenv("CI") ? 1.0 : 2.0;
    const double signalSecs = SANITIZED ? 8 : 30;
    Engine e;
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    t.centerHz = meshTuning().defMhz * 1e6;
    t.bandwidthMhz = meshTuning().bandwidthMhz;
    t.sampleRate = rate;
    t.synth.mode = 22;
    t.synth.pace = pace;
    t.synth.snrDb = 25;
    t.synth.modeVal[0] = 5;
    FileOptions fo;
    e.setStandard(22);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 21, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0, last = 0;
    bool seqOk = true;
    int reports = 0;
    double firstFrame = -1;
    const auto t0 = std::chrono::steady_clock::now();
    const double wall = signalSecs / pace;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < wall) {
        if (e.latestRx(rx, seq)) {
            reports++;
            seqOk &= rx.seq > last; last = rx.seq; seq = rx.seq;
            CHECK(rx.standard == 21, "report standard %d", rx.standard);
            if (rx.blocksOk > 0 && firstFrame < 0) firstFrame = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * pace;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.latestRx(rx, 0);
    const MeshTelemetry& m = rx.mesh;
    int mt = 0, mc = 0, named = 0;
    for (const auto& n : m.nodes) { (n.protocol == 1 ? mt : mc)++; named += !n.longName.empty(); }
    printf("engine, 2 Msps, %.0f s of signal (pace %.2f): %d reports, first frame after %.1f s\n  %s\n  dropped samples %llu\n", signalSecs, pace, reports, firstFrame,
           meshSummary(m).c_str(), (unsigned long long)e.droppedSamples());
    for (const auto& d : m.decoders) printf("  %s %.3f MHz: %llu frames, %llu CRC errors\n", d.preset.c_str(), d.freqHz / 1e6, (unsigned long long)d.frames, (unsigned long long)d.crcBad);
    CHECK(seqOk && reports >= signalSecs * 3, "report numbers: %d reports, grew %d", reports, seqOk);
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(e.droppedSamples() == 0, "dropped %llu samples", (unsigned long long)e.droppedSamples());
    CHECK(m.blocksBad == 0 && rx.blocksBad == m.blocksBad && rx.blocksOk == m.blocksOk, "frames %llu good, %llu bad", (unsigned long long)m.blocksOk, (unsigned long long)m.blocksBad);
    CHECK(firstFrame > 0 && firstFrame < 2.5, "first frame after %.1f s", firstFrame);
    CHECK(std::fabs(m.inputRate - rate) < 1 && std::fabs(m.tunedHz - 869.525e6) < 1, "set-up %.0f %.0f", m.inputRate, m.tunedHz);
    if (!SANITIZED) {
        CHECK(rx.state == 2 && rx.dataValid && m.blocksOk >= 20, "state %d, %llu frames", rx.state, (unsigned long long)m.blocksOk);
        CHECK(mt >= 5 && mc == 3 && named >= 8, "nodes: %d Meshtastic, %d MeshCore, %d named", mt, mc, named);
        CHECK(m.messages.size() >= 8, "%zu messages", m.messages.size());
        CHECK(std::fabs(rx.dataSnrDb - m.snrDb) < 1e-3, "SNR copied");
    } else {
        CHECK(m.blocksOk >= 3, "%llu frames", (unsigned long long)m.blocksOk);
    }
    // a retune empties the tables, and the report numbers go on
    TuneSettings t2 = t;
    t2.centerHz += 1e6;
    e.retuneReset(t2);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    RxTelemetry r2;
    // (the test signal starts again: a frame or two may already be back)
    CHECK(e.latestRx(r2, 0) && r2.seq > last && r2.mesh.blocksOk < m.blocksOk && r2.mesh.nodes.size() <= 2, "after a retune: seq %llu, %llu frames, %zu nodes",
          (unsigned long long)r2.seq, (unsigned long long)r2.mesh.blocksOk, r2.mesh.nodes.size());
    e.stop();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
