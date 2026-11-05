// Yaesu System Fusion test signal (see fusion_gen.h).
#include "dect2/fusion/fusion_gen.h"
#include <algorithm>
#include <cmath>

namespace dect2::fusion {

std::vector<uint8_t> genVoiceBytes(uint32_t seed, int frame, int sub, int kind) {
    uint32_t x = seed * 2654435761u + (uint32_t)frame * 40503u + (uint32_t)sub * 9973u + 12345u;
    const size_t n = kind == 1 ? 9 : kind == 2 ? 7 : 18;
    std::vector<uint8_t> v(n);
    for (auto& b : v) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
    if (kind == 2) v[6] &= 0x80;   // 49 bits: seven bits of padding
    return v;
}

namespace {
std::vector<uint8_t> field(const std::string& s, size_t n) { std::vector<uint8_t> v(n); padField(s, v.data(), n); return v; }
std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
}

bool genTransmissionBits(const GenParams& p, std::vector<uint8_t>& bits, std::vector<FrameContent>* frames) {
    bits.clear();
    if (frames) frames->clear();
    const auto csd1 = cat(field(p.dest, 10), field(p.src, 10));
    const auto csd2 = cat(field(p.down, 10), field(p.up, 10));
    std::vector<uint8_t> csd3;
    for (int i = 0; i < 4; i++) csd3 = cat(csd3, field(p.rem[i], 5));
    const size_t chunk = p.dt == 2 ? 10 : 20;
    const size_t maxSlow = p.dt == 0 ? 100 : p.dt == 2 ? 20 : p.dt == 1 ? 260 : 0;
    std::string slow = p.slow.substr(0, std::min(p.slow.size(), maxSlow));
    const int dtUnits = (int)((slow.size() + chunk - 1) / chunk);
    std::vector<std::vector<uint8_t>> dt;
    for (int i = 0; i < dtUnits; i++) dt.push_back(field(slow.substr(std::min(slow.size(), (size_t)i * chunk)), chunk));

    auto emit = [&](const FrameContent& c) {
        std::vector<uint8_t> b;
        if (!buildFrameBits(c, b)) return false;
        bits.insert(bits.end(), b.begin(), b.end());
        if (frames) frames->push_back(c);
        return true;
    };
    Fich f;
    f.cm = p.cm; f.mr = p.mr; f.voip = p.voip; f.dt = p.dt; f.sqlOff = p.sqlOff; f.sqlCode = p.sqlCode;
    f.cs = 2;

    FrameContent h;
    h.fich = f; h.fich.fi = 0;
    h.units = {csd1, csd2};
    if (!emit(h)) return false;

    // the repeating unit sequence of the communication frames
    std::vector<std::vector<uint8_t>> seq;
    int perFrame = 1;
    if (p.dt == 0) { seq = {csd1, csd2, csd3}; }
    else if (p.dt == 2) { seq = {field(p.dest, 10), field(p.src, 10), field(p.down, 10), field(p.up, 10), cat(field(p.rem[0], 5), field(p.rem[1], 5)), cat(field(p.rem[2], 5), field(p.rem[3], 5))}; }
    else if (p.dt == 1) { seq = {csd1, csd2, csd3}; perFrame = 2; }
    if (p.dt != 3) { for (auto& u : dt) seq.push_back(u); }
    if (p.dt == 1 && seq.size() % 2) seq.push_back(field("", 20));
    const int cycle = std::max(1, (int)seq.size() / perFrame);

    for (int k = 0; k < p.commFrames; k++) {
        FrameContent c;
        c.fich = f; c.fich.fi = 1;
        if (p.dt == 3) {
            c.fich.fn = 0; c.fich.ft = k == 0 ? 1 : 0;
            if (k == 0) { c.units = {csd3}; for (int s = 0; s < 2; s++) c.voice.push_back({3, genVoiceBytes(p.seed, k, s, 3)}); }
            else for (int s = 0; s < 5; s++) c.voice.push_back({3, genVoiceBytes(p.seed, k, s, 3)});
        } else {
            const int fn = k % cycle;
            c.fich.fn = fn; c.fich.ft = cycle - 1;
            for (int u = 0; u < perFrame; u++) c.units.push_back(seq[(size_t)(fn * perFrame + u)]);
            if (p.dt == 0 || p.dt == 2) for (int s = 0; s < 5; s++) c.voice.push_back({p.dt == 0 ? 1 : 2, genVoiceBytes(p.seed, k, s, p.dt == 0 ? 1 : 2)});
        }
        if (!emit(c)) return false;
    }
    FrameContent t = h;
    t.fich.fi = 2;
    return emit(t);
}

std::vector<std::complex<float>> modulateC4fm(const std::vector<uint8_t>& bits, double fs, double cfoHz, float amp, int idle, double devScale, double rateScale) {
    const int nsym = (int)bits.size() / 2;
    const double sps = fs / (4800.0 * rateScale);
    const int total = (int)std::ceil((nsym + 2 * idle + 8) * sps);
    std::vector<float> lvl((size_t)(nsym + 2 * idle + 8), 0.f);
    for (int i = 0; i < nsym; i++) lvl[(size_t)(idle + 4 + i)] = (float)dibitLevel(bits[(size_t)(2 * i)] * 2 + bits[(size_t)(2 * i + 1)]);
    constexpr double kPi = 3.14159265358979323846, alpha = 0.35;
    auto rc = [&](double t) {   // raised cosine, t in symbols
        const double a = std::fabs(t);
        if (a >= 4) return 0.0;
        const double s = a < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        const double d = 1.0 - 4 * alpha * alpha * t * t;
        const double c = std::fabs(d) < 1e-6 ? kPi / 4 : std::cos(kPi * alpha * t) / d;
        return s * c;
    };
    std::vector<std::complex<float>> out((size_t)total);
    double ph = 0;
    for (int n = 0; n < total; n++) {
        const double t = n / sps;
        const int c = (int)std::floor(t);
        double f = 0;
        for (int k = c - 4; k <= c + 4; k++) if (k >= 0 && k < (int)lvl.size()) f += lvl[(size_t)k] * 900.0 * devScale * rc(t - (k + 0.5));
        ph += 2 * kPi * (f + cfoHz) / fs;
        if (ph > kPi) ph -= 2 * kPi;
        if (ph < -kPi) ph += 2 * kPi;
        out[(size_t)n] = std::complex<float>((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
    }
    return out;
}

} // namespace dect2::fusion
