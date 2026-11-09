// BGAN forward-bearer demodulator (bgan_demod.h). ETSI TS 102 744-2-1 frame structure; the method: comb correlation of the pilots finds timing and the
// fine frequency, the unique word gives the frame start and subtype, then every frame is demodulated with a pilot-based channel estimate.
#include "dect2/bgan/bgan_demod.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::bgan {

namespace {

const double kPi = 3.141592653589793;

// standard root raised cosine, integral of the square over a symbol = 1
double rrc(double t, double alpha) {   // t in symbols
    const double eps = 1e-6;
    if (std::fabs(t) < eps) return 1.0 - alpha + 4.0 * alpha / kPi;
    if (std::fabs(std::fabs(t) - 1.0 / (4.0 * alpha)) < eps) {
        const double a = kPi / (4.0 * alpha);
        return alpha / std::sqrt(2.0) * ((1.0 + 2.0 / kPi) * std::sin(a) + (1.0 - 2.0 / kPi) * std::cos(a));
    }
    const double n = std::sin(kPi * t * (1.0 - alpha)) + 4.0 * alpha * t * std::cos(kPi * t * (1.0 + alpha));
    const double d = kPi * t * (1.0 - 16.0 * alpha * alpha * t * t);
    return n / d;
}

inline cf32 cube(const cf32* p, float f) {       // Catmull-Rom on p[-1..2], f in [0,1)
    const cf32 c0 = p[0];
    const cf32 c1 = 0.5f * (p[1] - p[-1]);
    const cf32 c2 = p[-1] - 2.5f * p[0] + 2.f * p[1] - 0.5f * p[2];
    const cf32 c3 = 0.5f * (p[2] - p[-1]) + 1.5f * (p[0] - p[1]);
    return c0 + f * (c1 + f * (c2 + f * c3));
}

void fftRadix2(std::vector<cf32>& a) {
    const int n = int(a.size());
    for (int i = 1, j = 0; i < n; i++) { int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) std::swap(a[i], a[j]); }
    for (int len = 2; len <= n; len <<= 1) {
        const double w = -2 * kPi / len;
        const cf32 wn(float(std::cos(w)), float(std::sin(w)));
        for (int i = 0; i < n; i += len) {
            cf32 c(1, 0);
            for (int j = 0; j < len / 2; j++) {
                const cf32 u = a[i + j], v = a[i + j + len / 2] * c;
                a[i + j] = u + v; a[i + j + len / 2] = u - v;
                c *= wn;
            }
        }
    }
}

// a tentative lock from the search
struct Acq { int bearerDs = 0; double metric = 0; double startSym = 0; double fCycSym = 0; };

} // namespace

struct BearerDemod::Impl {
    double fs = 0;
    int symRate = 0;
    double sps = 0, spsNom = 0, spsEst = 0;
    double alpha = 0.25;
    std::vector<float> mf;                      // matched filter taps
    std::vector<cf32> hist;                     // filter history
    std::vector<cf32> y;                        // matched filtered samples
    int64_t yBase = 0;                          // absolute index of y[0]
    int64_t yEnd() const { return yBase + int64_t(y.size()); }
    DemodStatus st;
    std::vector<std::string> lines;
    bool decodeAll = true;
    int force = -1;
    std::vector<int> cand;                      // candidate bearers
    std::vector<uint64_t> uwSigns[1];           // unused
    // tracking
    bool locked = false;
    double frameStart = 0;                      // absolute sample index (of the matched filter output) of the first UW symbol
    double fCyc = 0;                            // carrier offset in cycles per symbol
    int bearerIdx = -1;                         // the bearer that is being demodulated
    int decided = -1;                           // bearer confirmed by decoding
    int misses = 0;
    int okRun = 0;
    int cyclesSinceOk = 0;
    int triesThisFrame = 0;
    double lastBbSec = -1e18;
    double stateTime = 0;
    std::vector<cf32> frameSyms, psbuf;
    TurboDecoder dec;
    std::vector<float> llr;
    float okEma = 0, occEma = -1;
    int lastSubtype = 8;
    FrameLayout layout;
    int layoutFor = -1;
    int nextRate[16], nextMod[16];
    bool nextValid = false;

    void clearNext() { nextValid = false; for (int i = 0; i < 16; i++) { nextRate[i] = -1; nextMod[i] = 0; } }

