// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DTMB as the recordings of issues #61 and #62 show it: single carrier transmitters (C=1, PN595 with a fixed phase, 4QAM rate 0.8,
// interleaver mode 2) received at 10 Msps through a channel with an echo 10 us late and 3 dB down, 310 Hz off, the clock 0.5 ppm off.
// The recordings themselves do not decode: the radio (a network USRP) left out most of the signal, in runs of 1 to 75 ms, and no stretch
// of them is as long as the de-interleaver (510 frames, 0.3 s). These tests keep the receiver honest on the same signal without that:
//  - the clean channel decodes every packet;
//  - radio overflows shorter than a frame (the fixed header gives the frame grid back at once, one frame is erased) lose nothing;
//  - a gap of several frames: a fixed PN phase cannot tell how many frames went, so the de-interleaver is out of step until it has refilled
//    (0.3 s); what comes out in that time is thrown away by the LDPC and BCH checks, never passed on as wrong packets, and the lock holds.
#include "dect2/dtmb/dtmb_testkit.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dect2;
using namespace dect2::dtmb;
using namespace dect2::dtmb::kit;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Scenario onAir() {
    Scenario o;
    o.sc = makeSignal(Header::Pn595, Mapping::Qam4, Rate::R08, true, 10e6, 20);
    o.sc.tx.carriers = 1;
    o.sc.tx.phaseRotate = false;
    o.sc.cfoHz = 310;
    o.sc.sroPpm = 0.5;
    o.sc.echoes.push_back({3.0, 100.0});   // 10 us at 10 Msps
    o.seconds = 2.2;
    o.threads = 0;
    return o;
}

static int locks(const Result& r) {
    int n = 0;
    for (const std::string& l : r.log) if (l.find("frames found") != std::string::npos) n++;
    return n;
}

static void show(const char* name, const Result& r) {
    printf("  %-44s first packet %.2f s, last %.2f s, %llu good, %llu wrong, %llu missing; MER %.1f dB, echo %.1f us, %d lock(s)\n", name, r.firstGoodSec, r.lastGoodSec,
           (unsigned long long)r.good, (unsigned long long)r.wrong, (unsigned long long)r.missing, r.tel.merDb, r.tel.echoSpanUs, locks(r));
    if (getenv("DTMB_LOG")) for (const std::string& l : r.log) printf("      %s\n", l.c_str());
}

int main() {
    {
        const Scenario o = onAir();
        const Result r = run(o);
        show("clean", r);
        CHECK(r.tel.siOk && r.tel.carriers == 1 && r.tel.mapping == 1 && r.tel.rate == 2 && r.tel.interleaver == 2, "system information %d: carriers %d mapping %d rate %d interleaver %d",
              (int)r.tel.siOk, r.tel.carriers, r.tel.mapping, r.tel.rate, r.tel.interleaver);
        CHECK(r.firstGoodSec >= 0 && r.firstGoodSec < 0.5, "first packet at %.2f s", r.firstGoodSec);
        CHECK(r.wrong == 0 && r.missing == 0, "%llu wrong, %llu missing", (unsigned long long)r.wrong, (unsigned long long)r.missing);
        CHECK(r.goodFraction(r.firstGoodSec) > 0.97, "%.2f of the packets", r.goodFraction(r.firstGoodSec));
        CHECK(locks(r) == 1, "the lock was found %d times", locks(r));
    }
    {
        Scenario o = onAir();
        o.dropsPerSec = 6; o.dropMin = 100; o.dropMax = 5000;   // a frame is 5787 samples
        const Result r = run(o);
        show("overflows shorter than a frame, 6/s", r);
        CHECK(r.dropped > 0, "no samples were dropped");
        CHECK(r.wrong == 0, "%llu wrong packets", (unsigned long long)r.wrong);
        CHECK(locks(r) == 1, "the lock was found %d times", locks(r));
        CHECK(r.lastGoodSec > 2.0, "last packet at %.2f s", r.lastGoodSec);
        CHECK(r.good >= 0.97 * (double)(r.good + r.missing), "%.3f of the packets", (double)r.good / (double)std::max<uint64_t>(1, r.good + r.missing));
    }
    {
        Scenario o = onAir();
        o.gapAt = 0.9; o.gapSec = 0.004; o.gapRemove = true;   // about 7 frames missing
        const Result r = run(o);
        show("4 ms missing (7 frames)", r);
        CHECK(r.wrong == 0, "%llu wrong packets", (unsigned long long)r.wrong);
        CHECK(locks(r) == 1, "the lock was found %d times", locks(r));
        CHECK(r.lastGoodSec > 2.0, "no recovery: last packet at %.2f s", r.lastGoodSec);
    }
    printf(failures ? "dtmb_c1echo: %d FAILED\n" : "dtmb_c1echo: all passed\n", failures);
    return failures ? 1 : 0;
}
