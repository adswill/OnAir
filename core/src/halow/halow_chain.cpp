// Wi-Fi HaLow (802.11ah) receive chain at one internal rate (halow_chain.h): front end, STF detector, and per candidate the long training symbols, the
// SIG field of the S1G_1M / S1G_SHORT / S1G_LONG preambles and the data field. IEEE 802.11-2020 clause 23 (S1G PHY).
//
// The STF repeats every 8 us at every width, so a delay-and-correlate detector (lag 8 us) finds a preamble and estimates the carrier offset. For each candidate
// every width of the chain is tried: the long training symbols are matched for timing, the residual offset and the channel are estimated from them, the SIG
// field is decoded (BPSK, rate 1/2; the short and long preambles told apart by the rotation of the second SIG symbol) and must agree with the width and carry a
// valid MCS / stream combination; then the data field is decoded when it is one spatial stream of BCC.
#include "dect2/halow/halow_chain.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/halow/halow_demod.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <deque>
#include "dect2/common/mathconst.h"

namespace dect2 {

using namespace halow;

namespace {
using dect2::math::kPi;
constexpr float kThr = 0.30f;           // detector threshold on the normalised correlation

struct Cand {
    int64_t fire = 0;
    double cfo = 0;                      // from the STF
    int stage = 0;                       // 0 preamble not yet looked at, 1 SIG read, waiting for the data field
    int64_t s0 = 0;                      // first sample of the working segment
    double cfoTotal = 0;
    int w = 0;
    int64_t b1 = 0;                      // first long training symbol
    Sig sig;
    std::vector<cf32> H;
    float sigma2 = 1e-9f;                // noise power of one tone of the training symbols' spectrum
    float rssi = -200, snr = 0;
    int64_t needEnd = 0;
};

struct Seg {
    std::vector<cf32> x;
    int64_t s0 = 0;
};
}

struct HalowChain::Impl {
    HalowChain& self;
    const double inRate, Ri;
    const int S, N, lag, kW, kRunMin, delta, back;
    int64_t gate;
    std::vector<int> widths;
    int mask = 31;
    Demod dm;
    Stats st;
    int64_t nIn = 0;
    // front end: nothing when the input already is at the chain rate, else the exact (windowed sinc) resampler: a linear interpolator would
    // make the channel vary with the sample phase, which BPSK survives and 64-QAM does not
    bool direct = true;
    ExactResampler rs;
    std::vector<cf32> rsOut;
    // detector: the samples and, in step with them, the lag products y[n] conj(y[n - lag]) and the two powers, so the sliding sums are plain additions
    std::vector<cf32> buf;
    std::vector<float> pRe, pIm, q1, q2;
    int64_t base = 0, g = 0;
    std::complex<double> sa{0, 0};
    double sp1 = 0, sp2 = 0;
    int run = 0;
    bool armed = true;
    int64_t quietUntil = 0;
    std::deque<Cand> cands;

    Impl(HalowChain& s, double in, double ri, const std::vector<int>& w)
        : self(s), inRate(in), Ri(ri), S((int)std::lround(ri / 4e6)), N(128 * S), lag(32 * S), kW(96 * S), kRunMin(64 * S), delta(8 * S), back(520 * S), widths(w), dm(128 * S) {
        bool w1 = false;
        for (int x : w) if (x == W1) w1 = true;
        gate = w1 ? 2300 : (int64_t)(240e-6 * Ri) + 500 * S;
        direct = std::fabs(inRate - Ri) < 1.0;
        if (!direct) rs.configure(inRate, Ri, 8);   // 8 lobes: about 55 dB of rejection at half the cost of the 16 lobe filter (three chains run at 20 Msps)
    }

    bool active() const { for (int w : widths) if (mask >> w & 1) return true; return false; }

