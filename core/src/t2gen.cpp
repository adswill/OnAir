#include "dect2/t2gen.h"
#include "dect2/t2interleave.h"
#include "dect2/t2fec.h"
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
    pre_.lite = (p.s1 == 3 || p.s1 == 4) ? 1 : 0;
    if (p.payload) {
        L1PlpConf& c = post_.plps[0];
        c.mod = p.plpMod; c.cod = p.plpCod; c.rotation = p.plpRot ? 1 : 0; c.fecType = p.plpShort ? 0 : 1; c.timeIlLength = p.plpTi;
        PlpFec f; f.shortFrame = p.plpShort; f.rate = p.plpCod; f.mod = p.plpMod; f.rotation = p.plpRot;
        const FecDims d = fecDims(f);
        // how many FEC blocks fit: the P2 cells left after L1, plus the data cells of every data symbol
        post_.frameIdx = 0;
        L1Pre pre = pre_;
        const int nPost = (int)encodeL1Post(pre, post_, nP2_, false).size();
        const int nPre = (int)encodeL1Pre(pre).size();
        long cap = (long)nP2_ * pm_->p2DataCells() - nPre - nPost;
        std::vector<uint8_t> types;
        for (int s = nP2_; s < symbols_; s++) {
            pm_->symbolTypes(s, symbols_, types);
            for (int k = 0; k < k_; k++) cap += types[k] == kCellData;
        }
        plpBlocks_ = d.ok ? (int)(cap / d.cellsPerBlock) : 0;
        c.numBlocksMax = plpBlocks_;
        post_.dyn[0].numBlocks = plpBlocks_;
        post_.dyn[0].start = 0;
    }
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
    // PLP payload cells (only with p_.payload): they follow L1 in the P2 symbols and carry on through the data symbols
    std::vector<cf32> plp;
    size_t plpUsed = 0;
    if (p_.payload && plpBlocks_ > 0) {
        PlpFec f; f.shortFrame = p_.plpShort; f.rate = p_.plpCod; f.mod = p_.plpMod; f.rotation = p_.plpRot;
        const FecDims d = fecDims(f);
        const LdpcCode& ldpc = ldpcFor(f);
        const BchCode& bch = bchFor(f);
        const auto& map = bitInterleaverMap(f);
        const uint8_t* rnd = bbRandomiser();
        lastBb_.assign(plpBlocks_, {});
        std::vector<cf32> cells;
        for (int b = 0; b < plpBlocks_; b++) {
            // a transport stream of 188-byte packets in normal mode: each unit is the CRC-8 of the previous packet, then 187 bytes
            const int npk = (d.kBch - 80) / 1504;
            std::vector<uint8_t> bits(d.kBch, 0);
            BbHeader h;
            h.tsGs = 3; h.sisMis = 1; h.ccmAcm = 1; h.upl = 1504; h.dfl = npk * 1504; h.sync = 0x47; h.syncd = 0;
            buildBbHeader(h, bits.data());
            uint8_t prevCrc = 0;
            for (int k = 0; k < npk; k++) {
                uint8_t pay[187];
                for (int j = 0; j < 187; j++) pay[j] = (uint8_t)(I.rng() >> 11);
                size_t o = 80 + (size_t)k * 1504;
                auto putByte = [&](size_t at, uint8_t v) { for (int q = 0; q < 8; q++) bits[at + q] = (v >> (7 - q)) & 1; };
                putByte(o, prevCrc);
                for (int j = 0; j < 187; j++) putByte(o + 8 + (size_t)j * 8, pay[j]);
                // CRC-8 (x^8+x^7+x^6+x^4+x^2+1) of the 187 payload bytes
                unsigned crc = 0;
                for (int j = 0; j < 187; j++) for (int q = 7; q >= 0; q--) { unsigned fb = ((crc >> 7) & 1) ^ ((pay[j] >> q) & 1); crc = (crc << 1) & 0xff; if (fb) crc ^= 0xD5; }
                prevCrc = (uint8_t)crc;
            }
            lastBb_[b] = bits;
            for (int i = 0; i < d.kBch; i++) bits[i] ^= rnd[i];
            bch.encode(bits, d.kBch);
            ldpc.encode(bits);
            std::vector<uint16_t> lab(d.cellsPerBlock);
            for (int c = 0; c < d.cellsPerBlock; c++) { unsigned l = 0; for (int k = 0; k < d.bitsPerCell; k++) l = (l << 1) | bits[map[(size_t)c * d.bitsPerCell + k]]; lab[c] = (uint16_t)l; }
            std::vector<cf32> cl;
            qamMapBlock(f, lab, cl);
            cells.insert(cells.end(), cl.begin(), cl.end());
        }
        cellInterleave(f, plpBlocks_, p_.plpTi, cells, plp);
    }
    while ((int)stream.size() < nP2_ * cP2) {
        if (plpUsed < plp.size()) { stream.push_back(plp[plpUsed++]); continue; }
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
        int p2idx = 0, dIdx = 0;
        // data symbol: the cells in logical order, spread over the data carriers by the frequency interleaver
        std::vector<cf32> logical, carrierCell;
        std::vector<int> Hd;
        if (s >= nP2_) {
            int cD = 0;
            for (int k = 0; k < k_; k++) cD += types[k] == kCellData;
            logical.resize(cD);
            for (int j = 0; j < cD; j++) {
                if (plpUsed < plp.size()) logical[j] = plp[plpUsed++];
                else { uint32_t r = I.rng(); logical[j] = cf32((r & 1) ? a : -a, (r & 2) ? a : -a); }
            }
            freqInterleaverSeq(p_.s2field1, cD, (s & 1) != 0, Hd);
        }
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
                    const cf32 v = logical[Hd[dIdx++]];
                    I.re[bin] = v.real();
                    I.im[bin] = v.imag();
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
