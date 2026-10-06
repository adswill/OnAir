// End-to-end DVB-T: generator -> channel impairments -> receiver, checking lock, TPS and transport stream recovery.
#include "dect2/dvbt_gen.h"
#include "dect2/dvbt_rx.h"
#include "dect2/t2.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

struct Result {
    bool locked = false, tps = false; size_t packets = 0, good = 0, bad = 0; double snr = 0, secs = 0; int level = 0; RxTelemetry tel;
    double maxCfoErr = 0;   // worst distance of the reported carrier offset from the true one, in Hz, once locked
    size_t badLate = 0;     // bad packets that came out more than 100 ms after a disturbance had ended
    size_t lost = 0;        // packets missing or damaged after the first good one (counter gaps plus bad packets)
};

// burstMs > 0: a burst of noise at burstSnrDb (against the signal) starting burstAtSec into the signal, to see how the receiver rides out a disturbance
static Result run(const dvbt::Params& p, int frames, double snrDb, double cfoHz, double echoDb, int echoDelay, double sroPpm, double bwMhz = 8,
                  double burstMs = 0, double burstSnrDb = 0, double burstAtSec = 0, int slipSamples = 0) {
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
    size_t fed = 0;   // samples given to the receiver so far (the packet callback runs inside feed())
    uint32_t lastCounter = 0;
    const size_t burstEnd = burstMs > 0 ? (size_t)((burstAtSec + burstMs / 1e3) * fn) + 40000 : 0;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            r.packets++;
            const bool late = burstEnd && fed > burstEnd + (size_t)(0.1 * fn);
            if (q[1] & 0x80) { if (r.good) { r.bad++; if (late) r.badLate++; } continue; }
            uint32_t c; memcpy(&c, q + 4, 4);
            bool ok = q[0] == 0x47 && q[1] == 0x01 && q[2] == 0x00;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(c * 31 + j * 7);
            if (ok) {
                if (r.good && c > lastCounter + 1) r.lost += c - lastCounter - 1;
                lastCounter = c; r.good++;
            } else if (r.good) { r.bad++; if (late) r.badLate++; }
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
    for (int i = 0; i < 40000; i++) outv.push_back(cf32(nd(rng), nd(rng)) * (float)sigma);
    double pos = 1;
    bool slipped = false;
    const double step = 1.0 + sroPpm * 1e-6;
    while ((size_t)pos + 20 < tx.size()) {
        cf32 v = sroPpm == 0 ? tx[(size_t)pos] : sincSample(tx, pos);
        pos += step;
        if (echoDb > 0) { const cf32 e = delay[dpos % delay.size()]; delay[dpos % delay.size()] = v; v += e * (float)std::pow(10.0, -echoDb / 20.0); dpos++; }
        phase += dph;
        v *= cf32((float)std::cos(phase), (float)std::sin(phase));
        v += cf32(nd(rng), nd(rng)) * (float)sigma;
        if (slipSamples > 0 && !slipped && (double)(outv.size() - 40000) / fn >= burstAtSec) { slipped = true; pos += slipSamples; continue; }
        if (burstMs > 0) {
            const double tNow = (double)(outv.size() - 40000) / fn;
            if (tNow >= burstAtSec && tNow < burstAtSec + burstMs / 1e3) v += cf32(nd(rng), nd(rng)) * (float)(std::pow(10.0, -burstSnrDb / 20.0) / std::sqrt(2.0));
        }
        outv.push_back(v);
    }
    auto t0 = std::chrono::steady_clock::now();
    int chunkNo = 0;
    for (size_t i = 0; i < outv.size(); i += 1 << 14) {
        const size_t n = std::min<size_t>(1 << 14, outv.size() - i);
        fed = i + n;
        rx.feed(outv.data() + i, n);
        if (++chunkNo % 4 == 0 && fed > (size_t)(0.3 * fn)) { RxTelemetry t; rx.telemetry(t, 0); if (t.state >= 1 && t.dvbt.tpsOk) r.maxCfoErr = std::max(r.maxCfoErr, std::fabs(t.cfoHz - cfoHz)); }
    }
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rx.telemetry(r.tel, 0);
    r.locked = r.tel.state >= 1; r.tps = r.tel.dvbt.tpsOk; r.snr = r.tel.dataSnrDb; r.level = rx.detectLevel();
    return r;
}

