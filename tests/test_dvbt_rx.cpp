// End-to-end DVB-T: generator -> channel impairments -> receiver, checking lock, TPS and transport stream recovery.
#include "dect2/dvbt_gen.h"
#include "dect2/dvbt_rx.h"
#include "dect2/t2.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>
#include <cstdint>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)


// fractional-delay sample from a stream using a 32-tap Kaiser-windowed sinc (a clean resampler for the SRO simulation)
static cf32 sincSample(const std::vector<cf32>& x, double pos) {
    const int T = 16;
    const long i0 = (long)std::floor(pos);
    const double f = pos - i0;
    cf32 acc(0, 0);
    for (int t = -T + 1; t <= T; t++) {
        const long idx = i0 + t;
        if (idx < 0 || idx >= (long)x.size()) continue;
        const double d = t - f;
        const double s = std::fabs(d) < 1e-9 ? 1.0 : std::sin(M_PI * d) / (M_PI * d);
        const double w = 0.5 + 0.5 * std::cos(M_PI * d / T); // Hann window over the kernel
        acc += x[idx] * (float)(s * w);
    }
    return acc;
}

struct Result { bool locked = false, tps = false; size_t packets = 0, good = 0, bad = 0; double snr = 0, secs = 0; int level = 0; RxTelemetry tel; };

static Result run(const dvbt::Params& p, int frames, double snrDb, double cfoHz, double echoDb, int echoDelay, double sroPpm, double bwMhz = 8, double dcRel = 0) {
    const double fn = nativeRateHz(bwMhz);
    // transmit: packets carry a counter so the receiver output can be checked
    uint32_t counter = 0;
    std::vector<std::vector<uint8_t>> sent;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    });
    DvbtReceiver rx;
    rx.configure(fn, bwMhz);
    Result r;
    uint32_t expectNext = 0; bool started = false;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            r.packets++;
            if (q[1] & 0x80) { if (r.good) r.bad++; continue; }
            uint32_t c; memcpy(&c, q + 4, 4);
            bool ok = q[0] == 0x47 && q[1] == 0x01 && q[2] == 0x00;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(c * 31 + j * 7);
            if (ok) r.good++; else if (r.good) r.bad++;
            (void)expectNext; (void)started;
        }
    });
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0, 1);
    const double sigma = std::pow(10.0, -snrDb / 20.0) / std::sqrt(2.0);
    std::vector<cf32> sym, delay((size_t)std::max(1, echoDelay), cf32(0, 0));
    size_t dpos = 0;
    double phase = 0;
    const double dph = 2 * M_PI * cfoHz / fn;
    // sample-rate offset: resample the transmit stream by (1 + sro) with linear interpolation
    std::vector<cf32> tx;
    const int symbolsTotal = frames * 68;
    for (int s = 0; s < symbolsTotal; s++) { gen.nextSymbol(sym); tx.insert(tx.end(), sym.begin(), sym.end()); }
    // some lead-in of noise
    std::vector<cf32> outv;
    double pw = 0;
    for (const cf32& v : tx) pw += std::norm(v);
    const cf32 dc = cf32(1.f, -0.5f) * (float)(dcRel * std::sqrt(pw / std::max<size_t>(1, tx.size())));   // the radio's LO leakage
    for (int i = 0; i < 40000; i++) outv.push_back(cf32(nd(rng), nd(rng)) * (float)sigma);
    double pos = 1;
    const double step = 1.0 + sroPpm * 1e-6;
    while ((size_t)pos + 20 < tx.size()) {
        cf32 v = sroPpm == 0 ? tx[(size_t)pos] : sincSample(tx, pos);
        pos += step;
        if (echoDb > 0) { const cf32 e = delay[dpos % delay.size()]; delay[dpos % delay.size()] = v; v += e * (float)std::pow(10.0, -echoDb / 20.0); dpos++; }
        phase += dph;
        v *= cf32((float)std::cos(phase), (float)std::sin(phase));
        v += cf32(nd(rng), nd(rng)) * (float)sigma + dc;
        outv.push_back(v);
    }
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < outv.size(); i += 1 << 14) rx.feed(outv.data() + i, std::min<size_t>(1 << 14, outv.size() - i));
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rx.telemetry(r.tel, 0);
    r.locked = r.tel.state >= 1; r.tps = r.tel.dvbt.tpsOk; r.snr = r.tel.dataSnrDb; r.level = rx.detectLevel();
    return r;
}

