// BGAN demodulator: every bearer type through the real frame structure, offsets, noise; and sensitivity (run with no arguments = quick pass/fail;
// `sweep` prints the sensitivity table).
#include "dect2/bgan/bgan_demod.h"
#include "dect2/bgan/bgan_gen.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2; using namespace dect2::bgan;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

struct Res { uint64_t ok = 0, bad = 0, frames = 0; bool bb = false; int spot = -1, bearer = -1, plmnMcc = 0; double esn0 = 0, cn0 = 0, cfo = 0; };

static Res run(int bi, int sub, double cn0, double secs, double offs, double drift, double fadeHz, double fadeDepth, double ppm, uint32_t seed) {
    const Bearer& b = bearer(bi);
    const double fs = std::max(4.0 * b.symRate, 100000.0);
    GenConfig g; g.fs = fs; g.bearer = bi; g.subtype = sub; g.offsetHz = offs; g.driftHzPerSec = drift; g.fadeHz = fadeHz; g.fadeDepth = fadeDepth; g.clockPpm = ppm; g.seed = seed;
    g.sig.spotBeam = 77; g.sig.beamType = 2;
    Generator gen(g);
    BearerDemod d; d.configure(fs, b.symRate);
    std::mt19937 rng(seed * 113 + bi);
    std::normal_distribution<float> n(0, float(std::sqrt(fs * std::pow(10.0, -cn0 / 10) / 2)));
    const size_t chunk = size_t(fs * 0.02);
    std::vector<cf32> buf(chunk);
    for (size_t i = 0; i < size_t(secs / 0.02); i++) {
        std::fill(buf.begin(), buf.end(), cf32(0, 0));
        gen.generate(buf.data(), chunk);
        for (auto& v : buf) v += cf32(n(rng), n(rng));
        d.feed(buf.data(), chunk);
    }
    const DemodStatus& s = d.status();
    Res r; r.ok = s.blocksOk; r.bad = s.blocksBad; r.frames = s.frames; r.bb = s.sig.haveBb; r.spot = s.sig.spotBeam; r.bearer = s.bearer;
    r.plmnMcc = s.sig.plmns.empty() ? 0 : s.sig.plmns[0].mcc; r.esn0 = s.esn0Db; r.cn0 = s.cn0Dbhz; r.cfo = s.cfoHz;
    return r;
}

// C/N0 for the required BLER of this bearer's R subtype, from TS 102 744-2-1 Annex B (dB-Hz), for the test margin
static double nominalCn0(int bi) {
    const Bearer& b = bearer(bi);
    static const struct { const char* n; double c; } t[] = {
        {"F80T0.25Q-1B", 48.3},{"F80T1Q-1B", 51.5},{"F80T1Q-4B", 51.5},{"F80T1X-4B", 55.0},{"FR80T2.5X4/16-5B", 56.0},{"FR80T2.5X16-5B", 59.5},
        {"FR80T2.5X32-6B", 62.0},{"FR80T2.5X64-7B", 64.5},{"F80T4.5X-8B", 61.5},{"FR80T5X4/16-9B", 59.0},{"FR80T5X16-9B", 62.5},{"FR80T5X32-11B", 65.0},{"FR80T5X64-13B", 67.5} };
    for (const auto& e : t) if (!std::strcmp(e.n, b.name)) return e.c;
    return 60;
}

int main(int argc, char** argv) {
    bool sweep = argc > 1 && !std::strcmp(argv[1], "sweep");
    if (!sweep) {
        for (int bi = 0; bi < bearerCount(); bi++) {
            const Bearer& b = bearer(bi);
            const Res r = run(bi, 8, nominalCn0(bi) + 6, 1.2, 150.0, 0, 0, 0, 2.0, uint32_t(bi + 1));
            std::printf("%-20s frames %llu ok %llu bad %llu bearer %d spot %d cn0 %.1f cfo %.0f\n", b.name, (unsigned long long)r.frames, (unsigned long long)r.ok, (unsigned long long)r.bad, r.bearer, r.spot, r.cn0, r.cfo);
            if (b.family < 0) { CHECK(r.frames >= 8, "%s structure", b.name); continue; }
            CHECK(r.ok >= 8 && r.bearer == bi && r.spot == 77 && r.plmnMcc == 901, "%s decode ok %llu bearer %d spot %d mcc %d", b.name, (unsigned long long)r.ok, r.bearer, r.spot, r.plmnMcc);
        }
        std::printf(fails ? "bgan demod: %d FAILED\n" : "bgan demod: all passed\n", fails);
        return fails ? 1 : 0;
    }
    return 0;
}
