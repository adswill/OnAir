// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers receiver on the test signal: noise and a carrier offset, then every page of a cycle must arrive with its exact text and address.
// Cases: the mix at 15 dB with an offset, the mix with only POCSAG or only FLEX decoded, the same inverted (as some transmitters send it), the four-level FLEX speeds alone at 15 dB, and noise alone.
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static PagerTelemetry run(const SynthConfig& cfg, double rate, double seconds, bool noiseOnly = false, int protocols = 3, int pocsagBauds = 7) {
    PagerReceiver rx;
    rx.configure(rate);
    rx.setProtocols(protocols);
    rx.setPocsagBauds(pocsagBauds);
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
    {   // FLEX with four levels alone: 1600/4 (kFlex3200_4) and 3200/4 (kFlex6400_4)
        for (int sp = kFlex3200_4; sp <= kFlex6400_4; sp++) {
            SynthConfig c;
            c.snrDb = 15; c.cfoHz = 300; c.modeOpt[2] = sp + 1;
            const PagerTelemetry t = run(c, rate, pagerCycleSeconds(0, sp + 1) * 2.2);
            compare(t, pagerTestMessages(0, sp + 1), pagerSpeedName(sp));
        }
    }
    {   // a bad FIW must not drop lock: wipe the first frame's FIW, later 1600/4 pages still arrive
        SynthConfig c;
        c.snrDb = 40; c.cfoHz = 0; c.modeOpt[2] = kFlex3200_4 + 1;
        PagerReceiver rx;
        rx.configure(rate);
        rx.setProtocols(2);
        rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
        auto syn = makePagerSynth(c, rate);
        std::vector<cf32> buf(65536);
        PagerTelemetry t;
        uint64_t seq = 0;
        const int64_t fiw0 = (int64_t)(160.0 * rate / 1600.0), fiw1 = (int64_t)(220.0 * rate / 1600.0);
        const double secs = pagerCycleSeconds(0, kFlex3200_4 + 1) * 2.4;
        int64_t done = 0;
        while ((double)done < secs * rate) {
            syn->generate(buf.data(), buf.size());
            for (size_t i = 0; i < buf.size(); i++) {
                const int64_t s = done + (int64_t)i;
                if (s >= fiw0 && s < fiw1) buf[i] = cf32(0.f, 0.f);
            }
            rx.feed(buf.data(), buf.size());
            done += (int64_t)buf.size();
            PagerTelemetry n;
            if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
        }
        CHECK(t.speeds[kFlex3200_4].messages > 0, "bad FIW dropped FLEX 1600/4 lock (%llu pages)", (unsigned long long)t.speeds[kFlex3200_4].messages);
        printf("  bad FIW: %llu FLEX 1600/4 pages after the first frame's FIW was wiped\n", (unsigned long long)t.speeds[kFlex3200_4].messages);
    }
    {   // POCSAG 1200 only: no 512 or 2400 pages
        SynthConfig c;
        c.snrDb = 15; c.cfoHz = 500;
        const PagerTelemetry t = run(c, rate, pagerCycleSeconds() + 1.0, false, 3, 2);
        int n512 = 0, n1200 = 0, n2400 = 0;
        const std::vector<PagerMessage> empty;
        const std::vector<PagerMessage>& got = t.messages ? *t.messages : empty;
        for (const PagerMessage& m : got) {
            if (m.flex) continue;
            if (m.speed == kPocsag512) n512++;
            else if (m.speed == kPocsag1200) n1200++;
            else if (m.speed == kPocsag2400) n2400++;
        }
        CHECK(n512 == 0 && n2400 == 0 && n1200 > 0, "1200-only baud mask: 512=%d 1200=%d 2400=%d", n512, n1200, n2400);
    }
    {   // the mix with one protocol on (the POCSAG and FLEX entries of the app): only its pages, all of them
        for (int prot = 1; prot <= 2; prot++) {
            SynthConfig c;
            c.snrDb = 15; c.cfoHz = 400;
            const PagerTelemetry t = run(c, rate, pagerCycleSeconds() + 1.0, false, prot);
            compare(t, pagerTestMessages(prot), prot == 1 ? "POCSAG only" : "FLEX only");
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
