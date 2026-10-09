// HD Radio Layer 1 receivers and modulators (see hdr_phy.h). The acquisition, synchronisation and deinterleaving are adapted from nrsc5
// (GPL-3.0, github.com/theori-io/nrsc5): src/acquire.c (cyclic prefix correlation, filter taps, AM carrier tracking), src/sync.c (Costas
// loops on the reference subcarriers, integer offset search, block sync, channel estimate, MER, AM training symbols) and src/decode.c
// (interleavers). The demapping is soft decision for every constellation (nrsc5 uses hard decisions in AM), the modulators are OnAir's own.
#include "dect2/hdr_phy.h"
#include "dect2/fftutil.h"
#include "dect2/hdr_fec.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace dect2 { namespace hdr {

namespace {
using cd = std::complex<double>;
constexpr float kPi = 3.14159265358979f;
constexpr int kLbStart = kFftFm / 2 - 546;      // 478: subcarrier -546
constexpr int kUbEnd = kFftFm / 2 + 546;        // 1570: subcarrier +546
constexpr int kPw = 19;                         // FM partition width (18 data subcarriers and a reference)
constexpr int kMiddleRef = 30;
constexpr int kFilterDelay = 15;

// nrsc5 acquire.c: band-pass filters for the cyclic prefix correlation (they keep the analog host out of it)
const float kTapsFm[32] = {-0.000685643230099231f, 0.005636964458972216f, 0.009015781804919243f, -0.015486305579543114f, -0.035108357667922974f,
    0.017446253448724747f, 0.08155813068151474f, 0.007995186373591423f, -0.13311293721199036f, -0.0727422907948494f, 0.15914097428321838f,
    0.16498781740665436f, -0.1324498951435089f, -0.2484012246131897f, 0.051773931831121445f, 0.2821577787399292f, 0.051773931831121445f,
    -0.2484012246131897f, -0.1324498951435089f, 0.16498781740665436f, 0.15914097428321838f, -0.0727422907948494f, -0.13311293721199036f,
    0.007995186373591423f, 0.08155813068151474f, 0.017446253448724747f, -0.035108357667922974f, -0.015486305579543114f, 0.009015781804919243f,
    0.005636964458972216f, -0.000685643230099231f, 0.f};
const float kTapsAm[32] = {-0.00038464731187559664f, -0.00021618751634377986f, 0.0026779419276863337f, -0.00029802651260979474f,
    -0.0012626448879018426f, -0.0013182522961869836f, -0.012252614833414555f, 0.015980124473571777f, 0.037112727761268616f,
    -0.05451361835002899f, -0.05804193392395973f, 0.11320608854293823f, 0.055298302322626114f, -0.16878043115139008f,
    -0.022917453199625015f, 0.19178225100040436f, -0.022917453199625015f, -0.16878043115139008f, 0.055298302322626114f,
    0.11320608854293823f, -0.05804193392395973f, -0.05451361835002899f, 0.037112727761268616f, 0.015980124473571777f,
    -0.012252614833414555f, -0.0013182522961869836f, -0.0012626448879018426f, -0.00029802651260979474f, 0.0026779419276863337f,
    -0.00021618751634377986f, -0.00038464731187559664f, 0.f};

// nrsc5 sync.c: the compatible base mode of every PSMI (1011s table 6-4)
const int kCompat[64] = {0, 1, 2, 3, 1, 5, 6, 5, 6, 1, 2, 11, 1, 5, 6, 5, 6, 1, 2, 3, 1, 5, 6, 5, 6, 1, 2, 11, 1, 5, 6, 5,
                         6, 1, 2, 3, 1, 5, 6, 5, 6, 1, 2, 11, 1, 5, 6, 5, 6, 1, 2, 3, 1, 5, 6, 5, 6, 1, 2, 11, 1, 5, 6, 5};

std::vector<float> windowShape(int fft, int cp) {
    std::vector<float> s((size_t)(fft + cp));
    for (int i = 0; i < fft + cp; i++) {
        if (i < cp) s[(size_t)i] = std::sin(kPi / 2 * (float)i / (float)cp);
        else if (i < fft) s[(size_t)i] = 1.f;
        else s[(size_t)i] = std::cos(kPi / 2 * (float)(i - fft) / (float)cp);
    }
    return s;
}

inline float norm2(cf32 v) { return v.real() * v.real() + v.imag() * v.imag(); }
inline float clamp1(float x) { return std::max(-1.f, std::min(1.f, x)); }

float phaseDiff(float a, float b) {
    float d = a - b;
    while (d > kPi / 2) d -= kPi;
    while (d < -kPi / 2) d += kPi;
    return d;
}

int fuzzyMatch(const signed char* needle, int nlen, const uint8_t* data, int size) {
    for (int n = 0; n < size; n++) {
        int i;
        for (i = 0; i < nlen; i++) {
            if (needle[i] < 0) continue;
            if ((uint8_t)needle[i] != data[(n + i) % size]) break;
        }
        if (i == nlen) return n;
    }
    return -1;
}

// soft bits of one rail of the AM constellations (positive: 1)
inline void soft64(float v, float* o) { const float a = std::fabs(v); o[0] = v; o[1] = 2.f - a; o[2] = 1.f - std::fabs(a - 2.f); }
inline void soft16(float v, float* o) { o[0] = v; o[1] = 1.f - std::fabs(v); }

// The cyclic prefix correlation shared by the FM and AM acquisition (nrsc5 acquire_process())
struct CpSearch {
    std::vector<cf32> y;
    std::vector<cf32> sums;
    // returns the sample offset of the symbol start and the correlation value there
    int run(const cf32* in, int fft, int cp, int nsym, const float* taps, const std::vector<float>& shape, cf32& best) {
        const int fftcp = fft + cp;
        const int total = fftcp * (nsym + 1);
        y.assign((size_t)total, cf32(0, 0));
        for (int i = 0; i < total; i++) {
            cf32 acc(0, 0);
            const int k0 = std::max(0, i - 31);
            for (int k = k0; k <= i; k++) acc += taps[i - k] * in[k];
            y[(size_t)i] = acc;
        }
        sums.assign((size_t)fftcp, cf32(0, 0));
        for (int i = 0; i < fftcp; i++) {
            cf32 s(0, 0);
            for (int j = 0; j < nsym; j++) s += y[(size_t)(i + j * fftcp)] * std::conj(y[(size_t)(i + j * fftcp + fft)]);
            sums[(size_t)i] = s;
        }
        float maxMag = -1;
        int at = 0;
        best = cf32(0, 0);
        for (int i = 0; i < fftcp; i++) {
            cf32 v(0, 0);
            for (int j = 0; j < cp; j++) v += sums[(size_t)((i + j) % fftcp)] * (shape[(size_t)j] * shape[(size_t)(j + fft)]);
            const float m = norm2(v);
            if (m > maxMag) { maxMag = m; best = v; at = (i + fftcp - kFilterDelay) % fftcp; }
        }
        return at;
    }
};
} // namespace

// ================================================================ FM receiver

