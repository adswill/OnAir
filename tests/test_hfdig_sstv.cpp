// HF digital SSTV: test audio -> decoder, every mode, at 20 dB (signal to noise in 3 kHz) with a +-100 Hz tuning error and +-100 ppm of sample
// clock error; the mode picked from the line period when the VIS header is left out; and a Robot 36 picture through the engine.
// The long modes are fed straight from the generator, faster than real time.
#include "dect2/engine.h"
#include "dect2/hfdig_gen.h"
#include "dect2/hfdig_sstv.h"
#include "dect2/hfdig_tel.h"
#include "dect2/modes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Noise {
    uint64_t s = 12345;
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

// feeds the test audio of one mode to a decoder until a picture is done (or the time is up); returns the telemetry
static HfdigSstvTelemetry run(const SynthConfig& cfg, double snrDb, double maxSec) {
    auto gen = makeSstvTestAudio(cfg);
    auto dec = makeSstvDecoder();
    Noise nz;
    const double sigma = std::sqrt(0.125 / std::pow(10.0, snrDb / 10.0) * 4000.0 / 3000.0);
    std::vector<float> buf(800);
    HfdigSstvTelemetry t;
    for (int blk = 0; blk < (int)(maxSec * 10); blk++) {
        gen->generate(buf.data(), buf.size());
        for (float& v : buf) v += (float)(sigma * nz.g());
        dec->feedAudio(buf.data(), buf.size());
        if (blk % 10 == 0) { dec->telemetry(t); if (t.picturesDone >= 1) break; }
    }
    dec->telemetry(t);
    return t;
}

static double mae(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255;
    double s = 0;
    for (size_t i = 0; i < a.size(); i++) s += std::abs((int)a[i] - (int)b[i]);
    return s / (double)a.size();
}

int main() {
    for (int m = 0; m < sstvModeCount(); m++) {
        const SstvModeInfo& mi = sstvModeInfo(m);
        SynthConfig cfg;
        cfg.modeOpt[1] = m;
        cfg.modeVal[0] = (m & 1) ? -100 : 100;    // a dial error of +-100 Hz and a sound card clock +-100 ppm off, alternating
        cfg.sroPpm = (m & 1) ? -100 : 100;
        const double sec = 0.4 + 0.91 + mi.lineMs * (mi.width > 320 && mi.height > 300 ? mi.height / 2 : mi.height) / 1000.0 + 3;
        const HfdigSstvTelemetry t = run(cfg, 20, sec + 2);
        std::vector<uint8_t> ref;
        sstvTestPicture(mi.width, mi.height, ref);
        const bool ok = t.picturesDone >= 1 && t.image && t.image->complete;
        const double e = ok ? mae(t.image->rgb, ref) : 255;
        printf("%-15s VIS %3d (sent %3d) lines %3d/%3d slant %+6.0f ppm offset %+5.1f Hz  mean error %.2f\n", mi.name, t.visCode, mi.vis, t.lines, mi.height,
               t.slantPpm, t.offsetHz, e);
        CHECK(ok, "%s: no complete picture", mi.name);
        CHECK(t.visCode == mi.vis, "%s: VIS %d", mi.name, t.visCode);
        CHECK(t.mode == mi.name, "%s: mode '%s'", mi.name, t.mode.c_str());
        CHECK(e < 12, "%s: mean error %.2f", mi.name, e);
        CHECK(!t.history.empty() && t.history[0] == t.image, "%s: history", mi.name);
    }
    {   // no VIS header: the mode comes from the line period
        const int idx[2] = {3, 2};
        for (int k = 0; k < 2; k++) {
            const SstvModeInfo& mi = sstvModeInfo(idx[k]);
            SynthConfig cfg;
            cfg.modeOpt[1] = idx[k]; cfg.modeOpt[2] = 1; cfg.modeVal[0] = 30; cfg.sroPpm = 50;
            const HfdigSstvTelemetry t = run(cfg, 20, 0.4 + mi.lineMs * 256 / 1000.0 + 3);
            printf("%s without VIS: picked '%s', lines %d\n", mi.name, t.mode.c_str(), t.lines);
            CHECK(t.mode == mi.name && t.visCode == -1 && t.lines > 200, "no-VIS %s: '%s' vis %d lines %d", mi.name, t.mode.c_str(), t.visCode, t.lines);
        }
    }
    {   // through the engine: Robot 36 (index 0), the synthetic source at 6 times real time
        Engine e;
        e.hfdig().setSilent(true);
        const ModeTuning mt = hfdigTuning();
        DeviceInfo dev;
        TuneSettings t;
        t.centerHz = mt.defMhz * 1e6;
        t.bandwidthMhz = mt.bandwidthMhz;
        t.sampleRate = mt.sampleRate;
        t.synth.mode = 27;
        t.synth.modeOpt[0] = 1;
        t.synth.modeOpt[1] = 0;
        t.synth.snrDb = 25;
        t.synth.pace = 6;
        FileOptions fo;
        e.setStandard(27);
        CHECK(e.start(dev, t, fo), "engine start");
        RxTelemetry rx;
        uint64_t seq = 0;
        bool done = false;
        const auto t0 = std::chrono::steady_clock::now();
        while (!done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40) {
            if (e.latestRx(rx, seq)) { seq = rx.seq; done = rx.hfdig.sstv.picturesDone >= 1; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        e.stop();
        const HfdigSstvTelemetry& s = rx.hfdig.sstv;
        std::vector<uint8_t> ref;
        sstvTestPicture(320, 240, ref);
        const double err = s.image ? mae(s.image->rgb, ref) : 255;
        printf("engine: %.1f s, '%s' VIS %d lines %d, mean error %.2f\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), s.mode.c_str(), s.visCode, s.lines, err);
        CHECK(done && s.mode == "Robot 36" && s.lines == 240, "engine: done %d mode '%s' lines %d", (int)done, s.mode.c_str(), s.lines);
        CHECK(err < 20, "engine: mean error %.2f", err);
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
