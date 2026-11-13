// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 Phase 1 test signal (see p25_gen.h): whole transmissions as C4FM, over noise.
#include "dect2/p25/p25_gen.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_proto.h"
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
constexpr int kBaud = 4800;
constexpr double kUnit = 600.0;   // Hz per level: +3 is 1800 Hz

struct FrameBuilder {
    std::vector<int8_t>& out;
    int nac;
    int statusSym = 3;            // "11": idle (outbound)
    // sync + NID + payload dibits, the status symbols put in, null dibits up to len (0: the next multiple of 36)
    void frame(int duid, const std::vector<uint8_t>& payload, int len = 0) {
        std::vector<uint8_t> d;
        for (int k = 0; k < p25::kSyncSym; k++) d.push_back((uint8_t)p25::symToDibit(p25::kSync[k]));
        uint8_t nid[64];
        p25::nidEncode(nac, duid, nid);
        for (int i = 0; i < 32; i++) d.push_back((uint8_t)((nid[2 * i] << 1) | nid[2 * i + 1]));
        d.insert(d.end(), payload.begin(), payload.end());
        int p = 0;
        for (uint8_t v : d) {
            if (p25::isStatusPos(p)) { out.push_back((int8_t)p25::dibitToSym(statusSym)); p++; }
            out.push_back((int8_t)p25::dibitToSym(v)); p++;
        }
        if (len == 0) len = (p + p25::kStatusEvery - 1) / p25::kStatusEvery * p25::kStatusEvery;
        while (p < len) { out.push_back((int8_t)p25::dibitToSym(p25::isStatusPos(p) ? statusSym : 0)); p++; }
    }
};

void bitsToDibits(const uint8_t* b, int n, std::vector<uint8_t>& d) { for (int i = 0; i + 1 < n; i += 2) d.push_back((uint8_t)((b[i] << 1) | b[i + 1])); }
void valBits(uint64_t v, int n, std::vector<uint8_t>& b) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }

// 24 hexbits (data + RS parity) as Hamming(10,6) words: 240 bits as 120 dibits, in six pieces of 20
std::vector<uint8_t> lduHex(const uint8_t* data, int k) {
    uint8_t s[24] = {};
    memcpy(s, data, (size_t)k);
    p25::rsEncode(s, 24, k);
    std::vector<uint8_t> b;
    for (int i = 0; i < 24; i++) valBits(p25::hamming10Encode(s[i]), 10, b);
    std::vector<uint8_t> d;
    bitsToDibits(b.data(), (int)b.size(), d);
    return d;
}

void ldu(FrameBuilder& fb, int duid, const std::vector<uint8_t>& hexD, uint32_t& vfn) {
    std::vector<uint8_t> p;
    auto vc = [&] {
        uint8_t bits[144]; char fr[8][23]; uint8_t dib[72];
        p25KnownVoice(vfn++, bits);
        p25::imbeUnflatten(bits, fr);
        p25::imbeInterleave(fr, dib);
        p.insert(p.end(), dib, dib + 72);
    };
    vc(); vc();
    for (int i = 0; i < 6; i++) { p.insert(p.end(), hexD.begin() + 20 * i, hexD.begin() + 20 * (i + 1)); vc(); }
    p.insert(p.end(), 16, (uint8_t)0);   // low speed data: zeros (a codeword of its cyclic code)
    vc();
    fb.frame(duid, p, p25::frameLen(duid));
}

void tsbkFrame(FrameBuilder& fb, std::vector<p25::Tsbk> ts) {
    std::vector<uint8_t> p;
    for (size_t i = 0; i < ts.size(); i++) {
        ts[i].last = i + 1 == ts.size();
        uint8_t b[96], d[98];
        p25::tsbkBits(ts[i], b);
        p25::trellisEncode(b, d);
        p.insert(p.end(), d, d + 98);
    }
    fb.frame(p25::kTsbk, p);
}

p25::Tsbk tsbk(int op, uint64_t args) { p25::Tsbk t; t.opcode = op; t.args = args; return t; }
} // namespace

void p25KnownVoice(uint32_t fn, uint8_t out[144]) {
    uint32_t s = fn * 2654435761u + 777u;
    for (int i = 0; i < 144; i++) { s = s * 1103515245u + 12345u; out[i] = (uint8_t)((s >> 16) & 1); }
}

