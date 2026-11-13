// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Yaesu Fusion voice: the sub-frames go to the user-installed codec plug-in when there is one, in the form the header documents, and
// the callsigns and data are decoded without it. A fake plug-in file and opener stand in for the real codec (OnAir ships none).
#include "fusion_test_util.h"
#include "dect2/codec/speech_codec.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
using namespace dect2;
using namespace dect2::fusion;
#include "../common/test_util.h"
using namespace fusiontest;

struct Log { std::mutex mu; std::vector<std::vector<uint8_t>> frames; };

class FakeCodec : public codec::SpeechCodec {
public:
    explicit FakeCodec(Log* l) : l_(l) {}
    int sampleRate() const override { return 8000; }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override {
        std::lock_guard<std::mutex> lk(l_->mu);
        l_->frames.push_back(coded);
        return std::vector<int16_t>(160, (int16_t)1000);   // 20 ms
    }
private:
    Log* l_;
};

int main() {
    const int kinds[4] = {1, 0, 2, 3};
    for (int dt : {0, 2, 3}) {
        GenParams p;
        p.dt = dt; p.commFrames = 10; p.src = "LY1ABC"; p.dest = "CQCQCQ"; p.seed = 11 + (uint32_t)dt;
        Log log;
        FusionReceiver r;
        r.setSilent(true);
        size_t tapped = 0; int tapRate = 0;
        r.setAudioTap([&](const float* x, size_t n, int sr) { (void)x; tapped += n; tapRate = sr; });
        r.setSpeechCodec(std::make_unique<FakeCodec>(&log));
        run(r, signal({p}, 0, 25));
        const FusionTelemetry t = last(r);
        // what the generator sent, in order
        std::vector<uint8_t> bits;
        std::vector<FrameContent> sent;
        genTransmissionBits(p, bits, &sent);
        std::vector<std::vector<uint8_t>> want;
        for (const auto& c : sent) for (const auto& v : c.voice) want.push_back(v.bytes);
        CHECK(!want.empty() && log.frames == want, "dt %d: the plug-in got %zu frames, expected %zu (equal: %d)", dt, log.frames.size(), want.size(), (int)(log.frames == want));
        CHECK(t.codecReady && t.voiceDecoded == want.size() && t.voiceSubFrames == want.size(), "dt %d: decoded %llu of %llu", dt, (unsigned long long)t.voiceDecoded, (unsigned long long)t.voiceSubFrames);
        CHECK(tapped == want.size() * 160 && tapRate == 8000, "dt %d: audio %zu samples at %d Hz", dt, tapped, tapRate);
        CHECK(t.src == "LY1ABC", "dt %d: source '%s'", dt, t.src.c_str());
        (void)kinds;
    }

    // no codec: callsigns, data and counts, no audio
    {
        GenParams p;
        p.dt = 2; p.commFrames = 8; p.src = "LY1ABC"; p.dest = "CQCQCQ"; p.slow = "no codec here";
        FusionReceiver r;
        r.setSilent(true);
        size_t tapped = 0;
        r.setAudioTap([&](const float*, size_t n, int) { tapped += n; });
        run(r, signal({p}, 0, 25));
        const FusionTelemetry t = last(r);
        CHECK(tapped == 0 && !t.codecReady && t.voiceDecoded == 0, "audio without a codec");
        CHECK(t.codecText == "codec not installed", "text '%s'", t.codecText.c_str());
        CHECK(t.voiceSubFrames == 40, "voice sub-frames counted: %llu", (unsigned long long)t.voiceSubFrames);
        CHECK(t.src == "LY1ABC" && t.slowText.find("no codec here") != std::string::npos, "data without codec: '%s' '%s'", t.src.c_str(), t.slowText.c_str());
    }

    // the loader gate: no file -> not installed; file but no notice -> the notice; accepted -> ready, and the receiver then uses it
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "onair_fusion_codec_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        Log log;
        auto opener = [&](const std::string&, std::string&) { return std::unique_ptr<codec::SpeechCodec>(new FakeCodec(&log)); };
        std::unique_ptr<codec::SpeechCodec> c;
        codec::CodecLookup l = codec::loadSpeechCodec("ysf", dir.string(), true, c, opener);
        CHECK(l.status == codec::CodecStatus::NotInstalled && !c, "empty folder: status %d", (int)l.status);
        std::ofstream(dir / codec::speechCodecFileName("ysf")) << "plug-in";
        l = codec::loadSpeechCodec("ysf", dir.string(), false, c, opener);
        CHECK(l.status == codec::CodecStatus::NoticeRequired && !c, "no notice: status %d", (int)l.status);
        l = codec::loadSpeechCodec("ysf", dir.string(), true, c, opener);
        CHECK(l.status == codec::CodecStatus::Ready && c, "accepted: status %d", (int)l.status);
        GenParams p;
        p.dt = 0; p.commFrames = 6;
        FusionReceiver r;
        r.setSilent(true);
        r.setSpeechCodec(std::move(c));
        run(r, signal({p}, 0, 25));
        CHECK(log.frames.size() == 30, "frames through the loaded plug-in: %zu", log.frames.size());
        fs::remove_all(dir);
    }
    return testSummary("test_fusion_codec");
}
