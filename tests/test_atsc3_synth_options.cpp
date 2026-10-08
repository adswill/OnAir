// The options of the ATSC 3.0 test signal (SynthConfig::modeOpt, see atsc3_synth.h): every choice of modulation, code rate, FEC frame, guard interval, video
// codec and L1-Detail mode must give a signal the real receiver decodes into a transport stream with video and sound.
#include "atsc3_sim.h"
#include "dect2/atsc3_rx.h"
#include "dect2/atsc3_synth.h"
#include "jobs.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, m) do { if (!(c)) { jprintf("FAIL: %s\n", m); fails++; } } while (0)

// the 8-bit samples of the synthetic source: the receiver gets what a HackRF would send
static void quantize(cf32* v, size_t n) {
    auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
    for (size_t i = 0; i < n; i++) v[i] = cf32(q(v[i].real()), q(v[i].imag()));
}

struct Opt {
    const char* name;
    int opt[6];
    double snr;
    int wantCodec;   // 0 any, 1 must be HEVC, 2 must not be HEVC
};

static void run(const Opt& o) {
    const double rate = 6.144e6, secs = 9;
    SynthConfig cfg;
    cfg.snrDb = o.snr;
    for (int i = 0; i < 6; i++) cfg.modeOpt[i] = o.opt[i];
    auto synth = makeAtsc3Synth(cfg, rate);
    if (!synth) { jprintf("  %-28s FAIL: no generator\n", o.name); fails++; return; }
    Atsc3Rx rx;
    std::vector<uint8_t> ts;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { ts.insert(ts.end(), p, p + n * 188); });
    rx.setBlocking(true);
    rx.configure(rate);
    std::vector<cf32> buf(65536);
    for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) { synth->generate(buf.data(), buf.size()); quantize(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    Atsc3Telemetry t;
    uint64_t seq = 0;
    long last = -1;
    // wait until the decoder has stopped producing frames: no new frame for 200 ms. Polled every 20 ms (not 200 ms with a fixed minimum of 1 s).
    int still = 0;
    for (int i = 0; i < 1000 && still < 10; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (rx.telemetry(t, seq)) seq = t.seq;
        still = (i > 0 && t.frames == last) ? still + 1 : 0;
        last = t.frames;
    }
    rx.telemetry(t, 0);
    rx.stop();
    long v = 0, a = 0;
    bool hevc = false, aac = false;
    const bool opened = sim::countTs(ts, v, a, hevc, aac);
    const bool serviceOk = t.services.size() == 1 && t.services[0].serviceId == 1001 && t.selected == 1001;
    jprintf("  %-28s frames %ld (%ld failed), bb %ld (%ld bad), service %s, TS %zu B: %ld video %s + %ld audio %s, PLP %d bits/cell rate %d/15 %d\n", o.name, t.frames, t.framesFailed, t.bbPackets, t.bbBad,
           serviceOk ? "ok" : "MISSING", ts.size(), v, hevc ? "hevc" : "other", a, aac ? "aac" : "other",
           t.frame.plps.empty() ? 0 : t.frame.plps[0].bitsPerCell, t.frame.plps.empty() ? 0 : t.frame.plps[0].rate15, t.frame.plps.empty() ? 0 : t.frame.plps[0].nInner);
    CHECK(t.frames >= (long)(secs / 0.15), "frames decoded");
    CHECK(t.framesFailed <= 2 && t.bbBad == 0, "no bad frames or packets");
    CHECK(serviceOk && t.serviceReady, "service list and signaling");
    CHECK(opened && aac && a >= (long)((secs - 5) * 46), "audio");
    CHECK(v >= (long)((secs - 5) * 25), "video");
    if (o.wantCodec == 1) CHECK(hevc, "HEVC video");
    if (o.wantCodec == 2) CHECK(!hevc, "not HEVC video");
}

int main() {
    const Opt all[] = {
        {"QPSK 8/15 64K (default)", {0, 0, 0, 0, 0, 0}, 25, 0},
        {"16QAM 8/15", {1, 0, 0, 0, 0, 0}, 25, 0},
        {"64QAM 8/15", {2, 0, 0, 0, 0, 0}, 30, 0},
        {"256QAM 8/15", {3, 0, 0, 0, 0, 0}, 40, 0},
        {"QPSK 8/15 16K", {0, 0, 1, 0, 0, 0}, 25, 0},
        {"16QAM 6/15 16K", {1, 5, 1, 0, 0, 0}, 25, 0},
        {"QPSK 4/15", {0, 3, 0, 0, 0, 0}, 25, 0},
        {"QPSK 13/15", {0, 12, 0, 0, 0, 0}, 25, 0},
        {"guard 192", {0, 0, 0, 1, 0, 0}, 25, 0},
        {"guard 2048 (1/4)", {0, 0, 0, 7, 0, 0}, 25, 0},
        {"guard 4864", {0, 0, 0, 12, 0, 0}, 25, 0},
        {"L1-Detail mode 4", {0, 0, 0, 0, 0, 4}, 25, 0},
        {"H.264", {0, 0, 0, 0, 2, 0}, 25, 0},
        {"MPEG-2 video", {0, 0, 0, 0, 3, 0}, 25, 2},
        {"256QAM 13/15 16K", {3, 12, 1, 0, 0, 0}, 40, 0},
    };
    // the cases are independent: each one runs on its own thread, the output keeps the order of the cases
    testjobs::Jobs jobs;
    for (const auto& o : all) jobs.add([&o] { run(o); });
    jobs.run();
    jprintf(fails ? "atsc3 synth options: FAILED\n" : "atsc3 synth options: ok\n");
    return fails ? 1 : 0;
}
