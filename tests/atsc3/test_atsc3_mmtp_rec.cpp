// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MMTP on a recording of real UDP datagrams: the file named by the environment variable ONAIR_ATSC3_MMTP_REC (skipped, exit code 77, when it
// is not set or not there; no recording ships with OnAir). The file is a list of records, each: source IPv4 address (4 bytes, network order),
// destination IPv4 address (4 bytes, network order), source port and destination port (2 bytes each, little endian), payload length (4
// bytes, little endian), payload. The first video service of the service list that uses MMTP is played through MmtpService; per video and
// audio asset at least 90% of the MPUs that lie wholly inside the recording must come out, with one init segment and decode times that only
// go up.
#include "dect2/atsc3/atsc3_ip.h"
#include "dect2/atsc3/atsc3_mmtp.h"
#include "dect2/common/endian.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace dect2::atsc3;
using dect2::endian::loadBe32;
using dect2::endian::loadLe16;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main() {
    const char* path = getenv("ONAIR_ATSC3_MMTP_REC");
    if (!path || !*path) { printf("ONAIR_ATSC3_MMTP_REC is not set: skipped\n"); return 77; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { printf("%s cannot be read: skipped\n", path); return 77; }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<UdpDatagram> dg;
    for (size_t p = 0; p + 16 <= d.size();) {
        UdpDatagram u;
        u.srcIp = loadBe32(&d[p]);
        u.dstIp = loadBe32(&d[p + 4]);
        u.srcPort = loadLe16(&d[p + 8]);
        u.dstPort = loadLe16(&d[p + 10]);
        const size_t n = (size_t)d[p + 12] | (size_t)d[p + 13] << 8 | (size_t)d[p + 14] << 16 | (size_t)d[p + 15] << 24;
        if (n > d.size() - p - 16) break;
        u.payload.assign(d.begin() + p + 16, d.begin() + p + 16 + n);
        dg.push_back(std::move(u));
        p += 16 + n;
    }
    SltService svc;
    for (auto& u : dg) {
        if (u.dstIp != kLlsAddress || u.dstPort != kLlsPort) continue;
        for (auto& t : parseLlsAll(u.payload.data(), u.payload.size()))
            for (auto& s : t.slt.services)
                if (!svc.serviceId && s.slsProtocol == 2 && s.category == 1) svc = s;
        if (svc.serviceId) break;
    }
    printf("  %zu datagrams; MMTP service %d at %s:%d\n", dg.size(), svc.serviceId, ipToString(svc.slsDstIp).c_str(), svc.slsDstPort);
    if (!svc.serviceId) { printf("no MMTP video service in the recording: skipped\n"); return 77; }

    // the MPUs of each asset in the file, by their packets
    std::map<uint16_t, std::set<uint32_t>> seen;
    for (auto& u : dg) {
        if (u.dstIp != svc.slsDstIp || u.dstPort != svc.slsDstPort) continue;
        MmtpPacket p;
        MpuPayload m;
        if (parseMmtpPacket(u.payload.data(), u.payload.size(), p) && p.type == MmtpMpu && parseMpuPayload(p.payload, p.payloadSize, m)) seen[p.packetId].insert(m.mpuSequence);
    }
    MmtpService s(svc.slsSrcIp, svc.slsDstIp, svc.slsDstPort);
    struct Lane { std::string handler; int inits = 0, segs = 0; bool rising = true; uint64_t last = 0; };
    std::map<uint16_t, Lane> lanes;
    s.onObject = [&](uint16_t pid, const std::string& h, bool init, const std::vector<uint8_t>& b) {
        Lane& l = lanes[pid];
        l.handler = h;
        if (init) { l.inits++; return; }
        // the decode time is in the tfdt (version 1) of the only traf: moof(8) mfhd(16) traf(8) tfhd(size) tfdt
        size_t pos = 8 + 16 + 8;
        if (b.size() < pos + 8) return;
        pos += loadBe32(&b[pos]);
        if (b.size() < pos + 20) return;
        const uint64_t t = (uint64_t)loadBe32(&b[pos + 12]) << 32 | loadBe32(&b[pos + 16]);
        if (l.segs > 0 && t <= l.last) l.rising = false;
        l.last = t;
        l.segs++;
    };
    for (auto& u : dg) s.push(u);
    const MmtpStats st = s.stats();
    printf("  %ld packets, %ld MP tables, %ld MPUs delivered, %ld dropped, %ld late packets, %ld bad, %ld assets skipped\n", st.packets, st.mpts, st.mpusDelivered, st.mpusDropped, st.late, st.bad,
           st.assetsSkipped);
    int av = 0;
    for (auto& kv : lanes) {
        const Lane& l = kv.second;
        // the first and the last MPU of the recording are cut by its start and end
        const long whole = std::max<long>(1, (long)seen[kv.first].size() - 2);
        printf("  packet id %u (%s): %d of %ld whole MPUs, %d init segment(s), decode times %s\n", (unsigned)kv.first, l.handler.c_str(), l.segs, whole, l.inits, l.rising ? "rising" : "NOT rising");
        av++;
        CHECK(l.inits == 1, "one init segment per asset");
        CHECK(l.rising, "decode times only go up");
        CHECK(l.segs * 10 >= whole * 9, "at least 90% of the MPUs");
    }
    CHECK(av >= 2, "a video and an audio asset");
    printf(fails ? "atsc3 mmtp recording: FAILED\n" : "atsc3 mmtp recording: ok\n");
    return fails ? 1 : 0;
}
