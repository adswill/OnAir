// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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

struct Res { uint64_t ok = 0, bad = 0, frames = 0, dec = 0, lost = 0; bool bb = false; int spot = -1, bearer = -1, plmnMcc = 0; double esn0 = 0, cn0 = 0, cfo = 0; };

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
    Res r; r.ok = s.blocksOk; r.bad = s.blocksBad; r.frames = s.frames; r.bb = s.sig.haveBb; r.spot = s.sig.spotBeam; r.bearer = s.bearer; r.dec = s.framesDecoded; r.lost = s.framesLost;
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
    static const struct { int b, sub; double req; const char* lab; } sw[] = {
        {0,8,48.3,"F80T0.25Q R"},{0,0,40.1,"F80T0.25Q L8"},{2,8,53.6,"F80T1Q-4B R"},{2,0,46.1,"F80T1Q-4B L8"},{3,8,53.5,"F80T1X-4B R"},
        {4,8,57.0,"FR2.5X4/16 R"},{5,8,57.3,"FR2.5X16 R"},{6,8,60.0,"FR2.5X32 R"},{7,8,60.7,"FR2.5X64 R"},{8,8,59.6,"F4.5X R"},
        {9,8,60.6,"FR5X4/16 R"},{10,8,60.0,"FR5X16 R"},{11,8,62.3,"FR5X32 R"},{12,8,64.0,"FR5X64 R"} };
    std::printf("AWGN sensitivity: lowest C/N0 with >= 90 percent of frames decoded (6 s, 0.5 dB steps), against the Annex B figure for BLER 1e-3 (which assumes an ideal modem):\n");
    for (const auto& e : sw) {
        double best = 0;
        for (double c = e.req - 2.0; c <= e.req + 8; c += 0.5) {
            const Res r = run(e.b, e.sub, c, 6.0, 300.0, 0, 0, 0, 3.0, 11);
            if (r.frames >= 60 && double(r.dec) >= 0.9 * double(r.frames)) { best = c; break; }
        }
        std::printf("  %-16s required %.1f dB-Hz: decoded from %.1f (loss %+.1f dB) symbol rate %d kBd\n", e.lab, e.req, best, best ? best - e.req : 99.0, bearer(e.b).symRate / 1000);
    }
    std::printf("Impairments at the R subtype (+4 dB over the figure above): decoded frames / frames\n");
    static const struct { const char* lab; double off, drift, fh, fd, ppm; } imp[] = {
        {"offset +1500 Hz", 1500,0,0,0,0},{"offset -2500 Hz", -2500,0,0,0,0},{"Doppler drift 20 Hz/s", 0,20,0,0,0},
        {"clock +20 ppm", 0,0,0,0,20},{"clock -20 ppm", 0,0,0,0,-20},{"clock +50 ppm", 0,0,0,0,50},{"clock -50 ppm", 0,0,0,0,-50},{"fading 2 Hz, 50 %", 0,0,2,0.5,0},{"fading 20 Hz, 50 %", 0,0,20,0.5,0},{"fading 20 Hz, 80 %", 0,0,20,0.8,0} };
    for (const auto& im : imp) {
        for (int bi : {0, 2, 3, 8, 10, 12}) {
            double req = 0; for (const auto& e : sw) if (e.b == bi && e.sub == 8) req = e.req;
            const Res r = run(bi, 8, req + 4, 4.0, im.off, im.drift, im.fh, im.fd, im.ppm, 21);
            std::printf("  %-22s %-16s %llu/%llu lost %llu cfo %.0f\n", im.lab, bearer(bi).name, (unsigned long long)r.dec, (unsigned long long)r.frames, (unsigned long long)r.lost, r.cfo);
        }
    }
    return 0;
}
