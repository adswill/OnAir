// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA voice echo: the same piece of speech must reach the codec once. The clear cell of scene 1 sends a traffic burst of its own random bits in
// every frame of timeslot 3 (the bits depend on the slot counter, so each burst is a fingerprint of its air time). Captures that follow each other,
// a capture decoded twice, and captures that overlap must not give any burst twice; the frames of a lost stretch are silence, never a repeat.
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_voice.h"
#include "dect2/source/source.h"
#include <cstdio>
#include <map>
#include <vector>
using namespace dect2::tetra;
#include "../common/test_util.h"

struct Rec final : SpeechFrameSink {
    std::map<std::vector<uint8_t>, int> seen;
    int coded = 0, gaps = 0;
    void onCodedFrame(const uint8_t* b, size_t n) override { coded++; seen[std::vector<uint8_t>(b, b + n)]++; }
    void onMissingBurst() override { gaps++; }
    int repeats() const { int r = 0; for (const auto& kv : seen) if (kv.second > 1) r += kv.second - 1; return r; }
};

int main() {
    const double rate = 300e3, now = 100;
    const dect2::TetraCellCfg cell = dect2::tetraTestCell(1, 1);
    const int ts = cell.trafficTs;
    dect2::SynthConfig sc; sc.mode = 48; sc.snrDb = 30; sc.modeOpt[0] = 1;
    auto syn = dect2::makeTetraSynth(sc, rate);
    const size_t cap = (size_t)(rate * 2.3);
    std::vector<dect2::cf32> x(cap * 4);   // four captures' worth of continuous air
    syn->generate(x.data(), x.size());
    const double off = 392.6e6 - dect2::kTetraTestCentreHz;
    auto chan = [&](size_t from, size_t n) { return channelize(x.data() + from, n, rate, off); };
    auto calls = [&](CallTracker& c) {
        c.setMainCarrier(cell.si.mainCarrier);
        CallEvent e; e.kind = 1; e.callId = 7; e.enc = 0; e.slots = 1 << (4 - ts); e.carrier = cell.si.mainCarrier; c.onEvent(e, now);
    };

    {   // captures one after the other: every burst once, the stretch between them is silence
        Rec r; VoiceTimeline tl; CallTracker c; calls(c);
        int dup = 0;
        for (int k = 0; k < 3; k++) {
            const auto s = chan(cap * (size_t)k, cap);
            dup += decodeCarrier(s.data(), s.size(), -1, -1, -1, &r, &c, now, ts, &tl).voiceDup;
        }
        CHECK(r.coded > 80, "three captures gave %d bursts", r.coded);
        CHECK(r.repeats() == 0, "consecutive captures: %d bursts played twice", r.repeats());
        CHECK(dup == 0, "consecutive captures: nothing is dropped as a repeat (%d)", dup);
    }
    {   // the same capture decoded twice (the same call found on two carriers, or a capture handed over twice): the second time adds nothing
        Rec r; VoiceTimeline tl; CallTracker c; calls(c);
        const auto s = chan(0, cap);
        const CarrierDecode d1 = decodeCarrier(s.data(), s.size(), -1, -1, -1, &r, &c, now, ts, &tl);
        const int first = r.coded, gaps = r.gaps;
        const CarrierDecode d2 = decodeCarrier(s.data(), s.size(), -1, -1, -1, &r, &c, now, ts, &tl);
        CHECK(first > 30 && d1.voiceDup == 0, "first decode: %d bursts, %d dropped", first, d1.voiceDup);
        CHECK(r.coded == first && r.gaps == gaps && r.repeats() == 0, "second decode of the same air: %d more bursts, %d more silences, %d repeats", r.coded - first, r.gaps - gaps, r.repeats());
        CHECK(d2.voiceDup > 30, "second decode: its frames are dropped as repeats (%d)", d2.voiceDup);
    }
    {   // overlapping captures: the second starts 0.6 s before the first ended
        Rec r; VoiceTimeline tl; CallTracker c; calls(c);
        const auto a = chan(0, cap);
        decodeCarrier(a.data(), a.size(), -1, -1, -1, &r, &c, now, ts, &tl);
        const size_t from = cap - (size_t)(rate * 0.6);
        const auto b = chan(from, cap);
        const CarrierDecode d = decodeCarrier(b.data(), b.size(), -1, -1, -1, &r, &c, now, ts, &tl);
        CHECK(r.repeats() == 0, "overlapping captures: %d bursts played twice", r.repeats());
        CHECK(d.voiceDup >= 5, "the overlap is dropped (%d frames)", d.voiceDup);
    }
    {   // no timeline kept by the caller: a capture is still free of repeats inside itself
        Rec r; CallTracker c; calls(c);
        const auto s = chan(0, cap);
        decodeCarrier(s.data(), s.size(), -1, -1, -1, &r, &c, now, ts, nullptr);
        CHECK(r.coded > 30 && r.repeats() == 0, "one capture without a timeline: %d bursts, %d repeats", r.coded, r.repeats());
    }
    {   // FrameBuffer keeps the order of speech and silence and hands exactly that on
        FrameBuffer fb; Rec r;
        const uint8_t a[4] = {1, 0, 1, 1}, b[4] = {0, 0, 1, 0};
        fb.onMissingBurst(); fb.onCodedFrame(a, 4); fb.onMissingBurst(); fb.onCodedFrame(b, 4);
        CHECK(fb.size() == 4, "buffer holds %zu events", fb.size());
        fb.flush(r);
        CHECK(r.coded == 2 && r.gaps == 2 && r.seen.size() == 2, "flush: %d bursts, %d silences", r.coded, r.gaps);
    }
    {   // a carrier that gave only silence is not the capture's voice; a later one with speech is (VoiceStreamPicker with a quiet followed stream)
        VoiceStreamPicker p; int tn = 0;
        p.newCapture();
        CHECK(p.offer(1, 10, tn) && tn == -1, "nothing followed: the first carrier is offered");
        p.heard(1, 0, 10);
        CHECK(p.offer(2, 10, tn), "the first carrier gave no speech: the next one is still offered");
        p.heard(2, 3, 10);
        CHECK(!p.offer(1, 10, tn) && !p.offer(3, 10, tn), "speech heard: no other carrier of the capture reaches the sink");
        p.newCapture();
        CHECK(p.offer(2, 12.3, tn) && tn == 3 && !p.offer(1, 12.3, tn), "next capture: the same carrier and slot only");
    }
    return testSummary("tetra echo");
}
