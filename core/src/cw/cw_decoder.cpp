// CW adaptive decoder (see cw_decoder.h).
#include "dect2/cw/cw_decoder.h"
#include "dect2/cw/cw_lm.h"
#include "dect2/cw/cw_morse.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dect2 {
namespace cw {

FistTune& fistTune() {
    static FistTune t;
    return t;
}

namespace {

constexpr int kBeam = 32;
constexpr int kCand = kBeam * 3 + 4;
constexpr int kPend = 28;
constexpr int kWord = 16;
constexpr int kOutQ = 512;
constexpr int kBack = 160;
constexpr int kRing = 16;
constexpr float kHistLo = -1.6f, kHistSpan = 4.4f;

// ---- the Morse table as a trie: node = 1, then 2n (dot) / 2n+1 (dash)
struct Tables {
    const char* nodeText[1024] = {};
    uint8_t nodeSym[1024] = {};          // token id
    bool alive[1024] = {};               // a code, or the start of one
    const char* tok[80] = {};
    uint8_t tokLm[80] = {};
    int nTok = 0;
    int space = 0, unknown = 0;
    int tokOf(const char* t) {
        for (int i = 0; i < nTok; i++) if (!std::strcmp(tok[i], t)) return i;
        tok[nTok] = t; tokLm[nTok] = (uint8_t)lmSymbol(t);
        return nTok++;
    }
    Tables() {
        for (int len = 1; len <= 9; len++)
            for (int bits = 0; bits < (1 << len); bits++) {
                std::string code;
                for (int b = len - 1; b >= 0; b--) code += ((bits >> b) & 1) ? '-' : '.';
                const char* t = decodeSymbol(code);
                if (!t) continue;
                const int node = (1 << len) | bits;
                nodeText[node] = t;
                nodeSym[node] = (uint8_t)tokOf(t);
            }
        space = tokOf(" ");
        unknown = tokOf("*");
        for (int n = 1023; n >= 1; n--) alive[n] = nodeText[n] || (n < 512 && (alive[2 * n] || alive[2 * n + 1]));
    }
    bool hasChild(int n) const { return n < 512 && (alive[2 * n] || alive[2 * n + 1]); }
};
const Tables& T() { static Tables t; return t; }

inline float lpGauss(float x, float mu, float sg) {   // log density: 99.5 % the class, 0.5 % a broad tail
    const float z = (x - mu) / sg;
    const FistTune& tu = fistTune();
    const float a = -0.5f * z * z - std::log(sg) + std::log1p(-tu.tailW);
    const float sb = sg * tu.tailMul + tu.tailBase, zb = (x - mu) / sb;
    const float b = -0.5f * zb * zb - std::log(sb) + std::log(tu.tailW);
    const float m = std::max(a, b);
    return m + std::log(std::exp(a - m) + std::exp(b - m));
}
inline float clipf(float v, float lim) { return v > lim ? lim : v < -lim ? -lim : v; }

struct Hyp {
    float score = 0;
    uint16_t node = 1;                   // the code of the character in progress (1: none; 0: not a Morse code)
    uint8_t nPend = 0, wLen = 0;
    uint8_t c1 = kLmSpace, c2 = kLmSpace;
    uint8_t zc = 0;                      // the worst z of the elements of the character in progress, x40
    uint8_t atWord = 1;                  // at the start of a word (after a word gap): a mark here may be noise in the pause
    uint8_t skip = 0;                    // the last mark was read as noise: the gap after it goes on with the pause
    uint8_t pend[kPend] = {};
    uint8_t pz[kPend] = {};
    uint8_t word[kWord] = {};
};

bool sameHyp(const Hyp& a, const Hyp& b) {
    return a.node == b.node && a.nPend == b.nPend && a.c1 == b.c1 && a.c2 == b.c2 && a.wLen == b.wLen && a.skip == b.skip &&
           !std::memcmp(a.pend, b.pend, a.nPend) && !std::memcmp(a.word, b.word, a.wLen);
}

struct Ev { bool on; float ms; };

} // namespace

struct FistDecoder::Impl {
    // ---- state
    bool locked = false;
    float manualDot = 0;
    // the model (ln ms): classes at L + offset
    float L = 0, ldr = 1.0986f, gi = 0, gc = 1.0986f, gw = 1.9459f;
    float sd = 0.2f, sD = 0.25f, si = 0.3f, sc = 0.3f, sw = 0.4f;
    int n = 0;                           // events learned from since the lock
    float fit = 1; int badRun = 0;
    float ring[kRing] = {}; int ringN = 0;
    // windows of recent durations relative to the dot length at the time: the classes are re-fitted from them (batch k-means)
    static constexpr int kWm = 64, kWg = 96;
    float wm[kWm] = {}, wg[kWg] = {}; int nwm = 0, nwg = 0, sinceM = 0, sinceG = 0;
    bool batchM = false, batchG = false;
    int sinceCheck = 0;
    float fitG = 1;                      // the same for the gaps
    bool confirmed = false;              // the signal has shown Morse timing: text is released
    int vis = 0;                         // output tokens before this index may be popped
    float hMarks[kFistBins] = {}, hGaps[kFistBins] = {};
    // before the lock
    Ev back[kBack]; int nBack = 0;
    // the merging queue
    Ev q[4]; int nq = 0;
    bool haveMark = false;               // a mark was seen (a space before the first one is nothing)
    bool idleFlushed = false;
    // the sender learned before the lock was lost (a long fade, a burst of noise): a new lock at about the same speed starts from it
    struct Prior { bool ok = false; float L, ldr, gi, gc, gw, sd, sD, si, sc, sw; } prior;
    // how often the detector opens on noise in a gap (a mark under a third of a dot between two gaps): the soft decisions' measure of
    // how weak the signal is. Not the split marks: after a doubling of the speed the new gaps look like drop-outs to the old model
    float rSpike = 0.01f;
    bool lastSpace = true;
    // the beam
    Hyp hyp[kBeam]; int nh = 1;
    Hyp cand[kCand];
    // committed text
    DecTok out[kOutQ]; int oh = 0, ot = 0;

    Impl() { reset(); }

    void reset() {
        locked = false; L = 0; ldr = 1.0986f; gi = 0; gc = 1.0986f; gw = 1.9459f;
        sd = 0.2f; sD = 0.25f; si = 0.3f; sc = 0.3f; sw = 0.4f; n = 0; fit = 1; badRun = 0; ringN = 0;
        nwm = nwg = sinceM = sinceG = 0; batchM = batchG = false;
        std::memset(hMarks, 0, sizeof hMarks); std::memset(hGaps, 0, sizeof hGaps);
        nBack = 0; nq = 0; haveMark = false; idleFlushed = false; collecting = false; nPost = 0; prior = Prior(); lastSpace = true; rSpike = 0.01f;
        nh = 1; hyp[0] = Hyp(); oh = ot = vis = 0; fitG = 1; confirmed = false;
    }

