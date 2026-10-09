// HF digital skeleton through the engine: the synthetic source plays the mode's test signal, the receiver makes the 8 kHz sideband audio
// and hands it to all three decoders, the engine reports telemetry through latestRx() with the mode's standard. About 2 s; quick and silent.
#include "dect2/engine.h"
#include "dect2/hfdig_gen.h"
#include "dect2/hfdig_rx.h"
#include "dect2/hfdig_tel.h"
#include "dect2/modes.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const ModeTuning mt = hfdigTuning();
    CHECK(mt.stdMode == 27 && modeTuningById("hfdig") && modeTuningById("hfdig")->stdMode == 27, "tuning table");
    // the generator for every test signal choice (noise alone while the decoders have no test audio)
    for (int which = 0; which < 3; which++) {
        SynthConfig sc;
        sc.modeOpt[0] = which;
        auto syn = makeModeSynth(27, sc, 2e6);
        CHECK(syn != nullptr, "no generator for test signal %d", which);
        if (!syn) continue;
        std::vector<cf32> v(20000);
        syn->generate(v.data(), v.size());
        double p = 0;
        bool finite = true;
        for (const cf32& s : v) { p += std::norm(s); finite &= std::isfinite(s.real()) && std::isfinite(s.imag()); }
        p /= (double)v.size();
        CHECK(finite && p > 1e-5 && p < 0.05, "test signal %d: power %g", which, p);
    }
    CHECK(makeHfdigTestAudio(3, SynthConfig()) == nullptr, "test audio 3 should not exist");

    Engine e;
    e.hfdig().setSilent(true);
    std::atomic<size_t> tapped{0};
    e.hfdig().setAudioTap([&](const float* x, size_t n) {
        bool ok = true;
        for (size_t i = 0; i < n; i++) ok &= x[i] >= -1.f && x[i] <= 1.f;
        if (ok) tapped += n;
    });
    DeviceInfo dev;                                          // the synthetic source
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 27;
    FileOptions fo;
    e.setStandard(27);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 26, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 2.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            reports++;
            if (rx.standard != 26) wrong++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.stop();
    const HfdigTelemetry& h = rx.hfdig;
    printf("HF digital: %d reports in 2 s, last: %s\n  audio to RTTY %llu, SSTV %llu, FreeDV %llu samples, tap %zu\n", reports, hfdigSummary(h).c_str(),
           (unsigned long long)h.rtty.audioSamples, (unsigned long long)h.sstv.audioSamples, (unsigned long long)h.freedv.audioSamples, tapped.load());
    CHECK(reports >= 3, "%d reports", reports);
    CHECK(wrong == 0, "%d reports with another standard", wrong);
    CHECK(rx.rateOk, "the engine says the rate is too low");
    CHECK(h.inputRate > 0 && h.levelDb > -60 && h.levelDb < -10, "input %.0f, level %.1f dBFS", h.inputRate, h.levelDb);
    CHECK(h.audioDb > -150, "sideband level %.1f dBFS", h.audioDb);
    // all three decoders get the same audio, about 8000 samples a second
    CHECK(h.rtty.audioSamples >= 4000 && h.rtty.audioSamples == h.sstv.audioSamples && h.sstv.audioSamples == h.freedv.audioSamples,
          "audio %llu %llu %llu", (unsigned long long)h.rtty.audioSamples, (unsigned long long)h.sstv.audioSamples, (unsigned long long)h.freedv.audioSamples);
    CHECK(tapped.load() >= h.rtty.audioSamples, "tap %zu", tapped.load());
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
