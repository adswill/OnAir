#include "dect2/t2gen.h"
#include "dect2/t2interleave.h"
#include "dect2/dsp_compat.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace dect2 {

struct T2Generator::Impl {
    int log2N = 0;
    std::mt19937 rng;
    cf32 p1c[kP1CLen], p1a[kP1ALen], p1b[kP1BLen];
    std::vector<float> re, im;
};

static void inverseFft(int log2n, std::vector<float>& re, std::vector<float>& im) { fftSplit(re.data(), im.data(), log2n, true); }

static void buildP1(T2Generator::Impl& I, int s1, int s2code) {
    int mod[384], idx = 0;
    auto put = [&](uint8_t b) { for (int j = 7; j >= 0; j--) mod[idx++] = (b >> j) & 1; };
    for (int i = 0; i < 8; i++) put(kS1Patterns[s1][i]);
    for (int i = 0; i < 32; i++) put(kS2Patterns[s2code][i]);
    for (int i = 0; i < 8; i++) put(kS1Patterns[s1][i]);

    int rnd[384], sr = 0x4e46;
    for (int i = 0; i < 384; i++) {
        int b = (sr ^ (sr >> 1)) & 1;
        rnd[i] = b ? -1 : 1;
        sr >>= 1;
        if (b) sr |= 0x4000;
    }
    int d[385];
    d[0] = 1;
    for (int i = 1; i < 385; i++) d[i] = mod[i - 1] ? -d[i - 1] : d[i - 1];
    float c[384];
    for (int i = 0; i < 384; i++) c[i] = (float)(d[i + 1] * rnd[i]);

    auto synth = [&](int shift, cf32* out) {
        std::vector<float> re(1024, 0.f), im(1024, 0.f);
        for (int i = 0; i < 384; i++) {
            int f = kP1ActiveCarriers[i] + 86 - 512 + shift; // signed carrier index
            re[(f + 1024) % 1024] = c[i];
        }
        inverseFft(10, re, im);
        float sc = 1.0f / std::sqrt(384.0f);
        for (int n = 0; n < 1024; n++) out[n] = cf32(re[n] * sc, im[n] * sc);
    };
    cf32 shifted[1024];
    synth(0, I.p1a);
    synth(1, shifted); // +1 bin  ==  multiply by exp(+j 2 pi n / 1024)
    memcpy(I.p1c, shifted, sizeof I.p1c);
    memcpy(I.p1b, shifted + 542, sizeof I.p1b);
}

T2Generator::T2Generator(const TxParams& p) : i_(new Impl), p_(p) {
    const FftMode* m = fftModeFromS2(p.s2field1);
    if (!m || p.s2field1 > 5) m = fftModeFromS2(1);
    p_.s2field1 = m->code;
    n_ = m->n;
    g_ = guardSamples(n_, p.giIdx);
    nP2_ = m->nP2;
    PilotConfig pc;
    pc.fftCode = m->code; pc.ext = p.ext && m->kExt; pc.pp = p.pp; pc.tr = p.tr; pc.giIdx = p.giIdx;
    pm_.reset(new PilotMap(pc));
    for (int pp = 0; !pm_->valid() && pp < 8; pp++) { pc.pp = pp; pm_.reset(new PilotMap(pc)); }
    p_.pp = pc.pp;
    k_ = pm_->carriers();
    i_->log2N = (int)std::lround(std::log2((double)n_));
    i_->rng.seed(p.seed);
    i_->re.resize(n_);
    i_->im.resize(n_);
    if (p.dataSymbols > 0) symbols_ = p.dataSymbols;
    else {
        double tsym = (double)(n_ + g_) * 7.0 / 64.0 * 1e-6; // seconds at 8 MHz
        symbols_ = std::max(m->nP2 + 2, (int)std::lround(0.060 / tsym));
        symbols_ = std::min(symbols_, 400);
    }
    buildP1(*i_, p.s1, (p.s2field1 << 1) | (p.mixed ? 1 : 0));

    // L1 signalling for this configuration
    pre_.type = 0;
    pre_.bwtExt = pc.ext ? 1 : 0;
    pre_.s1 = p.s1;
    pre_.s2 = (p.s2field1 << 1) | (p.mixed ? 1 : 0);
    pre_.guardInterval = p.giIdx;
    pre_.papr = p.tr ? 2 : 0;
    pre_.l1Mod = p.l1Mod;
    pre_.pilotPattern = p_.pp;
    pre_.cellId = p.cellId; pre_.networkId = p.networkId; pre_.systemId = p.systemId;
    pre_.numFrames = 2;
    pre_.numDataSyms = symbols_ - nP2_;
    pre_.numRf = 1;
    pre_.version = 2;
    pre_.postScrambled = p.l1Scrambled ? 1 : 0;
    post_.rf.resize(1);
    post_.rf[0].freq = 522000000;
    post_.plps.resize(1);
    post_.plps[0].id = 0; post_.plps[0].type = 1; post_.plps[0].mod = 2; post_.plps[0].cod = 2; post_.plps[0].fecType = 1;
    post_.plps[0].numBlocksMax = 100; post_.plps[0].groupId = 1; post_.plps[0].timeIlLength = 3; post_.plps[0].plpMode = 1;
    post_.dyn.resize(1);
    post_.dyn[0].numBlocks = 100;
}

