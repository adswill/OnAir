// DTMB receiver on the test signal: lock, bit-exact packets, header modes and profiles, carrier and clock offsets, echoes, input rates, odd chunk
// sizes, 8-bit samples with DC offset and IQ imbalance, dropouts, reset.
#include "dect2/dtmb_gen.h"
#include "dect2/dtmb_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double cpuSeconds() { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

struct Opt {
    SignalConfig sc;
    double seconds = 2.0;
    int chunkMode = 0;           // 0: 16384 samples, 1: a mix of 1, 7, 4096, 65536 and odd sizes
    bool quantise = true;        // 8 bits like the radio
    cf32 dc{0, 0};               // DC offset added before quantisation
    double iqGainDb = 0, iqPhaseDeg = 0;
    double gapAt = -1, gapSec = 0; bool gapRemove = false;   // a dropout: zeros, or samples that are simply missing
    double resetAt = -1;
    int threads = 2;
};

struct Res {
    uint64_t good = 0, wrong = 0, missing = 0, backwards = 0;
    double firstGoodSec = -1, lastGoodSec = -1;
    uint64_t goodAfterReset = 0;
    DtmbTelemetry tel;
    uint64_t seqBack = 0;
    double rxCpu = 0, signalSec = 0;
    std::vector<std::string> log;
};

static bool verbose = false;

static Res run(const Opt& o) {
    Res r;
    Signal sig(o.sc, testPacketSource(1));
    DtmbReceiver rx;
    rx.configure(o.sc.rate);
    rx.setDecoderThreads(o.threads);
    uint32_t expect = 0xFFFFFFFFu;
    double now = 0;
    bool afterReset = false;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            uint32_t num = 0xFFFFFFFFu;
            if (checkTestPacket(p + i * 188, 1, &num)) {
                r.good++;
                if (afterReset) r.goodAfterReset++;
                if (r.firstGoodSec < 0) r.firstGoodSec = now;
                r.lastGoodSec = now;
                if (expect != 0xFFFFFFFFu) { if (num < expect) r.backwards++; else r.missing += num - expect; }
                expect = num + 1;
            } else r.wrong++;
        }
    });
    rx.setLogCallback([&](const std::string& s) { r.log.push_back(s); });
    const size_t total = (size_t)(o.seconds * o.sc.rate);
    std::vector<cf32> buf;
    std::mt19937 rng(12);
    const float gI = (float)std::pow(10.0, o.iqGainDb / 40.0), gQ = 1.f / gI;
    const float sp = (float)std::sin(o.iqPhaseDeg * 3.14159265 / 180.0), cp = (float)std::cos(o.iqPhaseDeg * 3.14159265 / 180.0);
    uint64_t last = 0, produced = 0, fed = 0;
    static const size_t odd[] = {1, 7, 4096, 65536, 3, 1000, 12345, 65535, 17, 8191};
    size_t oddIdx = 0;
    double cpu = 0;
    while (produced < total) {
        size_t n = o.chunkMode == 0 ? 16384 : (produced < 3000 ? 1 : produced < 6000 ? 7 : odd[oddIdx++ % 10]);
        n = std::min(n, total - (size_t)produced);
        buf.resize(n);
        sig.generate(buf.data(), n);
        for (auto& v : buf) {
            float re = v.real(), im = v.imag();
            if (o.iqGainDb != 0 || o.iqPhaseDeg != 0) { const float a = re * gI, b = im * gQ; re = a; im = b * cp + a * sp; }
            re += o.dc.real(); im += o.dc.imag();
            if (o.quantise) { re = std::round(std::min(127.f, std::max(-128.f, re * 128.f))) / 128.f; im = std::round(std::min(127.f, std::max(-128.f, im * 128.f))) / 128.f; }
            v = cf32(re, im);
        }
        now = (double)produced / o.sc.rate;
        const bool inGap = o.gapAt >= 0 && now >= o.gapAt && now < o.gapAt + o.gapSec;
        if (o.resetAt >= 0 && !afterReset && now >= o.resetAt) { rx.reset(); afterReset = true; }
        const double c0 = cpuSeconds();
        if (inGap && o.gapRemove) { /* the samples never arrive */ }
        else {
            if (inGap) for (auto& v : buf) v = cf32(0, 0);
            rx.feed(buf.data(), n);
        }
        cpu += cpuSeconds() - c0;
        produced += n;
        fed += n;
        DtmbTelemetry t;
        if (rx.telemetry(t, last)) {
            if (t.seq < last) r.seqBack++;
            last = t.seq; r.tel = t;
            if (verbose) printf("      t=%.2f state %d frames %llu loss %.0f%% ok %llu bad %llu dropped %llu packets %llu good %llu missing %llu\n", now, t.state, (unsigned long long)t.frames, t.frameLossPct, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
                                (unsigned long long)t.cwDropped, (unsigned long long)t.packets, (unsigned long long)r.good, (unsigned long long)r.missing);
        }
    }
    const double c0 = cpuSeconds();
    rx.flush();
    cpu += cpuSeconds() - c0;
    r.rxCpu = cpu; r.signalSec = o.seconds;
    (void)fed;
    return r;
}