    bool build() {
        if (fs < 2.2 * symRate) return false;
        spsNom = sps = spsEst = fs / symRate;
        alpha = 0;
        cand.clear();
        for (int i = 0; i < bearerCount(); i++) if (bearer(i).symRate == symRate) { cand.push_back(i); alpha = bearer(i).rolloff; }
        const int span = 10;
        const int H = int(std::ceil(span * sps / 2));
        const int L = 2 * H + 1;
        mf.resize(L);
        double norm = 0;
        for (int i = 0; i < L; i++) { const double v = rrc((i - H) / sps, alpha); mf[i] = float(v); norm += v * v; }
        for (auto& v : mf) v /= float(std::sqrt(norm));    // unit energy: the symbol amplitude after the filter is sqrt(sps)
        hist.assign(L - 1, cf32(0, 0));
        return true;
    }
    void hardReset() {
        y.clear(); yBase = 0; hist.assign(mf.size() ? mf.size() - 1 : 0, cf32(0, 0));
        locked = false; decided = -1; misses = 0; okRun = 0; frameStart = 0; fCyc = 0; bearerIdx = -1; okEma = 0; occEma = -1; clearNext();
        st = DemodStatus{};
        layoutFor = -1;
        lines.clear();
    }

    inline cf32 at(double t) const {
        const double r = t - double(yBase);
        const int64_t i = int64_t(std::floor(r));
        if (i < 1 || i + 2 >= int64_t(y.size())) return cf32(0, 0);
        return cube(&y[i], float(r - i));
    }

    void ensureLayout(int b) { if (layoutFor != b) { layout = frameLayout(bearer(b)); layoutFor = b; } }

    // ---------- search ----------
    bool search() {
        // two candidate windows half a frame apart: one of them has at least 3/4 of its pilots in the same frame
        int frameSym = bearer(cand[0]).mos;
        for (int c : cand) frameSym = std::max(frameSym, bearer(c).mos);
        const int need = int(std::ceil(2.2 * frameSym * sps));
        if (int64_t(y.size()) < need) return false;
        double bestMet = 0, bestF = 0, bestStart = 0;
        int bestDs = -1, bestPs = 0;
        // distinct (ds, ps) pairs of the candidates
        struct DP { int ds, ps; };
        std::vector<DP> dps;
        for (int c : cand) {
            const Bearer& b = bearer(c);
            bool have = false;
            for (auto& d : dps) if (d.ds == b.ds && d.ps == b.ps) have = true;
            if (!have) dps.push_back({ b.ds, b.ps });
        }
        const int os = 4;
        const double tt = sps / os;
        const int z0 = 0;
        int zn = int((2.2 * frameSym + 8) * os);
        std::vector<cf32> z(zn);
        for (int i = 0; i < zn; i++) z[i] = at(yBase + 8.0 + tt * i);
        double power = 0;
        for (auto& v : z) power += std::norm(v);
        power /= zn;
        (void)z0;
        for (int w = 0; w < 2; w++) {
            const int wStart = w == 0 ? 0 : (frameSym / 2) * os;
            for (const DP& dp : dps) {
                const int L = dp.ds + 1;
                const int K = dp.ps;
                int N = 256; while (N < 2 * K) N *= 2;
                std::vector<cf32> buf(N);
                for (int tau = 0; tau < L * os; tau++) {
                    double sum = 0;
                    std::fill(buf.begin(), buf.end(), cf32(0, 0));
                    int count = 0;
                    for (int k = 0; k < K; k++) {
                        const int idx = wStart + tau + k * L * os;
                        if (idx >= zn) break;
                        buf[k] = z[idx]; count++;
                    }
                    if (count < K * 3 / 4) continue;
                    fftRadix2(buf);
                    float peak = 0; int peakBin = 0;
                    for (int i = 0; i < N; i++) { const float m = std::norm(buf[i]); sum += m; if (m > peak) { peak = m; peakBin = i; } }
                    const double met = peak / (sum / N + 1e-20);
                    if (met > bestMet) {
                        bestMet = met; bestDs = dp.ds; bestPs = dp.ps;
                        const int fb = peakBin > N / 2 ? peakBin - N : peakBin;
                        bestF = double(fb) / N / L;           // cycles per symbol, mod 1/L
                        bestStart = (wStart + tau) / double(os);
                    }
                }
            }
        }
        if (bestMet < 14.0 || bestDs < 0) { drop(int64_t(frameSym * sps)); return true; }
        // the frame start: pilot #0 at start + uw + ds symbols, so start = bestStart - (40 + ds) modulo L; try one frame of positions, resolving the alias with the unique word
        const int L = bestDs + 1;
        const uint64_t* uw = uniqueWords();
        double bestUw = 0, bestS = 0, bestFa = 0;
        int bestW = -1;
        const int aliases = 4;
        for (int n = 0; n < frameSym / L + 2; n++) {
            const double s = bestStart + n * L - (40 + bestDs);
            if (s < 2) continue;
            const double s4 = s * os;
            if (s4 + 40 * os + 2 >= zn) continue;
            for (int m = -aliases; m <= aliases; m++) {
                const double fa = bestF + double(m) / L;
                // cumulative correlator for a list of 15 words with fast sign changes: evaluate per word
                for (int wi = 0; wi < kSubtypes; wi++) {
                    double re = 0, im = 0;
                    const uint64_t word = uw[wi];
                    for (int k = 0; k < 40; k++) {
                        const int idx = int(std::lround(s4)) + k * os;
                        const cf32 v = z[idx];
                        const double ph = -2 * kPi * fa * k;
                        const double cr = std::cos(ph), ci = std::sin(ph);
                        const double sgn = (word >> (39 - k) & 1) ? 1 : -1;      // sign fixed below for the pilot polarity
                        re += sgn * (v.real() * cr - v.imag() * ci);
                        im += sgn * (v.real() * ci + v.imag() * cr);
                    }
                    const double met = (re * re + im * im);
                    if (met > bestUw) { bestUw = met; bestS = s; bestFa = fa; bestW = wi; }
                }
            }
        }
        const double normUw = bestUw / (40 * power + 1e-20);
        if (bestW < 0 || normUw < 16.0) { drop(int64_t(frameSym * sps * 0.5)); return true; }
        // pick the first bearer candidate with this pilot structure
        int chosen = -1;
        for (int c : cand) if (bearer(c).ds == bestDs && bearer(c).ps == bestPs) { chosen = c; break; }
        if (chosen < 0) { drop(int64_t(frameSym * sps * 0.5)); return true; }
        frameStart = double(yBase) + 8.0 + tt * bestS * os;
        fCyc = bestFa;
        spsEst = sps;
        bearerIdx = chosen;
        locked = true;
        misses = 0; okRun = 0;
        // drop what is before the frame
        drop(int64_t(std::max(0.0, frameStart - double(yBase) - 16)));
        return true;
    }

