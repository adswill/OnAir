// Iridium voice calls (core/include/dect2/iridium/iridium_voice.h) with synthetic voice slots from our own generator (iridiumBuildVoice) and the
// stub codec plug-in (tests/codec/stub_codec_plugin.c, no codec). Proves the chain we can prove without a real recording:
//  1. the 39 voice bytes come out of a burst exactly as built (frame layer, LCW frame type 0);
//  2. no plug-in: calls are listed ("codec needed"), nothing reaches a codec, no sound;
//  3. with the plug-in: every readable frame reaches it as 39 bytes, the sound is 160 samples (8 kHz) -> 960 at 48 kHz per frame, only for the
//     listened call; two simultaneous calls are kept apart (frequency, direction); a gap of more than a second starts a new call;
//  4. a fill-pattern field is "not readable" and silent; Listen off is silent but still decodes and records;
//  5. satellite and beam come from a broadcast frame on the same frequency just before; each ended call is saved as a WAV of the right size.
// NOT covered: a real Iridium recording, the real vocoder, the real FEC / interleaving of the voice field (unverified, see the header).
#include "dect2/iridium/iridium_voice.h"
#include "dect2/voice/clip_history.h"
#include "../codec/plugin_test_util.h"
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static IridiumBurstBits burst(const std::vector<uint8_t>& bits, double t, double f, bool dl = true) {
    IridiumBurstBits b;
    b.bits = bits; b.downlink = dl; b.timeSec = t; b.freqHz = f;
    return b;
}

static std::vector<uint8_t> payloadOf(uint32_t seed) {
    std::mt19937 g(seed);
    std::vector<uint8_t> p(kIridiumVoiceBytes);
    for (auto& x : p) x = uint8_t(g());
    p[0] |= 1;   // never a fill pattern
    return p;
}

static void feedCall(IridiumVoice& v, int frames, double t0, double f, bool dl, uint32_t seed) {
    for (int i = 0; i < frames; i++) {
        const IridiumBurstBits b = burst(iridiumBuildVoice(payloadOf(seed + uint32_t(i))), t0 + 0.09 * i, f + 3.0 * i, dl);
        v.feed(b, decodeIridiumBurst(b));
    }
}

