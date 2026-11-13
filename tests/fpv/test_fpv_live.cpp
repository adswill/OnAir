// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FPV video receiver on the test signal: the picture locks (PAL and NTSC), the sound subcarrier is demodulated, the channel levels follow what lies in
// the capture, an empty channel reports nothing, and the signal a MHz or two off the channel centre is still found.
#include "dect2/fpv/fpv_gen.h"
#include "dect2/fpv/fpv_live.h"
#include "dect2/fpv/fpv_chan.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

namespace {

struct Run {
    FpvTelemetry t;
    std::shared_ptr<const AtvFrame> frame;
    std::vector<float> audio;
    int lockedAfterMs = -1;
};

Run run(const FpvGenConfig& g, double centreHz, double secs, bool audio) {
    Run r;
    FpvGenerator gen(g);
    FpvReceiver rx;
    rx.setSilent(true);
    rx.configure(g.rate);
    rx.setCenterHz(centreHz);
    rx.setAudio(audio);
    if (audio) rx.setAudioTap([&](const float* m, size_t n) { r.audio.insert(r.audio.end(), m, m + n); });
    uint64_t seq = 0, fseq = 0;
    std::vector<cf32> buf(65536);
    size_t done = 0;
    const size_t total = (size_t)(secs * g.rate);
    while (done < total) {
        const size_t n = std::min(buf.size(), total - done);
        gen.generate(buf.data(), n);
        rx.feed(buf.data(), n);
        done += n;
        FpvTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; r.t = t; if (t.state == 2 && r.lockedAfterMs < 0) r.lockedAfterMs = (int)(1000.0 * (double)done / g.rate); }
        while (auto f = rx.frame(fseq)) r.frame = f;
    }
    return r;
}

double tone(const std::vector<float>& a, size_t from, double hz, double fs) {   // power at hz, Goertzel-like, against the total power
    double re = 0, im = 0, tot = 0;
    const size_t n = a.size() - from;
    for (size_t i = 0; i < n; i++) { const double ph = 2 * M_PI * hz * (double)i / fs; re += a[from + i] * std::cos(ph); im += a[from + i] * std::sin(ph); tot += (double)a[from + i] * a[from + i]; }
    return tot > 0 ? 2.0 * (re * re + im * im) / ((double)n * n) / (tot / (double)n) : 0;
}

const FpvChannelInfo* ch(const FpvTelemetry& t, const char* name) { for (const auto& c : t.channels) if (c.name == name) return &c; return nullptr; }

} // namespace