void p25TxSymbols(const P25GenTx& tx, std::vector<int8_t>& sym) {
    FrameBuilder fb{sym, tx.nac};
    const double ldu2Sec = 2 * 864.0 / kBaud;
    if (tx.kind == P25GenTx::Control) {
        const int n = std::max(1, (int)std::lround(tx.durSec / (360.0 / kBaud)));
        const uint64_t base5 = (uint64_t)std::llround(tx.idenBaseHz / 5);
        const uint64_t spac = (uint64_t)std::llround(tx.idenSpacingHz / 125);
        const uint64_t idenUp = (1ULL << 60) | (100ULL << 51) | (180ULL << 42) | (spac << 32) | base5;   // 12.5 kHz wide, transmit 45 MHz below
        const uint64_t idenVu = (2ULL << 60) | (4ULL << 56) | ((1ULL << 13 | 800ULL) << 42) | (50ULL << 32) | (uint64_t)(460000000 / 5);   // 460 MHz, 6.25 kHz, +5 MHz
        const uint64_t net = ((uint64_t)tx.wacn << 36) | ((uint64_t)tx.sysId << 24) | ((uint64_t)tx.ctrlChannel << 8) | 0x70;
        const uint64_t rfss = ((uint64_t)tx.sysId << 40) | ((uint64_t)tx.rfss << 32) | ((uint64_t)tx.site << 24) | ((uint64_t)tx.ctrlChannel << 8) | 0x70;
        const uint64_t adj = ((uint64_t)tx.sysId << 40) | ((uint64_t)tx.rfss << 32) | ((uint64_t)tx.adjSite << 24) | ((uint64_t)tx.adjChannel << 8) | 0x70;
        const uint64_t grant = ((uint64_t)tx.grantChannel << 40) | ((uint64_t)tx.grantTg << 24) | (uint64_t)tx.grantSrc;
        const uint64_t upd = ((uint64_t)tx.grantChannel << 48) | ((uint64_t)tx.grantTg << 32) | ((uint64_t)0x2005 << 16) | 1300;
        for (int f = 0; f < n; f++) {
            if (!tx.script.empty()) {
                std::vector<p25::Tsbk> ts;
                for (const auto& m : tx.script[(size_t)f % tx.script.size()]) ts.push_back(tsbk(m.first, m.second));
                tsbkFrame(fb, ts);
                continue;
            }
            if (f % 2 == 0) tsbkFrame(fb, {tsbk(p25::kNetSts, net), tsbk(p25::kRfssSts, rfss), tsbk(p25::kIdenUp, idenUp)});
            else tsbkFrame(fb, {tsbk(p25::kAdjSts, adj), tsbk(p25::kGrpVGrant, grant), tsbk(f % 4 == 1 ? p25::kIdenUpVu : p25::kGrpVGrantUpd, f % 4 == 1 ? idenVu : upd)});
        }
        return;
    }
    if (tx.kind == P25GenTx::Data) {
        uint8_t b[96] = {};
        std::vector<uint8_t> v;
        valBits(0x15 | 0x20, 8, v);              // outbound, unconfirmed
        valBits(0xC4, 8, v);         // SAP 4: IP data
        valBits(0, 8, v);                        // MFID
        valBits((uint64_t)tx.llid, 24, v);
        valBits(0x80, 8, v);                     // full message, no blocks follow
        valBits(0, 8, v); valBits(0, 8, v); valBits(0, 8, v);
        for (int i = 0; i < 80; i++) b[i] = v[(size_t)i];
        const uint16_t c = p25::crcCcitt(b, 80);
        for (int i = 0; i < 16; i++) b[80 + i] = (uint8_t)((c >> (15 - i)) & 1);
        uint8_t d[98];
        p25::trellisEncode(b, d);
        fb.frame(p25::kPdu, std::vector<uint8_t>(d, d + 98));
        return;
    }
    fb.statusSym = 1;   // "01": busy
    p25::Crypto cr;
    cr.algId = tx.algId; cr.keyId = tx.keyId; cr.tg = tx.group ? tx.dst : 0;
    if (tx.algId != p25::kAlgClear) for (int i = 0; i < 9; i++) cr.mi[i] = (uint8_t)(0x11 * (i + 1));
    if (!tx.noHeader) {
        uint8_t h[36] = {};
        p25::hduHexbits(cr, h);
        p25::rsEncode(h, 36, 20);
        std::vector<uint8_t> b;
        for (int i = 0; i < 36; i++) valBits(p25::golay18Encode(h[i]), 18, b);
        std::vector<uint8_t> d;
        bitsToDibits(b.data(), (int)b.size(), d);
        fb.frame(p25::kHdu, d, p25::frameLen(p25::kHdu));
    }
    p25::Lc lc;
    lc.lco = tx.group ? p25::kLcGroup : p25::kLcUnit;
    lc.svcOpts = (tx.emergency ? 0x80 : 0) | (tx.algId != p25::kAlgClear ? 0x40 : 0) | 4;
    lc.tg = tx.dst; lc.dst = tx.dst; lc.src = tx.src;
    uint8_t lch[12], esh[16];
    p25::lcHexbits(lc, lch);
    p25::esHexbits(cr, esh);
    const std::vector<uint8_t> lcD = lduHex(lch, 12), esD = lduHex(esh, 16);
    const int n = std::max(1, (int)std::lround(tx.durSec / ldu2Sec));
    uint32_t vfn = 0;
    for (int f = 0; f < n; f++) { ldu(fb, p25::kLdu1, lcD, vfn); ldu(fb, p25::kLdu2, esD, vfn); }
    if (tx.noTerm) return;
    p25::Lc t; t.lco = p25::kLcTerm; t.dst = tx.dst;
    uint8_t th[24] = {};
    p25::lcHexbits(t, th);
    p25::rsEncode(th, 24, 12);
    std::vector<uint8_t> b;
    for (int i = 0; i < 12; i++) valBits(p25::golay24Encode((th[2 * i] << 6) | th[2 * i + 1]), 24, b);
    std::vector<uint8_t> d;
    bitsToDibits(b.data(), (int)b.size(), d);
    fb.frame(p25::kTdulc, d, p25::frameLen(p25::kTdulc));
    fb.frame(p25::kTdu, {}, p25::frameLen(p25::kTdu));
}

