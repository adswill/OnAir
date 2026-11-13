// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PMTs with inverted elementary-stream PIDs (issue 498, DPRK DVB-T2): the PMT gives video 0x1F37 and audio 0x1F36 while the packets come on
// 0x00C8 / 0x00C9 (PID XOR 0x1FFF), the PCR PID is the real video PID, and a 101-byte private program descriptor (tag 0x54) sits in front.
// The demuxer must take the inverse PIDs, the service filter must pass them and write the PMT again with them (valid CRC, a new version),
// and ordinary streams must not be touched: a declared PID that carries data, an inverse with too few packets, an inverse another stream declares.
// Simulated only: the layout copies the recordings attached to issue 498 (Pyongyang 554 MHz), no broadcast data is included.
#include "dect2/tv/ts.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> section(uint8_t tid, int ext, int ver, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> s = {tid, 0, 0, (uint8_t)(ext >> 8), (uint8_t)ext, (uint8_t)(0xC1 | ((ver & 31) << 1)), 0, 0};
    s.insert(s.end(), body.begin(), body.end());
    const int len = (int)s.size() - 3 + 4;
    s[1] = (uint8_t)(0xB0 | ((len >> 8) & 0x0F)); s[2] = (uint8_t)len;
    const uint32_t c = mpegCrc32(s.data(), (int)s.size());
    s.push_back((uint8_t)(c >> 24)); s.push_back((uint8_t)(c >> 16)); s.push_back((uint8_t)(c >> 8)); s.push_back((uint8_t)c);
    return s;
}
static void psi(int pid, const std::vector<uint8_t>& sec, uint8_t& cc, std::vector<uint8_t>& ts) {
    uint8_t p[188]; memset(p, 0xFF, 188);
    p[0] = 0x47; p[1] = (uint8_t)(0x40 | ((pid >> 8) & 0x1F)); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (cc++ & 15)); p[4] = 0;
    memcpy(p + 5, sec.data(), sec.size());
    ts.insert(ts.end(), p, p + 188);
}
static void pes(int pid, uint8_t sid, bool start, uint8_t& cc, std::vector<uint8_t>& ts) {
    uint8_t p[188];
    for (int k = 0; k < 188; k++) p[k] = (uint8_t)(pid * 3 + k);
    p[0] = 0x47; p[1] = (uint8_t)((start ? 0x40 : 0) | ((pid >> 8) & 0x1F)); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (cc++ & 15));
    if (start) { p[4] = 0; p[5] = 0; p[6] = 1; p[7] = sid; p[8] = 0; p[9] = 0; p[10] = 0x80; p[11] = 0x80; p[12] = 5; }
    ts.insert(ts.end(), p, p + 188);
}

struct Svc { int prog, pmtPid, pcrPid, vDecl, aDecl, vReal, aReal; };

static std::vector<uint8_t> pmtSection(const Svc& s, bool dprkDesc, int ver = 0) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)(0xE0 | (s.pcrPid >> 8))); b.push_back((uint8_t)s.pcrPid);
    std::vector<uint8_t> pd;
    if (dprkDesc) { pd.push_back(0x54); pd.push_back(101); pd.push_back(4); pd.push_back(4); pd.push_back(0); pd.push_back(0); pd.push_back(0); for (int k = 0; k < 96; k++) pd.push_back((uint8_t)(k * 37 + 11)); }
    b.push_back((uint8_t)(0xF0 | (pd.size() >> 8))); b.push_back((uint8_t)pd.size());
    b.insert(b.end(), pd.begin(), pd.end());
    const uint8_t avc[] = {0x28, 0x04, 0x64, 0x00, 0x28, 0x3F}, lang[] = {0x0A, 0x04, 'k', 'o', 'r', 0x00};
    b.push_back(0x1B); b.push_back((uint8_t)(0xE0 | (s.vDecl >> 8))); b.push_back((uint8_t)s.vDecl); b.push_back(0xF0); b.push_back(6); b.insert(b.end(), avc, avc + 6);
    b.push_back(0x04); b.push_back((uint8_t)(0xE0 | (s.aDecl >> 8))); b.push_back((uint8_t)s.aDecl); b.push_back(0xF0); b.push_back(6); b.insert(b.end(), lang, lang + 6);
    return section(0x02, s.prog, ver, b);
}

// a multiplex: PAT and PMTs every cycle, then video (40 packets) and audio (4) of every service on their real PIDs
static std::vector<uint8_t> mux(const std::vector<Svc>& sv, int cycles, bool dprkDesc, int videoPerCycle = 40) {
    std::vector<uint8_t> ts;
    uint8_t ccPat = 0, ccPmt[8] = {0}, ccV[8] = {0}, ccA[8] = {0};
    std::vector<uint8_t> pat;
    for (auto& s : sv) { pat.push_back((uint8_t)(s.prog >> 8)); pat.push_back((uint8_t)s.prog); pat.push_back((uint8_t)(0xE0 | (s.pmtPid >> 8))); pat.push_back((uint8_t)s.pmtPid); }
    const std::vector<uint8_t> patSec = section(0x00, 0x20EC, 0, pat);
    for (int c = 0; c < cycles; c++) {
        psi(0, patSec, ccPat, ts);
        for (size_t i = 0; i < sv.size(); i++) psi(sv[i].pmtPid, pmtSection(sv[i], dprkDesc), ccPmt[i], ts);
        for (size_t i = 0; i < sv.size(); i++) {
            for (int k = 0; k < videoPerCycle; k++) pes(sv[i].vReal, 0xE0, k == 0, ccV[i], ts);
            for (int k = 0; k < 4; k++) pes(sv[i].aReal, 0xC0, k == 0, ccA[i], ts);
        }
    }
    return ts;
}

