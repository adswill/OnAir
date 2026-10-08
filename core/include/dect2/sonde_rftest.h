// Shared body of the per-type radio tests of the radiosonde mode (DFM, M10, M20) through the whole receiver: rates, carrier offset,
// clock offset, 8-bit samples, DC offset, a gap, reset, and the SNR at which frames start to fail.
#pragma once
#include "sonde_testkit.h"

namespace dect2 {
namespace sondetest {

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct TypeSpec { const char* type; int mask; const char* serial; double offsetHz; double alt0, vv; };
static const TypeSpec kTypes[3] = {
    {"DFM", 2, "17012345", -800e3, 22000, -15.0},
    {"M10", 4, "310-2-11329", 1100e3, 8000, 5.0},
    {"M20", 8, "211-4-01234", -1400e3, 15000, 5.0},
};

inline SynthConfig cfgOf(const TypeSpec& t, double snr = 30) {
    SynthConfig c; c.mode = 15; c.snrDb = snr; c.modeOpt[0] = t.mask; c.modeOpt[1] = 1; return c;
}

inline void check(const TypeSpec& t, const Result& r, double secs, unsigned minOk, const char* what) {
    const SondeInfo* s = find(r.tel, t.serial);
    CHECK(s != nullptr, "%s %s: sonde not found", t.type, what);
    if (!s) return;
    CHECK(s->framesOk >= minOk && s->framesBad <= 2, "%s %s: %llu frames ok (want %u), %llu bad", t.type, what, (unsigned long long)s->framesOk, minOk, (unsigned long long)s->framesBad);
    CHECK(s->hasPos && std::fabs(s->lat - (t.alt0 > 20000 ? 25.30 : t.alt0 > 10000 ? 25.00 : 25.10)) < 0.01, "%s %s: position %.4f", t.type, what, s->lat);
    int bad = 0;
    for (const auto& tp : s->track) {
        const double tt = tp.unixT - 1780272000.0;
        if (std::fabs(tp.altM - (t.alt0 + t.vv * tt)) > 10.0) bad++;
    }
    CHECK(bad == 0, "%s %s: %d track points off the flight", t.type, what, bad);
    CHECK(s->hasVel && std::fabs(s->vSpeed - t.vv) < 0.3 && std::fabs(s->hSpeed - 10.0) < 0.3, "%s %s: velocity %.2f %.2f", t.type, what, s->vSpeed, s->hSpeed);
    CHECK(s->hasTemp, "%s %s: no temperature", t.type, what);
    (void)secs;
}

inline int runTypeRf(int idx) {
    {
        const TypeSpec& t = kTypes[idx];
        // base case and rates
        for (double rate : {8e6, 10e6, 20e6}) {
            Result r = run(cfgOf(t), rate, 10);
            char w[48]; snprintf(w, sizeof w, "%.0f Msps", rate / 1e6);
            check(t, r, 10, 6, w);
            const SondeInfo* s = find(r.tel, t.serial);
            if (s) CHECK(std::fabs(s->offsetHz - t.offsetHz) < 700.0, "%s %s: carrier at %.0f", t.type, w, s->offsetHz);
            printf("%s %s: rtf %.0f, ok %llu bad %llu\n", t.type, w, r.rtf, s ? (unsigned long long)s->framesOk : 0ull, s ? (unsigned long long)s->framesBad : 0ull);
        }
        if (t.offsetHz < 0 ? t.offsetHz > -0.9e6 : t.offsetHz < 0.9e6) {
            Result r = run(cfgOf(t), 2e6, 10);
            check(t, r, 10, 6, "2 Msps");
        }
        for (double cfo : {-10000.0, 10000.0}) {
            SynthConfig c = cfgOf(t); c.cfoHz = cfo;
            char w[48]; snprintf(w, sizeof w, "cfo %+.0f Hz", cfo);
            check(t, run(c, 8e6, 10), 10, 6, w);
        }
        for (double sro : {-50.0, 50.0}) {
            SynthConfig c = cfgOf(t); c.sroPpm = sro;
            char w[48]; snprintf(w, sizeof w, "sro %+.0f ppm", sro);
            check(t, run(c, 8e6, 12), 12, 7, w);
        }
        { Impair im; im.quant8 = true; im.dcI = 0.03f; im.dcQ = -0.02f; check(t, run(cfgOf(t, 25), 8e6, 10, im), 10, 6, "8 bit and DC offset"); }
        { Impair im; im.chunk = 4096; check(t, run(cfgOf(t), 8e6, 10, im), 10, 6, "chunk 4096"); }
        { Impair im; im.gapAtS = 4.3; im.gapLenS = 0.02; check(t, run(cfgOf(t), 8e6, 10, im), 10, 5, "20 ms gap"); }
        {
            Impair im; im.resetAtS = 5.0;
            Result r = run(cfgOf(t), 8e6, 10, im);
            const SondeInfo* s = find(r.tel, t.serial);
            CHECK(s && s->framesOk >= (t.mask == 2 ? 1u : 2u), "%s after reset: %llu frames", t.type, s ? (unsigned long long)s->framesOk : 0ull);
        }
        printf("%s snr sweep (SNR in 10 kHz), frames ok / bad over 15 s:\n", t.type);
        for (double snr : {16.0, 14.0, 12.0, 10.0, 9.0}) {
            Result r = run(cfgOf(t, snr), 8e6, 15);
            const SondeInfo* s = find(r.tel, t.serial);
            const unsigned long long ok = s ? s->framesOk : 0, bad = s ? s->framesBad : 0;
            printf("  %4.0f dB: %llu / %llu\n", snr, ok, bad);
            if (snr >= 14.0) CHECK(ok >= 8 && bad <= 2, "%s snr %.0f dB: ok %llu bad %llu", t.type, snr, ok, bad);
        }
    }
    return fails ? 1 : 0;
}

} // namespace sondetest
} // namespace dect2

