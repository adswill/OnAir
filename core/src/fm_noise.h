// Gaussian noise table for the FM test signal: unit variance values, read at pseudo random positions (much cheaper than a Gaussian generator per sample).
#pragma once
#include <cstdint>
#include <random>
#include <vector>

namespace dect2 {

struct FmNoiseTable {
    static constexpr unsigned kBits = 18;
    std::vector<float> t;
    FmNoiseTable() {
        t.resize(size_t(1) << kBits);
        std::mt19937 rng(20240611);
        std::normal_distribution<float> nd(0.f, 1.f);
        for (auto& v : t) v = nd(rng);
    }
    static const FmNoiseTable& get() { static const FmNoiseTable n; return n; }
};

}
