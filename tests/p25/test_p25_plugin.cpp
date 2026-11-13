// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 phase 1 voice through a user-installed codec plug-in "imbe" (stub library, tests/codec/stub_codec_plugin.c, no codec): the test schedule
// has clear voice calls and an encrypted one. Every clear IMBE frame reaches the plug-in as 23 bytes and comes out as 160 samples of audio;
// the encrypted call is reported but none of its frames reaches the plug-in. Also checks the AMBE (D-STAR) and AMBE+2 (NXDN, dPMR) vocoder
// classes against the stub: 12 byte frames in, 160 samples out. NOT covered: a real codec, a real P25 / NXDN / D-STAR recording.
#include "dect2/codec/codec_registry.h"
#include "dect2/dstar/dstar_ambe.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_imbe.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
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
static std::map<size_t, int> gSizes;   // frame size -> number of frames seen by the plug-ins

class Counting final : public codec::SpeechCodec {
public:
    explicit Counting(std::unique_ptr<codec::SpeechCodec> in) : in_(std::move(in)) {}
    int sampleRate() const override { return in_->sampleRate(); }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override { { std::lock_guard<std::mutex> lk(gMu); gSizes[coded.size()]++; } return in_->decode(coded); }
private:
    std::unique_ptr<codec::SpeechCodec> in_;
};

int main() {
#ifdef _WIN32
    _putenv_s("DECT2_MBELIB_PATH", "none");
#else
    setenv("DECT2_MBELIB_PATH", "none", 1);
#endif
    const fs::path dir = fs::temp_directory_path() / "onair-test-p25-plugin";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string folder = dir.string();
    for (const char* id : {"imbe", "ambe", "ambe2"}) {
        fs::copy_file(STUB_PLUGIN_PATH, dir / codec::speechCodecFileName(id), ec);
        CHECK(!ec, "copy of the stub failed");
        const std::string cid = id;
        codec::setPluginFactory(cid, [folder, cid]() -> std::unique_ptr<codec::SpeechCodec> {
            std::unique_ptr<codec::SpeechCodec> c;
            codec::loadSpeechCodec(cid, folder, true, c);
            return c ? std::make_unique<Counting>(std::move(c)) : nullptr;
        }, folder + "/" + cid);
    }

    {   // the vocoder classes of the other modes: right frame size in, 160 samples out
        P25Imbe imbe; DstarAmbe dstar; NxdnAmbe nxdn;
        CHECK(imbe.ok() && dstar.ok() && nxdn.ok(), "decoders not ok with the plug-ins loaded");
        std::string p;
        CHECK(p25ImbeAvailable(&p) && dstarAmbeAvailable() && nxdnAmbeAvailable(), "Available() ignores the plug-in");
        int16_t pcm[160];
        char fr[8][23] = {};
        uint8_t bits[72] = {};
        memset(pcm, 0, sizeof pcm); imbe.decode(fr, pcm);
        CHECK(pcm[1] == 38, "imbe pcm[1] = %d", pcm[1]);
        memset(pcm, 0, sizeof pcm); dstar.decode(bits, pcm);
        CHECK(pcm[1] == 38, "ambe pcm[1] = %d", pcm[1]);
        memset(pcm, 0, sizeof pcm); nxdn.decode(bits, pcm);
        CHECK(pcm[1] == 38, "ambe2 pcm[1] = %d", pcm[1]);
        std::lock_guard<std::mutex> lk(gMu);
        CHECK(gSizes[23] == 1 && gSizes[12] == 2, "frame sizes seen: 23 -> %d, 12 -> %d", gSizes[23], gSizes[12]);
        gSizes.clear();
    }

    P25GenConfig c;
    c.rate = 500000; c.snrDb = 40; c.loop = false;
    c.txs = p25TestSchedule();
    P25Generator g(c);
    P25Receiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    int clearFrames = 0, encFrames = 0;
    size_t tapped = 0;
    rx.setVoiceTap([&](const uint8_t*, bool clear) { (clear ? clearFrames : encFrames)++; });
    rx.setAudioTap([&](const float*, size_t n) { tapped += n; });
    double total = c.leadSec;
    for (const auto& t : c.txs) { std::vector<int8_t> s; p25TxSymbols(t, s); total += s.size() / 4800.0 + t.gapSec; }
    std::vector<cf32> buf(25000);
    for (double s = 0; s < total + 1.5; s += buf.size() / c.rate) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    P25Telemetry t;
    rx.telemetry(t, 0);
    int seen;
    { std::lock_guard<std::mutex> lk(gMu); seen = gSizes[23]; }
    printf("clear frames %d, encrypted frames %d, plug-in frames %d, audio %zu samples\n", clearFrames, encFrames, seen, tapped);
    CHECK(clearFrames > 20, "too few clear voice frames (%d)", clearFrames);
    CHECK(encFrames > 0, "the encrypted call was not seen");
    CHECK(seen > 0 && seen <= clearFrames, "plug-in saw %d frames for %d clear ones (an encrypted frame must never reach it)", seen, clearFrames);
    CHECK(seen >= clearFrames * 9 / 10, "plug-in saw only %d of %d clear frames", seen, clearFrames);
    CHECK(tapped == (size_t)seen * 160, "audio %zu samples for %d frames", tapped, seen);
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
