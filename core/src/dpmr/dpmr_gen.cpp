// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR test signal (see dpmr_gen.h): whole transmissions as four-level FSK, over noise.
#include "dect2/dpmr/dpmr_gen.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dpmr/dpmr_proto.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kShapeRate = 48000.0;

struct Builder {
    std::vector<int8_t>& s;
    std::vector<uint8_t>* sync;
    void sym(int v, bool isSync) { s.push_back((int8_t)v); if (sync) sync->push_back(isSync); }
    void syms(const int* v, int n, bool isSync) { for (int i = 0; i < n; i++) sym(v[i], isSync); }
    void bits(const uint8_t* b, int n) { for (int i = 0; i + 1 < n; i += 2) sym(dpmr::dibitToSym((b[i] << 1) | b[i + 1]), false); }
    void block(const uint8_t* info, int nW) { uint8_t air[288]; dpmr::blockEncode(info, nW, air); bits(air, 12 * nW); }
    void cc(int c) { int v[dpmr::kCcSym]; dpmr::ccSymbols(c, v); syms(v, dpmr::kCcSym, false); }
    void header(const dpmr::Header& h, int c, bool packet) {
        syms(packet ? dpmr::kFs4 : dpmr::kFs1, dpmr::kFsLong, true);
        uint8_t info[80];
        dpmr::headerBits(h, info);
        block(info, 10); cc(c); block(info, 10);
    }
    void frameStart(int f, int c) { if (f % 2 == 0) syms(dpmr::kFs2, dpmr::kFsShort, true); else cc(c); }
    void cch(const dpmr::Cch& x) { uint8_t info[48]; dpmr::cchBits(x, info); block(info, 6); }
    void end(uint32_t own) {
        syms(dpmr::kFs3, dpmr::kFsLong, true);
        dpmr::End e; e.own = own;
        uint8_t info[48]; dpmr::endBits(e, info); block(info, 6);
    }
};
} // namespace

void dpmrKnownVoice(uint32_t fn, uint8_t out[72]) {
    uint32_t s = fn * 2246822519u + 777u;
    for (int i = 0; i < 72; i++) { s = s * 1103515245u + 12345u; out[i] = (uint8_t)((s >> 16) & 1); }
}

void dpmrTxSymbols(const DpmrGenTx& tx, std::vector<int8_t>& sym, std::vector<uint8_t>* isSync) {
    Builder b{sym, isSync};
    for (int i = 0; i < 36; i++) b.sym(i % 2 ? -3 : 3, true);   // preamble (kept free of symbol errors, like the syncs)
    dpmr::Header h;
    h.called = tx.called; h.own = tx.own; h.callType = tx.callType; h.emergency = tx.emergency;
    if (tx.kind == DpmrGenTx::Status) {
        h.type = dpmr::kHdrStatus; h.commMode = dpmr::kData1; h.extra = tx.status;
        b.header(h, tx.cc, false);
        b.end(tx.own);
        return;
    }
    if (tx.kind == DpmrGenTx::Message || tx.kind == DpmrGenTx::Packet) {
        const int blocks = std::max(1, (int)(tx.text.size() + 20) / 21);
        h.commMode = tx.kind == DpmrGenTx::Packet ? dpmr::kPacket : dpmr::kData2; h.extra = blocks;
        b.header(h, tx.cc, tx.kind == DpmrGenTx::Packet);
        for (int f = 0; f < blocks; f++) {
            b.frameStart(f, tx.cc);
            dpmr::Cch c; c.frame = f % 4; c.commMode = h.commMode; c.callType = h.callType;
            c.idPart = (int)((f % 4) < 2 ? (tx.called >> (f % 2 ? 0 : 12)) : (tx.own >> (f % 2 ? 0 : 12))) & 0xFFF;
            b.cch(c);
            uint8_t info[192];
            dpmr::dataBits(f, tx.text.substr((size_t)f * 21, 21), info);
            b.block(info, 24);
        }
        b.end(tx.own);
        return;
    }
    h.commMode = dpmr::kVoice;
    if (!tx.noHeader) b.header(h, tx.cc, false);
    const int n = std::max(4, (int)std::lround(tx.durSec / 0.08 / 4) * 4);
    uint32_t vfn = 0;
    for (int f = 0; f < n; f++) {
        b.frameStart(f, tx.cc);
        dpmr::Cch c; c.frame = f % 4; c.commMode = dpmr::kVoice; c.callType = tx.callType; c.emergency = tx.emergency; c.slowData = 0x2A5A5;
        c.idPart = (int)((f % 4) < 2 ? (tx.called >> (f % 2 ? 0 : 12)) : (tx.own >> (f % 2 ? 0 : 12))) & 0xFFF;
        b.cch(c);
        uint8_t tch[288], pn[288];
        for (int v = 0; v < 4; v++) dpmrKnownVoice(vfn++, tch + 72 * v);
        dpmr::pn9(pn, 288);
        for (int i = 0; i < 288; i++) tch[i] ^= pn[i];
        b.bits(tch, 288);
    }
    if (!tx.noEnd) b.end(tx.own);
}

