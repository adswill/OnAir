// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multitone paging detector: the synthetic test signal through the receiver (multitone_gen.h, multitone_rx.h). The signal is invented (the real
// format is not public): tones, then FSK words with the POCSAG BCH check behind an invented header. Checked: the tones and their lengths, the
// baud rate at 512 / 1200 / 2400, both polarities, a carrier error of +-2 kHz, low SNR, other sample rates, noise alone (nothing logged), a
// standard POCSAG batch (its sync word is flagged), a plain tone and noise through the FSK slicer, and the results file.
#include "dect2/multitone/multitone_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/multitone/multitone_rx.h"
#include "dect2/multitone/multitone_tel.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static MultitoneTelemetry run(MultitoneGenConfig c, double secs, MultitoneReceiver* keep = nullptr) {
    MultitoneGenerator g(c);
    MultitoneReceiver local;
    MultitoneReceiver& r = keep ? *keep : local;
    r.configure(c.rate);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(secs * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    MultitoneTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const MtBurst* firstBurst(const MultitoneTelemetry& t) { return t.bursts.empty() ? nullptr : &t.bursts.front(); }

static void checkData(const char* what, const MultitoneTelemetry& t, double baud, int pol) {
    const MtBurst* b = firstBurst(t);
    CHECK(b != nullptr, "%s: no FSK burst (%zu tx)", what, t.tx.size());
    if (!b) return;
    CHECK(std::fabs(b->baud - baud) < 0.01 * baud, "%s: baud %.2f, expected %.0f", what, b->baud, baud);
    (void)pol;   // the BCH check passes an inverted stream too: polarity is not told
    CHECK(b->bestRun >= 8 && b->validWords >= 10, "%s: %d valid words, run %d of %d", what, b->validWords, b->bestRun, b->totalWords);
    CHECK(!b->syncFound, "%s: POCSAG sync claimed in an invented stream", what);
    CHECK(b->verdict.find("unverified") != std::string::npos && b->verdict.find("NOT known") != std::string::npos, "%s: verdict '%s'", what, b->verdict.c_str());
    // the words read back are the generator's, in order, from the first valid one
    const auto words = multitoneTestWords(12);
    bool match = false;
    for (size_t i = 0; i + 1 < b->words.size() && !match; i++) if ((b->words[i] == words[1] && b->words[i + 1] == words[2]) || (b->words[i] == ~words[1] && b->words[i + 1] == ~words[2])) match = true;
    CHECK(match, "%s: the generator's words 1 and 2 not found in the output", what);
}

int main() {
    MultitoneGenConfig c;
    c.rate = 2e6;
    c.snrDb = 30;
    {   // the cycle: a tone sequence, then data
        const MultitoneTelemetry t = run(c, 10.5);
        printf("  cycle: %s\n", multitoneSummary(t).c_str());
        CHECK(t.tx.size() == 2, "%zu transmissions", t.tx.size());
        const MtTx* tone = nullptr;
        for (const auto& x : t.tx) if (x.burst == 0) tone = &x;
        CHECK(tone && tone->toneCount == 3, "tones %d", tone ? tone->toneCount : -1);
        if (tone) {
            CHECK(tone->kind == "tone sequence", "kind '%s'", tone->kind.c_str());
            float want[3] = {238.3f, 474.6f, 1000.f};
            int found = 0;
            for (float w : want) for (const auto& tn : t.tones) if (std::fabs(tn.hz - w) < 4 && tn.totalSec > 0.25 && tn.totalSec < 0.65) found++;
            CHECK(found == 3, "%d of 3 tones with the right length (%s)", found, tone->tones.c_str());
            CHECK(std::fabs(tone->devHz - 2500) < 400, "tone deviation %.0f Hz", tone->devHz);
            bool l1 = false;
            for (float h : tone->lineHz) if (std::fabs(h - 474.6f) < 12) l1 = true;
            CHECK(l1, "no spectral line near 474.6 Hz");
        }
        checkData("1200", t, 1200, 1);
        if (firstBurst(t)) CHECK(firstBurst(t)->nearestStd == "1200" && std::fabs(firstBurst(t)->durSec - 1.0) < 0.6, "std '%s' dur %.2f", firstBurst(t)->nearestStd.c_str(), firstBurst(t)->durSec);
        CHECK(t.blocksOk >= 10 && t.state >= 0 && t.dataValid, "counters");
    }
    for (double baud : {512.0, 2400.0}) {
        MultitoneGenConfig d = c; d.content = 3; d.baud = baud;
        const MultitoneTelemetry t = run(d, 3.5 + 12.0 * 32.0 / baud);
        char w[32]; snprintf(w, sizeof w, "%.0f baud", baud);
        checkData(w, t, baud, 1);
    }
    {   // inverted polarity
        MultitoneGenConfig d = c; d.content = 3; d.invert = true;
        checkData("inverted", run(d, 3.5), 1200, -1);
    }
    for (double off : {-2000.0, 2000.0}) {   // a carrier 2 kHz off the channel centre
        MultitoneGenConfig d = c; d.content = 3; d.carrierOffsetHz = off;
        char w[32]; snprintf(w, sizeof w, "offset %+.0f", off);
        checkData(w, run(d, 3.5), 1200, 1);
    }
    {   // weak
        MultitoneGenConfig d = c; d.content = 3; d.snrDb = 18;
        checkData("18 dB", run(d, 3.5), 1200, 1);
    }
    for (double fs : {1e6, 2.4e6}) {
        MultitoneGenConfig d = c; d.content = 3; d.rate = fs;
        char w[32]; snprintf(w, sizeof w, "%.1f Msps", fs / 1e6);
        checkData(w, run(d, 3.5), 1200, 1);
    }
    // audio FFSK (the structure measured on the real recordings): found, baud, tones, the 0x605A pair and the words after it
    for (int variant = 0; variant < 4; variant++) {
        MultitoneGenConfig d = c; d.content = 5;
        if (variant == 1) d.carrierOffsetHz = 2000;
        if (variant == 2) d.snrDb = 20;
        if (variant == 3) d.rate = 1e6;
        const MultitoneTelemetry t = run(d, 3.0);
        const MtBurst* b = firstBurst(t);
        CHECK(b && b->ffsk, "FFSK %d: no FFSK burst (%zu tx)", variant, t.tx.size());
        if (!b) continue;
        CHECK(std::fabs(b->baud - 585.3) < 4 && std::fabs(b->toneHiHz - 585.3) < 4 && std::fabs(b->toneLoHz - 292.65) < 2, "FFSK %d: baud %.1f tones %.1f %.1f", variant, b->baud, b->toneLoHz, b->toneHiHz);
        CHECK(b->syncCount >= 2 && b->afterSync.rfind("7B91 310E C47F FFFF", 0) == 0, "FFSK %d: sync x%d, after '%s'", variant, b->syncCount, b->afterSync.c_str());
        CHECK(b->bestRun < 2 && !b->syncFound && t.tx[0].kind == "FFSK data" && b->verdict.find("unverified") != std::string::npos, "FFSK %d: run %d kind '%s' verdict '%s'", variant, b->bestRun, t.tx[0].kind.c_str(), b->verdict.c_str());
    }
    {   // noise alone: nothing logged
        MultitoneGenConfig d = c; d.content = 1;
        const MultitoneTelemetry t = run(d, 8);
        CHECK(t.tx.empty() && t.bursts.empty() && t.txTotal == 0, "noise only: %zu tx, %zu bursts", t.tx.size(), t.bursts.size());
    }
    {   // tones alone: no data may be claimed
        MultitoneGenConfig d = c; d.content = 2;
        const MultitoneTelemetry t = run(d, 5);
        CHECK(t.tx.size() == 1 && t.bursts.empty() && t.tx[0].toneCount == 3, "tones only: %zu tx, %zu bursts, %d tones", t.tx.size(), t.bursts.size(), t.tx.empty() ? -1 : t.tx[0].toneCount);
    }
    {   // a standard POCSAG batch: the sync word is found and the stream says so
        MultitoneGenConfig d = c; d.content = 4;
        const MultitoneTelemetry t = run(d, 3.5);
        const MtBurst* b = firstBurst(t);
        CHECK(b && b->syncFound && b->verdict.find("POCSAG mode") != std::string::npos, "POCSAG sync: %s", b ? b->verdict.c_str() : "no burst");
        CHECK(b && b->bestRun >= 3, "POCSAG batch run %d", b ? b->bestRun : -1);
    }
    {   // the results file
        MultitoneReceiver r;
        const MultitoneTelemetry t = run(c, 10.5, &r);
        const std::string blob = r.saveResults();
        MultitoneReceiver r2;
        r2.configure(c.rate);
        CHECK(r2.loadResults(blob), "load");
        MultitoneTelemetry t2;
        CHECK(r2.telemetry(t2, 0) && t2.tx.size() == t.tx.size() && t2.bursts.size() == t.bursts.size() && t2.tones.size() == t.tones.size(), "round trip");
        r2.clearResults();
        r2.telemetry(t2, 0);
        CHECK(t2.tx.empty() && t2.bursts.empty() && !r2.loadResults("junk"), "clear");
    }
    {   // the slicer on things that are not FSK: a plain tone and noise
        std::vector<float> tone(96000), noise(96000);
        std::mt19937 rng(5);
        dect2::normal_distribution<float> nd(0.f, 0.3f);
        for (size_t i = 0; i < tone.size(); i++) { tone[i] = 0.4f * (float)std::sin(2 * 3.14159265 * 600.0 * (double)i / 48000.0); noise[i] = nd(rng); }
        CHECK(!multitone::sliceFsk(tone, 48000).found, "a plain tone read as FSK");
        CHECK(!multitone::sliceFsk(noise, 48000).found, "noise read as FSK");
    }
    {   // the bit search on random bits: no run of valid words
        std::mt19937 rng(9);
        std::vector<uint8_t> bits(4000);
        for (auto& b : bits) b = (uint8_t)(rng() & 1);
        MtBurst b; b.baud = 1200;
        multitone::analyseBits(bits, b);
        CHECK(b.bestRun < 2 && !b.syncFound && b.verdict.find("unverified") != std::string::npos, "random bits: run %d '%s'", b.bestRun, b.verdict.c_str());
    }
    printf(fails ? "multitone rx: %d FAILED\n" : "multitone rx: all passed\n", fails);
    return fails ? 1 : 0;
}
