// DVB-SH OFDM (SH-A) receiver, EN 302 583 V1.2.1. See dvbsh_rx.h.
#include "dect2/dvbsh/dvbsh_rx.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/dsp/fftutil.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace dect2 {
namespace dvbsh {

namespace {
constexpr float kLlrScale = 4.f;   // int8 LLRs in the time de-interleaver: 1/4 steps, +-31.75
constexpr double kTwoPi = 6.283185307179586;
}

struct Receiver::Impl {
    double fs = 64e6 / 7;
    bool lowPriority = false;
    size_t maxTiBytes = (size_t)768 << 20;
    std::function<void(const uint8_t*, size_t)> cb;
    RxStatus st;

    // sample buffer: buf[0] is absolute sample base
    std::vector<cf32> buf;
    long long base = 0;

    // acquisition
    int level = 0;
    int mode = 0, guard = 0, N = 0, G = 0, S = 0, K = 0, kc = 0;
    long long pos = 0;            // absolute start of the next FFT window
    double fnorm = 0;             // carrier offset in cycles per sample (removed)
    bool intKnown = false;
    int intVotes = 0, intCand = 0;
    std::unique_ptr<Fft> fft;
    std::vector<cf32> Y, Yprev;   // carriers 0..K-1 of this and of the previous symbol
    bool havePrev = false;
    std::deque<uint8_t> tpsHist;  // differential TPS bits
    int symIdx = -1, frameIdx = 0;   // of the next symbol, once frame synchronised
    int tpsFails = 0;
    uint8_t frameBits[68];
    TpsFrame tf[4];
    bool tfOk[4] = {false, false, false, false};
    int symsSinceSync = 0;
    double drift = 0;             // accumulated timing slope (rad per carrier)
    int shiftPending = 0;

    // channel estimate
    std::vector<cf32> Hp;         // at pilot carriers (k % 3 == 0)
    float sigma2 = 0.1f;
    double sigAvg = 1;

    // data path
    bool paramsKnown = false;
    Params p;
    int sf = 0;                   // super-frame number in the SH frame of the current super-frame
    bool dataOn = false;
    int pat = 4;
    TimeInterleaver<int8_t> ti;
    std::vector<int> L;
    long long gIn = 0;            // IUs into the de-interleaver since the data path started
    std::vector<int8_t> iuAcc;
    std::vector<float> cwLlr;
    int cwIdx = -1;               // codeword being assembled (by SH position), -1 none
    int cwFill = 0;
    bool cwValid = false;
    long iterSum = 0;

    void resetAll() {
        buf.clear(); base = 0; level = 0; intKnown = false; intVotes = 0; havePrev = false; tpsHist.clear(); symIdx = -1; tpsFails = 0;
        for (bool& b : tfOk) b = false;
        paramsKnown = false; dataOn = false; fnorm = 0; drift = 0;
        RxStatus keep;
        keep.codewordsOk = st.codewordsOk; keep.codewordsBad = st.codewordsBad; keep.packetsOk = st.packetsOk; keep.packetsBad = st.packetsBad;
        keep.tpsFrames = st.tpsFrames;
        st = keep;
    }

