// TETRAPOL voice plumbing on the traffic-channel test signal: the encryption gate (clear / encrypted / no key reference), the 16-byte plug-in
// frame, listening off, and the whole chain through a real plug-in file (tests/codec/tone_codec_plugin.c, a fake codec that returns a
// tone) into the PCM output and the per-call WAV. There is no TETRAPOL codec here and none is needed.
#include "dect2/codec/codec_registry.h"
#include "dect2/codec/speech_codec.h"
#include "dect2/tetrapol/tetrapol_gen.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/tetrapol/tetrapol_voice.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>
using namespace dect2;
namespace tp = dect2::tetrapol;
#include "../common/test_util.h"

namespace {

// an in-process stand-in codec: checks each frame against the test pattern and answers with 160 samples
struct Seen { int frames = 0, badSize = 0, damaged = 0, matched = 0; };
class FakeCodec final : public codec::SpeechCodec {
public:
    explicit FakeCodec(Seen* s) : s_(s) {}
    int sampleRate() const override { return 8000; }
    std::vector<int16_t> decode(const std::vector<uint8_t>& c) override {
        s_->frames++;
        if (c.size() != tp::kPluginFrameBytes) { s_->badSize++; return {}; }
        if (c[15] & 1) s_->damaged++;
        for (uint32_t n = 0; n < (uint32_t)kTetrapolTestVoiceFrames; n++) {
            uint8_t want[tp::kVoiceBits];
            tp::knownVoiceBits(n, want);
            tp::VoiceFrame v;
            memcpy(v.codec, want, sizeof want);
            uint8_t fb[tp::kPluginFrameBytes];
            tp::packPluginFrame(v, false, fb);
            if (!memcmp(fb, c.data(), 15)) { s_->matched++; break; }
        }
        return std::vector<int16_t>(160, 100);
    }
private:
    Seen* s_;
};

struct Run { TetrapolTelemetry t; size_t pcm = 0; };

Run run(int scene, int pol, int band, double secs, std::unique_ptr<codec::SpeechCodec> c, bool listen = true, const std::string& rec = "",
        double cfo = 0, double snr = 30) {
    SynthConfig sc;
    sc.mode = 81; sc.snrDb = snr; sc.cfoHz = cfo;
    sc.modeOpt[1] = scene;
    sc.modeOpt[2] = pol;
    sc.modeOpt[3] = band;
    const double rate = 250000;
    auto syn = makeTetrapolSynth(sc, rate);
    TetrapolReceiver rx;
    rx.configure(rate);
    rx.reset();
    rx.setCodec(std::move(c));
    rx.setListen(listen);
    rx.setRecordFolder(rec);
    Run r;
    rx.setPcmOut([&r](const int16_t*, size_t n, int rate) { if (rate == 8000) r.pcm += n; });
    std::vector<cf32> buf(12500);
    for (double done = 0; done < secs; done += (double)buf.size() / rate) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    rx.setRecordFolder("");   // closes a recording still open
    rx.telemetry(r.t, 0);
    return r;
}

void testFrameLayout() {
    tp::VoiceFrame v;
    v.codec[0] = 1; v.codec[7] = 1; v.codec[8] = 1; v.codec[119] = 1;
    uint8_t b[tp::kPluginFrameBytes];
    tp::packPluginFrame(v, true, b);
    CHECK(b[0] == 0x81 && b[1] == 0x80 && b[14] == 0x01 && b[15] == 0x01, "plug-in frame layout %02x %02x %02x %02x", b[0], b[1], b[14], b[15]);
    CHECK(codec::pluginFrameBytes("tetrapol") == tp::kPluginFrameBytes, "registry frame size");
    const codec::CodecInfo* ci = codec::findCodec("tetrapol");
    CHECK(ci && ci->kind == codec::CodecKind::Patented, "codec table entry");
    CHECK(codec::speechCodecFileName("tetrapol").find("onair-codec-tetrapol") == 0, "plug-in file name");
}

void testGate() {
    tp::CallGate g;
    bool st = false;
    tp::CallSignal s;
    s.kind = tp::CallSignal::CallStart; s.keyRef = 0x00;
    g.onSignal(s, 0.0);
    CHECK(g.onVoice(0.1, st) == tp::Verdict::Clear && st, "clear call");
    s.keyRef = 0x12;
    g.onSignal(s, 0.5);
    CHECK(g.onVoice(0.6, st) == tp::Verdict::Encrypted && !st, "encrypted is sticky inside a call");
    s.keyRef = 0x00;
    g.onSignal(s, 0.7);
    CHECK(g.onVoice(0.8, st) == tp::Verdict::Encrypted, "a later clear message does not undo encrypted");
    CHECK(g.tick(2.0) && !g.active(), "call ends after a quiet second");
    CHECK(g.onVoice(2.1, st) == tp::Verdict::Unknown && st, "no key reference: unknown");
    tp::CallSignal e; e.kind = tp::CallSignal::CallEnd;
    g.onSignal(e, 2.2);
    CHECK(g.tick(2.2) && !g.active(), "D_CALL_END ends the call");
    s.keyRef = 0x00;
    g.onSignal(s, 2.3);
    CHECK(g.onVoice(7.0, st) == tp::Verdict::Unknown, "a D_CALL_START seconds before does not count");
    tp::CallSignal ga; ga.kind = tp::CallSignal::GroupActivation; ga.keyRef = 0x00;
    tp::CallGate g2;
    g2.onSignal(ga, 0.0);
    CHECK(g2.onVoice(0.1, st) == tp::Verdict::Unknown, "a group activation (for another channel) does not open the gate");
}

} // namespace

