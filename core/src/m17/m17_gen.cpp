// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 test signal (see m17_gen.h): whole M17 transmissions as 4FSK, over noise.
#include "dect2/m17/m17_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/m17/m17_codec2.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kSps = 10;                 // 48 kHz shaping rate
constexpr double kShapeRate = 4800.0 * kSps;

// A synthetic voice at 8 kHz: a glottal pulse train (pitch gliding 100 - 170 Hz) through three formant resonators that move between
// vowels, with a syllable rhythm. Enough for Codec 2 to have something speech-like to code.
struct Voice {
    double t = 0, ph = 0;
    double y1[3] = {}, y2[3] = {};
    std::mt19937 rng;
    explicit Voice(uint32_t s) : rng(s) {}
    int16_t next() {
        static const double F[5][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {530, 1840, 2480}, {570, 840, 2410}, {300, 870, 2240}};
        const double fs = 8000;
        t += 1 / fs;
        const double pitch = 135 + 35 * std::sin(2 * kPi * 0.7 * t);
        ph += pitch / fs;
        double x = 0;
        if (ph >= 1) { ph -= 1; x = 1; }
        x += 0.02 * dect2::normal_distribution<double>(0, 1)(rng);
        const double seg = t * 4;   // a vowel every 250 ms, blended
        const int a = (int)seg % 5, b = (a + 1) % 5;
        const double f = seg - std::floor(seg);
        double out = 0;
        for (int k = 0; k < 3; k++) {
            const double fc = F[a][k] * (1 - f) + F[b][k] * f, bw = 80 + 40 * k;
            const double r = std::exp(-kPi * bw / fs), c = 2 * r * std::cos(2 * kPi * fc / fs);
            const double y = x + c * y1[k] - r * r * y2[k];
            y2[k] = y1[k]; y1[k] = y;
            out += y * (k == 0 ? 1.0 : k == 1 ? 0.6 : 0.3);
        }
        const double env = 0.25 + 0.75 * std::pow(std::fabs(std::sin(2 * kPi * 1.9 * t)), 0.6);
        return (int16_t)std::max(-32000.0, std::min(32000.0, out * env * 1400));
    }
};

void push(std::vector<int8_t>& v, const int8_t* s, int n) { v.insert(v.end(), s, s + n); }
} // namespace

void m17KnownPayload(uint16_t fn, uint8_t out[16]) {
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(((fn & 0x7FFF) * 16 + i) * 37 + 11);
}