    // Takes mixed samples: resamples them to the chain rate (unless equal) and runs the detector.
    void feed(const cf32* x, size_t n) {
        nIn += (int64_t)n;
        if (!active()) return;   // a manual width that this chain does not carry: nothing to resample or detect
        if (direct) { detect(x, n); return; }
        rsOut.clear();
        rs.process(x, n, rsOut);
        detect(rsOut.data(), rsOut.size());
    }

    // ---------------------------------------------------------------- detector

    // The samples at the chain rate: lag products and powers in one pass (which vectorises), then the sliding window sums and the threshold in a second
    void detect(const cf32* y, size_t n) {
        if (!n) return;
        const size_t n0 = buf.size();
        buf.insert(buf.end(), y, y + n);
        pRe.resize(n0 + n); pIm.resize(n0 + n); q1.resize(n0 + n); q2.resize(n0 + n);
        const size_t L = (size_t)lag;
        for (size_t i = n0; i < n0 + n; i++) {
            const float yr = buf[i].real(), yi = buf[i].imag();
            const float zr = i >= L ? buf[i - L].real() : 0.f, zi = i >= L ? buf[i - L].imag() : 0.f;   // before the first lag: nothing to correlate with
            pRe[i] = yr * zr + yi * zi; pIm[i] = yi * zr - yr * zi; q1[i] = yr * yr + yi * yi; q2[i] = zr * zr + zi * zi;
        }
        g += (int64_t)n;
        const size_t W = (size_t)kW;
        for (size_t i = n0; i < n0 + n; i++) {
            double dr = pRe[i], di = pIm[i], d1 = q1[i], d2 = q2[i];
            if (i >= W) { dr -= pRe[i - W]; di -= pIm[i - W]; d1 -= q1[i - W]; d2 -= q2[i - W]; }
            sa += std::complex<double>(dr, di); sp1 += d1; sp2 += d2;
            // |sum| / sqrt(p1 p2) > thr, written with squares: no square root per sample
            const double a2 = std::norm(sa), pp = sp1 * sp2 + 1e-30;
            if (a2 > (double)kThr * kThr * pp && sp1 > 1e-8) run++; else run = 0;
            if (a2 < 0.09 * pp) armed = true;
            const int64_t nAbs = base + (int64_t)i;
            if (armed && run >= kRunMin && nAbs >= quietUntil) {
                armed = false;
                st.preambles++;
                if (cands.size() >= 8) continue;   // noise looks like STF forever; unbounded candidates blew the heap on Start
                std::complex<double> s(0, 0);
                for (size_t k = 1; k <= (size_t)(64 * S) && k <= i + 1; k++) s += std::complex<double>(pRe[i + 1 - k], pIm[i + 1 - k]);
                Cand c;
                c.fire = nAbs;
                c.cfo = std::arg(s) * Ri / (2 * kPi * lag);
                cands.push_back(c);
            }
        }
    }

    // ---------------------------------------------------------------- the working segment

    // the samples s0 .. end of the buffer, rotated by -cfo; false when they are not all there (or too many)
    bool makeSeg(Seg& s, int64_t s0, int64_t end, double cfo) const {
        if (s0 < base || end > g || end <= s0) return false;
        if (end - s0 > 131072LL * S) return false;   // a waiting candidate must not copy the whole buffer
        s.s0 = s0;
        s.x.resize((size_t)(end - s0));
        const std::complex<double> stp = std::polar(1.0, -2 * kPi * cfo / Ri);
        std::complex<double> r(1, 0);
        for (size_t i = 0; i < s.x.size(); i++) {
            const cf32& v = buf[(size_t)(s0 - base) + i];
            s.x[i] = cf32(v.real() * (float)r.real() - v.imag() * (float)r.imag(), v.real() * (float)r.imag() + v.imag() * (float)r.real());
            r *= stp;
            if ((i & 1023) == 1023) r /= std::abs(r);
        }
        return true;
    }

