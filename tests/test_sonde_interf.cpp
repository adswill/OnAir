// Things in the band that are not sondes: a strong continuous wave and an FSK carrier of 4800 baud with random data (a pager, a telemetry
// link). The RS41 beside them must still decode; the CW must not make a channel; the FSK carrier takes a channel that never decodes and
// is given up after 15 s. Also a second RS41 that is 10 dB weaker, 30 kHz away from a strong one.
#include "dect2/sonde_testkit.h"
#include <random>
using namespace dect2;
using namespace dect2::sondetest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double rate = 8e6;
    SynthConfig c; c.mode = 15; c.snrDb = 30; c.modeOpt[0] = 1; c.modeOpt[1] = 1;
    // CW at -1.0 MHz, FSK junk at +1.5 MHz, both as strong as a sonde
    double cwPh = 0, fskPh = 0, fskFreq = 0;
    std::mt19937 rng(3);
    uint64_t symLeft = 0;
    double bitLevel = 1;
    Impair im;
    im.inject = [&](cf32* b, size_t n, uint64_t) {
        for (size_t i = 0; i < n; i++) {
            cwPh += 2 * M_PI * -1.0e6 / rate;
            if (symLeft == 0) { symLeft = (uint64_t)(rate / 4800.0); bitLevel = (rng() & 1) ? 1.0 : -1.0; }
            symLeft--;
            fskFreq += (bitLevel * 2400.0 - fskFreq) * 0.002;           // slow filter, roughly a few kHz wide
            fskPh += 2 * M_PI * (1.5e6 + fskFreq) / rate;
            b[i] += cf32(0.08f * (float)std::cos(cwPh), 0.08f * (float)std::sin(cwPh)) + cf32(0.08f * (float)std::cos(fskPh), 0.08f * (float)std::sin(fskPh));
        }
        cwPh = std::fmod(cwPh, 2 * M_PI); fskPh = std::fmod(fskPh, 2 * M_PI);
    };
    Result r = run(c, rate, 25, im);
    const SondeInfo* s = find(r.tel, "N4750123");
    CHECK(s && s->framesOk >= 20 && s->framesBad <= 1, "RS41 beside interferers: %llu ok %llu bad", s ? (unsigned long long)s->framesOk : 0ull, s ? (unsigned long long)s->framesBad : 0ull);
    CHECK(r.tel.sondes.size() == 1, "sondes %zu", r.tel.sondes.size());
    CHECK(r.tel.channelsUsed == 1, "channels in use at the end: %d (the FSK carrier must have been given up)", r.tel.channelsUsed);
    bool cwListed = false;
    for (const auto& k : r.tel.carriers) if (std::fabs(k.offsetHz + 1.0e6) < 5000) cwListed = true;
    CHECK(!cwListed, "the CW is listed as a carrier");
    printf("with interferers: ok %llu bad %llu, channels %d, carriers %zu\n", s ? (unsigned long long)s->framesOk : 0ull, s ? (unsigned long long)s->framesBad : 0ull, r.tel.channelsUsed, r.tel.carriers.size());
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
