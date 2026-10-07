// The ATSC 3.0 test signal (atsc3_synth.h) through the real receiver: the generator is played through the ModeSynth interface into Atsc3Rx, the way the app's
// synthetic source and the engine do. The receiver must find the bootstrap, decode L1 and the frames, learn the service list, and deliver a transport stream
// with the video and audio of the test card, at several output sample rates, with noise, carrier offset and clock offset.
//   test_atsc3_synth_rx                 a few cases of 12 s (the ctest test)
//   test_atsc3_synth_rx --full [secs]   the whole matrix (rates 6.144, 8, 10, 20 Msps; SNR 25 and 15 dB; carrier +-3 kHz; clock +-20 ppm; chunks 1, 7, 4096, 65536)
//   test_atsc3_synth_rx --one rate_msps snr cfo ppm chunk secs [modeOpt0 modeOpt1 modeOpt2 modeOpt3 modeOpt4]
#include "atsc3_sim.h"
#include "dect2/atsc3_rx.h"
#include "dect2/atsc3_synth.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static double wall() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// the 8-bit samples of the synthetic source: the receiver gets what a HackRF would send
static void quantize(cf32* v, size_t n) {
    auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
    for (size_t i = 0; i < n; i++) v[i] = cf32(q(v[i].real()), q(v[i].imag()));
}

struct Case {
    double rate = 10e6, snr = 25, cfo = 0, ppm = 0;
    size_t chunk = 65536;
    double secs = 12;
    int opt[6] = {};
    const char* name = "";
};

struct Result {
    bool ok = false;
    double genX = 0, rxLoad = 0, cfoEst = 0;
    long frames = 0, failed = 0, bb = 0, bbBad = 0, objects = 0, video = 0, audio = 0;
    bool hevc = false, aac = false, ready = false, service = false;
};

