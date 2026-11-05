// NXDN test signal (see nxdn_gen.h): whole transmissions as four-level FSK, over noise.
#include "dect2/nxdn/nxdn_gen.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/nxdn/nxdn_proto.h"
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

struct FrameBuilder {
    std::vector<int8_t>& out;
    int ran;
    void bitsToSyms(const uint8_t* b, int n, std::vector<int8_t>& s) { for (int i = 0; i + 1 < n; i += 2) s.push_back((int8_t)nxdn::dibitToSym((b[i] << 1) | b[i + 1])); }
    // payload: the channels after the LICH, as bits (348 or fewer, the rest filled)
    void frame(const nxdn::Lich& l, const std::vector<uint8_t>& payload) {
        for (int k = 0; k < nxdn::kFswSym; k++) out.push_back((int8_t)nxdn::kFsw[k]);
        std::vector<int8_t> s;
        const uint8_t lv = nxdn::lichEncode(l);
        for (int i = 7; i >= 0; i--) s.push_back((lv >> i) & 1 ? -3 : 3);
        std::vector<uint8_t> p = payload;
        p.resize(348, 0);
        bitsToSyms(p.data(), 348, s);
        uint8_t pn[nxdn::kPayloadSym];
        nxdn::pn9(pn, nxdn::kPayloadSym);
        for (int i = 0; i < nxdn::kPayloadSym; i++) out.push_back((int8_t)(pn[i] ? -s[(size_t)i] : s[(size_t)i]));
    }
    void sacch(int structure, const uint8_t* msg18, std::vector<uint8_t>& p) {
        uint8_t info[26] = {};
        for (int i = 0; i < 2; i++) info[i] = (uint8_t)((structure >> (1 - i)) & 1);
        for (int i = 0; i < 6; i++) info[2 + i] = (uint8_t)((ran >> (5 - i)) & 1);
        if (msg18) for (int i = 0; i < 18; i++) info[8 + i] = msg18[i];
        uint8_t air[60];
        nxdn::encode(nxdn::Ch::Sacch, info, air);
        p.insert(p.end(), air, air + 60);
    }
    void facch(const nxdn::Msg& m, std::vector<uint8_t>& p) {
        uint8_t info[80], air[144];
        nxdn::buildMsg(m, info, 80);
        nxdn::encode(nxdn::Ch::Facch1, info, air);
        p.insert(p.end(), air, air + 144);
    }
    // a frame of two FACCH1 (non-superframe SACCH)
    void facchFrame(const nxdn::Msg& a, const nxdn::Msg& b) {
        std::vector<uint8_t> p;
        sacch(0, nullptr, p);
        facch(a, p); facch(b, p);
        frame({nxdn::kRtch, nxdn::kFctSacchNs, 0, 1}, p);
    }
};
} // namespace

void nxdnKnownVoice(uint32_t fn, uint8_t out[72]) {
    uint32_t s = fn * 2654435761u + 12345u;
    for (int i = 0; i < 72; i++) { s = s * 1103515245u + 12345u; out[i] = (uint8_t)((s >> 16) & 1); }
}

void nxdnTxSymbols(const NxdnGenTx& tx, std::vector<int8_t>& sym) {
    FrameBuilder fb{sym, tx.ran};
    for (int i = 0; i < 24; i++) sym.push_back((int8_t)(i % 2 ? -3 : 3));   // preamble
    const double frameSec = nxdn::kFrameSym / (double)tx.baud;
    nxdn::Msg m;
    m.callType = tx.callType; m.src = tx.src; m.dst = tx.dst; m.cipher = tx.cipher;
    nxdn::Msg idle; idle.type = nxdn::kIdle;
    if (tx.kind == NxdnGenTx::Site) {
        const int n = std::max(1, (int)std::lround(tx.durSec / frameSec));
        for (int f = 0; f < n; f++) {
            nxdn::Msg s;
            s.type = f % 2 ? nxdn::kSrvInfo : nxdn::kSiteInfo;
            s.location = tx.location; s.channel = 0x0101; s.service = 0x3C00;
            uint8_t info[155] = {};
            info[0] = 0; info[1] = 0;
            for (int i = 0; i < 6; i++) info[2 + i] = (uint8_t)((tx.ran >> (5 - i)) & 1);
            nxdn::buildMsg(s, info + 8, 147);
            uint8_t air[300];
            nxdn::encode(nxdn::Ch::Cac, info, air);
            fb.frame({nxdn::kRcch, nxdn::kFctCac, 0, 1}, std::vector<uint8_t>(air, air + 300));
        }
        return;
    }
    if (tx.kind == NxdnGenTx::Status) {
        m.type = nxdn::kStatReq; m.status = tx.status;
        fb.facchFrame(m, m);
        return;
    }
    if (tx.kind == NxdnGenTx::Data) {
        std::vector<nxdn::Msg> q;
        const int blocks = (int)(tx.text.size() + 7) / 8;
        m.type = nxdn::kSdHeader; m.blocks = blocks;
        q.push_back(m);
        for (int b = 0; b < blocks; b++) { nxdn::Msg d; d.type = nxdn::kSdData; d.block = b; d.text = tx.text.substr((size_t)b * 8, 8); q.push_back(d); }
        nxdn::Msg rel = m; rel.type = nxdn::kTxRel;
        q.push_back(rel);
        if (q.size() % 2) q.push_back(idle);
        for (size_t i = 0; i < q.size(); i += 2) fb.facchFrame(q[i], q[i + 1]);
        return;
    }
    m.type = nxdn::kVcall;
    if (!tx.noHeader) fb.facchFrame(m, m);
    uint8_t sf[72];
    nxdn::buildMsg(m, sf, 72);
    const int n = std::max(4, (int)std::lround(tx.durSec / frameSec / 4) * 4);
    uint32_t vfn = 0;
    uint8_t sfBad[72];
    if (tx.sacchGlitchCipher) { nxdn::Msg g = m; g.cipher = tx.sacchGlitchCipher; g.keyId = 5; nxdn::buildMsg(g, sfBad, 72); }
    for (int f = 0; f < n; f++) {
        std::vector<uint8_t> p;
        const uint8_t* use = (tx.sacchGlitchCipher && f / 4 == 2) ? sfBad : sf;
        fb.sacch(3 - (f % 4), use + 18 * (f % 4), p);
        for (int v = 0; v < 4; v++) { uint8_t vb[72]; nxdnKnownVoice(vfn++, vb); p.insert(p.end(), vb, vb + 72); }
        fb.frame({nxdn::kRtch, nxdn::kFctSacchSf, 3, 1}, p);
    }
    if (!tx.noRelease) { nxdn::Msg rel = m; rel.type = nxdn::kTxRel; fb.facchFrame(rel, rel); }
}