int main() {
    testFrameLayout();
    testGate();

    // clear call: every voice frame reaches the codec, in order, with the pattern of the test signal; both polarities and band codings
    for (int pol = 0; pol < 2; pol++)
        for (int band = 0; band < 2; band++) {
            Seen s;
            // 8.5 s: the first call may start before the frame sync (its D_CALL_START unheard, so refused); the second one is whole
            const Run r = run(1, pol, band, 8.5, std::make_unique<FakeCodec>(&s), true, "", pol ? -700 : 450);
            printf("  clear pol %d band %d: %s; codec %d matched %d damaged %d pcm %zu calls %zu\n", pol, band, tetrapolSummary(r.t).c_str(),
                   s.frames, s.matched, s.damaged, r.pcm, r.t.calls.size());
            CHECK(s.badSize == 0, "bad plug-in frame size");
            CHECK(s.frames >= kTetrapolTestVoiceFrames && s.matched >= s.frames - 2, "clear call: %d frames, %d match the pattern", s.frames, s.matched);
            CHECK(r.pcm == (size_t)s.frames * 160, "PCM %zu for %d frames", r.pcm, s.frames);
            const uint64_t lateRefused = r.t.calls.size() > 1 && r.t.calls.back().verdict == 0 ? r.t.calls.back().voiceFrames : 0;
            CHECK(r.t.voiceRefused <= lateRefused && r.t.calls.size() >= 2 && r.t.calls[r.t.calls.size() - 2].verdict == 1, "clear verdict");
        }
    // encrypted call and a call without key reference: nothing reaches the codec
    for (int scene : {2, 3}) {
        Seen s;
        const Run r = run(scene, 1, 0, 4.5, std::make_unique<FakeCodec>(&s));
        printf("  scene %d: %s; codec %d refused %llu\n", scene, tetrapolSummary(r.t).c_str(), s.frames, (unsigned long long)r.t.voiceRefused);
        CHECK(s.frames == 0 && r.pcm == 0, "scene %d: %d frames reached the codec", scene, s.frames);
        CHECK(r.t.voiceRefused >= (uint64_t)kTetrapolTestVoiceFrames - 20, "scene %d: refused %llu", scene, (unsigned long long)r.t.voiceRefused);
        CHECK(!r.t.calls.empty() && r.t.calls.front().verdict == (scene == 2 ? 2 : 0), "scene %d verdict", scene);
        for (const auto& e : r.t.events) CHECK(e.text.find("call") == std::string::npos || e.text.find("1234") == std::string::npos, "an id in the log");
    }
    // listening off: the clear call is not decoded
    {
        Seen s;
        const Run r = run(1, 0, 0, 4.5, std::make_unique<FakeCodec>(&s), false);
        CHECK(s.frames == 0 && r.pcm == 0, "listen off: %d frames decoded", s.frames);
    }
    // no codec: counted, no sound
    {
        const Run r = run(1, 0, 0, 4.5, nullptr);
        CHECK(r.t.voiceFrames >= (uint64_t)kTetrapolTestVoiceFrames - 10 && r.pcm == 0 && r.t.voiceDecoded == 0, "no codec");
    }
#ifdef TONE_PLUGIN_PATH
    // the real plug-in loader with the fake tone plug-in, and the per-call WAV
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "onair-test-tetrapol-voice";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "rec", ec);
        fs::copy_file(TONE_PLUGIN_PATH, dir / codec::speechCodecFileName("tetrapol"), ec);
        CHECK(!ec, "copy plug-in: %s", ec.message().c_str());
        std::unique_ptr<codec::SpeechCodec> c;
        codec::CodecLookup l = codec::loadSpeechCodec("tetrapol", dir.string(), false, c);
        CHECK(l.status == codec::CodecStatus::NoticeRequired && !c, "notice first");
        l = codec::loadSpeechCodec("tetrapol", dir.string(), true, c);
        CHECK(l.status == codec::CodecStatus::Ready && c, "plug-in loads: %s", l.message.c_str());
        // 8.5 s as the clear-call runs above: the first call starts before the frame sync and is refused (no WAV), the second is whole
        const Run r = run(1, 1, 0, 8.5, std::move(c), true, (dir / "rec").string());
        // 8.5 s hold the refused first call, the whole second call and the start of the third (cut by the end of the run, a short WAV of its own):
        // exactly one WAV holds a whole call, and the tone is in it
        const uintmax_t whole = 44 + 2 * 160 * (uintmax_t)(kTetrapolTestVoiceFrames - 10);
        size_t wavs = 0, wholeWavs = 0;
        uintmax_t bytes = 0;
        std::vector<int16_t> head;
        for (const auto& e : fs::directory_iterator(dir / "rec", ec)) {
            if (e.path().extension() != ".wav") continue;
            wavs++;
            const uintmax_t sz = e.file_size(ec);
            if (sz < whole) continue;
            wholeWavs++;
            bytes = sz;
            FILE* f = fopen(e.path().string().c_str(), "rb");
            if (f) {
                uint8_t h[44 + 32];
                head.clear();
                if (fread(h, 1, sizeof h, f) == sizeof h) for (int i = 0; i < 16; i++) head.push_back((int16_t)(h[44 + 2 * i] | (h[45 + 2 * i] << 8)));
                fclose(f);
            }
        }
        printf("  tone plug-in: %zu wav, %zu whole, %ju bytes, decoded %llu\n", wavs, wholeWavs, bytes, (unsigned long long)r.t.voiceDecoded);
        CHECK(wholeWavs == 1 && wavs <= 2, "one WAV of the whole call (%zu files, %zu whole)", wavs, wholeWavs);
        CHECK(head.size() == 16 && head[2] == 8000 && head[6] == -8000, "the tone is in the WAV");
        for (const auto& e : fs::directory_iterator(dir / "rec", ec)) CHECK(e.path().filename().string().find("1234") == std::string::npos, "an id in a file name");
        fs::remove_all(dir, ec);
    }
#else
    printf("  (tone plug-in test skipped: TONE_PLUGIN_PATH not set)\n");
#endif
    return testSummary("tetrapol_voice");
}