static const TsService* find(const TsSnapshot& sn, int prog) { for (auto& s : sn.services) if (s.id == prog) return &s; return nullptr; }

// the PMT of `prog` in the filter's output: ES PIDs, version, CRC
static bool outPmt(const std::vector<uint8_t>& ts, int pmtPid, std::vector<int>& pids, int& ver, int& pcr) {
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) {
        const uint8_t* p = &ts[i];
        if ((((p[1] & 0x1F) << 8) | p[2]) != pmtPid || !(p[1] & 0x40)) continue;
        const uint8_t* s = p + 5 + p[4];
        const int sl = ((s[1] & 0x0F) << 8) | s[2], end = 3 + sl - 4;
        const uint32_t have = ((uint32_t)s[end] << 24) | ((uint32_t)s[end + 1] << 16) | ((uint32_t)s[end + 2] << 8) | s[end + 3];
        if (mpegCrc32(s, end) != have) { printf("  output PMT CRC wrong\n"); return false; }
        ver = (s[5] >> 1) & 31; pcr = ((s[8] & 0x1F) << 8) | s[9];
        pids.clear();
        const int pil = ((s[10] & 0x0F) << 8) | s[11];
        for (int k = 12 + pil; k + 5 <= end; k += 5 + (((s[k + 3] & 0x0F) << 8) | s[k + 4])) pids.push_back(((s[k + 1] & 0x1F) << 8) | s[k + 2]);
        return true;   // (the last one would do as well: every copy is patched the same way)
    }
    return false;
}

