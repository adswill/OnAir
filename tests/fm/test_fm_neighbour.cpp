// First-adjacent cancellation: a strong neighbour 200 kHz (and 400 kHz) from the wanted station; RDS and the audio with the canceller on and off. A station alone must be
// untouched (nothing detected).
#include "dect2/fm/fm_gen.h"
#include "dect2/fm/fm_rx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct R { uint64_t groups; double audio; bool ps; float nDb; };

static R run(double neighbourDb, double offset, bool on, bool stereoN) {
    const double rate = 2e6, secs = 10;
    FmGenConfig w; w.rate = rate; w.cnrDb = 200; w.leftHz = w.rightHz = 1000; w.stereo = false; w.ps = "WANTED  "; w.pi = 0x7A11;
    FmGenConfig n = w; n.cfoHz = offset; n.leftHz = 2300; n.rightHz = 700; n.stereo = stereoN; n.ps = "NEIGHBOR"; n.pi = 0x7B22; n.seed = 9;
    FmGenerator gw(w), gn(n);
    std::vector<cf32> a, b;
    gw.generate((size_t)(secs * rate), a); gn.generate((size_t)(secs * rate), b);
    const float g = (float)std::pow(10.0, neighbourDb / 20);
    std::mt19937 r(4); std::normal_distribution<float> gs(0, 0.7071f * (float)std::pow(10.0, -60.0 / 20));
    for (size_t i = 0; i < a.size(); i++) a[i] += b[i] * g + cf32(gs(r), gs(r));
    FmReceiver rx; rx.setSilent(true); rx.setStereoMode(1); rx.configure(rate); rx.setStationFinder(false); rx.setNeighbourCancel(on);
    std::vector<float> audio;
    rx.setAudioTap([&](const float* l, const float*, size_t k) { audio.insert(audio.end(), l, l + k); });
    FmTelemetry t; uint64_t seq = 0;
    for (size_t i = 0; i < a.size(); i += 65536) { rx.feed(a.data() + i, std::min<size_t>(65536, a.size() - i)); FmTelemetry u; if (rx.telemetry(u, seq)) { seq = u.seq; t = u; } }
    double cr = 0, ci = 0, tot = 0; const size_t s0 = 96000;
    for (size_t i = s0; i < audio.size(); i++) { const double ph = 2 * M_PI * 1000 * (double)i / 48000; cr += audio[i] * std::cos(ph); ci += audio[i] * std::sin(ph); tot += audio[i] * audio[i]; }
    const double m = (double)(audio.size() - s0), tone = 2 * (cr * cr + ci * ci) / m / m, rest = std::max(tot / m - tone, 1e-12);
    return {t.rdsGroups, 10 * std::log10(tone / rest), t.psName == "WANTED  " && t.piCode == 0x7A11, t.neighbourDb};
}

int main() {
    double gain20 = 0;
    for (double off : {200e3, -200e3}) for (double lvl : {0.0, 10.0, 15.0, 20.0, 25.0, 30.0, 35.0}) for (bool st : {false, true}) {
        const R a = run(lvl, off, false, st), b = run(lvl, off, true, st);
        printf("offset %+.0f kHz neighbour %+.0f dB %s: groups %3llu -> %3llu, audio %5.1f -> %5.1f dB, PS %d -> %d, level read %.1f dB\n", off / 1e3, lvl, st ? "stereo" : "mono  ", (unsigned long long)a.groups, (unsigned long long)b.groups, a.audio, b.audio, a.ps, b.ps, b.nDb);
        if (off == 200e3 && lvl == 20 && !st) gain20 = b.audio - a.audio;
        CHECK((double)b.groups >= 0.95 * (double)a.groups, "offset %.0f level %.0f: RDS fell (%llu -> %llu)", off, lvl, (unsigned long long)a.groups, (unsigned long long)b.groups);
    }
    { const R a = run(-200, 200e3, true, false); CHECK(a.nDb == 0, "alone: a neighbour reported (%.1f)", a.nDb); }
    printf("audio gain at +200 kHz, +20 dB neighbour, mono: %.1f dB\n", gain20);
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
