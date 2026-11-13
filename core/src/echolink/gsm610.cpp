// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM 06.10 full-rate codec: see gsm610.h. Section numbers below are those of the GSM 06.10 / ETSI EN 300 961 algorithm description.
#include "dect2/echolink/gsm610.h"
#include <cstring>

namespace dect2 {
namespace echolink {

namespace {

using word = int16_t;
using longword = int32_t;
constexpr int kMinWord = -32768, kMaxWord = 32767;
constexpr longword kMinLong = INT32_MIN, kMaxLong = INT32_MAX;

// ---- 5.1 basic arithmetic --------------------------------------------------------------------------------------------------------------
inline word sat(longword x) { return (word)(x < kMinWord ? kMinWord : x > kMaxWord ? kMaxWord : x); }
inline word add(int a, int b) { return sat((longword)a + b); }
inline word sub(int a, int b) { return sat((longword)a - b); }
inline word mult(word a, word b) { return (a == kMinWord && b == kMinWord) ? (word)kMaxWord : (word)(((longword)a * b) >> 15); }
inline word mult_r(word a, word b) { return (a == kMinWord && b == kMinWord) ? (word)kMaxWord : (word)(((longword)a * b + 16384) >> 15); }
inline word gabs(word a) { return a < 0 ? (a == kMinWord ? (word)kMaxWord : (word)-a) : a; }
inline longword L_add(longword a, longword b) {
    const int64_t s = (int64_t)a + b;
    return s < kMinLong ? kMinLong : s > kMaxLong ? kMaxLong : (longword)s;
}
// the number of left shifts that normalise a (0 for 0)
inline int norm(longword a) {
    if (a == 0) return 0;
    if (a < 0) {
        if (a <= -1073741824) return 0;
        a = ~a;
    }
    int n = 0;
    while (a < 0x40000000) { a <<= 1; n++; }
    return n;
}
inline word asr(word a, int n) {
    if (n >= 16) return (word)-(a < 0);
    if (n <= -16) return 0;
    if (n < 0) return (word)(a << -n);
    return (word)(a >> n);
}
inline word asl(word a, int n) {
    if (n >= 16) return 0;
    if (n <= -16) return (word)-(a < 0);
    if (n < 0) return asr(a, -n);
    return (word)(a << n);
}
// 0 <= num <= denum, 15-bit quotient
inline word gdiv(word num, word denum) {
    if (num == 0) return 0;
    longword L_num = num, L_denum = denum;
    word d = 0;
    for (int k = 0; k < 15; k++) {
        d = (word)(d << 1);
        L_num <<= 1;
        if (L_num >= L_denum) { L_num -= L_denum; d++; }
    }
    return d;
}

// ---- tables (section 5.4) ----------------------------------------------------------------------------------------------------------------
const word kDLB[4] = {6554, 16384, 26214, 32767};
const word kQLB[4] = {3277, 11469, 21299, 32767};
const word kH[11] = {-134, -374, 0, 2054, 5741, 8192, 5741, 2054, 0, -374, -134};
const word kNRFAC[8] = {29128, 26215, 23832, 21846, 20165, 18725, 17476, 16384};
const word kFAC[8] = {18431, 20479, 22527, 24575, 26623, 28671, 30719, 32767};
const word kA[8] = {20480, 20480, 20480, 20480, 13964, 15360, 8534, 9036};
const word kB[8] = {0, 0, 2048, -2560, 94, -1792, -341, -1144};
const word kMIC[8] = {-32, -32, -16, -16, -8, -8, -4, -4};
const word kMAC[8] = {31, 31, 15, 15, 7, 7, 3, 3};
const word kINVA[8] = {13107, 13107, 13107, 13107, 19223, 17476, 31454, 29708};

// ---- 4.2.4 - 4.2.7 LPC analysis ---------------------------------------------------------------------------------------------------------
void autocorrelation(word* s, longword* L_ACF) {
    word smax = 0;
    for (int k = 0; k < 160; k++) { const word t = gabs(s[k]); if (t > smax) smax = t; }
    int scalauto = 0;
    if (smax != 0) scalauto = 4 - norm((longword)smax << 16);
    if (scalauto > 0) {
        const word f = (word)(16384 >> (scalauto - 1));
        for (int k = 0; k < 160; k++) s[k] = mult_r(s[k], f);
    }
    for (int k = 0; k <= 8; k++) {
        longword L = 0;
        for (int i = k; i < 160; i++) L += (longword)s[i] * s[i - k];
        L_ACF[k] = L << 1;
    }
    if (scalauto > 0)
        for (int k = 0; k < 160; k++) s[k] = (word)(s[k] << scalauto);
}

void reflectionCoefficients(const longword* L_ACF, word* r) {
    if (L_ACF[0] == 0) { for (int i = 0; i < 8; i++) r[i] = 0; return; }
    const int temp0 = norm(L_ACF[0]);
    word ACF[9], P[9], K[9];
    for (int i = 0; i <= 8; i++) ACF[i] = (word)((longword)((uint32_t)L_ACF[i] << temp0) >> 16);
    for (int i = 1; i <= 7; i++) K[i] = ACF[i];
    for (int i = 0; i <= 8; i++) P[i] = ACF[i];
    for (int n = 1; n <= 8; n++, r++) {
        const word t = gabs(P[1]);
        if (P[0] < t) {
            for (int i = n; i <= 8; i++) *r++ = 0;
            return;
        }
        *r = gdiv(t, P[0]);
        if (P[1] > 0) *r = (word)-*r;
        if (n == 8) return;
        P[0] = add(P[0], mult_r(P[1], *r));
        for (int m = 1; m <= 8 - n; m++) {
            P[m] = add(P[m + 1], mult_r(K[m], *r));
            K[m] = add(K[m], mult_r(P[m + 1], *r));
        }
    }
}

void transformToLAR(word* r) {
    for (int i = 0; i < 8; i++) {
        word t = gabs(r[i]);
        if (t < 22118) t = (word)(t >> 1);
        else if (t < 31130) t = (word)(t - 11059);
        else t = (word)((t - 26112) << 2);
        r[i] = r[i] < 0 ? (word)-t : t;
    }
}

void quantizeLAR(const word* LAR, int16_t* LARc) {
    for (int i = 0; i < 8; i++) {
        word t = mult(kA[i], LAR[i]);
        t = add(t, kB[i]);
        t = add(t, 256);
        t = (word)(t >> 9);
        LARc[i] = (int16_t)(t > kMAC[i] ? kMAC[i] - kMIC[i] : (t < kMIC[i] ? 0 : t - kMIC[i]));
    }
}

// ---- 4.2.8 - 4.2.10 short-term filters ---------------------------------------------------------------------------------------------------
void decodeLAR(const int16_t* LARc, word* LARpp) {
    for (int i = 0; i < 8; i++) {
        word t = (word)(add(LARc[i], kMIC[i]) << 10);
        t = sub(t, kB[i] * 2);
        t = mult_r(kINVA[i], t);
        LARpp[i] = add(t, t);
    }
}
void coeff0_12(const word* j1, const word* j, word* LARp) {
    for (int i = 0; i < 8; i++) { LARp[i] = add(j1[i] >> 2, j[i] >> 2); LARp[i] = add(LARp[i], j1[i] >> 1); }
}
void coeff13_26(const word* j1, const word* j, word* LARp) {
    for (int i = 0; i < 8; i++) LARp[i] = add(j1[i] >> 1, j[i] >> 1);
}
void coeff27_39(const word* j1, const word* j, word* LARp) {
    for (int i = 0; i < 8; i++) { LARp[i] = add(j1[i] >> 2, j[i] >> 2); LARp[i] = add(LARp[i], j[i] >> 1); }
}
void coeff40_159(const word* j, word* LARp) { for (int i = 0; i < 8; i++) LARp[i] = j[i]; }

void LARpToRp(word* LARp) {
    for (int i = 0; i < 8; i++) {
        if (LARp[i] < 0) {
            const word t = LARp[i] == kMinWord ? (word)kMaxWord : (word)-LARp[i];
            LARp[i] = (word)-((t < 11059) ? (word)(t << 1) : ((t < 20070) ? (word)(t + 11059) : add(t >> 2, 26112)));
        } else {
            const word t = LARp[i];
            LARp[i] = (t < 11059) ? (word)(t << 1) : ((t < 20070) ? (word)(t + 11059) : add(t >> 2, 26112));
        }
    }
}

void stAnalysis(word* u, const word* rp, int n, word* s) {
    for (; n--; s++) {
        word di = *s, sav = *s;
        for (int i = 0; i < 8; i++) {
            const word ui = u[i], rpi = rp[i];
            u[i] = sav;
            sav = add(ui, mult_r(rpi, di));
            di = add(di, mult_r(rpi, ui));
        }
        *s = di;
    }
}

void stSynthesis(word* v, const word* rrp, int n, const word* wt, word* sr) {
    while (n--) {
        word sri = *wt++;
        for (int i = 7; i >= 0; i--) {
            sri = sub(sri, mult_r(rrp[i], v[i]));
            v[i + 1] = add(v[i], mult_r(rrp[i], sri));
        }
        *sr++ = v[0] = sri;
    }
}

// ---- 4.2.11 - 4.2.12 long-term predictor -------------------------------------------------------------------------------------------------
void ltpParameters(const word* d, const word* dp, int16_t* bc_out, int16_t* Nc_out) {
    word dmax = 0;
    for (int k = 0; k < 40; k++) { const word t = gabs(d[k]); if (t > dmax) dmax = t; }
    int temp = 0;
    if (dmax != 0) temp = norm((longword)dmax << 16);
    const int scal = temp > 6 ? 0 : 6 - temp;
    word wt[40];
    for (int k = 0; k < 40; k++) wt[k] = (word)(d[k] >> scal);
    longword L_max = 0;
    int Nc = 40;
    for (int lambda = 40; lambda <= 120; lambda++) {
        longword L = 0;
        for (int k = 0; k < 40; k++) L += (longword)wt[k] * dp[k - lambda];
        if (L > L_max) { Nc = lambda; L_max = L; }
    }
    *Nc_out = (int16_t)Nc;
    L_max <<= 1;
    L_max = L_max >> (6 - scal);
    longword L_power = 0;
    for (int k = 0; k < 40; k++) { const longword t = dp[k - Nc] >> 3; L_power += t * t; }
    L_power <<= 1;
    if (L_max <= 0) { *bc_out = 0; return; }
    if (L_max >= L_power) { *bc_out = 3; return; }
    const int t2 = norm(L_power);
    const word R = (word)((longword)((uint32_t)L_max << t2) >> 16);
    const word S = (word)((longword)((uint32_t)L_power << t2) >> 16);
    int bc = 0;
    for (; bc <= 2; bc++) if (R <= mult(S, kDLB[bc])) break;
    *bc_out = (int16_t)bc;
}

void ltpAnalysisFilter(int bc, int Nc, const word* dp, const word* d, word* dpp, word* e) {
    const word bp = kQLB[bc];
    for (int k = 0; k < 40; k++) {
        dpp[k] = mult_r(bp, dp[k - Nc]);
        e[k] = sub(d[k], dpp[k]);
    }
}

// ---- 4.2.13 - 4.2.17 RPE ----------------------------------------------------------------------------------------------------------------
void weightingFilter(const word* e /* e[-5..44] valid */, word* x) {
    for (int k = 0; k < 40; k++) {
        longword L = 4096;
        for (int i = 0; i < 11; i++) L += (longword)e[k - 5 + i] * kH[i];
        L >>= 13;
        x[k] = sat(L);
    }
}

void gridSelection(const word* x, word* xM, int16_t* Mc_out) {
    longword EM = 0;
    int Mc = 0;
    for (int m = 0; m < 4; m++) {
        longword L = 0;
        for (int i = 0; i <= 12; i++) { const longword t = x[m + 3 * i] >> 2; L += t * t; }
        L <<= 1;
        if (m == 0 || L > EM) { EM = L; Mc = m; }
    }
    for (int i = 0; i <= 12; i++) xM[i] = x[Mc + 3 * i];
    *Mc_out = (int16_t)Mc;
}

void xmaxcToExpMant(word xmaxc, word* exp_out, word* mant_out) {
    word exp = 0;
    if (xmaxc > 15) exp = (word)((xmaxc >> 3) - 1);
    word mant = (word)(xmaxc - (exp << 3));
    if (mant == 0) { exp = -4; mant = 7; }
    else {
        while (mant <= 7) { mant = (word)(mant << 1 | 1); exp--; }
        mant -= 8;
    }
    *exp_out = exp; *mant_out = mant;
}

void apcmQuantize(const word* xM, int16_t* xMc, word* mant_out, word* exp_out, int16_t* xmaxc_out) {
    word xmax = 0;
    for (int i = 0; i <= 12; i++) { const word t = gabs(xM[i]); if (t > xmax) xmax = t; }
    word exp = 0;
    word temp = (word)(xmax >> 9);
    int itest = 0;
    for (int i = 0; i <= 5; i++) {
        itest |= (temp <= 0);
        temp = (word)(temp >> 1);
        if (itest == 0) exp++;
    }
    const word xmaxc = add(xmax >> (exp + 5), exp << 3);
    word mant;
    xmaxcToExpMant(xmaxc, &exp, &mant);
    const int t1 = 6 - exp;
    const word t2 = kNRFAC[mant];
    for (int i = 0; i <= 12; i++) {
        word t = (word)(xM[i] << t1);
        t = mult(t, t2);
        t = (word)(t >> 12);
        xMc[i] = (int16_t)(t + 4);
    }
    *mant_out = mant; *exp_out = exp; *xmaxc_out = xmaxc;
}

void apcmInverse(const int16_t* xMc, word mant, word exp, word* xMp) {
    const word t1 = kFAC[mant];
    const word t2 = sub(6, exp);
    const word t3 = asl(1, sub(t2, 1));
    for (int i = 0; i < 13; i++) {
        word t = (word)((xMc[i] << 1) - 7);
        t = (word)(t << 12);
        t = mult_r(t1, t);
        t = add(t, t3);
        xMp[i] = asr(t, t2);
    }
}

void gridPositioning(int Mc, const word* xMp, word* ep) {
    for (int k = 0; k < 40; k++) ep[k] = 0;
    for (int i = 0; i <= 12; i++) ep[Mc + 3 * i] = xMp[i];
}

// ---- bits ---------------------------------------------------------------------------------------------------------------------------------
struct BitW {
    uint8_t* p; int bit = 0;
    void put(unsigned v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if ((bit & 7) == 0) p[bit >> 3] = 0;
            if ((v >> i) & 1) p[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
            bit++;
        }
    }
};
struct BitR {
    const uint8_t* p; int bit = 0;
    int16_t get(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; i++, bit++) v = (v << 1) | ((p[bit >> 3] >> (7 - (bit & 7))) & 1);
        return (int16_t)v;
    }
};
const int kLarBits[8] = {6, 6, 5, 5, 4, 4, 3, 3};

} // namespace

void gsmPack(const GsmParams& p, uint8_t out[kGsmFrameBytes]) {
    BitW w{out};
    w.put(0xD, 4);
    for (int i = 0; i < 8; i++) w.put((unsigned)p.LARc[i], kLarBits[i]);
    for (int s = 0; s < 4; s++) {
        w.put((unsigned)p.Nc[s], 7); w.put((unsigned)p.bc[s], 2); w.put((unsigned)p.Mc[s], 2); w.put((unsigned)p.xmaxc[s], 6);
        for (int i = 0; i < 13; i++) w.put((unsigned)p.xMc[s][i], 3);
    }
}

bool gsmUnpack(const uint8_t in[kGsmFrameBytes], GsmParams& p) {
    BitR r{in};
    const bool ok = r.get(4) == 0xD;
    for (int i = 0; i < 8; i++) p.LARc[i] = r.get(kLarBits[i]);
    for (int s = 0; s < 4; s++) {
        p.Nc[s] = r.get(7); p.bc[s] = r.get(2); p.Mc[s] = r.get(2); p.xmaxc[s] = r.get(6);
        for (int i = 0; i < 13; i++) p.xMc[s][i] = r.get(3);
    }
    return ok;
}

// ---- encoder ------------------------------------------------------------------------------------------------------------------------------
void GsmEncoder::reset() { *this = GsmEncoder(); }

void GsmEncoder::encode(const int16_t pcm[kGsmFrameSamples], GsmParams& p) {
    word so[160];
    // 4.2.1 - 4.2.3 preprocessing: downscaling, offset compensation, pre-emphasis
    for (int k = 0; k < 160; k++) {
        const word SO = (word)((pcm[k] >> 3) << 2);
        const word s1 = (word)(SO - z1_);   // no overflow: |SO| < 2^14
        z1_ = SO;
        longword L_s2 = (longword)s1 << 15;
        const word msp = (word)(Lz2_ >> 15);
        const word lsp = (word)(Lz2_ - ((longword)msp << 15));
        L_s2 += mult_r(lsp, 32735);
        const longword L_temp = (longword)msp * 32735;
        Lz2_ = L_add(L_temp, L_s2);
        const longword L_t2 = L_add(Lz2_, 16384);
        const word msp2 = mult_r(mp_, -28180);
        mp_ = (word)(L_t2 >> 15);
        so[k] = add(mp_, msp2);
    }
    // LPC analysis
    longword L_ACF[9];
    word LAR[8];
    autocorrelation(so, L_ACF);
    reflectionCoefficients(L_ACF, LAR);
    transformToLAR(LAR);
    quantizeLAR(LAR, p.LARc);
    // short-term analysis filter
    word* LARpp_j = LARpp_[j_];
    word* LARpp_j1 = LARpp_[j_ ^= 1];
    word LARp[8];
    decodeLAR(p.LARc, LARpp_j);
    coeff0_12(LARpp_j1, LARpp_j, LARp); LARpToRp(LARp); stAnalysis(u_, LARp, 13, so);
    coeff13_26(LARpp_j1, LARpp_j, LARp); LARpToRp(LARp); stAnalysis(u_, LARp, 14, so + 13);
    coeff27_39(LARpp_j1, LARpp_j, LARp); LARpToRp(LARp); stAnalysis(u_, LARp, 13, so + 27);
    coeff40_159(LARpp_j, LARp); LARpToRp(LARp); stAnalysis(u_, LARp, 120, so + 40);
    // subframes
    word e[50] = {};
    word* dp = dp0_ + 120;
    for (int k = 0; k < 4; k++) {
        word dpp[40];
        ltpParameters(so + k * 40, dp, &p.bc[k], &p.Nc[k]);
        ltpAnalysisFilter(p.bc[k], p.Nc[k], dp, so + k * 40, dpp, e + 5);
        word x[40], xM[13], xMp[13], mant, exp;
        weightingFilter(e + 5, x);
        gridSelection(x, xM, &p.Mc[k]);
        apcmQuantize(xM, p.xMc[k], &mant, &exp, &p.xmaxc[k]);
        apcmInverse(p.xMc[k], mant, exp, xMp);
        gridPositioning(p.Mc[k], xMp, e + 5);
        for (int i = 0; i < 40; i++) dp[i] = add(e[5 + i], dpp[i]);
        dp += 40;
    }
    std::memmove(dp0_, dp0_ + 160, 120 * sizeof(word));
}

void GsmEncoder::encode(const int16_t pcm[kGsmFrameSamples], uint8_t out[kGsmFrameBytes]) {
    GsmParams p;
    encode(pcm, p);
    gsmPack(p, out);
}

// ---- decoder ------------------------------------------------------------------------------------------------------------------------------
void GsmDecoder::reset() { *this = GsmDecoder(); }

void GsmDecoder::decode(const GsmParams& p, int16_t pcm[kGsmFrameSamples]) {
    word wt[160];
    word* drp = dp0_ + 120;
    for (int j = 0; j < 4; j++) {
        word erp[40], xMp[13], exp, mant;
        xmaxcToExpMant(p.xmaxc[j], &exp, &mant);
        apcmInverse(p.xMc[j], mant, exp, xMp);
        gridPositioning(p.Mc[j] & 3, xMp, erp);
        // 4.3.2 long-term synthesis
        const word Nr = (p.Nc[j] < 40 || p.Nc[j] > 120) ? nrp_ : p.Nc[j];
        nrp_ = Nr;
        const word brp = kQLB[p.bc[j] & 3];
        for (int k = 0; k < 40; k++) drp[k] = add(erp[k], mult_r(brp, drp[k - Nr]));
        for (int k = 0; k < 120; k++) drp[-120 + k] = drp[-80 + k];
        for (int k = 0; k < 40; k++) wt[j * 40 + k] = drp[k];
    }
    // 4.3.3 short-term synthesis
    word* LARpp_j = LARpp_[j_];
    word* LARpp_j1 = LARpp_[j_ ^= 1];
    word LARp[8];
    decodeLAR(p.LARc, LARpp_j);
    word* s = pcm;
    coeff0_12(LARpp_j1, LARpp_j, LARp); LARpToRp(LARp); stSynthesis(v_, LARp, 13, wt, s);
    coeff13_26(LARpp_j1, LARpp_j, LARp); LARpToRp(LARp); stSynthesis(v_, LARp, 14, wt + 13, s + 13);
    coeff27_39(LARpp_j1, LARpp_j, LARp); LARpToRp(LARp); stSynthesis(v_, LARp, 13, wt + 27, s + 27);
    coeff40_159(LARpp_j, LARp); LARpToRp(LARp); stSynthesis(v_, LARp, 120, wt + 40, s + 40);
    // 4.3.5 - 4.3.7 de-emphasis, upscaling, truncation
    for (int k = 0; k < 160; k++) {
        msr_ = add(s[k], mult_r(msr_, 28180));
        s[k] = (word)(add(msr_, msr_) & 0xFFF8);
    }
}

bool GsmDecoder::decode(const uint8_t in[kGsmFrameBytes], int16_t pcm[kGsmFrameSamples]) {
    GsmParams p;
    if (!gsmUnpack(in, p)) { std::memset(pcm, 0, kGsmFrameSamples * sizeof(int16_t)); return false; }
    decode(p, pcm);
    return true;
}

} // namespace echolink
} // namespace dect2
