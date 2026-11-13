// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Recording a service and the live buffer: the single-program filter (a synthetic two-programme multiplex in, one programme out: valid PAT and
// PMT, continuity counters intact, every PID of the service and nothing else), the packet ring (overflow, reading behind, jump to live), and the
// writer queue (a stalled disk drops and counts, never blocks).
#include "dect2/tv/ts.h"
#include "dect2/tv/ts_record.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

typedef std::vector<uint8_t> Bytes;
static std::map<int, int> gCc;

static void pkt(Bytes& ts, int pid, bool pusi, const uint8_t* d, int n, bool pcr = false) {
    uint8_t p[188]; memset(p, 0xFF, sizeof p);
    p[0] = 0x47; p[1] = (uint8_t)((pusi ? 0x40 : 0) | (pid >> 8)); p[2] = (uint8_t)pid;
    int off = 4;
    if (pcr) {
        p[3] = (uint8_t)(0x30 | (gCc[pid]++ & 15));
        p[4] = 183; p[5] = 0x10; for (int i = 6; i < 12; i++) p[i] = (uint8_t)(i * 3);
        off = 188;
    } else if (n < 184) {
        const int af = 184 - n;
        p[3] = (uint8_t)(0x30 | (gCc[pid]++ & 15));
        p[4] = (uint8_t)(af - 1); if (af > 1) p[5] = 0;
        off = 4 + af;
    } else p[3] = (uint8_t)(0x10 | (gCc[pid]++ & 15));
    if (n && !pcr) memcpy(p + off, d, n);
    ts.insert(ts.end(), p, p + 188);
}
static void section(Bytes& ts, int pid, Bytes sec) {
    const uint32_t crc = mpegCrc32(sec.data(), (int)sec.size());
    for (int s = 24; s >= 0; s -= 8) sec.push_back((uint8_t)(crc >> s));
    Bytes d; d.push_back(0); d.insert(d.end(), sec.begin(), sec.end());
    pkt(ts, pid, true, d.data(), (int)d.size());
}
static void pat(Bytes& ts) {
    Bytes s = {0x00, 0xB0, 0, 0x12, 0x34, 0xC1, 0, 0, 0, 1, 0xE1, 0x00, 0, 2, 0xE2, 0x00};
    s[2] = (uint8_t)(s.size() - 3 + 4);
    section(ts, 0, s);
}
static void pmt(Bytes& ts, int sid, int pmtPid, int pcrPid, const std::vector<std::pair<int, int>>& es) {
    Bytes s = {0x02, 0xB0, 0, (uint8_t)(sid >> 8), (uint8_t)sid, 0xC1, 0, 0, (uint8_t)(0xE0 | (pcrPid >> 8)), (uint8_t)pcrPid, 0xF0, 0};
    for (auto& e : es) { s.push_back((uint8_t)e.first); s.push_back((uint8_t)(0xE0 | (e.second >> 8))); s.push_back((uint8_t)e.second); s.push_back(0xF0); s.push_back(0); }
    s[2] = (uint8_t)(s.size() - 3 + 4);
    section(ts, pmtPid, s);
}
static int pidOf(const uint8_t* p) { return ((p[1] & 0x1F) << 8) | p[2]; }