T2Generator::~T2Generator() {
    delete i_;
}

void T2Generator::nextFrame(std::vector<cf32>& out) {
    Impl& I = *i_;
    out.resize(frameLength());
    cf32* o = out.data();
    memcpy(o, I.p1c, sizeof I.p1c); o += kP1CLen;
    memcpy(o, I.p1a, sizeof I.p1a); o += kP1ALen;
    memcpy(o, I.p1b, sizeof I.p1b); o += kP1BLen;

    const float a = 0.70710678f;
    const float norm = 5.0f / std::sqrt(27.0f * (float)k_);
    std::vector<uint8_t> types;
    // ---- P2 payload: L1-pre, L1-post, then filler, distributed over the P2 symbols and frequency interleaved
    post_.frameIdx = frameNo_ & 0xff;
    L1Pre pre = pre_;
    auto postCells = encodeL1Post(pre, post_, nP2_, false);
    auto preCells = encodeL1Pre(pre);
    const int cP2 = pm_->p2DataCells();
    std::vector<cf32> stream;
    stream.insert(stream.end(), preCells.begin(), preCells.end());
    stream.insert(stream.end(), postCells.begin(), postCells.end());
    while ((int)stream.size() < nP2_ * cP2) {
        uint32_t r = I.rng();
        stream.push_back(cf32((r & 1) ? 0.70710678f : -0.70710678f, (r & 2) ? 0.70710678f : -0.70710678f));
    }
    std::vector<std::vector<cf32>> p2sym;
    p2Distribute(stream, nP2_, cP2, (int)preCells.size(), (int)postCells.size(), p2sym);
    for (int l = 0; l < nP2_; l++) {
        std::vector<int> H;
        freqInterleaverSeq(p_.s2field1, cP2, (l & 1) != 0, H);
        std::vector<cf32> out(cP2);
        for (int j = 0; j < cP2; j++) out[j] = p2sym[l][H[j]];
        p2sym[l] = out;
    }
    frameNo_++;
    for (int s = 0; s < symbols_; s++) {
        pm_->symbolTypes(s, symbols_, types);
        int p2idx = 0;
        std::fill(I.re.begin(), I.re.end(), 0.f);
        std::fill(I.im.begin(), I.im.end(), 0.f);
        for (int k = 0; k < k_; k++) {
            int f = k - (k_ - 1) / 2;
            size_t bin = (size_t)((f + n_) % n_);
            switch (types[k]) {
            case kCellData: {
                if (s < nP2_) {
                    cf32 v = p2sym[s][p2idx++];
                    I.re[bin] = v.real();
                    I.im[bin] = v.imag();
                } else {
                    uint32_t r = I.rng();
                    I.re[bin] = (r & 1) ? a : -a;
                    I.im[bin] = (r & 2) ? a : -a;
                }
            } break;
            case kCellP2Papr:
            case kCellTrPapr: break;
            default: I.re[bin] = pm_->pilot(s, k, types[k]).real(); break;
            }
        }
        inverseFft(I.log2N, I.re, I.im);
        for (int i = 0; i < g_; i++) o[i] = cf32(I.re[n_ - g_ + i], I.im[n_ - g_ + i]) * norm;
        for (int i = 0; i < n_; i++) o[g_ + i] = cf32(I.re[i], I.im[i]) * norm;
        o += n_ + g_;
    }
}

} // namespace dect2