struct FmRx::Impl {
    L1Sink* sink;
    // acquisition
    std::vector<cf32> in;            // 33 symbols
    int idx = 0;
    std::vector<float> shape;
    CpSearch cps;
    float prevAngle = 0;
    cd phase{1, 0};
    int keepExtra = 0;
    int cfo = 0;                     // integer offset, subcarriers
    int state = 0;
    Fft fft{kFftFm};
    std::vector<cf32> fftBuf;
    // sync
    std::vector<cf32> sb;            // [subcarrier][symbol of the block]
    std::vector<float> ph;
    std::vector<float> cFreq, cPhase;
    int sidx = 0;
    int psmi = 1, bc = 0;
    int cfoWait = 0;
    float alpha = 0, beta = 0;
    int samperr = 0;
    float angle = 0;
    float errL = 0, errU = 0;
    int merCnt = 0;
    float merL = 0, merU = 0;
    // decoding
    std::vector<float> pm;           // 16 blocks of soft bits
    bool started = false;
    Viterbi vit;
    std::vector<float> soft;
    std::vector<uint8_t> bits;
    double ber = 0;
    uint64_t syncs = 0, p1Frames = 0, p1Bad = 0;
    int badRun = 0;
    int lastBc = -1;
    std::vector<cf32> constel;
    struct Px { PxDeinterleaver dei; std::vector<float> buf, blk; bool started = false; } px[2];   // P3 and P4 on the extended partitions

    explicit Impl(L1Sink* s) : sink(s) {
        in.assign((size_t)kSymFm * (kBlk + 1), cf32(0, 0));
        shape = windowShape(kFftFm, kCpFm);
        fftBuf.assign(kFftFm, cf32(0, 0));
        sb.assign((size_t)kFftFm * kBlk, cf32(0, 0));
        ph.assign((size_t)kFftFm * kBlk, 0.f);
        cFreq.assign(kFftFm, 0.f);
        cPhase.assign(kFftFm, 0.f);
        pm.assign((size_t)16 * kPmBlock, 0.f);
        const float bw = 0.05f, damp = 0.70710678f;
        const float den = 1 + 2 * damp * bw + bw * bw;
        alpha = 4 * damp * bw / den;
        beta = 4 * bw * bw / den;
        reset();
    }

    void reset() {
        idx = 0; prevAngle = 0; phase = cd(1, 0); keepExtra = 0; cfo = 0; state = 0;
        std::fill(cFreq.begin(), cFreq.end(), 0.f);
        std::fill(cPhase.begin(), cPhase.end(), 0.f);
        sidx = 0; psmi = 1; bc = 0; cfoWait = 0; samperr = 0; angle = 0;
        errL = errU = 0; merCnt = 0; merL = merU = 0;
        started = false; ber = 0; badRun = 0; lastBc = -1;
        constel.clear();
        for (auto& x : px) { x.dei.reset(kP3LenMp3); x.started = false; }
    }

    cf32& S(int k, int n) { return sb[(size_t)k * kBlk + (size_t)n]; }
    float& P(int k, int n) { return ph[(size_t)k * kBlk + (size_t)n]; }

    void feed(const cf32* x, size_t n) {
        const int size = kSymFm * (kBlk + 1);
        for (size_t i = 0; i < n; i++) {
            in[(size_t)idx++] = std::conj(x[i]);
            if (idx == size) acquire();
        }
    }

    // ---- nrsc5 acquire_process()
    void acquire() {
        const int fftcp = kSymFm;
        float ang;
        int se;
        if (state == 2) {
            se = fftcp / 2 + samperr;
            samperr = 0;
            const float diff = -angle;
            angle = 0;
            ang = prevAngle + diff;
            prevAngle = ang;
        } else {
            cf32 best;
            se = cps.run(in.data(), kFftFm, kCpFm, kBlk, kTapsFm, shape, best);
            const float diff = std::arg(best * std::polar(1.f, -prevAngle));
            const float factor = prevAngle != 0 ? 0.25f : 1.f;
            ang = prevAngle + diff * factor;
            prevAngle = ang;
            if (state == 0) state = 1;
        }
        syncAdjust(fftcp / 2 - se);
        ang -= 2 * kPi * (float)cfo;
        phase *= std::polar(1.0, -(double)(fftcp / 2 - se) * ang / kFftFm);
        const cd inc = std::polar(1.0, (double)ang / kFftFm);
        for (int i = 0; i < kBlk; i++) {
            for (int j = 0; j < fftcp; j++) {
                const cf32 s = cf32(phase) * in[(size_t)(i * fftcp + j + se)];
                if (j < kCpFm) fftBuf[(size_t)j] = shape[(size_t)j] * s;
                else if (j < kFftFm) fftBuf[(size_t)j] = s;
                else fftBuf[(size_t)(j - kFftFm)] += shape[(size_t)j] * s;
                phase *= inc;
            }
            phase /= std::abs(phase);
            fft.forward(fftBuf.data());
            for (int k = 0; k < kFftFm; k++) S(k, sidx) = fftBuf[(size_t)((k + kFftFm / 2) % kFftFm)];
            if (++sidx == kBlk) { sidx = 0; syncProcess(); }
        }
        const int keep = fftcp + (fftcp / 2 - se) + keepExtra;
        keepExtra = 0;
        memmove(in.data(), in.data() + (idx - keep), sizeof(cf32) * (size_t)keep);
        idx = keep;
    }

    void syncAdjust(int adj) {
        for (int i = 0; i < 14 * kPw + 1; i++) {
            cPhase[(size_t)(kLbStart + i)] -= (float)adj * (float)(kLbStart + i - kFftFm / 2) * 2 * kPi / kFftFm;
            cPhase[(size_t)(kUbEnd - i)] -= (float)adj * (float)(kUbEnd - i - kFftFm / 2) * 2 * kPi / kFftFm;
        }
    }

    // ---- nrsc5 sync.c
    void adjustRef(int ref, int c) {
        static const signed char sync[32] = {-1, 1, -1, -1, -1, 1, 1, 0, 1, -1, 0, 0, 0, -1, -1, 0, 0, 0, 0, 0, -1, 1, -1, 0, 0, 0, 0, 0, 0, 0, 0, -1};
        const float cfoFreq = 2 * kPi * (float)c * kCpFm / kFftFm;
        float& cp = cPhase[(size_t)ref];
        float& cf = cFreq[(size_t)ref];
        for (int n = 0; n < kBlk; n++) {
            cf32& v = S(ref, n);
            const float err = std::arg(v * v * std::polar(1.f, -2 * cp)) * 0.5f;
            P(ref, n) = cp;
            v *= std::polar(1.f, -cp);
            cf += beta * err;
            cf = std::max(-0.5f, std::min(0.5f, cf));
            cp += cf + cfoFreq + alpha * err;
            if (cp > kPi) cp -= 2 * kPi;
            if (cp < -kPi) cp += 2 * kPi;
        }
        float x = 0;
        for (int n = 0; n < kBlk; n++) x += S(ref, n).real() * (float)sync[n];
        if (x < 0) {
            for (int n = 0; n < kBlk; n++) { P(ref, n) += kPi; S(ref, n) *= -1.f; }
            cp += kPi;
        }
    }
    void resetRef(int ref) { for (int n = 0; n < kBlk; n++) S(ref, n) *= std::polar(1.f, P(ref, n)); }

