// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// T2-MI recordings played as files (issue 498, a Malaysian DVB-T2 modulator feed): an outer stream with T2-MI on PID 0x1000, no PCR, BB frames
// in high-efficiency mode with ISSY (the Malaysian feed: MATYPE F8) and, for a second PLP, null-packet deletion. TsFileSource must find the
// T2-MI PID, play the PLP with the most packets as an ordinary transport stream (with a PCR time map), switch PLP, delete its temporary file,
// survive a damaged T2-MI packet, and open an ordinary .ts file as before. Simulated only: no broadcast data is included.
#include "dect2/tv/playback.h"
#include "dect2/t2/t2mi_out.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint8_t crc8(const uint8_t* p, int n) {
    unsigned crc = 0;
    for (int i = 0; i < n; i++) for (int b = 7; b >= 0; b--) { unsigned fb = ((crc >> 7) & 1) ^ ((p[i] >> b) & 1); crc = (crc << 1) & 0xff; if (fb) crc ^= 0xD5; }
    return (uint8_t)crc;
}

// an inner transport stream: PAT, PMT, video PID `vpid` with a PCR every 20 packets (1000 packets per second of stream time) and,
// when `nulls`, null packets in runs of 1-3 between them
static std::vector<uint8_t> innerTs(int prog, int vpid, int n, bool nulls, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> ts;
    uint8_t ccv = 0;
    for (int i = 0; i < n; i++) {
        uint8_t p[188];
        if (i % 100 == 0) {   // PAT / PMT (contents do not matter here: a fixed pattern with the right PIDs)
            memset(p, 0xFF, 188); p[0] = 0x47; p[1] = 0x40; p[2] = 0x00; p[3] = (uint8_t)(0x10 | ((i / 100) & 15));
            p[4] = 0; p[5] = 0; p[6] = 0xB0; p[7] = 13; p[8] = 0; p[9] = 1; p[10] = 0xC1; p[11] = 0; p[12] = 0; p[13] = (uint8_t)(prog >> 8); p[14] = (uint8_t)prog; p[15] = 0xE1; p[16] = 0x00;
        } else {
            for (int k = 0; k < 188; k++) p[k] = (uint8_t)(rng() & 0xFF);
            p[0] = 0x47; p[1] = (uint8_t)((vpid >> 8) & 0x1F); p[2] = (uint8_t)vpid; p[3] = (uint8_t)(0x10 | (ccv++ & 15));
            if (i % 20 == 1) {
                p[3] = (uint8_t)(0x30 | (p[3] & 15)); p[4] = 7; p[5] = 0x10;
                const uint64_t base = (uint64_t)i * 90;   // 1 ms per packet
                p[6] = (uint8_t)(base >> 25); p[7] = (uint8_t)(base >> 17); p[8] = (uint8_t)(base >> 9); p[9] = (uint8_t)(base >> 1); p[10] = (uint8_t)(((base & 1) << 7) | 0x7E); p[11] = 0;
            }
        }
        ts.insert(ts.end(), p, p + 188);
        if (nulls && rng() % 3 == 0) {
            const int run = 1 + (int)(rng() % 3);
            for (int r = 0; r < run; r++) { uint8_t z[188]; memset(z, 0xFF, 188); z[0] = 0x47; z[1] = 0x1F; z[2] = 0xFF; z[3] = 0x10; ts.insert(ts.end(), z, z + 188); }
        }
    }
    return ts;
}

// HEM BB frames of a transport stream: user packets without the sync byte (+ a DNP byte with null-packet deletion), cut into data fields of `df` bytes
static std::vector<std::vector<uint8_t>> hemFrames(const std::vector<uint8_t>& ts, int df, bool npd) {
    std::vector<uint8_t> up;
    long lastDnp = -1;
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) {
        const bool isNull = ts[i + 1] == 0x1F && ts[i + 2] == 0xFF;
        if (npd && isNull && lastDnp >= 0 && up[(size_t)lastDnp] < 255) { up[(size_t)lastDnp]++; continue; }
        up.insert(up.end(), ts.begin() + (long)i + 1, ts.begin() + (long)i + 188);
        if (npd) { up.push_back(0); lastDnp = (long)up.size() - 1; }
    }
    const size_t stride = npd ? 188 : 187;
    std::vector<std::vector<uint8_t>> out;
    unsigned issy = 0x123456;
    for (size_t pos = 0; pos < up.size(); pos += (size_t)df) {
        const size_t n = std::min((size_t)df, up.size() - pos);
        const size_t first = (pos + stride - 1) / stride * stride;
        const unsigned syncd = first < pos + n ? (unsigned)((first - pos) * 8) : 0xFFFF;
        std::vector<uint8_t> f(10);
        f[0] = (uint8_t)(0xF8 | (npd ? 0x04 : 0)); f[1] = 0;
        f[2] = (uint8_t)(issy >> 16); f[3] = (uint8_t)(issy >> 8);
        f[4] = (uint8_t)((n * 8) >> 8); f[5] = (uint8_t)(n * 8);
        f[6] = (uint8_t)issy; f[7] = (uint8_t)(syncd >> 8); f[8] = (uint8_t)syncd;
        f[9] = (uint8_t)(crc8(f.data(), 9) ^ 0x01);   // HEM: the MODE bit is XORed into the CRC
        f.insert(f.end(), up.begin() + (long)pos, up.begin() + (long)(pos + n));
        out.push_back(f);
        issy = (issy + 977) & 0xFFFFFF;
    }
    return out;
}

