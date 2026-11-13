// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB (MPEG-1/2 Layer II) services through the whole receiver: every one of the 64 UEP profiles of EN 300 401 table 8 (32 to 384 kbit/s, protection
// levels 1 to 5) and EEP A/B levels, mono and stereo, 48 kHz and 24 kHz (MPEG-2 half sampling rate). The logical frames the receiver decodes must be
// the transmitter's to the bit, and the audio must be the tone: no clicks (the largest step between samples no bigger than the clean tone's) and
// no DC. At 24 kHz an MP2 frame spans two logical frames: it must be decoded whole. Plus: the UEP table checked row by row against the
// structure the standard gives it (the punctured length of every row fills its sub-channel to within the padding the standard allows).
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_gen.h"
#include "../common/jobs.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL line %d: ", __LINE__); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static const double kRate = 2.048e6;

struct Case { std::string name; int bitrate, uep, option, level, channels, rate; double snr; };

static void runCase(const Case& c) {
    dabgen::TxConfig tc;
    tc.utcSeconds = 1700000000;
    dabgen::TxService s;
    s.sid = 0xCE07; s.label = "MP2 test"; s.subId = 7; s.bitrate = c.bitrate; s.dabPlus = false; s.sampleRate = c.rate; s.channels = c.channels;
    s.leftHz = 1500; s.rightHz = 750; s.uepIndex = c.uep; s.option = c.option; s.level = c.level;
    tc.services = {s};
    dabgen::Transmitter ref(tc);
    if (ref.mp2Frames(0).empty()) { jprintf("%-36s skipped (no MP2 encoder for it)\n", c.name.c_str()); return; }
    std::set<std::vector<uint8_t>> sent;
    for (int f = 0; f < 500; f++) { std::vector<uint8_t> b; ref.logicalFrame(0, f, b); sent.insert(b); }
    SynthConfig sc;
    sc.snrDb = c.snr;
    auto syn = makeDabSynth(tc, sc, kRate);
    DabReceiver rx;
    rx.configure(kRate);
    rx.audio().setSilent(true);
    rx.select(7);
    uint64_t frames = 0, exact = 0;
    rx.setFrameTap([&](int sub, const uint8_t* b, int n) { if (sub != 7) return; frames++; exact += sent.count(std::vector<uint8_t>(b, b + n)); });
    std::vector<float> pcm;
    rx.audio().setPcmTap([&](const float* st, int n) { for (int i = 0; i < n; i++) pcm.push_back(st[2 * i]); });
    std::vector<cf32> x(32768);
    for (size_t d = 0; d < (size_t)(4 * kRate); d += x.size()) { syn->generate(x.data(), x.size()); rx.feed(x.data(), x.size()); }
    // the audio after the first 0.5 s it put out: DC and the largest step against the 1.5 kHz tone's own (amplitude 0.5: 2 pi 1500 / 48000 * 0.5)
    double dc = 0, step = 0;
    const size_t a = std::min(pcm.size(), (size_t)24000), b = pcm.size();
    for (size_t i = a; i < b; i++) dc += pcm[i];
    if (b > a) dc /= (double)(b - a);
    for (size_t i = a + 1; i < b; i++) step = std::max(step, (double)std::fabs(pcm[i] - pcm[i - 1]));
    const double toneStep = 2 * 3.14159265 * 1500 / 48000 * 0.5;
    const DabAudioStats st = rx.audio().stats();
    jprintf("%-36s %4llu frames, %4llu exact, audio %.2f s, %d Hz %d ch, DC %+.4f, largest step %.3f (tone %.3f)\n", c.name.c_str(), (unsigned long long)frames,
            (unsigned long long)exact, (double)pcm.size() / 48000, st.sampleRate, st.channels, dc, step, toneStep);
    CHECK(frames > 100 && exact == frames, "%s: %llu of %llu logical frames exact", c.name.c_str(), (unsigned long long)exact, (unsigned long long)frames);
    // as much audio as the logical frames carry (24 ms each), not more: half an MP2 frame of 24 kHz decoded as a whole one gave about 1.5 times that
    CHECK(pcm.size() > 48000 * 2 && (double)pcm.size() / 48000 < (double)frames * 0.024 + 0.3, "%s: %.2f s of audio from %llu logical frames", c.name.c_str(),
          (double)pcm.size() / 48000, (unsigned long long)frames);
    CHECK(std::fabs(dc) < 0.01 && step < 1.6 * toneStep, "%s: DC %.4f, step %.3f", c.name.c_str(), dc, step);
}

