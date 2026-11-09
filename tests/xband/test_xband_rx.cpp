// The X-band links end to end on the built-in test signals. Simulated only. Impairments: carrier offset, drift, clock error, noise, phase ambiguity.
#include "dect2/xband/xband_gen.h"
#include "dect2/xband/xband_instr.h"
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
// pixels of a plane against the test picture; expect(x, y) returns the 16-bit value or -1 to skip
template<class F> static void pixels(XbandReceiver& rx, int plane, const char* what, F expect, int minLines) {
    int w = 0, h = 0; std::vector<uint16_t> px;
    CHECK(rx.readPlane(plane, w, h, px), "%s: no plane %d", what, plane);
    int good = 0, seen = 0;
    for (int y = 0; y < h; y += 3) for (int x = 0; x < w; x += 7) {
        const int e = expect(x, y);
        if (e < 0) continue;
        seen++; good += std::abs((int)px[(size_t)y * w + x] - e) <= 16;
    }
    printf("    %s plane %d: %d lines, %d of %d pixels right\n", what, plane, h, good, seen);
    CHECK(h >= minLines, "%s plane %d lines %d", what, plane, h);
    CHECK(seen > 0 && good > seen * 99 / 100, "%s plane %d pixels %d of %d", what, plane, good, seen);
}
static XbTelemetry viaCadu(XbandReceiver& rx, int p, int n) {
    auto v = xbandTestCadus(p, n);
    const int cb = xbLink(p).caduBytes;
    for (int i = 0; i < n; i++) rx.pushCadu(&v[(size_t)i * cb]);
    rx.finish();
    XbTelemetry t; rx.telemetry(t, 0);
    return t;
}
static void instruments() {
    { XbandReceiver rx(kXbNpp); XbTelemetry t = viaCadu(rx, kXbNpp, 40000);
      printf("  VIIRS: %llu frames, %llu packets, %llu lines, %s\n", (unsigned long long)t.blocksOk, (unsigned long long)t.packets, (unsigned long long)t.lines, t.satName.c_str());
      CHECK(t.satName == "NOAA-20", "satellite name");
      for (int c : {3, 0, 4, 9, 7, 13, 16, 17, 20}) {
          int a, zh, ref, sc, dec; const int* zw, *os; xband::viirsChannelInfo(c, a, zh, zw, os, ref, sc, dec);
          pixels(rx, c, "VIIRS", [&](int x, int y) { return std::min<int>((xbTestPixel(c, x, y) * 2 & 0x3FFF) * sc, 65535); }, zh);
      }
      auto prods = rx.products(800);
      printf("    VIIRS products: %zu\n", prods.size()); CHECK(prods.size() >= 2, "VIIRS products"); }
    { XbandReceiver rx(kXbAquaAura); viaCadu(rx, kXbAquaAura, 7000);
      for (int c : {0, 1, 2, 6, 7, 30, 37}) {
          const int sub = c < 2 ? 4 : c < 7 ? 2 : 1;
          pixels(rx, c, "MODIS", [&](int x, int y) { return y < 10 * sub ? (int)xbTestPixel(c, x, y) << 4 : -1; }, 10 * sub);
      }
      CHECK(!rx.products(800).empty(), "MODIS products"); }
    { XbandReceiver rx(kXbMetopDump); XbTelemetry t = viaCadu(rx, kXbMetopDump, 400);
      CHECK(t.satName == "MetOp-B", "metop name");
      for (int c = 0; c < 5; c++) pixels(rx, c, "AVHRR", [&](int x, int y) { return (int)(xbTestPixel(c, x, y) >> 2) << 6; }, 10); }
    { XbandReceiver rx(kXbGcomW1); viaCadu(rx, kXbGcomW1, 400);
      for (int c : {0, 7, 19}) pixels(rx, c, "AMSR2", [&](int x, int y) { return x < 243 && y < 20 ? (int)xbTestPixel(c, x, y) << 4 : -1; }, 8); }
    { XbandReceiver rx(kXbLandsatX); viaCadu(rx, kXbLandsatX, 400);
      pixels(rx, 0, "TIRS", [&](int x, int y) { return (int)xbTestPixel(0, x, y) << 4; }, 8); }
    { XbandReceiver rx(kXbProbaS); viaCadu(rx, kXbProbaS, 300);
      pixels(rx, 0, "VEGETATION", [&](int x, int y) { return (int)xbTestPixel(0, x, y) << 4; }, 8); }
    { XbandReceiver rx(kXbMetopSg); viaCadu(rx, kXbMetopSg, 12000);
      for (int c : {0, 7, 13}) pixels(rx, c, "METimage", [&](int x, int y) { return (int)xbTestPixel(c, x, y) << 4; }, 24); }
    { XbandGenConfig g; g.profile = kXbOceansat2; g.rate = 35e6; g.snrDb = 20; g.cfoHz = 8000; XbandGenerator gen(g); XbandReceiver rx(kXbOceansat2); rx.configure(35e6);
      std::vector<cf32> b(1 << 16); for (int i = 0; i < 100; i++) { gen.generate(b.data(), b.size()); rx.feed(b.data(), b.size()); } rx.finish();
      XbTelemetry t; rx.telemetry(t, 0); printf("  Oceansat-2: %llu frames, %llu lines\n", (unsigned long long)t.blocksOk, (unsigned long long)t.lines);
      CHECK(t.lines >= 3, "oceansat lines");
      // the first frame goes by while the demodulator settles: the rows start at signal line 1
      pixels(rx, 2, "OCM", [&](int x, int y) { return y >= 0 ? (int)xbTestPixel(2, x, y + 1) << 4 : -1; }, 3); }
    { XbandGenConfig g; g.profile = kXbKanopus; g.rate = 90e6; g.snrDb = 22; g.cfoHz = 5000; XbandGenerator gen(g); XbandReceiver rx(kXbKanopus); rx.configure(90e6);
      std::vector<cf32> b(1 << 16); for (int i = 0; i < 700; i++) { gen.generate(b.data(), b.size()); rx.feed(b.data(), b.size()); } rx.finish();
      XbTelemetry t; rx.telemetry(t, 0); printf("  Kanopus-V: %llu frames, %llu lines, state %d\n", (unsigned long long)t.blocksOk, (unsigned long long)t.lines, t.state);
      CHECK(t.blocksOk > 50, "kanopus frames"); CHECK(t.lines > 400, "kanopus lines"); }
    // DMSP on the air: no CADU path
    { XbandGenConfig g; g.profile = kXbDmsp; g.rate = 2e6; g.snrDb = 16; g.cfoHz = 3000; XbandGenerator gen(g); XbandReceiver rx(kXbDmsp); rx.configure(2e6);
      std::vector<cf32> b(1 << 16); for (int i = 0; i < 60; i++) { gen.generate(b.data(), b.size()); rx.feed(b.data(), b.size()); } rx.finish();
      XbTelemetry t; rx.telemetry(t, 0);
      printf("  DMSP: %llu frames, %llu lines\n", (unsigned long long)t.blocksOk, (unsigned long long)t.lines);
      CHECK(t.lines >= 20, "dmsp lines");
      const int W = 490 * 15;
      // the first alarm frames pass before the deframer locks, so the rows start one or two lines into the signal
      int dy = 0;
      { int bw = 0, bh = 0, best = -1; std::vector<uint16_t> p; rx.readPlane(0, bw, bh, p);
        for (int d = 0; d < 3; d++) { int ok = 0; for (int y = 2; y < bh - 3; y++) for (int ox = 100; ox < W - 100; ox += 23) ok += p[(size_t)y * bw + (bw - 1 - ox)] == ((xbTestPixel(0, ox, y + d) >> 6 & 63) << 10); if (ok > best) { best = ok; dy = d; } } }
      pixels(rx, 0, "OLS", [&](int x, int y) { const int ox = W - 1 - x; if (ox < 16 || ox >= W - 16 || y < 1 || y >= 22) return -1; return (xbTestPixel(0, ox, y + dy) >> 6 & 63) << 10; }, 20);
    }
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
    if (only < 0 || only == 100) instruments();
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
