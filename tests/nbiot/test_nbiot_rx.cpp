// NB-IoT receiver (nbiot_phy.h searchDecode / probe): the four test cells (standalone, guard band, in-band with the same and with another PCI),
// one and two NRS ports, carrier offsets, sample clock errors, noise, other sample rates, an offset candidate, noise alone.
#include "dect2/nbiot/nbiot_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/nbiot/nbiot_phy.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::nbiot;
#include "../common/test_util.h"

namespace {

std::vector<cf32> signal(int scene, double snrDb, double cfoHz, double ppm, double rate, double sec, int skip = 0) {
    SynthConfig sc;
    sc.snrDb = snrDb; sc.cfoHz = cfoHz; sc.sroPpm = ppm;
    sc.modeOpt[0] = scene;
    auto g = makeNbiotSynth(sc, rate);
    std::vector<cf32> junk((size_t)skip + 1);
    g->generate(junk.data(), (size_t)skip + 1);
    std::vector<cf32> x((size_t)(sec * rate));
    g->generate(x.data(), x.size());
    return x;
}

struct Truth { NbTestTruth t; };

int run(const char* name, int scene, double snrDb, double cfoHz, double ppm, double rate, double offsetHz = 0, bool wantSib1 = true, int skip = 12345) {
    NbTestTruth t;
    nbiotTestCell(scene, &t);
    const std::vector<cf32> x = signal(scene, snrDb, cfoHz + offsetHz, ppm, rate, 1.0, skip);
    SearchOpts o;
    o.maxCfoHz = 22500;
    o.wantSib1 = wantSib1;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<CellResult> r = searchDecode(x.data(), x.size(), rate, offsetHz, o);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (r.empty()) { printf("  %-34s no cell found (%.1f s)\n", name, sec); CHECK(false, "%s: no cell", name); return 0; }
    const CellResult& c = r[0];
    printf("  %-34s PCI %d, cfo %+.0f Hz (%+.0f), %+.1f ppm, NPSS %.1f NSSS %.1f, MIB %d (%s, %d ports, SFN %d), SIB1 %d, %.1f dBFS, %.1f s\n", name, c.found.pci, c.found.cfoHz, cfoHz + t.rasterKhz * 1e3,
           c.found.clockPpm, c.found.npssScore, c.found.nsssScore, c.mib.ok, c.mib.ok ? opModeName(c.mib.mib.mode) : "-", c.mib.ports, c.mib.sfn, c.sib1Ok, c.meas.levelDb, sec);
    CHECK(c.found.pci == t.pci, "%s: PCI %d, expected %d", name, c.found.pci, t.pci);
    CHECK(std::fabs(c.found.cfoHz - (cfoHz + t.rasterKhz * 1e3)) < 150, "%s: carrier offset %.0f Hz, expected %.0f", name, c.found.cfoHz, cfoHz + t.rasterKhz * 1e3);
    if (std::fabs(ppm) > 0) CHECK(std::fabs(std::fabs(c.found.clockPpm) - std::fabs(ppm)) < 6, "%s: clock %.1f ppm, expected %.1f", name, c.found.clockPpm, ppm);
    CHECK(c.mib.ok && c.mib.mib.mode == t.mode && c.mib.ports == t.ports && c.mib.mib.sched == t.sched && c.mib.mib.valueTag == 5 && c.mib.mib.hsfnLsb == 1, "%s: MIB-NB", name);
    if (wantSib1) {
        CHECK(c.sib1Ok, "%s: SIB1-NB not decoded (%d tries)", name, c.sib1Tries);
        if (c.sib1Ok) {
            const Sib1& s = c.sib1;
            CHECK(s.plmns.size() == 1 && s.plmns[0].mcc == t.mcc && s.plmns[0].mnc == t.mnc && s.tac == t.tac && s.cellId == t.cellId && s.band == t.band && !s.barred, "%s: SIB1-NB fields (TAC %u, cell %u, band %d)", name, s.tac, s.cellId, s.band);
        }
    }
    return 1;
}

} // namespace

