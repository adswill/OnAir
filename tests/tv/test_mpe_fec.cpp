// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-H link layer (EN 301 192 clauses 7 and 9): MPE, time slicing and MPE-FEC.
//   1. RS(255,191): random words with errors and erasures up to the limit 2e + s <= 64 corrected, one beyond refused or left alone
//   2. MpeEncoder -> damaged transport packets (a lost packet loses its whole section) -> MpeDemux: every frame size (256 ... 1024 rows),
//      punctured RS columns, a packet loss sweep up to where MPE-FEC gives up; every datagram compared with the one sent, the real-time parameters (delta-t) read back
//   3. plain MPE (MAC addresses, no real-time parameters) and time slicing without MPE-FEC
//   4. the whole DVB-H chain: MPE-FEC transport stream -> DVB-T 4K in-depth generator -> noise and offset -> DVB-T receiver -> MpeDemux
//   5. the IP output: MPE-FEC with packet loss -> MpeDemux -> IpOut, the pcap file read back record by record, the UDP forward received
//      on a local socket (payload mode and whole packets), IPv4 fragments put back together before the payload goes out, bad input refused
// All signals are simulated: no recording of a DVB-H transmission was available.
#include "dect2/tv/mpe_fec.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/t2/t2.h"
#include "dect2/tv/ip_out.h"
#include "dect2/dvbs/dvbs_ip.h"
#include "../../core/src/net/netcompat.h"
#include "../../core/src/net/tcp_client.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <random>
#include <set>
#include <vector>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
static void check(bool c, const char* name, const char* what) { if (!c) { jprintf("FAIL: %s: %s\n", name, what); fails++; } }

static void testRs() {
    std::mt19937 rng(1);
    int ok = 0, tried = 0, refused = 0;
    for (int trial = 0; trial < 300; trial++) {
        uint8_t msg[191], word[255], par[64];
        for (auto& b : msg) b = (uint8_t)rng();
        mpe::rsEncode255(msg, par);
        std::memcpy(word, msg, 191); std::memcpy(word + 191, par, 64);
        uint8_t ref[255]; std::memcpy(ref, word, 255);
        const int s = (int)(rng() % 65);              // erasures
        const int e = (64 - s) / 2 - (trial % 3 == 0 ? 0 : (int)(rng() % 3));   // errors within the limit
        std::vector<int> pos(255);
        for (int i = 0; i < 255; i++) pos[i] = i;
        std::shuffle(pos.begin(), pos.end(), rng);
        std::vector<int> er(pos.begin(), pos.begin() + s);
        for (int i = 0; i < s; i++) word[pos[i]] = (uint8_t)rng();
        for (int i = s; i < s + std::max(0, e); i++) word[pos[i]] ^= (uint8_t)(1 + rng() % 255);
        const int r = mpe::rsDecode255(word, er.data(), (int)er.size());
        tried++;
        if (r >= 0 && std::memcmp(word, ref, 255) == 0) ok++;
    }
    jprintf("  RS(255,191): %d / %d words within 2e + s <= 64 corrected\n", ok, tried);
    check(ok == tried, "rs", "every word within the limit corrected");
    for (int trial = 0; trial < 60; trial++) {   // 33 errors and no erasures: beyond the code
        uint8_t msg[191], word[255], par[64];
        for (auto& b : msg) b = (uint8_t)rng();
        mpe::rsEncode255(msg, par);
        std::memcpy(word, msg, 191); std::memcpy(word + 191, par, 64);
        uint8_t ref[255]; std::memcpy(ref, word, 255);
        std::set<int> hit;
        while (hit.size() < 33) hit.insert((int)(rng() % 255));
        for (int p : hit) word[p] ^= (uint8_t)(1 + rng() % 255);
        const int r = mpe::rsDecode255(word, nullptr, 0);
        if (r < 0) refused++;
    }
    jprintf("  RS(255,191): %d / 60 words with 33 errors refused\n", refused);
    check(refused >= 50, "rs", "words beyond the limit are refused (nearly always)");
}

struct Run { long sent = 0, good = 0, bad = 0, wrong = 0; mpe::Stats st; };

