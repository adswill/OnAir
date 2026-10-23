// Mercury's MFSK floor through OnAir's own modulator: the tone layout Mercury's code computes, both geometries through noise near the
// floor, a dial error of +-60 Hz and a sound card running 0.4 % off (read from the preamble and postamble), Watterson fading, noise
// alone, and a link at the floor (CALL, ACCEPT, a data round of 23-byte pieces, control behind the floor mark, DISCONNECT) through the
// monitor. Verified against this implementation's own modulator only (docs/mercury.md).
#include "dect2/mercury/mercury_mfsk.h"
#include "dect2/mercury/mercury_gen.h"
#include "dect2/mercury/mercury_monitor.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::mercury;
#include "../common/test_util.h"

namespace {

int trials(int mode, double snr, double cfo, double ppm, int fading, int n, uint64_t seed) {
    int ok = 0;
    PortableRng r(seed);
    for (int t = 0; t < n; t++) {
        std::vector<uint8_t> pay(98);
        for (auto& b : pay) b = (uint8_t)r();
        std::vector<GenBurst> sc(1);
        sc[0].mode = mode; sc[0].frame = makeMfskFrame(pay.data(), pay.size()); sc[0].gapSec = 0.8;
        ChannelConfig ch;
        ch.snrDb = snr; ch.cfoHz = cfo; ch.sroPpm = ppm; ch.fading = fading; ch.seed = seed * 100 + (uint64_t)t;
        const std::vector<cf> x = renderScript(sc, ch, 0.3 + 0.1 * r.uniform());
        OfdmReceiver rx;
        rx.pushSideband(x.data(), x.size());
        rx.flush();
        for (const OfdmFrame& f : rx.take()) if (f.mode == mode && f.ldpcOk && f.crcPlain && f.payload == pay) { ok++; break; }
    }
    return ok;
}

} // namespace

int main(int argc, char**) {
    const bool full = argc > 1;
    // the layout (modem_mfsk.c, mfsk.c): 32 tones of the 50 carriers, centred on 1500 Hz, 13.12 s (26.24 s) a burst
    CHECK(std::fabs(mfskToneHz(kMfsk, 0) - 1984.375) < 1e-6 && std::fabs(mfskToneHz(kMfsk, 31) - 1015.625) < 1e-6, "MFSK tones %.3f .. %.3f", mfskToneHz(kMfsk, 0), mfskToneHz(kMfsk, 31));
    CHECK(std::fabs(mfskToneHz(kMfsk500, 0) - 1742.1875) < 1e-6 && std::fabs(mfskToneHz(kMfsk500, 31) - 1257.8125) < 1e-6, "MFSK500 tones");
    CHECK(mfskGeometry(kMfsk).burstSamples == 328 * 320 && mfskGeometry(kMfsk500).burstSamples == 328 * 640, "burst lengths");
    {
        const std::vector<int>& p = mfskInterleaver();
        std::vector<int> seen(1600, 0);
        for (int v : p) if (v >= 0 && v < 1600) seen[(size_t)v]++;
        int bad = 0;
        for (int s : seen) bad += s != 1;
        CHECK(p.size() == 1600 && bad == 0, "the interleaver is not a permutation");
    }
    const int N = full ? 20 : 6;
    struct P { int mode; double snr, cfo, ppm; int fading; int minOk; };
    const P pts[] = {
        {kMfsk, -9, 0, 0, kFadeNone, N}, {kMfsk, -10, 47, 0, kFadeNone, N - 1}, {kMfsk, -8, -60, 4000, kFadeNone, N / 2},
        {kMfsk, -4, 12, 50, kCcirPoor, N / 2},
        {kMfsk500, -12, 0, 0, kFadeNone, N}, {kMfsk500, -11, -38, 100, kFadeNone, N - 1},
    };
    uint64_t seed = 1;
    for (const P& p : pts) {
        const int ok = trials(p.mode, p.snr, p.cfo, p.ppm, p.fading, N, seed++);
        printf("  %-8s %+5.1f dB, %+4.0f Hz, %5.0f ppm, %-10s: %d of %d\n", modeName(p.mode), p.snr, p.cfo, p.ppm, fadingName(p.fading), ok, N);
        CHECK(ok >= p.minOk, "%s %+.1f dB %+.0f Hz %.0f ppm %s: %d of %d", modeName(p.mode), p.snr, p.cfo, p.ppm, fadingName(p.fading), ok, N);
    }
    if (full) {   // where it stops: delivered of 20 on AWGN (printed, not checked)
        for (double s : {-11.0, -12.0, -13.0}) printf("  MFSK     %+5.1f dB AWGN: %d of 20\n", s, trials(kMfsk, s, 5, 0, kFadeNone, 20, seed++));
        for (double s : {-13.0, -14.0, -15.0}) printf("  MFSK500  %+5.1f dB AWGN: %d of 20\n", s, trials(kMfsk500, s, 5, 0, kFadeNone, 20, seed++));
        for (double s : {-6.0, -8.0}) printf("  MFSK     %+5.1f dB CCIR poor: %d of 20\n", s, trials(kMfsk, s, 5, 0, kCcirPoor, 20, seed++));
    }
    {   // noise alone, 90 s: nothing
        std::vector<cf> x((size_t)(90 * 8000));
        PortableRng r(5);
        for (cf& v : x) v = cf((float)r.gauss(0.1), (float)r.gauss(0.1));
        OfdmReceiver rx;
        rx.push(x.data(), x.size());
        rx.flush();
        int dec = 0;
        for (const OfdmFrame& f : rx.take()) dec += f.ldpcOk;
        printf("  90 s of noise: %llu bursts tried, %d decoded\n", (unsigned long long)rx.detections(), dec);
        CHECK(dec == 0, "noise decoded");
    }
    {   // a link at the floor through the monitor
        const std::vector<GenBurst> sc = demoScript(4);
        ChannelConfig ch;
        ch.snrDb = -6; ch.cfoHz = 20; ch.seed = 3;
        const std::vector<cf> x = renderScript(sc, ch);
        OfdmReceiver rx;
        Monitor mon;
        rx.pushSideband(x.data(), x.size());
        rx.flush();
        for (const OfdmFrame& f : rx.take()) mon.onFrame(f, 1.7e9 + f.startSample / 8000.0, f.startSample / 8000.0, (f.endSample - f.startSample) / 8000.0);
        MercuryTelemetry t;
        mon.fill(t);
        for (auto it = t.frames.rbegin(); it != t.frames.rend(); ++it)
            printf("    %-8s %-10s dir %2d %-7s -> %-8s %+5.1f dB %s\n", it->modeName.c_str(), it->kind.c_str(), it->dir, it->from.c_str(), it->to.c_str(), it->snrDb, it->detail.c_str());
        CHECK(t.frames.size() == sc.size(), "%zu frames of %zu", t.frames.size(), sc.size());
        const MercurySession* s = nullptr;
        for (const auto& x2 : t.sessions) if (x2.id == 0x22) s = &x2;
        CHECK(s && s->caller == "LY2ONA" && s->ctlDeaf && s->state == "disconnected", "floor session");
        CHECK(s && s->d[0].text == demoFloorText(), "floor data: \"%s\"", s ? s->d[0].text.c_str() : "");
        int ctl = 0;
        for (const auto& f : t.frames) ctl += f.kind == "POLL";
        CHECK(ctl == 1, "the control frame behind the floor mark");
    }
    return testSummary("mercury mfsk");
}
