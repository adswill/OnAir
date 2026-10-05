// ATSC 3.0 through the engine, as the GUI uses it: an IQ recording (a few frames made here) is opened as a file source with the ATSC 3.0
// standard selected; the engine must lock, find the service, and its transport stream analysis must see a video and an audio stream.
#include "atsc3_sim.h"
#include "dect2/atsc3_sync.h"
#include "dect2/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <thread>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(int argc, char** argv) {
    const char* vp = argc > 1 ? argv[1] : "tests/data/route_video.mp4";
    const char* ap = argc > 2 ? argv[2] : "tests/data/route_audio.mp4";
    const double radio = 10e6;
    FramePlp plp; plp.id = 1; plp.fecType = 0; plp.mod = 2; plp.cod = 6;
    Bicm bicm(plpBicm(plp));
    sim::Stream st = sim::buildStream(vp, ap, bicm.kPayload() / 8);
    if (st.bb.empty()) { printf("test files not found\n"); return 1; }
    FrameSetup fs;
    fs.bs.preambleStructure = 0; fs.bs.bsrCoefficient = 8; fs.bs.numSymbols = 4; fs.bs.minTimeToNext = 1;
    fs.fftCode = 0; fs.guardCode = 1; fs.spPattern = 4; fs.numSymbols = 120; fs.l1DetailMode = 3; fs.sbsNullCells = 16;
    const double frameRate = postBootstrapRate(fs.bs);
    const double period = 0.118;
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0.f, 1.f);
    std::vector<cf32> wave((size_t)(0.02 * radio), cf32(0, 0));
    for (size_t i = 0; i < st.bb.size(); i += 8) {
        FramePlp fp = plp;
        for (size_t k = i; k < std::min(st.bb.size(), i + 8); k++) fp.bbPackets.push_back(st.bb[k]);
        auto frame = buildFrame(fs, {fp});
        std::vector<cf32> b, f, boot = generateBootstrap(fs.bs);
        resampleExact(boot.data(), boot.size(), kBootstrapRate, radio, b);
        resampleExact(frame.data(), frame.size(), frameRate, radio, f);
        std::vector<cf32> one(b);
        one.insert(one.end(), f.begin(), f.end());
        one.resize((size_t)(period * radio), cf32(0, 0));
        wave.insert(wave.end(), one.begin(), one.end());
    }
    const float sigma = (float)std::sqrt(std::pow(10.0, -26.0 / 10.0) / 2.0);
    const char* path = "/tmp/dect2_atsc3_test.cs8";
    {
        std::ofstream o(path, std::ios::binary);
        std::vector<int8_t> b8;
        for (size_t i = 0; i < wave.size(); i++) {
            cf32 v = wave[i] * cf32((float)std::cos(2 * M_PI * 3100.0 * i / radio), (float)std::sin(2 * M_PI * 3100.0 * i / radio)) + cf32(g(rng), g(rng)) * sigma;
            b8.push_back((int8_t)std::max(-127.f, std::min(127.f, std::round(v.real() * 22.f))));
            b8.push_back((int8_t)std::max(-127.f, std::min(127.f, std::round(v.imag() * 22.f))));
        }
        o.write((const char*)b8.data(), (std::streamsize)b8.size());
    }
    printf("  wrote %zu samples (%.2f s) to %s\n", wave.size(), wave.size() / radio, path);

    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "ATSC 3.0 test file";
    TuneSettings tune; tune.bandwidthMhz = 6; tune.sampleRate = radio;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = radio; fo.loop = false;
    e.setStandard(5);
    CHECK(e.start(dev, tune, fo), "engine started");
    CHECK(e.activeStandard() == 4, "ATSC 3.0 is the active standard");
    bool locked = false, ready = false, tsOk = false;
    Atsc3Telemetry t;
    for (int i = 0; i < 120 && !(locked && ready && tsOk); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (e.atsc3Telemetry(t)) { locked |= t.locked || t.frames >= 3; ready |= t.serviceReady; }
        auto ts = e.tsSnapshot();
        for (auto& s : ts.services) {
            bool v = false, a = false;
            for (auto& x : s.streams) { v |= x.kind == "video"; a |= x.kind == "audio"; }
            tsOk |= v && a;
        }
    }
    e.atsc3Telemetry(t);
    printf("  frames %ld (%ld failed), baseband packets %ld (%ld bad), services %zu, selected %d, TS bytes %ld, load %.2f\n", t.frames, t.framesFailed, t.bbPackets, t.bbBad, t.services.size(), t.selected, t.tsBytes, t.load);
    auto ts = e.tsSnapshot();
    for (auto& s : ts.services) { printf("  TS service %d '%s':", s.id, s.name.c_str()); for (auto& x : s.streams) printf(" %s/%s", x.kind.c_str(), x.codec.c_str()); printf("\n"); }
    RxTelemetry rt;
    CHECK(e.latestRx(rt, 0) && rt.standard == 4 && rt.state == 2, "telemetry reports ATSC 3.0, locked");
    CHECK(locked, "locked");
    CHECK(t.services.size() == 1 && t.services[0].serviceId == 1001 && t.selected == 1001, "service found and selected by itself");
    CHECK(ready, "service signaling received");
    CHECK(tsOk, "the transport stream analysis sees a video and an audio stream");
    e.stop();
    printf(fails ? "atsc3 engine: FAILED\n" : "atsc3 engine: ok\n");
    return fails ? 1 : 0;
}
