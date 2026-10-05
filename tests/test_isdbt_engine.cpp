// ISDB-T through the engine, as the GUI uses it: an IQ recording made here (a one-segment layer and a twelve-segment layer carrying the
// test programme) is opened as a file source with the ISDB-T standard selected; the engine must lock, decode the transport stream and its
// analysis must see the service with a video and an audio stream.
#include "dect2/demo_ts.h"
#include "dect2/engine.h"
#include "dect2/exact_resampler.h"
#include "dect2/isdbt_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace dect2;
using namespace dect2::isdbt;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    const double radio = 10e6;
    Params p;
    p.mode = 2; p.guard = kGi8; p.partial = true;
    p.layer[0].segments = 1; p.layer[0].mod = kQpsk; p.layer[0].rate = kR23; p.layer[0].ti = 1;
    p.layer[1].segments = 12; p.layer[1].mod = k16Qam; p.layer[1].rate = kR34; p.layer[1].ti = 1;
    Generator gen(p, singleLayerSource(1, demoTsSource(layerBitrate(p, 1) * 0.9)), 1);
    std::vector<cf32> sig, frame;
    for (int f = 0; f < 40; f++) { gen.nextFrame(frame); sig.insert(sig.end(), frame.begin(), frame.end()); }
    ExactResampler rs;
    rs.configure(kSampleRate, radio);
    std::vector<cf32> wave((size_t)(0.02 * radio), cf32(0, 0));
    rs.process(sig.data(), sig.size(), wave);
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0.f, 1.f);
    const float sigma = (float)std::sqrt(std::pow(10.0, -28.0 / 10.0) / 2.0);
    const std::string pathStr = (std::filesystem::temp_directory_path() / "dect2_isdbt_test.cs8").string();
    const char* path = pathStr.c_str();
    {
        std::ofstream o(path, std::ios::binary);
        std::vector<int8_t> b8;
        for (size_t i = 0; i < wave.size(); i++) {
            const double ph = 2 * M_PI * 2800.0 * (double)i / radio;
            cf32 v = wave[i] * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(g(rng), g(rng)) * sigma;
            b8.push_back((int8_t)std::max(-127.f, std::min(127.f, std::round(v.real() * 22.f))));
            b8.push_back((int8_t)std::max(-127.f, std::min(127.f, std::round(v.imag() * 22.f))));
            if (b8.size() >= (1 << 20)) { o.write((const char*)b8.data(), (std::streamsize)b8.size()); b8.clear(); }
        }
        o.write((const char*)b8.data(), (std::streamsize)b8.size());
    }
    printf("  wrote %zu samples (%.2f s) to %s\n", wave.size(), wave.size() / radio, path);

    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "ISDB-T test file";
    TuneSettings tune; tune.bandwidthMhz = 6; tune.sampleRate = radio;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = radio; fo.loop = false;
    e.setStandard(6);
    CHECK(e.start(dev, tune, fo), "engine started");
    CHECK(e.activeStandard() == 5, "ISDB-T is the active standard");
    bool tmcc = false, tsOk = false;
    RxTelemetry t;
    for (int i = 0; i < 120 && !(tmcc && tsOk); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (e.latestRx(t, 0)) tmcc |= t.standard == 5 && t.isdbt.tmccOk;
        auto ts = e.tsSnapshot();
        for (auto& s : ts.services) {
            bool v = false, a = false;
            for (auto& x : s.streams) { v |= x.kind == "video"; a |= x.kind == "audio"; }
            tsOk |= v && a;
        }
    }
    e.latestRx(t, 0);
    printf("  mode %d, GI index %d, TMCC %d, layers: A %d segments, B %d segments; SNR %.1f dB, CFO %.1f Hz\n", t.isdbt.mode, t.giIdx, (int)t.isdbt.tmccOk, t.isdbt.layer[0].segments, t.isdbt.layer[1].segments, t.dataSnrDb, t.cfoHz);
    CHECK(tmcc, "TMCC decoded");
    CHECK(t.isdbt.mode == 2 && t.isdbt.partial && t.isdbt.layer[0].segments == 1 && t.isdbt.layer[1].segments == 12, "parameters read from the TMCC");
    CHECK(std::fabs(t.cfoHz - 2800.0) < 60.0, "carrier offset");
    CHECK(tsOk, "the transport stream analysis sees a video and an audio stream");
    e.stop();
    printf(fails ? "isdbt engine: FAILED\n" : "isdbt engine: ok\n");
    return fails ? 1 : 0;
}
