// DVB-S/S2 receiver against real-world faults (tests/impair.h, REAL_WORLD_CHECKLIST.md): the generator's signal is damaged independently of the
// receiver, then fed through the real receiver, and the checkable test packets are counted.
#include "dect2/dvbs_testkit.h"
#include "impair.h"
#include "jobs.h"
#include <atomic>
#include <cstdio>
#include <functional>

using namespace dect2;
using namespace dect2::dvbs;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: "); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Out { uint64_t good = 0, bad = 0; double first = -1; DvbsTelemetry tel; };

static Out run(DvbsSignalConfig c, double secs, const std::function<void(std::vector<cf32>&)>& fault) {
    uint64_t pn = 0;
    c.ts = [&pn](uint8_t* p) { testPacket(pn++, p); };
    DvbsSignal sig(c);
    std::vector<cf32> x((size_t)(secs * c.sampleRate));
    sig.generate(x.data(), x.size());
    if (fault) fault(x);
    Out o;
    DvbsReceiver rx;
    rx.setBlocking(true);
    rx.configure(c.sampleRate);
    double t = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++, p += 188) {
            const uint64_t idx = ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | p[7];
            uint8_t ex[188];
            testPacket(idx, ex);
            if ((p[1] & 0x80) || memcmp(p, ex, 188) != 0) { o.bad++; continue; }
            if (o.first < 0) o.first = t;
            o.good++;
        }
    });
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 65536) {
        const size_t m = std::min<size_t>(65536, x.size() - i);
        rx.feed(x.data() + i, m);
        t = (double)(i + m) / c.sampleRate;
        DvbsTelemetry u;
        if (rx.telemetry(u, seq)) { seq = u.seq; o.tel = u; }
    }
    rx.flush();
    DvbsTelemetry u;
    if (rx.telemetry(u, 0)) o.tel = u;
    return o;
}

static DvbsSignalConfig sigOf(int standard, int mod, int rate, double rs, double fs, double snr) {
    DvbsSignalConfig c;
    c.tx.standard = standard; c.tx.mod = mod; c.tx.rate = rate; c.tx.symbolRate = rs;
    c.sampleRate = fs; c.snrDb = snr;
    return c;
}

// at least `frac` of the packets sent after `lock` seconds, and none damaged
static void good(const Out& o, const DvbsSignalConfig& c, double secs, double lock, double frac, const char* what) {
    const double want = dvbsNetBitrate(c.tx) / 1504.0 * (secs - lock) * frac;
    jprintf("%-44s %6llu good %llu bad, first %.2f s; %s\n", what, (unsigned long long)o.good, (unsigned long long)o.bad, o.first, dvbsSummary(o.tel).c_str());
    CHECK((double)o.good >= want, "%s: %llu good packets (wanted %.0f)", what, (unsigned long long)o.good, want);
    CHECK(o.bad == 0, "%s: %llu damaged packets", what, (unsigned long long)o.bad);
}

int main() {
    testjobs::Jobs jobs;
    // combined, per standard: the LNB 3 MHz off (its drift and tolerance), the radio's clock +80 ppm, an echo (-12 dB, a cable mismatch) and
    // an overdriven 8-bit radio
    struct C { int std, mod, rate; const char* name; };
    for (C cs : {C{2, k8psk, 6, "DVB-S2 8PSK"}, C{1, kQpsk, 2, "DVB-S QPSK"}}) for (double off : {-3e6, 3e6}) jobs.add([cs, off] {
        const double fs = 10e6, secs = 3;
        DvbsSignalConfig c = sigOf(cs.std, cs.mod, cs.rate, 2e6, fs, 16);
        Out o = run(c, secs, [&](std::vector<cf32>& x) {
            impair::shift(x, off, fs); x = impair::clock(x, 80); impair::echo(x, 3, -12, 1.0); impair::clip8(x, 4); });
        char w[96]; snprintf(w, sizeof w, "%s combined %+.0f MHz", cs.name, off / 1e6);
        good(o, c, secs, 1.2, 0.8, w);
        CHECK(std::fabs(o.tel.cfoHz - off * (1 + 80e-6)) < 20e3, "%s: carrier reported at %+.1f kHz", w, o.tel.cfoHz / 1e3);
    });
    // swapped I and Q (a mirrored spectrum)
    for (C cs : {C{2, kQpsk, 6, "DVB-S2 QPSK"}, C{1, kQpsk, 2, "DVB-S QPSK"}}) jobs.add([cs] {
        DvbsSignalConfig c = sigOf(cs.std, cs.mod, cs.rate, 2e6, 4e6, 14);
        Out o = run(c, 3, [](std::vector<cf32>& x) { impair::swapIq(x); impair::shift(x, 300e3, 4e6); });
        char w[96]; snprintf(w, sizeof w, "%s I/Q swapped", cs.name);
        good(o, c, 3, 1.2, 0.8, w);
    });
    // a file that starts in the middle of a frame, a USB drop, NaN samples
    for (C cs : {C{2, k8psk, 6, "DVB-S2 8PSK"}, C{1, kQpsk, 2, "DVB-S QPSK"}}) jobs.add([cs] {
        DvbsSignalConfig c = sigOf(cs.std, cs.mod, cs.rate, 2e6, 4e6, 14);
        Out o = run(c, 4, [](std::vector<cf32>& x) {
            impair::skip(x, 123457); impair::drop(x, 6000000, 33333);
            for (size_t i = 8000000; i < 8000100; i++) x[i] = cf32(NAN, NAN); });
        char w[96]; snprintf(w, sizeof w, "%s skip, drop, NaN", cs.name);
        jprintf("%-44s %6llu good %llu bad\n", w, (unsigned long long)o.good, (unsigned long long)o.bad);
        const double want = dvbsNetBitrate(c.tx) / 1504.0 * 4 * 0.5;
        CHECK((double)o.good >= want, "%s: %llu good packets (wanted %.0f)", w, (unsigned long long)o.good, want);
        CHECK(o.tel.state == 2, "%s: not locked at the end; %s", w, dvbsSummary(o.tel).c_str());
    });
    jobs.run();
    jprintf(fails ? "dvbs impair: FAILED (%d)\n" : "dvbs impair: ok\n", fails.load());
    return fails ? 1 : 0;
}
