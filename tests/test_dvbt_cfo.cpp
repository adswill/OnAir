// DVB-T with the radio off by whole carriers, for every guard interval. The receiver takes the whole-carrier part of the offset out by moving
// the carrier index, which leaves the phase of every carrier advancing by 2 pi m (N + G) / N from one symbol to the next: a quarter turn per
// carrier of offset with guard 1/4, an eighth with 1/8. The TPS bits are differential from symbol to symbol, so that turn has to come out of
// them, or the TPS never decodes and nothing else follows. The first case is the condition of a real capture (Hungary, 554 MHz) that did not
// decode: 8K, guard 1/4, 64-QAM 3/4, the radio 7.9 kHz off (7 carriers and a bit), recorded at 10 Msps.
#include "dect2/dvbt_gen.h"
#include "dect2/dvbt_rx.h"
#include "dect2/resampler.h"
#include "dect2/t2.h"
#include "jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <algorithm>
#include <cstdint>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Result { bool tps = false; size_t good = 0, bad = 0; RxTelemetry tel; };

// cfoCarriers: the radio's offset in subcarrier spacings; inRate: the rate the receiver is given (the native rate, or a radio's rate)
static Result run(const dvbt::Params& p, int frames, double cfoCarriers, double inRate, double snrDb) {
    const double fn = nativeRateHz(8);
    const int N = dvbt::fftN(p.mode);
    const double cfoHz = cfoCarriers * fn / N;
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    });
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0, 1);
    const float sigma = (float)(std::pow(10.0, -snrDb / 20.0) / std::sqrt(2.0));
    std::vector<cf32> sig, sym;
    for (int i = 0; i < 40000; i++) sig.push_back(cf32(0, 0));   // some lead-in before the signal
    for (int s = 0; s < frames * 68; s++) { gen.nextSymbol(sym); sig.insert(sig.end(), sym.begin(), sym.end()); }
    const double dph = 2 * M_PI * cfoHz / fn;
    for (size_t i = 0; i < sig.size(); i++) {
        const double a = dph * (double)i;
        sig[i] = sig[i] * cf32((float)std::cos(a), (float)std::sin(a)) + cf32(nd(rng), nd(rng)) * sigma;
    }
    std::vector<cf32> in;
    if (inRate != fn) { RationalResampler up; up.configure(fn, inRate); up.process(sig.data(), sig.size(), in); }
    else in.swap(sig);
    DvbtReceiver rx;
    rx.configure(inRate, 8);
    Result r;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            uint32_t c; memcpy(&c, q + 4, 4);
            bool ok = !(q[1] & 0x80) && q[0] == 0x47 && q[1] == 0x01 && q[2] == 0x00;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(c * 31 + j * 7);
            if (ok) r.good++; else if (r.good) r.bad++;
        }
    });
    for (size_t i = 0; i < in.size(); i += 1 << 14) rx.feed(in.data() + i, std::min<size_t>(1 << 14, in.size() - i));
    rx.telemetry(r.tel, 0);
    r.tps = r.tel.dvbt.tpsOk;
    return r;
}

int main() {
    struct C { const char* name; int mode, gi, mod, cr; double carriers, inRate; } cases[] = {
        // the real capture: 7.09 carriers (-7916 Hz) at 10 Msps, three quarters of a turn per symbol
        {"8K GI 1/4 64-QAM 3/4, -7.9 kHz, 10 Msps", dvbt::k8K, dvbt::kGi4, dvbt::k64Qam, dvbt::kR34, -7916.0 / (nativeRateHz(8) / 8192), 10e6},
        // a half turn: every TPS bit inverted
        {"8K GI 1/4 16-QAM 2/3, +2.1 carriers", dvbt::k8K, dvbt::kGi4, dvbt::k16Qam, dvbt::kR23, 2.1, 0},
        {"2K GI 1/4 16-QAM 2/3, +1.1 carriers", dvbt::k2K, dvbt::kGi4, dvbt::k16Qam, dvbt::kR23, 1.1, 0},
        // a quarter turn with the shorter guards
        {"8K GI 1/8 64-QAM 2/3, -1.9 carriers", dvbt::k8K, dvbt::kGi8, dvbt::k64Qam, dvbt::kR23, -1.9, 0},
        {"2K GI 1/16 QPSK 1/2, -3.9 carriers", dvbt::k2K, dvbt::kGi16, dvbt::kQpsk, dvbt::kR12, -3.9, 0},
        {"8K GI 1/32 64-QAM 3/4, +8.1 carriers", dvbt::k8K, dvbt::kGi32, dvbt::k64Qam, dvbt::kR34, 8.1, 0},
    };
    testjobs::Jobs jobs;
    for (auto& c : cases) {
        jobs.add([&c] {
            dvbt::Params p; p.mode = c.mode; p.guard = c.gi; p.mod = c.mod; p.crHp = c.cr; p.crLp = c.cr;
            const double rate = c.inRate > 0 ? c.inRate : nativeRateHz(8);
            const Result r = run(p, c.mode == dvbt::k8K ? 6 : 20, c.carriers, rate, 30);
            const bool ok = r.tps && r.tel.dvbt.mod == c.mod && r.tel.dvbt.crHp == c.cr && r.good > 50 && r.bad * 20 <= r.good + r.bad;
            jprintf("%-42s tps %d (mode %d gi %d mod %d cr %d)  CFO %.0f Hz  good %zu bad %zu  %s\n", c.name, r.tps, r.tel.dvbt.mode, r.tel.dvbt.guard,
                    r.tel.dvbt.mod, r.tel.dvbt.crHp, r.tel.cfoHz, r.good, r.bad, ok ? "OK" : "FAILED");
            CHECK(ok, "%s", c.name);
        });
    }
    jobs.run();
    jprintf(fails ? "DVB-T carrier offset tests FAILED\n" : "DVB-T carrier offset tests passed\n");
    return fails ? 1 : 0;
}