    // geometry (clause 23.3.8): symbol starts counted from the first long training symbol b1 (at 1 MHz b1 is the FFT window of the first LTF symbol)
    int64_t sigStart(int w, int64_t b1) const { const int full = N + N / 4; return w == W1 ? b1 - N / 4 + 4LL * full : b1 + 2LL * N; }
    int64_t dataStart(const Sig& s, int64_t b1) const { return sigStart(s.w, b1) + (int64_t)(info(s.w).sigSyms + extraSyms(s.pre, s.nsts)) * (N + N / 4); }
    int nsymOf(const Sig& s) const { return s.ndp ? 0 : s.agg ? s.len : dataSymbols(s.w, s.mcs, s.len, s.nss(), s.stbc); }
    int64_t frameEnd(const Sig& s, int64_t b1) const { return dataStart(s, b1) + (int64_t)nsymOf(s) * symLen(s.w, s.sgi); }

    // ---------------------------------------------------------------- one candidate

    // 0 failed (drop it), 1 done, 2 wait for more samples
    int tryCand(Cand& c) {
        if (c.stage == 0) {
            if (g < c.fire + gate) return 2;
            const int64_t s0 = std::max(base, c.fire - back);
            Seg seg;
            if (!makeSeg(seg, s0, std::min<int64_t>(g, c.fire + gate + 200LL * S), c.cfo)) return 0;
            struct Hyp { int w; int64_t b1; double q; };
            std::vector<Hyp> hyps;
            // The detector fires about 109 samples (x S) after the STF starts, give or take 6 on a strong signal: every width is searched +-40 (x S) around that;
            // only when none of them gives a clear match (the training field of the right width scores 0.7 and up, a neighbour's 0.27) the wide window of a weak signal's
            // jittering detection (-150 centre, +-128) is searched too.
            for (int pass = 0; pass < 2; pass++) {
                if (pass == 1 && (!hyps.empty() && hyps.front().q >= 0.6)) break;
                std::vector<Hyp> found;
                for (int w : widths) {
                    if (!(mask >> w & 1)) continue;
                    const bool w1 = w == W1;
                    const int ltag = w1 ? N + N / 4 : N, K = w1 ? 4 : 2;
                    std::vector<cf32> Lt((size_t)N);
                    ltfTime(w, Lt.data());
                    double eL = 0;
                    for (const cf32& v : Lt) eL += std::norm(v);
                    const int64_t bStf = info(w).stfSamples + (w1 ? N / 4 : N / 2);
                    const int64_t from = pass == 0 ? c.fire - 109LL * S + bStf - 40LL * S : c.fire - 150LL * S + bStf - 128LL * S;
                    const int64_t to = pass == 0 ? c.fire - 109LL * S + bStf + 40LL * S : c.fire - 150LL * S + bStf + 128LL * S;
                    double bestM = 0; int64_t bestB = -1;
                    for (int64_t b = from; b <= to; b++) {
                        const int64_t r = b - s0;
                        if (r < 0 || r + (int64_t)(K - 1) * ltag + N > (int64_t)seg.x.size()) continue;
                        double m = 0;
                        for (int k = 0; k < K; k++) {
                            std::complex<float> s(0, 0);
                            const cf32* p = seg.x.data() + r + (int64_t)k * ltag;
                            for (int i = 0; i < N; i++) s += p[i] * std::conj(Lt[(size_t)i]);
                            m += std::abs(s);
                        }
                        if (m > bestM) { bestM = m; bestB = b; }
                    }
                    if (bestB < 0) continue;
                    double eW = 0;
                    const cf32* p = seg.x.data() + (bestB - s0);
                    for (int k = 0; k < K; k++) for (int i = 0; i < N; i++) eW += std::norm(p[(int64_t)k * ltag + i]);
                    const double q = bestM / std::sqrt(eL * eW / K + 1e-30) / K;
                    if (q > 0.4) found.push_back({w, bestB, q});   // a weaker match is another width's training field or noise
                }
                for (const Hyp& f : found) {   // keep the better match per width
                    bool have = false;
                    for (Hyp& h : hyps) if (h.w == f.w) { have = true; if (f.q > h.q) h = f; }
                    if (!have) hyps.push_back(f);
                }
                std::sort(hyps.begin(), hyps.end(), [](const Hyp& a, const Hyp& b) { return a.q > b.q; });
            }
            std::sort(hyps.begin(), hyps.end(), [](const Hyp& a, const Hyp& b) { return a.q > b.q; });
            for (const Hyp& h : hyps) { Seg work = seg; if (sigAttempt(c, work, h.w, h.b1)) break; }
            if (c.stage == 0) { st.sigBad++; return 0; }
            st.sigOk++;
            st.perWidth[c.w]++;
            quietUntil = std::max(quietUntil, std::min<int64_t>(frameEnd(c.sig, c.b1), c.b1 + 400000LL * S));
        }
        return decodeData(c);
    }

