// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell buoy HF link test signal (datawell_gen.h): HXV vectors built from the documented layout (2FSK), our own test frames (4FSK).
#include "dect2/datawell/datawell_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/datawell/datawell_frames.h"
#include "dect2/datawell/datawell_hxv.h"
#include "dect2/datawell/datawell_rx.h"
#include <cmath>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

// ---- the test sea state: raw words in the documented layout (DWR-MkIII reference manual, tables 5.7.2 and 5.7.4)
unsigned dwTestSysWord(int num) {
    static const unsigned w[16] = {
        0x0B2,                       // Tp 0, M 1, T 0, F 1 (GPS fix), C 1, Tn 2
        340,                         // Hrms = 340 / 400 = 0.85 m
        56,                          // fz = 56 / 400 Hz (Tz about 7.1 s)
        1000,                        // PSDmax = 5000 exp(-1000 / 200)
        600,                         // Tr = 600 / 20 - 5 = 25 C
        470,                         // Tw = 470 / 20 - 5 = 18.5 C
        (26u << 4) | 1,              // 26 weeks of operational life, battery status 1
        160, 0, 0x800u | 80,         // accelerometer offsets +0.2, 0, -0.1 m/s2
        0, 0, 0, 0,                  // position, set below
        64,                          // orientation 90 degrees
        (6u << 8) | 225,             // inclination (90 / 128) (225 - 128 + 6 / 16)
    };
    unsigned v = w[num & 15];
    const unsigned lat = (unsigned)std::lround(54.5 / 90.0 * 8388608.0), lon = (unsigned)std::lround(5.2 / 180.0 * 8388608.0);
    if (num == 10) v = lat >> 12;                       // north: sign 0
    if (num == 11) v = lat & 0xFFF;
    if (num == 12) v = 0x800u | (lon >> 12);            // west: sign 1
    if (num == 13) v = lon & 0xFFF;
    return v;
}

// the four words of the spectrum entry n (table 5.7.2): freq index and direction, M2LSB / N2LSB / RPSD, spread and M2, N2 and K
DwTestSpecRaw dwTestSpecRaw(int n) {
    DwTestSpecRaw r;
    r.n = n; r.dir = (unsigned)((40 + 3 * n) & 0xFF); r.slsb = (unsigned)(n & 3); r.m2lsb = (unsigned)((n + 1) & 3); r.n2lsb = (unsigned)((n + 2) & 3);
    r.rpsd = (unsigned)(100 + 40 * n); r.s = (unsigned)(30 + n); r.m2 = (unsigned)(150 + n); r.n2 = (unsigned)(110 + n); r.k = (unsigned)(100 + n);
    return r;
}
uint16_t dwTestSpecWord(int n, int k) {
    const DwTestSpecRaw r = dwTestSpecRaw(n);
    switch (k) {
    case 0: return (uint16_t)((r.slsb << 14) | (r.n << 8) | r.dir);
    case 1: return (uint16_t)((r.m2lsb << 14) | (r.n2lsb << 12) | r.rpsd);
    case 2: return (uint16_t)((r.s << 8) | r.m2);
    default: return (uint16_t)((r.n2 << 8) | r.k);
    }
}
// displacements in cm at time t (s)
void dwTestDisp(double t, int& h, int& n, int& w) {
    h = (int)std::lround(60 * std::sin(2 * 3.14159265358979 * t / 7.2) + 20 * std::sin(2 * 3.14159265358979 * t / 3.1));
    n = (int)std::lround(30 * std::sin(2 * 3.14159265358979 * t / 7.2 + 1.0));
    w = (int)std::lround(25 * std::cos(2 * 3.14159265358979 * t / 7.2));
}

namespace {
using dect2::math::kPi;
constexpr double kNoiseC = 0.12;   // noise rms per component
}

struct DatawellGenerator::Impl {
    DatawellGenConfig c;
    std::mt19937 rng;
    dect2::normal_distribution<double> nd{0.0, 1.0};
    double amp = 0, phase = 0, cphase = 0;
    uint64_t g = 0;
    bool clean = false;
    double symPos = 0;            // position inside the symbol, in symbols
    std::vector<uint8_t> frame;   // the current frame's bits
    size_t fpos = 0;
    int curLevel = 0;
    std::vector<uint8_t> sent;    // not kept: the tests regenerate with the same seed
    uint32_t lfsr = 1;

    uint64_t vi = 0;              // HXV vector counter

    void newHxv() {
        const int pos = (int)(vi % 18), blk = (int)((vi / 18) % 16);
        uint16_t cyc = 0;
        if (pos == 0) cyc = 0x7FFF;
        else if (pos == 1) cyc = (uint16_t)((blk << 12) | dwTestSysWord(blk));
        else {
            const int g = (pos - 2) / 4, k = (pos - 2) % 4;
            cyc = dwTestSpecWord(4 * blk + g, k);
        }
        int h, n, w;
        dwTestDisp((double)vi / 1.28, h, n, w);
        if (c.wrsg) { int h2, n2, w2; dwTestDisp((double)(vi + 0.5) / 1.28, h2, n2, w2); n = h2; w = -682; }   // WR-SG: two vertical samples and a 0xAAA dummy
        const uint64_t v = datawell::hxvEncode(cyc, h, n, w);
        frame.clear();
        for (int i = 63; i >= 0; i--) frame.push_back((uint8_t)((v >> i) & 1));
        fpos = 0;
        vi++;
    }

