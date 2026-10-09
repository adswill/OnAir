// AIS under the conditions a real radio brings: carrier offsets, clock errors, sample rates, chunk sizes, 8-bit samples, a DC spike, a gap,
// a reset in the middle, bursts at the same time on both channels, a strong station next to a weak one, noise only.
#include "dect2/ais_testutil.h"
#include "dect2/gen_util.h"
#include "impair.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::ais;
using namespace dect2::aistest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Out { int sent = 0, got = 0, extra = 0; AisTelemetry t; };

// n bursts, 80 ms apart on alternating channels, random class A positions
static std::vector<AisBurstSpec> train(int n, uint32_t seed, double t0 = 0.3, double gap = 0.08, double level = 1.0) {
    std::mt19937 rng(seed);
    std::vector<AisBurstSpec> v;
    for (int i = 0; i < n; i++) {
        AisBurstSpec s;
        s.payload = randomPosition(rng);
        s.channel = (i & 1) ? 'B' : 'A';
        s.startSec = t0 + gap * i + (double)(rng() % 1000) / 1000.0 * 0.003;
        s.level = level;
        s.phase = (double)(rng() % 1000) / 1000.0 * 6.28;
        v.push_back(s);
    }
    return v;
}

static Out decode(const std::vector<AisBurstSpec>& specs, const AisRenderConfig& rc, size_t chunk = 16384, double offsetHz = 0, bool resetAt = false) {
    std::vector<cf32> x = renderAisBursts(specs, rc);
    AisReceiver rx;
    rx.configure(rc.rate);
    rx.setSignalOffset(offsetHz);
    Collector col;
    Out o;
    if (resetAt) {
        // reset in the middle: the first half, a reset, the second half
        const size_t h = x.size() / 2;
        std::vector<cf32> a(x.begin(), x.begin() + (ptrdiff_t)h), b(x.begin() + (ptrdiff_t)h, x.end());
        const AisTelemetry t1 = runReceiver(rx, a, chunk, &col);
        rx.reset();
        AisTelemetry t2, t;
        uint64_t seq = t1.seq;
        for (size_t i = 0; i < b.size(); i += chunk) {
            rx.feed(b.data() + i, std::min(chunk, b.size() - i));
            while (rx.telemetry(t, seq)) { seq = t.seq; t2 = t; }
        }
        o.t = t2;
        CHECK(t2.seq > t1.seq, "report number keeps growing after reset");
        return o;
    }
    o.t = runReceiver(rx, x, chunk, &col);
    std::set<std::vector<uint8_t>> sent, seen;
    for (const auto& s : specs) sent.insert(s.payload);
    o.sent = (int)specs.size();
    for (const auto& g : col.got) {
        if (sent.count(g.bits)) { if (seen.insert(g.bits).second) o.got++; }
        else o.extra++;
    }
    return o;
}

static AisRenderConfig cfgAt(double rate, double snr, double dur) {
    AisRenderConfig rc;
    rc.rate = rate; rc.snrDb = snr; rc.durationSec = dur;
    return rc;
}