    void drop(int64_t samples) {
        if (samples <= 0) return;
        samples = std::min<int64_t>(samples, int64_t(y.size()) - 8);
        if (samples <= 0) return;
        y.erase(y.begin(), y.begin() + samples);
        yBase += samples;
    }

    // ---------- one frame ----------
    bool processFrame() {
        ensureLayout(bearerIdx);
        const Bearer& B0 = bearer(bearerIdx);
        const int mos = B0.mos;
        if (frameStart + spsEst * (mos + 12) + 4 > double(yEnd())) return false;   // not enough samples yet
        // raw symbols of this frame, plus timing early/late at the pilots
        frameSyms.resize(mos);
        for (int i = 0; i < mos; i++) frameSyms[i] = at(frameStart + spsEst * i);
        double pwr = 0;
        for (auto& v : frameSyms) pwr += std::norm(v);
        pwr /= mos;
        const std::vector<int>& pp = layout.pilotPos;
        const int L = B0.ds + 1;
        // fine frequency: pairwise phase rotation of the pilots
        cf32 acc(0, 0), pC(0, 0);
        int K = int(pp.size());
        std::vector<cf32> pil(K);
        for (int i = 0; i < K; i++) {
            const double ph = -2 * kPi * fCyc * pp[i];
            pil[i] = frameSyms[pp[i]] * cf32(float(std::cos(ph)), float(std::sin(ph)));
        }
        for (int i = 1; i < K; i++) acc += pil[i] * std::conj(pil[i - 1]);
        const double df = std::arg(acc) / (2 * kPi * L);
        fCyc += df * 0.8;
        for (int i = 0; i < K; i++) {
            const double ph = -2 * kPi * fCyc * pp[i];
            pil[i] = frameSyms[pp[i]] * cf32(float(std::cos(ph)), float(std::sin(ph)));
            pC += pil[i];
        }
        // the unique word: which word, using the channel phase of the first pilots
        const double thr = std::arg(pC);
        const double th0 = thr;                     // pilots are at +(1+j)axis direction, so rotation reference = phase of pilots minus 45 degrees
        const uint64_t* uw = uniqueWords();
        double bestC = -1e18; int bestWord = -1; bool bestZp = B0.uwZeroIsPlus;
        for (int zpi = 0; zpi < 2; zpi++) {
            const bool zp = zpi == 1;
            // both polarities exist within a pilot structure (the 33.6 kBd bearers): only those that a candidate uses
            bool possible = false;
            for (int c : cand) if (bearer(c).ds == B0.ds && bearer(c).ps == B0.ps && bearer(c).uwZeroIsPlus == zp) possible = true;
            if (!possible) continue;
            for (int w = 0; w < kSubtypes; w++) {
                double c = 0;
                for (int k = 0; k < 40; k++) {
                    const double ph = -2 * kPi * fCyc * k - th0;
                    const cf32 v = frameSyms[k] * cf32(float(std::cos(ph)), float(std::sin(ph)));
                    const int bit = (uw[w] >> (39 - k)) & 1;
                    const double s = (bit ^ (zp ? 1 : 0)) ? 1 : -1;
                    c += s * v.real();
                }
                if (c > bestC) { bestC = c; bestWord = w; bestZp = zp; }
            }
        }
        if (decided >= 0) bestZp = bearer(decided).uwZeroIsPlus;
        if (bestZp != B0.uwZeroIsPlus) { for (int c : cand) if (bearer(c).ds == B0.ds && bearer(c).ps == B0.ps && bearer(c).uwZeroIsPlus == bestZp) { bearerIdx = c; break; } }
        const Bearer& Bp = bearer(bearerIdx);
        // signal amplitude from pilots; a real frame has a good UW score
        const double ampl = std::abs(pC) / K;
        const double pilotPower = ampl * ampl;
        const double uwScore = bestC / (40 * ampl + 1e-12);
        const bool gotUw = uwScore > 0.5 && bestWord >= 0;
        if (!gotUw) {
            misses++;
            st.framesLost++;
            if (misses > 4) { locked = false; st.state = 0; st.bearer = -1; decided = -1; }
            advance(mos, 0);
            return true;
        }
        misses = 0;
        // channel estimate: UW and pilots as references, smoothed
        struct Ref { int pos; cf32 h; };
        std::vector<Ref> refs;
        refs.reserve(K + 40);
        for (int k = 0; k < 40; k++) {
            const double ph = -2 * kPi * fCyc * k;
            const int bit = (uw[bestWord] >> (39 - k)) & 1;
            const float s = (bit ^ (Bp.uwZeroIsPlus ? 1 : 0)) ? 1.f : -1.f;
            refs.push_back({ k, frameSyms[k] * cf32(float(std::cos(ph)), float(std::sin(ph))) * s });
        }
        for (int i = 0; i < K; i++) refs.push_back({ pp[i], pil[i] });
        // smoothing window: wide enough to average noise, narrow enough for fading: about 1 ms worth, at least 3 pilots
        const int w = std::clamp(int(round(0.0008 * symRate / L)), 1, 8);
        const int n = int(refs.size());
        std::vector<cf32> smooth(n);
        for (int i = 0; i < n; i++) {
            cf32 s(0, 0); float tot = 0;
            for (int j = std::max(0, i - w); j <= std::min(n - 1, i + w); j++) { s += refs[j].h; tot += 1; }
            smooth[i] = s / tot;
        }
        // noise from the reference residuals
        double res = 0;
        for (int i = 0; i < n; i++) res += std::norm(refs[i].h - smooth[i]);
        res /= std::max(1, n);
        const double f1 = 1.0 / (2 * w + 1);
        double noiseVar = res / std::max(0.05, 1.0 - f1);    // the smoother removes part of the noise of each reference
        noiseVar = std::max(noiseVar, pilotPower * 1e-5);
        // estimates
        const double es = (pilotPower > noiseVar ? pilotPower : noiseVar * 1.01);
        const double pe = 2.0 * Bp.uwAxis * Bp.uwAxis;
        const double esn0 = 10 * std::log10(std::max(1e-6, (es - 0.0) / pe / noiseVar));
        st.esn0Db += 0.2 * (esn0 - st.esn0Db);
        st.cn0Dbhz = st.esn0Db + 10 * std::log10(double(symRate));
        st.cfoHz = fCyc * symRate;
        // data symbols, equalised: x = y / h * axis(1+j)
        const cf32 pref = cf32(Bp.uwAxis, Bp.uwAxis);
        const std::vector<int>& dp = layout.dataPos;
        std::vector<cf32> eq(dp.size());
        std::vector<float> nv(dp.size());
        int r = 0;
        for (size_t i = 0; i < dp.size(); i++) {
            const int pos = dp[i];
            while (r + 1 < n && refs[r + 1].pos < pos) r++;
            const double ph = -2 * kPi * fCyc * pos;
            const cf32 d = frameSyms[pos] * cf32(float(std::cos(ph)), float(std::sin(ph)));
            cf32 h;
            if (r + 1 < n && refs[r].pos <= pos && refs[r + 1].pos >= pos) {
                const float t = float(pos - refs[r].pos) / float(std::max(1, refs[r + 1].pos - refs[r].pos));
                h = smooth[r] * (1 - t) + smooth[r + 1] * t;
            } else h = smooth[std::min(r, n - 1)];
            eq[i] = d * (pref / (h + cf32(1e-9f, 0)));
            nv[i] = float(noiseVar * std::norm(pref / (h + cf32(1e-9f, 0))));
        }
        lastSubtype = bestWord;
        st.subtype = bestWord;
        double powData = 0;
        for (auto& v : eq) powData += std::norm(v);
        powData /= std::max<size_t>(1, eq.size());
        // try the candidates with this pilot structure until one decodes; once decided, only that one
        std::vector<int> tries;
        if (force >= 0) tries.push_back(force);
        else if (decided >= 0) tries.push_back(decided);
        else {
            // order the hypotheses: strong pilots relative to data (X16 style: pilot power 1.8) first when the data power says so
            for (int c : cand) if (bearer(c).ds == Bp.ds && bearer(c).ps == Bp.ps && bearer(c).family >= 0 && bearer(c).uwZeroIsPlus == Bp.uwZeroIsPlus) tries.push_back(c);
            const double rx = powData;           // pilot reference is scaled to `pref`; a bearer with pilots at power 1 gives data power 1, with outer-point pilots data power 1 too, after scale
            (void)rx;
        }
        bool decodedOk = false;
        int okFrom = -1;
        for (int c : tries) {
            const Bearer& B = bearer(c);
            if (B.family < 0) continue;
            CarrierSig trial = st.sig;
            std::vector<std::string> ln;
            int goodBlocks = 0, empties = 0, badBlocks = 0, attempted = 0;
            // the scaling of eq assumed bearer B0's pilot power; for a different axis, rescale
            const float scale = B.uwAxis / Bp.uwAxis;
            for (int blk = 0; blk < (decodeAll ? B.blocks : 1); blk++) {
                int sub = bestWord;
                bool m16 = false;
                if (B.outer && nextValid && blk < 16) { if (nextRate[blk] >= 0) sub = nextRate[blk]; if (B.dynamic && nextMod[blk]) m16 = true; }
                if (!B.outer && blk > 0 && trial.haveNextCode && blk < 16) { if (trial.nextCodeRate[blk] >= 0) sub = trial.nextCodeRate[blk]; }
                int fam = B.family;
                if (B.dynamic && m16) fam = B.family16;
                const Table* t = table(fam, sub);
                if (!t) { badBlocks++; continue; }
                const Mod m = t->bps == 2 ? Mod::Q4 : t->bps == 4 ? Mod::Q16 : t->bps == 5 ? Mod::Q32 : Mod::Q64;
                std::vector<float> llr(size_t(t->symbols) * t->bps);
                for (int i = 0; i < t->symbols; i++) {
                    // locate symbol i of block blk in the data stream
                    const int idx = B.outer ? i * B.blocks + blk : blk * B.teo + i;
                    if (idx >= int(eq.size())) break;
                    softDemap(m, eq[idx] * scale, nv[idx] * scale * scale, &llr[i * t->bps]);
                }
                std::vector<uint8_t> bytes;
                attempted++;
                dec.decode(*t, llr.data(), bytes, 8, [&](const std::vector<uint8_t>& o) { return blockLooksValid(o.data(), o.size()); });
                const int k = parseBlock(bytes.data(), bytes.size(), trial, &ln);
                if (k == 1) goodBlocks++;
                else if (k == 2) empties++;
                else badBlocks++;
            }
            const int total = goodBlocks + empties + badBlocks;
            const bool ok = goodBlocks > 0 && badBlocks * 2 <= total;
            if (ok || (decided == c && (goodBlocks + empties) > 0)) {
                decodedOk = true; okFrom = c;
                st.sig = trial;
                for (auto& l : ln) { lines.push_back(l); if (lines.size() > 200) lines.erase(lines.begin()); }
                st.blocksOk += goodBlocks; st.blocksBad += badBlocks; st.blocksEmpty += empties;
                if (st.sig.haveBb) st.seenBb = 0;
                if (total > 0) { const float occ = float(goodBlocks) / total; occEma = occEma < 0 ? occ : occEma + 0.1f * (occ - occEma); st.occupancy = occEma; }
                if (B.outer && trial.haveNextCode) { nextValid = true; for (int i = 0; i < 16; i++) { nextRate[i] = trial.nextCodeRate[i]; nextMod[i] = trial.nextCodeMod16[i]; } st.sig.haveNextCode = false; }
                break;
            } else if (decided == c) { st.blocksBad += badBlocks; }
            (void)attempted;
        }
        if (decodedOk) {
            okRun++;
            if (decided < 0 && okRun >= 2) decided = okFrom;
            bearerIdx = okFrom;
            cyclesSinceOk = 0;
        } else if (decided >= 0) {
            cyclesSinceOk++;
            if (cyclesSinceOk > 20) { decided = -1; okRun = 0; }
        } else { cyclesSinceOk++; }
        okEma += 0.1f * ((decodedOk ? 1.f : 0.f) - okEma);
        st.blockOkRate = okEma;
        st.frames++;
        st.bearer = bearerIdx;
        st.structureOnly = !decodedOk && decided < 0 && cyclesSinceOk > 3;
        st.state = decodedOk || decided >= 0 ? 2 : 1;
        // timing refinement: energy of the pilots +/- a quarter symbol
        double e[3] = { 0, 0, 0 };
        const double dts[3] = { -0.25, 0, 0.25 };
        for (int t = 0; t < 3; t++) {
            cf32 c(0, 0);
            for (int i = 0; i < K; i++) {
                const double ph = -2 * kPi * fCyc * pp[i];
                c += at(frameStart + spsEst * (pp[i] + dts[t])) * cf32(float(std::cos(ph)), float(std::sin(ph)));
            }
            e[t] = std::abs(c);
        }
        const double denom = e[0] - 2 * e[1] + e[2];
        double dt = 0;
        if (std::fabs(denom) > 1e-12) dt = 0.25 * 0.5 * (e[0] - e[2]) / denom;
        dt = std::clamp(dt, -0.25, 0.25);
        advance(mos, dt);
        return true;
    }

