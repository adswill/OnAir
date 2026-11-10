// Satellite detector modes against their own generators, simulated only: noise sweep, noise-only false alarms, every system's plumbing.
#include "dect2/satmisc/satdetect_live.h"
#include "dect2/satmisc/satdetect_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include "../common/test_long.h"
using namespace dect2;
using namespace dect2::satmisc;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static SatDetectTelemetry run(int std_, double rate, double sec, double snrDb, bool signal, double cfo = 0) {
    SatDetectReceiver rx; rx.setSystem(std_); rx.configure(rate);
    const SatSystem* s = satSystem(std_);
    rx.setCenterHz(s->defMhz * 1e6);
    SynthConfig c; c.snrDb = snrDb; c.modeOpt[0] = signal ? 0 : 1; c.cfoHz = cfo;
    auto gen = makeSatDetectSynth(std_, c, rate);
    const size_t block = 16384;
    std::vector<cf32> x(block);
    for (double t = 0; t < sec; t += (double)block / rate) { gen->generate(x.data(), block); rx.feed(x.data(), block); }
    SatDetectTelemetry t; rx.telemetry(t, 0);
    return t;
}

int main() {
    // every entry describes itself and its numbers stay in the block
    for (const auto& s : satSystems()) {
        CHECK(s.stdMode >= 552 && s.stdMode <= 581, "%s std %d", s.id, s.stdMode);
        ModeTuning t = satDetectTuning(s.stdMode);
        CHECK(t.stdMode == s.stdMode && t.defMhz > 0 && t.sampleRate > 0, "%s tuning", s.id);
    }
    // Carrier with a Doppler curve: found over a range of SNRs and offsets, on its catalogue channel
    // by default only the checked SNR points nearest the limit and a shorter skimmer capture (its three channels all key in the first 2 s); the long run adds the easier and the print-only points
    const bool L = testLong();
    for (double snr : {25.0, 18.0, 12.0}) {
        if (!L && snr != 18.0) continue;
        SatDetectTelemetry t = run(560, 1e6, 10, snr, true);
        printf("carrier snr %.0f dB: %s\n", snr, sdSummary(t).c_str());
        if (snr >= 18) CHECK(!t.lines.empty(), "no line at %.0f", snr);
    }
    { SatDetectTelemetry t = run(560, 1e6, 10, 30, true, 1500); CHECK(!t.lines.empty() && t.lines[0].channel == "Tsikada / Parus 150 MHz", "channel %s", t.lines.empty() ? "none" : t.lines[0].channel.c_str()); }
    { SatDetectTelemetry t = run(560, 1e6, 10, 30, false); CHECK(t.lines.empty(), "%zu false lines in noise", t.lines.size()); }
    // Channel skimmer
    { SatDetectTelemetry t = run(554, 10e6, testLongPick(3.0, 9.0), 30, true); printf("channels: %zu, %s\n", t.channels.size(), sdSummary(t).c_str()); CHECK(t.channels.size() >= 3 && t.channels.size() < 12, "channels %zu", t.channels.size()); }
    { SatDetectTelemetry t = run(554, 10e6, testLongPick(2.0, 4.0), 30, false); CHECK(t.channels.size() < 5, "%zu false channels in noise", t.channels.size()); }
    // Starlink frame gap at several SNRs in the capture
    for (double snr : {0.0, -5.0, -10.0, -15.0}) {
        if (!L && snr != -5.0) continue;
        SatDetectTelemetry t = run(556, 20e6, 2.0, snr, true);
        printf("starlink snr %.0f dB: level %.1f detected %d (rate %.3f)\n", snr, t.frames[0].levelDb, t.frames[0].detected, t.frames[0].measuredHz);
        if (snr >= -5) CHECK(t.frames[0].detected, "not detected at %.0f", snr);
    }
    { SatDetectTelemetry t = run(556, 20e6, 2.0, 0, false); printf("starlink noise: level %.1f\n", t.frames[0].levelDb); CHECK(!t.frames[0].detected, "false detection on noise, %.1f dB", t.frames[0].levelDb); }
    // OneWeb repeated sequence
    for (double snr : {5.0, 0.0, -5.0}) {
        if (!L && snr != 0.0) continue;
        SatDetectTelemetry t = run(557, 20e6, 1, snr, true);
        printf("oneweb snr %.0f dB: level %.1f detected %d\n", snr, t.frames[0].levelDb, t.frames[0].detected);
        if (snr >= 0) CHECK(t.frames[0].detected, "not detected at %.0f", snr);
    }
    { SatDetectTelemetry t = run(557, 20e6, 1, 0, false); printf("oneweb noise: level %.1f\n", t.frames[0].levelDb); CHECK(!t.frames[0].detected, "false detection on noise, %.1f dB", t.frames[0].levelDb); }
    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