    static void needleFor(int rsid, signed char* nd) {
        const signed char base[32] = {0, 1, 0, 0, 0, 1, 1, -1, 1, 0, 0, 0, -1, 0, 0, -1, -1, -1, -1, -1, 0, 1, 0, -1, -1, -1, -1, -1, -1, -1, -1, 0};
        memcpy(nd, base, 32);
        nd[10] = (signed char)(rsid >> 1);
        nd[11] = (signed char)((rsid >> 1) ^ (rsid & 1));
    }
    bool decodeRef(int ref, int rsid, int& obc, int& opsmi) {
        signed char nd[32];
        needleFor(rsid, nd);
        for (int n = 0; n < kBlk; n++) if (nd[n] >= 0 && nd[n] != (S(ref, n).real() > 0 ? 1 : 0)) return false;
        uint8_t d[32];
        uint8_t prev = 0;
        for (int n = 0; n < kBlk; n++) { const uint8_t b = S(ref, n).real() <= 0 ? 0 : 1; d[n] = b ^ prev; prev = b; }
        obc = (d[16] << 3) | (d[17] << 2) | (d[18] << 1) | d[19];
        opsmi = (d[25] << 5) | (d[26] << 4) | (d[27] << 3) | (d[28] << 2) | (d[29] << 1) | d[30];
        return true;
    }
    int findRef(int ref, int rsid) {
        signed char nd[32];
        needleFor(rsid, nd);
        uint8_t d[32];
        for (int n = 0; n < kBlk; n++) d[n] = S(ref, n).real() <= 0 ? 0 : 1;
        int m = fuzzyMatch(nd, 32, d, kBlk);
        if (m >= 0) return m;
        for (int n = 0; n < kBlk; n++) d[n] ^= 1;
        return fuzzyMatch(nd, 32, d, kBlk);
    }
    float smag(int ref) {
        float s = 0;
        for (int n = 0; n < kBlk; n++) s += std::fabs(S(ref, n).real());
        return s / kBlk;
    }
    void adjustData(int lower, int upper) {
        const float s0 = smag(lower), s19 = smag(upper);
        for (int n = 0; n < kBlk; n++) {
            const cf32 up = std::polar(1.f, P(upper, n)), lo = std::polar(1.f, P(lower, n));
            for (int k = 1; k < kPw; k++) {
                const cf32 C = cf32((float)kPw, (float)kPw) / ((float)k * s19 * up + (float)(kPw - k) * s0 * lo);
                S(lower + k, n) *= C;
            }
        }
    }
    void detectCfo() {
        for (int c = -2 * kPw; c < 2 * kPw; c++) {
            int cnt[32] = {};
            for (int i = 0; i <= 10; i++) {
                int r = c + kLbStart + i * kPw;
                adjustRef(r, c);
                int off = findRef(r, (kMiddleRef - i) & 3);
                resetRef(r);
                if (off >= 0) cnt[off]++;
                r = c + kUbEnd - i * kPw;
                adjustRef(r, c);
                off = findRef(r, (kMiddleRef - i) & 3);
                resetRef(r);
                if (off >= 0) cnt[off]++;
            }
            int best = -1, bestCnt = 0;
            for (int o = 0; o < 32; o++) if (cnt[o] > bestCnt) { best = o; bestCnt = cnt[o]; }
            if (best >= 0 && bestCnt >= 3) {
                keepExtra = ((kBlk - best) % kBlk) * kSymFm;
                cfo += c;
                cfoWait = 8;
                break;
            }
        }
    }

    void syncProcess() {
        int ppb;
        switch (kCompat[psmi & 63]) { case 2: ppb = 11; break; case 3: ppb = 12; break; case 5: case 6: case 11: ppb = 14; break; default: ppb = 10; }
        for (int i = 0; i < ppb * kPw + 1; i += kPw) { adjustRef(kLbStart + i, 0); adjustRef(kUbEnd - i, 0); }
        if (state == 1) {
            int good = 0, seenBc[16] = {}, seenPsmi[64] = {};
            for (int i = 0; i <= ppb; i++) {
                int b, p;
                if (decodeRef(kLbStart + i * kPw, (kMiddleRef - i) & 3, b, p)) { good++; seenBc[b]++; seenPsmi[p]++; }
                if (decodeRef(kUbEnd - i * kPw, (kMiddleRef - i) & 3, b, p)) { good++; seenBc[b]++; seenPsmi[p]++; }
            }
            if (good >= 4) {
                int mb = -1, mp = -1;
                for (int b = 0; b < 16; b++) if (seenBc[b] > good / 2) mb = b;
                for (int p = 0; p < 64; p++) if (seenPsmi[p] > good / 2) mp = p;
                if (mb >= 0 && mp >= 0) {
                    bc = mb; psmi = mp;
                    state = 2;
                    syncs++;
                    started = false; badRun = 0;
                    for (auto& x : px) { x.dei.reset(kCompat[psmi & 63] == 2 ? kP3LenMp2 : kP3LenMp3); x.started = false; }
                    sink->blockSync();
                }
            } else if (cfoWait == 0) detectCfo();
            else cfoWait--;
        }
        if (state != 2) return;
        // channel estimate between the references, sample clock and frequency tracking
        float se = 0, ang = 0, sxy = 0, sx2 = 0;
        for (int i = 0; i < ppb * kPw; i += kPw) {
            adjustData(kLbStart + i, kLbStart + i + kPw);
            adjustData(kUbEnd - i - kPw, kUbEnd - i);
            se += phaseDiff(P(kLbStart + i, 0), P(kLbStart + i + kPw, 0));
            se += phaseDiff(P(kUbEnd - i - kPw, 0), P(kUbEnd - i, 0));
        }
        se = se / (float)(ppb * 2) * kFftFm / kPw / (2 * kPi);
        for (int i = 0; i < ppb * kPw + 1; i += kPw) {
            float x = (float)(kLbStart + i - kFftFm / 2), y = cFreq[(size_t)(kLbStart + i)];
            ang += y; sxy += x * y; sx2 += x * x;
            x = (float)(kUbEnd - i - kFftFm / 2); y = cFreq[(size_t)(kUbEnd - i)];
            ang += y; sxy += x * y; sx2 += x * x;
        }
        se -= (sxy / sx2) * kFftFm / (2 * kPi) * kBlk;
        samperr = (int)std::lround(se);
        ang /= (float)((ppb + 1) * 2);
        angle = ang;
        for (int i = 0; i < ppb * kPw + 1; i += kPw) { cFreq[(size_t)(kLbStart + i)] -= ang; cFreq[(size_t)(kUbEnd - i)] -= ang; }
        // modulation error of the primary main data subcarriers
        float eL = 0, eU = 0;
        for (int n = 0; n < kBlk; n++)
            for (int i = 0; i < kPmPart * kPw; i += kPw)
                for (int j = 1; j < kPw; j++) {
                    cf32 c = S(kLbStart + i + j, n);
                    eL += norm2(cf32(c.real() >= 0 ? 1.f : -1.f, c.imag() >= 0 ? 1.f : -1.f) - c);
                    c = S(kUbEnd - i - kPw + j, n);
                    eU += norm2(cf32(c.real() >= 0 ? 1.f : -1.f, c.imag() >= 0 ? 1.f : -1.f) - c);
                }
        errL += eL; errU += eU;
        const float sigBlk = 2.f * kBlk * kPmPart * 18;
        if (++merCnt == 4) {
            merL = 10 * std::log10(sigBlk * (float)merCnt / std::max(1e-9f, errL));
            merU = 10 * std::log10(sigBlk * (float)merCnt / std::max(1e-9f, errU));
            merCnt = 0; errL = errU = 0;
        }
        const float multL = std::max(1.f, std::min(127.f, sigBlk / std::max(1e-9f, eL) * 10));
        const float multU = std::max(1.f, std::min(127.f, sigBlk / std::max(1e-9f, eU) * 10));
        float* blk = &pm[(size_t)bc * kPmBlock];
        int o = 0;
        for (int n = 0; n < kBlk; n++) {
            for (int i = kLbStart; i < kLbStart + kPmPart * kPw; i += kPw)
                for (int j = 1; j < kPw; j++) { const cf32 c = S(i + j, n); blk[o++] = clamp1(c.real()) * multL; blk[o++] = clamp1(c.imag()) * multL; }
            for (int i = kUbEnd - kPmPart * kPw; i < kUbEnd; i += kPw)
                for (int j = 1; j < kPw; j++) { const cf32 c = S(i + j, n); blk[o++] = clamp1(c.real()) * multU; blk[o++] = clamp1(c.imag()) * multU; }
        }
        constel.clear();
        for (int n = 0; n < kBlk; n += 8)
            for (int i = 0; i < kPmPart * kPw; i += kPw)
                for (int j = 1; j < kPw; j += 3) { constel.push_back(S(kLbStart + i + j, n)); constel.push_back(S(kUbEnd - kPmPart * kPw + i + j, n)); }
        // the extended partitions (MP2: one per side, MP3: two, MP11: four in two channels), read as nrsc5 sync.c does
        const int cm = kCompat[psmi & 63];
        if (cm == 2 || cm == 3 || cm == 11) {
            const int np = cm == 2 ? 1 : 2;
            for (int ch = 0; ch < (cm == 11 ? 2 : 1); ch++) {
                std::vector<float>& xb = px[ch].blk;
                xb.resize((size_t)kBlk * np * 2 * 36);
                int q = 0;
                const int first = kPmPart + ch * 2;
                for (int n = 0; n < kBlk; n++) {
                    for (int p = first; p < first + np; p++)
                        for (int j = 1; j < kPw; j++) { const cf32 c = S(kLbStart + p * kPw + j, n); xb[(size_t)q++] = clamp1(c.real()) * multL; xb[(size_t)q++] = clamp1(c.imag()) * multL; }
                    for (int p = first + np - 1; p >= first; p--)
                        for (int j = 1; j < kPw; j++) { const cf32 c = S(kUbEnd - (p + 1) * kPw + j, n); xb[(size_t)q++] = clamp1(c.real()) * multU; xb[(size_t)q++] = clamp1(c.imag()) * multU; }
                }
                decodePx(ch, cm == 2 ? kP3LenMp2 : kP3LenMp3);
            }
        }
        lastBc = bc;
        decodePids(blk);
        if (bc == 0) started = true;
        if (started && bc == 15) decodeP1();
        if (state == 2) bc = (bc + 1) % 16;
    }