int main() {
    struct C { int mode, gi, mod, cr; double snr, cfo, echo; int echoDelay; double sro; const char* name; int frames = 0; double burstMs = 0, burstSnr = 0, burstAt = 0; int slip = 0; } cases[] = {
        {dvbt::k2K, dvbt::kGi8, dvbt::kQpsk, dvbt::kR12, 30, 0, 0, 0, 0, "2K QPSK 1/2 clean"},
        {dvbt::k2K, dvbt::kGi32, dvbt::k64Qam, dvbt::kR34, 35, 0, 0, 0, 0, "2K 64-QAM 3/4 GI 1/32 clean"},
        {dvbt::k8K, dvbt::kGi8, dvbt::k16Qam, dvbt::kR23, 30, 0, 0, 0, 0, "8K 16-QAM 2/3 GI 1/8 clean"},
        {dvbt::k2K, dvbt::kGi8, dvbt::k16Qam, dvbt::kR23, 28, 3200, 0, 0, 0, "2K 16-QAM + CFO 3.2 kHz"},
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 1800, 0, 0, 0, "8K 64-QAM + CFO 1.8 kHz"},
        {dvbt::k2K, dvbt::kGi4, dvbt::k16Qam, dvbt::kR23, 30, 0, 8, 60, 0, "2K 16-QAM echo -8 dB at 60 samples"},
        {dvbt::k8K, dvbt::kGi8, dvbt::k64Qam, dvbt::kR23, 32, 0, 0, 0, 20, "8K 64-QAM + sample-rate offset 20 ppm"},
        // long enough for a slow timing loop to fall behind a cheap radio's clock, with the shortest guard to fall out of
        {dvbt::k8K, dvbt::kGi32, dvbt::k64Qam, dvbt::kR23, 32, 0, 0, 0, 20, "8K 64-QAM GI 1/32 + 20 ppm, 40 frames", 40},
        {dvbt::k8K, dvbt::kGi32, dvbt::k64Qam, dvbt::kR23, 32, 0, 0, 0, -20, "8K 64-QAM GI 1/32 - 20 ppm, 40 frames", 40},
        // a disturbance in the middle of a clean signal: the carrier tracker must not wander, and the stream must be back soon after
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4 + CFO 800 Hz, 200 ms noise burst", 50, 200, -5, 1.2},
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4 + CFO 800 Hz, 600 ms noise burst", 70, 600, -5, 1.2},
        // a chunk of samples lost from a clean signal (a radio that drops samples): the symbol timing is suddenly wrong and has to be found again
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4, 40 samples lost", 50, 0, 0, 1.2, 40},
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4, 700 samples lost", 50, 0, 0, 1.2, 700},
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4, 3000 samples lost", 50, 0, 0, 1.2, 3000},
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4, 5000 samples lost", 50, 0, 0, 1.2, 5000},
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4, 9400 samples lost", 50, 0, 0, 1.2, 9400},
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4, 20000 samples lost", 50, 0, 0, 1.2, 20000},
        // a whole radio buffer (13 symbols here): more than the pilot check can resolve, the frame position has to be found again from the TPS bits
        {dvbt::k8K, dvbt::kGi16, dvbt::k64Qam, dvbt::kR34, 30, 800, 0, 0, 0, "8K 64-QAM 3/4, 120000 samples lost", 80, 0, 0, 1.2, 120000},
    };
    const char* only = getenv("DVBT_CASE");   // development: run only the cases whose name contains this text
    for (auto& c : cases) {
        if (only && !strstr(c.name, only)) continue;
        dvbt::Params p; p.mode = c.mode; p.guard = c.gi; p.mod = c.mod; p.crHp = c.cr; p.crLp = c.cr;
        const Result r = run(p, c.frames ? c.frames : c.mode == dvbt::k8K ? 8 : 20, c.snr, c.cfo, c.echo, c.echoDelay, c.sro, 8, c.burstMs, c.burstSnr, c.burstAt, c.slip);
        const bool ok = r.tps && r.good > 50 && r.bad * 20 <= r.good + r.bad;
        printf("%-42s lock %d tps %d (mode %d gi %d mod %d cr %d)  packets %zu good %zu bad %zu  SNR %.1f dB  %.2fs  %s\n", c.name, r.locked, r.tps, r.tel.dvbt.mode, r.tel.dvbt.guard, r.tel.dvbt.mod, r.tel.dvbt.crHp, r.packets, r.good, r.bad, r.snr, r.secs, ok ? "OK" : "FAILED");
        if (c.slip > 0) printf("    %d samples lost: %zu packets lost or damaged in all, worst carrier-offset error %.0f Hz\n", c.slip, r.lost + r.bad, r.maxCfoErr);
        if (c.burstMs > 0) printf("    disturbance: worst carrier-offset error %.0f Hz (limit 60), bad packets long after the burst %zu\n", r.maxCfoErr, r.badLate);
        CHECK(ok, "%s", c.name);
        // Lost samples must cost about a block of symbols, not the quarter of a second it took when the receiver waited for three failed TPS frames
        if (c.slip > 0) CHECK(r.lost + r.bad <= (c.slip >= 100000 ? 3500 : 1500), "%s: %zu packets lost or damaged", c.name, r.lost + r.bad);
        if (c.burstMs > 0) CHECK(r.maxCfoErr < 60 && r.badLate == 0, "%s: carrier offset error %.0f Hz, %zu bad packets late", c.name, r.maxCfoErr, r.badLate);
    }
    printf(fails ? "DVB-T receiver tests FAILED\n" : "DVB-T receiver tests passed\n");
    return fails ? 1 : 0;
}
