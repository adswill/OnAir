// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 Phase 2 voice through a user-installed codec plug-in "ambe2" (stub library, tests/codec/stub_codec_plugin.c, no codec): every clear voice frame of the
// listened slot reaches the plug-in as 12 bytes and comes out as 160 samples; the other slot is not decoded; an encrypted call (or one whose encryption
// is not known yet) never reaches the plug-in. NOT covered: a real AMBE+2 codec, a real P25 Phase 2 recording.
#include "dect2/codec/codec_registry.h"
#include "dect2/p25p2/p25p2_gen.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace fs = std::filesystem;

static std::mutex gMu;
static std::map<size_t, int> gSizes;   // frame size -> number of frames seen by the plug-in

class Counting final : public codec::SpeechCodec {
public:
    explicit Counting(std::unique_ptr<codec::SpeechCodec> in) : in_(std::move(in)) {}
    int sampleRate() const override { return in_->sampleRate(); }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override { { std::lock_guard<std::mutex> lk(gMu); gSizes[coded.size()]++; } return in_->decode(coded); }
private:
    std::unique_ptr<codec::SpeechCodec> in_;
};

struct Out { int clear[3] = {0, 0, 0}, held[3] = {0, 0, 0}; size_t audio = 0; int plug = 0, plugOther = 0; P25p2Telemetry t; };

static Out run(const P25p2GenConfig& c, int listen) {
    { std::lock_guard<std::mutex> lk(gMu); gSizes.clear(); }
    Out o;
    P25p2Receiver rx;
    rx.setSilent(true);
    rx.setListenSlot(listen);
    rx.configure(c.rate);
    rx.setVoiceTap([&](int slot, const uint8_t*, bool clear) { (clear ? o.clear : o.held)[slot]++; });
    rx.setAudioTap([&](const float*, size_t n) { o.audio += n; });
    const std::vector<cf32> s = p25p2Signal(c);
    for (size_t i = 0; i < s.size(); i += 20000) rx.feed(s.data() + i, std::min<size_t>(20000, s.size() - i));
    rx.telemetry(o.t, 0);
    std::lock_guard<std::mutex> lk(gMu);
    o.plug = gSizes[12];
    for (const auto& kv : gSizes) if (kv.first != 12) o.plugOther += kv.second;
    return o;
}

int main() {
#ifdef _WIN32
    _putenv_s("DECT2_MBELIB_PATH", "none");
#else
    setenv("DECT2_MBELIB_PATH", "none", 1);
#endif
    const fs::path dir = fs::temp_directory_path() / "onair-test-p25p2-plugin";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string folder = dir.string();
    fs::copy_file(STUB_PLUGIN_PATH, dir / codec::speechCodecFileName("ambe2"), ec);
    CHECK(!ec, "copy of the stub failed");
    codec::setPluginFactory("ambe2", [folder]() -> std::unique_ptr<codec::SpeechCodec> {
        std::unique_ptr<codec::SpeechCodec> c;
        codec::loadSpeechCodec("ambe2", folder, true, c);
        return c ? std::make_unique<Counting>(std::move(c)) : nullptr;
    }, folder + "/ambe2");

    P25p2GenConfig c;
    c.rate = 240000; c.snrDb = 40; c.superframes = 14;
    P25p2GenCall a; a.channel = 1; a.startSf = 1; a.nSf = 6; a.src = 1111; a.group = 101;
    P25p2GenCall b; b.channel = 2; b.startSf = 1; b.nSf = 6; b.src = 2222; b.group = 202;
    c.calls = {a, b};
    const int f1 = p25p2VoiceFrames(c, 1), f2 = p25p2VoiceFrames(c, 2);

    for (int listen = 1; listen <= 2; listen++) {   // both calls clear: only the chosen slot reaches the plug-in
        const Out o = run(c, listen);
        const int want = listen == 1 ? f1 : f2;
        printf("listen slot %d: plug-in frames %d (slot frames %d), audio %zu samples\n", listen, o.plug, want, o.audio);
        CHECK(o.t.codec, "listen %d: the plug-in is not seen as the AMBE+2 decoder", listen);
        CHECK(o.plug >= want * 95 / 100 && o.plug <= want, "listen %d: %d plug-in frames for %d", listen, o.plug, want);
        CHECK(o.plugOther == 0, "listen %d: frames of another size reached the plug-in", listen);
        CHECK(o.audio == (size_t)o.plug * 160, "listen %d: audio %zu samples for %d frames", listen, o.audio, o.plug);
        CHECK(o.clear[1] >= f1 * 95 / 100 && o.clear[2] >= f2 * 95 / 100, "listen %d: the other slot is not even seen (%d, %d)", listen, o.clear[1], o.clear[2]);
    }
    {   // first heard: the slot that comes up first keeps the sound while its call lasts
        P25p2GenConfig d = c;
        d.calls[1].startSf = 3; d.calls[1].nSf = 3;
        const Out o = run(d, 0);
        const int w1 = p25p2VoiceFrames(d, 1);
        printf("first heard: plug-in frames %d (slot 1 has %d)\n", o.plug, w1);
        CHECK(o.plug >= w1 * 95 / 100 && o.plug <= w1, "first heard: %d plug-in frames for slot 1 with %d", o.plug, w1);
    }
    {   // an encrypted call chosen as the slot to listen to: nothing reaches the plug-in
        P25p2GenConfig d = c;
        d.calls[1].algId = 0x84;
        const Out o = run(d, 2);
        printf("encrypted slot 2: plug-in frames %d, held %d\n", o.plug, o.held[2]);
        CHECK(o.plug == 0 && o.audio == 0, "encrypted call: %d frames reached the plug-in", o.plug);
        CHECK(o.clear[2] == 0 && o.held[2] >= p25p2VoiceFrames(d, 2) * 95 / 100, "encrypted call: %d clear, %d held", o.clear[2], o.held[2]);
        CHECK(o.t.encryptedSkipped > 0, "encrypted call not counted");
    }
    {   // the encryption is flagged only in the service options of the voice channel user message (the PTT said clear): from then on no voice is decoded
        P25p2GenConfig d = c;
        d.calls[1].svcOpts = 0x40;
        const Out o = run(d, 2);
        printf("encrypted by service options: plug-in frames %d\n", o.plug);
        CHECK(o.plug < p25p2VoiceFrames(d, 2) / 2, "service-option encryption: %d of %d frames reached the plug-in", o.plug, p25p2VoiceFrames(d, 2));
    }
    return testSummary("test_p25p2_plugin");
}
