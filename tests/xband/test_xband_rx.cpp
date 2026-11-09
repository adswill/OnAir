// The X-band links end to end on the built-in test signals. Simulated only. Impairments: carrier offset, drift, clock error, noise, phase ambiguity.
#include "dect2/xband/xband_gen.h"
#include "dect2/xband/xband_proto.h"
#include "dect2/xband/xband_rx.h"
#include "dect2/xband/xband_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case { int profile; double rate, secs, snr, cfo, drift, ppm; int amb; uint64_t minOk, minLines; };

static XbTelemetry run(const Case& k, std::string* extra = nullptr) {
    XbandGenConfig g; g.profile = k.profile; g.rate = k.rate; g.snrDb = k.snr; g.cfoHz = k.cfo; g.driftHzPerS = k.drift; g.clockPpm = k.ppm; g.ambiguity = k.amb; g.seed = 7;
    XbandGenerator gen(g);
    XbandReceiver rx(k.profile);
    rx.configure(k.rate);
    std::vector<cf32> buf(1 << 16);
    XbTelemetry t; uint64_t seq = 0;
    for (double done = 0; done < k.secs * k.rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    rx.finish();
    rx.telemetry(t, seq);
    printf("  %-26s snr %3.0f cfo %7.0f amb %d: state %d, %llu ok %llu bad, %llu pkts, %llu lines, %s\n", xbLink(k.profile).name, k.snr, k.cfo, k.amb, t.state, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.packets, (unsigned long long)t.lines, t.phase.c_str());
    printf("     cfo est %.0f, snr %.1f, lock %.2f\n", t.cfoHz, t.snrDb, t.carrierLock);
    if (extra) *extra = t.satName;
    return t;
}
static XbTelemetry expect(const Case& k) {
    XbTelemetry t = run(k);
    CHECK(t.blocksOk >= k.minOk, "%s: %llu frames", xbLink(k.profile).name, (unsigned long long)t.blocksOk);
    CHECK(t.lines >= k.minLines, "%s: %llu lines", xbLink(k.profile).name, (unsigned long long)t.lines);
    return t;
}
int main(int argc, char** argv) {
    int only = argc > 1 ? atoi(argv[1]) : -1;
    std::vector<Case> cs = {
        {kXbSaralL, 3e6, 0.25, 20, 12000, 0, 0, 0, 5, 0},
        {kXbSaralL, 3e6, 0.25, 8, -8000, 500, 20, 5, 5, 0},
        {kXbOceansat3, 3e6, 0.2, 12, 20000, 0, 0, 1, 3, 0},
        {kXbProbaS, 6e6, 0.2, 12, 15000, 0, 0, 0, 3, 0},
        {kXbLandsatS, 4e6, 0.5, 15, 5000, 0, 0, 2, 5, 0},
        {kXbNpp, 25e6, 0.3, 20, 50000, 0, 0, 3, 5, 0},
        {kXbJpss2, 40e6, 0.2, 20, -30000, 0, 0, 0, 5, 0},
        {kXbAquaAura, 15e6, 0.1, 25, 10000, 0, 0, 0, 5, 0},
        {kXbTerra, 45e6, 0.12, 20, 40000, 0, 0, 0, 5, 0}, {kXbTerra, 39.375e6, 0.12, 30, 0, 0, 0, 0, 5, 0},
        {kXbMetopDump, 45e6, 0.02, 25, 10000, 0, 0, 2, 5, 0},
        {kXbProbaVX, 45e6, 0.05, 20, 0, 0, 0, 0, 3, 0},
        {kXbGcomW1, 20e6, 0.1, 20, 5000, 0, 0, 0, 3, 0},
        {kXbSaralX, 25e6, 0.1, 20, 5000, 0, 0, 1, 3, 0},
        {kXbOceansat3X, 120e6, 0.02, 25, 0, 0, 0, 0, 3, 0},
        {kXbMetopSg, 140e6, 0.02, 20, 0, 0, 0, 1, 3, 0},
    };
    for (int p = 0; p < kXbNumProfiles; p++) if (xbLink(p).caduBytes > 0 && xbLink(p).fec != kXbFecLdpc && xbLink(p).fec != kXbFecTurbo && (only < 0 || only == p)) {
        // the CADU layer alone (what a .cadu file does): every frame must come through, with symbol errors within the Reed-Solomon power
        auto v = xbandTestCadus(p, 30);
        XbandReceiver rx(p);
        const int cb = xbLink(p).caduBytes;
        for (int i = 0; i < 30; i++) {
            std::vector<uint8_t> f(v.begin() + i * cb, v.begin() + (i + 1) * cb);
            if (xbLink(p).rsI > 0 && i % 3 == 0) f[(size_t)40 + i] ^= 0x5A;
            rx.pushCadu(f.data());
        }
        rx.finish();
        XbTelemetry t; rx.telemetry(t, 0);
        printf("  CADU layer %-26s %llu ok %llu bad, %llu fixed\n", xbLink(p).name, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.rsCorrected);
        CHECK(t.blocksOk == 30 && t.blocksBad == 0, "cadu layer %s", xbLink(p).name);
    }
    for (auto& k : cs) if (only < 0 || k.profile == only) expect(k);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
