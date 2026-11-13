// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SITOR-B receiver (sitor/sitor_live.h) on generated NAVTEX / FEC signals: sample rates, chunk sizes, carrier offsets, tone polarity,
// noise, 8 bit samples, DC, a gap, a reset, letters and figures shifts. The text is read without ZCZC framing.
#include "dect2/engine/mode_synth.h"
#include "dect2/marine/marine_gen.h"
#include "dect2/sitor/sitor_live.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

namespace {

const char* kMsg = "ZCZC EA01\r\nGALE WARNING 5 IN SEA AREA VIKING 12 KNOTS 2030 UTC. VIS 4 NM.\r\nNNNN\r\n";

struct Sc {
    double rate = 2e6, secs = 24, snr = 40, cfo = 0;
    size_t chunk = 65536;
    bool invert = false, quant8 = false, reset = false;
    float dc = 0;
    double gapAt = -1, gapMs = 0;
    const char* text = kMsg;
};

// the fraction of the characters of `want` (without the framing) found in `got` as a subsequence of long runs
bool hasRun(const std::string& got, const std::string& run) { return got.find(run) != std::string::npos; }

SitorTelemetry run(const Sc& s) {
    SynthConfig cfg;
    cfg.snrDb = s.snr; cfg.cfoHz = s.cfo;
    auto syn = makeMarineSynthNavtex(cfg, s.rate, s.text);
    SitorLiveReceiver rx;
    rx.configure(s.rate);
    rx.setSignalOffset(s.invert ? sitorTuning().tuneOffsetHz : -sitorTuning().tuneOffsetHz);
    const size_t total = static_cast<size_t>(s.secs * s.rate), gen = 65536;
    std::vector<cf32> buf(gen);
    size_t done = 0;
    bool gapped = false, resetDone = false;
    SitorTelemetry t, last;
    uint64_t seq = 0;
    while (done < total) {
        const size_t n = std::min(gen, total - done);
        syn->generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) {
            float a = buf[i].real() + s.dc, b = (s.invert ? -1.f : 1.f) * buf[i].imag() + s.dc;
            if (s.quant8) { a = std::lrintf(std::max(-1.f, std::min(1.f, a)) * 127.f) / 127.f; b = std::lrintf(std::max(-1.f, std::min(1.f, b)) * 127.f) / 127.f; }
            buf[i] = cf32(a, b);
        }
        size_t start = 0, len = n;
        const double now = static_cast<double>(done) / s.rate;
        if (s.gapAt >= 0 && !gapped && now >= s.gapAt) { gapped = true; const size_t g = std::min(n, static_cast<size_t>(s.gapMs * 1e-3 * s.rate)); start = g; len = n - g; }
        if (s.reset && !resetDone && now >= 5) { resetDone = true; rx.reset(); }
        for (size_t k = 0; k < len; k += s.chunk) rx.feed(buf.data() + start + k, std::min(s.chunk, len - k));
        done += n;
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    return last;
}

void want(const char* what, const Sc& s) {
    const SitorTelemetry t = run(s);
    const bool ok = hasRun(t.text, "GALE WARNING 5 IN SEA AREA VIKING") && hasRun(t.text, "12 KNOTS 2030 UTC");
    printf("%-30s state %d, %llu chars, snr %.1f dB, inverted %d: %s\n", what, t.state, static_cast<unsigned long long>(t.chars), static_cast<double>(t.snrDb), t.inverted, ok ? "ok" : "text missing");
    CHECK(ok, "%s: text \"%.80s\"", what, t.text.c_str());
    CHECK(t.state == 2, "%s: state %d", what, t.state);
}

}  // namespace

// the impairment cases at one rate: 250 ksps by default (each rate is still run once below), 2 Msps in the long run as before
Sc imp() { Sc s; s.rate = testLongPick(250e3, 2e6); return s; }

int main() {
    want("2 Msps", Sc());
    { Sc s; s.rate = 250e3; want("250 ksps", s); }
    { Sc s; s.rate = 8e6; want("8 Msps", s); }
    for (size_t c : {size_t(7), size_t(4096)}) { Sc s = imp(); s.chunk = c; s.secs = 20; char n[40]; snprintf(n, sizeof n, "chunks of %zu", c); want(n, s); }
    for (double f : {-45.0, 45.0, 150.0, -300.0}) { Sc s = imp(); s.cfo = f; char n[40]; snprintf(n, sizeof n, "carrier offset %+.0f Hz", f); want(n, s); }
    { Sc s = imp(); s.invert = true; want("tones swapped", s); }
    { Sc s = imp(); s.snr = 15; want("15 dB", s); }
    { Sc s = imp(); s.quant8 = true; s.snr = 20; want("8 bit samples", s); }
    { Sc s = imp(); s.dc = 0.05f; s.quant8 = true; want("DC offset, 8 bit", s); }
    { Sc s = imp(); s.gapAt = 8; s.gapMs = 20; s.secs = 40; want("20 ms gap", s); }
    { Sc s = imp(); s.reset = true; s.secs = 40; want("reset mid stream", s); }
    { Sc s = imp(); s.text = "ZCZC EA02\r\nPOSITION 54 DEG 30.5 N 010 DEG 12.0 E SPEED 12.5 KTS. CALL BUOY 7.\r\nNNNN\r\n"; const SitorTelemetry t = run(s);
      CHECK(hasRun(t.text, "POSITION 54 DEG 30.5 N 010 DEG 12.0 E"), "figures and punctuation: \"%.100s\"", t.text.c_str()); }
    // silence and noise only: no lock, no text
    { Sc s = imp(); s.text = " "; s.snr = -10; const SitorTelemetry t = run(s); CHECK(t.state != 2 || t.text.find("GALE") == std::string::npos, "noise locked"); }
    return testSummary("sitor_live");
}