int main() {
    printf("clean signals, every operating mode (1 and 2 NRS ports)\n");
    for (int scene = 0; scene < 4; scene++) { char n[48]; snprintf(n, sizeof n, "scene %d, 20 dB", scene); run(n, scene, 20, 0, 0, 2e6); }
    printf("carrier and clock offsets\n");
    run("scene 0, +12 kHz", 0, 15, 12000, 0, 2e6);
    run("scene 1, -9.5 kHz, +15 ppm", 1, 15, -9500, 15, 2e6);
    run("scene 2, +3 kHz, -20 ppm", 2, 15, 3000, -20, 2e6);
    run("scene 3, -14 kHz, +8 ppm", 3, 15, -14000, 8, 2e6);
    // a cheap radio's crystal both ways (QA sweep 3.9: -50 ppm took a side peak of the NPSS in the first timing window)
    run("scene 0, -2 kHz, -50 ppm", 0, 15, -2000, -50, 2e6);
    run("scene 0, +2 kHz, +50 ppm", 0, 15, 2000, 50, 2e6);
    run("scene 2, -1 kHz, -40 ppm", 2, 15, -1000, -40, 2e6);
    printf("noise\n");
    run("scene 0, 5 dB", 0, 5, 1500, 0, 2e6);
    run("scene 1, 3 dB (2 ports)", 1, 3, -2000, 0, 2e6);
    run("scene 2, 0 dB, MIB only", 2, 0, 800, 0, 2e6, 0, false);
    printf("other sample rates and an offset candidate\n");
    run("scene 0, 1.92 Msps", 0, 15, 4000, 0, 1.92e6);
    run("scene 1, 2.4 Msps", 1, 15, -4000, 5, 2.4e6);
    run("scene 3, 3.2 Msps", 3, 15, 1000, 0, 3.2e6);
    run("scene 2, candidate at +300 kHz", 2, 15, 2000, 0, 2e6, 300e3);
    // the probe finds the raster point with a carrier
    {
        const std::vector<cf32> x = signal(0, 12, 0, 0, 2e6, 1.0, 777);
        std::vector<double> offs;
        for (int k = -6; k <= 6; k++) offs.push_back(k * 100e3 + (k == 3 ? 300e3 - 300e3 : 0));
        const std::vector<cf32> y = signal(0, 12, 300e3, 0, 2e6, 1.0, 777);
        const auto pr = probe(y.data(), y.size(), 2e6, offs, 22500, 4.6f);
        double best = 0, at = 0;
        for (const auto& p : pr) if (p.score > best) { best = p.score; at = p.offsetHz; }
        CHECK(!pr.empty() && std::fabs(at - 300e3) < 1, "the probe finds the carrier at %.0f kHz (score %.1f, %zu hits)", at / 1e3, best, pr.size());
        const auto none = probe(x.data(), x.size(), 2e6, {-500e3, -400e3, 400e3, 500e3}, 22500, 4.6f);
        CHECK(none.empty(), "the probe finds nothing on empty raster points (%zu)", none.size());
    }
    // noise alone: no cell
    {
        std::mt19937 rng(9);
        dect2::normal_distribution<float> nd(0.f, 0.1f);
        std::vector<cf32> x((size_t)2e6);
        for (auto& v : x) v = cf32{nd(rng), nd(rng)};
        SearchOpts o; o.maxCfoHz = 22500;
        const auto r = searchDecode(x.data(), x.size(), 2e6, 0, o);
        CHECK(r.empty(), "noise alone gives no cell (found %zu)", r.size());
        std::vector<double> offs;
        for (int k = -8; k <= 8; k++) offs.push_back(k * 100e3);
        const auto pr = probe(x.data(), x.size(), 2e6, offs, 22500, 4.6f);
        CHECK(pr.empty(), "the probe on noise finds %zu points", pr.size());
    }
    printf(fails ? "nbiot rx: %d FAILED\n" : "nbiot rx: all passed\n", fails);
    return fails ? 1 : 0;
}
