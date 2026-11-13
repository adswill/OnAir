// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CPU cost of the HF/VHF data receivers (pager, packet, PSK, CW) per second of signal, with a signal and with noise only.
//   bench_hfdata [secs=20]      prints: mode, rate, CPU-s per s of signal (signal / noise only), digest of the decoded counts
// Generation of the test signal is NOT in the timed part (it is made first, then fed).
#include "dect2/cw/cw_gen.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/psk/psk_gen.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include <sys/resource.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
static double cpuSec() { struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_stime.tv_sec + 1e-6 * (r.ru_utime.tv_usec + r.ru_stime.tv_usec); }
static std::vector<cf32> noise(size_t n, float s) {
    std::mt19937 g(5); std::normal_distribution<float> d(0, s);
    std::vector<cf32> v(n); for (auto& x : v) x = cf32(d(g), d(g)); return v;
}
template <class Rx, class Tel>
static void timeit(const char* name, double rate, const std::vector<cf32>& sig, const std::vector<cf32>& nz, double off, void (*prep)(Rx&)) {
    double c[2]; unsigned long long cnt = 0;
    for (int k = 0; k < 2; k++) {
        Rx rx; if constexpr (requires { rx.setSilent(true); }) rx.setSilent(true); rx.configure(rate); rx.setSignalOffset(off); if (prep) prep(rx);
        const auto& v = k ? nz : sig;
        const size_t blk = (size_t)(rate * 0.05);
        const double t0 = cpuSec();
        for (size_t o = 0; o + blk <= v.size(); o += blk) rx.feed(v.data() + o, blk);
        c[k] = cpuSec() - t0;
        if (!k) { Tel t; rx.telemetry(t, 0); if constexpr (requires { t.blocksOk; }) cnt = t.blocksOk; }
    }
    const double secs = sig.size() / rate;
    printf("%-10s %9.0f sps  signal %.4f  noise-only %.4f  CPU-s per s   good units %llu\n", name, rate, c[0] / secs, c[1] / secs, cnt);
    fflush(stdout);
}
template <class G>
static std::vector<cf32> gen(G& g, size_t n) { std::vector<cf32> v(n); for (size_t o = 0; o < n; o += 25000) g.generate(v.data() + o, std::min<size_t>(25000, n - o)); return v; }
int main(int argc, char** argv) {
    const double secs = argc > 1 ? atof(argv[1]) : 20;
    const char* only = argc > 2 ? argv[2] : "";
    auto want = [&](const char* n) { return !*only || strstr(n, only); };
    for (double rate : {1.2e6, 2.4e6}) {
        const size_t n = (size_t)(rate * secs);
        const float nl = 0.02f;
        for (int stdMode : {25, 26}) {
            if (!want(stdMode == 25 ? "pager" : "packet")) continue;
            SynthConfig sc; sc.mode = stdMode; sc.snrDb = 25; if (stdMode == 26) sc.modeOpt[0] = 0;
            auto g = makeModeSynth(stdMode, sc, rate);
            auto sig = gen(*g, n); auto nz = noise(n, nl);
            if (stdMode == 25) timeit<PagerReceiver, PagerTelemetry>("pager", rate, sig, nz, -pagerTuning().tuneOffsetHz, nullptr);
            else timeit<PacketReceiver, PacketTelemetry>("packet", rate, sig, nz, -packetTuning().tuneOffsetHz, nullptr);
        }
    }
    for (double rate : {250e3, 2.4e6}) {
        const size_t n = (size_t)(rate * secs);
        if (want("psk")) {
            PskGenConfig c; c.rate = rate; c.dialOffsetHz = -20000; c.signals = pskTestSignals();
            PskGenerator g(c); auto sig = gen(g, n); auto nz = noise(n, 0.02f);
            timeit<PskReceiver, PskTelemetry>("psk31", rate, sig, nz, -20000, nullptr);
        }
        if (want("cw")) {
            CwGenConfig c; c.rate = rate; c.dialOffsetHz = -20000; CwGenSignal s; s.offsetHz = 700; s.snrDb = 20; c.signals = {s};
            CwGenerator g(c); auto sig = gen(g, n); auto nz = noise(n, 0.02f);
            timeit<CwReceiver, CwTelemetry>("cw", rate, sig, nz, -20000, nullptr);
        }
    }
    // audio decoders at 8 kHz
    {
        struct A { const char* name; int which; int opt; };
        for (const A a : {A{"rtty", 0, 0}, A{"kbd26", 1, 26}, A{"kbd27", 1, 27}, A{"kbd28", 1, 28}, A{"kbd29", 1, 29}, A{"kbd30", 1, 30}, A{"kbd31", 1, 31}, A{"sstv", 2, 0}}) {
            if (!want(a.name)) continue;
            SynthConfig sc; sc.mode = 27; sc.snrDb = 25; sc.modeOpt[0] = a.opt;
            std::unique_ptr<HfdigTestAudio> g = a.which == 0 ? makeRttyTestAudio(sc) : a.which == 1 ? makeKbdTestAudio(sc) : makeSstvTestAudio(sc);
            if (!g) { printf("%s: no generator\n", a.name); continue; }
            const size_t n = (size_t)(8000 * secs * 3);
            std::vector<float> v(n);
            g->generate(v.data(), n);
            std::unique_ptr<HfdigDecoder> d;
            if (a.which == 0) d = std::make_unique<HfdigRtty>(); else if (a.which == 1) d = makeKbdDecoder(); else d = std::make_unique<HfdigSstv>();
            std::mt19937 rg(3); std::normal_distribution<float> nd(0, 0.02f);
            std::vector<float> nz(n); for (auto& x : nz) x = nd(rg);
            double c[2];
            for (int k = 0; k < 2; k++) {
                const double t0 = cpuSec();
                const auto& src = k ? nz : v;
                for (size_t o = 0; o + 800 <= n; o += 800) d->feedAudio(src.data() + o, 800);
                c[k] = cpuSec() - t0;
            }
            printf("%-10s %9.0f sps  signal %.4f  noise-only %.4f  CPU-s per s\n", a.name, 8000.0, c[0] / (n / 8000.0), c[1] / (n / 8000.0));
            fflush(stdout);
        }
    }
}