    // the data field: one spatial stream of BCC is decoded; everything else is reported from the SIG field
    int decodeData(Cand& c) {
        const Sig& sg = c.sig;
        const WidthInfo& wi = info(c.w);
        HalowEvent ev;
        ev.sig = sg; ev.cfoHz = c.cfoTotal; ev.rssi = c.rssi; ev.snrDb = c.snr;
        const int nss = sg.nss();
        const char* why = sg.ndp ? "NDP" : sg.stbc ? "STBC" : nss > 1 ? "spatial streams" : sg.ldpc ? "LDPC" : sg.agg ? "A-MPDU" : (sg.len < 1 ? "empty" : "");
        if (why[0]) { ev.why = why; if (self.onEvent) self.onEvent(ev); return 1; }
        const int nsym = nsymOf(sg);
        if (nsym <= 0 || nsym > 1024) { ev.why = "length"; if (self.onEvent) self.onEvent(ev); return 1; }
        const int sl = symLen(c.w, sg.sgi), giD = sg.sgi ? N / 8 : N / 4;
        const int64_t d0 = dataStart(sg, c.b1);
        c.needEnd = d0 + (int64_t)nsym * sl + 8;
        if (g < c.needEnd) return 2;
        Seg seg;
        if (!makeSeg(seg, c.s0, c.needEnd, c.cfoTotal)) return 0;
        std::vector<cf32> H = c.H;
        if (sg.pre == PreLong) {   // the long preamble's data is sent through the D-LTF's channel (beamforming): read it from there
            const int64_t at = sigStart(c.w, c.b1) + (int64_t)(wi.sigSyms + 1) * (N + N / 4) + N / 4 - delta - c.s0;
            if (at < 0 || at + N > (int64_t)seg.x.size()) return 0;
            std::vector<cf32> Y((size_t)N);
            std::vector<float> lt((size_t)N);
            ltfSpectrum(c.w, lt.data());
            dm.fftWin(seg.x.data() + at, Y.data());
            for (int t = 0; t < N; t++) H[(size_t)t] = lt[(size_t)t] != 0.f ? Y[(size_t)t] * lt[(size_t)t] : cf32(0, 0);
        }
        const int polBase = wi.sigSyms + (sg.pre == PreLong ? 1 : 0);
        std::vector<float> coded, nvv;
        std::vector<cf32> z;
        for (int j = 0; j < nsym; j++) {
            const int64_t at = d0 + (int64_t)j * sl + giD - delta - c.s0;
            if (at < 0 || at + N > (int64_t)seg.x.size()) { st.dataBad++; return 1; }
            if (!dm.dataSym(seg.x.data() + at, wi.data, polBase + j, H.data(), c.sigma2, z, nvv)) { st.dataBad++; return 1; }
            symbolLlr(z, nvv, wi, sg.mcs, coded);
        }
        if (!decodeDataField(coded, c.w, sg.mcs, nsym, sg.len, ev.psdu)) { st.dataBad++; return 1; }
        ev.psduValid = true;
        if (self.onEvent) self.onEvent(ev);
        return 1;
    }

