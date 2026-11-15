// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Kenwood FleetSync burst: encoder and decoder round trip for FleetSync and FleetSync II over fleet / unit combinations, single-bit error correction
// of every position of FleetSync II, inverted tone sense, noise, a level change, clock offset, leading silence, voice around the burst, and rejection
// of noise and a corrupted frame. Encoder and decoder are written from the same format description (chv_fsync.h): this shows self-consistency and the
// error correction, not agreement with a real Kenwood radio. Simulated signals only.
#include "dect2/chanvoice/chv_fsync.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2::chv;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<FsyncMessage> decode(const std::vector<float>& a, double rate = 48000) {
    FsyncDecoder d(rate);
    std::vector<FsyncMessage> v;
    d.setCallback([&](const FsyncMessage& m) { v.push_back(m); });
    for (size_t i = 0; i < a.size(); i += 480) d.feed(a.data() + i, std::min<size_t>(480, a.size() - i));
    return v;
}

int main() {
    std::mt19937 rng(7);
    std::normal_distribution<float> gauss(0.f, 1.f);
    // the block check: a built frame holds, a changed bit does not
    for (int fs2 = 0; fs2 < 2; fs2++) {
        const std::vector<uint8_t> pl = fsyncPayloadBits(107, 1772, fs2 != 0);
        CHECK(pl.size() == (fs2 ? 260u : 68u), "payload size %zu", pl.size());
        FsyncMessage m;
        CHECK(fsyncDecodeBits(pl.data(), pl.size(), m) && m.fleet == 107 && m.unit == 1772 && m.fs2 == (fs2 != 0), "payload decode fs2 %d", fs2);
        if (!fs2) { std::vector<uint8_t> bad = pl; bad[10] ^= 1; FsyncMessage x; CHECK(!fsyncDecodeBits(bad.data(), bad.size(), x), "FleetSync: a flipped bit passes"); }
    }
    // every fleet / unit field value class survives a round trip, FleetSync and II
    for (int fs2 = 0; fs2 < 2; fs2++) for (int fleet : {99, 100, 107, 200, 354}) for (int unit : {999, 1000, 1015, 1772, 3000, 5094}) {
        const std::vector<uint8_t> pl = fsyncPayloadBits(fleet, unit, fs2 != 0, 0x1234, 0x0ABC);
        FsyncMessage m;
        CHECK(fsyncDecodeBits(pl.data(), pl.size(), m) && m.fleet == fleet && m.unit == unit && (m.word1 >> 16) == 0x1234 && ((m.word2 >> 16) & 0xFFF) == 0xABC, "fleet %d unit %d fs2 %d", fleet, unit, fs2);
    }
    // FleetSync II corrects one wrong bit in each 15 bit code word, at any position of any word
    {
        const std::vector<uint8_t> pl = fsyncPayloadBits(107, 1772, true);
        int bad = 0, n = 0;
        for (int blk = 0; blk < 4; blk++) for (int half = 0; half < 4; half++) for (int bit = 0; bit < 15; bit++) {
            // block layout: word 1 (bits 0..31) = code words 2 then 1, word 2 = code words 4 then 3; the code word occupies the low 15 bits of its 16
            const int base = 4 + blk * 64 + (half < 2 ? 0 : 32) + ((half & 1) ? 0 : 16);   // first bit (MSB) of the 16 bit half
            std::vector<uint8_t> e = pl; e[(size_t)(base + 1 + (14 - bit))] ^= 1;
            FsyncMessage m; n++;
            if (!(fsyncDecodeBits(e.data(), e.size(), m) && m.fleet == 107 && m.unit == 1772 && m.fs2)) bad++;
        }
        CHECK(bad == 0, "FleetSync II: %d of %d single-bit errors not corrected", bad, n);
    }
    // audio: plain, with noise, level, inversion, offsets
    for (int fs2 = 0; fs2 < 2; fs2++) for (int inv = 0; inv < 2; inv++) {
        for (double rate : {48000.0, 44100.0}) {
            std::vector<float> a(4800, 0.f);
            const std::vector<float> b = fsyncEncode(107, 1772, fs2 != 0, rate, 0.3, 48, inv != 0);
            a.insert(a.end(), b.begin(), b.end());
            a.resize(a.size() + 4800, 0.f);
            auto v = decode(a, rate);
            CHECK(v.size() == 1 && v[0].fleet == 107 && v[0].unit == 1772 && v[0].fs2 == (fs2 != 0), "clean fs2 %d inv %d rate %.0f: %zu messages", fs2, inv, rate, v.size());
        }
    }
    // noise, a clock offset, leading silence of any length, voice-like audio before and after, a level change
    {
        int ok = 0, total = 0;
        for (int trial = 0; trial < 40; trial++) {
            const bool fs2 = trial & 1;
            const int fleet = 100 + (trial * 7) % 250, unit = 999 + (trial * 211) % 4000;
            const double rate = 48000.0 * (1.0 + 0.0004 * (trial % 5 - 2));   // the burst is made at a slightly different clock
            const std::vector<float> b = fsyncEncode(fleet, unit, fs2, rate, 0.2 + 0.02 * (trial % 6));
            std::vector<float> a((size_t)(1000 + trial * 137), 0.f);
            for (size_t i = 0; i < 6000; i++) a.push_back(0.1f * (float)std::sin(0.01 * (double)i * (1 + trial % 3)) * (float)std::sin(0.0007 * (double)i));   // voice-like noise before
            a.insert(a.end(), b.begin(), b.end());
            for (size_t i = 0; i < 6000; i++) a.push_back(0.1f * (float)std::sin(0.02 * (double)i));
            for (float& s : a) s += 0.02f * gauss(rng);
            auto v = decode(a);
            total++;
            if (v.size() == 1 && v[0].fleet == fleet && v[0].unit == unit && v[0].fs2 == fs2) ok++;
            else printf("  trial %d: %zu messages\n", trial, v.size());
        }
        CHECK(ok == total, "noisy trials: %d of %d decoded", ok, total);
    }
    // two bursts in a row, and a burst of two different units
    {
        std::vector<float> a(2000, 0.f);
        for (int k = 0; k < 3; k++) { const auto b = fsyncEncode(110 + k, 1000 + k * 100, k == 1, 48000, 0.3); a.insert(a.end(), b.begin(), b.end()); a.resize(a.size() + 3000, 0.f); }
        auto v = decode(a);
        CHECK(v.size() == 3 && v[0].fleet == 110 && v[1].unit == 1100 && v[2].fleet == 112, "three bursts: %zu", v.size());
    }
    // rejection: noise alone, and a frame whose data is damaged beyond the check
    {
        std::vector<float> a(48000 * 5);
        for (float& s : a) s = 0.3f * gauss(rng);
        CHECK(decode(a).empty(), "noise decoded as a burst");
        // a FleetSync frame with two flipped data bits: dropped, counted
        std::vector<uint8_t> pl = fsyncPayloadBits(107, 1772, false); pl[12] ^= 1; pl[40] ^= 1;
        FsyncMessage m; CHECK(!fsyncDecodeBits(pl.data(), pl.size(), m), "damaged frame accepted");
    }
    printf(fails ? "chv_fsync: %d FAILED\n" : "chv_fsync: all passed\n", fails);
    return fails ? 1 : 0;
}