// frames of random-sized datagrams, packet loss `loss`, optional real-time hint
static Run runTs(const mpe::EncoderConfig& c, int frames, double loss, unsigned seed, bool hint, size_t dgMax = 1400) {
    mpe::MpeEncoder enc(c);
    mpe::MpeDemux dmx;
    dmx.setRealTimeParams(hint);
    Run r;
    std::set<uint32_t> want;
    dmx.setDatagramCallback([&](const uint8_t* d, size_t n, bool rel, int pid) {
        uint32_t s = 0;
        if (pid == c.pid && rel && mpe::checkTestDatagram(d, n, &s)) { if (want.erase(s)) r.good++; else r.wrong++; }
        else r.bad++;
    });
    std::mt19937 rng(seed);
    uint32_t seq = 0;
    for (int f = 0; f < frames; f++) {
        std::vector<std::vector<uint8_t>> dg;
        size_t used = 0;
        const size_t cap = (size_t)191 * c.rows * 9 / 10;
        while (true) {
            const size_t len = 60 + rng() % (dgMax - 60);
            if (used + len > cap) break;
            dg.push_back(mpe::testDatagram(seq, len)); want.insert(seq); seq++; used += len;
        }
        r.sent += (long)dg.size();
        std::vector<uint8_t> ts;
        enc.frame(dg, ts);
        // some unrelated traffic in between (another PID)
        for (size_t i = 0; i < ts.size(); i += 188) {
            std::vector<uint8_t> pk(ts.begin() + (long)i, ts.begin() + (long)i + 188);
            if (std::uniform_real_distribution<double>(0, 1)(rng) < loss) pk[1] |= 0x80;   // transport_error_indicator: the RS of the TS failed
            dmx.feed(pk.data(), 1);
            if (i % (188 * 20) == 0) { uint8_t n[188]; std::memset(n, 0xFF, 188); n[0] = 0x47; n[1] = 0x01; n[2] = 0x00; n[3] = 0x10; dmx.feed(n, 1); }
        }
    }
    dmx.flush();
    r.st = dmx.stats();
    return r;
}

// a local UDP receiver on 127.0.0.1 (any free port)
struct UdpSink {
    sock_t s = kBadSock;
    int port = 0;
    UdpSink() {
        netInit();
        s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kBadSock) return;
        int big = 8 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&big, sizeof big);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = 0; inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (bind(s, (const sockaddr*)&a, sizeof a) != 0) { sockClose(s); s = kBadSock; return; }
        socklen_t l = sizeof a;
        getsockname(s, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        sockSetNonBlocking(s, true);
    }
    ~UdpSink() { if (s != kBadSock) sockClose(s); }
    std::vector<std::vector<uint8_t>> take() {
        std::vector<std::vector<uint8_t>> out;
        uint8_t b[70000];
        for (int idle = 0; idle < 50;) {
            const long long n = sockRecv(s, b, sizeof b);
            if (n > 0) { out.emplace_back(b, b + n); idle = 0; } else { idle++; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        }
        return out;
    }
};

static std::vector<std::vector<uint8_t>> readPcap(const std::string& path, uint32_t* linkType) {
    std::vector<std::vector<uint8_t>> recs;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return recs;
    uint8_t h[24];
    if (fread(h, 1, 24, f) == 24) {
        *linkType = (uint32_t)h[20] | (uint32_t)h[21] << 8 | (uint32_t)h[22] << 16 | (uint32_t)h[23] << 24;
        uint8_t r[16];
        while (fread(r, 1, 16, f) == 16) {
            const uint32_t incl = (uint32_t)r[8] | (uint32_t)r[9] << 8 | (uint32_t)r[10] << 16 | (uint32_t)r[11] << 24;
            std::vector<uint8_t> d(incl);
            if (fread(d.data(), 1, incl, f) != incl) break;
            recs.push_back(std::move(d));
        }
    }
    fclose(f);
    return recs;
}