    float dot() const { return std::exp(L); }

    // ---------------------------------------------------------------- output
    void push(const char* text, int conf, const char* alt) {
        DecTok t;
        std::strncpy(t.text, text, 7);
        t.conf = (uint8_t)std::max(0, std::min(100, conf));
        if (alt) std::strncpy(t.alt, alt, 7);
        if (t.text[0] == ' ') { if (lastSpace) return; lastSpace = true; } else lastSpace = false;
        const int nt = (ot + 1) % kOutQ;
        if (nt == oh) oh = (oh + 1) % kOutQ;   // full: the oldest goes
        out[ot] = t; ot = nt;
        if (confirmed) vis = ot;
    }

    // text held back until the timing has proven to be Morse; dropped when the lock is given up
    void confirmCheck() {
        if (confirmed || n < 14 || fit > 4.f || fitG > 4.f) return;
        confirmed = true; vis = ot;
    }
    void dropLock() {
        if (std::getenv("CW_TRACE")) fprintf(stderr, "DROPLOCK fit %.1f fitG %.1f\n", fit, fitG);
        if (confirmed) { prior = {true, L, ldr, gi, gc, gw, sd, sD, si, sc, sw}; }
        ot = vis;            // the held text was noise
        locked = false; confirmed = false; nBack = 0; nq = 0; haveMark = false; idleFlushed = false; collecting = false; nPost = 0;
        nh = 1; hyp[0] = Hyp(); n = 0; fit = 1; fitG = 1; badRun = 0; ringN = 0; nwm = nwg = 0; batchM = batchG = false;
    }

    // ---------------------------------------------------------------- beam
    void select(int nc) {
        std::sort(cand, cand + nc, [](const Hyp& a, const Hyp& b) { return a.score > b.score; });
        const float floor = cand[0].score - 22.f;
        nh = 0;
        for (int i = 0; i < nc && nh < kBeam; i++) {
            if (cand[i].score < floor) break;
            bool dup = false;
            for (int j = 0; j < nh; j++) if (sameHyp(hyp[j], cand[i])) { dup = true; break; }
            if (!dup) hyp[nh++] = cand[i];
        }
    }

    void emitTok(Hyp& h, int sym, float lmw) {
        if (h.nPend >= kPend) return;
        const Tables& t = T();
        const int lm = t.tokLm[sym];
        h.score += lmw * lmLogP(h.c2, h.c1, lm);
        h.c2 = h.c1; h.c1 = (uint8_t)lm;
        h.pend[h.nPend] = (uint8_t)sym; h.pz[h.nPend] = h.zc; h.nPend++;
        if (sym != t.space) { if (h.wLen < kWord) h.word[h.wLen++] = (uint8_t)lm; }
    }

    void emitChar(Hyp& h) {
        const Tables& t = T();
        const FistTune& tu = fistTune();
        if (h.node >= 2 && t.nodeText[h.node]) emitTok(h, t.nodeSym[h.node], tu.lmWeight);
        else { h.score -= 6.f; emitTok(h, t.unknown, tu.lmWeight); }
        h.node = 1; h.zc = 0;
    }
    void emitWordEnd(Hyp& h) {
        const FistTune& tu = fistTune();
        h.score += tu.wordWeight * lmWordBonus(h.word, h.wLen);
        emitTok(h, T().space, tu.lmWeight);
        h.wLen = 0;
        h.atWord = 1;
    }

    void extendMark(float x) {
        const float ld = lpGauss(x, L, sd) - 0.60f, lD = lpGauss(x, L + ldr, sD) - 0.80f;
        const float zd = std::fabs(x - L) / sd, zD = std::fabs(x - L - ldr) / sD;
        const float best = std::max(ld, lD);
        // in the pause before a word the detector may open on noise: that reading is kept beside the others, and the character model
        // decides ("CQ" after a noise blip, not ";Q" or "<AR>Q"). Its chance is the learned rate of noise in the gaps.
        const float lNoise = x < L + 0.4f ? std::log(std::max(0.003f, std::min(0.3f, rSpike))) + lpGauss(x, L - 0.7f, 0.5f) : -1e9f;
        int nc = 0;
        for (int i = 0; i < nh; i++) {
            if (hyp[i].atWord && hyp[i].node == 1 && lNoise > best - 9.f) {
                Hyp c = hyp[i];
                c.score += lNoise; c.skip = 1;
                cand[nc++] = c;
            }
            for (int dash = 0; dash < 2; dash++) {
                const float ll = dash ? lD : ld;
                if (ll < best - 9.f) continue;
                Hyp c = hyp[i];
                c.score += ll;
                const float z = dash ? zD : zd;
                c.zc = (uint8_t)std::max<int>(c.zc, (int)std::min(255.f, z * 40.f));
                if (c.node != 0) {
                    const int nn = c.node * 2 + dash;
                    if (c.node >= 512 || !T().alive[nn]) { c.node = 0; c.score -= 7.f; } else c.node = (uint16_t)nn;
                }
                c.atWord = 0; c.skip = 0;
                cand[nc++] = c;
            }
        }
        select(nc);
    }

    // wordOnly: the idle flush (a gap longer than a word threshold, still going)
    void extendSpace(float x, bool wordOnly) {
        const float li = lpGauss(x, L + gi, si) - 0.60f;
        const float lc = lpGauss(x, L + gc, sc) - 1.05f;
        const float lw = lpGauss(std::min(x, L + gw), L + gw, sw) - 2.30f;
        const Tables& t = T();
        const float zi = std::fabs(x - L - gi) / si, zc = std::fabs(x - L - gc) / sc, zw = std::max(0.f, (L + gw - x)) / sw;
        int nc = 0;
        for (int i = 0; i < nh; i++) {
            const Hyp& h = hyp[i];
            if (h.skip) {   // the gap after a noise mark: still the pause (the word gap was scored before the mark)
                Hyp c = h; c.skip = 0;
                cand[nc++] = c;
                continue;
            }
            if (!wordOnly && (h.node == 0 || t.hasChild(h.node))) {
                Hyp c = h; c.score += li;
                c.zc = (uint8_t)std::max<int>(c.zc, (int)std::min(255.f, zi * 40.f));
                cand[nc++] = c;
            }
            if (!wordOnly) {
                Hyp c = h; c.zc = (uint8_t)std::max<int>(c.zc, (int)std::min(255.f, zc * 40.f));
                c.score += lc; emitChar(c); cand[nc++] = c;
            }
            if (wordOnly && h.node == 1) {   // nothing in progress (the word was ended by its gap already): the pause goes on
                cand[nc++] = h;
                continue;
            }
            {
                Hyp c = h; c.zc = (uint8_t)std::max<int>(c.zc, (int)std::min(255.f, zw * 40.f));
                c.score += lw; emitChar(c); emitWordEnd(c); cand[nc++] = c;
            }
        }
        select(nc);
    }