    // the long training symbols at b1: residual offset, channel, SIG field of each preamble type; true when a SIG field passed
    bool sigAttempt(Cand& c, Seg& seg, int w, int64_t b1) {
        const WidthInfo& wi = info(w);
        const bool w1 = w == W1;
        const int full = N + N / 4;
        const int ltag = w1 ? full : N, K = w1 ? 4 : 2;
        const int64_t r1 = b1 - seg.s0 - delta;
        if (r1 < 0) return false;
        const int64_t sigWin = r1 + (w1 ? 4LL * full : 2LL * N + N / 4);
        const int64_t endSig = sigWin + (int64_t)(wi.sigSyms - 1) * full + N;
        if (endSig + 8 > (int64_t)seg.x.size()) return false;
        if (r1 + (int64_t)(K - 1) * ltag + N > (int64_t)seg.x.size()) return false;
        std::vector<float> lt((size_t)N);
        ltfSpectrum(w, lt.data());
        // residual carrier offset from the repeats of the training symbol
        std::vector<std::vector<cf32>> Y((size_t)K, std::vector<cf32>((size_t)N));
        for (int k = 0; k < K; k++) dm.fftWin(seg.x.data() + r1 + (int64_t)k * ltag, Y[(size_t)k].data());
        std::complex<double> d(0, 0);
        for (int k = 0; k + 1 < K; k++) for (int t = 0; t < N; t++) if (lt[(size_t)t] != 0.f) { const cf32 z = Y[(size_t)k + 1][(size_t)t] * std::conj(Y[(size_t)k][(size_t)t]); d += std::complex<double>(z.real(), z.imag()); }
        const double fres = std::arg(d) * Ri / (2 * kPi * ltag);
        {
            const std::complex<double> stp = std::polar(1.0, -2 * kPi * fres / Ri);
            std::complex<double> rr(1, 0);
            for (size_t i = 0; i < seg.x.size(); i++) { seg.x[i] *= cf32((float)rr.real(), (float)rr.imag()); rr *= stp; if ((i & 1023) == 1023) rr /= std::abs(rr); }
        }
        for (int k = 0; k < K; k++) dm.fftWin(seg.x.data() + r1 + (int64_t)k * ltag, Y[(size_t)k].data());
        std::vector<cf32> H((size_t)N, cf32(0, 0));
        double dev = 0; int dn = 0;
        for (int t = 0; t < N; t++) {
            if (lt[(size_t)t] == 0.f) continue;
            cf32 s(0, 0);
            for (int k = 0; k < K; k++) s += Y[(size_t)k][(size_t)t];
            s *= 1.f / (float)K;
            H[(size_t)t] = s * lt[(size_t)t];   // lt = +-1: the same as dividing
            for (int k = 0; k < K; k++) { dev += std::norm(Y[(size_t)k][(size_t)t] - s); dn++; }
        }
        const float sigma2 = dn ? (float)(dev / dn * K / std::max(1, K - 1)) : 1e-9f;   // noise power of one tone, from the spread of the repeats
        // SIG field: the soft bits of every symbol for BPSK and for QBPSK, then each preamble's reading
        std::vector<std::vector<float>> re((size_t)wi.sigSyms), im((size_t)wi.sigSyms);
        double nsum = 0; int ncnt = 0;
        for (int j = 0; j < wi.sigSyms; j++)
            if (!dm.sigSym(seg.x.data() + sigWin + (int64_t)j * full, wi.sig, j, H.data(), re[(size_t)j], im[(size_t)j], nsum, ncnt)) return false;
        Sig sg;
        bool ok = false;
        // S1G_LONG rotates the second SIG-A symbol by 90 degrees (QBPSK): whichever axis carries the energy of that symbol tells the preamble type
        int preGuess = Pre1M;
        if (!w1) {
            double eRe = 0, eIm = 0;
            for (float v : re[1]) eRe += std::fabs(v);
            for (float v : im[1]) eIm += std::fabs(v);
            preGuess = eIm > eRe ? PreLong : PreShort;
        }
        for (int pre : {preGuess}) {
            std::vector<float> coded;
            for (int j = 0; j < wi.sigSyms; j++) {
                const std::vector<float>& v = (pre == PreLong && j == 1) ? im[(size_t)j] : re[(size_t)j];
                std::vector<float> raw((size_t)wi.sigNcbps);
                for (int k = 0; k < wi.sigNcbps; k++) raw[(size_t)k] = v[(size_t)interleave(wi.sigNcol, wi.sigNrow, k)];
                if (w1) for (int i = 0; i < wi.sigNcbps / 2; i++) coded.push_back(raw[(size_t)i] + raw[(size_t)(i + wi.sigNcbps / 2)]);
                else coded.insert(coded.end(), raw.begin(), raw.end());
            }
            const std::vector<uint8_t> bits = viterbi(coded);
            if ((int)bits.size() < wi.sigInfoBits || !parseSig(bits.data(), w, sg, pre)) continue;
            ok = true;
            break;
        }
        if (!ok) return false;
        c.stage = 1; c.w = w; c.b1 = b1; c.sig = sg; c.H = H; c.sigma2 = sigma2;
        c.s0 = seg.s0; c.cfoTotal = c.cfo + fres;
        double pw = 0; const int64_t n0 = r1 + delta, n1 = std::min<int64_t>((int64_t)seg.x.size(), n0 + 256LL * S);
        for (int64_t i = n0; i < n1; i++) pw += std::norm(seg.x[(size_t)i]);
        c.rssi = (float)(10 * std::log10(pw / std::max<int64_t>(1, n1 - n0) + 1e-20));
        const double noise = ncnt ? nsum / ncnt : 1;
        c.snr = (float)std::max(0.0, std::min(40.0, -10 * std::log10(noise + 1e-6)));
        return true;
    }

