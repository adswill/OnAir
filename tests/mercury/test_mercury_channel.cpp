// Mercury OFDM through the HF channel models (Watterson, two equal paths, Gaussian Doppler), every mode, single-burst trials with a
// random frequency offset (+-25 Hz), start and 30 ppm of sample clock: delivered = acquired, LDPC converged and CRC right. The SNR
// points are those of Mercury's own bench (docs/MODES.md, "MPP" = 2 ms / 1 Hz, our CCIR poor) where it delivers 70 - 94 %, and AWGN
// near the threshold. The minimums checked are what this receiver reaches with margin, against OnAir's own modulator only.
// `test_mercury_channel full` runs 40 trials a point and the wider table.
#include "dect2/mercury/mercury_phy.h"
#include "dect2/mercury/mercury_gen.h"
#include "dect2/dsp/portable_rng.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
using namespace dect2::mercury;
#include "../common/test_util.h"

namespace {

int delivered(int mode, int fading, double snr, int trials, uint64_t seed0) {
    int ok = 0;
    PortableRng r(seed0);
    for (int t = 0; t < trials; t++) {
        std::vector<uint8_t> pay((size_t)geometry(mode).payloadBytes);
        for (auto& b : pay) b = (uint8_t)r();
        std::vector<GenBurst> sc(1);
        sc[0].mode = mode;
        sc[0].frame = makeFrame(mode, pay.data(), pay.size());
        sc[0].gapSec = 0.6;
        ChannelConfig ch;
        ch.snrDb = snr; ch.fading = fading; ch.cfoHz = r.uniform(-25, 25); ch.sroPpm = 30; ch.seed = seed0 * 1000 + (uint64_t)t;
        const std::vector<cf> x = renderScript(sc, ch, 0.3 + r.uniform(0, 0.2));
        OfdmReceiver rx;
        rx.pushSideband(x.data(), x.size());
        rx.flush();
        for (const OfdmFrame& f : rx.take()) if (f.ldpcOk && f.crcPlain && f.mode == mode && f.payload == pay) { ok++; break; }
    }
    return ok;
}

struct Point { int mode, fading; double snr; int minPct; };

} // namespace

int main(int argc, char** argv) {
    const bool full = argc > 1 && !std::strcmp(argv[1], "full");
    const int N = full ? 40 : 12;
    // mode, channel, SNR (3 kHz), minimum delivered %
    const Point pts[] = {
        {kDatac16, kFadeNone, -8, 75}, {kDatac16, kCcirPoor, -2, 60}, {kDatac16, kCcirGood, -5, 60},
        {kDatac15, kFadeNone, -8, 75}, {kDatac15, kCcirPoor, -2, 60},
        {kDatac13, kFadeNone, -7, 75}, {kDatac13, kCcirPoor, -2, 60},
        {kDatac14, kFadeNone, -4, 75}, {kDatac14, kCcirPoor, 0, 60},
        {kDatac4, kFadeNone, -5, 75}, {kDatac4, kCcirPoor, -1, 60},
        {kDatac0, kFadeNone, -1, 75}, {kDatac0, kCcirPoor, 2, 60},
        {kDatac3, kFadeNone, 0, 75}, {kDatac3, kCcirPoor, 3, 60}, {kDatac3, kItuLowModerate, 5, 50},
        {kDatac1, kFadeNone, 4, 75}, {kDatac1, kCcirPoor, 8, 60}, {kDatac1, kCcirModerate, 7, 60},
        {kDatac17, kFadeNone, 7, 75}, {kDatac17, kCcirPoor, 12, 50},
        {kQam16c2, kFadeNone, 14, 75}, {kQam16c2, kCcirModerate, 20, 50},
    };
    printf("  %-8s %-28s %6s  delivered\n", "mode", "channel", "SNR");
    uint64_t seed = 1;
    for (const Point& p : pts) {
        const int ok = delivered(p.mode, p.fading, p.snr, N, seed++);
        const int pct = 100 * ok / N;
        printf("  %-8s %-28s %+5.1f  %3d %%%s\n", ofdmMode(p.mode).name, fadingName(p.fading), p.snr, pct, pct < p.minPct ? "  (below the minimum)" : "");
        CHECK(pct >= p.minPct, "%s %s %+.1f dB: %d %% delivered, want %d %%", ofdmMode(p.mode).name, fadingName(p.fading), p.snr, pct, p.minPct);
    }
    if (full) {
        // flutter (10 Hz Doppler) defeats the pilot spacing of every mode, as it does Mercury's own receiver: printed, not checked
        for (int m : {kDatac16, kDatac3, kDatac1})
            printf("  %-8s %-28s %+5.1f  %3d %%\n", ofdmMode(m).name, fadingName(kFlutter), 10.0, 100 * delivered(m, kFlutter, 10, N, seed++) / N);
    }
    return testSummary("mercury channel");
}