    void newFrame() {
        if (c.format == 0) { newHxv(); return; }
        std::vector<uint8_t> by;
        by.push_back((uint8_t)(kDwTestSync >> 8)); by.push_back((uint8_t)(kDwTestSync & 0xFF));
        std::vector<uint8_t> pay;
        for (int i = 0; i < 12; i++) { lfsr = lfsr * 1664525u + 1013904223u; pay.push_back((uint8_t)(lfsr >> 24)); }
        by.insert(by.end(), pay.begin(), pay.end());
        int n = 0;
        const datawell::CrcSpec* s = datawell::crcSpecs(n);
        const uint32_t crc = datawell::crcCompute(s[0], pay.data(), pay.size());   // CRC-16/CCITT-FALSE
        by.push_back((uint8_t)(crc >> 8)); by.push_back((uint8_t)(crc & 0xFF));
        frame.clear();
        for (uint8_t b : by) for (int k = 7; k >= 0; k--) frame.push_back((uint8_t)((b >> k) & 1));
        fpos = 0;
    }
    int nextLevel() {
        if (frame.empty() || fpos >= frame.size()) newFrame();
        int v;
        if (c.format == 0) { v = frame[fpos++]; if (c.invert) v ^= 1; return v; }
        v = (frame[fpos] << 1) | frame[fpos + 1]; fpos += 2;
        if (c.invert) v ^= 3;
        return v;
    }
};

DatawellGenerator::DatawellGenerator(const DatawellGenConfig& cfg) : p_(new Impl) {
    Impl& m = *p_;
    m.c = cfg;
    m.rng.seed(cfg.seed);
    m.lfsr = cfg.seed * 2654435761u + 1;
    const double n0 = 2 * kNoiseC * kNoiseC / cfg.rate;   // complex noise power per Hz
    m.clean = cfg.snrDb >= 99;   // the transmitter's clean signal (TxSession): no noise, a fixed level instead of snrDb over the noise
    m.amp = m.clean ? 0.3 : std::sqrt(std::pow(10.0, cfg.snrDb / 10.0) * n0 * 100.0);
    m.curLevel = 0;
}
DatawellGenerator::~DatawellGenerator() = default;

void DatawellGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double fs = m.c.rate;
    const bool four = m.c.format == 1;
    const double baud = (four ? datawell::kBaud4Mfsk : datawell::kBaud2Fsk) * (1 + m.c.clockPpm * 1e-6);
    const double step = four ? datawell::kSpacing4Mfsk : datawell::kShift2Fsk;
    const int nl = four ? 4 : 2;
    for (size_t i = 0; i < n; i++) {
        std::complex<double> z(0, 0);
        if (!m.clean) { const double re = m.nd(m.rng) * kNoiseC; z = std::complex<double>(re, m.nd(m.rng) * kNoiseC); }
        if (m.g == 0) m.curLevel = m.c.noiseOnly || m.c.carrierOnly ? 0 : m.nextLevel();
        if (!m.c.noiseOnly) {
            double f = m.c.offsetHz;
            if (!m.c.carrierOnly) f += (m.curLevel - 0.5 * (nl - 1)) * step;
            m.phase += 2 * kPi * f / fs;
            if (m.phase > kPi) m.phase -= 2 * kPi; else if (m.phase < -kPi) m.phase += 2 * kPi;
            z += std::polar(m.amp, m.phase);
        }
        m.symPos += baud / fs;
        if (m.symPos >= 1) { m.symPos -= 1; if (!m.c.noiseOnly && !m.c.carrierOnly) m.curLevel = m.nextLevel(); }
        out[i] = cf32((float)z.real(), (float)z.imag());
        m.g++;
    }
}

namespace {
class DatawellSynth : public ModeSynth {
public:
    DatawellSynth(const SynthConfig& cfg, double fs) : fs_(fs) {
        DatawellGenConfig c;
        c.rate = fs;
        c.snrDb = cfg.snrDb;
        c.format = cfg.modeOpt[0] == 1 ? 1 : 0;
        c.noiseOnly = cfg.modeOpt[0] == 2;
        c.carrierOnly = cfg.modeOpt[0] == 3;
        c.invert = cfg.modeOpt[1] == 1;
        c.wrsg = cfg.modeOpt[2] == 1;
        c.offsetHz = cfg.modeVal[0] != 0 ? cfg.modeVal[0] : 1500;
        c.clockPpm = cfg.modeVal[1];
        gen_ = std::make_unique<DatawellGenerator>(c);
    }
    double sampleRate() const override { return fs_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    std::unique_ptr<DatawellGenerator> gen_;
    double fs_;
};
}

std::unique_ptr<ModeSynth> makeDatawellSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<DatawellSynth>(cfg, sampleRate);
}

} // namespace dect2