void m17TxSymbols(const M17GenTx& tx, bool codec2, std::vector<int8_t>& sym, uint32_t seed) {
    sym.clear();
    int8_t s8[8], f[192];
    // preamble: +3, -3 ... (ends on -3, the opposite of the LSF sync's first symbol)
    for (int i = 0; i < 192; i++) sym.push_back((int8_t)(i % 2 ? -3 : 3));
    m17::Lsf l;
    l.src = m17::encodeCallsign(tx.src);
    l.dst = m17::encodeCallsign(tx.dst);
    std::mt19937 rng(seed * 7919u + 3);
    const bool clear = tx.packet || tx.enc == 0;
    l.type = tx.packet ? m17::makeType(false, 0, 0, 0, tx.can) : m17::makeType(true, tx.dataType, tx.enc, clear ? tx.meta : tx.encSub, tx.can);
    auto setMeta = [&](int superframe) {
        memset(l.meta, 0, 14);
        if (!clear) { for (auto& b : l.meta) b = (uint8_t)rng(); return; }   // a nonce
        if (tx.packet) return;
        if (tx.meta == 0 && !tx.text.empty()) m17::textBlockPack(tx.text, superframe % m17::textBlockCount(tx.text), l.meta);
        else if (tx.meta == 1) {
            m17::Gnss g = tx.gnss;
            if (g.velValid) {   // move along the bearing
                const double d = g.speedKmh / 3.6 * superframe * 0.24;
                g.lat += d * std::cos(g.bearing * kPi / 180) / 111320.0;
                g.lon += d * std::sin(g.bearing * kPi / 180) / (111320.0 * std::cos(g.lat * kPi / 180));
            }
            m17::gnssPack(g, l.meta);
        } else if (tx.meta == 2) {
            const uint64_t a = m17::encodeCallsign(tx.ext1), b = tx.ext2.empty() ? 0 : m17::encodeCallsign(tx.ext2);
            for (int i = 0; i < 6; i++) { l.meta[i] = (uint8_t)(a >> (40 - 8 * i)); l.meta[6 + i] = (uint8_t)(b >> (40 - 8 * i)); }
        }
    };
    setMeta(0);
    uint8_t lsf[30], bits[368];
    m17::lsfPack(l, lsf);
    m17::buildLsf(lsf, bits);
    m17::frameSymbols(m17::kSyncLsf, bits, f);
    push(sym, f, 192);
    if (tx.packet) {
        std::vector<uint8_t> d;
        d.push_back(0x05);
        for (char c : tx.text) d.push_back((uint8_t)c);
        d.push_back(0);
        const uint16_t c = m17::crc16(d.data(), d.size());
        d.push_back((uint8_t)(c >> 8)); d.push_back((uint8_t)c);
        const int nf = (int)(d.size() + 24) / 25;
        for (int k = 0; k < nf; k++) {
            uint8_t ch[25] = {};
            const size_t o = (size_t)k * 25, n = std::min<size_t>(25, d.size() - o);
            memcpy(ch, d.data() + o, n);
            const bool eof = k == nf - 1;
            m17::buildPacket(ch, eof, eof ? (int)n : k, bits);
            m17::frameSymbols(m17::kSyncPacket, bits, f);
            push(sym, f, 192);
        }
    } else {
        const int nf = std::max(1, (int)std::lround(tx.durSec / 0.04));
        std::unique_ptr<M17Codec2> c2;
        std::unique_ptr<Voice> v;
        if (codec2 && clear && tx.dataType >= 2) {
            c2 = std::make_unique<M17Codec2>(tx.dataType == 3 ? 1600 : 3200);
            if (!c2->ok()) c2.reset();
            else if (!tx.pullPcm) v = std::make_unique<Voice>(seed);
        }
        const char* dataText = "M17 data stream from the OnAir test signal. ";
        size_t dpos = 0;
        for (int fn = 0; fn < nf; fn++) {
            if (fn > 0 && fn % 6 == 0) { setMeta(fn / 6); m17::lsfPack(l, lsf); }   // a new META at each superframe
            uint8_t pl[16];
            if (!clear) for (auto& b : pl) b = (uint8_t)rng();
            else if (c2) {
                std::vector<int16_t> pcm((size_t)c2->samplesPerFrame());
                const int per = tx.dataType == 3 ? 1 : 2;
                for (int k = 0; k < per; k++) {
                    if (tx.pullPcm) tx.pullPcm(pcm.data(), (int)pcm.size());
                    else if (v) { for (auto& s : pcm) s = v->next(); }
                    else std::fill(pcm.begin(), pcm.end(), 0);
                    c2->encode(pcm.data(), pl + 8 * k);
                }
                if (tx.dataType == 3) for (int k = 0; k < 8; k++) pl[8 + k] = (uint8_t)dataText[dpos++ % strlen(dataText)];
            } else {
                m17KnownPayload((uint16_t)fn, pl);
                if (tx.dataType != 2) for (int k = 0; k < 8; k++) pl[8 + k] = (uint8_t)dataText[dpos++ % strlen(dataText)];
            }
            const uint16_t fnw = (uint16_t)((fn & 0x7FFF) | (fn == nf - 1 ? 0x8000 : 0));
            m17::buildStream(lsf, fn % 6, fnw, pl, bits);
            m17::frameSymbols(m17::kSyncStream, bits, f);
            push(sym, f, 192);
        }
    }
    m17::syncSymbols(m17::kSyncEot, s8);
    for (int i = 0; i < 24; i++) push(sym, s8, 8);
}