std::vector<DpmrGenTx> dpmrTestSchedule() {
    std::vector<DpmrGenTx> s;
    DpmrGenTx t;
    t.own = 1001; t.called = 200; t.callType = 1; t.cc = 7; t.durSec = 2; s.push_back(t);
    t = DpmrGenTx(); t.kind = DpmrGenTx::Status; t.own = 1002; t.called = 1001; t.callType = 0; t.status = 17; t.cc = 7; s.push_back(t);
    t = DpmrGenTx(); t.kind = DpmrGenTx::Message; t.own = 2005; t.called = 2001; t.callType = 0; t.cc = 12; t.text = "HELLO FROM DPMR PMR446 CHANNEL"; s.push_back(t);
    t = DpmrGenTx(); t.kind = DpmrGenTx::Packet; t.own = 2006; t.called = 0xFFFFFF; t.callType = 2; t.cc = 12; t.text = "PACKET DATA"; s.push_back(t);
    t = DpmrGenTx(); t.own = 2001; t.called = 2002; t.callType = 0; t.cc = 12; t.durSec = 1.5; t.emergency = true; s.push_back(t);
    return s;
}

struct DpmrGenerator::Impl {
    DpmrGenConfig cfg;
    std::deque<float> fq;        // frequency (Hz) at kShapeRate; NaN = no carrier
    double pos = 0, ph = 0;
    size_t txIdx = 0;
    bool done = false;
    float sigma = 0;
    uint32_t rng;
    genutil::NoiseSource noise;
    explicit Impl(const DpmrGenConfig& c) : cfg(c), rng(c.seed * 747796405u + 1u), noise(c.seed) {}

    void silence(double sec) { for (int i = 0; i < (int)(sec * kShapeRate); i++) fq.push_back(NAN); }
    double urand() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

    void refill() {
        if (done || cfg.txs.empty()) { silence(0.1); return; }
        const DpmrGenTx& tx = cfg.txs[txIdx];
        std::vector<int8_t> sym;
        std::vector<uint8_t> sync;
        dpmrTxSymbols(tx, sym, &sync);
        if (cfg.symErrRate > 0)
            for (size_t k = 0; k < sym.size(); k++)
                if (!sync[k] && urand() < cfg.symErrRate) sym[k] = (int8_t)(sym[k] == 3 ? 1 : sym[k] == -3 ? -1 : sym[k] == 1 ? 3 : -3);
        const int sps = 20;
        const double unit = 350.0;
        const int span = 4 * sps;
        std::vector<float> g((size_t)(2 * span + 1));
        for (int k = -span; k <= span; k++) {   // raised cosine, roll-off 0.2
            const double t = (double)k / sps, a = 0.2;
            const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
            const double d = 1 - 4 * a * a * t * t;
            const double c = std::fabs(d) < 1e-9 ? kPi / 4 : std::cos(kPi * a * t) / d;
            g[(size_t)(k + span)] = (float)(sinc * c);
        }
        const size_t N = sym.size() * (size_t)sps + (size_t)(2 * span);
        std::vector<float> y(N, 0.f);
        for (size_t k = 0; k < sym.size(); k++)
            for (int j = 0; j <= 2 * span; j++) y[k * (size_t)sps + (size_t)j] += (float)(sym[k] * unit) * g[(size_t)j];
        for (size_t i = (size_t)span; i < N - (size_t)span; i++) fq.push_back(y[i] + (float)tx.chOffsetHz);
        silence(tx.gapSec);
        if (++txIdx >= cfg.txs.size()) { txIdx = 0; if (!cfg.loop) done = true; }
    }
};

double DpmrGenerator::noiseSigma(const DpmrGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double nb = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 6.25 kHz
    return std::sqrt(nb * c.rate / 6250 / 2);
}

DpmrGenerator::DpmrGenerator(const DpmrGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->sigma = (float)noiseSigma(c);
    p_->silence(c.leadSec);
}
DpmrGenerator::~DpmrGenerator() = default;

void DpmrGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double step = kShapeRate * (1 + m.cfg.clockPpm * 1e-6) / m.cfg.rate;
    const double off = m.cfg.dialOffsetHz + m.cfg.cfoHz;
    for (size_t i = 0; i < n; i++) {
        while (m.fq.size() < (size_t)m.pos + 4) m.refill();
        const size_t k = (size_t)m.pos;
        const double fr = m.pos - (double)k;
        const float a = m.fq[k], b = m.fq[k + 1];
        if (std::isnan(a) || std::isnan(b)) out[i] = cf32(0.f, 0.f);
        else {
            const double f = a + (b - a) * fr + off;
            m.ph = std::remainder(m.ph + 2 * kPi * f / m.cfg.rate, 2 * kPi);
            out[i] = cf32((float)(m.cfg.amp * std::cos(m.ph)), (float)(m.cfg.amp * std::sin(m.ph)));
        }
        m.pos += step;
        while (m.pos >= 1 && m.fq.size() > 8) { m.fq.pop_front(); m.pos -= 1; }
    }
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

namespace {
class DpmrSynth : public ModeSynth {
public:
    DpmrSynth(const SynthConfig& s, double rate) : rate_(rate) {
        DpmrGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -dpmrTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.txs = dpmrTestSchedule();
        gen_ = std::make_unique<DpmrGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<DpmrGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeDpmrSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<DpmrSynth>(cfg, sampleRate);
}

} // namespace dect2
