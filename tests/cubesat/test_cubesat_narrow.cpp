// CubeSat narrow paths (docs/SENSITIVITY-4.0-X3.md): each framing of the test signal (cubesat_gen.h) at a series of C/N values (dB in 25 kHz,
// the generator's own noise), with the carrier still, held just under the AFC's 1.5 kHz threshold (an error the AFC never mixes out), or drifting
// several kHz during the run like a satellite pass. Frames decoded / frames sent, for the 4.0 receiver (narrow paths beside the wide ones) and
// the 3.9 one (DECT2_CUBESAT_NONARROW=1), same signals; wrong frames and frames out of noise alone are counted too.
//   default: a guard (every framing decodes through a 12 kHz drift at 20 dB, AX100 Mode 6 / NGHam / U482C gain at their 3.9 50 % point,
//            noise alone gives no frame)
//   ONAIR_SENS_CALIBRATE=1: the table (X3_CN "hi:lo:step", default 14:4:1; X3_CYCLES, default 16)
// Simulated signals only.
#include "dect2/cubesat/cubesat_gen.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/cubesat/cubesat_tel.h"
#include "dect2/dsp/gen_util.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::cubesat;

namespace {

struct Scen { const char* name; double f0, f1; };   // carrier error at the start and at the end of the run (Hz), linear in between
const Scen kScen[] = {{"still", 0, 0}, {"+1.4 kHz", 1400, 1400}, {"-1.4 kHz", -1400, -1400}, {"drift +6..-6 kHz", 6000, -6000}};

struct Res { double sent = 0, got = 0, wrong = 0, cpu = 0; int by[kNumFramings] = {}; };   // by: wrong (or noise) frames per framing

void setNarrow(bool on) {
#ifndef _WIN32
    if (on) unsetenv("DECT2_CUBESAT_NONARROW"); else setenv("DECT2_CUBESAT_NONARROW", "1", 1);
#else
    _putenv_s("DECT2_CUBESAT_NONARROW", on ? "" : "1");
#endif
}

// one run: `cycles` cycles of framing mode `mode` (0: all) at C/N `cn`; noiseOnly: the receiver gets the noise alone
Res runOne(int mode, double cn, const Scen& sc, int cycles, bool noiseOnly, uint64_t seed) {
    const double rate = cubesatTuning().sampleRate;
    SynthConfig cfg; cfg.modeOpt[0] = mode; cfg.snrDb = cn; cfg.cfoHz = 0;
    std::unique_ptr<ModeSynth> g = makeCubesatSynth(cfg, rate);
    CubesatReceiver r;
    r.configure(rate);
    r.setSignalOffset(-cubesatTuning().tuneOffsetHz);
    const double cyc = cubesatGenCycleSec(mode);
    const size_t total = (size_t)(cyc * cycles * rate);
    // a random start inside the first cycle (the receiver tunes in at any time)
    std::vector<cf32> buf(16384);
    { size_t skip = (size_t)((double)(seed % 1000) / 1000.0 * cyc * rate); while (skip) { const size_t n = std::min(skip, buf.size()); g->generate(buf.data(), n); skip -= n; } }
    genutil::NoiseSource ns((uint32_t)(seed | 1));
    double ph = 0;
    const auto c0 = std::chrono::steady_clock::now();
    CubeTelemetry last; uint64_t seq = 0;
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t n = std::min(buf.size(), total - done);
        if (noiseOnly) { for (size_t i = 0; i < n; i++) buf[i] = 0; ns.add(buf.data(), n, 0.1f); }
        else g->generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) {
            const double f = sc.f0 + (sc.f1 - sc.f0) * (double)(done + i) / (double)total;
            ph += 2 * M_PI * f / rate;
            if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
            buf[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        r.feed(buf.data(), n);
        CubeTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    Res res;
    res.cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    const std::vector<CubeGenFrame> sent = cubesatGenCycle(mode);
    if (noiseOnly) { res.got = (double)last.blocksOk; for (const CubeFrame& f : last.frames) if (f.framing >= 0 && f.framing < kNumFramings) res.by[f.framing]++; return res; }
    res.sent = (double)sent.size() * (double)cycles;
    for (const CubeFrame& f : last.frames) {
        bool ok = false;
        for (const CubeGenFrame& s : sent) ok |= f.framing == s.framing && f.data == s.data;
        if (ok) res.got += 1; else { res.wrong += 1; if (f.framing >= 0 && f.framing < kNumFramings) res.by[f.framing]++; }
    }
    res.got = std::min(res.got, res.sent);
    return res;
}

// several runs in parallel (different starts and noise seeds), summed
Res runMany(int mode, double cn, const Scen& sc, int cycles, int runs, bool noiseOnly) {
    std::vector<Res> rs((size_t)runs);
    std::atomic<int> next{0};
    auto work = [&] { for (;;) { const int i = next++; if (i >= runs) return; rs[(size_t)i] = runOne(mode, cn, sc, cycles, noiseOnly, 1000003ull * (uint64_t)(i + 1) + (uint64_t)(cn * 10 + 500) * 7 + (uint64_t)mode * 131); } };
    std::vector<std::thread> th;
    const int nt = std::max(1, std::min(runs, (int)std::thread::hardware_concurrency()));
    for (int k = 0; k < nt; k++) th.emplace_back(work);
    for (auto& t : th) t.join();
    Res s;
    for (const Res& r : rs) { s.sent += r.sent; s.got += r.got; s.wrong += r.wrong; s.cpu += r.cpu; for (int k = 0; k < kNumFramings; k++) s.by[k] += r.by[k]; }
    return s;
}

double pct(const Res& r) { return r.sent > 0 ? 100.0 * r.got / r.sent : 0; }
std::string byText(const Res& r) {
    std::string t;
    for (int k = 0; k < kNumFramings; k++) if (r.by[k]) t += std::string(t.empty() ? "" : ", ") + framingName(k) + " " + std::to_string(r.by[k]);
    return t.empty() ? "-" : t;
}

} // namespace

int main() {
    for (int m = 0; m <= kNumFramings; m++) (void)cubesatGenCycle(m);   // build the signal caches before any thread runs
    const int framingsOfInterest[] = {Ax25G3ruh, Ax100Mode5, Ax100Mode6, CcsdsRs, U482c, CcsdsConcat, Usp, Ngham, Mobitex};
    if (getenv("ONAIR_SENS_CALIBRATE")) {
        double hi = 14, lo = 4, st = 1;
        if (getenv("X3_CN")) sscanf(getenv("X3_CN"), "%lf:%lf:%lf", &hi, &lo, &st);
        const int cycles = getenv("X3_CYCLES") ? atoi(getenv("X3_CYCLES")) : 16;
        for (int fr : framingsOfInterest) {
            printf("== %s (C/N in 25 kHz; frames decoded of sent, 4 runs of %d cycles; new | 3.9)\n", framingName(fr), cycles);
            for (const Scen& sc : kScen) {
                printf("-- %s\n", sc.name);
                double cpuN = 0, cpuO = 0;
                for (double cn = hi; cn >= lo - 1e-9; cn -= st) {
                    setNarrow(true); const Res a = runMany(fr + 1, cn, sc, cycles, 4, false);
                    setNarrow(false); const Res b = runMany(fr + 1, cn, sc, cycles, 4, false);
                    setNarrow(true);
                    cpuN += a.cpu; cpuO += b.cpu;
                    printf("  %5.1f dB   %5.1f %%  (wrong %.0f)   |  %5.1f %%  (wrong %.0f)%s%s\n", cn, pct(a), a.wrong, pct(b), b.wrong, a.wrong > b.wrong ? "   new wrong: " : "", a.wrong > b.wrong ? byText(a).c_str() : "");
                    fflush(stdout);
                    if (a.got == 0 && b.got == 0) break;
                }
                printf("   cpu: new %.1f s, 3.9 %.1f s (%+.0f %%)\n", cpuN, cpuO, cpuO > 0 ? 100 * (cpuN / cpuO - 1) : 0.0);
            }
        }
        const Scen still = kScen[0];
        setNarrow(true); const Res fa = runMany(0, 0, still, 40, 4, true);
        setNarrow(false); const Res fb = runMany(0, 0, still, 40, 4, true);
        setNarrow(true);
        printf("-- noise only, 4 runs of %.0f s: new %.0f frames (%s), 3.9 %.0f frames (%s)\n", 40 * cubesatGenCycleSec(0), fa.got, byText(fa).c_str(), fb.got, byText(fb).c_str());
        return 0;
    }
    int fails = 0;
    setNarrow(true);
    for (int fr : framingsOfInterest) {
        const Res r = runMany(fr + 1, 20, kScen[3], 6, 2, false);
        printf("%-22s 20 dB, drift +6..-6 kHz: %5.1f %% (wrong %.0f)\n", framingName(fr), pct(r), r.wrong);
        if (pct(r) < 90 || r.wrong > 0) { printf("FAIL: %s through the drift\n", framingName(fr)); fails++; }
    }
    const Res f = runMany(0, 0, kScen[0], 20, 2, true);
    printf("noise only: %.0f frames (%s)\n", f.got, byText(f).c_str());
    if (f.got > 0) { printf("FAIL: frames out of noise\n"); fails++; }
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}
