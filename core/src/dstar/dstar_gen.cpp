// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR test signal (see dstar_gen.h): whole DV transmissions as GMSK, over noise.
#include "dect2/dstar/dstar_gen.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kSps = 10;
constexpr double kShapeRate = 4800.0 * kSps;
constexpr double kDev = 1200.0;          // h = 0.5

std::string ddmm(double v, int degDigits, char pos, char neg, int decimals) {
    const char h = v >= 0 ? pos : neg;
    v = std::fabs(v);
    int d = (int)v;
    double m = (v - d) * 60;
    const double r = std::pow(10.0, decimals);
    m = std::round(m * r) / r;
    if (m >= 60) { m -= 60; d++; }
    char b[40];
    snprintf(b, sizeof b, "%0*d%0*.*f,%c", degDigits, d, decimals + 3, decimals, m, h);
    return b;
}
} // namespace

void dstarKnownVoice(uint32_t fn, uint8_t out[72]) {
    uint32_t s = fn * 2654435761u + 12345u;
    for (int i = 0; i < 72; i++) { s = s * 1103515245u + 12345u; out[i] = (uint8_t)((s >> 16) & 1); }
}

std::string dstarRmc(double lat, double lon, double speedKmh, int course) {
    char b[160];
    snprintf(b, sizeof b, "GPRMC,120000,A,%s,%s,%.1f,%d,111026,,", ddmm(lat, 2, 'N', 'S', 3).c_str(), ddmm(lon, 3, 'E', 'W', 3).c_str(), speedKmh / 1.852, course);
    return "$" + std::string(b) + "*" + dstar::nmeaChecksum(b) + "\r\n";
}

std::string dstarGga(double lat, double lon, double altM) {
    char b[160];
    snprintf(b, sizeof b, "GPGGA,120000,%s,%s,1,08,1.0,%.1f,M,,M,,", ddmm(lat, 2, 'N', 'S', 3).c_str(), ddmm(lon, 3, 'E', 'W', 3).c_str(), altM);
    return "$" + std::string(b) + "*" + dstar::nmeaChecksum(b) + "\r\n";
}

std::string dstarDprs(const std::string& call, double lat, double lon, const std::string& comment) {
    std::string la = ddmm(lat, 2, 'N', 'S', 2), lo = ddmm(lon, 3, 'E', 'W', 2);
    la.erase(la.find(','), 1); lo.erase(lo.find(','), 1);
    const std::string rest = call + ">API51,DSTAR*:!" + la + "/" + lo + ">" + comment + "\r";
    char b[16];
    snprintf(b, sizeof b, "$$CRC%04X,", dstar::dprsCrc(rest));
    return b + rest + "\n";
}

void dstarTxBits(const DstarGenTx& tx, std::vector<uint8_t>& bits, uint32_t) {
    bits.clear();
    for (int i = 0; i < 64; i++) bits.push_back((uint8_t)(i % 2 == 0));
    bits.insert(bits.end(), dstar::kFrameSync, dstar::kFrameSync + 15);
    uint8_t hdr[dstar::kHeaderBytes], hb[dstar::kHeaderBits];
    dstar::headerPack(tx.hdr, hdr);
    dstar::headerEncode(hdr, hb);
    for (int i = 0; i < tx.headerErrors && i < dstar::kHeaderBits; i++) hb[(size_t)(i * dstar::kHeaderBits / tx.headerErrors + 7) % dstar::kHeaderBits] ^= 1;
    bits.insert(bits.end(), hb, hb + dstar::kHeaderBits);
    // the slow data blocks, sent round and round
    std::vector<std::vector<uint8_t>> blocks;
    auto chunks = [&](uint8_t type, const uint8_t* p, size_t n) {
        for (size_t o = 0; o < n; o += 5) {
            const size_t k = std::min<size_t>(5, n - o);
            std::vector<uint8_t> b(6, 0x66);
            b[0] = (uint8_t)(type | k);
            memcpy(b.data() + 1, p + o, k);
            blocks.push_back(b);
        }
    };
    if (!tx.text.empty()) {
        std::string t = tx.text;
        t.resize(20, ' ');
        for (int i = 0; i < 4; i++) { std::vector<uint8_t> b(6); b[0] = (uint8_t)(0x40 | i); memcpy(b.data() + 1, t.data() + 5 * i, 5); blocks.push_back(b); }
    }
    if (tx.headerResend) chunks(0x50, hdr, dstar::kHeaderBytes);
    for (const auto& g : tx.gps) chunks(0x30, (const uint8_t*)g.data(), g.size());
    if (tx.squelch >= 0) blocks.push_back({0xC2, (uint8_t)tx.squelch, (uint8_t)tx.squelch, 0x66, 0x66, 0x66});
    if (blocks.empty()) blocks.push_back(std::vector<uint8_t>(6, 0x66));
    const int nf = std::max(1, (int)std::lround(tx.durSec / 0.02));
    size_t bi = 0, sent = 0;
    std::vector<uint8_t> cur(6, 0x66);
    for (int fn = 0; fn < nf; fn++) {
        uint8_t v[72];
        dstarKnownVoice((uint32_t)fn, v);
        bits.insert(bits.end(), v, v + 72);
        const int idx = fn % dstar::kSuperframe;
        if (idx == 0) { bits.insert(bits.end(), dstar::kDataSync, dstar::kDataSync + 24); continue; }
        const int half = (idx - 1) & 1;
        if (half == 0) {
            cur = blocks[bi];
            bi = (bi + 1) % blocks.size();
            sent++;
            if (tx.dropBlock > 0 && sent <= blocks.size() && sent % (size_t)tx.dropBlock == 0) cur.assign(6, 0x66);   // in the first round only: the repeats bring it back
        }
        uint8_t d[3], db[24];
        for (int k = 0; k < 3; k++) d[k] = cur[(size_t)(3 * half + k)] ^ dstar::kSlowScramble[k];
        dstar::bytesToBits(d, 3, db);
        bits.insert(bits.end(), db, db + 24);
    }
    if (tx.endPattern) bits.insert(bits.end(), dstar::kEndPattern, dstar::kEndPattern + 48);
}