std::vector<M17GenTx> m17TestSchedule() {
    std::vector<M17GenTx> v;
    M17GenTx t;
    t = {}; t.src = "N0CALL"; t.dst = "@ALL"; t.can = 0; t.dataType = 2; t.meta = 0; t.text = "OnAir M17 test signal, 73"; t.durSec = 3.0; t.gapSec = 0.8; v.push_back(t);
    t = {}; t.packet = true; t.src = "N0CALL"; t.dst = "TEST1"; t.can = 0; t.text = "Hello from the OnAir M17 test signal"; t.gapSec = 0.8; v.push_back(t);
    t = {}; t.src = "N0CALL/P"; t.dst = "ECHO"; t.can = 3; t.dataType = 3; t.meta = 1;
    t.gnss.source = 15; t.gnss.station = 1; t.gnss.posValid = t.gnss.altValid = t.gnss.velValid = true; t.gnss.radius = 2;
    t.gnss.lat = 48.2085; t.gnss.lon = 16.3725; t.gnss.altM = 180; t.gnss.speedKmh = 36; t.gnss.bearing = 90;
    t.durSec = 2.4; t.gapSec = 0.8; v.push_back(t);
    t = {}; t.src = "N0CALL"; t.dst = "@ALL"; t.can = 5; t.dataType = 2; t.enc = 2; t.encSub = 2; t.durSec = 1.2; t.gapSec = 1.2; v.push_back(t);
    return v;
}

struct M17Generator::Impl {
    M17GenConfig cfg;
    std::vector<float> rrc;
    float txGain = 1;
    std::deque<float> fq;          // frequency (in units of 800 Hz) at 48 kHz, NaN = carrier off
    size_t txIdx = 0;
    bool done = false;
    double pos = 0;                // position in fq of the next output sample (48 kHz units)
    double ph = 0;
    int64_t n = 0;
    float sigma = 0;
    bool coded = false;
    genutil::NoiseSource noise;
    explicit Impl(const M17GenConfig& c) : cfg(c), noise(c.seed) {}

    void silence(double sec) { for (int i = 0; i < (int)(sec * kShapeRate); i++) fq.push_back(NAN); }
    void refill() {
        if (done || cfg.txs.empty()) { silence(0.1); return; }
        const M17GenTx& tx = cfg.txs[txIdx];
        std::vector<int8_t> sym;
        m17TxSymbols(tx, coded, sym, cfg.seed + (uint32_t)txIdx);
        // impulses through the RRC: the shaped frequency, with half a filter of ramp at each end
        const int L = (int)rrc.size(), N = (int)sym.size() * kSps + L;
        std::vector<float> y((size_t)N, 0.f);
        for (size_t k = 0; k < sym.size(); k++)
            for (int j = 0; j < L; j++) y[k * kSps + (size_t)j] += sym[k] * rrc[(size_t)j] * txGain;
        for (float v : y) fq.push_back(v);
        silence(tx.gapSec);
        if (++txIdx >= cfg.txs.size()) { txIdx = 0; if (!cfg.loop) done = true; }
    }
};

double M17Generator::noiseSigma(const M17GenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double n9 = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 9 kHz
    return std::sqrt(n9 * c.rate / 9000 / 2);
}

M17Generator::M17Generator(const M17GenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->rrc = m17::rrcTaps(kSps, 8);
    double s = 0;
    for (float v : p_->rrc) s += v;
    p_->txGain = (float)(kSps / s);   // a constant symbol gives its own value
    p_->sigma = (float)noiseSigma(c);
    p_->coded = c.voice && m17Codec2Available();
    p_->silence(c.leadSec);
}
M17Generator::~M17Generator() = default;
double M17Generator::timeSec() const { return (double)p_->n / p_->cfg.rate; }
bool M17Generator::codedVoice() const { return p_->coded; }

void M17Generator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    const double step = kShapeRate / m.cfg.rate;
    const double off = m.cfg.dialOffsetHz + m.cfg.cfoHz, dev = 800.0 * m.cfg.devScale;
    for (size_t i = 0; i < n; i++) {
        while (m.fq.size() < (size_t)m.pos + 4) m.refill();
        const size_t k = (size_t)m.pos;
        const double fr = m.pos - (double)k;
        const float a = m.fq[k], b = m.fq[k + 1];
        if (std::isnan(a) || std::isnan(b)) out[i] = cf32(0.f, 0.f);
        else {
            const double f = (a + (b - a) * fr) * dev + off;
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
class M17Synth : public ModeSynth {
public:
    M17Synth(const SynthConfig& s, double rate) : rate_(rate) {
        M17GenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -m17Tuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        auto all = m17TestSchedule();
        if (s.modeOpt[0] == 1) c.txs = {all[0]};
        else if (s.modeOpt[0] == 2) c.txs = {all[1]};
        else c.txs = all;
        gen_ = std::make_unique<M17Generator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<M17Generator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeM17Synth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<M17Synth>(cfg, sampleRate);
}

} // namespace dect2