    // nrsc5 decode_push_px1() / decode_push_px2(): pairs of blocks through the convolutional interleaver, a rate 1/2 code word each
    void decodePx(int ch, int frameLen) {
        Px& X = px[ch];
        if (X.dei.frameLen() != frameLen) { X.dei.reset(frameLen); X.started = false; }
        if (bc % 2 == 0) X.started = true;
        if (!X.started) return;
        X.buf.resize((size_t)frameLen * 2);
        std::copy(X.blk.begin(), X.blk.end(), X.buf.begin() + (long)frameLen * (bc % 2));
        if (bc % 2 == 0) return;
        if (!X.dei.push(X.buf.data(), soft)) return;
        bits.resize((size_t)frameLen);
        vit.decode(kCodeFm, soft.data(), (size_t)frameLen, bits.data());
        scramble(bits.data(), bits.size());
        sink->transfer(bits.data(), frameLen, 1 + ch);
    }
    static constexpr int kPmPart = 10;

    void decodePids(const float* blk) {
        const std::vector<int>& pos = fmPidsPos();
        soft.assign((size_t)kPidsLen * 3, 0.f);
        size_t out = 0;
        for (int i = 0; i < kPidsEncFm; i++) {
            soft[out++] = blk[pos[(size_t)i]];
            if (out % 6 == 5) soft[out++] = 0.f;
        }
        bits.resize(kPidsLen);
        vit.decode(kCodeFm, soft.data(), kPidsLen, bits.data());
        scramble(bits.data(), kPidsLen);
        sink->pids(bits.data());
    }

    void decodeP1() {
        const std::vector<int>& pos = fmP1Pos();
        soft.assign((size_t)kP1LenFm * 3, 0.f);
        size_t out = 0;
        for (int i = 0; i < kP1EncFm; i++) {
            soft[out++] = pm[(size_t)pos[(size_t)i]];
            if (out % 6 == 5) soft[out++] = 0.f;
        }
        bits.resize(kP1LenFm);
        vit.decode(kCodeFm, soft.data(), kP1LenFm, bits.data());
        int cnt = 0;
        const int e = reencodeErrors(kCodeFm, soft.data(), bits.data(), kP1LenFm, &cnt);
        ber = cnt ? (double)e / cnt : 0;
        scramble(bits.data(), kP1LenFm);
        p1Frames++;
        if (!sink->transfer(bits.data(), kP1LenFm, 0)) {
            p1Bad++;
            if (++badRun >= 2) { state = 0; badRun = 0; }   // lost: search again
        } else badRun = 0;
    }
};

FmRx::FmRx(L1Sink* sink) : p_(std::make_unique<Impl>(sink)) {}
FmRx::~FmRx() = default;
void FmRx::reset() { p_->reset(); }
void FmRx::feed(const cf32* x, size_t n) { p_->feed(x, n); }
int FmRx::state() const { return p_->state; }
L1Stats FmRx::stats() const {
    L1Stats s;
    const Impl& m = *p_;
    s.state = m.state;
    // the band handed in is the conjugate: its offset is the opposite of the one the receiver corrects
    s.cfoHz = (double)(m.prevAngle - 2 * kPi * (float)m.cfo) * kRateFm / (2 * kPi * kFftFm);
    s.merLower = m.merL; s.merUpper = m.merU;
    s.ber = m.ber;
    s.mode = m.state == 2 ? m.psmi : -1;
    s.bc = m.lastBc;
    s.syncs = m.syncs;
    s.p1Frames = m.p1Frames; s.p1Bad = m.p1Bad;
    s.constel = m.constel;
    return s;
}

// ================================================================ AM receiver

