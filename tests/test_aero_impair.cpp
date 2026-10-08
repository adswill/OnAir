// Inmarsat Aero receiver against the impairments of a real radio, on the default test signal (10500 bit/s at -50 kHz, 1200 bit/s at +150 kHz):
// carrier offset +-10 kHz, drift, bit clock +-50 ppm, 8-bit samples with a DC offset, a 20 ms gap, reset() in the middle of the stream.
// Every case must decode both channels with no bad SU after the impairment, and every frame handed on must be one the generator sent.
#include "dect2/aero_gen.h"
#include "dect2/aero_rx.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case {
    const char* name;
    double cfo = 0, drift = 0, ppm = 0;
    bool quant = false;
    double gapAt = -1, resetAt = -1;
    cf32 dc{0, 0};
    double secs = 11;
};

static void run(const Case& k) {
    const double rate = 2e6;
    SynthConfig cfg;
    cfg.mode = 20;
    cfg.cfoHz = k.cfo;
    cfg.sroPpm = k.ppm;
    cfg.modeVal[1] = k.drift;
    cfg.modeOpt[1] = 3;
    AeroSynth syn(cfg, rate);
    syn.record(true);
    AeroReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-aeroTuning().tuneOffsetHz);
    std::set<std::vector<uint8_t>> got;
    size_t damaged = 0;
    rx.setFrameCallback([&](double, const AeroFrameEvent& e) { if (e.susBad == 0) got.insert(e.bytes); else damaged++; });
    std::vector<cf32> buf(16384);
    AeroTelemetry t, atReset;
    uint64_t last = 0;
    bool didGap = false, didReset = false;
    uint64_t okBefore = 0, badBefore = 0;
    for (double done = 0; done < k.secs * rate; done += (double)buf.size()) {
        syn.generate(buf.data(), buf.size());
        const double now = done / rate;
        if (k.quant || k.dc != cf32(0, 0))
            for (auto& v : buf) {
                v += k.dc;
                if (k.quant) v = cf32(std::round(std::max(-1.f, std::min(1.f, v.real())) * 127.f) / 127.f, std::round(std::max(-1.f, std::min(1.f, v.imag())) * 127.f) / 127.f);
            }
        if (k.gapAt >= 0 && !didGap && now >= k.gapAt) {          // 20 ms of nothing (the radio dropped samples)
            didGap = true;
            std::vector<cf32> z((size_t)(0.02 * rate), cf32(0, 0));
            rx.feed(z.data(), z.size());
        }
        if (k.resetAt >= 0 && !didReset && now >= k.resetAt) {
            didReset = true;
            rx.telemetry(atReset, 0);
            rx.reset();
            okBefore = atReset.blocksOk; badBefore = atReset.blocksBad;
        }
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, last)) { CHECK(t.seq > last, "%s: seq", k.name); last = t.seq; }
    }
    rx.telemetry(t, 0);
    const auto sent = syn.takeFrames();
    std::set<std::vector<uint8_t>> sentSet;
    for (const auto& f : sent) sentSet.insert(f.bytes);
    size_t exact = 0;
    for (const auto& g : got) exact += sentSet.count(g);
    printf("%-26s", k.name);
    int found = 0;
    for (const auto& sc : syn.channels()) {
        for (const auto& c : t.channels) {
            if (c.bitRate != sc.bitRate || c.state != 3) continue;
            found++;
            printf("  %5d: %+9.1f Hz Eb/N0 %4.1f frames %3llu SUs %4llu/%llu", c.bitRate, c.offsetHz, c.ebn0Db, (unsigned long long)c.frames, (unsigned long long)c.susOk,
                   (unsigned long long)c.susBad);
            CHECK(c.susBad == 0 || (k.gapAt >= 0 && c.susBad <= 2u * (c.bitRate == 10500 ? 26 : 6)), "%s: rate %d bad SUs %llu", k.name, c.bitRate, (unsigned long long)c.susBad);
            CHECK(std::fabs(c.offsetHz - (sc.offsetHz + k.cfo)) < 3000 + std::fabs(k.drift) * k.secs, "%s: rate %d at %.0f", k.name, c.bitRate, c.offsetHz);
        }
    }
    printf("  frames %zu exact %zu, damaged %zu\n", got.size(), exact, damaged);
    CHECK(damaged <= (k.gapAt >= 0 ? 4u : 0u), "%s: %zu damaged frames", k.name, damaged);
    CHECK(found == 2 && t.channels.size() == 2, "%s: %d of 2 channels decoded (%zu found)", k.name, found, t.channels.size());
    CHECK(exact == got.size() && got.size() > 10, "%s: %zu of %zu frames exact", k.name, exact, got.size());
    if (k.resetAt >= 0) CHECK(didReset && okBefore > 0 && t.blocksOk > 0 && t.seq > atReset.seq && t.blocksOk < okBefore + 2000, "%s: counts after reset", k.name);
    (void)badBefore;
}

int main() {
    Case cs[8];
    cs[0].name = "carrier +10 kHz"; cs[0].cfo = 10000;
    cs[1].name = "carrier -10 kHz"; cs[1].cfo = -10000;
    cs[2].name = "drift 5 Hz/s"; cs[2].drift = 5;
    cs[3].name = "clock +50 ppm"; cs[3].ppm = 50;
    cs[4].name = "clock -50 ppm"; cs[4].ppm = -50;
    cs[5].name = "8-bit, DC offset"; cs[5].quant = true; cs[5].dc = cf32(0.06f, -0.04f);
    cs[6].name = "20 ms gap at 6 s"; cs[6].gapAt = 6;
    cs[7].name = "reset at 6 s"; cs[7].resetAt = 6; cs[7].secs = 13;
    for (const Case& k : cs) run(k);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
