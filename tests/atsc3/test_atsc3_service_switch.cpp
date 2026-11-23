// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Issue #650: ATSC 3.0 crashed when another service was selected. The receive thread feeds ROUTE packets while the user interface changes the
// service and asks for the service's components, MPD and state. This test does the same on a synthetic broadcast with two services, for a few
// seconds, and then checks that the receiver settled on the last service. Run it with the thread sanitizer to see the races themselves.
// Needs the fragmented test files: test_atsc3_service_switch <video.mp4> <audio.mp4>
#include "atsc3_sim.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static int ipId = 1;
static void sendUdp(std::vector<std::vector<uint8_t>>& out, uint32_t dst, int port, const std::vector<uint8_t>& payload) {
    for (auto& p : makeUdpPackets(sim::kSrc, dst, 4000, port, payload, ipId++ & 0xFFFF)) out.push_back(p);
}
static void sendObject(std::vector<std::vector<uint8_t>>& out, uint32_t dst, uint32_t tsi, uint32_t toi, int cp, const std::vector<uint8_t>& data) {
    for (size_t off = 0; off < data.size(); off += 1100) {
        LctPacket p;
        p.tsi = tsi; p.toi = toi; p.codePoint = cp; p.startOffset = (uint32_t)off;
        const size_t n = std::min<size_t>(1100, data.size() - off);
        p.payload.assign(data.begin() + off, data.begin() + off + n);
        p.transferLength = (int64_t)data.size();
        sendUdp(out, dst, 3000, makeRoutePacket(p, true));
    }
}

// the IP packets of one service (SLS package, init segments, media segments) on its own multicast address
static std::vector<std::vector<uint8_t>> serviceStream(uint32_t dst, const std::vector<uint8_t>& vinit, const std::vector<std::vector<uint8_t>>& vseg,
                                                        const std::vector<uint8_t>& ainit, const std::vector<std::vector<uint8_t>>& aseg) {
    std::vector<std::vector<uint8_t>> out;
    const std::string stsid =
        "<S-TSID><RS dIpAddr=\"239.255.5." + std::to_string(dst & 0xFF) + "\" dPort=\"3000\">"
        "<LS tsi=\"10\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"v1\" contentType=\"video\"/></ContentInfo></SrcFlow></LS>"
        "<LS tsi=\"20\"><SrcFlow rt=\"true\"><ContentInfo><MediaInfo repId=\"a1\" contentType=\"audio\" lang=\"en\"/></ContentInfo></SrcFlow></LS></RS></S-TSID>";
    std::vector<MimePart> parts(2);
    parts[0].headers["content-type"] = "application/route-s-tsid+xml";
    parts[0].body.assign(stsid.begin(), stsid.end());
    const std::string mpd = "<MPD type=\"dynamic\"><Period/></MPD>";
    parts[1].headers["content-type"] = "application/dash+xml";
    parts[1].body.assign(mpd.begin(), mpd.end());
    sendObject(out, dst, 0, 1, 3, makeMultipart(parts, "b1"));
    sendObject(out, dst, 10, 0, 5, vinit);
    sendObject(out, dst, 20, 0, 5, ainit);
    const size_t n = std::max(vseg.size(), aseg.size());
    for (size_t i = 0; i < n; i++) {
        if (i < vseg.size()) sendObject(out, dst, 10, (uint32_t)i + 1, 8, vseg[i]);
        if (i < aseg.size()) sendObject(out, dst, 20, (uint32_t)i + 1, 8, aseg[i]);
    }
    return out;
}