int main() {
    // programme 1: PMT 0x100, PCR on 0x101, video 0x101 (H.264), audio 0x102 (AAC), data 0x103; programme 2: PMT 0x200, 0x201..0x202
    Bytes ts;
    for (int r = 0; r < 40; r++) {
        pat(ts);
        pmt(ts, 1, 0x100, 0x101, {{0x1B, 0x101}, {0x0F, 0x102}, {0x06, 0x103}});
        pmt(ts, 2, 0x200, 0x201, {{0x1B, 0x201}, {0x0F, 0x202}});
        uint8_t pl[184]; memset(pl, 0x5A, sizeof pl);
        pkt(ts, 0x101, false, nullptr, 0, true);
        for (int pid : {0x101, 0x102, 0x103, 0x201, 0x202, 0x11, 0x12, 0x1FFF}) { pl[0] = (uint8_t)r; pkt(ts, pid, r % 3 == 0, pl, 184); pkt(ts, pid, false, pl, 100); }
    }
    TsDemux dm;
    for (size_t i = 0; i < ts.size(); i += 188) dm.feed(&ts[i]);
    TsSnapshot snap = dm.snapshot();
    CHECK(snap.services.size() == 2, "services found: %zu", snap.services.size());

    ServiceFilter f; f.setStrict(true); f.select(1);
    Bytes out; uint8_t o[188];
    for (size_t i = 0; i < ts.size(); i += 188) if (f.process(&ts[i], &snap, o)) out.insert(out.end(), o, o + 188);
    CHECK(out.size() > 188 * 100, "output size %zu", out.size());
    std::set<int> pids; std::map<int, int> lastCc; int ccBad = 0, patN = 0;
    for (size_t i = 0; i < out.size(); i += 188) {
        const uint8_t* p = &out[i];
        const int pid = pidOf(p), cc = p[3] & 15;
        pids.insert(pid);
        const bool payload = p[3] & 0x10;
        if (lastCc.count(pid) && payload && cc != ((lastCc[pid] + 1) & 15)) ccBad++;
        if (payload) lastCc[pid] = cc;
        if (pid == 0) {   // a valid single-programme PAT
            patN++;
            const uint8_t* s = p + 5;
            const int len = ((s[1] & 15) << 8) | s[2];
            CHECK(mpegCrc32(s, 3 + len) == 0, "PAT CRC");
            CHECK(len == 13, "PAT lists %d bytes: one programme", len);
            CHECK(((s[8] << 8) | s[9]) == 1 && (((s[10] & 0x1F) << 8) | s[11]) == 0x100, "PAT programme 1 -> PMT 0x100");
        }
    }
    CHECK(pids == (std::set<int>{0, 0x100, 0x101, 0x102, 0x103}), "PIDs out: %zu (expected exactly PAT, PMT, 3 streams)", pids.size());
    CHECK(ccBad == 0, "%d continuity jumps in the output", ccBad);
    CHECK(patN >= 30, "PAT packets: %d", patN);
    TsDemux d2;
    for (size_t i = 0; i < out.size(); i += 188) d2.feed(&out[i]);
    const TsSnapshot s2 = d2.snapshot();
    CHECK(s2.services.size() == 1 && s2.services[0].id == 1 && s2.services[0].havePmt && s2.services[0].streams.size() == 3, "the output reads back as one programme with three streams");

    // ---- ring: overflow drops the oldest, reading behind, jump to live
    TsRing ring; ring.setCapacityPackets(100);
    uint8_t pk[188]; memset(pk, 0, sizeof pk);
    for (int i = 0; i < 250; i++) { pk[0] = 0x47; pk[5] = (uint8_t)i; pk[6] = (uint8_t)(i >> 8); ring.push(pk); }
    CHECK(ring.head() == 250 && ring.oldest() == 150, "head %llu oldest %llu", (unsigned long long)ring.head(), (unsigned long long)ring.oldest());
    CHECK(!ring.read(149, o), "a packet that was overwritten must not be readable");
    CHECK(ring.read(150, o) && (o[5] | (o[6] << 8)) == 150, "oldest kept packet is 150");
    CHECK(ring.read(249, o) && (o[5] | (o[6] << 8)) == 249, "newest is 249");
    CHECK(!ring.read(250, o), "the future is not readable");
    uint64_t cursor = 120;   // a reader that fell behind is moved to the oldest kept packet (what the player does)
    if (cursor < ring.oldest()) cursor = ring.oldest();
    CHECK(cursor == 150, "read-behind clamps to oldest");
    int n = 0; while (cursor < ring.head()) { CHECK(ring.read(cursor, o) && (o[5] | (o[6] << 8)) == (int)cursor, "sequence at %llu", (unsigned long long)cursor); cursor++; n++; }
    CHECK(n == 100, "read %d packets", n);
    cursor = ring.head();   // jump to live: nothing behind, the next packet pushed is the next one read
    pk[5] = 250; ring.push(pk);
    CHECK(ring.read(cursor, o) && o[5] == 250, "after the jump the next read is the next live packet");
    ring.setCapacityPackets(0); ring.push(pk);
    CHECK(ring.head() == 0, "a zero-size ring keeps nothing");
    // PCR reading
    uint8_t pc[188] = {0x47, 0x01, 0x01, 0x20, 183, 0x10};
    const uint64_t base = 90000ull * 5;   // 5 s
    pc[6] = (uint8_t)(base >> 25); pc[7] = (uint8_t)(base >> 17); pc[8] = (uint8_t)(base >> 9); pc[9] = (uint8_t)(base >> 1); pc[10] = (uint8_t)((base & 1) << 7);
    double sec = 0;
    CHECK(tsPcrSeconds(pc, sec) && sec > 4.999 && sec < 5.001, "PCR %f", sec);
    CHECK(!tsPcrSeconds(&ts[0], sec), "no PCR in a PAT packet");

    // ---- writer: a stalled disk drops and counts, the caller never waits; what was queued is written when it recovers
    const char* path = "test_ts_record.tmp.ts";
    {
        TsWriter w;
        CHECK(w.open(path, 188 * 1000), "open");
        w.holdForTest(true);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 5000; i++) w.write(ts.data(), 188);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 500, "write() took %.0f ms with the disk stalled", ms);
        CHECK(w.droppedPackets() == 4000, "dropped %llu (expected 4000)", (unsigned long long)w.droppedPackets());
        w.holdForTest(false);
        w.close();
        CHECK(w.bytes() == 188u * 1000, "written %llu bytes", (unsigned long long)w.bytes());
        CHECK(!w.failed(), "no disk error");
    }
    FILE* fp = fopen(path, "rb");
    CHECK(fp != nullptr, "file exists");
    if (fp) { fseek(fp, 0, SEEK_END); CHECK(ftell(fp) == 188 * 1000, "file size %ld", ftell(fp)); fclose(fp); }
    remove(path);
    { TsWriter w; CHECK(!w.open("/nonexistent-dir/x.ts"), "opening in a missing folder fails"); }

    printf(fails ? "FAILED (%d)\n" : "ts_record: all passed\n", fails);
    return fails ? 1 : 0;
}