    // commit the first k tokens of the best reading, with a confidence and the best other reading
    void commit(int k) {
        if (k <= 0) return;
        const Tables& t = T();
        const Hyp best = hyp[0];
        k = std::min<int>(k, best.nPend);
        float tot = 0;
        float w[kBeam];
        for (int i = 0; i < nh; i++) { w[i] = std::exp(hyp[i].score - best.score); tot += w[i]; }
        for (int j = 0; j < k; j++) {
            float same = 0, altW = 0; int altSym = -1;
            for (int i = 0; i < nh; i++) {
                if (hyp[i].nPend > j && hyp[i].pend[j] == best.pend[j]) same += w[i];
                else if (hyp[i].nPend > j && w[i] > altW) { altW = w[i]; altSym = hyp[i].pend[j]; }
            }
            float conf = tot > 0 ? same / tot : 1.f;
            const int z = best.pz[j];
            if (z > 160) conf *= 0.5f; else if (z > 100) conf *= 0.75f;
            if (std::getenv("CW_TRACE2")) fprintf(stderr, "  => '%s' conf %.2f\n", t.tok[best.pend[j]], conf);
            push(t.tok[best.pend[j]], (int)std::lround(conf * 100), (altSym >= 0 && altW >= 0.03f * tot && altSym != best.pend[j]) ? t.tok[altSym] : nullptr);
        }
        int keep = 0;
        for (int i = 0; i < nh; i++) {
            Hyp& h = hyp[i];
            if (h.nPend < k || std::memcmp(h.pend, best.pend, (size_t)k)) continue;
            Hyp c = h;
            c.nPend = (uint8_t)(h.nPend - k);
            std::memmove(c.pend, h.pend + k, c.nPend); std::memmove(c.pz, h.pz + k, c.nPend);
            hyp[keep++] = c;
        }
        nh = std::max(1, keep);
    }

    void dump(const char* tag) {
        if (!std::getenv("CW_TRACE3")) return;
        const Tables& t = T();
        fprintf(stderr, "%s:", tag);
        for (int i = 0; i < std::min(nh, 4); i++) {
            fprintf(stderr, " [%.1f ", hyp[i].score);
            for (int j = 0; j < hyp[i].nPend; j++) fprintf(stderr, "%s", t.tok[hyp[i].pend[j]]);
            fprintf(stderr, "|%s]", hyp[i].node >= 2 && t.nodeText[hyp[i].node] ? t.nodeText[hyp[i].node] : "?");
        }
        fprintf(stderr, "\n");
    }

    void commitAfterStep() {
        dump("beam");
        const Hyp& best = hyp[0];
        int agree = best.nPend;
        for (int i = 1; i < nh; i++) {
            if (hyp[i].score < best.score - 14.f) break;
            int j = 0;
            while (j < agree && j < hyp[i].nPend && hyp[i].pend[j] == best.pend[j]) j++;
            agree = std::min(agree, j);
        }
        const int depth = std::max(0, (int)best.nPend - fistTune().delay);
        commit(std::max(agree, depth));
    }

    // ---------------------------------------------------------------- learning
    static int binOf(float rel) { return std::max(0, std::min(kFistBins - 1, (int)((rel - kHistLo) / kHistSpan * kFistBins))); }

    void clampModel() {
        ldr = std::max(0.64f, std::min(1.65f, ldr));
        gi = std::max(-1.2f, std::min(0.7f, gi));
        gc = std::max(0.5f, std::min(2.7f, gc));          // up to 15 dots: Farnsworth spacing (20 / 10 WPM sends ~12)
        gw = std::max(gc + 0.25f, std::min(3.7f, gw));    // up to 40 dots
        sd = std::max(0.07f, std::min(0.5f, sd)); sD = std::max(0.07f, std::min(0.5f, sD));
        si = std::max(0.07f, std::min(0.5f, si)); sc = std::max(0.07f, std::min(0.6f, sc)); sw = std::max(0.1f, std::min(0.7f, sw));
        L = std::max(std::log(20.f), std::min(std::log(260.f), L));
    }

    float rate(float minRate) const { return std::max(minRate, 1.f / (float)(n + 3)); }

    void learnMark(float x) {
        const FistTune& tu = fistTune();
        for (int i = 0; i < kFistBins; i++) hMarks[i] *= 0.99f;
        hMarks[binOf(x - L)] += 1.f;
        const float zd = (x - L) / sd, zD = (x - L - ldr) / sD;
        const float pd = 0.55f * std::exp(-0.5f * zd * zd) / sd, pD = 0.45f * std::exp(-0.5f * zD * zD) / sD;
        const float tot = pd + pD + 1e-12f;
        const float rd = pd / tot, rD = pD / tot;
        const float z2 = std::min(zd * zd, zD * zD);
        ring[ringN % kRing] = x; ringN++;   // the speed check sees every mark (twice the speed: the dots are half a dot long)
        // on a noisy signal (the detector opens on noise in the gaps: rSpike) a mark under half a dot is that noise, not a sign of a
        // wrong model: it does not count against the fit (which drops the lock) and teaches nothing
        if (x < L - 0.7f && rSpike > 0.02f) { n++; return; }
        fit += 0.08f * (std::min(z2, 25.f) - fit);
        if (z2 > 9.f) badRun++; else badRun = 0;
        if (z2 > 16.f) { n++; return; }   // an outlier teaches nothing
        const float a = rate(tu.alphaC), aL = rate(tu.alphaL);
        const float dd = x - L, dD = x - L - ldr;
        if (manualDot <= 0) L += clipf(aL * (rd * dd + 0.35f * rD * dD), 0.25f);
        ldr += clipf(a * rD * dD, 0.2f);
        if (!batchM) {
            sd = std::sqrt(std::max(0.0025f, sd * sd + a * rd * (dd * dd - sd * sd)));
            sD = std::sqrt(std::max(0.0025f, sD * sD + a * rD * (dD * dD - sD * sD)));
        }
        n++;
        // the window of marks, relative to the (updated) dot length
        if (nwm >= kWm) { std::memmove(wm, wm + 1, sizeof(float) * (kWm - 1)); nwm = kWm - 1; }
        wm[nwm++] = x - L;
        if (++sinceM >= 6 && nwm >= 16) { sinceM = 0; batchMarks(); }
        clampModel();
    }