std::vector<P25GenTx> p25TestSchedule() {
    std::vector<P25GenTx> s;
    P25GenTx t;
    t.kind = P25GenTx::Control; t.durSec = 1.5; s.push_back(t);
    t = P25GenTx(); t.src = 1234567; t.dst = 1201; t.durSec = 2.0; s.push_back(t);
    t = P25GenTx(); t.src = 7654321; t.dst = 1300; t.algId = 0x84; t.keyId = 0x1234; t.durSec = 1.5; s.push_back(t);
    t = P25GenTx(); t.kind = P25GenTx::Data; t.llid = 2002; s.push_back(t);
    t = P25GenTx(); t.src = 2002; t.dst = 2003; t.group = false; t.emergency = true; t.durSec = 1.0; s.push_back(t);
    return s;
}

struct P25Generator::Impl {
    P25GenConfig cfg;
    std::deque<float> fq;        // frequency (Hz) at kShapeRate; NaN = no carrier
    double pos = 0, ph = 0;
    size_t txIdx = 0;
    bool done = false;
    int64_t n = 0;
    float sigma = 0;
    genutil::NoiseSource noise;
    explicit Impl(const P25GenConfig& c) : cfg(c), noise(c.seed) {}

    void silence(double sec) { for (int i = 0; i < (int)(sec * kShapeRate); i++) fq.push_back(NAN); }

    void refill() {
        if (done || cfg.txs.empty()) { silence(0.1); return; }
        const P25GenTx& tx = cfg.txs[txIdx];
        std::vector<int8_t> sym;
        p25TxSymbols(tx, sym);
        const int sps = (int)std::lround(kShapeRate / kBaud);
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
        for (size_t k = 0; k < sym.size(); k++) {
            const size_t c0 = k * (size_t)sps;
            for (int j = 0; j <= 2 * span; j++) y[c0 + (size_t)j] += (float)(sym[k] * kUnit) * g[(size_t)j];
        }
        for (size_t i = (size_t)span; i < N - (size_t)span; i++) fq.push_back(y[i]);
        silence(tx.gapSec);
        if (++txIdx >= cfg.txs.size()) { txIdx = 0; if (!cfg.loop) done = true; }
    }
};

double P25Generator::noiseSigma(const P25GenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double nb = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 12.5 kHz
    return std::sqrt(nb * c.rate / 12500 / 2);
}

P25Generator::P25Generator(const P25GenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->sigma = (float)noiseSigma(c);
    p_->silence(c.leadSec);
}
P25Generator::~P25Generator() = default;
double P25Generator::timeSec() const { return (double)p_->n / p_->cfg.rate; }

void P25Generator::generate(cf32* out, size_t n) {
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
    m.n += (int64_t)n;
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

namespace {
class P25Synth : public ModeSynth {
public:
    P25Synth(const SynthConfig& s, double rate) : rate_(rate) {
        P25GenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -p25Tuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        for (const auto& t : p25TestSchedule())
            if (s.modeOpt[0] == 0 || (s.modeOpt[0] == 1 && t.kind == P25GenTx::Control) || (s.modeOpt[0] == 2 && t.kind == P25GenTx::Voice)) c.txs.push_back(t);
        gen_ = std::make_unique<P25Generator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<P25Generator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeP25Synth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<P25Synth>(cfg, sampleRate);
}

} // namespace dect2
