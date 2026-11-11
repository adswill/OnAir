// Auto for the keyboard modes (Olivia, Contestia, MFSK, THOR, DominoEX, Hell): every set with offset, drift and noise, simulated signals only (makeKbdTx).
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/autoparam/autoparam.h"
#include "sim.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
using namespace dect2::autotest;
#include "../common/test_util.h"
#include "../common/test_long.h"

namespace {
std::vector<float> tx(const KbdConfig& c, double seconds, double offset, double ppm, const std::string& text = "") {
    KbdTxConfig t;
    t.cfg = c; t.offsetHz = offset; t.clockPpm = ppm;
    if (!text.empty()) t.text = text;
    auto g = makeKbdTx(t);
    std::vector<float> a((size_t)(seconds * 8000));
    g->generate(a.data(), a.size());
    return a;
}
bool hit(const std::vector<KbdAutoHyp>& h, const KbdConfig& c) {
    if (h.empty()) return false;
    if (c.family == KbdOlivia || c.family == KbdContestia) return h[0].tones == c.tones && h[0].bandwidth == c.bandwidth;
    return h[0].variant == c.variant;
}
const char* nameOf(const std::vector<KbdAutoHyp>& h) { static char b[64]; if (h.empty()) return "none"; snprintf(b, sizeof b, "%d/%d v%d (%u chars, %.2f read)", h[0].tones, h[0].bandwidth, h[0].variant, h[0].chars, h[0].readable); return b; }

struct Tally { int ok = 0, n = 0; };
// by default every second setting of a family at 15 dB (the checked point) and 4 noise-alone runs; the long run (ONAIR_TEST_LONG=1) does every
// setting at 15 and 6 dB and 10 noise runs
Tally sweepFamily(int family, double snr, bool verbose) {
    Tally t;
    int idx = 0;
    if (family == KbdOlivia || family == KbdContestia) {
        for (int bw : kKbdBwList) for (int tones : {4, 8, 16, 32, 64}) {
            if ((double)bw / tones < 7.8) continue;
            if (!testLong() && idx++ % 2) continue;
            KbdConfig c; c.family = family; c.tones = tones; c.bandwidth = bw;
            auto a = tx(c, (double)bw / tones < 10 ? 45 : 24, t.n % 2 ? 40 : -40, 0);
            shift(a, 8000, 0, 5);
            addNoise(a, 8000, snr, 3000, (uint64_t)(t.n + 11));
            const auto h = kbdAutoAnalyse(a.data(), a.size(), family, 3);
            t.n++;
            if (hit(h, c)) t.ok++;
            else if (verbose) printf("  %s %d/%d -> %s\n", kbdFamilyName(family), tones, bw, nameOf(h));
        }
    } else {
        for (int v = 0; v < kbdVariantCount(family); v++) {
            if (!testLong() && idx++ % 2) continue;
            KbdConfig c; c.family = family; c.variant = v;
            auto a = tx(c, (family == KbdMfsk && v <= 2) || (family != KbdMfsk && family != KbdHell && v <= 1) ? 45 : 24, t.n % 2 ? 30 : -30, 0);
            shift(a, 8000, 0, 5);
            addNoise(a, 8000, snr, 3000, (uint64_t)(t.n + 31));
            const auto h = kbdAutoAnalyse(a.data(), a.size(), family, 3);
            t.n++;
            if (hit(h, c)) t.ok++;
            else if (verbose) printf("  %s -> %s\n", kbdVariantName(family, v), nameOf(h));
        }
    }
    return t;
}

}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < kKbdFamilies; f++) {
        for (double snr : {15.0, 6.0}) {
            if (!testLong() && snr != 15.0) continue;
            const Tally t = sweepFamily(f, snr, snr == 15.0);
            printf("%-14s %4.0f dB in 3 kHz: %d of %d exact (offset, 5 Hz/min drift)\n", kbdFamilyName(f), snr, t.ok, t.n);
            if (snr == 15.0 && f != KbdHell) CHECK(t.ok * 100 >= t.n * 60, "%s %g: %d of %d", kbdFamilyName(f), snr, t.ok, t.n);
        }
    }
    // noise alone
    for (int f = 0; f < kKbdFamilies; f++) {
        int bad = 0;
        const int tot = testLongPick(4, 10);
        for (int k = 0; k < tot; k++) {
            auto a = noiseOnly(8000, 24, 0.2, (uint64_t)(5000 + 100 * f + k));
            bad += !kbdAutoAnalyse(a.data(), a.size(), f, 3).empty();
        }
        printf("%-14s noise alone: %d false locks of %d\n", kbdFamilyName(f), bad, tot);
        CHECK(bad == 0, "%s noise false locks %d", kbdFamilyName(f), bad);
    }
    // two signals in view (Olivia 32/1000 and an MFSK16 half as strong)
    {
        KbdConfig c1; c1.family = KbdOlivia; c1.tones = 32; c1.bandwidth = 1000; c1.centreHz = 1000;
        KbdConfig c2; c2.family = KbdMfsk; c2.variant = 3; c2.centreHz = 2300;
        auto a = tx(c1, 24, 0, 0);
        const auto b = tx(c2, 24, 0, 0);
        for (size_t i = 0; i < a.size(); i++) a[i] += 0.5f * b[i];
        addNoise(a, 8000, 12, 3000, 77);
        const auto h = kbdAutoAnalyse(a.data(), a.size(), KbdOlivia, 3);
        printf("two signals in view: Olivia 32/1000 -> %s\n", nameOf(h));
    }
    // live: Auto finds the set from wrong starting values, follows a change, and a manual setting stays
    {
        auto d = makeKbdDecoder();
        KbdConfig c; c.family = KbdOlivia; c.tones = 16; c.bandwidth = 500; c.autoParams = true;
        d->setConfig(c);
        KbdConfig s; s.family = KbdOlivia; s.tones = 8; s.bandwidth = 500;
        auto a = tx(s, 34, 25, 0);
        addNoise(a, 8000, 12, 3000, 91);
        for (size_t i = 0; i < a.size(); i += 2048) { d->feedAudio(&a[i], std::min<size_t>(2048, a.size() - i)); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        d->feedAudio(&a[0], 2048);
        HfdigKbdTelemetry t; d->telemetry(t);
        printf("live Olivia: locked %d, %d tones %d Hz, %llu chars\n", (int)t.autoLocked, t.autoTones, t.autoBandwidth, (unsigned long long)t.chars);
        CHECK(t.autoLocked && t.autoTones == 8 && t.autoBandwidth == 500, "live found");
        KbdConfig s2; s2.family = KbdOlivia; s2.tones = 16; s2.bandwidth = 1000;
        auto b = tx(s2, 48, -30, 0);
        addNoise(b, 8000, 12, 3000, 92);
        for (size_t i = 0; i < b.size(); i += 2048) { d->feedAudio(&b[i], std::min<size_t>(2048, b.size() - i)); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        d->feedAudio(&b[0], 2048);
        d->telemetry(t);
        printf("live Olivia after the change: locked %d, %d tones %d Hz, %llu chars\n", (int)t.autoLocked, t.autoTones, t.autoBandwidth, (unsigned long long)t.chars);
        CHECK(t.autoLocked && t.autoTones == 16 && t.autoBandwidth == 1000, "live relock");
        KbdConfig m = c; m.autoParams = false; m.tones = 32; m.bandwidth = 500;
        d->setConfig(m);
        d->feedAudio(&b[0], 2048);
        d->telemetry(t);
        CHECK(!t.autoOn && t.cfg.tones == 32 && t.cfg.bandwidth == 500, "manual kept");
        printf("manual after Auto: %d tones %d Hz, auto %d\n", t.cfg.tones, t.cfg.bandwidth, (int)t.autoOn);
    }
    printf("%.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("autoparam_kbd");
}