    // a Gaussian mixture of K classes: fixed share priors, an outlier floor (junk, pauses), started at quantiles of the window.
    // shared: one spread for all classes (the gaps: stops a class from collapsing on a few odd values)
    // init: the starting means (null: quantiles of the window by the priors)
    static bool gmm(const float* v, int m, int K, const float* prior, float sigFloor, bool shared, float* mu, float* sg, int* cnt,
                    const float* init = nullptr) {
        m = std::min(m, 128);
        float srt[128];
        std::memcpy(srt, v, sizeof(float) * (size_t)m);
        std::sort(srt, srt + m);
        float cum = 0;
        for (int k = 0; k < K; k++) { mu[k] = init ? init[k] : srt[std::min(m - 1, (int)((cum + 0.5f * prior[k]) * (float)m))]; sg[k] = 0.25f; cum += prior[k]; }
        const float outl = 0.02f / 4.f;
        for (int it = 0; it < 25; it++) {
            float nk[3] = {0, 0, 0}, sm[3] = {0, 0, 0};
            float pv[128][3];
            for (int i = 0; i < m; i++) {
                float p[3], tot = outl;
                for (int k = 0; k < K; k++) {
                    const float z = (v[i] - mu[k]) / sg[k];
                    p[k] = prior[k] * std::exp(-0.5f * z * z) / sg[k] / 2.5066f; tot += p[k];
                }
                for (int k = 0; k < K; k++) { const float r = p[k] / tot; pv[i][k] = r; nk[k] += r; sm[k] += r * v[i]; }
            }
            float pooledSq = 0, pooledN = 0;
            float sq[3] = {0, 0, 0};
            for (int k = 0; k < K; k++) {
                if (nk[k] < 1.5f) continue;
                mu[k] = sm[k] / nk[k];
            }
            for (int i = 0; i < m; i++) for (int k = 0; k < K; k++) { const float d = v[i] - mu[k]; sq[k] += pv[i][k] * d * d; }
            for (int k = 0; k < K; k++) { pooledSq += sq[k]; pooledN += nk[k]; }
            for (int k = 0; k < K; k++) {
                if (nk[k] < 1.5f) continue;
                const float var = shared ? pooledSq / std::max(1.f, pooledN) : sq[k] / nk[k];
                sg[k] = std::max(sigFloor, std::min(0.5f, std::sqrt(std::max(0.f, var))));
            }
            for (int k = 0; k < K; k++) cnt[k] = (int)std::lround(nk[k]);
        }
        for (int k = 1; k < K; k++) if (mu[k] < mu[k - 1] + 0.1f) return false;
        return true;
    }

    // dots and dashes from a set of marks (ln), anchored on the dashes. The dashes carry most of the key-down time, so a 2-means in
    // linear time (started at the 25th and 90th percentiles) finds them even when the detector opens on noise in the gaps of a weak
    // signal; a mixture of three classes (noise blips, dots, dashes) started from that dash length then gives the dot, the ratio and
    // the spreads. Without the blip class those blips pulled the dot length down to the noise (a 22 WPM signal at 0 dB was read at
    // 60 WPM, all "E" and "T").
    static bool dashFit(const float* xs, int m, float& dotL, float& ratio, float& sdd, float& sDD, int minCnt) {
        if (m < 8) return false;
        m = std::min(m, 128);
        float lin[128];
        for (int i = 0; i < m; i++) lin[i] = std::exp(xs[i]);
        std::sort(lin, lin + m);
        float c1 = lin[m / 4], c2 = lin[m * 9 / 10];
        int n1 = 0, n2 = 0;
        for (int it = 0; it < 12; it++) {
            float s1 = 0, s2 = 0; n1 = n2 = 0;
            const float mid = 0.5f * (c1 + c2);
            for (int i = 0; i < m; i++) { if (lin[i] < mid) { s1 += lin[i]; n1++; } else { s2 += lin[i]; n2++; } }
            if (n1) c1 = s1 / (float)n1;
            if (n2) c2 = s2 / (float)n2;
        }
        if (n2 < minCnt || c2 < 1.8f * c1) return false;
        const float d = std::log(c2);
        const float init[3] = {d - 2.3f, d - 1.1f, d};
        const float prior[3] = {0.12f, 0.50f, 0.38f};
        float mu[3], sg[3]; int cnt[3] = {0, 0, 0};
        if (!gmm(xs, m, 3, prior, 0.06f, false, mu, sg, cnt, init)) return false;
        if (cnt[1] < minCnt || cnt[2] < minCnt || mu[2] - mu[1] < 0.5f || mu[2] - mu[1] > 1.7f) return false;
        dotL = mu[1]; ratio = mu[2] - mu[1]; sdd = sg[1]; sDD = sg[2];
        return true;
    }

    void batchMarks() {
        float c1, r, s1, s2;
        if (!dashFit(wm, nwm, c1, r, s1, s2, 3)) return;
        const float mu[2] = {c1, c1 + r}, sg[2] = {s1, s2};
        if (manualDot <= 0) { L += c1; for (int i = 0; i < nwm; i++) wm[i] -= c1; }
        ldr = 0.5f * ldr + 0.5f * (mu[1] - c1);
        sd = 0.5f * sd + 0.5f * sg[0];
        sD = 0.5f * sD + 0.5f * sg[1];
        batchM = true;
    }

    // the three gap classes (inside a character, between characters, between words) from a window of gaps (ln, relative to the dot).
    // Started from the data rather than from fixed shares: the shortest quarter gives the gap inside a character, the median of the
    // longer ones the gap between characters, their top the word gap. Farnsworth spacing puts the character gaps at 8 - 12 dots and
    // the word gaps at 20 - 30, where share-based starting points landed inside the class of short gaps. A fit with more word gaps than character gaps is not Morse spacing (a run of split marks
    // made a false class) and is not taken.
    // minCnt: the fewest gaps a class needs (the backlog at the lock has only a few characters)
    bool fitGaps(const float* v, int m, float w, int minCnt = 4) {
        if (m < 4 * minCnt) return false;
        m = std::min(m, 128);
        float srt[128];
        std::memcpy(srt, v, sizeof(float) * (size_t)m);
        std::sort(srt, srt + m);
        float init[3];
        init[0] = srt[m / 4];
        float rest[128]; int nr = 0;
        for (int i = 0; i < m; i++) if (srt[i] > init[0] + 0.55f) rest[nr++] = srt[i];
        if (nr < minCnt) return false;
        init[1] = rest[nr / 2];                                      // most of the longer gaps end a character
        init[2] = std::max(init[1] + 0.6f, rest[std::min(nr - 1, (int)(0.92f * (float)nr))]);
        const float prior[3] = {0.58f, 0.34f, 0.08f};
        float mu[3], sg[3]; int cnt[3] = {0, 0, 0};
        const bool conv = gmm(v, m, 3, prior, 0.07f, true, mu, sg, cnt, init);
        if (std::getenv("CW_TRACE")) fprintf(stderr, "gapGMM n %d mu %.2f %.2f %.2f sg %.2f %.2f %.2f cnt %d %d %d\n", m, mu[0], mu[1], mu[2], sg[0], sg[1], sg[2], cnt[0], cnt[1], cnt[2]);
        if (!conv || cnt[0] < minCnt || cnt[1] < minCnt || mu[1] - mu[0] < 0.45f || cnt[1] + 1 < cnt[2]) return false;
        gi += w * (mu[0] - gi); gc += w * (mu[1] - gc);
        si += w * (sg[0] - si); sc += w * (sg[1] - sc);
        if (mu[2] - mu[1] > 0.35f && cnt[2] >= 2) { gw += w * (mu[2] - gw); sw += w * (sg[2] - sw); }
        else gw = std::max(gw, gc + 0.6f);
        return true;
    }