    // ---------------------------------------------------------------- feed side

    // Works the waiting candidates (a candidate waits until its samples are in) and drops samples nobody needs any more.
    void process() {
        while (!cands.empty()) {
            Cand& c = cands.front();
            if (c.fire < quietUntil && c.stage == 0) { cands.pop_front(); continue; }
            const int r = tryCand(c);
            if (r == 2) break;
            cands.pop_front();
        }
        // keep the buffer short
        int64_t keep = g - 16384LL * S;
        if (!cands.empty()) keep = std::min(keep, cands.front().stage == 0 ? cands.front().fire - back - 8 : cands.front().s0);
        if (keep > base && keep - base > 262144LL * S) {
            size_t drop = (size_t)(keep - base - 16384LL * S);
            if (drop > buf.size()) drop = buf.size();
            if (drop) {
                buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)drop);
                pRe.erase(pRe.begin(), pRe.begin() + (ptrdiff_t)drop); pIm.erase(pIm.begin(), pIm.begin() + (ptrdiff_t)drop);
                q1.erase(q1.begin(), q1.begin() + (ptrdiff_t)drop); q2.erase(q2.begin(), q2.begin() + (ptrdiff_t)drop);
                base += (int64_t)drop;
            }
        }
        while (cands.size() > 8) cands.pop_front();
    }
};

HalowChain::HalowChain(double inRate, double chainRate, const std::vector<int>& widths) : p_(std::make_unique<Impl>(*this, inRate, chainRate, widths)) {}
HalowChain::~HalowChain() = default;
void HalowChain::setWidthMask(int mask) { p_->mask = mask; }
void HalowChain::feed(const cf32* mixed, size_t n) { p_->feed(mixed, n); }
void HalowChain::process() { p_->process(); }
int64_t HalowChain::samples() const { return p_->nIn; }
const HalowChain::Stats& HalowChain::stats() const { return p_->st; }

} // namespace dect2
