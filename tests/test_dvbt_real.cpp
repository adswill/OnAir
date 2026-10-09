// DVB-T against real-world faults (REAL_WORLD_CHECKLIST.md), applied with tests/impair.h, which was written apart from the receiver:
// a UHF radio off by up to 50 ppm (+-43 kHz at 860 MHz, far more than 20 carriers in 8K and in narrow channels), a sample clock 80 ppm
// fast, hierarchical transmissions (the receiver gives the high-priority stream), signalling with a reserved LP code rate in a
// non-hierarchical multiplex, and everything at once with an echo and an overdriven 8-bit radio.
#include "dect2/dvbt_gen.h"
#include "dect2/dvbt_rx.h"
#include "dect2/resampler.h"
#include "dect2/t2.h"
#include "impair.h"
#include "jobs.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Case {
    const char* name;
    int mode, gi, mod, cr, hier = 0, crLp = -1;
    double bw = 8, offHz = 0, ppm = 0, inRate = 0;
    bool echo = false, clip = false;
    int frames = 0;
};

struct Result { bool tps = false; size_t good = 0, bad = 0; RxTelemetry tel; };

static Result run(const Case& c) {
    dvbt::Params p; p.mode = c.mode; p.guard = c.gi; p.mod = c.mod; p.crHp = c.cr; p.hier = c.hier;
    p.crLp = c.crLp >= 0 ? c.crLp : c.cr;
    const double fn = nativeRateHz(c.bw);
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    });
    const int frames = c.frames ? c.frames : (c.mode == dvbt::k8K ? 6 : 20);
    std::vector<cf32> sig(30000, cf32(0, 0)), sym;
    for (int s = 0; s < frames * 68; s++) { gen.nextSymbol(sym); sig.insert(sig.end(), sym.begin(), sym.end()); }
    if (c.echo) impair::echo(sig, dvbt::guardSamples(c.mode, c.gi) / 3, -6.0, 2.0);
    if (c.ppm != 0) sig = impair::clock(sig, c.ppm);
    impair::shift(sig, c.offHz, fn);
    impair::noise(sig, 32.0, 3);
    if (c.clip) impair::clip8(sig, 0.6);   // rms 0.6 of full scale: the peaks clip
    std::vector<cf32> in;
    const double rate = c.inRate > 0 ? c.inRate : fn;
    if (rate != fn) { RationalResampler up; up.configure(fn, rate); up.process(sig.data(), sig.size(), in); }
    else in.swap(sig);
    DvbtReceiver rx;
    rx.configure(rate, c.bw);
    Result r;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            uint32_t v; memcpy(&v, q + 4, 4);
            bool ok = !(q[1] & 0x80) && q[0] == 0x47 && q[1] == 0x01 && q[2] == 0x00;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(v * 31 + j * 7);
            if (ok) r.good++; else if (r.good) r.bad++;
        }
    });
    for (size_t i = 0; i < in.size(); i += 1 << 14) rx.feed(in.data() + i, std::min<size_t>(1 << 14, in.size() - i));
    rx.telemetry(r.tel, 0);
    r.tps = r.tel.dvbt.tpsOk;
    return r;
}

int main() {
    using namespace dvbt;
    std::vector<Case> cases;
    auto add = [&](Case c) { cases.push_back(c); };
    // 50 ppm at the top of UHF: 43 kHz, 38 carriers in 8K at 8 MHz, 62 in 8K at 5 MHz
    { Case c{"8K GI 1/4 64-QAM 3/4, +43 kHz", k8K, kGi4, k64Qam, kR34}; c.offHz = 43e3; add(c); }
    { Case c{"8K GI 1/8 16-QAM 2/3, -43 kHz, 10 Msps", k8K, kGi8, k16Qam, kR23}; c.offHz = -43e3; c.inRate = 10e6; add(c); }
    { Case c{"8K GI 1/32 16-QAM 1/2, 5 MHz, +43 kHz", k8K, kGi32, k16Qam, kR12}; c.bw = 5; c.offHz = 43e3; add(c); }
    { Case c{"2K GI 1/4 QPSK 2/3, 6 MHz, -43 kHz", k2K, kGi4, kQpsk, kR23}; c.bw = 6; c.offHz = -43e3; add(c); }
    { Case c{"8K GI 1/16 64-QAM 2/3, 7 MHz, +31 kHz", k8K, kGi16, k64Qam, kR23}; c.bw = 7; c.offHz = 31e3; add(c); }
    // a sample clock 80 ppm off: 0.65 samples of drift per 8K symbol, a third of a carrier of spread at the band edges
    { Case c{"8K GI 1/4 64-QAM 3/4, clock +80 ppm", k8K, kGi4, k64Qam, kR34}; c.ppm = 80; c.frames = 10; add(c); }
    { Case c{"8K GI 1/8 64-QAM 5/6, clock -80 ppm", k8K, kGi8, k64Qam, kR56}; c.ppm = -80; c.frames = 10; add(c); }
    // hierarchical: the high-priority stream
    { Case c{"8K GI 1/4 16-QAM alpha 1, HP 1/2", k8K, kGi4, k16Qam, kR12}; c.hier = 1; c.crLp = kR34; add(c); }
    { Case c{"2K GI 1/8 64-QAM alpha 2, HP 2/3", k2K, kGi8, k64Qam, kR23}; c.hier = 2; c.crLp = kR56; add(c); }
    { Case c{"8K GI 1/32 64-QAM alpha 4, HP 3/4", k8K, kGi32, k64Qam, kR34}; c.hier = 3; c.crLp = kR78; add(c); }
    // non-hierarchical with a reserved value in the (unused) LP code rate field
    { Case c{"2K GI 1/4 16-QAM 2/3, LP rate field 7", k2K, kGi4, k16Qam, kR23}; c.crLp = 7; add(c); }
    // everything at once: the worst UHF offset, a fast clock, an echo inside the guard and an overdriven 8-bit radio, at 10 Msps
    { Case c{"8K GI 1/4 64-QAM 2/3, +43 kHz +80 ppm echo clip8", k8K, kGi4, k64Qam, kR23}; c.offHz = 43e3; c.ppm = 80; c.echo = true; c.clip = true;
      c.inRate = 10e6; c.frames = 10; add(c); }
    testjobs::Jobs jobs;
    for (auto& c : cases) {
        jobs.add([&c] {
            const Result r = run(c);
            const bool ok = r.tps && r.tel.dvbt.mod == c.mod && r.tel.dvbt.crHp == c.cr && r.tel.dvbt.hier == c.hier && r.good > 50 &&
                            r.bad * 50 <= r.good + r.bad;
            jprintf("%-50s tps %d  CFO %7.0f Hz  good %5zu bad %4zu  %s\n", c.name, r.tps, r.tel.cfoHz, r.good, r.bad, ok ? "OK" : "FAILED");
            CHECK(ok, "%s", c.name);
        });
    }
    jobs.run();
    jprintf(fails ? "DVB-T real-world tests FAILED\n" : "DVB-T real-world tests passed\n");
    return fails ? 1 : 0;
}
