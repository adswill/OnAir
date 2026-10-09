// RS41 through the whole receiver: generator -> IQ at several rates -> carrier search, channel, decoder. Rates, carrier offsets,
// clock offsets, chunk sizes, 8-bit samples, DC offset, a gap, reset(), and the SNR at which frames start to fail.
#include "dect2/sonde_testkit.h"
#include "impair.h"
using namespace dect2;
using namespace dect2::sondetest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static SynthConfig cfg1(double snr = 30) {
    SynthConfig c; c.mode = 15; c.snrDb = snr; c.modeOpt[0] = 1; c.modeOpt[1] = 1; return c;
}

// the first RS41 of the test signal: frames decoded, position follows the flight (12 km + 5 m/s)
static void checkRs41(const Result& r, double secs, double minRatio, const char* what) {
    const SondeInfo* s = find(r.tel, "N4750123");
    CHECK(s != nullptr, "%s: sonde not found", what);
    if (!s) return;
    const double expect = std::floor(secs) - 2;       // the first frames go to finding the carrier
    CHECK(s->framesOk >= expect * minRatio, "%s: %llu frames ok of about %.0f (bad %llu)", what, (unsigned long long)s->framesOk, expect, (unsigned long long)s->framesBad);
    CHECK(s->hasPos && s->hasTime, "%s: no position", what);
    int bad = 0;
    for (const auto& tp : s->track) {
        const double t = tp.unixT - 1780272000.0;
        if (std::fabs(tp.altM - (12000 + 5.0 * t)) > 3.0 || std::fabs(tp.lat - 25.20) > 1e-4) bad++;
    }
    CHECK(bad == 0 && s->track.size() >= 3, "%s: %d track points off the flight (of %zu)", what, bad, s->track.size());
    CHECK(s->hasVel && std::fabs(s->vSpeed - 5.0) < 0.05 && std::fabs(s->hSpeed - 10.0) < 0.05, "%s: velocity %.2f %.2f", what, s->vSpeed, s->hSpeed);
}

int main() {
    // rates (non-integer ratios included)
    for (double rate : {2e6, 2.4e6, 3.2e6, 8e6, 10e6, 12.288e6, 20e6}) {
        const double secs = 8;
        Result r = run(cfg1(), rate, secs);
        char w[64]; snprintf(w, sizeof w, "rate %.1f Msps", rate / 1e6);
        checkRs41(r, secs, 0.95, w);
        printf("%s: rtf %.0f, frames ok %llu bad %llu\n", w, r.rtf, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad);
        if (!kSanitized) CHECK(r.rtf > 5.0, "%s: real-time factor %.1f", w, r.rtf);
        const SondeInfo* s = find(r.tel, "N4750123");
        if (s) CHECK(std::fabs(s->offsetHz) < 600.0, "%s: carrier at %.0f Hz", w, s->offsetHz);
    }
    // carrier offset (sonde TCXO and radio error), and clock offset
    for (double cfo : {-20000.0, -10000.0, -4000.0, 4000.0, 10000.0, 20000.0}) {   // 50 ppm of 403 MHz is 20 kHz
        SynthConfig c = cfg1(); c.cfoHz = cfo;
        Result r = run(c, 8e6, 8);
        char w[64]; snprintf(w, sizeof w, "cfo %+.0f Hz", cfo);
        checkRs41(r, 8, 0.95, w);
        const SondeInfo* s = find(r.tel, "N4750123");
        if (s) CHECK(std::fabs(s->offsetHz - cfo) < 500.0, "%s: measured %.0f", w, s->offsetHz);
    }
    for (double sro : {-100.0, -50.0, 50.0, 100.0}) {
        SynthConfig c = cfg1(); c.sroPpm = sro; c.cfoHz = 3000;
        Result r = run(c, 8e6, 12);
        char w[64]; snprintf(w, sizeof w, "sro %+.0f ppm", sro);
        checkRs41(r, 12, 0.95, w);
    }
    {   // combined (REAL_WORLD_CHECKLIST.md): 50 ppm low, a sample clock 80 ppm fast, an echo, 8-bit clipping
        SynthConfig c = cfg1(); c.sroPpm = 80; c.cfoHz = -20000;
        Impair im; im.quant8 = true;
        im.inject = [](cf32* x, size_t n, uint64_t) { std::vector<cf32> v(x, x + n); impair::echo(v, 5, -8, 2.0); impair::clip8(v, 3.0); std::copy(v.begin(), v.end(), x); };
        checkRs41(run(c, 8e6, 12, im), 12, 0.95, "combined -20 kHz +80 ppm echo 8 bit");
    }
    // chunk sizes
    for (size_t chunk : {(size_t)7, (size_t)4096, (size_t)65536}) {
        Impair im; im.chunk = chunk;
        Result r = run(cfg1(), 2e6, chunk == 7 ? 4 : 8, im);
        char w[64]; snprintf(w, sizeof w, "chunk %zu", chunk);
        checkRs41(r, chunk == 7 ? 4 : 8, 0.9, w);
    }
    { // 8-bit samples, DC offset, gap, reset
        Impair im; im.quant8 = true; im.dcI = 0.03f; im.dcQ = -0.02f;
        Result r = run(cfg1(25), 8e6, 10, im);
        checkRs41(r, 10, 0.95, "8 bit and DC offset");
        Impair g; g.gapAtS = 4.3; g.gapLenS = 0.02;
        Result rg = run(cfg1(), 8e6, 10, g);
        checkRs41(rg, 10, 0.85, "20 ms gap");
        const SondeInfo* s = find(rg.tel, "N4750123");
        if (s) CHECK(s->framesOk >= 7, "gap: %llu frames", (unsigned long long)s->framesOk);
        Impair rs; rs.resetAtS = 5.0;
        Result rr = run(cfg1(), 8e6, 10, rs);
        const SondeInfo* sr = find(rr.tel, "N4750123");
        CHECK(sr && sr->framesOk >= 3 && sr->framesOk <= 5, "after reset: %llu frames", sr ? (unsigned long long)sr->framesOk : 0ull);
        CHECK(rr.tel.seq > 0, "seq");
    }
    // SNR in 10 kHz: where frames fail
    printf("snr sweep, frames ok over 15 s (about 14 sent):\n");
    for (double snr : {14.0, 10.0, 9.0, 8.0, 7.0}) {
        Result r = run(cfg1(snr), 8e6, 15);
        const SondeInfo* s = find(r.tel, "N4750123");
        const unsigned long long ok = s ? s->framesOk : 0, bad = s ? s->framesBad : 0;
        printf("  %4.0f dB: ok %llu bad %llu\n", snr, ok, bad);
        if (snr >= 10.0) CHECK(ok >= 12 && bad <= 1, "snr %.0f dB: ok %llu bad %llu", snr, ok, bad);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