    void batchGaps() {
        if (fitGaps(wg, nwg, 0.5f)) batchG = true;
    }

    void learnGap(float x) {
        const FistTune& tu = fistTune();
        for (int i = 0; i < kFistBins; i++) hGaps[i] *= 0.99f;
        hGaps[binOf(x - L)] += 1.f;
        if (x - L > 3.4f) return;   // a pause between transmissions teaches nothing
        const float zi = (x - L - gi) / si, zc = (x - L - gc) / sc, zw = (x - L - gw) / sw;
        float pi_ = 0.55f * std::exp(-0.5f * zi * zi) / si, pc = 0.35f * std::exp(-0.5f * zc * zc) / sc, pw = 0.10f * std::exp(-0.5f * zw * zw) / sw;
        if (x - L > gw) pw = std::max(pw, 0.10f / sw);     // longer than the word gap: a word gap
        const float tot = pi_ + pc + pw + 1e-12f;
        const float ri = pi_ / tot, rc = pc / tot, rw = pw / tot;
        {
            const float zz = std::min(25.f, std::min(zi * zi, std::min(zc * zc, x - L > gw ? 0.f : zw * zw)));
            fitG += 0.08f * (zz - fitG);
        }
        const float a = rate(tu.alphaC);
        const float di = x - L - gi, dc = x - L - gc, dw = x - L - gw;
        if (!batchG) {
            gi += clipf(a * ri * di, 0.2f); gc += clipf(a * rc * dc, 0.2f); gw += clipf(a * rw * dw, 0.3f);
            si = std::sqrt(std::max(0.0025f, si * si + a * ri * (di * di - si * si)));
            sc = std::sqrt(std::max(0.0025f, sc * sc + a * rc * (dc * dc - sc * sc)));
            sw = std::sqrt(std::max(0.01f, sw * sw + a * rw * (dw * dw - sw * sw)));
        }
        if (nwg >= kWg) { std::memmove(wg, wg + 1, sizeof(float) * (kWg - 1)); nwg = kWg - 1; }
        wg[nwg++] = x - L;
        if (++sinceG >= 6 && nwg >= 24) { sinceG = 0; batchGaps(); }
        clampModel();
    }

    static void twoMeans(const float* v, int m, float& c1, float& c2, int& n1, int& n2, float& sig) {
        float lo = v[0], hi = v[0];
        for (int i = 1; i < m; i++) { lo = std::min(lo, v[i]); hi = std::max(hi, v[i]); }
        c1 = lo + 0.25f * (hi - lo); c2 = lo + 0.75f * (hi - lo);
        n1 = n2 = 0;
        for (int it = 0; it < 15; it++) {
            float s1 = 0, s2 = 0; n1 = n2 = 0;
            const float mid = 0.5f * (c1 + c2);
            for (int i = 0; i < m; i++) { if (v[i] < mid) { s1 += v[i]; n1++; } else { s2 += v[i]; n2++; } }
            if (n1) c1 = s1 / (float)n1;
            if (n2) c2 = s2 / (float)n2;
        }
        float ss = 0; const float mid = 0.5f * (c1 + c2);
        for (int i = 0; i < m; i++) { const float d = v[i] - (v[i] < mid ? c1 : c2); ss += d * d; }
        sig = std::sqrt(ss / (float)std::max(1, m - 2));
    }

    // a run of marks the model cannot explain: the speed changed. Rebuild the dot length from the last marks.
    void relock() {
        if (std::getenv("CW_TRACE")) fprintf(stderr, "RELOCK\n");
        const int m = std::min(ringN, kRing);
        if (m < 10) return;
        float v[kRing];
        for (int i = 0; i < m; i++) v[i] = ring[(ringN - m + i) % kRing];
        float c1, c2, sig; int n1, n2;
        twoMeans(v, m, c1, c2, n1, n2, sig);
        if (c2 - c1 >= 0.5f && sig <= 0.5f && n1 >= 2 && n2 >= 2) {
            if (manualDot <= 0) L = c1;
            ldr = std::max(0.64f, std::min(1.65f, c2 - c1));
            n = std::min(n, 3);
        } else {
            float mean = 0; for (int i = 0; i < m; i++) mean += v[i];
            mean /= (float)m;
            float ss = 0; for (int i = 0; i < m; i++) ss += (v[i] - mean) * (v[i] - mean);
            if (std::sqrt(ss / (float)m) > 0.35f) return;
            if (manualDot <= 0) L = std::fabs(mean - L - ldr) < std::fabs(mean - L) ? mean - ldr : mean;
            n = std::min(n, 3);
        }
        nwm = nwg = 0;      // the windows hold the old speed
        badRun = 0; fit = 1;
        clampModel();
    }

    // the first estimate of the classes from what was heard before the lock: marks in two clusters, the gaps in three
    void fromBacklog(const float* xs, int m, int from) {
        float c1, c2, sg; int n1, n2;
        float fd, fr, f1, f2;
        if (dashFit(xs, m, fd, fr, f1, f2, 2)) {
            if (manualDot <= 0) L = fd;
            ldr = std::max(0.64f, std::min(1.65f, fr));
            sd = std::max(0.1f, std::min(0.4f, f1));
            sD = std::max(0.1f, std::min(0.4f, f2));
        } else if (m >= 8) {
            twoMeans(xs, m, c1, c2, n1, n2, sg);
            if (c2 - c1 >= 0.52f && n1 >= 2 && n2 >= 2) {
                const float mid = 0.5f * (c1 + c2);
                float s1 = 0, s2 = 0;
                for (int i = 0; i < m; i++) { const float d = xs[i] - (xs[i] < mid ? c1 : c2); (xs[i] < mid ? s1 : s2) += d * d; }
                if (manualDot <= 0) L = c1;
                ldr = std::max(0.64f, std::min(1.65f, c2 - c1));
                sd = std::max(0.1f, std::min(0.4f, std::sqrt(s1 / (float)n1)));
                sD = std::max(0.1f, std::min(0.4f, std::sqrt(s2 / (float)n2)));
            }
        }
        {   // the gap classes too, so a first transmission with wide spacing (Farnsworth) is not read as one letter a word
            float g[kBack]; int ng = 0;
            for (int i = from; i < nBack; i++)
                if (!back[i].on && back[i].ms >= 10.f && i > 0 && back[i - 1].on && back[i - 1].ms >= 18.f) {
                    const float r = std::log(back[i].ms) - L;
                    if (r < 3.4f) g[ng++] = r;
                }
            fitGaps(g, ng, 1.0f, 2);
        }
        clampModel();
    }