std::vector<DstarGenTx> dstarTestSchedule() {
    std::vector<DstarGenTx> v;
    DstarGenTx t;
    t.hdr.flag[0] = 0x40; t.hdr.rpt2 = "LY0VNO G"; t.hdr.rpt1 = "LY0VNO B"; t.hdr.ur = "CQCQCQ"; t.hdr.my = "N0CALL"; t.hdr.sfx = "ID51";
    t.text = "OnAir D-STAR test 73";
    t.gps = {dstarRmc(54.68720, 25.27970, 36, 90), dstarGga(54.68720, 25.27970, 112)};
    t.durSec = 4.0; t.gapSec = 0.8;
    v.push_back(t);
    t = {};
    t.hdr.ur = "CQCQCQ"; t.hdr.my = "LY1TST"; t.hdr.sfx = "";
    t.text = "Hello via simplex";
    t.gps = {dstarDprs("LY1TST-7", 54.89850, 23.90360, "OnAir D-PRS test")};
    t.squelch = 12;
    t.durSec = 3.0; t.gapSec = 1.0;
    v.push_back(t);
    return v;
}

struct DstarGenerator::Impl {
    DstarGenConfig cfg;
    std::vector<float> g;          // Gaussian, 10 samples per bit
    std::deque<float> fq;          // frequency (in units of the deviation) at 48 kHz, NaN = carrier off
    size_t txIdx = 0;
    bool done = false;
    double pos = 0, ph = 0;
    int64_t n = 0;
    float sigma = 0;
    genutil::NoiseSource noise;
    explicit Impl(const DstarGenConfig& c) : cfg(c), noise(c.seed) {}

    void silence(double sec) { for (int i = 0; i < (int)(sec * kShapeRate); i++) fq.push_back(NAN); }
    void refill() {
        if (done || cfg.txs.empty()) { silence(0.1); return; }
        const DstarGenTx& tx = cfg.txs[txIdx];
        std::vector<uint8_t> bits;
        dstarTxBits(tx, bits, cfg.seed + (uint32_t)txIdx);
        const int L = (int)g.size();
        const size_t N = bits.size() * kSps + (size_t)L;
        std::vector<float> y(N, 0.f);
        for (size_t k = 0; k < bits.size(); k++) {
            const float s = bits[k] ? 1.f : -1.f;
            for (int r = 0; r < kSps; r++) for (int j = 0; j < L; j++) y[k * kSps + (size_t)r + (size_t)j] += s * g[(size_t)j];
        }
        for (float v : y) fq.push_back(v);
        silence(tx.gapSec);
        if (++txIdx >= cfg.txs.size()) { txIdx = 0; if (!cfg.loop) done = true; }
    }
};

double DstarGenerator::noiseSigma(const DstarGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double nb = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 6.25 kHz
    return std::sqrt(nb * c.rate / 6250 / 2);
}

DstarGenerator::DstarGenerator(const DstarGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * 0.5) * kSps;   // BT 0.5, in samples
    double s = 0;
    for (int k = -2 * kSps; k <= 2 * kSps; k++) { const float v = (float)std::exp(-0.5 * k * k / (sigma * sigma)); p_->g.push_back(v); s += v; }
    for (auto& v : p_->g) v = (float)(v / s);
    p_->sigma = (float)noiseSigma(c);
    p_->silence(c.leadSec);
}
DstarGenerator::~DstarGenerator() = default;
double DstarGenerator::timeSec() const { return (double)p_->n / p_->cfg.rate; }

void DstarGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double step = kShapeRate / m.cfg.rate;
    const double off = m.cfg.dialOffsetHz + m.cfg.cfoHz;
    for (size_t i = 0; i < n; i++) {
        while (m.fq.size() < (size_t)m.pos + 4) m.refill();
        const size_t k = (size_t)m.pos;
        const double fr = m.pos - (double)k;
        const float a = m.fq[k], b = m.fq[k + 1];
        if (std::isnan(a) || std::isnan(b)) out[i] = cf32(0.f, 0.f);
        else {
            const double f = (a + (b - a) * fr) * kDev + off;
            m.ph = std::remainder(m.ph + 2 * kPi * f / m.cfg.rate, 2 * kPi);
            out[i] = cf32((float)(m.cfg.amp * std::cos(m.ph)), (float)(m.cfg.amp * std::sin(m.ph)));
        }
        m.pos += step;
        while (m.pos >= 1 && m.fq.size() > 8) { m.fq.pop_front(); m.pos -= 1; }
    }
    m.n += (int64_t)n;
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

namespace {
class DstarSynth : public ModeSynth {
public:
    DstarSynth(const SynthConfig& s, double rate) : rate_(rate) {
        DstarGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -dstarTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        auto all = dstarTestSchedule();
        if (s.modeOpt[0] == 1) c.txs = {all[0]}; else c.txs = all;
        gen_ = std::make_unique<DstarGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<DstarGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeDstarSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<DstarSynth>(cfg, sampleRate);
}

} // namespace dect2