// A scrambled service (FIG 0/2 CAId and CA flag): it is reported as scrambled and not played (decoded it would be noise); a clear one next to it plays
static void conditionalAccess() {
    dabgen::TxConfig tc;
    tc.utcSeconds = 1700000000;
    tc.services = dabgen::defaultServices();
    tc.services[0].ca = true;   // "OnAir Tones 1", sub-channel 1
    SynthConfig sc;
    sc.snrDb = 25;
    for (int sub : {1, 3}) {
        auto syn = makeDabSynth(tc, sc, kRate);
        DabReceiver rx;
        rx.configure(kRate);
        rx.audio().setSilent(true);
        rx.select(sub);
        size_t loud = 0;
        rx.audio().setPcmTap([&](const float* st, int n) { for (int i = 0; i < n; i++) loud += std::fabs(st[2 * i]) > 0.01f; });
        std::vector<cf32> x(32768);
        for (size_t d = 0; d < (size_t)(3 * kRate); d += x.size()) { syn->generate(x.data(), x.size()); rx.feed(x.data(), x.size()); }
        DabTelemetry t;
        rx.telemetry(t, 0);
        const DabEnsemble e = rx.ensemble();
        const auto it = e.services.find(tc.services[0].sid);
        const bool flagged = it != e.services.end() && it->second.caId == 1 && !it->second.comps.empty() && it->second.comps[0].ca;
        jprintf("conditional access, sub-channel %d: scrambled %d, service flagged %d, %zu loud samples\n", sub, (int)t.scrambled, (int)flagged, loud);
        CHECK(flagged, "the CA flags of FIG 0/2 were not read");
        if (sub == 1) CHECK(t.scrambled && loud == 0, "scrambled service: reported %d, %zu loud samples played", (int)t.scrambled, loud);
        else CHECK(!t.scrambled && loud > 48000, "clear service next to it: reported %d, %zu loud samples", (int)t.scrambled, loud);
    }
}

int main() {
    conditionalAccess();
    std::vector<Case> cs;
    for (int idx = 0; idx < 64; idx++) {
        int size, br, lv, info;
        if (!dab::uepGeometry(idx, size, br, lv, info)) continue;
        // Layer II at 48 kHz: 32, 48, 56 and 80 kbit/s are mono only, 224 kbit/s and more stereo only (ISO/IEC 11172-3 2.4.2.3)
        const int ch = (br <= 56 || br == 80) ? 1 : 2;
        cs.push_back({"UEP " + std::to_string(idx) + " (" + std::to_string(br) + " kbit/s, level " + std::to_string(lv) + ")", br, idx, 0, 2, ch, 48000, 25});
    }
    cs.push_back({"EEP 3-A 128 kbit/s stereo", 128, -1, 0, 2, 2, 48000, 25});
    cs.push_back({"EEP 1-A 192 kbit/s stereo", 192, -1, 0, 0, 2, 48000, 25});
    cs.push_back({"EEP 2-B 128 kbit/s stereo", 128, -1, 1, 1, 2, 48000, 25});
    cs.push_back({"EEP 4-B 96 kbit/s mono", 96, -1, 1, 3, 1, 48000, 25});
    cs.push_back({"UEP 34 128 kbit/s mono", 128, 34, 0, 2, 1, 48000, 25});
    cs.push_back({"UEP 36 128 kbit/s, 8 dB", 128, 36, 0, 2, 2, 48000, 8});
    cs.push_back({"24 kHz, 64 kbit/s mono, EEP 3-A", 64, -1, 0, 2, 1, 24000, 25});
    cs.push_back({"24 kHz, 64 kbit/s UEP 15", 64, 15, 0, 2, 1, 24000, 25});
    testjobs::Jobs jobs;
    for (const Case& c : cs) jobs.add([c] { runCase(c); });
    jobs.run(4);
    if (fails) { jprintf("%d failed\n", fails.load()); return 1; }
    jprintf("test_dab_uep: all passed\n");
    return 0;
}