static void testIpOut() {
    const std::string path = (std::filesystem::temp_directory_path() / ("onair_test_ip_out_" + std::to_string((unsigned long)std::random_device{}()) + ".pcap")).string();
    UdpSink sink;
    check(sink.s != kBadSock, "ip out", "local UDP socket");
    IpOut io;
    check(!io.active(), "ip out", "off by default");
    IpOutConfig c; c.pcap = true; c.pcapPath = path; c.udp = true; c.udpHost = "127.0.0.1"; c.udpPort = sink.port;
    check(io.configure(c) && io.active(), "ip out", "pcap and UDP switched on");
    // MPE-FEC frames with 3 % of the transport packets lost: what MPE-FEC repairs comes out, nothing damaged does
    mpe::EncoderConfig ec; ec.rows = 512; ec.pid = 0x321;
    mpe::MpeEncoder enc(ec);
    mpe::MpeDemux dmx;
    dmx.setRealTimeParams(true);
    std::vector<std::vector<uint8_t>> delivered;
    dmx.setDatagramCallback([&](const uint8_t* d, size_t n, bool rel, int) { if (rel) { delivered.emplace_back(d, d + n); io.packet(d, n); } });
    std::mt19937 rng(21);
    uint32_t seq = 0;
    for (int f = 0; f < 4; f++) {
        std::vector<std::vector<uint8_t>> dg;
        size_t used = 0;
        while (true) { const size_t len = 40 + rng() % 1400; if (used + len > (size_t)191 * ec.rows * 8 / 10) break; dg.push_back(mpe::testDatagram(seq++, len)); used += len; }
        std::vector<uint8_t> ts;
        enc.frame(dg, ts);
        for (size_t i = 0; i < ts.size(); i += 188) { uint8_t pk[188]; memcpy(pk, ts.data() + i, 188); if (rng() % 100 < 3) pk[1] |= 0x80; dmx.feed(pk, 1); }
    }
    dmx.flush();
    const IpOutStats st = io.stats();
    const auto udp = sink.take();
    io.close();
    uint32_t link = 0;
    const auto recs = readPcap(path, &link);
    size_t same = 0, udpSame = 0;
    for (size_t i = 0; i < recs.size() && i < delivered.size(); i++) same += recs[i] == delivered[i];
    for (size_t i = 0; i < udp.size() && i < delivered.size(); i++) udpSame += udp[i].size() + 28 == delivered[i].size() && memcmp(udp[i].data(), delivered[i].data() + 28, udp[i].size()) == 0;
    jprintf("  IP output: %u datagrams sent, %zu delivered by MPE-FEC, pcap %zu records (%zu identical, link type %u), UDP %zu payloads (%zu identical), stats %llu/%llu/%llu\n", seq, delivered.size(), recs.size(), same,
            link, udp.size(), udpSame, (unsigned long long)st.packets, (unsigned long long)st.written, (unsigned long long)st.forwarded);
    check(delivered.size() >= seq * 95 / 100, "ip out", "MPE-FEC delivers nearly every datagram at 3 % loss");
    check(recs.size() == delivered.size() && same == delivered.size() && link == 101, "ip out", "pcap holds every datagram, byte for byte (link type RAW)");
    check(udp.size() == delivered.size() && udpSame == delivered.size(), "ip out", "UDP forward: every UDP payload, in order");
    std::remove(path.c_str());
    // whole packets, IPv4 fragments, invalid input
    IpOutConfig w; w.udp = true; w.udpHost = "127.0.0.1"; w.udpPort = sink.port; w.wholePackets = true;
    check(io.configure(w), "ip out", "whole-packet forward on");
    const std::vector<uint8_t> big = mpe::testDatagram(77, 3000);
    io.packet(big.data(), big.size());
    const uint8_t junk[40] = {0x45, 0, 0, 40};
    io.packet(junk, sizeof junk);
    auto u2 = sink.take();
    check(u2.size() == 1 && u2[0] == big, "ip out", "whole IP packet forwarded, a bad header dropped");
    check(io.stats().invalid == 1, "ip out", "the bad packet is counted");
    IpOutConfig pl = w; pl.wholePackets = false;
    io.configure(pl);
    const auto frags = dvbsip::fragmentIpv4(big, 1400);
    for (const auto& fr : frags) io.packet(fr.data(), fr.size());
    auto u3 = sink.take();
    check(frags.size() >= 3 && u3.size() == 1 && u3[0].size() == big.size() - 28 && memcmp(u3[0].data(), big.data() + 28, u3[0].size()) == 0, "ip out", "fragments reassembled before the payload is forwarded");
    IpOutConfig bad; bad.udp = true; bad.udpHost = "not an address";
    check(!io.configure(bad) && !io.error().empty() && !io.active(), "ip out", "a bad address is refused with a message");
    IpOutConfig badFile; badFile.pcap = true; badFile.pcapPath = "/nonexistent-dir-onair/x.pcap";
    check(!io.configure(badFile) && !io.active(), "ip out", "an unwritable pcap path is refused");
}

