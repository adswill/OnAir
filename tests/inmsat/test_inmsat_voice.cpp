// Stage 3 of the Inmarsat all-carriers mode: the voice mixer and recorder WITHOUT the codec (silence and the install message, the coded frames
// are still saved) and with a fake test decoder (mute / solo, mixing, WAV recording). The real AMBE codec is never part of the tests; the C-channel
// decoder that feeds the mixer is tested in tests/aero/test_aero_cchannel.cpp and tests/inmsat/test_inmsat_cchannel.cpp.
#include "dect2/inmsat/inmsat_voice.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace dect2;
using namespace dect2::inmsat;
#include "../common/test_util.h"

namespace {
// 12-byte frame: byte 0 is the amplitude in units of 100; the decoder returns 160 samples of that constant
struct FakeCodec : codec::SpeechCodec {
    int sampleRate() const override { return 8000; }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override {
        if (coded.size() != 12) return {};
        return std::vector<int16_t>(160, (int16_t)(coded[0] * 100));
    }
};
std::vector<uint8_t> frame(int amp) { std::vector<uint8_t> f(12, 0); f[0] = (uint8_t)amp; return f; }
long fileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }
}

int main() {
    // a folder of its own: create_directory is true only when it made a new one, so a leftover or another run's folder is never written to or
    // removed (POSIX mkdir(path, mode) does not exist on MinGW)
    std::filesystem::path own;
    {
        std::error_code ec;
        const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
        const std::string stem = "inmsat_voice_test_" + std::to_string((long)getpid()) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (int k = 0; !ec && k < 100 && own.empty(); k++) {
            const std::filesystem::path p = tmp / (stem + "_" + std::to_string(k));
            if (std::filesystem::create_directory(p, ec)) own = p;
        }
        if (own.empty()) { printf("FAIL: no temporary folder of its own: %s\n", ec.message().c_str()); return 1; }
    }
    const std::string dir = own.generic_string();
    // 1. no codec: silence, the install message, calls counted, coded frames saved
    {
        VoiceMixer m;
        m.setDecoderFactory([] { return std::unique_ptr<codec::SpeechCodec>(); });   // what pluginVocoder() gives when nothing is installed
        m.setRecording(dir);
        m.setCodecFolder("/Users/test/codecs");
        for (int i = 0; i < 10; i++) { m.pushFrame(7, 1545.5e6, frame(20), 0.02 * i); m.pushFrame(8, 1546.1e6, frame(30), 0.02 * i); }
        int16_t out[800];
        for (auto& v : out) v = 123;
        m.mix(out, 800);
        bool silent = true;
        for (auto v : out) silent &= v == 0;
        CHECK(silent, "output is not silent without a codec");
        VoiceStatus s = m.status();
        CHECK(!s.codecReady && s.channels.size() == 2 && s.activeCalls == 2, "status: ready %d channels %zu calls %d", s.codecReady, s.channels.size(), s.activeCalls);
        CHECK(s.channels[0].frames == 10 && s.channels[0].decoded == 0, "frames %llu decoded %llu", (unsigned long long)s.channels[0].frames, (unsigned long long)s.channels[0].decoded);
        m.endIdle(10.0);
        s = m.status();
        CHECK(s.activeCalls == 0, "calls after idle: %d", s.activeCalls);
        // the raw frames of the first call are on disk: 10 frames of 12 bytes
        bool found = false;
        for (int n = 1; n <= 2; n++) {
            const long sz = fileSize(dir + "/inmarsat_1545500_" + std::to_string(n) + ".bits");
            if (sz == 120) found = true;
        }
        CHECK(found, "raw frames not saved");
    }
    // the message names the file to install (checked with the registry's real status line; the plug-in is not installed in the test)
    {
        VoiceMixer m;      // default factory: pluginVocoder("aeroambe")
        m.setCodecFolder("/Users/test/codecs");
        m.pushFrame(1, 1545.5e6, frame(5), 0);
        const VoiceStatus s = m.status();
        printf("message: %s\n", s.message.c_str());
        CHECK(s.message.find("AMBE") != std::string::npos && s.message.find("onair-codec-aeroambe") != std::string::npos && s.message.find("/Users/test/codecs") != std::string::npos,
              "install message incomplete");
        int16_t out[160];
        m.mix(out, 160);
        for (auto v : out) CHECK(v == 0, "not silent");
    }
    // 2. a fake decoder: mixing, mute, solo, recording
    {
        VoiceMixer m;
        m.setDecoderFactory([] { return std::unique_ptr<codec::SpeechCodec>(new FakeCodec); });
        m.setRecording(dir);
        auto push = [&](int id, int amp, int frames, double t0) { for (int i = 0; i < frames; i++) m.pushFrame(id, 1545e6 + id * 1e4, frame(amp), t0 + 0.02 * i); };
        int16_t out[160];
        push(1, 10, 3, 0); push(2, 20, 3, 0);                       // 1000 and 2000
        m.mix(out, 160);
        CHECK(out[0] == 3000 && out[159] == 3000, "mix %d %d", out[0], out[159]);
        m.setMute(1, true);
        m.mix(out, 160);
        CHECK(out[5] == 2000, "muted channel 1: %d", out[5]);    // the muted channel's audio is dropped, not queued
        m.setMute(1, false); m.setSolo(1, true);
        m.mix(out, 160);
        CHECK(out[5] == 1000, "solo channel 1: %d", out[5]);
        m.setSolo(1, false);
        m.mix(out, 160);
        CHECK(out[5] == 0, "queue empty after 3 frames: %d", out[5]);
        const VoiceStatus s = m.status();
        CHECK(s.codecReady && s.message.empty() && s.activeCalls == 2, "status ready %d msg '%s' calls %d", s.codecReady, s.message.c_str(), s.activeCalls);
        // clipping
        push(1, 250, 1, 1); push(2, 250, 1, 1);
        m.mix(out, 160);
        CHECK(out[0] == 32767, "clip %d", out[0]);
        m.endIdle(100.0);
        // WAV: 3 + 1 frames of 160 samples for channel 1 -> 640 samples = 1280 bytes + 44
        bool wav = false;
        for (int n = 1; n <= 8; n++) if (fileSize(dir + "/inmarsat_1545010_" + std::to_string(n) + ".wav") == 44 + 1280) wav = true;
        CHECK(wav, "WAV of channel 1 not the expected size");
        // the header is valid RIFF with the right data length
        for (int n = 1; n <= 8; n++) {
            const std::string p = dir + "/inmarsat_1545010_" + std::to_string(n) + ".wav";
            if (fileSize(p) != 44 + 1280) continue;
            std::ifstream f(p, std::ios::binary);
            char h[44]; f.read(h, 44);
            uint32_t dl; std::memcpy(&dl, h + 40, 4);
            CHECK(std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVEfmt ", 8) == 0 && dl == 1280, "WAV header");
        }
    }
    { std::error_code ec; std::filesystem::remove_all(own, ec); CHECK(!ec, "remove the temporary folder: %s", ec.message().c_str()); }
    return testSummary("inmsat_voice");
}
