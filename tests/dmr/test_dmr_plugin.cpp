// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DMR voice through a user-installed codec plug-in (a stub plug-in library, tests/codec/stub_codec_plugin.c, no codec). Proves the chain:
//  1. the plug-in loader opens the stub from a codecs folder as onair-codec-ambe2.<ext> and decodes a 12 byte frame to 160 samples;
//  2. the code words are packed MSB first (packCodeWords);
//  3. generator -> DmrReceiver -> plug-in -> 8 kHz PCM -> 48 kHz tap: every voice burst of the followed slot gives 480 samples (3 frames), the
//     samples are the stub's pattern, and the telemetry names the plug-in;
//  4. slot selection: listening to slot 1 or slot 2 decodes only that slot;
//  5. encrypted calls (privacy flag in the service options) are never decoded: no frame reaches the plug-in, no sound.
// NOT covered: a real codec, a real DMR recording.
#include "dect2/codec/codec_registry.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::dmr;
#include "../common/test_util.h"

namespace fs = std::filesystem;

static std::atomic<int> gFrames{0};

// counts the frames the receiver sends, then asks the real stub library
class Counting final : public codec::SpeechCodec {
public:
    explicit Counting(std::unique_ptr<codec::SpeechCodec> in) : in_(std::move(in)) {}
    int sampleRate() const override { return in_->sampleRate(); }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override { gFrames++; return in_->decode(coded); }
private:
    std::unique_ptr<codec::SpeechCodec> in_;
};

static std::string gFolder;

static void installStub() {
    const fs::path dir = fs::temp_directory_path() / "onair-test-dmr-plugin";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    gFolder = dir.string();
    fs::copy_file(STUB_PLUGIN_PATH, dir / codec::speechCodecFileName("ambe2"), ec);
    CHECK(!ec, "copy of the stub plug-in failed: %s", ec.message().c_str());
}

static void testLoader() {
    std::unique_ptr<codec::SpeechCodec> c;
    codec::CodecLookup r = codec::loadSpeechCodec("ambe2", gFolder, false, c);
    CHECK(r.status == codec::CodecStatus::NoticeRequired, "notice first");
    r = codec::loadSpeechCodec("ambe2", gFolder, true, c);
    CHECK(r.status == codec::CodecStatus::Ready && c, "stub did not load: %s", r.message.c_str());
    if (!c) return;
    char bits[96] = {};
    bits[0] = 1; bits[7] = 1;                            // 0x81 in the first byte
    const std::vector<uint8_t> f = codec::packCodeWords(bits, 4, 24);
    CHECK(f.size() == 12 && f[0] == 0x81, "packCodeWords: %zu bytes, first %02x", f.size(), f.empty() ? 0 : f[0]);
    const std::vector<int16_t> pcm = c->decode(f);
    CHECK(pcm.size() == 160 && pcm[0] == 1000 + 0x81 && pcm[3] == 3 * 37 + 1, "stub PCM %zu samples", pcm.size());
    CHECK(codec::findCodec("ambe2") && codec::findCodec("ambe2")->kind == codec::CodecKind::Patented, "registry knows ambe2 as patented");
}

struct Run { uint64_t decoded = 0, skipped = 0; int audioSlot = 0; size_t tapped = 0, nz = 0; uint64_t bursts[3] = {}; std::string path; int frames = 0; };

static Run run(int listen, bool encrypted) {
    gFrames = 0;
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(2.4e6);
    rx.setListenSlot(listen);
    Run o;
    rx.setAudioTap([&](const float* l, const float*, size_t n) { o.tapped += n; for (size_t i = 0; i < n; i++) o.nz += l[i] != 0.f; });
    rx.setVoiceCallback([&](int slot, int, const uint8_t*) { o.bursts[slot == 2 ? 2 : 1]++; });
    DmrGenConfig c;
    c.snrDb = 30; c.traffic = 2; c.encrypted = encrypted;
    DmrSignal sig(c);
    std::vector<cf32> g(65536);
    for (int blk = 0; blk < 110; blk++) { sig.generate(g.data(), g.size()); rx.feed(g.data(), g.size()); }
    DmrTelemetry t;
    rx.telemetry(t, 0);
    o.decoded = t.voiceDecoded; o.audioSlot = t.audioSlot; o.path = t.ambePath; o.skipped = rx.encryptedBurstsSkipped(); o.frames = gFrames;
    return o;
}

int main() {
    // no mbelib may answer instead of the plug-in
#ifdef _WIN32
    _putenv_s("DECT2_MBELIB_PATH", "none");
#else
    setenv("DECT2_MBELIB_PATH", "none", 1);
#endif
    installStub();
    testLoader();
    const std::string folder = gFolder;
    codec::setPluginFactory("ambe2", [folder]() -> std::unique_ptr<codec::SpeechCodec> {
        std::unique_ptr<codec::SpeechCodec> c;
        codec::loadSpeechCodec("ambe2", folder, true, c);
        return c ? std::make_unique<Counting>(std::move(c)) : nullptr;
    }, folder + "/stub");

    const Run a = run(0, false);
    printf("auto: %llu decoded of %llu+%llu bursts, slot %d, %d plug-in frames, %zu samples, %zu non-zero\n", (unsigned long long)a.decoded,
           (unsigned long long)a.bursts[1], (unsigned long long)a.bursts[2], a.audioSlot, a.frames, a.tapped, a.nz);
    CHECK(a.decoded > 5, "nothing was decoded through the plug-in (%llu)", (unsigned long long)a.decoded);
    CHECK(a.frames >= (int)a.decoded * 3 && a.frames % 3 == 0, "frames %d for %llu bursts (3 frames per burst; telemetry lags a little)", a.frames, (unsigned long long)a.decoded);
    CHECK(a.tapped >= a.decoded * 2880 && a.nz > a.tapped / 2, "tap got %zu samples, %zu non-zero", a.tapped, a.nz);
    CHECK(a.path == folder + "/stub", "telemetry names the plug-in: %s", a.path.c_str());

    uint64_t anyDecoded = 0;
    for (int s = 1; s <= 2; s++) {
        const Run r = run(s, false);
        printf("slot %d only: %llu decoded of %llu bursts on it, audio slot %d\n", s, (unsigned long long)r.decoded, (unsigned long long)r.bursts[s], r.audioSlot);
        CHECK(r.audioSlot == 0 || r.audioSlot == s, "listening to slot %d played slot %d", s, r.audioSlot);
        CHECK(r.decoded <= r.bursts[s], "slot %d: %llu decoded but only %llu bursts on it", s, (unsigned long long)r.decoded, (unsigned long long)r.bursts[s]);
        anyDecoded += r.decoded;
    }
    CHECK(anyDecoded > 5, "fixed-slot runs decoded nothing");

    const Run e = run(0, true);
    printf("encrypted: %llu decoded, %llu bursts skipped, %d plug-in frames, %zu samples\n", (unsigned long long)e.decoded, (unsigned long long)e.skipped, e.frames, e.tapped);
    CHECK(e.decoded == 0 && e.frames == 0 && e.tapped == 0, "encrypted traffic reached the vocoder");
    CHECK(e.skipped > 0, "encrypted bursts were not seen");

    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
