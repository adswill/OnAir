// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The whole ATSC 3.0 chain in simulation, from MP4 fragments to a transport stream:
//   fragments -> ROUTE (LCT objects) -> UDP/IP, with the service list (LLS) -> ALP -> baseband packets -> frames (Preamble, L1, subframe, BICM)
//   -> a channel with an echo and noise -> frame decoder -> ALP -> IP -> LLS and ROUTE -> remuxer -> transport stream.
// The result is read back with libavformat. Needs the fragmented test files: test_atsc3_receiver <video.mp4> <audio.mp4>
#include "atsc3_sim.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(int argc, char** argv) {
    const char* vp = argc > 1 ? argv[1] : "tests/data/route_video.mp4";
    const char* ap = argc > 2 ? argv[2] : "tests/data/route_audio.mp4";
    FramePlp plp;
    plp.id = 1; plp.fecType = 0; plp.mod = 2; plp.cod = 6;
    Bicm bicm(plpBicm(plp));
    CHECK(bicm.ok(), "PLP configuration");
    const int bytes = bicm.kPayload() / 8;
    sim::Stream st0 = sim::buildStream(vp, ap, bytes);
    if (st0.bb.empty()) { printf("test files not found\n"); return 1; }
    auto& bbs = st0.bb;
    printf("  %zu IP packets, %zu bytes of ALP stream, %zu baseband packets of %d bytes\n", st0.ip.size(), st0.alp.size(), bbs.size(), bytes);

    // ---- frames
    FrameSetup fs;
    fs.bs.preambleStructure = 0; fs.bs.bsrCoefficient = 8; fs.bs.numSymbols = 4;
    fs.fftCode = 0; fs.guardCode = 1; fs.spPattern = 4; fs.numSymbols = 40; fs.l1DetailMode = 3; fs.sbsNullCells = 16;
    const size_t perFrame = 24;

    Atsc3Receiver rx;
    CHECK(!rx.selectService(1001), "the service is not known yet");
    std::vector<uint8_t> ts;
    std::thread reader([&] { uint8_t buf[8192]; for (;;) { int k = rx.readTs(buf, sizeof buf); if (k <= 0) break; ts.insert(ts.end(), buf, buf + k); } });

    std::mt19937 rng(5);
    dect2::normal_distribution<float> g(0.f, 1.f);
    int frames = 0, decoded = 0;
    for (size_t i = 0; i < bbs.size(); i += perFrame) {
        FramePlp fp = plp;
        for (size_t k = i; k < std::min(bbs.size(), i + perFrame); k++) fp.bbPackets.push_back(bbs[k]);
        auto tx = buildFrame(fs, {fp});
        CHECK(!tx.empty(), "frame built");
        if (tx.empty()) break;
        std::vector<cf32> r(tx.size() + 200, cf32(0, 0));
        for (size_t s = 0; s < tx.size(); s++) r[s] += tx[s];
        for (size_t s = 0; s + 30 < tx.size(); s++) r[s + 30] += tx[s] * cf32(0.15f, 0.1f);
        double sig = 0;
        for (size_t s = 0; s < tx.size(); s++) sig += std::norm(r[s]);
        sig /= tx.size();
        float sigma = (float)std::sqrt(sig / std::pow(10.0, 26.0 / 10.0) / 2.0);
        for (auto& v : r) v += cf32{g(rng), g(rng)} * sigma;
        decoded += rx.pushFrame(r.data(), r.size(), fs.bs);
        frames++;
    }
    for (int i = 0; i < 60; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); size_t a = ts.size(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (a == ts.size() && a > 0) break; }
    rx.stop();
    reader.join();

    auto st = rx.stats();
    printf("  %d frames (%d decoded): %ld baseband packets (%ld bad), %ld ALP packets, %ld UDP datagrams, %ld LLS tables, %ld ROUTE objects, %zu bytes of transport stream\n",
           frames, decoded, st.bbPackets, st.bbBad, st.alpPackets, st.udp, st.llsTables, st.routeObjects, ts.size());
    CHECK(decoded == frames && st.bbBad == 0, "every frame and every baseband packet decoded");
    auto svcs = rx.services();
    CHECK(svcs.size() == 1 && svcs[0].serviceId == 1001 && svcs[0].shortName == "TEST" && svcs[0].majorChannel == 7, "service list");
    CHECK(rx.selectedService() == 1001 && rx.serviceReady() && rx.components().size() == 2, "service signaling and components");
    CHECK(!rx.mpd().empty(), "MPD received");
    CHECK(st.routeObjects >= 6, "ROUTE objects");
    CHECK(ts.size() > 188 * 20 && ts[0] == 0x47, "transport stream out");

    long vc = 0, ac = 0;
    bool hevc = false, aacOk = false;
    CHECK(sim::countTs(ts, vc, ac, hevc, aacOk), "read back the transport stream");
    CHECK(hevc && aacOk, "HEVC video and AAC audio");
    printf("  transport stream: %ld video frames, %ld audio frames\n", vc, ac);
    CHECK(vc >= 70 && ac >= 130, "nearly all frames came through");

    // ---- transport stream in ALP (A/330 5.4): the reader switches to it only for a run of such packets. Single ones between IP packets
    // (what a damaged baseband packet can look like) are forgotten, bytes and count, however many of them come over time.
    {
        Atsc3Receiver r2;
        auto tsPacket = [](int i) { std::vector<uint8_t> t(188, (uint8_t)i); t[0] = 0x47; t[1] = 0x01; t[2] = 0x00; t[3] = 0x10 | (i & 15); return t; };
        sim::ipStream.clear();
        sim::sendUdp(0xEFFF0909u, 5000, std::vector<uint8_t>(100, 0x55));
        const std::vector<uint8_t> ipPk = sim::ipStream.back();
        AlpPacket ipA; ipA.type = AlpIpv4; ipA.data = ipPk;
        auto bbOf = [](const std::vector<uint8_t>& alp) { return makeBbPacket(1200, alp, 0, -1); };
        std::vector<std::vector<uint8_t>> lone;
        for (int i = 0; i < 40; i++) { lone.push_back(bbOf(alpSingle(ipA))); lone.push_back(bbOf(alpTs(tsPacket(i)))); }
        lone.push_back(bbOf(alpSingle(ipA)));
        r2.pushBbPackets(1, lone);
        uint8_t buf[8192];
        const int k0 = r2.readTsTimed(buf, sizeof buf, 200);
        CHECK(k0 == 0, "40 single transport stream packets between IP packets do not switch the reader");
        std::vector<std::vector<uint8_t>> run;
        std::vector<uint8_t> want;
        for (int i = 0; i < 20; i++) { auto t = tsPacket(100 + i); want.insert(want.end(), t.begin(), t.end()); run.push_back(bbOf(alpTs(t))); }
        r2.pushBbPackets(1, run);
        std::vector<uint8_t> got;
        for (int k; (k = r2.readTsTimed(buf, sizeof buf, 200)) > 0;) got.insert(got.end(), buf, buf + k);
        printf("  transport stream in ALP: %d bytes after 40 single packets, %zu bytes after a run of 20 (%zu sent)\n", k0, got.size(), want.size());
        CHECK(got == want, "a run of 20 switches the reader, and only the run comes out");
        r2.stop();
    }
    printf(fails ? "atsc3 receiver: FAILED\n" : "atsc3 receiver: ok\n");
    return fails ? 1 : 0;
}
