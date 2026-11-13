// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR receiver against the test signal with real-world faults (tests/common/impair.h): tuning error +-2 kHz, a channel off centre with a
// clock error, noise, swapped I/Q, bit errors in the header (corrected; or too many, then the header comes from the slow data), lost slow
// data blocks, a start in the middle of a call (late entry), and a carrier that stops without the end pattern. Silent: no sound device.
// Each case plays the test schedule once: a repeater call with a message and NMEA GPS, and a simplex call with D-PRS and code squelch.
#include "dect2/dstar/dstar_gen.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/dstar/dstar_tel.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 500000;
constexpr double kOff = -50000;

struct Fault {
    const char* name;
    double cfo = 0, ppm = 0, snr = 30, shift = 0;
    bool swap = false;
    double skip = 0;
    int hdrErr = 0, drop = 0;
    bool noEnd = false;
    double noise = 0;            // extra white noise over the whole band (impair::noise), dB SNR; 0 none
};
struct Result { DstarTelemetry t; uint64_t voiceBits = 0, voiceErr = 0; };

static Result run(const Fault& f) {
    DstarGenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.snrDb = f.snr; c.cfoHz = f.cfo; c.loop = false; c.seed = 5;
    c.txs = dstarTestSchedule();
    for (auto& t : c.txs) { t.headerErrors = f.hdrErr; t.dropBlock = f.drop; t.endPattern = !f.noEnd; }
    double total = c.leadSec + 1.0;
    for (const auto& t : c.txs) total += t.durSec + 0.3 + t.gapSec;
    DstarGenerator g(c);
    std::vector<cf32> x((size_t)(total * kRate));
    g.generate(x.data(), x.size());
    if (f.shift != 0) impair::shift(x, f.shift, kRate);
    if (f.ppm != 0) x = impair::clock(x, f.ppm);
    if (f.swap) impair::swapIq(x);
    if (f.noise != 0) impair::noise(x, f.noise, 9);
    if (f.skip > 0) impair::skip(x, (size_t)(f.skip * kRate));
    DstarReceiver r;
    r.setSilent(true);
    Result res;
    // the voice bits as received, against the known pattern; frame numbers counted from each call's first frame
    uint32_t fn = 0; int lastCalls = -1;
    r.configure(kRate);
    r.setSignalOffset(kOff);   // swapped I/Q moves the channel to the other side too: the mirrored front end finds it there
    DstarTelemetry live;
    r.setVoiceTap([&](const uint8_t* b) {
        uint8_t want[72];
        dstarKnownVoice(fn++, want);
        if (f.skip == 0) { res.voiceBits += 72; for (int i = 0; i < 72; i++) res.voiceErr += b[i] != want[i]; }
    });
    for (size_t o = 0; o < x.size(); o += 4096) {
        r.feed(x.data() + o, std::min<size_t>(4096, x.size() - o));
        if (r.telemetry(live, 0) && (int)live.log.size() != lastCalls) { lastCalls = (int)live.log.size(); fn = 0; }
    }
    r.telemetry(res.t, 0);
    return res;
}

static const DstarCall* callOf(const DstarTelemetry& t, const std::string& my) {
    for (const auto& c : t.log) if (c.my == my) return &c;
    return nullptr;
}
static bool posNear(const DstarTelemetry& t, const std::string& call, double lat, double lon) {
    for (const auto& p : t.positions) if (p.call == call && std::fabs(p.lat - lat) < 2e-4 && std::fabs(p.lon - lon) < 2e-4) return true;
    return false;
}