static void run(const char* name, const std::vector<uint8_t>& ts, const std::vector<Svc>& sv, const std::vector<bool>& expectRemap, double drop, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<uint8_t> in;
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) if (drop <= 0 || u(rng) >= drop) in.insert(in.end(), ts.begin() + i, ts.begin() + i + 188);
    TsDemux d;
    for (size_t i = 0; i + 188 <= in.size(); i += 188) d.feed(&in[i]);
    const TsSnapshot sn = d.snapshot();
    for (size_t k = 0; k < sv.size(); k++) {
        const TsService* s = find(sn, sv[k].prog);
        CHECK(s && s->havePmt && s->streams.size() == 2, "%s: service %d missing", name, sv[k].prog);
        if (!s || s->streams.size() != 2) continue;
        const int wantV = expectRemap[k] ? sv[k].vReal : sv[k].vDecl, wantA = expectRemap[k] ? sv[k].aReal : sv[k].aDecl;
        CHECK(s->streams[0].pid == wantV && s->streams[1].pid == wantA, "%s: service %d PIDs 0x%X 0x%X, want 0x%X 0x%X", name, sv[k].prog, s->streams[0].pid, s->streams[1].pid, wantV, wantA);
        CHECK((s->streams[0].declaredPid >= 0) == expectRemap[k], "%s: service %d declaredPid %d", name, sv[k].prog, s->streams[0].declaredPid);
        if (expectRemap[k]) CHECK(s->streams[0].declaredPid == sv[k].vDecl && s->streams[1].declaredPid == sv[k].aDecl, "%s: declared PIDs kept", name);
        CHECK(s->pcrPid == sv[k].pcrPid && s->declaredPcrPid < 0, "%s: PCR PID 0x%X", name, s->pcrPid);
        CHECK(s->streams[0].kind == "video" && s->streams[1].kind == "audio", "%s: kinds %s %s", name, s->streams[0].kind.c_str(), s->streams[1].kind.c_str());
        // the service filter (the player's and the recorder's): the real PIDs pass, the PMT comes out with them
        for (int strict : {0, 1}) {
            ServiceFilter f; f.setStrict(strict != 0); f.select(sv[k].prog);
            std::vector<uint8_t> out; uint8_t o[188];
            uint64_t vIn = 0, vOut = 0, aOut = 0;
            for (size_t i = 0; i + 188 <= in.size(); i += 188) {
                const int pid = ((in[i + 1] & 0x1F) << 8) | in[i + 2];
                if (pid == sv[k].vReal) vIn++;
                if (f.process(&in[i], &sn, o)) {
                    out.insert(out.end(), o, o + 188);
                    const int op = ((o[1] & 0x1F) << 8) | o[2];
                    if (op == sv[k].vReal) vOut++;
                    if (op == sv[k].aReal) aOut++;
                    CHECK(op != sv[k].vReal || memcmp(o, &in[i], 188) == 0, "%s: a video packet was changed", name);
                }
            }
            std::vector<int> pids; int ver = -1, pcr = -1;
            CHECK(outPmt(out, sv[k].pmtPid, pids, ver, pcr), "%s: no PMT in the filter output", name);
            CHECK(pids.size() == 2 && pids[0] == wantV && pids[1] == wantA, "%s strict %d: output PMT PIDs wrong", name, strict);
            CHECK(ver == (expectRemap[k] ? 16 : 0), "%s strict %d: output PMT version %d", name, strict, ver);
            CHECK(pcr == sv[k].pcrPid, "%s: output PCR PID 0x%X", name, pcr);
            if (expectRemap[k]) CHECK(vIn > 0 && vOut == vIn && aOut > 0, "%s strict %d: video %llu of %llu passed, audio %llu", name, strict, (unsigned long long)vOut, (unsigned long long)vIn, (unsigned long long)aOut);
        }
    }
    if (expectRemap[0]) {
        bool lab = false;
        for (auto& p : sn.pids) if (p.pid == sv[0].vReal && p.label.find("PMT says") != std::string::npos) lab = true;
        CHECK(lab, "%s: the PID list does not say what the PMT declared", name);
    }
    printf("%s: %zu packets in, %zu services\n", name, in.size() / 188, sn.services.size());
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // the four services of the 554 MHz multiplex: PMT 0x1E/0x82/0xE6/0x14A, video = PCR 0xC8/0x12C/0x190/0x1F4, PMT PIDs inverted
    std::vector<Svc> dprk;
    const int pmts[4] = {0x1E, 0x82, 0xE6, 0x14A}, vids[4] = {0xC8, 0x12C, 0x190, 0x1F4};
    for (int i = 0; i < 4; i++) dprk.push_back({1 + 8 * i, pmts[i], vids[i], vids[i] ^ 0x1FFF, (vids[i] + 1) ^ 0x1FFF, vids[i], vids[i] + 1});
    const std::vector<bool> all(4, true);
    run("dprk clean", mux(dprk, 12, true), dprk, all, 0, 1);
    run("dprk 10% packet loss", mux(dprk, 12, true), dprk, all, 0.10, 2);
    {   // stray packets on the declared PIDs (bit errors in the PID field of a weak signal) must not stop the remap
        std::vector<uint8_t> ts = mux(dprk, 12, true);
        uint8_t cc = 0;
        for (int k = 0; k < 3; k++) pes(0x1F37, 0xE0, false, cc, ts);
        run("dprk with stray packets", ts, dprk, all, 0, 3);
    }
    // an ordinary multiplex: declared PIDs carry the streams, nothing changes and the PMT passes as it is
    std::vector<Svc> normal = {{1, 0x100, 0x101, 0x101, 0x102, 0x101, 0x102}, {2, 0x200, 0x201, 0x201, 0x202, 0x201, 0x202}};
    run("ordinary mux", mux(normal, 12, false), normal, {false, false}, 0, 4);
    {   // a declared PID that is silent, and its inverse has only a few packets: no remap
        std::vector<Svc> few = {{1, 0x1E, 0xC8, 0x1F37, 0x1F36, 0xC8, 0xC9}};
        run("inverse with few packets", mux(few, 2, true, 3), few, {false}, 0, 5);
    }
    {   // service 2 declares PIDs whose inverse service 1 declares and uses: no remap for service 2 (its streams are just silent)
        std::vector<Svc> two = {{1, 0x100, 0x101, 0x101, 0x102, 0x101, 0x102}, {2, 0x200, 0x101, 0x101 ^ 0x1FFF, 0x102 ^ 0x1FFF, 0x101, 0x102}};
        std::vector<uint8_t> ts = mux({two[0]}, 12, false);
        // add service 2's PAT entry and PMT without any packets of its own
        TsDemux d;
        std::vector<uint8_t> pat = {0, 1, 0xE1, 0x00, 0, 2, 0xE2, 0x00};
        std::vector<uint8_t> extra; uint8_t c0 = 12, c1 = 0;
        psi(0, section(0x00, 0x20EC, 1, pat), c0, extra);
        psi(0x200, pmtSection(two[1], false), c1, extra);
        ts.insert(ts.end(), extra.begin(), extra.end());
        for (size_t i = 0; i + 188 <= ts.size(); i += 188) d.feed(&ts[i]);
        const TsSnapshot sn = d.snapshot();
        const TsService* s2 = find(sn, 2);
        CHECK(s2 && s2->havePmt && s2->streams.size() == 2 && s2->streams[0].pid == (0x101 ^ 0x1FFF) && s2->streams[0].declaredPid < 0, "inverse declared elsewhere: remapped anyway");
        const TsService* s1 = find(sn, 1);
        CHECK(s1 && s1->streams.size() == 2 && s1->streams[0].pid == 0x101 && s1->streams[0].declaredPid < 0, "inverse declared elsewhere: service 1 changed");
        printf("inverse declared by another service: checked\n");
    }
    printf(fails ? "ts pid invert FAILED (%d)\n" : "ts pid invert passed\n", fails);
    return fails ? 1 : 0;
}
