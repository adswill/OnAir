// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PLP selection: a multiplex of two PLPs, the service (with the service list) on one and other traffic on the other. Once the service plays,
// the receiver decodes only the PLP that carries it and the signalling; the transport stream it gives is the same as with every PLP decoded.
// When the service moves to the PLP that is left out, the receiver notices that its packets stopped and decodes every PLP again.
// Also: the link mapping table and the PLPID of the S-TSID are read.
//   test_atsc3_plpselect <video.mp4> <audio.mp4>
#include "atsc3_sim.h"
#include "dect2/atsc3/atsc3_ip.h"
#include "dect2/atsc3/atsc3_route.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

namespace {

// IP packets -> ALP -> baseband packets of `bytes` bytes (as sim::buildStream does)
std::vector<std::vector<uint8_t>> packBb(const std::vector<std::vector<uint8_t>>& ip, int bytes) {
    std::vector<uint8_t> alp;
    std::vector<size_t> starts;
    for (auto& p : ip) {
        AlpPacket a; a.type = AlpIpv4; a.data = p;
        auto w = alpSingle(a);
        starts.push_back(alp.size());
        alp.insert(alp.end(), w.begin(), w.end());
    }
    std::vector<std::vector<uint8_t>> bb;
    size_t pos = 0, next = 0;
    while (pos < alp.size()) {
        const int room = bytes - 2;
        while (next < starts.size() && starts[next] < pos) next++;
        const int pointer = (next < starts.size() && starts[next] < pos + room) ? (int)(starts[next] - pos) : 8191;
        const size_t take = std::min<size_t>(room, alp.size() - pos);
        std::vector<uint8_t> chunk(alp.begin() + pos, alp.begin() + pos + take), pk;
        if ((int)take == room) {
            BbHeader h; h.pointer = pointer; h.twoByteBase = true;
            pk = makeBbHeader(h);
            pk.insert(pk.end(), chunk.begin(), chunk.end());
        } else pk = makeBbPacket(bytes, chunk, pointer == 8191 ? -1 : pointer, -1);
        bb.push_back(pk);
        pos += take;
    }
    return bb;
}

// A datagram of other traffic: to a flow of no service (`dst`, port 5000)
std::vector<uint8_t> otherDatagram(uint32_t dst, uint32_t& seed, int id) {
    std::vector<uint8_t> pl(960);
    for (auto& b : pl) { seed = seed * 1103515245u + 12345u; b = (uint8_t)(seed >> 24); }
    return makeUdpPackets(0x0A010109u, dst, 4000, 5000, pl, id & 0xFFFF)[0];
}

// Baseband packets that carry nothing, up to a multiple of `n`
void padBb(std::vector<std::vector<uint8_t>>& bb, size_t n, int bytes) {
    while (bb.size() % n) bb.push_back(makeBbPacket(bytes, {}, -1, -1));
}

// Other traffic in `n` baseband packets; its last ALP packet ends in them
std::vector<std::vector<uint8_t>> otherTraffic(size_t n, int bytes, uint32_t dst, uint32_t seed) {
    std::vector<std::vector<uint8_t>> ip;
    while ((ip.size() + 1) * 1000 < (n - 1) * (size_t)(bytes - 2)) ip.push_back(otherDatagram(dst, seed, (int)ip.size()));
    auto bb = packBb(ip, bytes);
    padBb(bb, n, bytes);
    return bb;
}

struct Run {
    std::vector<uint8_t> ts;
    int frames = 0, decoded = 0;
    int otherDecoded = 0, otherSkipped = 0, serviceSkipped = 0;   // frames in which each PLP was decoded or left out
    int otherDecodedAtEnd = 0;                                   // ... in the last 20 frames
    long objectsAtEnd = 0;                                       // ROUTE objects of the service delivered in the last 8 frames
    ReceiverStats st;
    uint64_t mask = 0;
    bool ready = false;
};

// Each frame: `perFrame` baseband packets of the service's stream on one PLP and as many of other traffic on the other. `moveAt`: from that
// frame on, the service's stream is on PLP 1 and the other traffic on PLP 0 (-1: never).
Run run(const std::vector<std::vector<uint8_t>>& svc, const std::vector<std::vector<uint8_t>>& other, bool allPlps, int moveAt, size_t perFrame,
        const FramePlp& plp0, const FramePlp& plp1, const FrameSetup& fs) {
    Run out;
    Atsc3Receiver rx;
    if (allPlps) rx.setDecodeAllPlps(true);
    rx.selectService(1001);
    std::atomic<size_t> tsBytes{0};   // out.ts itself is read only after the reader has joined
    std::thread reader([&] { uint8_t buf[8192]; for (;;) { int k = rx.readTs(buf, sizeof buf); if (k <= 0) break; out.ts.insert(out.ts.end(), buf, buf + k); tsBytes.store(out.ts.size()); } });
    const size_t nFrames = svc.size() / perFrame;
    long objects8 = 0;
    for (size_t f = 0; f < nFrames; f++) {
        FramePlp a = plp0, b = plp1;
        const bool moved = moveAt >= 0 && (int)f >= moveAt;
        FramePlp& s = moved ? b : a;
        FramePlp& o = moved ? a : b;
        for (size_t k = 0; k < perFrame; k++) { s.bbPackets.push_back(svc[f * perFrame + k]); o.bbPackets.push_back(other[f * perFrame + k]); }
        if (f + 8 == nFrames) objects8 = rx.stats().objectsDelivered;
        auto tx = buildFrame(fs, {a, b});
        if (tx.empty()) { CHECK(false, "frame built"); break; }
        out.decoded += rx.pushFrame(tx.data(), tx.size(), fs.bs);
        out.frames++;
        const FrameInfo fi = rx.frameInfo();
        const int otherId = moved ? 0 : 1;
        for (auto& p : fi.plps) {
            // a PLP left out shows no blocks in the frame
            if (p.id == otherId) {
                if (p.blocks > 0) { out.otherDecoded++; if (f + 20 >= nFrames) out.otherDecodedAtEnd++; }
                else out.otherSkipped++;
            } else if (p.blocks == 0) out.serviceSkipped++;
        }
    }
    for (int i = 0; i < 60; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); const size_t s = tsBytes.load(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (s == tsBytes.load() && s > 0) break; }
    rx.stop();
    reader.join();
    out.st = rx.stats();
    out.objectsAtEnd = out.st.objectsDelivered - objects8;
    out.mask = rx.plpDecodeMask();
    out.ready = rx.serviceReady();
    return out;
}

void lmtAndStsid() {
    // two PLPs: PLP 0 with one flow (with a SID), PLP 5 with two flows (the second compressed, with a context id)
    const std::vector<uint8_t> lmt = {
        (uint8_t)((1 << 2) | 3),
        (uint8_t)(0 << 2 | 3), 1,
        239, 255, 5, 1, 239, 255, 5, 1, 0x0F, 0xA0, 0x0B, 0xB8, 0x80, 7,
        (uint8_t)(5 << 2 | 3), 2,
        10, 1, 1, 9, 239, 255, 9, 9, 0x0F, 0xA0, 0x13, 0x88, 0x00,
        10, 1, 1, 9, 239, 255, 9, 10, 0x0F, 0xA0, 0x13, 0x89, 0x40, 3,
    };
    std::vector<LmtFlow> f;
    CHECK(parseLmt(lmt.data(), lmt.size(), f) && f.size() == 3, "link mapping table parsed");
    if (f.size() == 3) {
        CHECK(f[0].plp == 0 && f[0].dstIp == 0xEFFF0501u && f[0].dstPort == 3000 && f[0].srcPort == 4000 && f[0].sid == 7 && f[0].contextId == -1, "LMT flow 1");
        CHECK(f[1].plp == 5 && f[1].srcIp == 0x0A010109u && f[1].dstIp == 0xEFFF0909u && f[1].dstPort == 5000 && f[1].sid == -1, "LMT flow 2");
        CHECK(f[2].plp == 5 && f[2].dstPort == 5001 && f[2].contextId == 3 && f[2].sid == -1, "LMT flow 3");
    }
    CHECK(!parseLmt(lmt.data(), lmt.size() - 1, f), "a cut link mapping table is refused");
    Stsid st;
    CHECK(parseStsid("<S-TSID><RS dIpAddr=\"239.255.5.1\" dPort=\"3000\" PLPID=\"2\"><LS tsi=\"10\"/><LS tsi=\"20\" PLPID=\"3\"/></RS>"
                     "<RS dIpAddr=\"239.255.5.2\" dPort=\"3002\"><LS tsi=\"30\"/></RS></S-TSID>", st) && st.sessions.size() == 2, "S-TSID parsed");
    if (st.sessions.size() == 2 && st.sessions[0].channels.size() == 2) {
        CHECK(st.sessions[0].plpId == 2 && st.sessions[0].channels[0].plpId == -1 && st.sessions[0].channels[1].plpId == 3, "S-TSID PLPID of a session and of an LCT channel");
        CHECK(st.sessions[1].plpId == -1, "S-TSID without PLPID");
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* vp = argc > 1 ? argv[1] : "tests/data/route_video.mp4";
    const char* ap = argc > 2 ? argv[2] : "tests/data/route_audio.mp4";
    lmtAndStsid();

    FramePlp plp0, plp1;
    plp0.id = 0; plp0.fecType = 0; plp0.mod = 2; plp0.cod = 6;
    plp1 = plp0; plp1.id = 1;
    Bicm bicm(plpBicm(plp0));
    CHECK(bicm.ok(), "PLP configuration");
    const int bytes = bicm.kPayload() / 8;
    sim::Stream st0 = sim::buildStream(vp, ap, bytes);
    if (st0.bb.empty()) { printf("test files not found\n"); return 1; }
    // the service's IP packets with three datagrams of other traffic after each, two baseband packets per frame of about 0.2 s: the service
    // lasts long enough for the receiver to learn its PLP (2 s), for that choice to reach the frames (Atsc3Receiver::kPlanLag frames later) and
    // then to leave the other one out
    const size_t perFrame = 2;
    std::vector<std::vector<uint8_t>> mixed;
    uint32_t seed = 777;
    for (auto& p : st0.ip) { mixed.push_back(p); for (int k = 0; k < 3; k++) mixed.push_back(otherDatagram(0xEFFF0808u, seed, (int)mixed.size())); }
    auto svc = packBb(mixed, bytes);
    padBb(svc, perFrame, bytes);
    const auto other = otherTraffic(svc.size(), bytes, 0xEFFF0909u, 12345);
    FrameSetup fs;
    fs.bs.preambleStructure = 0; fs.bs.bsrCoefficient = 8; fs.bs.numSymbols = 4;
    fs.fftCode = 0; fs.guardCode = 1; fs.spPattern = 4; fs.numSymbols = 200; fs.l1DetailMode = 3; fs.sbsNullCells = 16;
    printf("  %zu baseband packets of the service's PLP, %zu per frame\n", svc.size(), perFrame);

    const Run all = run(svc, other, true, -1, perFrame, plp0, plp1, fs);
    const Run sel = run(svc, other, false, -1, perFrame, plp0, plp1, fs);
    for (const Run* r : {&all, &sel})
        printf("  %s: %d frames (%d decoded), PLP 1 decoded in %d and left out in %d, %ld PLP-frames left out, %ld/%ld switches, %zu bytes of TS\n",
               r == &all ? "every PLP" : "selection", r->frames, r->decoded, r->otherDecoded, r->otherSkipped, r->st.plpsLeftOut, r->st.plpSelections,
               r->st.plpRelearns, r->ts.size());
    CHECK(all.decoded == all.frames && sel.decoded == sel.frames, "every frame decoded");
    CHECK(all.otherSkipped == 0 && all.st.plpsLeftOut == 0 && all.mask == ~0ull, "with every PLP asked for, none is left out");
    CHECK(all.ready && sel.ready, "the service plays");
    // the service's PLP is never left out; the other one is, once the service plays, until the end
    CHECK(sel.serviceSkipped == 0, "the service's PLP is always decoded");
    CHECK(sel.otherSkipped > sel.frames / 2 && sel.otherDecodedAtEnd == 0, "the PLP without the service is left out once the service plays");
    CHECK(sel.mask == 1 && sel.st.plpSelections == 1 && sel.st.plpRelearns == 0, "one switch, to PLP 0 alone");
    CHECK(sel.st.bbBad == 0 && all.st.bbBad == 0, "no baseband packet lost");
    CHECK(!all.ts.empty() && sel.ts == all.ts, "the same transport stream as with every PLP decoded");
    long vc = 0, ac = 0, va = 0, aa = 0;
    bool hevc = false, aac = false;
    CHECK(sim::countTs(all.ts, va, aa, hevc, aac) && sim::countTs(sel.ts, vc, ac, hevc, aac), "read back the transport streams");
    printf("  video and audio frames: every PLP %ld and %ld, selection %ld and %ld\n", va, aa, vc, ac);
    CHECK(vc >= 70 && vc == va && ac == aa, "the service's video comes through, the same frames as with every PLP decoded");

    // the service moves to PLP 1 while PLP 1 is left out: its packets stop, PLP 0 still decodes cleanly, so every PLP is decoded again. Both
    // streams are cut at an ALP packet boundary where they change PLPs, as a sender would move them.
    const size_t half = mixed.size() / 2;
    auto svcMove = packBb(std::vector<std::vector<uint8_t>>(mixed.begin(), mixed.begin() + (long)half), bytes);
    padBb(svcMove, perFrame, bytes);
    const int moveAt = (int)(svcMove.size() / perFrame);
    auto rest = packBb(std::vector<std::vector<uint8_t>>(mixed.begin() + (long)half, mixed.end()), bytes);
    padBb(rest, perFrame, bytes);
    svcMove.insert(svcMove.end(), rest.begin(), rest.end());
    auto otherMove = otherTraffic((size_t)moveAt * perFrame, bytes, 0xEFFF0909u, 12345);
    const auto otherRest = otherTraffic(svcMove.size() - otherMove.size(), bytes, 0xEFFF0909u, 999);
    otherMove.insert(otherMove.end(), otherRest.begin(), otherRest.end());
    const Run mvAll = run(svcMove, otherMove, true, moveAt, perFrame, plp0, plp1, fs);
    const Run mv = run(svcMove, otherMove, false, moveAt, perFrame, plp0, plp1, fs);
    printf("  moved, every PLP: %ld ROUTE objects, %ld in the last 8 frames\n", mvAll.st.objectsDelivered, mvAll.objectsAtEnd);
    long vm = 0, am = 0;
    CHECK(sim::countTs(mv.ts, vm, am, hevc, aac), "read back the transport stream after the move");
    printf("  moved at frame %d, selection: %ld ROUTE objects, %ld switches to a subset, %ld back to every PLP, %ld video frames, %ld audio frames, %ld ROUTE objects in the last 8 frames, PLP mask at the end %llx\n",
           moveAt, mv.st.objectsDelivered, mv.st.plpSelections, mv.st.plpRelearns, vm, am, mv.objectsAtEnd, (unsigned long long)mv.mask);
    CHECK(mv.st.plpSelections >= 1 && mv.st.plpRelearns >= 1, "the move was noticed and every PLP decoded again");
    CHECK((mv.mask >> 1) & 1, "the service's new PLP is decoded at the end");
    CHECK(mvAll.objectsAtEnd > 0 && mv.objectsAtEnd > 0, "the service's objects come again after the move");

    // only one of the service's flows moves: a second ROUTE session of the service (239.255.8.8:5000, the datagrams between the service's
    // packets) goes to PLP 1 while the signalling and media session stays on PLP 0 (with as much traffic of no service in place of the moved
    // flow, two baseband packets per frame, so that each half lasts about 7 s). Its packets stop although the service's others keep coming
    // and PLP 0 decodes cleanly, so every PLP is decoded again and PLP 1 is kept from then on.
    {
        sim::Stream st1 = sim::buildStream(vp, ap, bytes, "<RS dIpAddr=\"239.255.8.8\" dPort=\"5000\"><LS tsi=\"1\"/></RS>");
        std::vector<std::vector<uint8_t>> head, svcTail, flowTail;
        uint32_t seed1 = 4242;
        const size_t halfIp = st1.ip.size() / 2;
        for (size_t i = 0; i < st1.ip.size(); i++) {
            if (i < halfIp) head.push_back(st1.ip[i]); else svcTail.push_back(st1.ip[i]);
            for (int k = 0; k < 3; k++) {
                auto d = otherDatagram(0xEFFF0808u, seed1, (int)(i * 3 + k));
                if (i < halfIp) head.push_back(d);
                else { flowTail.push_back(d); svcTail.push_back(otherDatagram(0xEFFF0707u, seed1, (int)(i * 3 + k))); }
            }
        }
        const size_t pf = 2;
        auto p0 = packBb(head, bytes);
        padBb(p0, pf, bytes);
        const int splitAt = (int)(p0.size() / pf);
        auto t0 = packBb(svcTail, bytes);
        padBb(t0, pf, bytes);
        p0.insert(p0.end(), t0.begin(), t0.end());
        auto p1 = otherTraffic((size_t)splitAt * pf, bytes, 0xEFFF0909u, 31);
        auto t1 = packBb(flowTail, bytes);
        p1.insert(p1.end(), t1.begin(), t1.end());
        while (p1.size() < p0.size()) p1.push_back(makeBbPacket(bytes, {}, -1, -1));
        const Run pmAll = run(p0, p1, true, -1, pf, plp0, plp1, fs);
        const Run pm = run(p0, p1, false, -1, pf, plp0, plp1, fs);
        printf("  one flow moved at frame %d of %d: every PLP %ld UDP datagrams, selection %ld; %ld switches to a subset, %ld back to every PLP, PLP 1 decoded in %d frames, %d of the last 20, mask at the end %llx\n",
               splitAt, pm.frames, pmAll.st.udp, pm.st.udp, pm.st.plpSelections, pm.st.plpRelearns, pm.otherDecoded, pm.otherDecodedAtEnd,
               (unsigned long long)pm.mask);
        CHECK(pm.st.plpSelections >= 1 && pm.st.plpRelearns >= 1, "the move of one flow was noticed and every PLP decoded again");
        CHECK(((pm.mask >> 1) & 1) && pm.otherDecodedAtEnd == 20, "the PLP the flow moved to is decoded at the end");
        CHECK(pm.ts == pmAll.ts, "the same transport stream as with every PLP decoded when one flow moves");
    }

    std::fputs(fails ? "atsc3 PLP selection: FAILED\n" : "atsc3 PLP selection: ok\n", stdout);
    return fails ? 1 : 0;
}