namespace {
constexpr int kCenterAm = kFftAm / 2;
constexpr int kRefAm = 1, kPidsInner = 27, kPidsOuter = 53, kInnerStart = 2, kMiddleStart = 28, kOuterStart = 57, kMaxAm = 81;

// decimation by 16, 744187.5 -> 46511.7 Hz: a windowed sinc (Kaiser), passband 16 kHz, stopband from 30 kHz
std::vector<float> decimTaps() {
    const int n = 256;
    std::vector<float> h((size_t)n);
    const double fc = 23000.0 / kRateFm, beta = 7.0;
    auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
    double sum = 0;
    for (int i = 0; i < n; i++) {
        const double m = i - (n - 1) / 2.0;
        const double sc = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
        const double r = 2.0 * i / (n - 1) - 1.0;
        const double w = i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
        h[(size_t)i] = (float)(sc * w);
        sum += sc * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}
}

struct AmRx::Impl {
    L1Sink* sink;
    std::vector<float> taps = decimTaps();
    std::vector<cf32> in;
    int idx = 0;
    std::vector<float> shape;
    CpSearch cps;
    float prevAngle = 0;
    cd phase{1, 0};
    int keepExtra = 0;
    int cfo = 0;
    int state = 0;
    Fft fft{kFftAm};
    std::vector<cf32> fftBuf;
    std::vector<cf32> sb;            // [bin][symbol]
    int sidx = 0;
    int psmi = 1, bc = 0, rdbi = 0;
    int cfoWait = 0;
    unsigned history = 0;
    int samperr = 0;
    float slope = 0;                 // the carrier's frequency from the last block, radians per sample
    std::vector<cf32> constel;
    // decoding: soft bits of the frame (8 blocks x 32 rows x 25 columns)
    std::vector<float> pl, pu, ss, tt;   // 6, 6, 4 and 2 per element
    std::vector<std::vector<float>> mlHist, muHist;   // the M bits of the last frames (diversity delay)
    int frames = 0;
    Viterbi vit;
    std::vector<float> soft, cw;
    std::vector<uint8_t> bits;
    double ber = 0;
    float mer = 0, merAcc = 0;
    int merN = 0;
    uint64_t syncs = 0, p1Frames = 0, p1Bad = 0;
    int badRun = 0, lastBc = -1;
    // decimator ring
    std::vector<cf32> ring;
    size_t rpos = 0;
    int dphase = 0;

    explicit Impl(L1Sink* s) : sink(s) {
        in.assign((size_t)kSymAm * (kBlk + 1), cf32(0, 0));
        shape = windowShape(kFftAm, kCpAm);
        fftBuf.assign(kFftAm, cf32(0, 0));
        sb.assign((size_t)kFftAm * kBlk, cf32(0, 0));
        pl.assign((size_t)8 * 32 * kAmCols * 6, 0.f);
        pu.assign(pl.size(), 0.f);
        ss.assign((size_t)8 * 32 * kAmCols * 4, 0.f);
        tt.assign((size_t)8 * 32 * kAmCols * 2, 0.f);
        ring.assign(taps.size() * 2, cf32(0, 0));
        reset();
    }
    void reset() {
        idx = 0; prevAngle = 0; phase = cd(1, 0); keepExtra = 0; cfo = 0; state = 0; slope = 0;
        sidx = 0; psmi = 1; bc = 0; rdbi = 0; cfoWait = 0; history = 0; samperr = 0;
        mlHist.clear(); muHist.clear(); frames = 0; ber = 0; mer = 0; merAcc = 0; merN = 0; badRun = 0; lastBc = -1;
        std::fill(ring.begin(), ring.end(), cf32(0, 0));
        rpos = 0; dphase = 0;
    }
    cf32& S(int k, int n) { return sb[(size_t)k * kBlk + (size_t)n]; }

    void feed(const cf32* x, size_t n) {
        const size_t L = taps.size();
        const int size = kSymAm * (kBlk + 1);
        for (size_t i = 0; i < n; i++) {
            // a doubled ring: the last L samples are always contiguous at ring[rpos .. rpos + L)
            ring[rpos] = x[i];
            ring[rpos + L] = x[i];
            rpos = (rpos + 1) % L;
            if (++dphase < 16) continue;
            dphase = 0;
            const cf32* hp = &ring[rpos];
            float re = 0, im = 0;
            for (size_t k = 0; k < L; k++) { re += taps[k] * hp[k].real(); im += taps[k] * hp[k].imag(); }
            in[(size_t)idx++] = cf32(re, im);
            if (idx == size) acquire();
        }
    }

    void acquire() {
        const int fftcp = kSymAm, off = (kFftAm - kCpAm) / 2;
        float ang;
        int se;
        if (state == 2) {
            se = fftcp / 2 + samperr;
            samperr = 0;
            ang = prevAngle;
        } else {
            cf32 best;
            se = cps.run(in.data(), kFftAm, kCpAm, kBlk, kTapsAm, shape, best);
            const float diff = std::arg(best * std::polar(1.f, -prevAngle));
            const float factor = prevAngle != 0 ? 0.25f : 1.f;
            ang = prevAngle + diff * factor;
            prevAngle = ang;
            if (state == 0) state = 1;
        }
        ang -= 2 * kPi * (float)cfo;
        phase *= std::polar(1.0, -(double)(fftcp / 2 - se) * ang / kFftAm);
        cd inc = std::polar(1.0, (double)ang / kFftAm);
        // the analog carrier gives the phase and the fine frequency (nrsc5 acquire.c, AM)
        {
            float y = 0, sy = 0, sxy = 0, sx2 = 0;
            cf32 lastC(1, 0);
            cd tp = phase;
            std::vector<float> mag(kFftAm, 0.f);
            for (int i = 0; i < kBlk; i++) {
                symbolFft(i, se, tp, inc, off);
                const float x = (float)fftcp * ((float)i - (float)(kBlk - 1) / 2);
                const cf32 c = fftBuf[(size_t)((kCenterAm + kFftAm / 2) % kFftAm)];
                if (i == 0) y = std::arg(c);
                else y += std::arg(c / lastC);
                lastC = c;
                sy += y; sxy += x * y; sx2 += x * x;
                if (state != 2)
                    for (int j = kCenterAm - kPidsOuter; j <= kCenterAm + kPidsOuter; j++) mag[(size_t)j] += std::abs(fftBuf[(size_t)((j + kFftAm / 2) % kFftAm)]);
            }
            if (state != 2) {
                int mi = kCenterAm;
                for (int j = kCenterAm - kPidsOuter; j <= kCenterAm + kPidsOuter; j++) if (mag[(size_t)j] > mag[(size_t)mi]) mi = j;
                cfo += mi - kCenterAm;
            }
            slope = sxy / sx2;
            inc *= std::polar(1.0, -(double)slope);
            // nrsc5 subtracts another 0.06 rad here; OnAir puts the carrier at exactly 0: then the analog audio cancels completely when the
            // complementary lower subcarriers are added to the upper ones, and a digital part turned against the carrier only costs cos()
            // of that angle there (the training symbols take care of the phase of every column)
            phase *= std::polar(1.0, (double)(-sy / kBlk + slope * kBlk * fftcp / 2));
        }
        for (int i = 0; i < kBlk; i++) {
            symbolFft(i, se, phase, inc, off);
            for (int k = 0; k < kFftAm; k++) S(k, sidx) = fftBuf[(size_t)((k + kFftAm / 2) % kFftAm)];
            if (++sidx == kBlk) { sidx = 0; syncProcess(); }
        }
        const int keep = fftcp + (fftcp / 2 - se) + keepExtra;
        keepExtra = 0;
        memmove(in.data(), in.data() + (idx - keep), sizeof(cf32) * (size_t)keep);
        idx = keep;
    }

    void symbolFft(int i, int se, cd& ph, const cd& inc, int off) {
        for (int j = 0; j < kSymAm; j++) {
            const cf32 s = cf32(ph) * in[(size_t)(i * kSymAm + j + se)];
            const size_t d = (size_t)((j + off) % kFftAm);
            if (j < kCpAm) fftBuf[d] = shape[(size_t)j] * s;
            else if (j < kFftAm) fftBuf[d] = s;
            else fftBuf[d] += shape[(size_t)j] * s;
            ph *= inc;
        }
        ph /= std::abs(ph);
        fft.forward(fftBuf.data());
    }

