// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell buoy HF link: the bit stream analysis (datawell_frames.h): known CRC check values, the repetition period, the constant start
// pattern, the CRC attempt on frames built with a few CRCs, and no false positives on random bits.
#include "dect2/datawell/datawell_frames.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2::datawell;
#include "../common/test_util.h"

static const CrcSpec* find(const char* name) {
    int n = 0;
    const CrcSpec* s = crcSpecs(n);
    for (int i = 0; i < n; i++) if (!strcmp(s[i].name, name)) return &s[i];
    return nullptr;
}

static void put(std::vector<uint8_t>& bits, const std::vector<uint8_t>& bytes, bool lsb = false) {
    for (uint8_t b : bytes) for (int k = 0; k < 8; k++) bits.push_back((uint8_t)(lsb ? (b >> k) & 1 : (b >> (7 - k)) & 1));
}

// frames of `len` payload bytes, a sync of two bytes and the named CRC (big endian) over the payload; returns the bit stream of n frames
static std::vector<uint8_t> stream(const char* crc, int n, int len, std::mt19937& rng, bool lsb = false, bool inv = false, int slip = 0) {
    std::vector<uint8_t> bits;
    for (int i = 0; i < slip; i++) bits.push_back((uint8_t)(rng() & 1));
    for (int f = 0; f < n; f++) {
        std::vector<uint8_t> by = {0x1A, 0xCF}, pay;
        for (int i = 0; i < len; i++) pay.push_back((uint8_t)(rng() >> 8));
        const CrcSpec* s = find(crc);
        const uint32_t c = crcCompute(*s, pay.data(), pay.size());
        by.insert(by.end(), pay.begin(), pay.end());
        for (int i = s->width / 8 - 1; i >= 0; i--) by.push_back((uint8_t)(c >> (8 * i)));
        put(bits, by, lsb);
    }
    if (inv) for (auto& b : bits) b ^= 1;
    return bits;
}

int main() {
    {   // the check values of the catalogue (the ASCII string 123456789)
        const uint8_t d[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        struct { const char* n; uint32_t v; } want[] = {{"CRC-16/CCITT-FALSE", 0x29B1}, {"CRC-16/XMODEM", 0x31C3}, {"CRC-16/KERMIT", 0x2189}, {"CRC-16/X-25", 0x906E},
            {"CRC-16/AUG-CCITT", 0xE5CC}, {"CRC-16/ARC", 0xBB3D}, {"CRC-16/MODBUS", 0x4B37}, {"CRC-16/BUYPASS", 0xFEE8}, {"CRC-16/DNP", 0xEA82},
            {"CRC-32/ISO-HDLC", 0xCBF43926}, {"CRC-8", 0xF4}, {"CRC-8/MAXIM", 0xA1}};
        for (const auto& w : want) { const CrcSpec* s = find(w.n); CHECK(s && crcCompute(*s, d, 9) == w.v, "%s: %08X", w.n, s ? crcCompute(*s, d, 9) : 0); }
    }
    std::mt19937 rng(7);
    {   // period, start pattern and CRC attempt on 14 frames of 12 bytes, starting off the frame boundary
        const std::vector<uint8_t> bits = stream("CRC-16/KERMIT", 14, 12, rng, false, false, 37);
        const Period p = findPeriod(bits, 32, 2048);
        CHECK(p.lag == 128 && p.z > 9, "period %d z %.1f", p.lag, p.z);
        const Sync s = findSync(bits, 128);
        CHECK(s.found && s.hex.compare(0, 4, "1ACF") == 0 && s.start == 37, "sync %d '%s' start %d", s.found, s.hex.c_str(), s.start);
        std::vector<std::vector<uint8_t>> fr;
        for (int i = 0; i < 10; i++) fr.emplace_back(bits.begin() + 37 + i * 128, bits.begin() + 37 + (i + 1) * 128);
        const CrcHit h = bestCrc(fr, s.len);
        CHECK(h.name == "CRC-16/KERMIT" && h.pass == 10 && !h.inverted && !h.lsbFirst, "crc '%s' %d / %d inv %d lsb %d", h.name.c_str(), h.pass, h.total, h.inverted, h.lsbFirst);
        CHECK(frameCrcOk(fr[0], h), "frameCrcOk");
        fr[0][60] ^= 1;
        CHECK(!frameCrcOk(fr[0], h), "a flipped bit must fail");
    }
    {   // inverted and LSB first, a 32 bit CRC; one bad frame in ten
        std::vector<uint8_t> bits = stream("CRC-32/ISO-HDLC", 12, 20, rng, true, true);
        const int L = 16 + 160 + 32;
        std::vector<std::vector<uint8_t>> fr;
        for (int i = 0; i < 10; i++) fr.emplace_back(bits.begin() + i * L, bits.begin() + (i + 1) * L);
        fr[4][100] ^= 1;
        const CrcHit h = bestCrc(fr, 16);
        CHECK(h.name == "CRC-32/ISO-HDLC" && h.pass == 9 && h.inverted && h.lsbFirst, "crc32 '%s' %d / %d inv %d lsb %d", h.name.c_str(), h.pass, h.total, h.inverted, h.lsbFirst);
    }
    {   // random frames with no CRC: nothing fits; random bits have no period and no sync
        std::vector<std::vector<uint8_t>> fr;
        for (int i = 0; i < 12; i++) { std::vector<uint8_t> f; for (int k = 0; k < 128; k++) f.push_back((uint8_t)(rng() & 1)); fr.push_back(f); }
        const CrcHit h = bestCrc(fr, 0);
        CHECK(h.name.empty(), "false CRC '%s' %d / %d", h.name.c_str(), h.pass, h.total);
        std::vector<uint8_t> bits;
        for (int k = 0; k < 8192; k++) bits.push_back((uint8_t)(rng() & 1));
        const Period p = findPeriod(bits, 32, 2048);
        CHECK(p.z < 7, "random bits: period %d z %.1f", p.lag, p.z);
        CHECK(!findSync(bits, 128).found, "random bits: sync");
        std::vector<uint8_t> ones(4000, 1);
        CHECK(findPeriod(ones, 32, 2048).lag == 0, "constant bits");
    }
    CHECK(bitsToHex((const uint8_t*)"\x01\x00\x01\x01\x00\x00\x00\x00\x01", 9, false, false) == "B080", "hex");
    printf(fails ? "datawell frames: %d FAILED\n" : "datawell frames: all passed\n", fails);
    return fails ? 1 : 0;
}