int main() {
    // 1. the field comes out as built
    {
        const std::vector<uint8_t> p = payloadOf(7);
        const IridiumBurstBits b = burst(iridiumBuildVoice(p, 0x15, 0x1234), 0, 1.6e9);
        std::vector<uint8_t> q;
        CHECK(iridiumVoicePayload(b, q) && q == p, "voice field differs from the built one");
        const IridiumFrame f = decodeIridiumBurst(b);
        CHECK(f.type == IridiumType::Voice && f.ok && f.lcwFt == 0, "burst is not a good voice frame (type %s ok %d)", f.typeName.c_str(), f.ok);
        std::vector<uint8_t> shortBits(b.bits.begin(), b.bits.begin() + 200);
        IridiumBurstBits s = b; s.bits = shortBits;
        CHECK(!iridiumVoicePayload(s, q), "a short burst gave a voice field");
    }
    namespace fs = std::filesystem;
    const fs::path rec = fs::temp_directory_path() / "onair-test-iridium-voice";
    std::error_code ec;
    fs::remove_all(rec, ec);
    fs::create_directories(rec, ec);

    // 2. no plug-in
    {
        IridiumVoice v;
        v.setSilent(true);
        size_t tapped = 0;
        v.setAudioTap([&](int, const float*, size_t n) { tapped += n; });
        feedCall(v, 10, 1.0, 1.6200e9, true, 100);
        v.closeAll();
        const auto c = v.calls();
        CHECK(c.size() == 1 && c[0].frames == 10 && c[0].readable == 10, "calls without plug-in: %zu", c.size());
        CHECK(!c.empty() && c[0].status == "codec needed" && c[0].decoded == 0, "status without plug-in: %s", c.empty() ? "-" : c[0].status.c_str());
        CHECK(tapped == 0 && v.framesToCodec() == 0, "sound or codec frames without a plug-in");
    }

    // 3-5. with the plug-in
    CHECK(plugtest::install("iridium", STUB_PLUGIN_PATH), "stub install");
    {
        IridiumVoice v;
        v.setSilent(true);
        v.setRecordFolder(rec.string());
        std::map<int, size_t> tapped;
        v.setAudioTap([&](int id, const float*, size_t n) { tapped[id] += n; });
        // a broadcast frame names satellite 17, beam 9 on the first call's channel
        const IridiumBurstBits ibc = burst(iridiumBuildIbc(17, 9, 1.7e9), 0.9, 1.6200e9 + 500);
        const IridiumFrame fi = decodeIridiumBurst(ibc);
        CHECK(fi.ok && fi.satId == 17, "IBC test frame not decoded");
        v.feed(ibc, fi);
        // two calls at once (different frequency and direction), interleaved in time
        for (int i = 0; i < 12; i++) {
            const IridiumBurstBits a = burst(iridiumBuildVoice(payloadOf(200 + uint32_t(i))), 1.0 + 0.09 * i, 1.6200e9 + 2.0 * i, true);
            const IridiumBurstBits b = burst(iridiumBuildVoice(payloadOf(300 + uint32_t(i))), 1.02 + 0.09 * i, 1.6263e9, false);
            v.feed(a, decodeIridiumBurst(a));
            v.feed(b, decodeIridiumBurst(b));
        }
        // a fill pattern field is not readable
        for (int i = 0; i < 3; i++) {
            const IridiumBurstBits u = burst(iridiumBuildVoice(std::vector<uint8_t>(kIridiumVoiceBytes, 0xaa)), 2.2 + 0.09 * i, 1.6200e9 + 30);
            v.feed(u, decodeIridiumBurst(u));
        }
        // after a gap of more than a second: a new call on the first channel
        feedCall(v, 5, 4.0, 1.6200e9, true, 400);
        v.closeAll();
        const auto c = v.calls();
        CHECK(c.size() == 3, "expected 3 calls, got %zu", c.size());
        if (c.size() == 3) {
            CHECK(c[0].downlink && c[0].frames == 15 && c[0].readable == 12 && c[0].decoded == 12, "call 1: %u frames %u readable %u decoded", c[0].frames, c[0].readable, c[0].decoded);
            CHECK(!c[1].downlink && c[1].frames == 12 && c[1].decoded == 12, "call 2 (uplink): %u frames %u decoded", c[1].frames, c[1].decoded);
            CHECK(c[2].downlink && c[2].frames == 5 && c[2].id == 3, "call 3 after the gap: %u frames", c[2].frames);
            CHECK(c[0].sat == 17 && c[0].beam == 9, "call 1 satellite %d beam %d", c[0].sat, c[0].beam);
            CHECK(c[1].sat == -1, "call 2 got a satellite from another frequency");
            CHECK(c[0].status == "decoded", "status %s", c[0].status.c_str());
            // Listen = first call with sound: call 1 only
            CHECK(tapped[1] == 12 * 960 && tapped.count(2) == 0, "listened sound: call 1 %zu samples, call 2 %zu", tapped[1], tapped[2]);
            CHECK(fs::exists(c[0].file) && fs::file_size(c[0].file) == 44 + 12 * 160 * 2, "recording of call 1: %s", c[0].file.c_str());
            CHECK(fs::exists(c[1].file) && fs::file_size(c[1].file) == 44 + 12 * 160 * 2, "recording of call 2 missing");
            // the WAV holds real sound (not zeros), at the codec's rate
            std::vector<int16_t> pcm; int rate = 0;
            CHECK(voice::wavReadMono16(c[0].file, pcm, rate) && rate == 8000 && pcm.size() == 12 * 160 && voice::pcmHasSound(pcm), "recording of call 1 is silent or unreadable");
            CHECK(c[0].warning.empty() && c[0].status == "decoded", "warning on a good call: %s", c[0].warning.c_str());
        }
        CHECK(plugtest::gBadSize == 0, "%d frames of the wrong size to the plug-in", plugtest::gBadSize.load());
        CHECK(v.framesToCodec() == 12 + 12 + 5, "frames to codec %llu", (unsigned long long)v.framesToCodec());
    }
    // Listen off and an explicit call
    {
        IridiumVoice v;
        v.setSilent(true);
        std::map<int, size_t> tapped;
        v.setAudioTap([&](int id, const float*, size_t n) { tapped[id] += n; });
        v.setListenCall(-1);
        feedCall(v, 4, 1.0, 1.6200e9, true, 500);
        CHECK(tapped.empty(), "sound with Listen off");
        v.setListenCall(1);
        feedCall(v, 4, 1.4, 1.6200e9, true, 600);
        CHECK(tapped[1] == 4 * 960, "sound after choosing call 1: %zu", tapped[1]);
        CHECK(v.calls().size() == 1 && v.calls()[0].decoded == 8, "Listen off must still decode");
    }
    // lost bursts: a hole of two 90 ms frames is filled with silence so the call keeps its timing; a data slot in the middle is not sent
    {
        IridiumVoice v;
        v.setSilent(true);
        v.setRecordFolder(rec.string());
        const double t[] = {1.0, 1.09, 1.36, 1.45};   // 1.09 -> 1.36 loses two frames
        for (int i = 0; i < 4; i++) {
            const IridiumBurstBits b = burst(iridiumBuildVoice(payloadOf(1300 + uint32_t(i))), t[i], 1.6210e9, true);
            v.feed(b, decodeIridiumBurst(b));
        }
        v.closeAll();
        const auto c = v.calls();
        CHECK(!c.empty() && c.back().gapFrames == 2 && c.back().decoded == 4, "gap fill: %u gap frames, %u decoded", c.empty() ? 0u : c.back().gapFrames, c.empty() ? 0u : c.back().decoded);
        CHECK(!c.empty() && fs::exists(c.back().file) && fs::file_size(c.back().file) == 44 + 6 * 160 * 2, "gap-filled recording size");
    }
    // mixer: solo beats Listen, mute silences but still decodes; silence frames are counted and not sent
    {
        IridiumVoice v;
        v.setSilent(true);
        std::map<int, size_t> tapped;
        v.setAudioTap([&](int id, const float*, size_t n) { tapped[id] += n; });
        v.setListenCall(1);
        feedCall(v, 3, 1.0, 1.6200e9, true, 700);      // call 1
        feedCall(v, 3, 1.0, 1.6263e9, false, 800);     // call 2
        CHECK(tapped[1] == 3 * 960 && tapped.count(2) == 0, "listen call 1 only");
        v.setSolo(2, true);
        feedCall(v, 3, 1.3, 1.6200e9, true, 710);
        feedCall(v, 3, 1.3, 1.6263e9, false, 810);
        CHECK(tapped[1] == 3 * 960 && tapped[2] == 3 * 960, "solo 2 heard alone: %zu %zu", tapped[1], tapped[2]);
        v.setMute(2, true);
        feedCall(v, 3, 1.6, 1.6263e9, false, 820);
        CHECK(tapped[2] == 3 * 960, "muted call still played");
        const auto c = v.calls();
        CHECK(c.size() == 2 && c[1].decoded == 9 && c[1].mute && c[1].solo, "muted call must still decode");
        // silence frames: LCW 001111 / 100000000000000000000
        for (int i = 0; i < 3; i++) {
            const IridiumBurstBits b = burst(iridiumBuildVoice(payloadOf(900 + uint32_t(i)), 15, 0x100000), 2.0 + 0.09 * i, 1.6200e9 + 5, true);
            const IridiumFrame f = decodeIridiumBurst(b);
            CHECK(f.lcw2 == 15 && f.lcw3 == 0x100000, "lcw fields %d %lld", f.lcw2, (long long)f.lcw3);
            v.feed(b, f);
        }
        CHECK(v.calls()[0].silent == 3 && v.calls()[0].decoded == 6, "silence frames: %u silent, %u decoded", v.calls()[0].silent, v.calls()[0].decoded);
    }
    // byte order of the voice field = iridium-toolkit bits_to_dfs.py: first bit received is the least significant bit of the byte
    {
        std::vector<uint8_t> raw(312, 0);
        raw[0] = 1; raw[9] = 1;
        const IridiumBurstBits b = burst(iridiumBuildLcwFrame(0, 0, 0, raw), 0, 1.6e9);
        std::vector<uint8_t> q;
        CHECK(iridiumVoicePayload(b, q) && q[0] == 0x01 && q[1] == 0x02, "bit order: %02x %02x", q.empty() ? 0 : q[0], q.size() < 2 ? 0 : q[1]);
    }
    // a plug-in that answers with zeros: the call is "silent audio", no WAV is written, and the user is told
    {
        struct Zero final : codec::SpeechCodec {
            int sampleRate() const override { return 8000; }
            std::vector<int16_t> decode(const std::vector<uint8_t>&) override { return std::vector<int16_t>(160, 0); }
        };
        codec::setPluginFactory("iridium", [] { return std::unique_ptr<codec::SpeechCodec>(new Zero); }, "test-zero");
        const fs::path rec2 = rec / "zero";
        fs::create_directories(rec2, ec);
        IridiumVoice v;
        v.setSilent(true);
        v.setRecordFolder(rec2.string());
        feedCall(v, 30, 1.0, 1.6200e9, true, 1000);
        v.closeAll();
        const auto c = v.calls();
        CHECK(c.size() == 1 && c[0].decoded == 30, "zero codec: calls %zu", c.size());
        CHECK(!c.empty() && c[0].status == "silent audio" && c[0].file.empty() && !c[0].warning.empty(), "zero codec: status '%s' file '%s' warning '%s'", c.empty() ? "-" : c[0].status.c_str(), c.empty() ? "-" : c[0].file.c_str(), c.empty() ? "-" : c[0].warning.c_str());
        size_t wavs = 0;
        for (auto& e : fs::directory_iterator(rec2)) wavs += e.path().extension() == ".wav";
        CHECK(wavs == 0, "a silent WAV was written (%zu files)", wavs);
        // a codec that is missing: warning says so, still no file
        codec::setPluginFactory("iridium", nullptr, "");
        IridiumVoice w;
        w.setSilent(true);
        w.setRecordFolder(rec2.string());
        feedCall(w, 5, 1.0, 1.6200e9, true, 1100);
        w.closeAll();
        const auto d = w.calls();
        CHECK(d.size() == 1 && d[0].status == "codec needed" && d[0].warning.find("codec") != std::string::npos && d[0].file.empty(), "missing codec: status/warning");
        for (auto& e : fs::directory_iterator(rec2)) wavs += e.path().extension() == ".wav";
        CHECK(wavs == 0, "a WAV was written without a codec");
    }
    fs::remove_all(rec, ec);
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