    void advance(int mos, double dtSym) {
        const double before = frameStart;
        frameStart += spsEst * mos + dtSym * spsEst * 0.6;
        // clock rate tracking
        if (dtSym != 0) { const double measured = (frameStart - before) / mos; spsEst += 0.12 * (measured - spsEst); }
        drop(int64_t(std::max(0.0, frameStart - double(yBase) - 16)));
    }

    void feed(const cf32* x, size_t n) {
        const size_t L = mf.size();
        for (size_t i = 0; i < n; i++) {
            // direct form
            hist.push_back(x[i]);
            cf32 v(0, 0);
            const size_t hs = hist.size();
            for (size_t k = 0; k < L; k++) v += hist[hs - 1 - k] * mf[k];
            y.push_back(v);
        }
        if (hist.size() > L * 2) hist.erase(hist.begin(), hist.end() - L);
        for (int guard = 0; guard < 8; guard++) {
            if (locked) {
                if (!processFrame()) break;
            } else {
                if (!search()) break;
                if (!locked) continue;
            }
        }
        // bound memory if something went wrong
        if (y.size() > size_t(20 * spsNom * 13440)) drop(int64_t(y.size()) - int64_t(4 * spsNom * 13440));
    }
};

BearerDemod::BearerDemod() : p_(std::make_unique<Impl>()) { p_->clearNext(); }
BearerDemod::~BearerDemod() = default;
bool BearerDemod::configure(double fs, int symRate) {
    p_->fs = fs; p_->symRate = symRate;
    if (!p_->build()) return false;
    p_->hardReset();
    return true;
}
void BearerDemod::reset() { p_->hardReset(); }
void BearerDemod::feed(const cf32* x, size_t n) { p_->feed(x, n); }
const DemodStatus& BearerDemod::status() const { return p_->st; }
void BearerDemod::takeLines(std::vector<std::string>& out) { out = std::move(p_->lines); p_->lines.clear(); }
void BearerDemod::setDecodeAll(bool on) { p_->decodeAll = on; }
void BearerDemod::setForceBearer(int idx) { p_->force = idx; if (idx >= 0) { p_->bearerIdx = idx; p_->decided = idx; } else p_->decided = -1; }

} // namespace dect2::bgan
