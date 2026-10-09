// FreeDV decoder of the HF digital receiver. Without the codec2 library (the normal case): the loader says "not found" cleanly, the
// decoder counts audio and reports no library, and there is no test audio. With DECT2_CODEC2_PATH pointing at the library: a loopback
// (freedv_tx test audio, plus noise, into the decoder) per mode must reach sync and decode speech frames. Quick; no sound.
#include "dect2/hfdig_freedv.h"
#include "dect2/hfdig_gen.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const char* env = getenv("DECT2_CODEC2_PATH");
    const bool want = env && *env;
    std::string path, ver;
    const bool found = hfdigFreedvLibrary(&path, &ver);
    auto dec = makeFreedvDecoder();
    std::vector<float> noise(8000, 0.01f);
    dec->feedAudio(noise.data(), noise.size());
    HfdigFreedvTelemetry t;
    dec->telemetry(t);
    CHECK(t.audioSamples == 8000, "audio %llu", (unsigned long long)t.audioSamples);
    CHECK(t.libFound == found, "telemetry library flag");
    if (!found) {
        CHECK(t.state == 0 && t.mode == -1, "state without library %d", t.state);
        CHECK(makeFreedvTestAudio(SynthConfig()) == nullptr, "test audio without library");
        dec->reset();
        if (want) { printf("FAIL: DECT2_CODEC2_PATH=%s does not load\n", env); return 1; }
        printf("skipped: codec2 not found\n");
        return fails ? 1 : 0;
    }
    printf("codec2: %s (version %s)\n", path.c_str(), ver.c_str());
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    for (int m = 0; m < kFreedvModes; m++) {
        SynthConfig sc;
        sc.modeOpt[1] = m;
        auto gen = makeFreedvTestAudio(sc);
        CHECK(gen != nullptr, "no test audio for %s", freedvModeName(m));
        if (!gen) continue;
        hfdigFreedvSetMode(m + 1);
        auto d = makeFreedvDecoder();
        size_t speech = 0;
        d->setSpeechOut([&](const float*, size_t n) { speech += n; });
        std::vector<float> buf(8000);
        const float sigma = 0.35f / std::pow(10.f, 10.f / 20.f);   // 10 dB SNR (over the full 4 kHz, rms 0.35 signal)
        for (int s = 0; s < 40; s++) {
            gen->generate(buf.data(), buf.size());
            for (float& v : buf) v += sigma * nd(rng);
            d->feedAudio(buf.data(), buf.size());
        }
        d->telemetry(t);
        printf("%s: sync %d snr %.1f dB, speech frames %llu (%zu samples), text \"%s\"\n", freedvModeName(m), (int)t.sync[m], t.snrDb[m],
               (unsigned long long)t.speechFrames, speech, t.text.c_str());
        CHECK(t.open[m] && t.sync[m], "%s: no sync", freedvModeName(m));
        CHECK(t.text.compare(0, 5, "ONAIR") == 0, "%s: text \"%s\"", freedvModeName(m), t.text.c_str());
        CHECK(t.speechFrames > 0 && speech > 0, "%s: no speech", freedvModeName(m));
    }
    hfdigFreedvSetMode(0);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