    int findBlock(int ref) {
        static const signed char nd[32] = {0, 1, 1, 0, 0, 1, 0, -1, -1, 1, -1, -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        uint8_t d[32];
        for (int n = 0; n < kBlk; n++) {
            d[n] = S(ref, n).imag() <= 0 ? 0 : 1;
            if (nd[n] >= 0 && d[n] != (uint8_t)nd[n]) return -1;
        }
        if (d[7] ^ d[8]) return -1;
        if (d[10] ^ d[11] ^ d[12] ^ d[13]) return -1;
        if (d[15] ^ d[16] ^ d[17] ^ d[18] ^ d[19] ^ d[20]) return -1;
        if (d[23] ^ d[24] ^ d[25] ^ d[26] ^ d[27] ^ d[28] ^ d[29] ^ d[30] ^ d[31]) return -1;
        const int b = (d[17] << 2) | (d[18] << 1) | d[19];
        if (b == 0) { psmi = (d[26] << 4) | (d[27] << 3) | (d[28] << 2) | (d[29] << 1) | d[30]; rdbi = d[15]; }
        return b;
    }
    int findRef(int ref) {
        static const signed char nd[23] = {0, 1, 1, 0, 0, 1, 0, -1, -1, 1, -1, -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, 1, 1};
        uint8_t d[32];
        for (int n = 0; n < kBlk; n++) d[n] = S(ref, n).imag() <= 0 ? 0 : 1;
        return fuzzyMatch(nd, 23, d, kBlk);
    }

    void syncProcess() {
        for (int i = kRefAm; i <= kMaxAm; i++) for (int n = 0; n < kBlk; n++) S(kCenterAm - i, n) = -std::conj(S(kCenterAm - i, n));
        const bool ma3 = psmi == 2;
        if (!ma3) for (int i = kRefAm; i <= kPidsOuter; i++) for (int n = 0; n < kBlk; n++) S(kCenterAm + i, n) += S(kCenterAm - i, n);
        if (state == 1 && cfoWait == 0) {
            const int o = findRef(kCenterAm + kRefAm);
            if (o > 0) { keepExtra = ((kBlk - o) % kBlk) * kSymAm; cfoWait = 8; }
        } else if (cfoWait > 0) cfoWait--;
        if (state == 1) {
            const int b = findBlock(kCenterAm + kRefAm);
            if (b < 0) history = 0;
            else history = (history << 4) | (unsigned)b;
            if ((history & 0xFFFF) == 0x5670) {
                bc = 0; state = 2; syncs++; history = 0;
                mlHist.clear(); muHist.clear(); frames = 0; badRun = 0;
                sink->blockSync();
            }
        }
        if (state != 2) return;
        lastBc = bc;
        // PIDS: two 16-QAM subcarriers with training symbols in rows 8 and 24
        const int p1 = ma3 ? -kPidsInner : kPidsInner, p2 = ma3 ? kPidsInner : kPidsOuter;
        const cf32 m1 = 2.f * cf32(1.5f, -0.5f) / (S(kCenterAm + p1, 8) + S(kCenterAm + p1, 24));
        const cf32 m2 = 2.f * cf32(1.5f, -0.5f) / (S(kCenterAm + p2, 8) + S(kCenterAm + p2, 24));
        std::vector<float> ps((size_t)kBlk * 2 * 4);
        for (int n = 0; n < kBlk; n++) {
            const cf32 a = S(kCenterAm + p1, n) * m1, b = S(kCenterAm + p2, n) * m2;
            soft16(a.real(), &ps[(size_t)(n * 2) * 4]); soft16(a.imag(), &ps[(size_t)(n * 2) * 4 + 2]);
            soft16(b.real(), &ps[(size_t)(n * 2 + 1) * 4]); soft16(b.imag(), &ps[(size_t)(n * 2 + 1) * 4 + 2]);
        }
        decodePids(ps);
        // primary, secondary and tertiary: each column has two training symbols
        const int pri = ma3 ? kInnerStart : kOuterStart, sec = kMiddleStart, ter = ma3 ? kMiddleStart : kInnerStart;
        cf32 plM[kAmCols], puM[kAmCols], sM[kAmCols], tM[kAmCols];
        float se = 0;
        for (int col = 0; col < kAmCols; col++) {
            const int t1 = (5 + 11 * col) % 32, t2 = (21 + 11 * col) % 32;
            auto mult = [&](int k, cf32 ref) { return 2.f * ref / (S(k, t1) + S(k, t2)); };
            plM[col] = mult(kCenterAm - pri - col, cf32(2.5f, -2.5f));
            puM[col] = mult(kCenterAm + pri + col, cf32(2.5f, -2.5f));
            if (!ma3) { sM[col] = mult(kCenterAm + sec + col, cf32(1.5f, -0.5f)); tM[col] = mult(kCenterAm + ter + col, cf32(-0.5f, 0.5f)); }
            else { sM[col] = mult(kCenterAm + sec + col, cf32(2.5f, -2.5f)); tM[col] = mult(kCenterAm - ter - col, cf32(2.5f, -2.5f)); }
            if (col > 0) {
                se += phaseDiff(std::arg(plM[col]), std::arg(plM[col - 1]));
                se += phaseDiff(std::arg(puM[col]), std::arg(puM[col - 1]));
            }
        }
        se = se / (2 * (kAmCols - 1)) * kFftAm / (2 * kPi);
        samperr = (int)std::lround(se);
        // soft bits, weighted by the noise of each sideband
        float e = 0;
        int ne = 0;
        const size_t base = (size_t)bc * 32 * kAmCols;
        for (int n = 0; n < kBlk; n++)
            for (int col = 0; col < kAmCols; col++) {
                const size_t el = base + (size_t)n * kAmCols + (size_t)col;
                const cf32 a = S(kCenterAm - pri - col, n) * plM[col], b = S(kCenterAm + pri + col, n) * puM[col];
                if (n == 0) constel.clear();
                if ((n & 3) == 0) { constel.push_back(a); constel.push_back(b); }
                soft64(a.real(), &pl[el * 6]); soft64(a.imag(), &pl[el * 6 + 3]);
                soft64(b.real(), &pu[el * 6]); soft64(b.imag(), &pu[el * 6 + 3]);
                auto nearest = [](float v) { return std::max(-3.5f, std::min(3.5f, std::floor(v) + 0.5f)); };
                e += norm2(a - cf32(nearest(a.real()), nearest(a.imag()))) + norm2(b - cf32(nearest(b.real()), nearest(b.imag())));
                ne += 2;
                if (!ma3) {
                    const cf32 s = S(kCenterAm + sec + col, n) * sM[col], t = S(kCenterAm + ter + col, n) * tM[col];
                    soft16(s.real(), &ss[el * 4]); soft16(s.imag(), &ss[el * 4 + 2]);
                    tt[el * 2] = t.real(); tt[el * 2 + 1] = t.imag();
                }
            }
        const float m = 10.5f * (float)ne / std::max(1e-6f, e);
        merAcc += m; merN++;
        if (merN == 8) { mer = 10 * std::log10(merAcc / (float)merN); merAcc = 0; merN = 0; }
        const float w = std::min(100.f, m);       // weight: the MER of this block (linear)
        for (int n = 0; n < kBlk * kAmCols * 6; n++) { pl[base * 6 + (size_t)n] *= w; pu[base * 6 + (size_t)n] *= w; }
        if (bc == 7) frameDone(ma3);
        bc = (bc + 1) % 8;
    }

    void decodePids(const std::vector<float>& ps) {
        const AmMaps& M = amMaps();
        std::vector<float> il(120), iu(120);
        for (int n = 0; n < 120; n++) {
            il[(size_t)n] = ps[(size_t)(M.pidsRowL[(size_t)n] * 2) * 4 + (size_t)M.pidsBitL[(size_t)n]];
            iu[(size_t)n] = ps[(size_t)(M.pidsRowU[(size_t)n] * 2 + 1) * 4 + (size_t)M.pidsBitU[(size_t)n]];
        }
        const bool off1 = psmi == 1 && rdbi;
        soft.assign(240, 0.f);
        for (int i = 0; i < 10; i++)
            for (int j = 0; j < 12; j++) {
                soft[(size_t)(i * 24 + kPidsIlDelay[j])] = off1 ? 0.f : il[(size_t)(i * 12 + j)];
                soft[(size_t)(i * 24 + kPidsIuDelay[j])] = iu[(size_t)(i * 12 + j)];
            }
        bits.resize(kPidsLen);
        vit.decode(kCodeE2, soft.data(), kPidsLen, bits.data());
        scramble(bits.data(), kPidsLen);
        sink->pids(bits.data());
    }

    void frameDone(bool ma3) {
        const AmMaps& M = amMaps();
        auto get = [](const std::vector<float>& mat, int words, const AmBit& b) { return mat[(size_t)b.elem * (size_t)words + (size_t)b.bit]; };
        std::vector<float> ml(18000), mu(18000);
        for (int n = 0; n < 18000; n++) { ml[(size_t)n] = get(pl, 6, M.ml[(size_t)n]); mu[(size_t)n] = get(pu, 6, M.mu[(size_t)n]); }
        mlHist.push_back(ml); muHist.push_back(mu);
        if (mlHist.size() > 4) { mlHist.erase(mlHist.begin()); muHist.erase(muHist.begin()); }
        frames++;
        if (mlHist.size() == 4 && !ma3) {
            // the code words of this frame: B bits now, M bits from three frames ago
            const std::vector<float>& mlD = mlHist.front();
            const std::vector<float>& muD = muHist.front();
            std::vector<float> p1(72000);
            for (int i = 0; i < 6000; i++)
                for (int j = 0; j < 3; j++) {
                    p1[(size_t)(i * 12 + kBlDelay[j])] = get(pl, 6, M.bl[(size_t)(i * 3 + j)]);
                    p1[(size_t)(i * 12 + kMlDelay[j])] = mlD[(size_t)(i * 3 + j)];
                    p1[(size_t)(i * 12 + kBuDelay[j])] = get(pu, 6, M.bu[(size_t)(i * 3 + j)]);
                    p1[(size_t)(i * 12 + kMuDelay[j])] = muD[(size_t)(i * 3 + j)];
                }
            cw.assign((size_t)8 * kP1LenAm * 3, 0.f);
            size_t o = 0;
            for (size_t i = 0; i < cw.size(); i++) cw[i] = kPunctE1[i % 15] ? p1[o++] : 0.f;
            int errs = 0, cnt = 0;
            bits.resize(kP1LenAm);
            for (int b = 0; b < 8; b++) {
                const float* sp = &cw[(size_t)b * kP1LenAm * 3];
                vit.decode(kCodeE1, sp, kP1LenAm, bits.data());
                int c = 0;
                errs += reencodeErrors(kCodeE1, sp, bits.data(), kP1LenAm, &c);
                cnt += c;
                scramble(bits.data(), kP1LenAm);
                p1Frames++;
                if (!sink->transfer(bits.data(), kP1LenAm, 0)) {
                    p1Bad++;
                    if (++badRun >= 8) { state = 0; badRun = 0; return; }
                } else badRun = 0;
            }
            ber = cnt ? (double)errs / cnt : 0;
            if (!rdbi) {
                // P3 on the secondary (16-QAM) and tertiary (QPSK) sidebands
                std::vector<float> p3(36000);
                for (int i = 0; i < 6000; i++) {
                    for (int j = 0; j < 2; j++) p3[(size_t)(i * 6 + kElDelay[j])] = get(tt, 2, M.el[(size_t)(i * 2 + j)]);
                    for (int j = 0; j < 4; j++) p3[(size_t)(i * 6 + kEuDelay[j])] = get(ss, 4, M.eu[(size_t)(i * 4 + j)]);
                }
                cw.assign((size_t)kP3LenMa1 * 3, 0.f);
                o = 0;
                for (size_t i = 0; i < cw.size(); i++) cw[i] = kPunctE2[i % 6] ? p3[o++] : 0.f;
                bits.resize(kP3LenMa1);
                vit.decode(kCodeE2, cw.data(), kP3LenMa1, bits.data());
                scramble(bits.data(), kP3LenMa1);
                sink->transfer(bits.data(), kP3LenMa1, 1);
            }
        }
    }
};

AmRx::AmRx(L1Sink* sink) : p_(std::make_unique<Impl>(sink)) {}
AmRx::~AmRx() = default;
void AmRx::reset() { p_->reset(); }
void AmRx::feed(const cf32* x, size_t n) { p_->feed(x, n); }
int AmRx::state() const { return p_->state; }
L1Stats AmRx::stats() const {
    L1Stats s;
    const Impl& m = *p_;
    s.state = m.state;
    s.cfoHz = -((double)(m.prevAngle - 2 * kPi * (float)m.cfo) / kFftAm - (double)m.slope) * kRateAm / (2 * kPi);
    s.merLower = s.merUpper = m.mer;
    s.ber = m.ber;
    s.mode = m.state == 2 ? m.psmi : -1;
    s.bc = m.lastBc;
    s.syncs = m.syncs;
    s.p1Frames = m.p1Frames; s.p1Bad = m.p1Bad;
    s.constel = m.constel;
    return s;
}

// ================================================================ modulators

FmTx::FmTx() : X_((size_t)kFftFm), shape_(windowShape(kFftFm, kCpFm)) {}

// 1011s section 11: the reference subcarrier data sequence (primary), differentially encoded per block
void FmTx::block(const uint8_t* pm, const uint8_t* px, int bc, int psmi, float amp, std::vector<cf32>& out) {
    const int cm = kCompat[psmi & 63];
    const int ppb = cm == 2 ? 11 : cm == 3 ? 12 : (cm == 5 || cm == 6 || cm == 11) ? 14 : 10;   // partitions per sideband
    const int np = cm == 2 ? 1 : cm == 3 ? 2 : 0;                                                   // extended partitions per sideband
    static Fft fft(kFftFm);
    static const int rsidOf[4] = {2, 1, 0, 3};   // table 11-3, columns 0..30 (the upper columns mirror it)
    uint8_t R[61][32];
    for (int col = 0; col < 61; col++) {
        const int rsid = col <= 30 ? rsidOf[col % 4] : rsidOf[(60 - col) % 4];
        uint8_t r[32];
        const int res2 = 0, sci = 0, res1 = 0, p3isi = 1, res0 = 0;
        const int r1 = rsid >> 1, r0 = rsid & 1;
        const uint8_t sync1[7] = {0, 1, 1, 0, 0, 1, 0};
        for (int i = 0; i < 7; i++) r[i] = sync1[i];
        r[7] = (uint8_t)res2; r[8] = (uint8_t)res2; r[9] = 1;
        r[10] = (uint8_t)r1; r[11] = (uint8_t)r0; r[12] = (uint8_t)sci; r[13] = (uint8_t)(sci ^ r1 ^ r0); r[14] = 0; r[15] = (uint8_t)res1;
        for (int i = 0; i < 4; i++) r[16 + i] = (uint8_t)((bc >> (3 - i)) & 1);
        r[20] = (uint8_t)(res1 ^ ((bc >> 3) & 1) ^ ((bc >> 2) & 1) ^ ((bc >> 1) & 1) ^ (bc & 1));
        r[21] = 1; r[22] = 1; r[23] = (uint8_t)p3isi; r[24] = (uint8_t)res0;
        int par = p3isi ^ res0;
        for (int i = 0; i < 6; i++) { r[25 + i] = (uint8_t)((psmi >> (5 - i)) & 1); par ^= r[25 + i]; }
        r[31] = (uint8_t)par;
        uint8_t prev = 0;
        for (int i = 0; i < 32; i++) { prev ^= r[i]; R[col][i] = prev; }
    }
    for (int n = 0; n < kBlk; n++) {
        std::fill(X_.begin(), X_.end(), cf32(0, 0));
        auto put = [&](int sc, cf32 v) { X_[(size_t)((sc + kFftFm) % kFftFm)] = v * amp; };
        for (int i = 0; i <= ppb; i++) {
            const cf32 lo = R[i][n] ? cf32(1, 1) : cf32(-1, -1), hi = R[60 - i][n] ? cf32(1, 1) : cf32(-1, -1);
            put(-546 + i * kPw, lo);
            put(546 - i * kPw, hi);
        }
        if (px && np) {   // the extended partitions, next to the primary main ones (inner side)
            const uint8_t* xr = px + (size_t)n * (size_t)(np * 72);
            int q = 0;
            for (int p = 10; p < 10 + np; p++)
                for (int j = 1; j < kPw; j++, q += 2) put(-546 + p * kPw + j, cf32(xr[q] ? 1.f : -1.f, xr[q + 1] ? 1.f : -1.f));
            for (int p = 10 + np - 1; p >= 10; p--)
                for (int j = 1; j < kPw; j++, q += 2) put(546 - (p + 1) * kPw + j, cf32(xr[q] ? 1.f : -1.f, xr[q + 1] ? 1.f : -1.f));
        }
        const uint8_t* row = pm + (size_t)n * 720;
        for (int p = 0; p < 20; p++) {
            const int first = p < 10 ? -546 + p * kPw : 356 + (p - 10) * kPw;
            for (int j = 1; j < kPw; j++) {
                const int c = p * 36 + (j - 1) * 2;
                put(first + j, cf32(row[c] ? 1.f : -1.f, row[c + 1] ? 1.f : -1.f));
            }
        }
        fft.inverse(X_.data());
        for (int j = 0; j < kSymFm; j++) out.push_back(std::conj(shape_[(size_t)j] * X_[(size_t)(j % kFftFm)]));
    }
}

cf32 amQam64(int w) {
    static const float lv[8] = {-3.5f, 3.5f, -0.5f, 0.5f, -2.5f, 2.5f, -1.5f, 1.5f};
    return cf32(lv[w & 7], lv[(w >> 3) & 7]);
}
cf32 amQam16(int w) {
    static const float lv[4] = {-1.5f, 1.5f, -0.5f, 0.5f};
    return cf32(lv[w & 3], lv[(w >> 2) & 3]);
}
cf32 amQpsk(int w) { return cf32((w & 1) ? 0.5f : -0.5f, (w & 2) ? 0.5f : -0.5f); }

AmTx::AmTx() : X_(4096), shape_(windowShape(4096, 16 * kCpAm)) {}

// 1012s sections 11 and 12, MA1: the upper sidebands as they are, the lower ones negated and conjugated; the analog carrier is not part of it
void AmTx::block(const uint8_t* pl, const uint8_t* pu, const uint8_t* s, const uint8_t* t, const uint8_t* pids, int bc, const AmLevels& lv, std::vector<cf32>& out) {
    static Fft fft(4096);
    constexpr int N = 4096, CP = 16 * kCpAm, off = 16 * ((kFftAm - kCpAm) / 2);
    // system control data sequence (table 11-1), MA1, no reduced bandwidth
    uint8_t r[32];
    const uint8_t sync1[7] = {0, 1, 1, 0, 0, 1, 0};
    for (int i = 0; i < 7; i++) r[i] = sync1[i];
    const int pli = 0, res4 = 0, hppi = 0, aabi = 0, rdbi = 0, res3 = 0, smi = 1;
    r[7] = (uint8_t)pli; r[8] = (uint8_t)pli; r[9] = 1; r[10] = (uint8_t)res4; r[11] = (uint8_t)hppi; r[12] = (uint8_t)aabi;
    r[13] = (uint8_t)(res4 ^ hppi ^ aabi); r[14] = 0; r[15] = (uint8_t)rdbi; r[16] = (uint8_t)res3;
    for (int i = 0; i < 3; i++) r[17 + i] = (uint8_t)((bc >> (2 - i)) & 1);
    r[20] = (uint8_t)(rdbi ^ res3 ^ ((bc >> 2) & 1) ^ ((bc >> 1) & 1) ^ (bc & 1));
    r[21] = 1; r[22] = 1; r[23] = r[24] = r[25] = 0;
    int par = 0;
    for (int i = 0; i < 5; i++) { r[26 + i] = (uint8_t)((smi >> (4 - i)) & 1); par ^= r[26 + i]; }
    r[31] = (uint8_t)par;
    for (int n = 0; n < kBlk; n++) {
        std::fill(X_.begin(), X_.end(), cf32(0, 0));
        auto pair = [&](int sc, cf32 v) {        // upper subcarrier and its negated conjugate below
            X_[(size_t)sc] = v;
            X_[(size_t)(N - sc)] = -std::conj(v);
        };
        pair(1, cf32(0, r[n] ? 0.5f : -0.5f) * lv.ref);
        pair(kPidsInner, amQam16(pids[n * 2]) * lv.pids);
        pair(kPidsOuter, amQam16(pids[n * 2 + 1]) * lv.pids);
        for (int c = 0; c < kAmCols; c++) {
            const size_t e = (size_t)n * kAmCols + (size_t)c;
            X_[(size_t)(kOuterStart + c)] = amQam64(pu[e]) * lv.pri;
            X_[(size_t)(N - kOuterStart - c)] = -std::conj(amQam64(pl[e])) * lv.pri;
            pair(kMiddleStart + c, amQam16(s[e]) * lv.sec);
            pair(kInnerStart + c, amQpsk(t[e]) * lv.ter);
        }
        fft.inverse(X_.data());
        for (int j = 0; j < N + CP; j++) out.push_back(shape_[(size_t)j] * X_[(size_t)((j + off) % N)]);
    }
}

}} // namespace dect2::hdr