std::vector<NxdnGenTx> nxdnTestSchedule() {
    std::vector<NxdnGenTx> s;
    NxdnGenTx t;
    t.kind = NxdnGenTx::Site; t.baud = 4800; t.durSec = 1.0; t.ran = 5; t.location = 0x0A4C2B; s.push_back(t);
    t = NxdnGenTx(); t.baud = 2400; t.src = 1001; t.dst = 200; t.callType = 1; t.ran = 1; t.durSec = 2.0; s.push_back(t);
    t = NxdnGenTx(); t.kind = NxdnGenTx::Status; t.baud = 2400; t.src = 1002; t.dst = 1001; t.callType = 4; t.status = 17; t.ran = 1; s.push_back(t);
    t = NxdnGenTx(); t.kind = NxdnGenTx::Data; t.baud = 4800; t.src = 2005; t.dst = 2001; t.callType = 4; t.text = "HELLO FROM NXDN96"; t.ran = 5; s.push_back(t);
    t = NxdnGenTx(); t.baud = 4800; t.src = 2001; t.dst = 2002; t.callType = 4; t.ran = 5; t.durSec = 1.5; s.push_back(t);
    return s;
}

struct NxdnGenerator::Impl {
    NxdnGenConfig cfg;
    std::deque<float> fq;        // frequency (Hz) at kShapeRate; NaN = no carrier
    double pos = 0, ph = 0;
    size_t txIdx = 0;
    bool done = false;
    int64_t n = 0;
    float sigma = 0;
    uint32_t rng;
    genutil::NoiseSource noise;
    explicit Impl(const NxdnGenConfig& c) : cfg(c), rng(c.seed * 747796405u + 1u), noise(c.seed) {}

    void silence(double sec) { for (int i = 0; i < (int)(sec * kShapeRate); i++) fq.push_back(NAN); }
    double urand() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

    void refill() {
        if (done || cfg.txs.empty()) { silence(0.1); return; }
        const NxdnGenTx& tx = cfg.txs[txIdx];
        std::vector<int8_t> sym;
        nxdnTxSymbols(tx, sym);
        // symbol errors on everything after the preamble except the frame syncs
        if (cfg.symErrRate > 0)
            for (size_t k = 24; k < sym.size(); k++) {
                if ((k - 24) % nxdn::kFrameSym < (size_t)nxdn::kFswSym) continue;
                if (urand() < cfg.symErrRate) sym[k] = (int8_t)(sym[k] == 3 ? 1 : sym[k] == -3 ? -1 : sym[k] == 1 ? 3 : -3);
            }
        const int sps = (int)std::lround(kShapeRate / tx.baud);
        const double unit = tx.baud == 4800 ? 800.0 : 350.0;
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
            const size_t c0 = k * (size_t)sps;   // the pulse centre is c0 + span
            for (int j = 0; j <= 2 * span; j++) y[c0 + (size_t)j] += (float)(sym[k] * unit) * g[(size_t)j];
        }
        for (size_t i = (size_t)span; i < N - (size_t)span; i++) fq.push_back(y[i]);
        silence(tx.gapSec);
        if (++txIdx >= cfg.txs.size()) { txIdx = 0; if (!cfg.loop) done = true; }
    }
};

double NxdnGenerator::noiseSigma(const NxdnGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double nb = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 12.5 kHz
    return std::sqrt(nb * c.rate / 12500 / 2);
}

NxdnGenerator::NxdnGenerator(const NxdnGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->sigma = (float)noiseSigma(c);
    p_->silence(c.leadSec);
}
NxdnGenerator::~NxdnGenerator() = default;
double NxdnGenerator::timeSec() const { return (double)p_->n / p_->cfg.rate; }

void NxdnGenerator::generate(cf32* out, size_t n) {
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
class NxdnSynth : public ModeSynth {
public:
    NxdnSynth(const SynthConfig& s, double rate) : rate_(rate) {
        NxdnGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -nxdnTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        for (const auto& t : nxdnTestSchedule())
            if (s.modeOpt[0] == 0 || (s.modeOpt[0] == 1 && t.baud == 2400) || (s.modeOpt[0] == 2 && t.baud == 4800)) c.txs.push_back(t);
        gen_ = std::make_unique<NxdnGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<NxdnGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeNxdnSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<NxdnSynth>(cfg, sampleRate);
}

} // namespace dect2