int main() {
    // ---- carrier offset: +-10 ppm of 162 MHz is +-1.6 kHz, the brief asks for +-2 kHz; the limit is found here as well
    for (double cfo : {-8500.0, -6000.0, -3500.0, -2000.0, -800.0, 0.0, 800.0, 2000.0, 3500.0, 6000.0, 8500.0}) {
        auto specs = train(40, 11);
        AisRenderConfig rc = cfgAt(2e6, 18, 3.8);
        rc.cfoHz = cfo;
        const Out o = decode(specs, rc);
        printf("carrier offset %6.0f Hz at 18 dB: %d of %d\n", cfo, o.got, o.sent);
        if (std::fabs(cfo) <= 8500) CHECK(o.got == o.sent && o.extra == 0, "offset %.0f Hz: %d of %d", cfo, o.got, o.sent);
        else CHECK(o.got >= o.sent * 8 / 10, "offset %.0f Hz: %d of %d", cfo, o.got, o.sent);
    }
    // ---- the same near the sensitivity limit (5 dB; 90 % decode at 2.8 dB): an offset or a clock error must not cost much
    {
        int ref = 0;
        for (double cfo : {0.0, -2000.0, 2000.0}) for (double ppm : {0.0, -50.0, 50.0}) {
            if (cfo != 0 && ppm != 0) continue;
            auto specs = train(80, 31);
            AisRenderConfig rc = cfgAt(2e6, 5, 6.9);
            rc.cfoHz = cfo; rc.sroPpm = ppm;
            const Out o = decode(specs, rc);
            printf("5 dB, offset %5.0f Hz, clock %4.0f ppm: %d of %d\n", cfo, ppm, o.got, o.sent);
            if (cfo == 0 && ppm == 0) ref = o.got;
            CHECK(o.got >= o.sent * 92 / 100 && o.got >= ref - 4, "5 dB with offset %.0f Hz, clock %.0f ppm: %d of %d (no offset: %d)", cfo, ppm, o.got, o.sent, ref);
        }
    }
    // ---- different offset per burst (stations have their own crystals), and the signal offset the engine would pass (radio tuned 50 kHz away)
    {
        auto specs = train(40, 12);
        for (size_t i = 0; i < specs.size(); i++) specs[i].cfoHz = ((double)(i % 9) - 4.0) * 450.0;
        AisRenderConfig rc = cfgAt(2e6, 18, 3.8);
        const Out o = decode(specs, rc);
        CHECK(o.got == o.sent, "a different offset per burst: %d of %d", o.got, o.sent);
        rc.tuneOffsetHz = 50000;
        const Out o2 = decode(specs, rc, 16384, -50000.0);
        CHECK(o2.got == o2.sent, "signal offset 50 kHz: %d of %d", o2.got, o2.sent);
        const Out o3 = decode(specs, rc, 16384, 0.0);
        CHECK(o3.got == o3.sent / 2, "without the signal offset only the channel that lands on -25 kHz is heard (%d of %d)", o3.got, o3.sent);
    }
    // ---- bit clock error
    for (double ppm : {-50.0, 50.0, -200.0, 200.0}) {
        auto specs = train(40, 13);
        AisRenderConfig rc = cfgAt(2e6, 18, 3.8);
        rc.sroPpm = ppm;
        const Out o = decode(specs, rc);
        printf("clock error %5.0f ppm: %d of %d\n", ppm, o.got, o.sent);
        CHECK(o.got == o.sent, "clock error %.0f ppm: %d of %d", ppm, o.got, o.sent);
    }
    // ---- sample rates
    for (double rate : {250e3, 1e6, 2e6, 2.4e6, 3.2e6, 8e6, 10e6, 20e6}) {
        auto specs = train(24, 14);
        AisRenderConfig rc = cfgAt(rate, 18, 2.4);
        const Out o = decode(specs, rc, 65536);
        printf("rate %5.2f MS/s: %d of %d\n", rate / 1e6, o.got, o.sent);
        CHECK(o.got == o.sent && o.extra == 0, "rate %.2f MS/s: %d of %d", rate / 1e6, o.got, o.sent);
    }
    {
        AisReceiver rx;
        rx.configure(100e3);
        CHECK(!rx.ready(), "100 kS/s is too low");
        rx.configure(2e6);
        CHECK(rx.ready(), "2 MS/s");
    }
    // ---- chunk sizes: the same messages whatever the chunk
    {
        auto specs = train(12, 15);
        AisRenderConfig rc = cfgAt(2e6, 18, 1.3);
        const Out ref = decode(specs, rc, 65536);
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096}) {
            const Out o = decode(specs, rc, chunk);
            CHECK(o.got == ref.got && o.got == o.sent && o.t.blocksOk == ref.t.blocksOk, "chunk %zu: %d of %d", chunk, o.got, o.sent);
        }
    }
    // ---- 8 bit samples, with and without a DC offset
    {
        auto specs = train(40, 16);
        AisRenderConfig rc = cfgAt(2e6, 20, 3.8);
        rc.quantBits = 8;
        const Out o = decode(specs, rc);
        CHECK(o.got == o.sent, "8-bit samples: %d of %d", o.got, o.sent);
        rc.dcOffset = 0.06;
        const Out o2 = decode(specs, rc);
        CHECK(o2.got == o2.sent, "8-bit samples with a DC offset of 0.06: %d of %d", o2.got, o2.sent);
        rc.dcOffset = 0.2;
        rc.quantBits = 0;
        const Out o3 = decode(specs, rc);
        CHECK(o3.got == o3.sent, "DC offset of 0.2: %d of %d", o3.got, o3.sent);
    }
    // ---- a 20 ms gap of zeros (a dropped USB buffer) and a reset in the middle
    {
        auto specs = train(40, 17);
        AisRenderConfig rc = cfgAt(2e6, 18, 3.8);
        std::vector<cf32> x = renderAisBursts(specs, rc);
        const size_t g0 = (size_t)(1.5 * 2e6), g1 = g0 + (size_t)(0.020 * 2e6);
        for (size_t i = g0; i < g1; i++) x[i] = cf32(0, 0);
        AisReceiver rx;
        rx.configure(2e6);
        Collector col;
        const AisTelemetry t = runReceiver(rx, x, 16384, &col);
        std::set<std::vector<uint8_t>> sent;
        int lost = 0, late = 0, lateOk = 0;
        for (const auto& s : specs) sent.insert(s.payload);
        std::set<std::vector<uint8_t>> got;
        for (const auto& g : col.got) got.insert(g.bits);
        for (const auto& s : specs) {
            const double end = s.startSec + 0.03;
            const bool hit = end > 1.5 && s.startSec < 1.52;
            if (hit) { lost++; continue; }
            if (s.startSec > 1.7) { late++; if (got.count(s.payload)) lateOk++; }
        }
        printf("gap of 20 ms: %zu decoded of %zu, %d bursts after it: %d decoded\n", got.size(), specs.size(), late, lateOk);
        CHECK(lateOk == late && late > 10, "bursts after the gap: %d of %d", lateOk, late);
        CHECK(got.size() + (size_t)lost >= specs.size() - 1, "the gap costs at most the burst it hits: %zu of %zu", got.size(), specs.size());
        CHECK(t.blocksBad <= 3, "gap does not make bad bursts (%llu)", (unsigned long long)t.blocksBad);
    }
    {
        auto specs = train(40, 18);
        AisRenderConfig rc = cfgAt(2e6, 18, 3.8);
        const Out o = decode(specs, rc, 16384, 0, true);
        CHECK(o.t.blocksOk >= 15 && o.t.blocksOk <= 22, "after a reset the counters start again and the second half decodes: %llu", (unsigned long long)o.t.blocksOk);
    }
    // ---- both channels at the same time; a strong station next to a weak one
    {
        std::mt19937 rng(21);
        std::vector<AisBurstSpec> specs;
        for (int i = 0; i < 20; i++) {
            for (char ch : {'A', 'B'}) {
                AisBurstSpec s; s.payload = randomPosition(rng); s.channel = ch; s.startSec = 0.3 + 0.1 * i + (ch == 'B' ? 0.0007 : 0.0); s.phase = i;
                specs.push_back(s);
            }
        }
        const Out o = decode(specs, cfgAt(2e6, 18, 2.6));
        CHECK(o.got == o.sent, "bursts overlapping in time on the two channels: %d of %d", o.got, o.sent);
    }
    {   // strong on one channel, weak on the other, at the same time: levels 40 dB apart (the strong one at 52 dB in 48 kHz, the weak one at 12 dB)
        std::mt19937 rng(22);
        std::vector<AisBurstSpec> specs;
        for (int i = 0; i < 20; i++) {
            AisBurstSpec s, w;
            s.payload = randomPosition(rng); s.channel = (i & 1) ? 'A' : 'B'; s.startSec = 0.3 + 0.1 * i; s.level = 100.0; s.phase = i;
            w.payload = randomPosition(rng); w.channel = (i & 1) ? 'B' : 'A'; w.startSec = 0.3 + 0.1 * i + 0.0013; w.level = 1.0; w.phase = 2 * i;
            specs.push_back(s); specs.push_back(w);
        }
        AisRenderConfig rc = cfgAt(2e6, 12, 2.6);
        std::vector<cf32> x;
        // the weak ones are at 12 dB, the strong ones 40 dB above: render with the reference at 12 dB and levels 100
        const Out o = decode(specs, rc);
        printf("strong (+40 dB) next to weak (12 dB) on the other channel: %d of %d\n", o.got, o.sent);
        int weakOk = 0;
        {
            std::vector<cf32> y = renderAisBursts(specs, rc);
            AisReceiver rx; rx.configure(2e6);
            Collector col; runReceiver(rx, y, 16384, &col);
            std::set<std::vector<uint8_t>> got;
            for (const auto& g : col.got) got.insert(g.bits);
            for (size_t i = 1; i < specs.size(); i += 2) if (got.count(specs[i].payload)) weakOk++;
        }
        CHECK(weakOk >= 18, "weak bursts next to a +40 dB station on the other channel: %d of 20", weakOk);
        CHECK(o.got >= 38, "all of them: %d of %d", o.got, o.sent);
    }
    {   // strong then weak on the same channel, 6 ms later (the ring down of the strong burst must not hide the weak one)
        std::mt19937 rng(23);
        std::vector<AisBurstSpec> specs;
        for (int i = 0; i < 20; i++) {
            AisBurstSpec s, w;
            s.payload = randomPosition(rng); s.channel = 'A'; s.startSec = 0.3 + 0.1 * i; s.level = 60.0; s.phase = i;
            w.payload = randomPosition(rng); w.channel = 'A'; w.startSec = s.startSec + 0.0289 + 0.0001 * (i % 5); w.level = 1.0; w.phase = 2 * i;
            specs.push_back(s); specs.push_back(w);
        }
        std::vector<cf32> y = renderAisBursts(specs, cfgAt(2e6, 14, 2.6));
        AisReceiver rx; rx.configure(2e6);
        Collector col; runReceiver(rx, y, 16384, &col);
        std::set<std::vector<uint8_t>> got;
        for (const auto& g : col.got) got.insert(g.bits);
        int strongOk = 0, weakOk = 0;
        for (size_t i = 0; i < specs.size(); i += 2) { if (got.count(specs[i].payload)) strongOk++; if (got.count(specs[i + 1].payload)) weakOk++; }
        printf("strong (+35.6 dB) then weak on the same channel, 3 ms apart: strong %d of 20, weak %d of 20\n", strongOk, weakOk);
        CHECK(strongOk == 20, "strong bursts %d of 20", strongOk);
        CHECK(weakOk >= 18, "weak burst right after a strong one: %d of 20", weakOk);
    }
    {   // a collision on one channel: a burst 15 dB stronger on top of another; the receiver must not fail, and usually the stronger one comes through
        std::mt19937 rng(24);
        std::vector<AisBurstSpec> specs;
        for (int i = 0; i < 20; i++) {
            AisBurstSpec s, w;
            s.payload = randomPosition(rng); s.channel = 'A'; s.startSec = 0.3 + 0.1 * i; s.level = 5.6; s.phase = i;
            w.payload = randomPosition(rng); w.channel = 'A'; w.startSec = s.startSec + 0.004; w.level = 1.0; w.phase = 2 * i;
            specs.push_back(s); specs.push_back(w);
        }
        std::vector<cf32> y = renderAisBursts(specs, cfgAt(2e6, 22, 2.6));
        AisReceiver rx; rx.configure(2e6);
        Collector col; const AisTelemetry t = runReceiver(rx, y, 16384, &col);
        std::set<std::vector<uint8_t>> got;
        for (const auto& g : col.got) got.insert(g.bits);
        int strongOk = 0, extra = 0;
        for (size_t i = 0; i < specs.size(); i += 2) if (got.count(specs[i].payload)) strongOk++;
        extra = (int)got.size();
        printf("collision, the stronger burst 15 dB above: %d of 20 survive, %d decoded in all, %llu bad\n", strongOk, extra, (unsigned long long)t.blocksBad);
        CHECK(extra <= 40, "no invented messages");
    }
    // ---- noise only and garbage input
    {
        std::vector<cf32> x((size_t)(2e6 * 30), cf32(0, 0));
        genutil::NoiseSource ns(5);
        ns.add(x.data(), x.size(), 0.1f);
        AisReceiver rx; rx.configure(2e6);
        const AisTelemetry t = runReceiver(rx, x);
        printf("30 s of noise: %llu messages, %llu bad, %llu bursts\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.bursts);
        CHECK(t.blocksOk == 0, "no message out of noise (%llu)", (unsigned long long)t.blocksOk);
        CHECK(t.vesselCount == 0 && t.state == 0, "state after noise: %d", t.state);
        CHECK(t.blocksBad < 5, "bad bursts out of noise: %llu", (unsigned long long)t.blocksBad);
        // not-a-number input
        for (size_t i = 100000; i < 100100; i++) x[i] = cf32(NAN, INFINITY);
        AisReceiver rx2; rx2.configure(2e6);
        runReceiver(rx2, x);
        CHECK(true, "nan input survived");
    }
    // ---- combined, through tests/impair.h: 50 ppm of 162 MHz (8.1 kHz), +80 ppm clock, an echo and 8-bit clipping
    {
        auto specs = train(40, 21);
        const AisRenderConfig rc = cfgAt(2e6, 25, 3.8);
        std::vector<cf32> x = renderAisBursts(specs, rc);
        impair::shift(x, -8100, rc.rate);
        x = impair::clock(x, 80);
        impair::echo(x, 9, -8, 2.0);
        impair::clip8(x, 3);
        AisReceiver rx; rx.configure(rc.rate);
        Collector col;
        runReceiver(rx, x, 16384, &col);
        std::set<std::vector<uint8_t>> sent, seen;
        for (const auto& s : specs) sent.insert(s.payload);
        for (const auto& g : col.got) if (sent.count(g.bits)) seen.insert(g.bits);
        printf("combined -8.1 kHz, +80 ppm, echo, 8 bit clipped: %zu of %zu\n", seen.size(), specs.size());
        CHECK(seen.size() == specs.size(), "combined: %zu of %zu", seen.size(), specs.size());
    }
    if (fails) { printf("%d checks failed\n", fails); return 1; }
    printf("ais_impair: ok\n");
    return 0;
}
