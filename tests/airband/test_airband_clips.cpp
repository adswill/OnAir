// Airband clips (round 4 #10): one WAV per transmission from the real receiver on generated channels (two channels with their own keying, a
// tuning error and noise), file names, short openings dropped, the close time, a stuck carrier cut at 10 minutes, the 30 s replay, switched off.
#include "dect2/airband/airband_clips.h"
#include "dect2/airband/airband_gen.h"
#include "dect2/airband/airband_rx.h"
#include "dect2/voice/clip_history.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
using namespace dect2;
#include "../common/test_util.h"

namespace fs = std::filesystem;

static size_t wavs(const std::string& dir, const char* part = "") {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) if (e.path().extension() == ".wav" && e.path().filename().string().find(part) != std::string::npos) n++;
    return n;
}

int main() {
    const std::string dir = (fs::temp_directory_path() / "onair_test_airband_clips").string();
    std::error_code ec;
    fs::remove_all(dir, ec);
    CHECK(AirbandClipWriter::fileName("Tower 1/x", 118.1e6, 1'800'000'000) == "airband-TOWER_1_X-118.100-20270115T080000Z.wav", "file name %s",
          AirbandClipWriter::fileName("Tower 1/x", 118.1e6, 1'800'000'000).c_str());

    // the writer alone: bursts of sound between zeros
    {
        AirbandClipWriter w(dir + "/unit");
        int64_t clk = 1'800'000'000;
        w.setClock([&] { return clk; });
        w.setChannels({{118.1e6, "TWR"}, {121.5e6, "GUARD"}});
        w.setEnabled(true);
        std::vector<float> tone(8000), quiet(8000, 0.f);
        for (size_t i = 0; i < tone.size(); i++) tone[i] = 0.3f * (float)std::sin(i * 0.785);
        auto put = [&](int ch, const std::vector<float>& v, double sec) { for (double s = 0; s < sec - 1e-9; s += 1) w.feed(ch, v.data(), (size_t)(std::min(1.0, sec - s) * 8000)); clk += (int64_t)sec; };
        put(0, tone, 2); put(0, quiet, 2);                 // a 2 s call, closed after 1 s of silence
        put(0, std::vector<float>(tone.begin(), tone.begin() + 1600), 0.2); put(0, quiet, 2);   // 0.2 s: a squelch click, not saved
        put(1, tone, 3); put(1, std::vector<float>(4000, 0.f), 0.5); put(1, tone, 1); put(1, quiet, 2);   // a 0.5 s pause inside one call
        auto c = w.takeFinished();
        CHECK(c.size() == 2, "two clips (%zu)", c.size());
        if (c.size() == 2) {
            CHECK(std::fabs(c[0].seconds - 2.0) < 0.01 && c[0].label == "TWR 118.100 MHz" && c[0].mode == "airband", "first clip %.2f s %s", c[0].seconds, c[0].label.c_str());
            CHECK(std::fabs(c[1].seconds - 4.5) < 0.01 && c[1].label.find("GUARD") == 0, "a pause inside one call stays one clip (%.2f s)", c[1].seconds);
            int rate = 0; double sec = 0;
            CHECK(voice::wavInfo(c[0].path, rate, sec) && rate == 8000 && std::fabs(sec - 2.0) < 0.01, "a readable 8 kHz WAV");
        }
        std::string err;
        const std::string rp = w.saveReplay(1, err);
        int rate = 0; double sec = 0;
        CHECK(!rp.empty() && voice::wavInfo(rp, rate, sec) && std::fabs(sec - 4.5 - 1.0) < 0.05, "replay holds the call and its tail, not the gaps (%.2f s)", sec);
        CHECK(w.saveReplay(5, err).empty(), "replay of a silent channel: nothing");
        w.setEnabled(false);
        put(0, tone, 2); put(0, quiet, 2);
        CHECK(w.takeFinished().empty() && !w.saveReplay(0, err).empty(), "switched off: no clip, the replay still works");
        // a stuck carrier is cut every 10 minutes
        w.setEnabled(true);
        for (int s = 0; s < 1300; s++) w.feed(0, tone.data(), tone.size());
        w.flush();
        c = w.takeFinished();
        CHECK(c.size() == 3 && std::fabs(c[0].seconds - 600) < 0.01, "stuck carrier: 600 + 600 + 100 s (%zu)", c.size());
    }

    // the receiver: two generated channels with their own keying, a 2 kHz tuning error and noise
    {
        const double rate = 1e6, dial = 118.700e6, off = airbandTuning().tuneOffsetHz;
        AirbandGenConfig g;
        g.rate = rate; g.dialOffsetHz = -off; g.snrDb = 25; g.cfoHz = 2000;
        AirbandGenChannel a; a.offsetHz = 0; a.audio = 0; a.bursts = {{1.0, 2.0}, {5.0, 1.5}, {8.0, 0.2}};
        AirbandGenChannel b; b.offsetHz = 100e3; b.is833 = false; b.audio = 1; b.levelDb = -6; b.bursts = {{2.0, 3.0}};
        g.chans = {a, b};
        AirbandGenerator gen(g);
        std::vector<cf32> x((size_t)(11 * rate));
        for (size_t o = 0; o < x.size(); o += 65536) gen.generate(x.data() + o, std::min<size_t>(65536, x.size() - o));
        AirbandReceiver rx;
        rx.setSilent(true);
        rx.configure(rate);
        rx.setCenterHz(dial + off);
        std::vector<AirbandChannel> list(2);
        list[0].freqHz = dial; list[0].label = "Tower";
        list[1].freqHz = dial + 100e3;
        rx.setChannels(list);
        auto w = std::make_shared<AirbandClipWriter>(dir + "/rx");
        w->setEnabled(true);
        w->setChannels({{dial, "Tower"}, {dial + 100e3, airbandName(dial + 100e3, false)}});
        rx.setClipWriter(w);
        for (size_t o = 0; o < x.size(); o += 8192) rx.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
        w->flush();
        const auto c = w->takeFinished();
        int tw = 0, other = 0;
        double twSec = 0;
        for (const auto& k : c) { if (k.label.find("Tower") == 0) { tw++; twSec += k.seconds; } else other++; printf("  clip %s %.2f s\n", k.label.c_str(), k.seconds); }
        CHECK((tw == 2 || tw == 3) && other == 1, "a clip per transmission: Tower 2, or 3 when the 0.2 s one with its hang time passes 0.4 s; the other 1 (%d, %d)", tw, other);
        CHECK(twSec > 3.4 && twSec < 6.5, "Tower clips: 3.5 s of calls plus the hang times (%.2f)", twSec);
        CHECK(wavs(dir + "/rx", "TOWER") == (size_t)tw && wavs(dir + "/rx", "118.800") == (size_t)other, "the files are named with the channel and frequency");
    }
    fs::remove_all(dir, ec);
    return testSummary("airband_clips");
}