static SignalConfig make(Header h, Mapping m, Rate rt, bool mode2, double rate, double snr) {
    SignalConfig sc;
    sc.rate = rate; sc.snrDb = snr;
    sc.tx.header = h; sc.tx.profile.map = m; sc.tx.profile.rate = rt; sc.tx.profile.mode2 = mode2;
    sc.tx.phaseRotate = h != Header::Pn595;
    return sc;
}

// the receiver found what was sent, and everything after the start-up came out right
static void expectGood(const char* name, const Res& r, const SignalConfig& sc, double minFraction, double maxLockSec) {
    const double perSec = netBitrate(sc.tx.header, sc.tx.profile) / kTsBits;
    const double upTime = r.lastGoodSec - r.firstGoodSec;
    printf("  %-44s first packet at %.2f s, %llu good, %llu wrong, %llu missing; %s %s %s mode %d C/N %.1f MER %.1f clock %.1f ppm cfo %.0f Hz; cpu %.2fx real time\n", name, r.firstGoodSec, (unsigned long long)r.good,
           (unsigned long long)r.wrong, (unsigned long long)r.missing, headerInfo(sc.tx.header).name, mappingName(sc.tx.profile.map), rateName(sc.tx.profile.rate), sc.tx.profile.mode2 ? 2 : 1, r.tel.snrPnDb, r.tel.merDb,
           r.tel.clockPpm, r.tel.cfoHz, r.signalSec / std::max(r.rxCpu, 1e-9));
    CHECK(r.firstGoodSec >= 0 && r.firstGoodSec < maxLockSec, "%s: first packet at %.2f s", name, r.firstGoodSec);
    CHECK(r.wrong == 0, "%s: %llu wrong packets", name, (unsigned long long)r.wrong);
    CHECK(r.backwards == 0, "%s: packets out of order", name);
    CHECK((double)r.good >= minFraction * perSec * std::max(0.0, r.signalSec - maxLockSec), "%s: %llu packets, expected about %.0f", name, (unsigned long long)r.good, perSec * (r.signalSec - maxLockSec));
    CHECK(r.seqBack == 0, "%s: telemetry sequence went back", name);
    (void)upTime;
}

static const char* only = nullptr;

static void show(const Res& r) {
    if (!verbose) return;
    for (const auto& l : r.log) printf("      [log] %s\n", l.c_str());
    printf("      %s\n", dtmbSummary(r.tel).c_str());
}

#define CASE(NAME) if (!only || strstr(NAME, only))

