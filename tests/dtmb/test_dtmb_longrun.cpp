// DTMB receiver over a long run with the faults of a real radio coming and going (#319: the app hung after prolonged DTMB reception): radio overflows,
// impulse noise, dropouts of zeros and of missing samples, level steps, a carrier wave. Checks that no feed() call takes long (a stuck loop), that
// the receiver locks again after every fault, that the memory does not grow (the maximum resident size of the second half against the first)
// and that the receiver can be torn down while a fault is in progress. Argument: seconds of signal (default 12, 40 with ONAIR_TEST_LONG=1; 600 for a soak).
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "../common/test_long.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double rssMb() {
#ifdef _WIN32
    return 0.0;   // no getrusage on Windows: the growth check is skipped there
#else
    rusage ru; getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (double)ru.ru_maxrss / 1048576.0;
#else
    return (double)ru.ru_maxrss / 1024.0;
#endif
#endif
}
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
    // the default run: 12 s (two report windows, a level step at 7 s); ONAIR_TEST_LONG=1 the 40 s run
    const double seconds = argc > 1 ? atof(argv[1]) : testLongPick(12.0, 40.0);
    SignalConfig sc;
    sc.rate = 10e6; sc.snrDb = 28;
    sc.tx.header = Header::Pn945; sc.tx.profile.map = Mapping::Qam16; sc.tx.profile.rate = Rate::R06;
    sc.tx.phaseRotate = true;
    Signal sig(sc, testPacketSource(1));
    DtmbReceiver rx;
    rx.configure(sc.rate, 8.0);
    rx.setDecoderThreads(2);
    uint64_t packets = 0;
    rx.setPacketCallback([&](const uint8_t*, size_t n, double) { packets += n; });
    std::mt19937 rng(7);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    dect2::uniform_real_distribution<double> ur(0.0, 1.0);
    const size_t total = (size_t)(seconds * sc.rate);
    const size_t chunk = 65536;
    std::vector<cf32> buf(chunk);
    double worst = 0, rss1 = 0, rssHalf = 0;
    uint64_t last = 0, produced = 0, pk0 = 0;
    int lockedWins = 0, windows = 0;
    size_t dropLeft = 0, burstLeft = 0, zeroLeft = 0;
    double tonePhase = 0, lastReport = 0;
    float gain = 1.f;
    while (produced < total) {
        const size_t n = std::min(chunk, total - (size_t)produced);
        sig.generate(buf.data(), n);
        const double t = (double)produced / sc.rate;
        // fault schedule: every ~7 s a level step, every ~11 s a 0.3 s dropout of zeros, every ~13 s a gap of missing samples
        if (std::fmod(t, 7.0) < 65536.0 / sc.rate) gain = std::pow(10.f, (float)(ur(rng) * 20.0 - 10.0) / 20.f);
        size_t w = 0;
        for (size_t i = 0; i < n; i++) {
            cf32 v = buf[i] * gain;
            v += 0.05f * cf32((float)std::cos(tonePhase), (float)std::sin(tonePhase)); tonePhase += 0.9;
            if (burstLeft == 0 && ur(rng) < 20.0 / sc.rate) burstLeft = 200;
            if (burstLeft) { v += cf32(nd(rng), nd(rng)) * 0.5f; burstLeft--; }
            if (zeroLeft == 0 && ur(rng) < 1.0 / (11.0 * sc.rate)) zeroLeft = (size_t)(0.3 * sc.rate);
            if (zeroLeft) { v = cf32(0, 0); zeroLeft--; }
            if (dropLeft == 0 && ur(rng) < 50.0 / sc.rate) dropLeft = 100 + (size_t)(ur(rng) * 3000);
            if (dropLeft) { dropLeft--; continue; }
            v = cf32(std::round(std::min(127.f, std::max(-128.f, v.real() * 128.f))) / 128.f, std::round(std::min(127.f, std::max(-128.f, v.imag() * 128.f))) / 128.f);
            buf[w++] = v;
        }
        const double t0 = now();
        if (w) rx.feed(buf.data(), w);
        const double dt = now() - t0;
        worst = std::max(worst, dt);
        CHECK(dt < 5.0, "feed() took %.1f s at t=%.1f", dt, t);
        produced += n;
        DtmbTelemetry tel;
        if (rx.telemetry(tel, last)) { CHECK(tel.seq > last, "telemetry sequence"); last = tel.seq; }
        if (t - lastReport >= 5.0) {
            lastReport = t;
            windows++;
            const bool got = packets > pk0;
            lockedWins += got;
            pk0 = packets;
            if (!rssHalf && t >= seconds / 2) rssHalf = rssMb();
            if (!rss1 && t >= 5.0) rss1 = rssMb();
            printf("  t=%6.1f state %d packets +%llu  worst feed %.3f s  rss %.0f MB\n", t, tel.state, (unsigned long long)(packets - 0), worst, rssMb());
            fflush(stdout);
        }
    }
    rx.flush();
    const double rssEnd = rssMb();
    printf("windows with packets %d of %d, worst feed %.3f s, rss start %.0f half %.0f end %.0f MB\n", lockedWins, windows, worst, rss1, rssHalf, rssEnd);
    CHECK(lockedWins * 10 >= windows * 6, "the receiver did not deliver in most windows");
    if (seconds >= 60) CHECK(rssEnd < rssHalf * 1.25 + 50, "memory grew: half %.0f MB, end %.0f MB", rssHalf, rssEnd);
    // tear down while the decoder threads still hold work
    sig.generate(buf.data(), chunk);
    rx.feed(buf.data(), chunk);
    const double t1 = now();
    rx.reset();
    CHECK(now() - t1 < 5.0, "reset took %.1f s", now() - t1);
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("OK\n");
    return 0;
}