static Result runCase(const Case& c) {
    Result r;
    SynthConfig cfg;
    cfg.snrDb = c.snr; cfg.cfoHz = c.cfo; cfg.sroPpm = c.ppm;
    for (int i = 0; i < 6; i++) cfg.modeOpt[i] = c.opt[i];
    auto synth = makeAtsc3Synth(cfg, c.rate);
    if (!synth) { printf("  FAIL: no generator\n"); fails++; return r; }
    CHECK(synth->sampleRate() == c.rate, "sample rate reported");
    Atsc3Rx rx;
    std::vector<uint8_t> ts;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { ts.insert(ts.end(), p, p + n * 188); });
    rx.setBlocking(true);
    rx.configure(c.rate);
    const size_t total = (size_t)(c.secs * c.rate);
    std::vector<cf32> buf(65536);
    double genSec = 0;
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t n = std::min(buf.size(), total - done);
        const double t0 = wall();
        for (size_t off = 0; off < n; off += c.chunk) synth->generate(buf.data() + off, std::min(c.chunk, n - off));
        genSec += wall() - t0;
        quantize(buf.data(), n);
        rx.feed(buf.data(), n);
    }
    // let the receiver finish what is queued
    Atsc3Telemetry t;
    uint64_t seq = 0;
    long lastFrames = -1;
    for (int i = 0; i < 100; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (rx.telemetry(t, seq)) seq = t.seq;
        if (i > 3 && t.frames == lastFrames) break;
        lastFrames = t.frames;
    }
    rx.telemetry(t, 0);
    rx.stop();
    r.genX = genSec > 0 ? c.secs / genSec : 0;
    r.rxLoad = t.load; r.cfoEst = t.cfoHz;
    r.frames = t.frames; r.failed = t.framesFailed; r.bb = t.bbPackets; r.bbBad = t.bbBad; r.objects = t.routeObjects;
    r.ready = t.serviceReady;
    r.service = t.services.size() == 1 && t.services[0].serviceId == 1001 && t.selected == 1001;
    bool opened = sim::countTs(ts, r.video, r.audio, r.hevc, r.aac);
    printf("  %-22s %5.3f Msps snr %2.0f cfo %+5.0f ppm %+3.0f chunk %5zu %2.0fs: frames %ld (%ld failed), bb %ld (%ld bad), objects %ld, cfo est %+.0f Hz, TS %zu B %s: %ld video %s + %ld audio %s | gen x%.1f, rx load %.2f\n",
           c.name, c.rate / 1e6, c.snr, c.cfo, c.ppm, c.chunk, c.secs, r.frames, r.failed, r.bb, r.bbBad, r.objects, r.cfoEst, ts.size(), opened ? "" : "(unreadable)",
           r.video, r.hevc ? "hevc" : "other", r.audio, r.aac ? "aac" : "other", r.genX, r.rxLoad);
    const double perFrameSec = 0.1025;
    const long expect = (long)(c.secs / perFrameSec);
    CHECK(r.frames >= expect * 9 / 10, "nearly every frame found and decoded");
    CHECK(r.failed <= 2, "frames decode");
    CHECK(r.bbBad == 0, "every baseband packet is good");
    CHECK(r.service, "the service list has the one service, and it is selected");
    CHECK(r.ready, "service signaling received");
    CHECK(std::fabs(r.cfoEst - c.cfo) < 300.0, "carrier offset estimate");
    CHECK(opened && r.aac, "transport stream with AAC audio");
    CHECK(r.hevc || r.video > 0, "video in the transport stream");
    // the remuxer holds back the first second or so; of the rest, nearly all pictures and sound frames come through
    CHECK(r.video >= (long)((c.secs - 5.0) * 25), "video frames");
    CHECK(r.audio >= (long)((c.secs - 5.0) * 46.875), "audio frames");
    r.ok = true;
    return r;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--one") && argc >= 8) {
        Case c; c.name = "one";
        c.rate = atof(argv[2]) * 1e6; c.snr = atof(argv[3]); c.cfo = atof(argv[4]); c.ppm = atof(argv[5]); c.chunk = (size_t)atol(argv[6]); c.secs = atof(argv[7]);
        for (int i = 0; i < 6 && 8 + i < argc; i++) c.opt[i] = atoi(argv[8 + i]);
        runCase(c);
    } else if (argc > 1 && !strcmp(argv[1], "--full")) {
        const double secs = argc > 2 ? atof(argv[2]) : 20;
        const double rates[] = {6.144e6, 8e6, 10e6, 20e6};
        for (double rate : rates) { Case c; c.name = "rate"; c.rate = rate; c.secs = secs; c.snr = 25; runCase(c); }
        for (double snr : {25.0, 15.0}) { Case c; c.name = "snr"; c.snr = snr; c.secs = secs; runCase(c); }
        for (double cfo : {-3000.0, 3000.0}) { Case c; c.name = "cfo"; c.cfo = cfo; c.secs = secs; runCase(c); }
        for (double ppm : {-20.0, 20.0}) { Case c; c.name = "clock"; c.ppm = ppm; c.secs = secs; runCase(c); }
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) { Case c; c.name = "chunk"; c.chunk = chunk; c.secs = secs; c.snr = 25; c.cfo = 1500; c.ppm = 5; runCase(c); }
        { Case c; c.name = "all"; c.rate = 8e6; c.snr = 15; c.cfo = -3000; c.ppm = 20; c.secs = secs; runCase(c); }
    } else {
        { Case c; c.name = "10 Msps"; c.rate = 10e6; c.snr = 25; c.cfo = 2500; c.ppm = 10; c.chunk = 4096; runCase(c); }
        { Case c; c.name = "6.144 Msps"; c.rate = 6.144e6; c.snr = 15; c.cfo = -3000; c.ppm = -20; c.chunk = 65536; runCase(c); }
        { Case c; c.name = "8 Msps"; c.rate = 8e6; c.snr = 15; c.cfo = 3000; c.ppm = 20; c.chunk = 7777; runCase(c); }
    }
    printf(fails ? "atsc3 synth rx: FAILED\n" : "atsc3 synth rx: ok\n");
    return fails ? 1 : 0;
}
