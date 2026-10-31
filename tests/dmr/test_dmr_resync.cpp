// DMR cold-start resync (3.5): the first short transmission heard by a fresh receiver (a scan lands on a channel, a handset keys up after silence),
// with a carrier offset the receiver has not learned yet, noise and a clock error. Measures the share of first calls that decode and the delay from the
// start of the call to the first voice burst. Also: after the carrier has gone away for a while the receiver keeps what it learned, so the next short
// call (same radio) is as fast as a warm one. Run with an argument to print only.
#include "dect2/dmr/dmr_eval.h"
#include "dect2/dmr/dmr_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace dect2;
static int fails = 0;

struct Res { bool ok = false; double first = -1; int frames = 0, expect = 0; };

static Res runCold(double cfo, double snr, double ppm, bool mobile, uint32_t seed, double secs) {
    DmrGenConfig g;
    g.rate = 1.2e6; g.direct = true; g.mobile = mobile; g.snrDb = snr; g.cfoHz = cfo; g.sroPpm = ppm; g.seed = seed; g.traffic = 2;
    g.superMin = 1; g.superSpan = 1; g.textMessages = false; g.talkerAlias = false;
    DmrSignal sig(g);
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(g.rate);
    std::vector<double> voice;
    double now = 0;
    rx.setVoiceCallback([&](int, int, const uint8_t*) { voice.push_back(now); });
    const size_t chunk = 4800, total = (size_t)(secs * g.rate);
    std::vector<cf32> buf(chunk);
    for (size_t done = 0; done < total; done += chunk) {
        sig.generate(buf.data(), chunk);
        for (auto& v : buf) {
            auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
            v = cf32(q(v.real()), q(v.imag()));
        }
        now = (double)(done + chunk) / g.rate;
        rx.feed(buf.data(), chunk);
    }
    Res r;
    for (const DmrTruth& tr : sig.truth()) {
        if (tr.kind > 2 || !tr.finished) continue;
        int n = 0;
        for (double v : voice) if (v >= tr.startSec - 0.01 && v < tr.endSec + 0.1) { if (r.first < 0) r.first = v - tr.startSec; n++; }
        r.frames = n; r.expect = tr.voiceBursts; if (getenv("RS_DEBUG")) printf("   call %.3f..%.3f voice %d/%d\n", tr.startSec, tr.endSec, n, tr.voiceBursts);
        r.ok = n >= 0.5 * std::max(1, tr.voiceBursts);
        break;                                   // the first call only: cold
    }
    return r;
}

int main(int argc, char** argv) {
    const bool printOnly = argc > 1;
    struct Case { const char* name; double cfo, snr, ppm; bool mobile; double minShare; double maxMs; };
    const Case cases[] = {
        {"cfo 0, 25 dB", 0, 25, 0, false, 1.0, 400},
        {"cfo +3 kHz", 3000, 25, 0, false, 1.0, 400},
        {"cfo -6 kHz", -6000, 25, 0, false, 1.0, 600},
        {"cfo +9 kHz", 9000, 25, 0, false, 0.9, 700},
        {"cfo -9.5 kHz, 50 ppm", -9500, 25, 50, false, 0.9, 700},
        {"cfo +8 kHz, 12 dB", 8000, 12, 0, false, 0.8, 800},
        {"cfo -4 kHz, 8 dB, mobile", -4000, 8, 20, true, 0.7, 900},
    };
    printf("%-28s %5s %8s %8s %9s %9s\n", "case", "runs", "decoded", "frames", "mean ms", "max ms");
    for (const Case& c : cases) {
        int runs = 0, ok = 0, firstN = 0;
        double sum = 0, mx = 0, fr = 0;
        for (uint32_t seed = 1; seed <= 8; seed++) {
            const Res r = runCold(c.cfo, c.snr, c.ppm, c.mobile, seed, 3.0);
            runs++;
            if (r.ok) ok++;
            fr += std::min(1.0, r.frames / std::max(1.0, (double)r.expect));
            if (r.first >= 0) { firstN++; sum += r.first; mx = std::max(mx, r.first); }
        }
        const double share = (double)ok / runs;
        printf("%-28s %5d %7.0f%% %7.0f%% %9.0f %9.0f\n", c.name, runs, 100 * share, 100 * fr / runs, firstN ? 1000 * sum / firstN : -1.0, 1000 * mx);
        if (!printOnly && share < c.minShare) { printf("FAIL: %s: only %.0f%% of cold first calls decoded\n", c.name, 100 * share); fails++; }
        if (!printOnly && firstN && 1000 * mx > c.maxMs) { printf("FAIL: %s: slowest first voice %.0f ms (limit %.0f)\n", c.name, 1000 * mx, c.maxMs); fails++; }
    }
    // the carrier goes away for 2 s and comes back: what the receiver learned (offset) is kept, the next call is warm
    {
        DmrGenConfig g;
        g.rate = 1.2e6; g.direct = true; g.snrDb = 25; g.cfoHz = -9000; g.seed = 5; g.traffic = 2; g.superMin = 1; g.superSpan = 1;
        g.textMessages = false; g.talkerAlias = false;
        DmrSignal sig(g);
        DmrReceiver rx;
        rx.setSilent(true);
        rx.configure(g.rate);
        std::vector<double> voice;
        double now = 0;
        rx.setVoiceCallback([&](int, int, const uint8_t*) { voice.push_back(now); });
        const size_t chunk = 4800;
        std::vector<cf32> buf(chunk);
        double carrierOffUntil = 0;
        for (size_t done = 0; done < (size_t)(16 * g.rate); done += chunk) {
            now = (double)(done + chunk) / g.rate;
            if (now > 6 && now < 8) { sig.setCarrier(false); carrierOffUntil = 8; } else sig.setCarrier(true);
            sig.generate(buf.data(), chunk);
            rx.feed(buf.data(), chunk);
        }
        (void)carrierOffUntil;
        double worst = 0; int n = 0;
        for (const DmrTruth& tr : sig.truth()) {
            if (tr.kind > 2 || !tr.finished || tr.startSec < 8.2 || tr.endSec > 15) continue;
            double first = -1;
            for (double v : voice) if (v >= tr.startSec - 0.01 && v < tr.endSec + 0.1) { first = v - tr.startSec; break; }
            n++;
            worst = std::max(worst, first < 0 ? 9.0 : first);
        }
        printf("after a 2 s carrier gap: %d calls, slowest first voice %.0f ms\n", n, 1000 * worst);
        if (!printOnly && n > 0 && worst > 0.25) { printf("FAIL: warm resync after the gap too slow (%.0f ms)\n", 1000 * worst); fails++; }
    }
    printf(fails ? "%d FAILED\n" : "done\n", fails);
    return fails ? 1 : 0;
}
