// FM click suppression (r2-broadcast #18): near the FM threshold the repaired discriminator output must give at least as many RDS groups and a
// better audio SNR than the raw one, and a clean station must not be touched at all.
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct R { double audioSnr; uint64_t groups, clicks; };

static R run(double cnr, double cfo, bool on, bool stereo, uint32_t seed) {
    FmGenConfig c; c.rate = 2e6; c.cnrDb = cnr; c.cfoHz = cfo; c.leftHz = c.rightHz = 1000; c.stereo = stereo; c.seed = seed;
    FmGenerator g(c);
    FmReceiver rx; rx.setSilent(true); rx.setStereoMode(1); rx.configure(c.rate); rx.setStationFinder(false); rx.setClickSuppression(on);
    std::vector<float> audio;
    rx.setAudioTap([&](const float* l, const float*, size_t n) { audio.insert(audio.end(), l, l + n); });
    std::vector<cf32> x;
    const double secs = 12;
    g.generate((size_t)(secs * c.rate), x);
    FmTelemetry t; uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 65536) { rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i)); FmTelemetry u; if (rx.telemetry(u, seq)) { seq = u.seq; t = u; } }
    // the audio SNR: the 1 kHz tone against everything else, after the first 2 s
    double a = 0, b = 0, tot = 0; const size_t s0 = 96000;
    for (size_t i = s0; i < audio.size(); i++) { const double w = 2 * M_PI * 1000 * (double)i / 48000; a += audio[i] * std::cos(w); b += audio[i] * std::sin(w); tot += audio[i] * audio[i]; }
    const double n = (double)(audio.size() - s0), tone = 2 * (a * a + b * b) / n / n, rest = std::max(tot / n - tone, 1e-12);
    return {10 * std::log10(tone / rest), t.rdsGroups, t.clicksFixed};
}

int main(int argc, char** argv) {
    // --quick: the 10 dB point of the fast test tier; the whole sweep is labelled slow
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    const std::vector<double> cfos = quick ? std::vector<double>{0.0} : std::vector<double>{0.0, 12000.0};
    const std::vector<bool> stereos = quick ? std::vector<bool>{false} : std::vector<bool>{false, true};
    const std::vector<double> cnrs = quick ? std::vector<double>{10.0} : std::vector<double>{6.0, 8.0, 10.0, 12.0, 14.0, 18.0};
    double gainAt10 = 0, groupsOff = 0, groupsOn = 0;
    for (double cfo : cfos) for (bool stereo : stereos) for (double cnr : cnrs) {
        const R off = run(cnr, cfo, false, stereo, 7), on = run(cnr, cfo, true, stereo, 7);
        printf("cnr %4.0f cfo %+6.0f %s: audio %5.1f -> %5.1f dB, RDS groups %4llu -> %4llu, clicks %llu\n", cnr, cfo, stereo ? "stereo" : "mono  ", off.audioSnr, on.audioSnr,
               (unsigned long long)off.groups, (unsigned long long)on.groups, (unsigned long long)on.clicks);
        if (cnr == 10 && cfo == 0 && !stereo) gainAt10 = on.audioSnr - off.audioSnr;
        groupsOff += (double)off.groups; groupsOn += (double)on.groups;
        CHECK(on.audioSnr >= off.audioSnr - 0.5, "cnr %.0f: audio got worse (%.1f -> %.1f)", cnr, off.audioSnr, on.audioSnr);
    }
    CHECK(gainAt10 > 0.5, "no audio gain at 10 dB C/N (%.2f dB)", gainAt10);
    CHECK(groupsOn >= groupsOff * 0.99, "RDS groups fell (%.0f -> %.0f)", groupsOff, groupsOn);
    // a clean station is untouched: not one click
    const R clean = run(45, 0, true, true, 3);
    CHECK(clean.clicks == 0, "clean station: %llu clicks repaired", (unsigned long long)clean.clicks);
    printf("audio gain at 10 dB C/N: %.2f dB; RDS groups over the sweep %.0f -> %.0f\n", gainAt10, groupsOff, groupsOn);
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