    // ------------------------------------------------------------------ search: FFT size, guard, timing, fractional offset
    bool search() {
        const size_t need = 80000;
        if (buf.size() < need) return false;
        const int len = (int)need;
        double best = 0;
        int bm = -1, bg = 0, bt = 0;
        double bphase = 0;
        std::vector<std::complex<double>> c, acc;
        std::vector<double> e, en;
        for (int m : {k1K, k2K, k4K, k8K}) {
            const int n = fftN(m);
            const int L = len - n;
            c.assign(L, 0); e.assign(L, 0);
            for (int i = 0; i < L; i++) {
                c[i] = std::complex<double>(buf[i]) * std::conj(std::complex<double>(buf[i + n]));
                e[i] = 0.5 * (std::norm(buf[i]) + std::norm(buf[i + n]));
            }
            for (int g = 0; g < 4; g++) {
                const int gl = guardSamples(m, g), s = n + gl;
                if (L - gl < 3 * s) continue;
                acc.assign(s, 0); en.assign(s, 0);
                std::complex<double> w = 0;
                double we = 0;
                for (int i = 0; i < gl; i++) { w += c[i]; we += e[i]; }
                for (int i = 0; i + gl < L; i++) {
                    acc[i % s] += w; en[i % s] += we;
                    w += c[i + gl] - c[i]; we += e[i + gl] - e[i];
                }
                for (int t = 0; t < s; t++) {
                    const double r = std::abs(acc[t]) / (en[t] + 1e-30);
                    if (r > best) { best = r; bm = m; bg = g; bt = t; bphase = std::arg(acc[t]); }
                }
            }
        }
        if (bm < 0 || best < 0.25) {   // nothing OFDM-like: drop most of the buffer and wait
            buf.erase(buf.begin(), buf.begin() + (long)(need / 2));
            base += (long long)(need / 2);
            return false;
        }
        mode = bm; guard = bg; N = fftN(mode); G = guardSamples(mode, guard); S = N + G; K = carriersK(mode); kc = (K - 1) / 2;
        fft = std::make_unique<Fft>(N);
        fnorm = -bphase / (kTwoPi * N);
        ncoPhase = 0; ncoPos = base;
        const int backoff = std::max(1, G / 8);
        pos = base + bt + G - backoff;
        Y.assign(K, 0); Yprev.assign(K, 0); Hp.assign(K, 0);
        level = 1; st.level = 1;
        if (getenv("DVBSH_DEBUG")) fprintf(stderr, "dvbsh: search %s GI %d metric %.3f t %d fnorm %.6f\n", modeName(mode), guard, best, bt, fnorm);
        intKnown = false; intVotes = 0; havePrev = false; symIdx = -1; tpsHist.clear(); drift = 0;
        return true;
    }