    // ---------------------------------------------------------------- the lock
    // the shortest mark the lock takes: noise in the gaps of a weak signal opens the detector for a few ms at a time; those blips are much
    // shorter than the dashes, which carry most of the key-down time (0.15 of the length at the 70th percentile of the key-down time: half a dot)
    float blipFloor(int from) const {
        float v[kBack]; int m = 0; float tot = 0;
        for (int i = from; i < nBack; i++) if (back[i].on && back[i].ms >= 18.f && back[i].ms < 1200.f) { v[m++] = back[i].ms; tot += back[i].ms; }
        if (m < 8) return 18.f;
        std::sort(v, v + m);
        float acc = 0;
        for (int i = 0; i < m; i++) { acc += v[i]; if (acc >= 0.3f * tot) return std::max(18.f, 0.15f * v[i]); }
        return 18.f;
    }

    bool tryLock() {
        float xs[kBack]; int m = 0;
        const int from = std::max(0, nBack - 70);
        const float floorMs = blipFloor(from);
        for (int i = from; i < nBack; i++) if (back[i].on && back[i].ms >= floorMs && back[i].ms < 1200.f) xs[m++] = std::log(back[i].ms);
        if (m < 8) return false;
        float c1, c2, sig; int n1, n2;
        twoMeans(xs, m, c1, c2, n1, n2, sig);
        float dotL = 0, ratio = 0, s1, s2;
        if (dashFit(xs, m, dotL, ratio, s1, s2, 2) && std::fabs(dotL - c1) > 0.2f) {}   // blips in the marks: the dash-anchored fit
        else if (c2 - c1 >= 0.52f && n1 >= 2 && n2 >= 2 && sig <= 0.45f) { dotL = c1; ratio = c2 - c1; }
        else if (m >= 20) {
            float mean = 0; for (int i = 0; i < m; i++) mean += xs[i];
            mean /= (float)m;
            float ss = 0; for (int i = 0; i < m; i++) ss += (xs[i] - mean) * (xs[i] - mean);
            if (std::sqrt(ss / (float)m) > 0.30f) return false;
            float sp[kBack]; int ns = 0;
            for (int i = 0; i < nBack; i++) if (!back[i].on && back[i].ms >= 10.f && back[i].ms < 2.5f * std::exp(mean)) sp[ns++] = std::log(back[i].ms);
            if (ns < 6) return false;
            std::sort(sp, sp + ns);
            const float unit = sp[ns / 2];
            if (mean - unit < 0.45f) { dotL = mean; ratio = 1.0986f; }
            else if (mean - unit > 0.8f) { dotL = mean - 1.0986f; ratio = 1.0986f; }
            else return false;
        } else return false;
        if (manualDot > 0) dotL = std::log(manualDot);
        const float ms = std::exp(dotL);
        if (ms < 20.f || ms > 280.f) return false;
        {   // the gaps must look like Morse too: the shorter half of them is about one dot long
            float sp[kBack]; int ns = 0;
            for (int i = std::max(0, nBack - 70); i < nBack; i++) if (!back[i].on && back[i].ms >= 18.f && back[i].ms < 3000.f) sp[ns++] = std::log(back[i].ms);
            if (ns < 6) return false;
            std::sort(sp, sp + ns);
            float lowQ = 0; for (int i = 0; i < ns / 2; i++) lowQ += sp[i];
            lowQ /= (float)(ns / 2);
            if (lowQ < dotL - 1.0f || lowQ > dotL + 0.7f) return false;
            float ss = 0; for (int i = 0; i < ns / 2; i++) ss += (sp[i] - lowQ) * (sp[i] - lowQ);
            if (std::sqrt(ss / (float)(ns / 2)) > 0.5f) return false;
            // and three gaps between characters at least, so the gap classes can be fitted before the backlog is read: with the book's
            // classes Farnsworth character gaps (8 - 12 dots) read as word gaps ("C Q D E")
            int longGaps = 0;
            for (int i = 0; i < ns; i++) if (sp[i] > dotL + 0.7f) longGaps++;
            if (longGaps < 3) return false;
        }
        L = dotL; ldr = std::max(0.64f, std::min(1.65f, ratio));
        gi = 0; gc = 1.0986f; gw = 1.9459f;
        sd = 0.2f; sD = 0.25f; si = 0.3f; sc = 0.3f; sw = 0.4f; n = 0; fit = 1; badRun = 0; ringN = 0;
        nwm = nwg = sinceM = sinceG = 0; batchM = batchG = false; fitG = 1; confirmed = false;
        if (prior.ok && std::fabs(L - prior.L) < 0.25f) {   // the same sender again: its classes, refined by the backlog below
            ldr = prior.ldr; gi = prior.gi; gc = prior.gc; gw = prior.gw;
            sd = prior.sd; sD = prior.sD; si = prior.si; sc = prior.sc; sw = prior.sw;
            batchM = batchG = true;                       // known classes: only the window fits move them
        }
        fromBacklog(xs, m, from);
        locked = true;
        return true;
    }

    // ---------------------------------------------------------------- events
    void processMark(float ms) {
        const float x = std::log(std::max(1.f, ms));
        if (std::getenv("CW_TRACE2")) fprintf(stderr, "MARK %.0f dot %.0f zd %.1f zD %.1f ldr %.2f sd %.2f sD %.2f\n", ms, std::exp(L), (x - L) / sd, (x - L - ldr) / sD, ldr, sd, sD);
        extendMark(x);
        learnMark(x);
        haveMark = true;
        confirmCheck();
        if (n >= 40 && (fit > 8.f || (n >= 40 && fitG > 8.f))) { dropLock(); return; }
        if (badRun >= 6 && fit > 5.f) relock();
        else if (++sinceCheck >= 6 && ringN >= 16) { sinceCheck = 0; checkSpeed(); }
    }

    // is the whole recent run of marks better explained by another speed? (a change of speed by a factor the classes cannot follow:
    // slower dots look like dashes, and the model happily reads them so)
    void checkSpeed() {
        if (manualDot > 0) return;
        float v[kRing];
        for (int i = 0; i < kRing; i++) v[i] = ring[(ringN - kRing + i) % kRing];
        float c1, c2, sig; int n1, n2;
        twoMeans(v, kRing, c1, c2, n1, n2, sig);
        if (c2 - c1 < 0.5f || n1 < 3 || n2 < 3 || sig > 0.4f || std::fabs(c1 - L) < 0.25f) return;
        float cur = 0, nw = 0;
        const float nl = c1, nr = std::max(0.64f, std::min(1.65f, c2 - c1));
        for (int i = 0; i < kRing; i++) {
            const float a = (v[i] - L) / sd, b = (v[i] - L - ldr) / sD;
            cur += std::min(25.f, std::min(a * a, b * b));
            const float c = (v[i] - nl) / sd, d = (v[i] - nl - nr) / sD;
            nw += std::min(25.f, std::min(c * c, d * d));
        }
        if (nw > 0.5f * cur) return;
        if (std::getenv("CW_TRACE")) fprintf(stderr, "SPEED %.0f -> %.0f\n", std::exp(L), std::exp(nl));
        L = nl; ldr = nr; n = std::min(n, 3);
        nwm = nwg = 0; badRun = 0; fit = 1;
        clampModel();
    }
    void processSpace(float ms) {
        if (!haveMark) return;
        if (!locked) return;
        const float x = std::log(std::max(1.f, ms));
        if (std::getenv("CW_TRACE2")) fprintf(stderr, "SPACE %.0f  zi %.1f zc %.1f zw %.1f\n", ms, (x - L - gi) / si, (x - L - gc) / sc, (x - L - gw) / sw);
        extendSpace(x, false);
        learnGap(x);
        commitAfterStep();
    }
    void process(const Ev& e) { if (e.on) processMark(e.ms); else processSpace(e.ms); }