int main() {
    const double r5 = 5806e6;
    // PAL, 6.5 MHz sound, on the channel centre
    {
        FpvGenConfig g; g.cnrDb = 30; g.cfoHz = 0.4e6;
        const Run r = run(g, r5, 3.0, true);
        const FpvTelemetry& t = r.t;
        printf("PAL: state %d, %s, tuned %s %.0f MHz, level %.1f dBFS (%.1f over noise), video SNR %.1f dB, locked after %d ms, sound %d sub %.1f MHz level %.1f dB\n", t.state,
               t.video.system.c_str(), t.tunedName.c_str(), t.tunedHz / 1e6, t.levelDb, t.overDb, t.snrDb, r.lockedAfterMs, (int)t.audioPresent, t.audioSubHz / 1e6, t.audioLevelDb);
        CHECK(t.state == 2 && t.dataValid, "PAL: not locked (state %d)", t.state);
        CHECK(t.video.lines == 625 && t.video.colourSystem == "PAL", "PAL: %d lines %s", t.video.lines, t.video.colourSystem.c_str());
        CHECK(t.tunedName == "R5", "tuned channel '%s'", t.tunedName.c_str());
        CHECK(t.overDb > 8, "PAL: %.1f dB over noise", t.overDb);
        CHECK(r.frame && r.frame->width == 768 && r.frame->height == 576, "PAL: picture size");
        CHECK(t.audioPresent && t.audioSubHz == 6.5e6, "sound subcarrier %.2f MHz present %d (sub dB %.1f / %.1f)", t.audioSubHz / 1e6, (int)t.audioPresent, t.audioSubDb[0], t.audioSubDb[1]);
        CHECK(t.audioSubDb[1] > 15 && t.audioSubDb[0] < 6, "subcarrier strengths %.1f / %.1f dB", t.audioSubDb[0], t.audioSubDb[1]);
        CHECK(r.audio.size() > 48000, "%zu audio samples", r.audio.size());
        if (r.audio.size() > 48000) {
            const size_t from = 24000;   // after the demodulator has settled
            const double p = tone(r.audio, from, 1000.0, 48000.0);
            printf("  sound: %.0f %% of the power at 1 kHz\n", p * 100);
            CHECK(p > 0.5, "1 kHz tone is only %.0f %% of the sound", p * 100);
        }
    }
    // NTSC with the sound on 6.0 MHz (found by itself)
    {
        FpvGenConfig g; g.ntsc = 1; g.sound = 2; g.cnrDb = 28;
        const Run r = run(g, r5, 3.0, true);
        const FpvTelemetry& t = r.t;
        printf("NTSC: state %d, %s, sound sub %.1f MHz\n", t.state, t.video.system.c_str(), t.audioSubHz / 1e6);
        CHECK(t.state == 2, "NTSC: not locked (state %d)", t.state);
        CHECK(t.video.lines == 525, "NTSC: %d lines", t.video.lines);
        CHECK(t.audioSubHz == 6.0e6 && t.audioPresent, "NTSC: sound subcarrier %.2f MHz", t.audioSubHz / 1e6);
    }
    // the radio is a little off the channel (3 MHz): the receiver mixes the channel down itself
    {
        FpvGenConfig g; g.cnrDb = 30; g.offsetHz = 5806e6 - 5803e6;
        const Run r = run(g, 5803e6, 3.0, false);
        printf("off-centre: state %d, tuned %s, %s\n", r.t.state, r.t.tunedName.c_str(), r.t.video.system.c_str());
        CHECK(r.t.state == 2 && r.t.video.lines == 625, "off-centre: state %d, %d lines", r.t.state, r.t.video.lines);
    }
    // no transmitter: nothing is reported, no sound
    {
        FpvGenConfig g; g.off = true; g.cnrDb = 30;
        const Run r = run(g, r5, 2.0, true);
        printf("empty: state %d, over noise %.1f dB, sound %d\n", r.t.state, r.t.overDb, (int)r.t.audioPresent);
        CHECK(r.t.state == 0 && !r.t.dataValid, "empty: state %d", r.t.state);
        CHECK(!r.t.audioPresent, "empty: sound present");
        const FpvChannelInfo* c = ch(r.t, "R5");
        CHECK(c && !c->present, "empty: R5 present");
        // a transmitter after the empty channel: the noise floor learnt here makes it stand out
    }
    // a scan: step the centre over the channels, transmitter on R5 and A8 only (the generator is told where the capture is)
    {
        const double rate = 20e6;
        FpvReceiver rx;
        rx.setSilent(true);
        rx.configure(rate);
        const auto all = fpvAllChannels();
        const double txs[2] = {5806e6, 5725e6};
        const double centres[] = {5658e6, 5695e6, 5725e6, 5732e6, 5769e6, 5806e6, 5843e6, 5880e6, 5917e6};
        uint64_t seq = 0;
        FpvTelemetry t;
        for (double c : centres) {
            // by default only the empty R1, both transmitters and the top end (the middle steps measure the same way); the long run steps every centre
            if (!testLong() && c != 5658e6 && c != 5725e6 && c != 5806e6 && c != 5917e6) continue;
            rx.setCenterHz(c);
            // the strongest transmitter inside the capture (±5 MHz), or none
            const double off5806 = std::fabs(txs[0] - c), off5725 = std::fabs(txs[1] - c);
            FpvGenConfig g; g.cnrDb = 30;
            if (off5806 <= 5e6) g.offsetHz = txs[0] - c; else if (off5725 <= 5e6) g.offsetHz = txs[1] - c; else g.off = true;
            FpvGenerator gen(g);
            std::vector<cf32> buf(65536);
            for (size_t done = 0; done < (size_t)(1.0 * rate); done += buf.size()) {
                gen.generate(buf.data(), buf.size());
                rx.feed(buf.data(), buf.size());
                FpvTelemetry x;
                if (rx.telemetry(x, seq)) { seq = x.seq; t = x; }
            }
        }
        int present = 0;
        for (const auto& c : t.channels) if (c.present) { present++; printf("  scan: %s %.0f MHz %.1f dBFS %.1f dB over noise\n", c.name.c_str(), c.freqHz / 1e6, c.levelDb, c.overDb); }
        const FpvChannelInfo* a = ch(t, "R5");
        const FpvChannelInfo* b = ch(t, "A8");
        const FpvChannelInfo* e = ch(t, "R1");
        CHECK(a && a->present && a->overDb > 15, "scan: R5 %s", a ? "weak" : "missing");
        CHECK(b && b->present && b->overDb > 15, "scan: A8 %s", b ? "weak" : "missing");
        CHECK(e && !e->present && e->ageSec >= 0, "scan: R1 should be measured and empty");
        CHECK(ch(t, "G1") && ch(t, "G1")->ageSec < 0, "scan: the 1.2 GHz channels were never in the capture");
        CHECK(present >= 2 && present <= 8, "scan: %d channels present", present);
    }
    // below 16 Msps the receiver says it cannot work
    {
        FpvReceiver rx; rx.configure(10e6);
        CHECK(!rx.ready(), "10 Msps must not be ready");
    }
    return testSummary("fpv live");
}