    // ------------------------------------------------------------------ one OFDM symbol
    std::vector<cf32> X, lastX;
    double ncoPhase = 0;          // carrier phase removed at sample ncoPos (cycles)
    long long ncoPos = 0;
    void fftAt(long long at) {
        X.resize(N);
        const long long off = at - base;
        // a phase accumulator: a change of fnorm only bends the phase from here on
        ncoPhase += fnorm * (double)(at - ncoPos);
        ncoPhase -= std::floor(ncoPhase);
        ncoPos = at;
        for (int i = 0; i < N; i++) {
            const double ph = -kTwoPi * (ncoPhase + fnorm * i);
            X[i] = buf[(size_t)(off + i)] * cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        fft->forward(X.data());
    }
    inline int bin(int k) const { return ((k - kc) % N + N) % N; }

    void processSymbol() {
        fftAt(pos);
        pos += S;
        if (!intKnown) {
            // integer offset: the continual pilots repeat from symbol to symbol, the data do not
            std::vector<cf32>& last = lastX;
            if (last.size() == (size_t)N) {
                const auto& cp = continualPilots(mode);
                const int dmax = std::min(N / 8, 160);
                double bestm = 0, sum = 0;
                int bd = 0;
                for (int d = -dmax; d <= dmax; d++) {
                    std::complex<double> z = 0;
                    for (int k : cp) { const int b = (bin(k) + d + N) % N; z += std::complex<double>(X[b]) * std::conj(std::complex<double>(last[b])); }
                    const double m = std::abs(z);
                    sum += m;
                    if (m > bestm) { bestm = m; bd = d; }
                }
                const double mean = sum / (2 * dmax + 1);
                if (bestm > 3 * mean) {
                    if (intVotes > 0 && bd == intCand) intVotes++;
                    else { intCand = bd; intVotes = 1; }
                }
                if (intVotes >= 3) {
                    intKnown = true;
                    if (getenv("DVBSH_DEBUG")) fprintf(stderr, "dvbsh: integer offset %d\n", intCand);
                    fnorm += (double)intCand / N;
                    last.clear();
                    havePrev = false;
                    return;
                }
            }
            last = X;
            return;
        }
        for (int k = 0; k < K; k++) Y[k] = X[bin(k)];
        if (!havePrev) { Yprev = Y; havePrev = true; return; }

        // common phase between symbols on the continual pilots: the residual carrier offset
        const auto& cp = continualPilots(mode);
        std::complex<double> zc = 0;
        for (int k : cp) zc += std::complex<double>(Y[k]) * std::conj(std::complex<double>(Yprev[k]));
        const double cpe = std::arg(zc);
        fnorm += 0.05 * cpe / (kTwoPi * S);
        st.cfoHz = fnorm * fs;

        // TPS: differential BPSK, the same bit on every TPS carrier
        std::complex<double> zt = 0;
        for (int k : tpsCarriers(mode)) zt += std::complex<double>(Y[k]) * std::conj(std::complex<double>(Yprev[k]));
        const uint8_t tb = zt.real() < 0 ? 1 : 0;

        if (symIdx < 0) {
            tpsHist.push_back(tb);
            if (tpsHist.size() > 68) tpsHist.pop_front();
            if (tpsHist.size() == 68) {
                uint8_t b[68];
                for (int i = 0; i < 68; i++) b[i] = tpsHist[i];
                TpsFrame f;
                if (tpsDecode(b, f, 1) && f.mode == mode && f.guard == guard) {
                    if (getenv("DVBSH_DEBUG")) fprintf(stderr, "dvbsh: TPS sync frame %d\n", f.frameIdx);
                    symIdx = 0; frameIdx = (f.frameIdx + 1) & 3; symsSinceSync = 0; tpsFails = 0;
                    tf[f.frameIdx] = f; tfOk[f.frameIdx] = true;
                    for (int i = 0; i < 4; i++) if (i != f.frameIdx) tfOk[i] = false;
                    level = std::max(level, 1);
                    st.tpsFrames++;
                }
            }
            Yprev = Y;
            return;
        }
        const int l = symIdx;
        frameBits[l] = tb;
        symbolData(l, cpe);
        if (l == 67) endOfFrame();
        Yprev = Y;
        if (++symIdx == 68) { symIdx = 0; frameIdx = (frameIdx + 1) & 3; }
    }

    void endOfFrame() {
        TpsFrame f;
        if (tpsDecode(frameBits, f, 2) && f.frameIdx == frameIdx && f.mode == mode && f.guard == guard && f.sh) {
            tpsFails = 0;
            tf[frameIdx] = f; tfOk[frameIdx] = true;
            st.tpsFrames++;
        } else {
            tfOk[frameIdx] = false;
            if (++tpsFails >= 3) { lostLock(); return; }
        }
        if (frameIdx == 3 && tfOk[0] && tfOk[1] && tfOk[2] && tfOk[3]) {
            Params q;
            int sfq = 0;
            if (tpsCombine(tf, q, sfq)) {
                q.bwMhz = p.bwMhz;
                const bool same = paramsKnown && q.mode == p.mode && q.guard == p.guard && q.mod == p.mod && q.hier == p.hier && q.patHp == p.patHp &&
                                  q.patLp == p.patLp && q.ti == p.ti;
                if (!same) { p = q; paramsKnown = true; dataOn = false; }
                p.cellId = q.cellId;
                sf = (sfq + 1) % superFramesPerShFrame(p);   // the next super-frame
                level = std::max(level, 2);
                st.level = std::max(st.level, 2);
                st.p = p; st.sfInSh = sf;
                if (!dataOn) startData();
            }
        } else if (frameIdx == 3 && paramsKnown) {
            sf = (sf + 1) % superFramesPerShFrame(p);
        }
    }

    void lostLock() {
        level = 0; st.level = 0; symIdx = -1; paramsKnown = false; dataOn = false; intKnown = false; intVotes = 0; havePrev = false; lastX.clear();
        tpsHist.clear();
        for (bool& b : tfOk) b = false;
        // restart the search from here
        const long long drop = std::min<long long>((long long)buf.size(), pos - base);
        if (drop > 0) { buf.erase(buf.begin(), buf.begin() + (long)drop); base += drop; }
    }

    void startData() {
        st.tiTooLarge = false;
        if (!tiDelays(p.ti, kShFrameCu, L)) { dataOn = false; return; }
        int lmax = 0;
        size_t bytes = 0;
        for (int v : L) { lmax = std::max(lmax, v); bytes += (size_t)v * kIuBits; }
        st.tiLmax = lmax; st.tiBytes = bytes;
        if (bytes > maxTiBytes) { st.tiTooLarge = true; dataOn = false; return; }
        ti.configure(L, true);
        pat = lowPriority && p.hier ? p.patLp : p.patHp;
        gIn = 0; iuAcc.clear(); cwIdx = -1; cwFill = 0; cwValid = false;
        cwLlr.assign(nbil(pat), 0.f);
        dataOn = true;
        dataStarted = false;
    }
    bool dataStarted = false;

    // ------------------------------------------------------------------ channel estimate, equaliser, demapper, de-interleavers
    std::vector<uint8_t> roles;
    std::vector<cf32> cells, deint;
    std::vector<float> n0, n0d, llr, stream;
    std::vector<std::complex<double>> zbuf;
    void symbolData(int l, double /*cpe*/) {
        const auto& w = dvbt::prbsW();
        const auto& cp = continualPilots(mode);
        auto pv = [&](int k) { return (w[k] ? -1.f : 1.f) * 4.f / 3.f; };
        carrierRoles(mode, l, roles);
        // the window moved by a sample after the last symbol: turn the estimates with it
        if (shiftPending) {
            for (int k = 0; k < K; k += 3) {
                const double a = -shiftPending * kTwoPi * (k - kc) / N;
                Hp[k] *= cf32((float)std::cos(a), (float)std::sin(a));
            }
            shiftPending = 0;
        }
        // phase and timing drift since the last estimate, from the continual pilots
        if (symsSinceSync > 0) {
            // common phase, then a least-squares phase slope across the continual pilots (timing drift)
            std::complex<double> z0 = 0;
            double err = 0, pw = 0;
            zbuf.resize(cp.size());
            for (size_t i = 0; i < cp.size(); i++) {
                const int k = cp[i];
                zbuf[i] = std::complex<double>(Y[k]) * std::conj(std::complex<double>(Hp[k]) * (double)pv(k));
                z0 += zbuf[i];
            }
            const double ph0 = std::arg(z0);
            double kbar = 0;
            for (int k : cp) kbar += k;
            kbar /= cp.size();
            double num = 0, den = 0;
            for (size_t i = 0; i < cp.size(); i++) {
                const double ph = std::arg(zbuf[i] * std::conj(z0));
                const double dk = cp[i] - kbar;
                num += dk * ph; den += dk * dk;
            }
            double slope = den > 0 ? num / den : 0;
            if (!std::isfinite(slope)) slope = 0;
            const double phc = ph0 - slope * (kbar - kc);   // the common phase at the centre carrier
            const cf32 r0((float)std::cos(phc), (float)std::sin(phc));
            for (int k = 0; k < K; k += 3) {
                const double a = slope * (k - kc);
                Hp[k] *= r0 * cf32((float)std::cos(a), (float)std::sin(a));
            }
            for (int k : cp) { err += std::norm(Y[k] - Hp[k] * pv(k)); pw += std::norm(Hp[k]); }
            const float s2 = (float)(err / cp.size() / 2.0);   // the prediction error holds the noise twice
            sigma2 = symsSinceSync < 8 ? s2 : 0.9f * sigma2 + 0.1f * s2;
            sigAvg = pw / cp.size();
            drift += slope;
            // keep the window inside the guard: a slope of 2 pi / N per carrier is one sample. The next FFT takes the moved window, so the
            // estimates turn with it after this symbol's pilots are in.
            if (std::fabs(drift) * N / kTwoPi >= 1.0) {
                shiftPending = drift > 0 ? 1 : -1;   // window late: move it earlier
                pos -= shiftPending;
                drift -= shiftPending * kTwoPi / N;
            }
        }
        for (int k = 3 * (l % 4); k < K; k += 12) Hp[k] = Y[k] / pv(k);
        for (int k : cp) Hp[k] = Y[k] / pv(k);
        symsSinceSync++;
        if (sigma2 > 0) st.snrDb = 10 * std::log10(std::max(1e-6, sigAvg / sigma2));
        if (!dataOn || symsSinceSync < 5) return;
        if (!dataStarted) { if (l != 0 || frameIdx != 0) return; dataStarted = true; }

        // equalise the data cells
        const int Nd = dataCarriers(mode);
        cells.resize(Nd); n0.resize(Nd); deint.resize(Nd); n0d.resize(Nd);
        int q = 0;
        for (int k = 0; k < K; k++) {
            if (roles[k] != 0) continue;
            const int k0 = k / 3 * 3;
            const float f = (float)(k - k0) / 3.f;
            const cf32 h = k0 + 3 < K ? Hp[k0] * (1.f - f) + Hp[k0 + 3] * f : Hp[k0];
            const float hh = std::max(1e-12f, std::norm(h));
            cells[q] = Y[k] * std::conj(h) / hh;
            n0[q] = 0.5f * sigma2 / hh;   // per axis (dvbt::demap)
            q++;
        }
        const auto& H = symbolPermutation(mode);
        if (l % 2) for (int i = 0; i < Nd; i++) { deint[H[i]] = cells[i]; n0d[H[i]] = n0[i]; }
        else for (int i = 0; i < Nd; i++) { deint[i] = cells[H[i]]; n0d[i] = n0[H[i]]; }
        const int vb = p.mod == k16Qam ? 4 : 2;
        llr.resize((size_t)Nd * vb);
        dvbt::demap(deint.data(), n0d.data(), Nd, p.mod, p.hier, llr.data());
        // bit de-multiplexing back to the stream
        const int v = streamBitsPerCell(p);
        stream.resize((size_t)Nd * v);
        for (int i = 0; i < Nd; i++) {
            const float* b = &llr[(size_t)i * vb];
            if (p.hier) { const int o = lowPriority ? 2 : 0; stream[2 * i] = b[o]; stream[2 * i + 1] = b[o + 1]; }
            else if (v == 4) { stream[4 * i] = b[0]; stream[4 * i + 1] = b[2]; stream[4 * i + 2] = b[1]; stream[4 * i + 3] = b[3]; }
            else { stream[2 * i] = b[0]; stream[2 * i + 1] = b[1]; }
        }
        // position in the SH frame
        const int fps = framesPerShFrame(p);
        const int frameNo = sf * 4 + frameIdx;
        const long long bitPos = (long long)(frameNo % std::max(1, fps)) * 68 * (Nd * v) + (long long)l * (Nd * v);
        if (bitPos % ((long long)kShFrameIu * kIuBits) == 0 && gIn % kShFrameIu != 0) { startData(); dataStarted = true; }
        if (gIn == 0 && bitPos % ((long long)kShFrameIu * kIuBits) != 0) return;   // wait for a SH frame start
        for (size_t i = 0; i < stream.size(); i++) {
            const float t = std::max(-127.f, std::min(127.f, std::round(stream[i] * kLlrScale)));
            iuAcc.push_back((int8_t)(t == 0 ? (stream[i] >= 0 ? 1 : -1) : t));
            if (iuAcc.size() == (size_t)kIuBits) { pushIu(); iuAcc.clear(); }
        }
        level = 3;
    }

    void pushIu() {
        int8_t out[kIuBits];
        const int branch = (int)(gIn % kBranches);
        ti.setBranch(branch);
        ti.process(iuAcc.data(), out);
        const bool valid = gIn >= 48LL * L[branch];
        const long long gOut = gIn - 48LL * ti.lmax();
        gIn++;
        const long long posOut = ((gOut % kShFrameIu) + kShFrameIu) % kShFrameIu;
        const int cwIu = codewordIu(pat), ncw = codewordsPerShFrame(pat);
        if (posOut >= (long long)ncw * cwIu) return;   // padding
        const int c = (int)(posOut / cwIu), off = (int)(posOut % cwIu);
        if (off == 0) { cwIdx = c; cwFill = 0; cwValid = true; }
        if (cwIdx != c || off != cwFill) { cwIdx = -1; return; }
        if (!valid) cwValid = false;
        for (int i = 0; i < kIuBits; i++) cwLlr[(size_t)off * kIuBits + i] = out[i] / kLlrScale;
        if (++cwFill == cwIu) {
            if (cwValid) decodeCodeword(c);
            cwIdx = -1;
        }
    }

    std::vector<float> coded;
    std::vector<uint8_t> info;
    void decodeCodeword(int /*c*/) {
        coded.resize(ntcb(pat));
        bitDeinterleave(cwLlr.data(), pat, coded.data());
        const int it = turboDecode(coded.data(), kTcInput, pat, 8, info, [](const uint8_t* b) { return eframeChecks(b); });
        uint8_t pk[188 * kPackets];
        const EframeInfo e = parseEframe(info.data(), pk);
        iterSum += it;
        if (e.headerOk && e.packetsOk == kPackets) st.codewordsOk++;
        else st.codewordsBad++;
        st.packetsOk += e.packetsOk;
        st.packetsBad += kPackets - e.packetsOk;
        const long n = st.codewordsOk + st.codewordsBad;
        st.avgIterations = n ? (double)iterSum / n : 0;
        st.level = 3;
        if (cb) cb(pk, kPackets);
    }

    void feed(const cf32* x, size_t n) {
        buf.insert(buf.end(), x, x + n);
        for (;;) {
            if (level == 0) { if (!search()) break; continue; }
            if (pos < base) pos = base;
            if (pos + N > base + (long long)buf.size()) break;
            const int lvl = level;
            processSymbol();
            if (level == 0 && lvl != 0) continue;
            // drop what is behind the window
            const long long keep = pos - base - 2 * S;
            if (keep > (long long)S * 8) { buf.erase(buf.begin(), buf.begin() + (long)keep); base += keep; }
        }
        st.level = std::max(st.level, level);
        if (level < st.level && level < 3) st.level = level;
    }
};

Receiver::Receiver() : d_(std::make_unique<Impl>()) {}
Receiver::~Receiver() = default;
void Receiver::configure(double fs) { d_->fs = fs; d_->resetAll(); }
void Receiver::reset() { d_->resetAll(); }
void Receiver::setLowPriority(bool lp) { if (lp != d_->lowPriority) { d_->lowPriority = lp; d_->dataOn = false; if (d_->paramsKnown) d_->startData(); } }
void Receiver::setMaxInterleaverBytes(size_t b) { d_->maxTiBytes = b; }
void Receiver::setPacketCallback(std::function<void(const uint8_t*, size_t)> cb) { d_->cb = std::move(cb); }
void Receiver::feed(const cf32* x, size_t n) { d_->feed(x, n); }
RxStatus Receiver::status() const { return d_->st; }

} // namespace dvbsh
} // namespace dect2
