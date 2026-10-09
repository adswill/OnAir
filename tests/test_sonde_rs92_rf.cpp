// RS92 through the whole receiver: serial, frame number, calibration progress, announced frequency, no position, impairments, SNR sweep.
#include "dect2/sonde_testkit.h"
using namespace dect2;
using namespace dect2::sondetest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static SynthConfig cfg(double snr = 30) {
    SynthConfig c; c.mode = 15; c.snrDb = snr; c.modeOpt[0] = 16; c.modeOpt[1] = 1; return c;
}

int main() {
    for (double rate : {8e6, 10e6, 20e6}) {
        for (double cfo : {0.0, -20000.0, 20000.0}) {   // 50 ppm of 405 MHz is 20 kHz
            SynthConfig c = cfg(); c.cfoHz = cfo;
            Result r = run(c, rate, 9);
            const SondeInfo* s = find(r.tel, "P4953934");
            CHECK(s != nullptr, "rate %.0f cfo %.0f: not found", rate, cfo);
            if (!s) continue;
            CHECK(s->framesOk >= 6 && s->framesBad <= 1, "rate %.0f cfo %.0f: ok %llu bad %llu", rate, cfo, (unsigned long long)s->framesOk, (unsigned long long)s->framesBad);
            CHECK(!s->hasPos && !s->hasTemp && s->sats == 9 && s->type == "RS92", "fields");
            CHECK(std::fabs(s->offsetHz - (2.3e6 + cfo)) < 600.0, "offset %.0f", s->offsetHz);
            CHECK(s->calTotal == 32 && s->calDone >= 5, "calibration %d/%d", s->calDone, s->calTotal);
            CHECK(std::fabs(s->freqHz - 405.3e6 - cfo) < 700.0, "frequency %.0f", s->freqHz);
        }
    }
    for (double sro : {-100.0, 100.0}) { SynthConfig c = cfg(); c.sroPpm = sro; Result r = run(c, 8e6, 9); const SondeInfo* s = find(r.tel, "P4953934"); CHECK(s && s->framesOk >= 6, "sro %+.0f", sro); }
    printf("snr sweep, frames ok over 15 s:\n");
    for (double snr : {14.0, 10.0, 9.0, 8.0}) {
        Result r = run(cfg(snr), 8e6, 15);
        const SondeInfo* s = find(r.tel, "P4953934");
        const unsigned long long ok = s ? s->framesOk : 0, bad = s ? s->framesBad : 0;
        printf("  %4.0f dB: ok %llu bad %llu\n", snr, ok, bad);
        if (snr >= 10.0) CHECK(ok >= 12 && bad <= 1, "snr %.0f dB: ok %llu bad %llu", snr, ok, bad);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
