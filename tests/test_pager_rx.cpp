// Pagers receiver on the test signal: noise and a carrier offset, then every page of a cycle must arrive with its exact text and address.
// Cases: the mix at 15 dB with an offset, the same inverted (as some transmitters send it), the four-level FLEX speeds alone at 15 dB, and noise alone.
#include "dect2/pager_gen.h"
#include "dect2/pager_rx.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static PagerTelemetry run(const SynthConfig& cfg, double rate, double seconds, bool noiseOnly = false) {
    PagerReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
    auto syn = makePagerSynth(cfg, rate);
    std::vector<cf32> buf(65536);
    PagerTelemetry t;
    uint64_t seq = 0;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) for (auto& v : buf) v = cf32(0.f, 0.f); else syn->generate(buf.data(), buf.size());
        if (noiseOnly) { static unsigned s = 12345; for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); } }
        rx.feed(buf.data(), buf.size());
        PagerTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    return t;
}

// every expected page present exactly as sent, and nothing else
static int compare(const PagerTelemetry& t, const std::vector<PagerTestMessage>& want, const char* what) {
    int found = 0;
    const std::vector<PagerMessage> empty;
    const std::vector<PagerMessage>& got = t.messages ? *t.messages : empty;
    for (const PagerTestMessage& w : want) {
        bool ok = false;
        for (const PagerMessage& m : got)
            if (m.speed == w.speed && m.flex == w.flex && m.address == w.address && m.function == w.function && m.type == w.type && m.text == w.text) { ok = true; break; }
        CHECK(ok, "%s: missing %s address %u '%s'", what, pagerSpeedName(w.speed), w.address, w.text.c_str());
        found += ok;
    }
    int stray = 0;
    for (const PagerMessage& m : got) {
        bool known = false;
        for (const PagerTestMessage& w : want)
            if (m.speed == w.speed && m.address == w.address && m.type == w.type && m.text == w.text) known = true;
        if (!known) { stray++; printf("  stray: %s %u %s '%s'\n", pagerSpeedName(m.speed), m.address, pagerTypeName(m.type), m.text.c_str()); }
    }
    CHECK(stray == 0, "%s: %d pages that were not sent", what, stray);
    printf("  %s: %d of %zu pages, %llu code words good, %llu lost, snr %.1f dB, cfo %.0f Hz\n", what, found, want.size(), (unsigned long long)t.blocksOk,
           (unsigned long long)t.blocksBad, t.snrDb, t.cfoHz);
    return found;
}

int main() {
    const double rate = 1.2e6;
    {   // the whole cycle, 15 dB, carrier 700 Hz off
        SynthConfig c;
        c.snrDb = 15; c.cfoHz = 700;
        const PagerTelemetry t = run(c, rate, pagerCycleSeconds() + 1.0);
        compare(t, pagerTestMessages(), "mix 15 dB");
        CHECK(t.dataValid && t.blocksOk > 500 && t.blocksBad < t.blocksOk / 10, "code words %llu good %llu lost", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        for (int s = 0; s < kPagerSpeeds; s++) CHECK(t.speeds[s].messages > 0 && t.speeds[s].transmissions > 0, "%s: no page or transmission counted", pagerSpeedName(s));
        CHECK(std::fabs(t.cfoHz - 700) < 250, "carrier offset %.0f Hz", t.cfoHz);
    }
    {   // inverted polarity, other offset
        SynthConfig c;
        c.snrDb = 15; c.cfoHz = -900; c.modeOpt[1] = 1;
        const PagerTelemetry t = run(c, rate, pagerCycleSeconds() + 1.0);
        compare(t, pagerTestMessages(), "inverted 15 dB");
    }
    {   // FLEX with four levels alone, both speeds
        for (int sp = kFlex3200_4; sp <= kFlex6400_4; sp++) {
            SynthConfig c;
            c.snrDb = 15; c.cfoHz = 300; c.modeOpt[2] = sp + 1;
            const PagerTelemetry t = run(c, rate, pagerCycleSeconds(0, sp + 1) * 2.2);
            compare(t, pagerTestMessages(0, sp + 1), pagerSpeedName(sp));
        }
    }
    {   // nothing but noise: no page, no state
        SynthConfig c;
        const PagerTelemetry t = run(c, rate, 20.0, true);
        CHECK(t.messagesTotal == 0 && (!t.messages || t.messages->empty()), "%llu pages out of noise", (unsigned long long)t.messagesTotal);
        printf("  noise: %llu pages, %llu code words\n", (unsigned long long)t.messagesTotal, (unsigned long long)(t.blocksOk + t.blocksBad));
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
