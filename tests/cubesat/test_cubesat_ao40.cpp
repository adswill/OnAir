// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AO-40 FEC (FUNcube-1 / Nayif-1 BPSK 1200): the real recording test vector of gr-satellites (tests/data/ao40: 5450 soft symbols, the frame
// window, the 256 byte frame), encode / decode round trip, noise, the distributed sync hunter (either polarity, offsets, sync errors).
#include "dect2/cubesat/cubesat_ao40.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>
using namespace dect2::cubesat;
#include "../common/test_util.h"

#ifndef DECT2_SOURCE_DIR
#define DECT2_SOURCE_DIR "."
#endif

static std::vector<uint8_t> readBytes(const std::string& n) {
    std::ifstream f(std::string(DECT2_SOURCE_DIR) + "/tests/data/ao40/" + n, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::vector<float> readFloats(const std::string& n) {
    const std::vector<uint8_t> b = readBytes(n);
    std::vector<float> v(b.size() / 4);
    if (!v.empty()) std::memcpy(v.data(), b.data(), v.size() * 4);
    return v;
}

int main() {
    const std::vector<float> sym = readFloats("symbols.f32"), win = readFloats("window.f32");
    const std::vector<uint8_t> ref = readBytes("frame.bin");
    CHECK(sym.size() == 5450 && win.size() == 5200 && ref.size() == 256, "test vectors present (%zu %zu %zu)", sym.size(), win.size(), ref.size());
    if (ref.size() != 256) return testSummary("cubesat ao40");
    // the real window
    {
        std::vector<uint8_t> fr; int e = -1;
        CHECK(ao40DecodeWindow(win.data(), fr, e) && fr == ref, "real window decodes to the reference frame");
        printf("real window: %d RS byte errors\n", e);
    }
    // the real stream through the hunter
    {
        Ao40Hunter h; int n = 0; std::vector<uint8_t> got;
        h.onFrame = [&](const std::vector<uint8_t>& f, int) { n++; got = f; };
        h.reset();
        for (float s : sym) h.push(s);
        CHECK(n == 1 && got == ref, "hunter on the real stream: %d frames", n);
        Ao40Hunter hi; n = 0; got.clear();
        hi.onFrame = [&](const std::vector<uint8_t>& f, int) { n++; got = f; };
        hi.reset();
        for (float s : sym) hi.push(-s);
        CHECK(n == 1 && got == ref, "hunter, inverted polarity: %d frames", n);
    }
    // encode / decode, with noise
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.f, 1.f);
    std::vector<uint8_t> f(256);
    for (auto& b : f) b = (uint8_t)rng();
    const std::vector<float> w = ao40Encode(f);
    {
        std::vector<uint8_t> out; int e = -1;
        CHECK(ao40DecodeWindow(w.data(), out, e) && out == f && e == 0, "clean round trip (rs %d)", e);
    }
    // noise: a rate 1/2 K=7 code with Reed-Solomon works at sigma 0.75 (Es/N0 about 2.5 dB) and fails far beyond
    for (int k = 0; k < 6; k++) {
        std::vector<float> x = w;
        for (auto& v : x) v += 0.75f * g(rng);
        std::vector<uint8_t> out; int e = -1;
        CHECK(ao40DecodeWindow(x.data(), out, e) && out == f, "sigma 0.75 trial %d", k);
    }
    {
        std::vector<float> x = w;
        for (auto& v : x) v += 3.0f * g(rng);
        std::vector<uint8_t> out; int e = -1;
        CHECK(!ao40DecodeWindow(x.data(), out, e) || out != f, "sigma 3 must not give the frame");
    }
    // hunter: junk in front, two frames back to back, wrong polarity, bad sync bits, noise
    for (int pol = 0; pol < 2; pol++) {
        Ao40Hunter h; std::vector<std::vector<uint8_t>> got;
        h.onFrame = [&](const std::vector<uint8_t>& fr, int) { got.push_back(fr); };
        h.reset();
        std::vector<float> s;
        for (int i = 0; i < 1234; i++) s.push_back(g(rng));
        std::vector<uint8_t> f2(256);
        for (auto& b : f2) b = (uint8_t)rng();
        std::vector<float> w2 = ao40Encode(f2);
        for (int j = 0; j < 5; j++) w2[(size_t)(80 * (j * 11))] = -w2[(size_t)(80 * (j * 11))];   // five wrong sync bits
        s.insert(s.end(), w.begin(), w.end());
        s.insert(s.end(), w2.begin(), w2.end());
        for (int i = 0; i < 300; i++) s.push_back(g(rng));
        for (auto& v : s) v = (pol ? -1.f : 1.f) * (v + 0.5f * g(rng));
        for (float v : s) h.push(v);
        CHECK(got.size() == 2 && got[0] == f && got[1] == f2, "hunter pol %d: %zu frames", pol, got.size());
    }
    // pure noise: no frame
    {
        Ao40Hunter h; int n = 0;
        h.onFrame = [&](const std::vector<uint8_t>&, int) { n++; };
        h.reset();
        for (int i = 0; i < 60000; i++) h.push(g(rng));
        CHECK(n == 0, "no frame from noise (%d)", n);
    }
    return testSummary("cubesat ao40");
}