// a signalling T2-MI packet (L1-current 0x10, timestamp 0x20): what a modulator feed carries next to the BB frames
static std::vector<uint8_t> otherPacket(uint8_t type, uint8_t count, int bytes) {
    std::vector<uint8_t> p = {type, count, 0x40, 0, (uint8_t)((bytes * 8) >> 8), (uint8_t)(bytes * 8)};
    for (int k = 0; k < bytes; k++) p.push_back((uint8_t)(k * 13 + type));
    const uint32_t c = t2mi::crc32(p.data(), p.size());
    p.push_back((uint8_t)(c >> 24)); p.push_back((uint8_t)(c >> 16)); p.push_back((uint8_t)(c >> 8)); p.push_back((uint8_t)c);
    return p;
}

static std::vector<uint8_t> outerTs(const std::vector<std::vector<uint8_t>>& f0, const std::vector<std::vector<uint8_t>>& f3, int corruptAt) {
    std::vector<uint8_t> ts;
    uint8_t cc = 0, cnt = 0, ccPat = 0;
    size_t a = 0, b = 0;
    int frame = 0, t2pk = 0;
    while (a < f0.size() || b < f3.size()) {
        if (frame % 8 == 0) {   // outer PAT on PID 0 (a fixed pattern), the signalling packets, and outer null packets: none carries a PCR
            uint8_t p[188]; memset(p, 0xFF, 188); p[0] = 0x47; p[1] = 0x40; p[2] = 0; p[3] = (uint8_t)(0x10 | (ccPat++ & 15)); p[4] = 0; p[5] = 0; p[6] = 0xB0; p[7] = 13;
            ts.insert(ts.end(), p, p + 188);
            t2mi::toTs(otherPacket(0x20, cnt++, 11), 0x1000, cc, ts);
            t2mi::toTs(otherPacket(0x10, cnt++, 69), 0x1000, cc, ts);
            uint8_t z[188]; memset(z, 0xFF, 188); z[0] = 0x47; z[1] = 0x1F; z[2] = 0xFF; z[3] = 0x10;
            ts.insert(ts.end(), z, z + 188);
        }
        for (int k = 0; k < 2 && a < f0.size(); k++, a++) {
            const size_t at = ts.size();
            t2mi::toTs(t2mi::bbPacket(cnt++, 0, (uint8_t)frame, 0, k == 0, f0[a]), 0x1000, cc, ts);
            if (t2pk++ == corruptAt) ts[at + 188 * 3 + 50] ^= 0x5A;   // inside the BB frame: the T2-MI CRC-32 fails, the frame is lost
        }
        if (b < f3.size()) { t2mi::toTs(t2mi::bbPacket(cnt++, 0, (uint8_t)frame, 3, true, f3[b]), 0x1000, cc, ts); b++; }
        frame++;
    }
    return ts;
}

static bool writeFile(const std::string& p, const std::vector<uint8_t>& d) { FILE* f = u8fopen(p, "wb"); if (!f) return false; const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size(); fclose(f); return ok; }

static std::vector<uint8_t> readAll(const TsFileSource& s) {
    std::vector<uint8_t> out(188 * 512);
    std::vector<uint8_t> all;
    for (uint64_t i = s.first(); i < s.end();) { const size_t g = s.read(i, out.data(), 512); if (!g) break; all.insert(all.end(), out.begin(), out.begin() + (long)(g * 188)); i += g; }
    return all;
}

// the packets that are not null packets, and the number of null packets
static std::vector<uint8_t> nonNull(const std::vector<uint8_t>& ts, size_t& nulls) {
    std::vector<uint8_t> o; nulls = 0;
    for (size_t i = 0; i + 188 <= ts.size(); i += 188) { if (ts[i + 1] == 0x1F && ts[i + 2] == 0xFF) { nulls++; continue; } o.insert(o.end(), ts.begin() + (long)i, ts.begin() + (long)i + 188); }
    return o;
}

