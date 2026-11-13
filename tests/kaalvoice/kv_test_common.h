// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared helpers of the KAAL voice codec 4 tests: run a signal through an encoder/decoder pair with a channel in between.
#pragma once
#include "dect2/kaalvoice/kaal_voice.h"
#include "dect2/kaalvoice/kaal_voice_eval.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace kvtest {
using namespace dect2::kaalvoice;

// The channel sees every encoded frame (index, type, bytes) and may damage it: set `lost` for an erasure, flip bits, or set badClasses.
struct ChannelFrame {
    long index;
    FrameType type;
    std::vector<uint8_t>& bytes;
    bool lost;
    uint32_t badClasses;
};
using Channel = std::function<void(ChannelFrame&)>;

struct Run {
    std::vector<int16_t> out;
    std::vector<FrameType> types;
    std::vector<std::vector<uint8_t>> frames;   // what the encoder produced
    int delay = 0;
};

inline Run run(const Config& c, const std::vector<int16_t>& x, const Channel& ch = nullptr) {
    Encoder e(c);
    Decoder d(c);
    Run r;
    r.delay = e.info().delaySamples;
    const int n = e.info().frameSamples;
    std::vector<int16_t> f((size_t)n), o((size_t)n);
    std::vector<uint8_t> b;
    long idx = 0;
    for (size_t p = 0; p < x.size() + (size_t)r.delay + (size_t)n; p += (size_t)n, idx++) {
        for (int i = 0; i < n; i++) f[(size_t)i] = p + i < x.size() ? x[p + i] : 0;
        const FrameType t = e.encode(f.data(), b);
        r.types.push_back(t);
        r.frames.push_back(b);
        std::vector<uint8_t> rx = b;
        ChannelFrame cf{idx, t, rx, false, 0};
        if (ch) ch(cf);
        if (cf.lost) d.decode(FrameType::Speech, nullptr, 0, o.data());
        else d.decode(t, rx.empty() ? nullptr : rx.data(), rx.size(), o.data(), cf.badClasses);
        r.out.insert(r.out.end(), o.begin(), o.end());
    }
    return r;
}

inline eval::Metrics score(const std::vector<int16_t>& ref, const Run& r, int fs) { return eval::compare(ref, r.out, fs, r.delay); }

inline double peak(const std::vector<int16_t>& x) {
    int m = 0;
    for (int16_t v : x) m = std::max(m, std::abs((int)v));
    return m;
}

inline Config cfg(Profile p, int ms, int fs = 16000, bool dtx = false) {
    Config c;
    c.profile = p;
    c.frameMs = ms;
    c.sampleRate = fs;
    c.dtx = dtx;
    return c;
}

struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed) {}
    uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    double uni() { return next() / 16777216.0; }
};

}  // namespace kvtest
