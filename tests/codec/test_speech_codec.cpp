// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// User-installed speech codecs: the plug-in is looked for only in the codecs folder, the legal notice gates loading, a missing codec
// gives the "not installed" message, and a loaded codec decodes frames to PCM (a fake codec that returns silence stands in for one).
#include "dect2/codec/speech_codec.h"
#include "../common/test_util.h"
#include <filesystem>
#include <fstream>

using namespace dect2::codec;
namespace fs = std::filesystem;

namespace {

class SilenceCodec final : public SpeechCodec {
public:
    int sampleRate() const override { return 8000; }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override {
        return std::vector<int16_t>(coded.size() * 4, 0);   // silence, 4 samples per coded byte
    }
};

int gOpens = 0;
std::unique_ptr<SpeechCodec> fakeOpen(const std::string&, std::string&) {
    ++gOpens;
    return std::make_unique<SilenceCodec>();
}

void writeFile(const fs::path& p) {
    std::ofstream(p, std::ios::binary) << "not a real library: the fake opener does not read it";
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair-test-speech-codec";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string folder = dir.string();
    std::unique_ptr<SpeechCodec> codec;

    // The notice text is there and says what the user takes on.
    const std::string notice = speechCodecNotice();
    CHECK(notice.find("does not include") != std::string::npos, "notice says OnAir does not include the codec");
    CHECK(notice.find("laws") != std::string::npos, "notice mentions local laws");
    CHECK(notice.find("licence") != std::string::npos, "notice mentions the licence");

    // 1. No plug-in in the folder: not installed, with the docs link, and no codec.
    CodecLookup r = loadSpeechCodec("tetra", folder, false, codec, fakeOpen);
    CHECK(r.status == CodecStatus::NotInstalled, "missing plug-in is NotInstalled");
    CHECK(r.message.find("not installed") != std::string::npos, "not-installed message says so: %s", r.message.c_str());
    CHECK(r.message.find("docs/CODECS.md") != std::string::npos, "not-installed message links to docs/CODECS.md");
    CHECK(!codec, "no codec is returned when not installed");
    CHECK(gOpens == 0, "nothing is opened when not installed");

    // 2. Plug-in present, notice not accepted: the notice is required and nothing is loaded.
    const fs::path plugin = dir / speechCodecFileName("tetra");
    writeFile(plugin);
    r = loadSpeechCodec("tetra", folder, false, codec, fakeOpen);
    CHECK(r.status == CodecStatus::NoticeRequired, "plug-in without accepted notice is NoticeRequired");
    CHECK(!codec && gOpens == 0, "codec is not loaded before the notice is accepted");

    // 3. Notice accepted: the codec loads and decodes silence.
    r = loadSpeechCodec("tetra", folder, true, codec, fakeOpen);
    CHECK(r.status == CodecStatus::Ready, "accepted notice loads the codec");
    CHECK(codec != nullptr, "codec is returned when ready");
    CHECK(gOpens == 1, "the opener ran once");
    if (codec) {
        CHECK(codec->sampleRate() == 8000, "sample rate from the codec");
        const std::vector<int16_t> pcm = codec->decode(std::vector<uint8_t>(18, 0xAA));
        CHECK(pcm.size() == 72, "18 coded bytes give 72 samples, got %zu", pcm.size());
        bool silent = true;
        for (const int16_t s : pcm) silent = silent && s == 0;
        CHECK(silent, "decoded PCM is silence");
    }

    // 4. An opener that fails gives LoadFailed, not a crash.
    codec.reset();
    r = loadSpeechCodec("tetra", folder, true, codec,
                        [](const std::string&, std::string& err) { err = "test failure"; return std::unique_ptr<SpeechCodec>(); });
    CHECK(r.status == CodecStatus::LoadFailed, "failed open is LoadFailed");
    CHECK(!codec, "no codec after a failed open");

    // 4b. The real loader on a file that is not a library: LoadFailed, with the system's reason and the full path in the message.
    codec.reset();
    r = loadSpeechCodec("tetra", folder, true, codec);
    CHECK(r.status == CodecStatus::LoadFailed, "the real loader refuses a garbage file");
    CHECK(!codec, "no codec from a garbage file");
    CHECK(r.message.find("would not load") != std::string::npos && r.message.find(r.path) != std::string::npos &&
          r.message.size() > r.path.size() + 40, "the message carries the path and the system error: %s", r.message.c_str());

    // 4c. Notice not accepted and not installed give their own states.
    r = loadSpeechCodec("tetra", folder, false, codec, fakeOpen);
    CHECK(r.status == CodecStatus::NoticeRequired && !codec, "notice required until accepted");
    r = loadSpeechCodec("nothere", folder, true, codec, fakeOpen);
    CHECK(r.status == CodecStatus::NotInstalled && r.message.find("onair-codec-nothere") != std::string::npos, "not installed names the file");

    // 5. Bad ids are refused before any file is looked at.
    r = loadSpeechCodec("../tetra", folder, true, codec, fakeOpen);
    CHECK(r.status == CodecStatus::LoadFailed, "an id with path characters is refused");

    fs::remove_all(dir, ec);
    return testSummary("speech_codec");
}