    // ---------------------------------------------------------------- soft mark / space decisions
    // A weak signal's detector splits marks (a fade inside a dash) and opens on noise in the gaps. The clear cases (shorter than a third
    // of a dot) are merged outright; on a noisy detector the rest are decided by the model: the three events read as they are (mark,
    // gap, mark or gap, mark, gap, each in its best class) against the one merged event plus the log chance of such a detector error.
    // That chance is learned from the clear noise spikes (how often they happen is how noisy the detector is).
    static constexpr float kNoisy = 0.015f;   // the rate of clear detector errors above which the soft decisions are made
    float lMark(float x) const { return std::max(lpGauss(x, L, sd) - 0.60f, lpGauss(x, L + ldr, sD) - 0.80f); }
    float lGap(float x, bool intraOnly) const {
        const float li = lpGauss(x, L + gi, si) - 0.60f;
        if (intraOnly) return li;
        const float lc = lpGauss(x, L + gc, sc) - 1.05f;
        const float lw = lpGauss(std::min(x, L + gw), L + gw, sw) - 2.30f;
        return std::max(li, std::max(lc, lw));
    }
    bool softMerge(bool dropOut) const {
        if (!locked || n < 10) return false;
        const float d = dot();
        const float a = std::log(std::max(1.f, q[0].ms)), b = std::log(std::max(1.f, q[1].ms)), c = std::log(std::max(1.f, q[2].ms));
        const float all = std::log(q[0].ms + q[1].ms + q[2].ms);
        // only on a noisy detector (a weak signal): on a clean one the same patterns are a change of speed, which must reach the model
        if (dropOut) {
            if (q[1].ms > 0.8f * d || rSpike < kNoisy) return false;
            const float pr = std::max(0.003f, std::min(0.3f, rSpike));
            return lMark(all) + std::log(pr) > lMark(a) + lGap(b, false) + lMark(c);
        }
        if (rSpike < kNoisy) return false;
        const float pr = std::max(0.003f, std::min(0.3f, rSpike));
        // a mark alone between a word gap and a character gap or longer would be a word of one element ("E", "T"): rare in traffic
        // (-4.6: about 1 %), common as noise in the pause of a weak signal
        if (std::max(a, c) >= L + gw - sw && std::min(a, c) >= L + gc - sc && b < L + ldr + 2.f * sD)
            return lGap(all, false) + std::log(pr) > lGap(a, false) + lMark(b) + lGap(c, false) - 4.6f;
        if (q[1].ms > 0.9f * d) return false;
        // a dot between two gaps inside a character is a dot (a weak one is short): noise is looked for only next to a character gap
        if (std::max(a, c) < L + gc - sc) return false;
        return lGap(all, false) + std::log(pr) > lGap(a, false) + lMark(b) + lGap(c, false);
    }

    void drain(bool flush) {
        for (;;) {
            if (flush ? nq == 0 : nq < 3) return;
            if (nq >= 3) {
                const float d = dot();
                bool merge = false;
                if (q[0].on && !q[1].on && q[2].on) {            // a drop-out inside a mark?
                    merge = q[1].ms < 0.30f * d || softMerge(true);
                } else if (!q[0].on && q[1].on && !q[2].on) {    // a noise spike inside a gap?
                    const bool sure = q[1].ms < 0.33f * d;
                    merge = sure || softMerge(false);
                    rSpike += 0.02f * ((sure ? 1.f : 0.f) - rSpike);
                }
                if (merge) {
                    q[0].ms += q[1].ms + q[2].ms;
                    for (int i = 1; i + 2 < nq; i++) q[i] = q[i + 2];
                    nq -= 2; continue;
                }
            }
            const Ev e0 = q[0];
            for (int i = 1; i < nq; i++) q[i - 1] = q[i];
            nq--;
            process(e0);
            if (!locked) { nq = 0; return; }
        }
    }

    void feed(bool on, float ms) {
        if (nq >= 4) { const Ev e0 = q[0]; for (int i = 1; i < nq; i++) q[i - 1] = q[i]; nq--; process(e0); if (!locked) { nq = 0; return; } }
        q[nq++] = {on, ms};
        drain(false);
    }

    void event(bool on, float ms) {
        if (ms <= 0) return;
        if (std::getenv("CW_TRACE")) fprintf(stderr, "%c %.0f  (dot %.0f)\n", on ? 'M' : 's', ms, locked ? std::exp(L) : 0.f);
        if (!on && idleFlushed) {   // the pause ends: what follows is held until the timing is checked (afterPause)
            idleFlushed = false;
            if (locked && haveMark) {
                learnGap(std::log(std::max(1.f, ms)));
                if (ms > 2.f * std::exp(L + gw)) { nPost = 0; collecting = true; }   // a pause, not a word gap
            }
            return;
        }
        if (collecting && locked) {
            post[nPost++] = {on, ms};
            int marks = 0;
            for (int i = 0; i < nPost; i++) marks += post[i].on;
            if (marks >= kPostMarks || nPost >= kPost || (!on && marks >= 4 && ms > std::exp(L + 0.5f * (gc + gw)))) afterPause();
            return;
        }
        if (!locked) {
            if (nBack >= kBack) { std::memmove(back, back + 40, sizeof(Ev) * (size_t)(kBack - 40)); nBack -= 40; }
            back[nBack++] = {on, ms};
            if (tryLock()) {
                const int m = nBack; nBack = 0;
                for (int i = 0; i < m && locked; i++) feed(back[i].on, back[i].ms);
            }
            return;
        }
        idleFlushed = false;
        feed(on, ms);
    }