int main() {
    struct C { int mode, gi, mod, cr; double snr, cfo, echo; int echoDelay; double sro; const char* name; } cases[] = {
        {dvbt::k2K, dvbt::kGi8, dvbt::kQpsk, dvbt::kR12, 30, 0, 0, 0, 0, "2K QPSK 1/2 clean"},
        {dvbt::k2K, dvbt::kGi32, dvbt::k64Qam, dvbt::kR34, 35, 0, 0, 0, 0, "2K 64-QAM 3/4 GI 1/32 clean"},
        {dvbt::k8K, dvbt::kGi8, dvbt::k16Qam, dvbt::kR23, 30, 0, 0, 0, 0, "8K 16-QAM 2/3 GI 1/8 clean"},
        {dvbt::k2K, dvbt::kGi8, dvbt::k16Qam, dvbt::kR23, 28, 3200, 0, 0, 0, "2K 16-QAM + CFO 3.2 kHz"},
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 1800, 0, 0, 0, "8K 64-QAM + CFO 1.8 kHz"},
        {dvbt::k2K, dvbt::kGi4, dvbt::k16Qam, dvbt::kR23, 30, 0, 8, 60, 0, "2K 16-QAM echo -8 dB at 60 samples"},
        {dvbt::k8K, dvbt::kGi8, dvbt::k64Qam, dvbt::kR23, 32, 0, 0, 0, 20, "8K 64-QAM + sample-rate offset 20 ppm"},
    };
    for (auto& c : cases) {
        dvbt::Params p; p.mode = c.mode; p.guard = c.gi; p.mod = c.mod; p.crHp = c.cr; p.crLp = c.cr;
        const Result r = run(p, c.mode == dvbt::k8K ? 8 : 20, c.snr, c.cfo, c.echo, c.echoDelay, c.sro);
        const bool ok = r.tps && r.good > 50 && r.bad * 20 <= r.good + r.bad;
        printf("%-42s lock %d tps %d (mode %d gi %d mod %d cr %d)  packets %zu good %zu bad %zu  SNR %.1f dB  %.2fs  %s\n", c.name, r.locked, r.tps, r.tel.dvbt.mode, r.tel.dvbt.guard, r.tel.dvbt.mod, r.tel.dvbt.crHp, r.packets, r.good, r.bad, r.snr, r.secs, ok ? "OK" : "FAILED");
        CHECK(ok, "%s", c.name);
    }
    {   // a HackRF's DC spike sits on the centre carrier, a continual pilot in 8K mode (Australian 7 MHz channel, off by -150 Hz)
        dvbt::Params p; p.mode = dvbt::k8K; p.guard = dvbt::kGi16; p.mod = dvbt::k64Qam; p.crHp = p.crLp = dvbt::kR34;
        const Result r = run(p, 8, 30, -150, 0, 0, 0, 7, 0.1);
        const bool ok = r.tps && r.good > 50 && r.bad * 20 <= r.good + r.bad;
        printf("%-42s lock %d tps %d  packets %zu good %zu bad %zu  SNR %.1f dB  %.2fs  %s\n", "8K 64-QAM 7 MHz, DC spike, CFO -150 Hz", r.locked, r.tps, r.packets, r.good, r.bad, r.snr, r.secs, ok ? "OK" : "FAILED");
        CHECK(ok, "DC spike on the centre pilot");
    }
    printf(fails ? "DVB-T receiver tests FAILED\n" : "DVB-T receiver tests passed\n");
    return fails ? 1 : 0;
}
