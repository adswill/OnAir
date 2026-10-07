// DRM receiver under impairments: noise, carrier and clock offset, 8 bit quantisation, DC offset, IQ imbalance, interferers, impulse noise, stream faults,
// echoes and the fading channels of Annex B.1, a reset in the middle. Every line prints the numbers; the checks are set where the receiver is expected to
// work with some margin, not at the limit (the limits are in docs/modes/drm.md).
#include "data/drm/testkit.h"
#include <cstdio>
#include <string>
using namespace drmtest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// audio super frames expected from a run of `secs` seconds that has locked after 6 s (long interleaver)
static double share(const Result& r, double secs) { return (double)r.exact / std::max(1.0, (secs - 6.0) / 0.4); }

static void line(const char* what, const Result& r, double secs) {
    const DrmTelemetry& t = r.tel;
    printf("%-40s state %d lock %5.1f s  FAC %llu/%llu SDC %llu/%llu  exact %3llu bad %3llu gaps %2llu (%3.0f%%)  SNR %5.1f  CFO %+8.1f  SRO %+6.1f  lost %llu  %5.1fx\n", what, t.state, r.lockSecs,
           (unsigned long long)t.facOk, (unsigned long long)t.facBad, (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad, (unsigned long long)r.exact, (unsigned long long)r.bad,
           (unsigned long long)r.gaps, 100.0 * share(r, secs), t.snrDb, t.cfoHz, t.sroPpm, (unsigned long long)r.lostLocks, r.rt());
}

static Result go(const char* what, const Sig& s, double rate, double secs, std::function<Gen(Gen)> wrap = nullptr, RunOpt o = RunOpt()) {
    o.secs = secs;
    Gen g = synthGen(s, rate);
    if (wrap) g = wrap(g);
    const Result r = runRx(g, rate, o);
    line(what, r, secs);
    return r;
}

// the lowest SNR at which at least 90 % of the audio frames come out exact, in 1 dB steps
static double threshold(const char* what, Sig s, double hi, double lo) {
    double found = 1e9;
    for (double snr = hi; snr >= lo; snr -= 1.0) {
        s.snrDb = snr;
        const double secs = 20;
        RunOpt o; o.secs = secs;
        const Result r = runRx(synthGen(s, 192000), 192000, o);
        char b[80];
        snprintf(b, sizeof b, "%s %.0f dB", what, snr);
        line(b, r, secs);
        if (share(r, secs) >= 0.9) found = snr; else break;
    }
    printf("  -> %s: 90%% of the audio frames at %.0f dB and above\n", what, found);
    return found;
}

static void testNoise() {
    Sig b64; b64.mode = 2;                                  // B 10 kHz 64-QAM, protection level 1
    const double t64 = threshold("B 64-QAM PL1", b64, 20, 15);
    CHECK(t64 <= 19, "64-QAM needs %.0f dB (expected 19 or less)", t64);
    Sig b16 = b64; b16.qam16 = 1;                           // 16-QAM protection level 0
    const double t16 = threshold("B 16-QAM PL0", b16, 13, 8);
    CHECK(t16 <= 13, "16-QAM needs %.0f dB (expected 13 or less)", t16);
    Sig a16 = b16; a16.mode = 1;
    const double ta = threshold("A 16-QAM PL0", a16, 12, 7);
    CHECK(ta <= 13, "mode A 16-QAM needs %.0f dB", ta);
}

static void testOffsets() {
    for (double cfo : {-10000.0, -2500.0, -123.4, 7.3, 1000.0, 5000.0, 12000.0}) {
        Sig s; s.snrDb = 28; s.cfoHz = cfo;
        char b[64]; snprintf(b, sizeof b, "carrier offset %+.1f Hz (192 kHz)", cfo);
        const Result r = go(b, s, 192000, 18);
        CHECK(r.tel.state == 2 && share(r, 18) > 0.85 && r.bad == 0, "%s: not clean", b);
        CHECK(std::fabs(r.tel.cfoHz - cfo) < 3.0, "%s: reported %.1f Hz", b, r.tel.cfoHz);
    }
    for (double sro : {-100.0, -30.0, 30.0, 100.0}) {
        Sig s; s.snrDb = 28; s.sroPpm = sro;
        char b[64]; snprintf(b, sizeof b, "clock offset %+.0f ppm (192 kHz)", sro);
        const Result r = go(b, s, 192000, 18);
        CHECK(r.tel.state == 2 && share(r, 18) > 0.85 && r.bad == 0, "%s: not clean", b);
        CHECK(std::fabs(r.tel.sroPpm - sro) < 8.0, "%s: reported %.1f ppm", b, r.tel.sroPpm);
    }
    // a clock offset carries the symbols across the delay range the channel estimate can express (mode A: 144 samples) after 15 to 20 s: the window must follow
    for (double sro : {-100.0, 100.0}) {
        Sig s; s.snrDb = 28; s.sroPpm = sro; s.mode = 1;
        char b[64]; snprintf(b, sizeof b, "mode A, clock offset %+.0f ppm, 50 s", sro);
        const Result r = go(b, s, 192000, 50);
        CHECK(r.tel.state == 2 && r.bad == 0 && r.lostLocks == 0 && r.exact >= 100, "%s: %llu exact %llu bad, %llu locks lost", b, (unsigned long long)r.exact, (unsigned long long)r.bad, (unsigned long long)r.lostLocks);
        CHECK(std::fabs(r.tel.sroPpm - sro) < 8.0, "%s: reported %.1f ppm", b, r.tel.sroPpm);
    }
    for (double sro : {-300.0, 300.0}) {   // far more than any radio has: for the record
        Sig s; s.snrDb = 28; s.sroPpm = sro;
        char b[64]; snprintf(b, sizeof b, "clock offset %+.0f ppm (192 kHz)", sro);
        go(b, s, 192000, 18);
    }
    {   // both together, at the radio rate
        Sig s; s.snrDb = 28; s.sroPpm = 60; s.cfoHz = 3333.3;
        const Result r = go("CFO +3333 Hz, +60 ppm (2 Msps)", s, 2e6, 16);
        CHECK(r.tel.state == 2 && share(r, 16) > 0.8 && r.bad == 0, "combined offsets: not clean");
    }
}

static void testRadio() {
    Sig s; s.snrDb = 30;
    // levels of a real radio at 8 bit: -30, -20 and -10 dBFS rms
    for (double lvl : {-30.0, -20.0, -10.0}) {
        char b[64]; snprintf(b, sizeof b, "8 bit, %.0f dBFS rms (2 Msps)", lvl);
        const Result r = go(b, s, 2e6, 16, [&](Gen g) { return withQuantize8(g, std::pow(10.0, lvl / 20.0)); });
        CHECK(r.tel.state == 2 && share(r, 16) > 0.8 && r.bad == 0, "%s: not clean", b);
    }
    {
        const Result r = go("DC offset 0.05 + 0.03j, 8 bit", s, 2e6, 16, [&](Gen g) { return withQuantize8(withDc(g, cf32(0.05f, 0.03f)), 0.05); });
        CHECK(r.tel.state == 2 && share(r, 16) > 0.8 && r.bad == 0, "DC offset: not clean");
    }
    {
        const Result r = go("IQ imbalance 1 dB, 5 degrees", s, 2e6, 16, [&](Gen g) { return withIqImbalance(g, 1.0, 5.0); });
        CHECK(r.tel.state == 2 && share(r, 16) > 0.8 && r.bad == 0, "IQ imbalance: not clean");
    }
    {   // a carrier inside the 48 kHz channel but outside the signal, and one outside it that the filter has to remove
        const Result r = go("CW +12 kHz, 20 dB above (2 Msps)", s, 2e6, 16, [&](Gen g) { return withTone(g, 2e6, 12000, 20); });
        CHECK(r.tel.state == 2 && share(r, 16) > 0.8 && r.bad == 0, "in-channel carrier: not clean");
        const Result r2 = go("CW +60 kHz, 40 dB above (2 Msps)", s, 2e6, 16, [&](Gen g) { return withTone(g, 2e6, 60000, 40); });
        CHECK(r2.tel.state == 2 && share(r2, 16) > 0.8 && r2.bad == 0, "out-of-channel carrier: not clean");
        const Result r3 = go("CW +400 kHz, 50 dB above (2 Msps)", s, 2e6, 16, [&](Gen g) { return withTone(g, 2e6, 400000, 50); });
        CHECK(r3.tel.state == 2 && share(r3, 16) > 0.8 && r3.bad == 0, "far carrier: not clean");
    }
    {   // bursts of impulse noise: 1 ms every 0.7 s, five times the rms of the signal
        const Result r = go("impulse noise 1 ms / 0.7 s, +14 dB", s, 192000, 20, [&](Gen g) { return withImpulses(g, 134400, 192, 0.2 * 5.0, 3); });
        CHECK(r.tel.state == 2 && share(r, 20) > 0.5, "impulse noise: lost the signal");
    }
}

// the receiver has to come back after the radio drops samples
static void testFaults() {
    Sig s; s.snrDb = 30;
    struct F { const char* name; std::vector<std::pair<double, double>> cuts, zeros; };
    const F faults[] = {
        {"6 ms of zeros at 10 s", {}, {{10.0, 0.006}}},
        {"20 ms of zeros at 10 s", {}, {{10.0, 0.020}}},
        {"3 ms of samples lost at 10 s", {{10.0, 0.003}}, {}},
        {"40 ms of samples lost at 10 s", {{10.0, 0.040}}, {}},
        {"2 s of zeros at 10 s", {}, {{10.0, 2.0}}},
    };
    for (const F& f : faults) {
        RunOpt o; o.secs = 30; o.cuts = f.cuts; o.zeros = f.zeros;
        const Result r = runRx(synthGen(s, 192000), 192000, o);
        line(f.name, r, 30);
        // The cells of a frame are spread over 2 s (long interleaver), and nothing in this test checks the audio CRC that flags a damaged frame, so
        // frames may be wrong for a while after the fault. 6 s after its end everything must be exact again.
        const double end = 10.0 + (f.zeros.empty() ? f.cuts[0].second : f.zeros[0].second) + 6.0;
        CHECK(r.tel.state == 2 && r.tel.dataValid, "%s: no lock at the end", f.name);
        CHECK(r.badAfter(end) == 0 && r.exactAfter(end) >= 12, "%s: %llu wrong and %llu exact frames after %.1f s", f.name, (unsigned long long)r.badAfter(end), (unsigned long long)r.exactAfter(end), end);
    }
    {   // reset() in the middle: forgets, locks again, the telemetry sequence only grows
        RunOpt o; o.secs = 24; o.resetAtFrame = 12;
        const Result r = runRx(synthGen(s, 192000), 192000, o);
        line("reset() after 12 frames", r, 24);
        CHECK(r.tel.state == 2 && r.tel.dataValid, "reset: no lock afterwards");
        CHECK(r.seqBackwards == 0, "reset: telemetry sequence went back");
        CHECK(r.exact >= 20 && r.bad == 0, "reset: %llu exact %llu bad", (unsigned long long)r.exact, (unsigned long long)r.bad);
    }
}

// echoes and the channels of Annex B.1 (profile 1 AWGN, 2 Rice with delay, 3 US consortium, 4 CCIR poor, 5 and 6 worse)
static void testChannels() {
    for (double delay : {40.0, 150.0, 200.0}) {
        Sig s; s.snrDb = 30; s.echoDb = 6; s.echoDelay = (int)delay;
        char b[64]; snprintf(b, sizeof b, "echo -6 dB at %.1f ms (192 kHz)", delay / 48.0);
        const Result r = go(b, s, 192000, 20);
        CHECK(r.tel.state == 2 && share(r, 20) > 0.8 && r.bad == 0, "%s: not clean", b);
    }
    {   // an echo at the very end of the guard interval (256 samples), and one beyond it: the receiver stays locked, the data suffer
        Sig s; s.snrDb = 30; s.echoDb = 6; s.echoDelay = 245;
        go("echo -6 dB at 5.1 ms (end of the guard)", s, 192000, 20);
        s.echoDb = 10; s.echoDelay = 300;
        go("echo -10 dB at 6.3 ms (beyond the guard)", s, 192000, 20);
    }
    for (int ch = 1; ch <= 6; ch++) {
        Sig s; s.snrDb = 32; s.channel = ch; s.qam16 = 1;
        char b[64]; snprintf(b, sizeof b, "B 16-QAM channel %d, 32 dB", ch);
        const Result r = go(b, s, 192000, 30);
        if (ch <= 3) CHECK(r.tel.state == 2 && share(r, 30) > 0.7, "%s: not decoding (%.0f%%)", b, 100 * share(r, 30));
    }
}

int main() {
    testNoise();
    testOffsets();
    testRadio();
    testFaults();
    testChannels();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