    // After a pause the sender may be another one, or the same at another speed. The first marks after it (up to a word) are held
    // and checked against the model: when a dot length that explains them (and their gaps) at least twice as well is found, the model
    // moves to it before they are read. The speed check that runs on the go needs 16 marks, so the first word after a pause at a new
    // speed was lost ("TEST" read as "T E T", or not at all).
    static constexpr int kPost = 40, kPostMarks = 10;
    Ev post[kPost]; int nPost = 0;
    bool collecting = false;
    float misfit(float l, const float* xs, int m, const float* gs, int ng) const {
        float e = 0;
        for (int i = 0; i < m; i++) {
            const float a = (xs[i] - l) / sd, b = (xs[i] - l - ldr) / sD;
            e += std::min(25.f, std::min(a * a, b * b));
        }
        for (int i = 0; i < ng; i++) {
            const float a = (gs[i] - l - gi) / si, b = (gs[i] - l - gc) / sc, c = gs[i] > l + gw ? 0.f : (gs[i] - l - gw) / sw;
            e += std::min(25.f, std::min(a * a, std::min(b * b, c * c)));
        }
        return e;
    }
    void afterPause() {
        collecting = false;
        float xs[kPost], gs[kPost]; int m = 0, ng = 0;
        const float d = dot();
        for (int i = 0; i < nPost; i++) {
            if (post[i].ms < 0.25f * d) continue;   // noise blips and drop-outs say nothing about the speed
            if (post[i].on) xs[m++] = std::log(post[i].ms); else if (i + 1 < nPost) gs[ng++] = std::log(post[i].ms);
        }
        if (m >= 4 && manualDot <= 0) {
            float cand[6]; int nc = 0;
            float c1, c2, sg; int n1, n2;
            twoMeans(xs, m, c1, c2, n1, n2, sg);
            if (c2 - c1 >= 0.5f) cand[nc++] = c1;
            float mean = 0; for (int i = 0; i < m; i++) mean += xs[i];
            mean /= (float)m;
            cand[nc++] = mean; cand[nc++] = mean - ldr;
            if (ng >= 3) { float g[kPost]; std::memcpy(g, gs, sizeof(float) * (size_t)ng); std::sort(g, g + ng); cand[nc++] = g[ng / 4] - gi; }
            const float cur = misfit(L, xs, m, gs, ng);
            float best = cur, bl = L;
            for (int i = 0; i < nc; i++) {
                const float l = std::max(std::log(20.f), std::min(std::log(260.f), cand[i]));
                const float e = misfit(l, xs, m, gs, ng);
                if (e < best) { best = e; bl = l; }
            }
            if (std::fabs(bl - L) > 0.18f && best < 0.5f * cur) {
                if (std::getenv("CW_TRACE")) fprintf(stderr, "AFTER PAUSE %.0f -> %.0f (misfit %.1f -> %.1f)\n", std::exp(L), std::exp(bl), cur, best);
                L = bl; n = std::min(n, 3);
                nwm = nwg = 0; badRun = 0; fit = 1; ringN = 0;
                clampModel();
            }
        }
        const int k = nPost; nPost = 0;
        for (int i = 0; i < k && locked; i++) feed(post[i].on, post[i].ms);
    }

    void idle(float ms) {
        if (!locked) return;
        if (collecting && nPost > 0) {   // a pause again: the held marks are read (a short word waits longer: the sender may be slower)
            int marks = 0;
            for (int i = 0; i < nPost; i++) marks += post[i].on;
            if (ms > (marks >= 4 ? std::exp(L + 0.5f * (gc + gw)) : 2.f * std::exp(L + gw))) afterPause();
            else return;
        }
        const float thr = std::exp(L + 0.5f * (gc + gw));
        if (ms < thr || idleFlushed) return;
        drain(true);
        if (!haveMark) return;
        if (hyp[0].nPend == 0 && hyp[0].node == 1 && nh == 1) { idleFlushed = true; return; }
        extendSpace(std::log(ms), true);
        commit(hyp[0].nPend);
        // from here on one reading
        Hyp b = hyp[0]; b.nPend = 0; b.node = 1; b.zc = 0; b.wLen = 0; b.score = 0;
        nh = 1; hyp[0] = b;
        idleFlushed = true;
    }
};

FistDecoder::FistDecoder() : p_(std::make_unique<Impl>()) {}
FistDecoder::~FistDecoder() = default;
void FistDecoder::reset() { const float m = p_->manualDot; p_->reset(); p_->manualDot = m; }
void FistDecoder::setManualWpm(double wpm) { p_->manualDot = wpm > 0 ? (float)(1200.0 / std::max(3.0, std::min(60.0, wpm))) : 0.f; }
void FistDecoder::mark(double ms) { p_->event(true, (float)ms); }
void FistDecoder::space(double ms) { p_->event(false, (float)ms); }
void FistDecoder::idle(double ms) { p_->idle((float)ms); }
bool FistDecoder::locked() const { return p_->locked; }
double FistDecoder::wpm() const { return p_->locked ? 1200.0 / dotMs() : 0; }
double FistDecoder::dotMs() const { return std::exp(p_->L); }
double FistDecoder::dashRatio() const { return std::exp(p_->ldr); }

size_t FistDecoder::pop(DecTok* out, size_t max) {
    Impl& m = *p_;
    size_t k = 0;
    while (m.oh != m.vis && k < max) { out[k++] = m.out[m.oh]; m.oh = (m.oh + 1) % kOutQ; }
    return k;
}

void FistDecoder::stats(FistStats& s) const {
    const Impl& m = *p_;
    s.locked = m.locked;
    s.dotMs = (float)std::exp(m.L); s.wpm = m.locked ? 1200.f / s.dotMs : 0.f;
    s.dashRatio = std::exp(m.ldr);
    s.intraGapDots = std::exp(m.gi); s.charGapDots = std::exp(m.gc); s.wordGapDots = std::exp(m.gw);
    s.sigDot = m.sd; s.sigDash = m.sD; s.sigChar = m.sc; s.sigWord = m.sw;
    const float spread = (m.sd + m.sD + m.si) / 3.f;
    s.quality = std::max(0.f, std::min(1.f, 1.f - (spread - 0.07f) / 0.30f));
    s.fit = m.fit;
    float mx = 1e-6f, gx = 1e-6f;
    for (int i = 0; i < kFistBins; i++) { mx = std::max(mx, m.hMarks[i]); gx = std::max(gx, m.hGaps[i]); }
    for (int i = 0; i < kFistBins; i++) { s.marks[i] = m.hMarks[i] / mx; s.gaps[i] = m.hGaps[i] / gx; }
}

int FistDecoder::liveAlternatives(char (*out)[48], int maxN) const {
    const Impl& m = *p_;
    const Tables& t = T();
    int k = 0;
    for (int i = 0; i < m.nh && k < maxN; i++) {
        std::string s;
        const Hyp& h = m.hyp[i];
        for (int j = 0; j < h.nPend; j++) s += t.tok[h.pend[j]];
        if (h.node >= 2 && t.nodeText[h.node]) { s += '['; s += t.nodeText[h.node]; s += ']'; }
        if (s.size() > 47) s = s.substr(s.size() - 47);
        bool dup = false;
        for (int j = 0; j < k; j++) if (s == out[j]) dup = true;
        if (dup || s.empty()) continue;
        std::strncpy(out[k], s.c_str(), 47); out[k][47] = 0; k++;
    }
    return k;
}

} // namespace cw
} // namespace dect2