int main() {
    const Fault cases[] = {
        {"clean"},
        {"offset +2 kHz", 2000}, {"offset -2 kHz", -2000},
        {"off centre +2.5 kHz, 50 ppm", 0, 50, 30, 2500},
        {"noise 14 dB", 0, 0, 14},
        {"swapped I/Q", 0, 0, 30, 0, true},
        {"header 25 bit errors", 0, 0, 30, 0, false, 0, 25},
        {"header ruined", 0, 0, 30, 0, false, 0, 160},
        {"slow data blocks lost", 0, 0, 30, 0, false, 0, 0, 9},
        {"no end pattern", 0, 0, 30, 0, false, 0, 0, 0, true},
        {"late start", 0, 0, 30, 0, false, 1.0},
        {"all at once", -2000, -40, 16, 0, true, 0, 25, 11},
    };
    printf("  %-28s %5s %5s %4s %4s %4s %6s %6s %6s %s\n", "fault", "calls", "hdrOK", "slow", "late", "pos", "vframe", "BER%", "SNRdB", "voice bit errors");
    for (const auto& f : cases) {
        const Result r = run(f);
        const DstarTelemetry& t = r.t;
        const DstarCall* a = callOf(t, "N0CALL");
        const DstarCall* b = callOf(t, "LY1TST");
        printf("  %-28s %5zu %5llu %4llu %4llu %4zu %6llu %6.2f %6.1f %llu / %llu\n", f.name, t.log.size(), (unsigned long long)t.headersOk, (unsigned long long)t.slowHeaders,
               (unsigned long long)t.lateEntries, t.positions.size(), (unsigned long long)t.voiceFrames, t.berPct, t.snrDb, (unsigned long long)r.voiceErr, (unsigned long long)r.voiceBits);
        const bool late = f.skip > 0;
        CHECK(b != nullptr, "%s: simplex call missing", f.name);
        if (!late) CHECK(a != nullptr, "%s: repeater call missing", f.name);
        CHECK(t.log.size() == 2, "%s: %zu calls", f.name, t.log.size());
        if (a) {
            CHECK(a->sfx == "ID51" && a->ur == "CQCQCQ" && a->rpt1 == "LY0VNO B" && a->rpt2 == "LY0VNO G" && (a->flag[0] & 0x40), "%s: header fields", f.name);
            CHECK(a->text == "OnAir D-STAR test 73", "%s: text '%s'", f.name, a->text.c_str());
            CHECK(a->hasPos, "%s: no position in the call", f.name);
            if (!late) CHECK(a->voiceFrames >= 195 && a->voiceFrames <= 201, "%s: %u voice frames", f.name, a->voiceFrames);
            if (f.hdrErr > 100 || late) CHECK(a->headerFromSlow, "%s: header should come from the slow data", f.name);
            else CHECK(!a->headerFromSlow, "%s: header from the radio header", f.name);
            if (late) CHECK(a->lateEntry, "%s: late entry", f.name);
            if (late) CHECK(a->durSec > 2.3 && a->durSec < 3.4, "%s: duration %.2f", f.name, a->durSec);
            CHECK(a->endSeen == !f.noEnd, "%s: end pattern %d", f.name, a->endSeen);
            if (!late) CHECK(std::fabs(a->durSec - 4.0) < 0.05, "%s: duration %.2f", f.name, a->durSec);
        }
        if (b) {
            CHECK(b->ur == "CQCQCQ" && b->rpt1 == "DIRECT" && !(b->flag[0] & 0x40), "%s: simplex header", f.name);
            CHECK(b->text == "Hello via simplex", "%s: text '%s'", f.name, b->text.c_str());
            CHECK(b->squelch == 12, "%s: squelch %d", f.name, b->squelch);
            // without an end pattern the call ends on carrier loss (three weak frames in a row, the two before taken back), so a few noise frames
            // may be counted as voice first; how many depends on the noise realisation (libm last bits), so allow up to 6 more
            CHECK(b->voiceFrames >= 145 && b->voiceFrames <= (f.noEnd ? 157u : 151u), "%s: %u voice frames", f.name, b->voiceFrames);
        }
        CHECK(posNear(t, "N0CALL", 54.68720, 25.27970), "%s: NMEA position", f.name);
        CHECK(posNear(t, "LY1TST-7", 54.89850, 23.90360), "%s: D-PRS position", f.name);
        CHECK(t.inverted == f.swap, "%s: inverted %d", f.name, t.inverted);
        if (!late && f.snr >= 30 && f.noise == 0 && !f.noEnd) CHECK(r.voiceBits > 0 && r.voiceErr == 0, "%s: voice bits %llu wrong", f.name, (unsigned long long)r.voiceErr);
        if (!late && f.noEnd) CHECK(r.voiceBits > 0 && r.voiceErr < r.voiceBits / 100, "%s: voice BER %llu / %llu", f.name, (unsigned long long)r.voiceErr, (unsigned long long)r.voiceBits);   // the trailing noise frames (see above)
        if (!late && f.snr < 30) CHECK(r.voiceBits > 0 && r.voiceErr < r.voiceBits / 100, "%s: voice BER %llu / %llu", f.name, (unsigned long long)r.voiceErr, (unsigned long long)r.voiceBits);
        if (f.cfo != 0 && !f.swap) CHECK(std::fabs(t.cfoHz - f.cfo) < 150, "%s: offset %.0f Hz", f.name, t.cfoHz);
    }

    // noise only: nothing heard
    {
        std::vector<cf32> x((size_t)(6 * kRate), cf32(0, 0));
        impair::noise(x, -100, 3);   // only noise
        for (auto& v : x) v *= 0.05f;
        DstarReceiver r;
        r.setSilent(true);
        r.configure(kRate); r.setSignalOffset(kOff);
        for (size_t o = 0; o < x.size(); o += 4096) r.feed(x.data() + o, std::min<size_t>(4096, x.size() - o));
        DstarTelemetry t;
        r.telemetry(t, 0);
        printf("  noise only: %zu calls, %llu headers bad, %llu late\n", t.log.size(), (unsigned long long)t.headersBad, (unsigned long long)t.lateEntries);
        CHECK(t.log.empty() && t.messages.empty() && t.positions.empty(), "noise only: %zu calls", t.log.size());
    }

    // the BER curve: the bit errors before any FEC, over the SNR
    printf("  SNR dB  header ok  voice BER %%\n");
    for (double snr : {20.0, 14.0, 11.0, 9.0}) {
        Fault f{"curve"}; f.snr = snr;
        const Result r = run(f);
        printf("  %6.1f  %9llu  %10.3f\n", snr, (unsigned long long)r.t.headersOk, r.voiceBits ? 100.0 * (double)r.voiceErr / (double)r.voiceBits : -1.0);
    }

    printf(fails ? "dstar rx: %d FAILED\n" : "dstar rx: all passed\n", fails);
    return fails ? 1 : 0;
}