static size_t filesIn(const std::filesystem::path& d) { size_t n = 0; std::error_code ec; for (auto& e : std::filesystem::directory_iterator(d, ec)) { (void)e; n++; } return n; }

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / ("onair_test_t2mi_file_" + std::to_string(std::random_device{}() % 1000000));
    const std::filesystem::path scratch = dir / "scratch";
    std::filesystem::create_directories(scratch, ec);
    const std::string outerPath = (dir / "feed.ts").string(), plainPath = (dir / "plain.ts").string(), damagedPath = (dir / "damaged.ts").string();

    const std::vector<uint8_t> in0 = innerTs(2, 0x220, 3000, false, 11);   // PLP 0: HEM + ISSY (as the Malaysian feed)
    const std::vector<uint8_t> in3 = innerTs(4, 0x420, 1000, true, 12);    // PLP 3: HEM + ISSY + null-packet deletion
    const auto f0 = hemFrames(in0, 5370, false), f3 = hemFrames(in3, 2111, true);
    CHECK(writeFile(outerPath, outerTs(f0, f3, -1)), "cannot write %s", outerPath.c_str());
    CHECK(writeFile(plainPath, in0), "cannot write %s", plainPath.c_str());
    CHECK(writeFile(damagedPath, outerTs(f0, f3, 40)), "cannot write %s", damagedPath.c_str());

    {   // the feed: found, PLP 0 (most packets) plays, byte for byte
        TsFileSource s; s.setScratchDir(scratch.string());
        CHECK(s.open(outerPath), "open feed");
        CHECK(s.t2mi() && s.t2miPid() == 0x1000, "T2-MI not found (pid 0x%X)", s.t2miPid());
        const std::vector<int> plps = s.t2miPlps();
        CHECK(plps.size() == 2 && plps[0] == 0 && plps[1] == 3, "PLPs: %zu", plps.size());
        CHECK(s.t2miPlp() == 0, "plays PLP %d, want 0", s.t2miPlp());
        const std::vector<uint8_t> got = readAll(s);
        CHECK(got == in0, "PLP 0 stream differs: %zu of %zu packets", got.size() / 188, in0.size() / 188);
        CHECK(s.duration() > 2.8 && s.duration() < 3.05, "PLP 0 duration %.3f s from the inner PCR", s.duration());
        CHECK(filesIn(scratch) == 1, "scratch files: %zu", filesIn(scratch));
        printf("feed PLP 0: %zu packets, %.2f s\n", got.size() / 188, s.duration());
        // the other PLP: null packets deleted by the modulator come back
        CHECK(s.selectT2miPlp(3) && s.t2miPlp() == 3, "select PLP 3");
        size_t nGot = 0, nWant = 0;
        const std::vector<uint8_t> g3 = nonNull(readAll(s), nGot), w3 = nonNull(in3, nWant);
        CHECK(g3 == w3, "PLP 3 stream differs: %zu of %zu packets", g3.size() / 188, w3.size() / 188);
        CHECK(nGot == nWant && nWant > 100, "PLP 3 null packets %zu, want %zu", nGot, nWant);
        CHECK(filesIn(scratch) == 1, "scratch files after the switch: %zu", filesIn(scratch));
        printf("feed PLP 3: %zu packets + %zu null packets\n", g3.size() / 188, nGot);
        CHECK(!s.selectT2miPlp(9) && s.t2miPlp() == 3, "a PLP that is not there");
        s.close();
        CHECK(filesIn(scratch) == 0, "scratch files after close: %zu", filesIn(scratch));
        CHECK(!s.t2mi(), "t2mi after close");
    }
    {   // a damaged T2-MI packet: one BB frame is lost, everything else comes out in order
        TsFileSource s; s.setScratchDir(scratch.string());
        CHECK(s.open(damagedPath) && s.t2mi() && s.t2miPlp() == 0, "open damaged feed");
        const std::vector<uint8_t> got = readAll(s);
        size_t j = 0, matched = 0;
        for (size_t i = 0; i + 188 <= got.size(); i += 188) {
            while (j + 188 <= in0.size() && memcmp(&got[i], &in0[j], 188) != 0) j += 188;
            if (j + 188 <= in0.size()) { matched++; j += 188; }
        }
        const size_t n = got.size() / 188, want = in0.size() / 188;
        CHECK(matched == n, "damaged: %zu of %zu packets are not from the stream", n - matched, n);
        CHECK(n + 40 >= want && n < want, "damaged: %zu of %zu packets (one frame is about 29)", n, want);
        printf("damaged feed: %zu of %zu packets\n", n, want);
    }
    {   // an ordinary transport stream is not T2-MI
        TsFileSource s; s.setScratchDir(scratch.string());
        CHECK(s.open(plainPath) && !s.t2mi() && s.end() == in0.size() / 188, "plain file");
        CHECK(readAll(s) == in0, "plain file contents");
        CHECK(filesIn(scratch) == 0, "plain file wrote scratch files");
    }
    std::filesystem::remove_all(dir, ec);
    printf(fails ? "t2mi file FAILED (%d)\n" : "t2mi file passed\n", fails);
    return fails ? 1 : 0;
}
