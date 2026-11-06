// TETRA voice path: an unencrypted generated traffic burst reaches the speech frame sink as the right coded frame; an
// encrypted one, and one with unknown encryption state, produce "not decoded" and the sink is never called.
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_phy.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_voice.h"
#include "dect2/tetra/tetra_voice_codec.h"
#include "dect2/source/source.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2::tetra;
#include "../common/test_util.h"

struct Sink : SpeechFrameSink {
    int calls = 0, gaps = 0; Bits last;
    void onCodedFrame(const uint8_t* b, size_t n) override { calls++; last.assign(b, b + n); }
    void onMissingBurst() override { gaps++; }
};

// A generated burst: the 432 type-4 bits are scrambled the way the transmitter does, then made soft. (The speech channel interleaver is
// the traffic decoder's business, see test_tetra_traffic.cpp; the voice path hands the descrambled type-4 bits on.)
static void makeBurst(const Bits& t3, uint32_t scr, std::vector<float>& l) {
    Bits t5 = t3;
    scramble(t5.data(), 432, scr);
    l.resize(432);
    for (int i = 0; i < 432; i++) l[(size_t)i] = t5[(size_t)i] ? 1.f : -1.f;
}

int main() {
    std::mt19937 g(9);
    Bits t3(432);
    for (auto& v : t3) v = (uint8_t)(g() & 1);
    const uint32_t scr = scramblerInit(262, 1, 5);
    std::vector<float> l;
    makeBurst(t3, scr, l);

    {   // unencrypted: the coded frame reaches the sink unchanged
        Sink s; CallEncryption e; e.known = true; e.encMode = 0;
        const VoiceVerdict v = processTrafficBurst(l.data(), l.data() + 216, scr, e, &s);
        CHECK(v == VoiceVerdict::Passed, "clear burst passes");
        CHECK(s.calls == 1 && s.last == t3, "clear burst: coded frame delivered intact");
        CHECK(std::strcmp(voiceVerdictText(v), "voice (clear)") == 0, "clear text");
    }
    {   // encrypted: not decoded, sink untouched
        Sink s; CallEncryption e; e.known = true; e.encMode = 1;
        const VoiceVerdict v = processTrafficBurst(l.data(), l.data() + 216, scr, e, &s);
        CHECK(v == VoiceVerdict::EncryptedNotDecoded, "encrypted burst refused");
        CHECK(s.calls == 0, "encrypted burst: sink never called");
        CHECK(std::strcmp(voiceVerdictText(v), "encrypted, not decoded") == 0, "encrypted text");
    }
    {   // a cell that only advertises air interface encryption, with a call known clear: decoded; encrypted evidence still refuses
        Sink s; CallEncryption e; e.known = true; e.cellOffersEncryption = true;
        CHECK(processTrafficBurst(l.data(), l.data() + 216, scr, e, &s) == VoiceVerdict::Passed, "advertising cell, clear call: decoded");
        Sink s2; CallEncryption e2; e2.known = true; e2.cellOffersEncryption = true; e2.encMode = 1;
        CHECK(processTrafficBurst(l.data(), l.data() + 216, scr, e2, &s2) == VoiceVerdict::EncryptedNotDecoded && s2.calls == 0, "encrypted evidence refused");
    }
    {   // state unknown: not decoded
        Sink s; CallEncryption e;
        CHECK(processTrafficBurst(l.data(), l.data() + 216, scr, e, &s) == VoiceVerdict::StateUnknownNotDecoded && s.calls == 0, "unknown state refused");
    }
    {   // the codec loader path: a silent fake codec stands in for the user-installed plug-in
        struct Silent final : dect2::codec::SpeechCodec {
            int sampleRate() const override { return 8000; }
            std::vector<int16_t> decode(const std::vector<uint8_t>& c) override { return std::vector<int16_t>(c.size() * 4, 0); }
        };
        auto setup = [](TetraVoiceSink& vs, size_t& pcmN, bool withCodec) {
            if (withCodec) vs.setCodec(std::make_unique<Silent>());
            // test-only stand-in for the (not yet implemented) traffic channel decoder: pack 8 bits per byte
            vs.setFrameDecoder([](const uint8_t* b, size_t n) { std::vector<uint8_t> o(n / 8); for (size_t i = 0; i < o.size(); i++) o[i] = b[i * 8]; return o; });
            vs.setPcmOut([&pcmN](const int16_t*, size_t n, int) { pcmN += n; });
        };
        {   TetraVoiceSink vs; size_t pcmN = 0; setup(vs, pcmN, true);
            CallEncryption e; e.known = true;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 54 * 4 && vs.framesDecoded() == 1, "clear burst gives PCM through the codec sink (%zu)", pcmN);
            e.encMode = 1;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 54 * 4 && vs.framesDecoded() == 1, "encrypted burst gives no further PCM");
            vs.onMissingBurst();
            CHECK(pcmN == 54 * 4 + 480 && vs.framesDecoded() == 1, "a missing burst gives 60 ms of silence at the codec's rate (%zu)", pcmN);
            vs.setFrameDecoder([](const uint8_t*, size_t) { return std::vector<uint8_t>(); });   // the channel decoding fails (CRC)
            e.encMode = 0;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 54 * 4 + 960 && vs.framesDecoded() == 1, "a burst that does not decode gives 60 ms of silence too (%zu)", pcmN);
        }
        {   TetraVoiceSink vs; size_t pcmN = 0; setup(vs, pcmN, false);
            CallEncryption e; e.known = true;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 0 && vs.status().find("not installed") != std::string::npos, "no codec: no audio, not-installed text");
        }
        {   TetraVoiceSink vs; vs.setCodec(std::make_unique<Silent>()); size_t pcmN = 0;
            vs.setPcmOut([&pcmN](const int16_t*, size_t n, int) { pcmN += n; });
            CallEncryption e; e.known = true;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 0, "no traffic channel decoder: nothing reaches the codec");
        }
    }
    {   // the first capture of a call gives only the tail of the speech: its audio is held until the capture is decoded, then played after as
        // much silence as the capture lacked plus the cushion, so the sound does not run dry before the next capture; while playing, straight through
        const int rate = 48000, fr = rate * 3 / 100;   // a 30 ms frame
        CaptureLeadIn li(rate, 2, 2.3);
        std::vector<float> out, frame((size_t)fr * 2, 0.5f);
        li.push(frame.data(), (size_t)fr, true, out);
        CHECK(out.empty() && li.holding(), "idle output: the first frame is held");
        for (int i = 1; i < 40; i++) li.push(frame.data(), (size_t)fr, true, out);   // 1.2 s in all
        CHECK(out.empty() && std::fabs(li.heldSec() - 1.2) < 1e-6, "held %.2f s", li.heldSec());
        li.captureEnd(out);
        const size_t lead = (size_t)((2.3 - 1.2 + CaptureLeadIn::kCushionSec) * rate);
        CHECK(!li.holding() && out.size() == 2 * (lead + 40 * (size_t)fr), "capture end: %.2f s of silence then the audio (%zu samples)", (double)lead / rate, out.size());
        CHECK(out.size() > 2 * lead && out[0] == 0.f && out[2 * lead - 1] == 0.f && out[2 * lead] == 0.5f, "silence first, the audio after it");
        out.clear(); li.push(frame.data(), (size_t)fr, false, out);
        CHECK(out.size() == frame.size() && !li.holding(), "playing: straight through");
        out.clear(); li.captureEnd(out);
        CHECK(out.empty(), "nothing held: nothing to write");
        CaptureLeadIn full(rate, 2, 2.3);
        for (int i = 0; i < 80; i++) full.push(frame.data(), (size_t)fr, true, out);   // 2.4 s: more than a capture
        out.clear(); full.captureEnd(out);
        CHECK(out.size() == 2 * ((size_t)(CaptureLeadIn::kCushionSec * rate) + 80 * (size_t)fr), "a whole capture from idle: the cushion only (%zu)", out.size());
        // silence at the head of the held audio (a capture lost before this one, filled with silence) only delays the start: as much goes
        // as keeps a capture's worth plus the cushion in front, the rest is dropped
        CaptureLeadIn filled(rate, 2, 2.3);
        const std::vector<float> zero((size_t)fr * 2, 0.f);
        for (int i = 0; i < 100; i++) filled.push(zero.data(), (size_t)fr, true, out);    // 3.0 s of silence
        for (int i = 0; i < 40; i++) filled.push(frame.data(), (size_t)fr, true, out);    // then 1.2 s of speech: 4.2 s held
        out.clear(); filled.captureEnd(out);
        const size_t want = (size_t)(2.3 * rate) + (size_t)(CaptureLeadIn::kCushionSec * rate);
        CHECK(out.size() >= 2 * (want - 2) && out.size() <= 2 * (want + 2) && out[out.size() - 1] == 0.5f, "3 s of silence then 1.2 s of speech: 1.1 s of silence, the cushion and the speech (%zu of %zu)", out.size() / 2, want);
        CaptureLeadIn pause(rate, 2, 2.3);
        for (int i = 0; i < 30; i++) pause.push(zero.data(), (size_t)fr, true, out);     // 0.9 s of silence (a pause before the first burst)
        for (int i = 0; i < 40; i++) pause.push(frame.data(), (size_t)fr, true, out);    // 1.2 s of speech: 2.1 s held, less than a capture
        out.clear(); pause.captureEnd(out);
        CHECK(out.size() == 2 * ((size_t)((2.3 - 2.1 + CaptureLeadIn::kCushionSec) * rate + 0.5) + 70 * (size_t)fr) || out.size() == 2 * ((size_t)((2.3 - 2.1 + CaptureLeadIn::kCushionSec) * rate) + 70 * (size_t)fr),
              "a pause before the first burst inside a capture is kept (%zu)", out.size() / 2);
    }
    {   // one voice stream: a group call heard on three sites in one capture reaches the codec once
        VoiceStreamPicker p; int tn = 0;
        p.newCapture();
        CHECK(p.offer(100, 10, tn) && tn == -1, "nothing followed: the first carrier may give voice");
        p.heard(100, 2, 10);
        CHECK(!p.offer(101, 10, tn) && !p.offer(102, 10, tn), "the other copies of the call in the same capture stay out");
        p.newCapture();
        CHECK(!p.offer(101, 13, tn), "next capture: another carrier stays out while the call is followed");
        CHECK(p.offer(100, 13, tn) && tn == 2, "next capture: the followed carrier, only its timeslot");
        p.heard(100, 2, 13);
        p.newCapture();
        CHECK(p.offer(100, 16, tn) && tn == 2, "still followed");
        p.heard(100, 0, 16);   // the call is over
        CHECK(p.offer(101, 16, tn) && tn == -1, "a quiet followed carrier frees the codec in the same capture");
        p.heard(101, 3, 16);
        p.newCapture();
        CHECK(!p.offer(100, 20, tn) && p.offer(101, 20, tn) && tn == 3, "the new call is followed");
        p.newCapture();
        CHECK(p.offer(100, 20 + VoiceStreamPicker::kHoldSec + 1, tn) && tn == -1, "a followed carrier not heard for kHoldSec is let go");
    }
    {   // decodeCarrier: the timeslot filter, and the encryption gate in front of it (the clear cell of scene 1)
        const double rate = 300e3;
        dect2::SynthConfig sc; sc.mode = 48; sc.snrDb = 30; sc.modeOpt[0] = 1;
        auto syn = dect2::makeTetraSynth(sc, rate);
        std::vector<dect2::cf32> x((size_t)(rate * 2.3));
        syn->generate(x.data(), x.size());
        const dect2::TetraCellCfg cell = dect2::tetraTestCell(1, 1);
        const std::vector<dect2::cf32> s = channelize(x.data(), x.size(), rate, 392.6e6 - dect2::kTetraTestCentreHz);
        const int ts = cell.trafficTs, other = ts == 1 ? 2 : 1;
        const double now = 100;
        // a call set-up for the traffic timeslot of the cell's main carrier, as decodeCarrier would read it; enc: 0 clear, 1 encrypted, -2 none heard
        auto decode = [&](int enc, int voiceTn, Sink& sink) {
            CallTracker calls;
            calls.setMainCarrier(cell.si.mainCarrier);
            if (enc >= -1) { CallEvent e; e.kind = 1; e.callId = 7; e.enc = enc; e.slots = 1 << (4 - ts); e.carrier = cell.si.mainCarrier; calls.onEvent(e, now); }
            return decodeCarrier(s.data(), s.size(), -1, -1, -1, &sink, &calls, now, voiceTn);
        };
        {   Sink k; const CarrierDecode d = decode(0, -1, k);
            CHECK(d.trafficSlots > 0 && k.calls == d.voicePassed && k.calls > 0 && d.voiceTn == ts, "first clear timeslot: %d of %d traffic bursts to the sink, slot %d", k.calls, d.trafficSlots, d.voiceTn);
            CHECK(k.gaps <= 1 && d.voiceGaps == k.gaps, "a slot of speech in every frame: no silence (%d)", k.gaps); }
        {   Sink k; const CarrierDecode d = decode(0, ts, k);
            CHECK(k.calls > 0 && d.voiceTn == ts, "the followed timeslot reaches the sink (%d)", k.calls); }
        {   Sink k; const CarrierDecode d = decode(0, other, k);
            CHECK(k.calls == 0 && d.voiceTn == 0 && d.voicePassed > 0, "another timeslot: clear but kept from the sink (%d passed)", d.voicePassed);
            CHECK(k.gaps >= 30 && k.gaps == d.voiceGaps, "the followed timeslot carries no speech: silence for each of its frames (%d)", k.gaps); }
        {   Sink k; const CarrierDecode d = decode(1, -1, k);
            CHECK(k.calls == 0 && k.gaps == 0 && d.voiceTn == 0 && d.voicePassed == 0 && d.voiceNotDecoded > 0, "encrypted call: nothing reaches the sink"); }
        {   Sink k; const CarrierDecode d = decode(-2, ts, k);
            CHECK(k.calls == 0 && d.voiceTn == 0 && d.voicePassed == 0 && k.gaps >= 30, "no call set-up heard: no speech, silence for the followed slot (%d)", k.gaps); }
        {   // a clear call is never flipped to encrypted by one stray cipher flag; two identical ones are believed; clear is accepted at once
            CallTracker c; c.setMainCarrier(cell.si.mainCarrier);
            auto ev = [&](int callId, int enc) { CallEvent e; e.kind = 1; e.callId = callId; e.enc = enc; e.slots = 1 << (4 - ts); e.carrier = cell.si.mainCarrier; c.onEvent(e, now); };
            ev(7, 0); ev(7, 1);
            CHECK(c.slotState(ts, now).known && c.slotState(ts, now).encMode == 0, "one stray encrypted flag on a clear call: still clear");
            ev(7, 0); ev(7, 1); ev(7, 0); ev(7, 1);
            CHECK(c.slotState(ts, now).encMode == 0, "non-consecutive encrypted flags: still clear");
            ev(7, 1); ev(7, 1);
            CHECK(c.slotState(ts, now).encMode == 1, "two consecutive encrypted flags: encrypted");
            ev(7, 0);
            CHECK(c.slotState(ts, now).known && c.slotState(ts, now).encMode == 0, "a clear set-up is accepted at once");
            ev(8, 1);
            CHECK(!c.slotState(ts, now).known, "a new call that starts with one encrypted flag stays unknown (not decoded)");
        }
        {   // the timeline across captures: the frames of the followed slot between the last capture and this one come first, as silence
            Sink k0; VoiceTimeline t0;
            CallTracker calls; calls.setMainCarrier(cell.si.mainCarrier);
            CallEvent e; e.kind = 1; e.callId = 7; e.enc = 0; e.slots = 1 << (4 - ts); e.carrier = cell.si.mainCarrier; calls.onEvent(e, now);
            const CarrierDecode d0 = decodeCarrier(s.data(), s.size(), -1, -1, -1, &k0, &calls, now, ts, &t0);
            const int n0 = k0.calls + k0.gaps;
            CHECK(t0.tn == ts && t0.next >= 0 && n0 > 30, "the timeline follows the slot: next frame %ld after %d frames", t0.next, n0);
            const long first = (t0.next - n0 + VoiceTimeline::kSpeechFrames) % VoiceTimeline::kSpeechFrames;   // the capture's first frame of the slot
            Sink k1; VoiceTimeline t1; t1.tn = ts; t1.next = (first - 7 + VoiceTimeline::kSpeechFrames) % VoiceTimeline::kSpeechFrames;
            decodeCarrier(s.data(), s.size(), -1, -1, -1, &k1, &calls, now, ts, &t1);
            CHECK(k1.calls == k0.calls && k1.gaps == k0.gaps + 7, "7 frames lost before the capture: 7 silences first (%d, %d)", k1.calls, k1.gaps);
            Sink k2; VoiceTimeline t2; t2.tn = ts; t2.next = (first - 500 + VoiceTimeline::kSpeechFrames) % VoiceTimeline::kSpeechFrames;
            decodeCarrier(s.data(), s.size(), -1, -1, -1, &k2, &calls, now, ts, &t2);
            CHECK(k2.gaps == k0.gaps, "a stream left long ago: no filling (%d)", k2.gaps);
            Sink k3; VoiceTimeline t3; t3.tn = other; t3.next = t1.next;
            decodeCarrier(s.data(), s.size(), -1, -1, -1, &k3, &calls, now, ts, &t3);
            CHECK(k3.gaps == k0.gaps && t3.tn == ts, "another timeslot followed before: a new stream (%d)", k3.gaps);
        }
    }
    {   // decodeCarrier: a capture with no MAC-RESOURCE at all (frames 2 to 18 of the clear cell: null PDUs, SYSINFO and the traffic slot, as a
        // capture taken during a talk spurt looks on the air) still gives the voice of a call whose clear set-up was read from an earlier capture;
        // an encrypted PDU of the capture still refuses it, and so does a set-up never heard
        const dect2::TetraCellCfg cell = dect2::tetraTestCell(1, 1);
        const int ts = cell.trafficTs;
        const double now = 100;
        auto capture = [&](bool encryptedSignalling, bool setupHeard, Sink& sink, int& resources) {
            dect2::TetraCellCfg c = cell; c.encryptedSignalling = encryptedSignalling;
            Modulator mod; std::vector<dect2::cf32> x;
            const uint32_t a0 = 33u * 72u + 4u;   // multiframe 34, frame 2, timeslot 1: frame 1 (the only MAC-RESOURCE of the cell) is left out
            for (uint32_t a = 0; a < 17u * 4u; a++) mod.push(cellSlot(c, a0 + a), x);
            CallTracker calls; calls.setMainCarrier(c.si.mainCarrier);
            if (setupHeard) { CallEvent e; e.kind = 1; e.callId = 7; e.enc = 0; e.slots = 1 << (4 - ts); e.carrier = c.si.mainCarrier; calls.onEvent(e, now - 5); }
            const CarrierDecode d = decodeCarrier(x.data(), x.size(), -1, -1, -1, &sink, &calls, now, -1);
            resources = 0; for (const auto& m : d.macs) resources += m.resources;
            return d;
        };
        {   Sink k; int res = 0; const CarrierDecode d = capture(false, true, k, res);
            CHECK(d.synced && d.trafficSlots > 0 && res == 0, "the capture has traffic (%d) and no MAC-RESOURCE (%d)", d.trafficSlots, res);
            CHECK(k.calls > 0 && k.calls == d.voicePassed && d.voiceTn == ts, "no MAC-RESOURCE in the capture, call known clear: voice (%d of %d, slot %d, %s)", k.calls, d.trafficSlots, d.voiceTn, d.voiceStatus); }
        {   Sink k; int res = 0; const CarrierDecode d = capture(true, true, k, res);
            CHECK(res > 0 && k.calls == 0 && d.voicePassed == 0 && d.voiceNotDecoded > 0, "an encrypted PDU in the capture: nothing reaches the sink (%s)", d.voiceStatus); }
        {   Sink k; int res = 0; const CarrierDecode d = capture(false, false, k, res);
            CHECK(k.calls == 0 && d.voicePassed == 0 && d.voiceNotDecoded > 0, "no set-up heard: nothing reaches the sink"); }
    }
    printf(fails ? "tetra voice: %d FAILED\n" : "tetra voice: all passed\n", fails);
    return fails ? 1 : 0;
}