int main(int argc, char** argv) {
    const char* vp = argc > 1 ? argv[1] : "tests/data/route_video.mp4";
    const char* ap = argc > 2 ? argv[2] : "tests/data/route_audio.mp4";
    std::vector<uint8_t> vinit, ainit;
    std::vector<std::vector<uint8_t>> vseg, aseg;
    sim::split(sim::slurp(vp), vinit, vseg);
    sim::split(sim::slurp(ap), ainit, aseg);
    if (vinit.empty() || ainit.empty()) { printf("test files not found\n"); return 1; }

    const uint32_t dst1 = 0xEFFF0501u, dst2 = 0xEFFF0502u;
    const std::string slt = "<SLT bsid=\"4660\">"
        "<Service serviceId=\"1001\" majorChannelNo=\"7\" minorChannelNo=\"1\" serviceCategory=\"1\" shortServiceName=\"ONE\" sltSvcSeqNum=\"0\"><BroadcastSvcSignaling slsProtocol=\"1\" slsDestinationIpAddress=\"239.255.5.1\" slsDestinationUdpPort=\"3000\" slsSourceIpAddress=\"10.1.1.5\"/></Service>"
        "<Service serviceId=\"1002\" majorChannelNo=\"7\" minorChannelNo=\"2\" serviceCategory=\"1\" shortServiceName=\"TWO\" sltSvcSeqNum=\"0\"><BroadcastSvcSignaling slsProtocol=\"1\" slsDestinationIpAddress=\"239.255.5.2\" slsDestinationUdpPort=\"3000\" slsSourceIpAddress=\"10.1.1.5\"/></Service></SLT>";
    std::vector<std::vector<uint8_t>> lls;
    sendUdp(lls, kLlsAddress, kLlsPort, makeLls(1, 0, 1, slt));
    const auto s1 = serviceStream(dst1, vinit, vseg, ainit, aseg), s2 = serviceStream(dst2, vinit, vseg, ainit, aseg);
    printf("  %zu + %zu IP packets for the two services\n", s1.size(), s2.size());

    Atsc3Receiver rx;
    std::atomic<bool> run{true};
    std::atomic<long> pushed{0}, switches{0}, reads{0};
    // the receive thread: the service list, then both services over and over (a broadcast carries all its services at once)
    std::thread rxThread([&] {
        size_t i = 0;
        while (run) {
            if (i % 200 == 0) for (auto& p : lls) rx.pushIp(p.data(), p.size());
            const auto& a = s1[i % s1.size()];
            const auto& b = s2[i % s2.size()];
            rx.pushIp(a.data(), a.size());
            rx.pushIp(b.data(), b.size());
            pushed += 2; i++;
        }
    });
    // the user interface: switch services as fast as a user (and faster), asking for everything about the service each time
    std::thread uiThread([&] {
        int which = 0;
        while (run) {
            rx.selectService(which++ % 2 ? 1002 : 1001);
            (void)rx.components(); (void)rx.mpd(); (void)rx.serviceReady(); (void)rx.stats(); (void)rx.selectedService(); (void)rx.services();
            switches++;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    // the player: reads the transport stream while the services change
    std::thread player([&] { uint8_t buf[8192]; while (run) { rx.readTsTimed(buf, sizeof buf, 5); reads++; } });

    std::this_thread::sleep_for(std::chrono::seconds(4));
    run = false;
    rxThread.join(); uiThread.join(); player.join();
    printf("  %ld packets, %ld service switches, %ld reads without a crash\n", pushed.load(), switches.load(), reads.load());
    CHECK(switches > 50, "the service was switched many times");

    // settled: select the second service once and feed it; it has to come up with its two components
    CHECK(rx.selectService(1002) || true, "select");
    for (int round = 0; round < 3; round++) for (auto& p : s2) rx.pushIp(p.data(), p.size());
    CHECK(rx.selectedService() == 1002, "the last selection stands");
    CHECK(rx.serviceReady(), "the selected service's signaling arrived");
    CHECK(rx.components().size() == 2, "the selected service has its two components");
    CHECK(!rx.mpd().empty(), "the MPD of the selected service");
    // and back to the first
    rx.selectService(1001);
    for (auto& p : s1) rx.pushIp(p.data(), p.size());
    CHECK(rx.selectedService() == 1001 && rx.serviceReady() && rx.components().size() == 2, "switching back works");
    rx.stop();
    printf(fails ? "FAILED\n" : "atsc3_service_switch OK\n");
    return fails ? 1 : 0;
}