int main(int argc, char** argv) {
    if (argc > 1) { only = argv[1]; verbose = true; }
    // ---- every header mode, a few profiles, both interleavers
    CASE("PN945 64QAM 0.6 mode 1") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam64, Rate::R06, false, 10e6, 35); o.seconds = 1.5;
        Res r = run(o); expectGood("PN945 64QAM 0.6 mode 1", r, o.sc, 0.97, 0.45); show(r);
        CHECK(r.tel.state == 2 && r.tel.siOk && r.tel.mapping == 4 && r.tel.rate == 1 && r.tel.interleaver == 1 && r.tel.header == 2 && r.tel.tsLock, "telemetry of the locked state");
        CHECK(r.tel.snrPnDb > 30 && r.tel.snrPnDb < 40, "C/N %.1f", r.tel.snrPnDb);
        CHECK(!r.tel.cells.empty() && !r.tel.cirDb.empty(), "constellation and impulse response in the telemetry");
    }
    CASE("PN420 4QAM 0.4 mode 2") {
        Opt o; o.sc = make(Header::Pn420, Mapping::Qam4, Rate::R04, true, 10e6, 20); o.seconds = 1.6;
        Res r = run(o); expectGood("PN420 4QAM 0.4 mode 2", r, o.sc, 0.97, 0.9); show(r);
    }
    CASE("PN595 16QAM 0.8 mode 1") {
        Opt o; o.sc = make(Header::Pn595, Mapping::Qam16, Rate::R08, false, 10e6, 30); o.seconds = 1.2;
        Res r = run(o); expectGood("PN595 16QAM 0.8 mode 1", r, o.sc, 0.97, 0.45); show(r);
    }
    CASE("PN945 32QAM") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam32, Rate::R08, false, 10e6, 30); o.seconds = 1.2;
        Res r = run(o); expectGood("PN945 32QAM 0.8 mode 1 (frame pairs)", r, o.sc, 0.97, 0.45); show(r);
    }
    CASE("PN420 4QAM-NR") {
        Opt o; o.sc = make(Header::Pn420, Mapping::Qam4Nr, Rate::R08, false, 10e6, 12); o.seconds = 1.2;
        Res r = run(o); expectGood("PN420 4QAM-NR 0.8 mode 1", r, o.sc, 0.97, 0.45); show(r);
    }
    // ---- carrier offset, clock offset, input rates
    CASE("cfo +5200") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam16, Rate::R06, false, 10e6, 30); o.sc.cfoHz = 5200; o.sc.sroPpm = 45; o.seconds = 1.4;
        Res r = run(o); expectGood("cfo +5200 Hz, clock 45 ppm", r, o.sc, 0.95, 0.6); show(r);
        CHECK(std::fabs(r.tel.cfoHz - 5200) < 40, "cfo %.1f", r.tel.cfoHz);
        CHECK(std::fabs(r.tel.clockPpm + 45) < 3, "clock %.1f ppm", r.tel.clockPpm);
    }
    CASE("cfo -9500") {
        Opt o; o.sc = make(Header::Pn420, Mapping::Qam16, Rate::R06, false, 10e6, 30); o.sc.cfoHz = -9500; o.sc.sroPpm = -60; o.seconds = 1.4;
        Res r = run(o); expectGood("cfo -9500 Hz, clock -60 ppm", r, o.sc, 0.95, 0.9); show(r);
        CHECK(std::fabs(r.tel.cfoHz + 9500) < 40, "cfo %.1f", r.tel.cfoHz);
    }
    for (double rate : {8e6, 12.5e6, 16e6, 20e6}) {
        char name[64]; snprintf(name, sizeof name, "input rate %.1f Msps", rate / 1e6);
        CASE(name) {
            Opt o; o.sc = make(Header::Pn945, Mapping::Qam16, Rate::R06, false, rate, 30); o.seconds = 1.0;
            Res r = run(o); expectGood(name, r, o.sc, 0.95, 0.5); show(r);
        }
    }
    // ---- echoes, also before the main path
    CASE("echoes 6 dB") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam16, Rate::R06, false, 10e6, 30); o.sc.echoes.push_back({6, 60}); o.sc.echoes.push_back({9, -80}); o.seconds = 1.2;
        Res r = run(o); expectGood("echoes 6 dB at +60 and 9 dB at -80 samples", r, o.sc, 0.95, 0.5); show(r);
        CHECK(r.tel.echoSpanUs > 12.f && r.tel.echoSpanUs < 20.f, "echo span %.1f us", r.tel.echoSpanUs);
    }
    CASE("PN420 echo") {
        Opt o; o.sc = make(Header::Pn420, Mapping::Qam16, Rate::R04, false, 10e6, 30); o.sc.echoes.push_back({3, 50}); o.seconds = 1.2;
        Res r = run(o); expectGood("PN420 echo 3 dB at +50 samples", r, o.sc, 0.95, 0.5); show(r);
    }
    // ---- odd chunk sizes, 8-bit samples with a DC offset and IQ imbalance
    CASE("odd chunks") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam16, Rate::R06, false, 10e6, 30); o.chunkMode = 1; o.dc = cf32(0.04f, -0.03f); o.iqGainDb = 0.8; o.iqPhaseDeg = 2.5; o.seconds = 1.2;
        Res r = run(o); expectGood("odd chunks, DC offset, IQ imbalance, 8 bit", r, o.sc, 0.95, 0.5); show(r);
    }
    // ---- dropouts and reset
    CASE("zeroed samples") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam16, Rate::R06, false, 10e6, 30); o.gapAt = 1.0; o.gapSec = 0.004; o.seconds = 1.8;
        Res r = run(o); show(r);
        printf("  zeroed samples for 4 ms: %llu good, %llu wrong, %llu missing\n", (unsigned long long)r.good, (unsigned long long)r.wrong, (unsigned long long)r.missing);
        CHECK(r.wrong == 0 && r.lastGoodSec > 1.6, "recovery after zeros: last packet at %.2f s", r.lastGoodSec);
        CHECK(r.missing < 3 * 480, "too many packets lost in a 4 ms dropout: %llu", (unsigned long long)r.missing);
    }
    CASE("samples missing") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam16, Rate::R06, false, 10e6, 30); o.gapAt = 1.0; o.gapSec = 0.0033; o.gapRemove = true; o.seconds = 2.2;
        Res r = run(o); show(r);
        printf("  3.3 ms of samples missing: %llu good, %llu wrong, last packet at %.2f s\n", (unsigned long long)r.good, (unsigned long long)r.wrong, r.lastGoodSec);
        CHECK(r.wrong == 0 && r.lastGoodSec > 1.9, "no recovery after missing samples: last packet at %.2f s", r.lastGoodSec);
    }
    CASE("reset") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam16, Rate::R06, false, 10e6, 30); o.resetAt = 0.9; o.seconds = 1.8;
        Res r = run(o); show(r);
        printf("  reset at 0.9 s: %llu good, after the reset %llu, last packet at %.2f s\n", (unsigned long long)r.good, (unsigned long long)r.goodAfterReset, r.lastGoodSec);
        CHECK(r.wrong == 0 && r.goodAfterReset > 100 && r.seqBack == 0, "reset: %llu packets after, telemetry went back %llu", (unsigned long long)r.goodAfterReset, (unsigned long long)r.seqBack);
    }
    // ---- no signal: stays searching, quiet
    CASE("noise only") {
        Opt o; o.sc = make(Header::Pn945, Mapping::Qam16, Rate::R06, false, 10e6, -80); o.seconds = 0.8;
        Res r = run(o); show(r);
        CHECK(r.good == 0 && r.tel.state == 0, "noise only: state %d", r.tel.state);
    }
    printf(failures ? "dtmb_rx: %d FAILED\n" : "dtmb_rx: all passed\n", failures);
    return failures ? 1 : 0;
}
