// Gain control tests: the stage split, ADC classification, and AutoGain / GainSweep closed loop on the synthetic source
// (whose level follows the LNA/VGA/amp gains and clips like an 8-bit ADC).
#include "dect2/engine.h"
#include "dect2/gain.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// The synthetic signal runs at half speed, so a slow machine does not change the outcome. The AGC and the sweep are given
// signal time (wall time x pace), which is what they would see on real hardware.
static const double kPace = 0.5;

static TuneSettings synthTune(GainSetting g) {
    TuneSettings t;
    t.bandwidthMhz = 8;
    t.synth.snrDb = 32;
    t.synth.tx.s2field1 = 1;   // 8K carriers: the AGC does not care about the mode, and a slow machine can keep up
    t.synth.tx.giIdx = 3;
    t.synth.gainModel = true;
    t.synth.pace = kPace;
    t.lnaDb = g.lna; t.vgaDb = g.vga; t.ampOn = g.amp;
    return t;
}

static void runAgc(const char* name, GainSetting start, double secs) {
    Engine e;
    DeviceInfo dev;
    TuneSettings t = synthTune(start);
    FileOptions fo;
    e.setStandard(1); // these tests are about DVB-T2 lock
    if (!e.start(dev, t, fo)) { printf("FAIL: cannot start\n"); fails++; return; }
    AutoGain agc;
    GainSetting g = start;
    SpectrumFrame sf;
    uint64_t seq = 0;
    int changes = 0;
    const double t0 = now();
    bool healthy = false;
    // The simulated time is the wall time scaled by kPace; a slow computer sees fewer spectra in it. So after the planned time the loop goes on
    // for a while longer (at most 12 simulated seconds) as long as the level is still not in the healthy range.
    while ((now() - t0) * kPace < secs || (!healthy && (now() - t0) * kPace < secs + 12)) {
        if (e.latestSpectrum(sf, seq)) {
            seq = sf.seq;
            const AdcStatus cur = classifyAdc(sf.stats.rmsDbfs, sf.stats.peak, sf.stats.clipFraction);
            healthy = (cur == AdcStatus::Good || cur == AdcStatus::High) && sf.stats.clipFraction < 0.002f;
            if (agc.update((now() - t0) * kPace, sf.stats, g)) { t.lnaDb = g.lna; t.vgaDb = g.vga; t.ampOn = g.amp; e.retune(t); changes++; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    e.latestSpectrum(sf, 0);
    AdcStatus st = classifyAdc(sf.stats.rmsDbfs, sf.stats.peak, sf.stats.clipFraction);
    printf("%s: start total %d dB -> LNA %d VGA %d amp %d (total %d dB), level %.1f dBFS, clip %.3f%%, status %s, %d changes\n", name, start.total(), g.lna, g.vga, g.amp, g.total(), sf.stats.rmsDbfs, sf.stats.clipFraction * 100, adcStatusName(st), changes);
    CHECK(st == AdcStatus::Good || st == AdcStatus::High, "%s: ADC not in a healthy range", name);
    CHECK(sf.stats.clipFraction < 0.002f, "%s: still clipping", name);
    CHECK(changes <= 14, "%s: AGC kept hunting (%d changes)", name, changes);
    e.stop();
}

int main() {
    // stage split
    for (int tot : {0, 10, 36, 62, 66, 80, 100, 116}) {
        GainSetting g = gainForTotal(tot);
        CHECK(g.lna % 8 == 0 && g.lna >= 0 && g.lna <= 40 && g.vga % 2 == 0 && g.vga >= 0 && g.vga <= 62, "bad split for %d", tot);
        CHECK(std::abs(g.total() - tot) <= 4 || tot > 112, "total %d mapped to %d", tot, g.total());
    }
    CHECK(gainForTotal(66).lna == 32 && gainForTotal(66).vga == 20 && gainForTotal(66).amp, "66 dB should be LNA 32 / VGA 20 / amp (the best setting found on air)");
    // classification
    CHECK(classifyAdc(-15, 0.6, 0) == AdcStatus::Good, "good");
    CHECK(classifyAdc(-15, 1.0, 0.01) == AdcStatus::Overload, "overload");
    CHECK(classifyAdc(-30, 0.1, 0) == AdcStatus::Low, "low");
    CHECK(classifyAdc(-60, 0.01, 0) == AdcStatus::NoSignal, "no signal");
    CHECK(classifyAdc(-8, 0.8, 0) == AdcStatus::High, "high");

    runAgc("too hot ", {40, 50, true}, 14);
    runAgc("too cold", {0, 4, false}, 14);

    // sweep: the winner must not clip and must have the best SNR among the non-clipping candidates
    {
        Engine e;
        DeviceInfo dev;
        TuneSettings t = synthTune({32, 20, true});
        FileOptions fo;
        e.setStandard(1);
        e.start(dev, t, fo);
        GainSweep sw;
        GainSetting g{32, 20, true};
        const double sw0 = now();
        sw.start(0);
        SpectrumFrame sf; uint64_t seq = 0;
        RxTelemetry rx; uint64_t rseq = 0;
        double snr = 0; bool locked = false;
        const double t0 = now();
        while (sw.active() && (now() - t0) * kPace < 120) {
            if (e.latestSpectrum(sf, seq)) seq = sf.seq;
            if (e.latestRx(rx, rseq)) { rseq = rx.seq; }
            locked = rx.dataValid; snr = rx.dataSnrDb;
            GainSweep::Sample s; s.snrDb = snr; s.locked = locked; s.clip = sf.stats.clipFraction; s.rms = sf.stats.rmsDbfs; s.peak = sf.stats.peak;
            if (sw.update((now() - sw0) * kPace, s, g)) { t.lnaDb = g.lna; t.vgaDb = g.vga; t.ampOn = g.amp; e.retune(t); }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        CHECK(!sw.active(), "sweep did not finish");
        for (auto& x : sw.entries()) printf("  LNA %2d VGA %2d amp %d: SNR %6.1f dB (%d samples) level %6.1f dBFS clip %.3f%%\n", x.g.lna, x.g.vga, x.g.amp, x.snrDb, x.n, x.rms, x.clip * 100);
        printf("sweep %s\n", sw.summary().c_str());
        double bs = -1e9;
        for (auto& x : sw.entries()) if (x.clip <= 0.001f) bs = std::max(bs, x.snrDb);
        for (auto& x : sw.entries()) if (x.g == sw.best()) { CHECK(x.clip <= 0.001f, "winner clips"); CHECK(x.snrDb >= bs - 0.01, "winner is not the best SNR"); }
        e.stop();
    }
    printf(fails ? "gain tests FAILED\n" : "gain tests passed\n");
    return fails ? 1 : 0;
}