int main() {
    testRs();
    testIpOut();
    testjobs::Jobs jobs;
    // 2. every frame size, losses the code can carry: all datagrams back
    for (int fs = 0; fs < 4; fs++)
        jobs.add([fs] {
            mpe::EncoderConfig c; c.rows = mpe::rowsOfFrameSize(fs); c.pid = 0x321; c.deltaTms = 500;
            const Run r = runTs(c, 3, 0.02, 3 + fs, true);
            char nm[64]; snprintf(nm, sizeof nm, "MPE-FEC %d rows, 2%% packets lost", c.rows);
            jprintf("  %s: sent %ld, good %ld, bad %ld, wrong %ld, rows clean %llu corrected %llu failed %llu, delta-t %d ms\n", nm, r.sent, r.good, r.bad, r.wrong,
                    (unsigned long long)r.st.rowsClean, (unsigned long long)r.st.rowsCorrected, (unsigned long long)r.st.rowsFailed, r.st.lastDeltaTms);
            // the first frame goes out before the stream is known to carry MPE-FEC: its datagrams come straight from the sections, lost ones stay lost
            check(r.good >= r.sent * 95 / 100 && r.wrong == 0, nm, "datagrams recovered");
            check(r.st.rowsCorrected > 0 && r.st.rows == c.rows && r.st.mpeFec, nm, "rows corrected, frame size read");
            check(r.st.lastDeltaTms == 500, nm, "delta-t read back");
        });
    jobs.add([] {   // punctured RS columns: weaker, still enough for 3% loss; and the frames after the first all complete
        mpe::EncoderConfig c; c.rows = 512; c.punctured = 32;
        const Run r = runTs(c, 4, 0.01, 9, true);
        jprintf("  MPE-FEC 512 rows, 32 RS columns punctured, 1%% lost: sent %ld, good %ld, rows corrected %llu failed %llu\n", r.sent, r.good,
                (unsigned long long)r.st.rowsCorrected, (unsigned long long)r.st.rowsFailed);
        check(r.good >= r.sent * 97 / 100 && r.wrong == 0, "punctured", "datagrams recovered");
    });
    jobs.add([] {   // loss sweep: where MPE-FEC (no puncturing, 256 rows) stops recovering everything
        double lastClean = 0;
        for (double loss = 0.01; loss <= 0.15; loss += 0.02) {
            mpe::EncoderConfig c; c.rows = 256;
            const Run r = runTs(c, 4, loss, 21, true, 400);
            jprintf("  loss sweep %.0f%%: sent %ld, good %ld, rows failed %llu\n", loss * 100, r.sent, r.good, (unsigned long long)r.st.rowsFailed);
            if (r.st.rowsFailed == 0) lastClean = loss;
        }
        jprintf("  loss sweep: every row corrected up to %.0f%% packet loss\n", lastClean * 100);
        check(lastClean >= 0.05, "loss sweep", "MPE-FEC carries 5% packet loss (sections of three packets)");
    });
    jobs.add([] {   // 3. plain MPE and time slicing without MPE-FEC: no loss, every datagram once
        mpe::EncoderConfig c; c.mpeFec = false; c.timeSlicing = false;
        Run r = runTs(c, 2, 0.0, 5, false);
        check(r.good == r.sent && r.bad == 0, "plain MPE", "every datagram");
        c.timeSlicing = true; c.deltaTms = 2500;
        r = runTs(c, 3, 0.0, 6, true);
        jprintf("  time slicing without MPE-FEC: sent %ld, good %ld, bursts %llu, delta-t %d ms\n", r.sent, r.good, (unsigned long long)r.st.bursts, r.st.lastDeltaTms);
        check(r.good == r.sent && r.st.bursts == 3 && r.st.lastDeltaTms == 2500, "time slicing", "datagrams, bursts and delta-t");
    });
    // 4. the whole DVB-H chain
    jobs.add([] {
        dvbt::Params p; p.mode = dvbt::k4K; p.guard = dvbt::kGi8; p.mod = dvbt::kQpsk; p.crHp = dvbt::kR12; p.indepth = true; p.dvbh = true; p.timeSlicing = true; p.mpeFec = true;
        mpe::EncoderConfig c; c.rows = 256; c.pid = 0x400; c.deltaTms = 200;
        mpe::MpeEncoder enc(c);
        std::deque<uint8_t> tsq;
        uint32_t seq = 0;
        long sent = 0;
        auto refill = [&] {
            std::vector<std::vector<uint8_t>> dg;
            size_t used = 0;
            while (used + 1000 < (size_t)191 * c.rows * 8 / 10) { dg.push_back(mpe::testDatagram(seq++, 1000)); used += 1000; }
            sent += (long)dg.size();
            std::vector<uint8_t> ts;
            enc.frame(dg, ts);
            tsq.insert(tsq.end(), ts.begin(), ts.end());
        };
        dvbt::Generator gen(p, [&](uint8_t* pkt) {
            if (tsq.size() < 188) refill();
            for (int i = 0; i < 188; i++) { pkt[i] = tsq.front(); tsq.pop_front(); }
        });
        const double fn = nativeRateHz(8);
        DvbtReceiver rx;
        rx.configure(fn, 8);
        mpe::MpeDemux dmx;
        dmx.setRealTimeParams(true);
        long good = 0, other = 0;
        std::set<uint32_t> seen;
        dmx.setDatagramCallback([&](const uint8_t* d, size_t n, bool rel, int) { uint32_t s; if (rel && mpe::checkTestDatagram(d, n, &s) && seen.insert(s).second) good++; else other++; });
        rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) { dmx.feed(pk, n); });
        std::mt19937 rng(4);
        std::normal_distribution<float> g(0.f, 1.f);
        const float sigma = (float)std::sqrt(std::pow(10.0, -9.0 / 10.0) / 2.0);   // 9 dB: QPSK 1/2 with some damaged TS packets
        std::vector<cf32> sym;
        double ph = 0;
        const double dph = 2 * M_PI * 1500.0 / fn;
        for (int s = 0; s < 68 * 4 * 6; s++) {
            gen.nextSymbol(sym);
            for (auto& v : sym) { ph = std::remainder(ph + dph, 2 * M_PI); v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sigma * g(rng), sigma * g(rng)); }
            rx.feed(sym.data(), sym.size());
        }
        dmx.flush();
        RxTelemetry t; rx.telemetry(t, 0);
        const auto& st = dmx.stats();
        jprintf("  DVB-H chain (4K in-depth QPSK 1/2, 9 dB): TPS %d (DVB-H %d, TS %d, MPE-FEC %d), TS packets ok %llu bad %llu, datagrams %ld of %ld sent, rows corrected %llu failed %llu\n",
                (int)t.dvbt.tpsOk, (int)t.dvbt.dvbh, (int)t.dvbt.timeSlicing, (int)t.dvbt.mpeFec, (unsigned long long)(t.dvbt.rsClean + t.dvbt.rsCorrected), (unsigned long long)t.dvbt.rsFailed,
                good, sent, (unsigned long long)st.rowsCorrected, (unsigned long long)st.rowsFailed);
        check(t.dvbt.tpsOk && t.dvbt.dvbh && t.dvbt.timeSlicing && t.dvbt.mpeFec, "chain", "DVB-H TPS bits");
        check(good > 200 && other == 0, "chain", "datagrams through the whole chain");
    });
    jobs.run();
    jprintf(fails ? "mpe_fec: FAILED\n" : "mpe_fec: ok\n");
    return fails ? 1 : 0;
}
