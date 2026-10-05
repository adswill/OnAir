// The complete receiver on a continuous signal: frames sent back to back with gaps, the bootstrap of each at 6.144 Msamples/s and the rest at
// the frame's rate, everything converted to the radio's rate, with a carrier frequency offset, noise and an echo. The synchroniser finds the
// frames on its own, and the receiver delivers a transport stream with all the video and audio frames.
#include "atsc3_sim.h"
#include "dect2/atsc3_sync.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static std::vector<cf32> convert(const std::vector<cf32>& x, double in, double out) {
    std::vector<cf32> y;
    resampleExact(x.data(), x.size(), in, out, y);
    return y;
}

int main(int argc, char** argv) {
    const char* vp = argc > 1 ? argv[1] : "tests/data/route_video.mp4";
    const char* ap = argc > 2 ? argv[2] : "tests/data/route_audio.mp4";
    const double radio = argc > 3 ? atof(argv[3]) * 1e6 : 12.288e6;
    const double cfo = 4321.0, snr = 24.0;
    FramePlp plp;
    plp.id = 1; plp.fecType = 0; plp.mod = 2; plp.cod = 6;
    Bicm bicm(plpBicm(plp));
    sim::Stream st = sim::buildStream(vp, ap, bicm.kPayload() / 8);
    if (st.bb.empty()) { printf("test files not found\n"); return 1; }

    FrameSetup fs;
    fs.bs.preambleStructure = 0; fs.bs.bsrCoefficient = 8; fs.bs.numSymbols = 4; fs.bs.minTimeToNext = 1;   // the next frame comes after at least 100 ms
    fs.fftCode = 0; fs.guardCode = 1; fs.spPattern = 4; fs.numSymbols = 120; fs.l1DetailMode = 3; fs.sbsNullCells = 16;   // 120 symbols are about 110 ms: longer than the signalled minimum of 100 ms
    const double frameRate = postBootstrapRate(fs.bs);
    const size_t perFrame = 8;   // five frames
    const double period = 0.118;   // frames back to back

    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.f, 1.f);
    std::vector<cf32> wave;   // the whole transmission at the radio's rate
    int frames = 0;
    for (size_t i = 0; i < st.bb.size(); i += perFrame) {
        FramePlp fp = plp;
        for (size_t k = i; k < std::min(st.bb.size(), i + perFrame); k++) fp.bbPackets.push_back(st.bb[k]);
        auto frame = buildFrame(fs, {fp});
        CHECK(!frame.empty(), "frame built");
        auto boot = generateBootstrap(fs.bs);
        auto b = convert(boot, kBootstrapRate, radio), f = convert(frame, frameRate, radio);
        std::vector<cf32> one(b);
        one.insert(one.end(), f.begin(), f.end());
        size_t total = (size_t)(period * radio);
        one.resize(std::max(total, one.size()), cf32(0, 0));
        wave.insert(wave.end(), one.begin(), one.end());
        frames++;
    }
    // some silence first so the first bootstrap is not at a block boundary
    std::vector<cf32> lead((size_t)(0.0377 * radio), cf32(0, 0));
    wave.insert(wave.begin(), lead.begin(), lead.end());
    // channel: echo, carrier offset, noise
    std::vector<cf32> rxw(wave.size(), cf32(0, 0));
    const size_t echo = 12;
    for (size_t i = 0; i < wave.size(); i++) { rxw[i] += wave[i]; if (i >= echo) rxw[i] += wave[i - echo] * cf32(0.15f, -0.1f); }
    double sig = 0; size_t cnt = 0;
    for (size_t i = 0; i < rxw.size(); i++) if (std::norm(rxw[i]) > 1e-9) { sig += std::norm(rxw[i]); cnt++; }
    sig /= std::max<size_t>(1, cnt);
    float sigma = (float)std::sqrt(sig / std::pow(10.0, snr / 10.0) / 2.0);
    for (size_t i = 0; i < rxw.size(); i++) {
        rxw[i] *= cf32((float)std::cos(2 * M_PI * cfo * i / radio), (float)std::sin(2 * M_PI * cfo * i / radio));
        rxw[i] += cf32(g(rng), g(rng)) * sigma;
    }
    printf("  %d frames, %.2f s of signal at %.3f Msamples/s (%zu samples), carrier offset %.0f Hz, SNR %.0f dB\n", frames, rxw.size() / radio, radio / 1e6, rxw.size(), cfo, snr);

    Atsc3Receiver rx;
    rx.selectService(1001);
    std::vector<uint8_t> ts;
    std::thread reader([&] { uint8_t buf[8192]; for (;;) { int k = rx.readTs(buf, sizeof buf); if (k <= 0) break; ts.insert(ts.end(), buf, buf + k); } });
    Atsc3Sync sync(radio, &rx);
    sync.setThreads(3);   // the frames are decoded on worker threads and must still arrive in order
    const size_t chunk = 65536 + 777;   // blocks of a size unrelated to the frame structure
    for (size_t i = 0; i < rxw.size(); i += chunk) sync.push(rxw.data() + i, std::min(chunk, rxw.size() - i));
    sync.flush();
    for (int i = 0; i < 60; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); size_t a = ts.size(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (a == ts.size() && a > 0) break; }
    rx.stop();
    reader.join();
    auto ss = sync.stats();
    auto rs = rx.stats();
    printf("  sync: %ld bootstraps found, %ld frames decoded, estimated offset %.1f Hz; receiver: %ld baseband packets (%ld bad), %ld ROUTE objects, %zu bytes of transport stream\n",
           ss.bootstraps, ss.frames, ss.cfoHz, rs.bbPackets, rs.bbBad, rs.routeObjects, ts.size());
    CHECK(ss.bootstraps == frames && ss.frames == frames, "every frame found and decoded");
    CHECK(std::fabs(ss.cfoHz - cfo) < 30.0, "carrier offset estimate");
    CHECK(rs.bbBad == 0 && rs.bbPackets == (long)st.bb.size(), "every baseband packet");
    long vc = 0, ac = 0;
    bool hevc = false, aac = false;
    CHECK(sim::countTs(ts, vc, ac, hevc, aac) && hevc && aac, "transport stream with HEVC and AAC");
    printf("  transport stream: %ld video frames, %ld audio frames\n", vc, ac);
    CHECK(vc >= 70 && ac >= 130, "nearly all frames came through");
    printf(fails ? "atsc3 stream: FAILED\n" : "atsc3 stream: ok\n");
    return fails ? 1 : 0;
}
