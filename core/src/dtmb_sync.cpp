// DTMB frame acquisition (see dtmb_sync.h).
#include "dect2/dtmb_sync.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2::dtmb {

namespace {
// references: the 255 chip core of PN420, the whole PN595 header, the 511 chip core of PN945 (index = Header value)
int refLength(Header h) { return headerInfo(h).core; }
}

Acquirer::Acquirer() : fft_(kBlock) {
    work_.resize((size_t)kBlock);
    x_.resize((size_t)kBlock);
    rho_.resize((size_t)kBlock);
    energy_.resize((size_t)kBlock + 1);
    for (int k = 0; k < 3; k++) {
        const Header h = (Header)k;
        const int len = refLength(h);
        const auto& chips = pnChips(h);
        std::vector<cf32> r((size_t)kBlock, cf32(0, 0));
        for (int i = 0; i < len; i++) r[(size_t)i] = cf32((float)chips[(size_t)i], 0.f);
        fft_.forward(r.data());
        for (auto& v : r) v = std::conj(v);
        spec_[k] = std::move(r);
    }
}

// Squared normalised correlation of the block with the core of a header, at every position (rho_[u]: the window starts at u). It runs over three
// 65536 point blocks per attempt, and attempts go on all the time while nothing is found: one division per position, no square root.
void Acquirer::correlate(int ref, const cf32* xs, int len) {
    const std::vector<cf32>& S = spec_[ref];
    for (int i = 0; i < kBlock; i++) work_[(size_t)i] = xs[i] * S[(size_t)i];
    fft_.inverse(work_.data());
    const float inv2 = 1.f / ((float)kBlock * (float)kBlock);
    const int valid = kBlock - len;
    float mx = 0;
    for (int u = 0; u < valid; u++) {
        const float e = energy_[(size_t)(u + len)] - energy_[(size_t)u];
        const float v = e > 1e-12f ? std::norm(work_[(size_t)u]) * inv2 / (e * (float)len) : 0.f;
        rho_[(size_t)u] = v;
        mx = std::max(mx, v);
    }
    for (int u = valid; u < kBlock; u++) rho_[(size_t)u] = 0.f;
    peak_[ref] = std::sqrt(mx);
}

// Carrier offset from the headers of the chain: |sum of header * chips * exp(-j w n)|^2 summed over the frames, on a grid of offsets. A header is a
// long known sequence, so this is a matched filter in frequency (main lobe +-fs/L, 8 kHz for PN945): no ambiguity, unlike the repeat of the core.
// Returns the offset found (relative to the block as given); `coherence` is the share of the header energy that adds up coherently.
double Acquirer::refineCfo(const cf32* r, const AcqResult& res, float& coherence) const {
    const HeaderInfo& hi = headerInfo(res.header);
    const int L = hi.length, F = hi.framesPerSuper, n = (int)res.starts.size();
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kFrames = 12;
    std::vector<std::vector<cf32>> x;
    double energy = 0;
    int8_t chips[945];
    for (int i = std::max(0, n - kFrames); i < n; i++) {
        const long s = res.starts[(size_t)i];
        if (s < 0 || s + L > kBlock) continue;
        const int frame = res.rotates ? ((res.frame - (n - 1 - i)) % F + F) % F : 0;
        pnHeader(res.header, res.rotates ? pnPhase(res.header, frame) : 0, chips);
        std::vector<cf32> v((size_t)L);
        for (int k = 0; k < L; k++) { v[(size_t)k] = r[s + k] * (float)chips[k]; energy += std::norm(r[s + k]); }
        x.push_back(std::move(v));
    }
    coherence = 0;
    if (x.empty() || energy <= 0) return 0;
    constexpr double kRange = 14000.0, kStep = 400.0;
    const int K = (int)(2 * kRange / kStep) + 1;
    std::vector<double> m((size_t)K, 0.0);
    for (int g = 0; g < K; g++) {
        const double f = -kRange + g * kStep, w = -2.0 * kPi * f / symRate;
        const cf32 step((float)std::cos(w), (float)std::sin(w));
        double acc = 0;
        for (const auto& v : x) {
            cf32 rot(1.f, 0.f), sum(0.f, 0.f);
            for (int k = 0; k < L; k++) {
                sum += v[(size_t)k] * rot;
                rot *= step;
                if ((k & 127) == 127) rot /= std::abs(rot);
            }
            acc += std::norm(sum);
        }
        m[(size_t)g] = acc;
    }
    int best = 0;
    for (int g = 1; g < K; g++) if (m[(size_t)g] > m[(size_t)best]) best = g;
    double off = 0;
    if (best > 0 && best + 1 < K && m[(size_t)best - 1] > 0 && m[(size_t)best + 1] > 0) {
        const double a = std::log(m[(size_t)best - 1]), b = std::log(m[(size_t)best]), c = std::log(m[(size_t)best + 1]);
        const double den = a - 2 * b + c;
        if (den < -1e-12) off = std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den));
    }
    coherence = (float)(m[(size_t)best] / ((double)L * energy));
    return -kRange + (best + off) * kStep;
}

// The strongest correlation within [u - before, u + after] for every u (sliding maximum)
static void slidingMax(const std::vector<float>& v, int n, int before, int after, std::vector<float>& out) {
    out.assign((size_t)n, 0.f);
    std::vector<int> dq((size_t)n + 8);
    int head = 0, tail = 0;
    int next = 0;   // next index to push
    for (int u = 0; u < n; u++) {
        const int hiIdx = std::min(n - 1, u + after);
        while (next <= hiIdx) {
            while (tail > head && v[(size_t)dq[(size_t)tail - 1]] <= v[(size_t)next]) tail--;
            dq[(size_t)tail++] = next++;
        }
        while (dq[(size_t)head] < u - before) head++;
        out[(size_t)u] = v[(size_t)dq[(size_t)head]];
    }
}

// How the frames of a signal are found. Every header has a sharp correlation peak at the position where its core starts; a PN phase that rotates
// moves the core inside the header from frame to frame (by the signed phase), so the peaks of successive frames sit at
//     u_i = S_0 + i * Lf - signedPhase(j0 + i)
// for a first header at S_0 and super-frame number j0 (no rotation: u_i = S_0 + i * Lf). Every hypothesis (an anchor peak in the first frame,
// rotating or not, j0) is scored by the correlation found at the predicted positions, a symbol or two of drift tolerated; the best one fixes the
// frame grid on the path of its anchor. (Chaining the strongest peak of every frame instead breaks when two paths of nearly equal strength swap
// places from frame to frame, a single-frequency network or a strong echo: the chain jumps by the delay between them. The tolerance has to stay at
// a symbol or two: neighbouring super-frame numbers differ by a symbol or two in phase, and the rotation is what tells them apart.)
bool Acquirer::run(const cf32* r, AcqResult& res, float thr) {
    res = AcqResult();
    for (int i = 0; i < kBlock; i++) x_[(size_t)i] = r[i];
    energy_[0] = 0;
    for (int i = 0; i < kBlock; i++) energy_[(size_t)i + 1] = energy_[(size_t)i] + std::norm(r[i]);
    fft_.forward(x_.data());
    const float thr2 = thr * thr;
    double bestScore = 0;
    for (int k = 0; k < 3; k++) {
        const Header h = (Header)k;
        const HeaderInfo& hi = headerInfo(h);
        const int Lf = hi.length + kBody, len = refLength(h), valid = kBlock - len;
        correlate(k, x_.data(), len);
        if (peak_[k] < thr) continue;
        slidingMax(rho_, valid, 1, 1, rmax_);   // +-1 symbol: a peak sits between two samples when the delay of the path is not a whole number of symbols
        // anchors: the strongest local maxima in the first frame period
        struct Anchor { long u; float v; };
        auto localMax = [&](long u) {
            const float v = rho_[(size_t)u];
            for (long w = std::max<long>(0, u - 8); w <= std::min<long>(valid - 1, u + 8); w++) if (rho_[(size_t)w] > v || (rho_[(size_t)w] == v && w < u)) return false;
            return true;
        };
        std::vector<Anchor> anchors;
        for (long u = 0; u < std::min<long>(valid, Lf + 200); u++) if (rho_[(size_t)u] >= thr2 && localMax(u)) anchors.push_back({u, rho_[(size_t)u]});
        std::sort(anchors.begin(), anchors.end(), [](const Anchor& a, const Anchor& b) { return a.v > b.v; });
        if (anchors.size() > 10) anchors.resize(10);
        if (anchors.empty()) continue;
        const int F = hi.framesPerSuper;
        std::vector<int> sph((size_t)F, 0);
        if (hi.cyclic()) for (int f = 0; f < F; f++) { const int p = pnPhase(h, f); sph[(size_t)f] = p > hi.core / 2 ? p - hi.core : p; }
        double hypScore = 0; long hypU0 = 0; int hypJ0 = 0, hypN = 0; bool hypRot = false;
        for (const Anchor& an : anchors) {
            for (int rot = 0; rot < (hi.cyclic() ? 2 : 1); rot++) {
                for (int j0 = 0; j0 < (rot ? F : 1); j0++) {
                    const long S0 = an.u + (rot ? sph[(size_t)j0] : 0);
                    double score = 0; int n = 0, good = 0;
                    long dev = 0;   // how far the anchor's path has drifted from the prediction (clock offset)
                    for (int i = 0;; i++) {
                        const long u = S0 + (long)i * Lf - (rot ? sph[(size_t)((j0 + i) % F)] : 0);
                        if (u < 0 || u >= valid) break;
                        const long c = u + dev;
                        float best = 0; long bestDev = dev;
                        if (c >= 0 && c < valid) best = rmax_[(size_t)c];
                        // follow the drift of the anchor's own path (a clock offset moves it by a fraction of a symbol per frame)
                        if (best >= thr2) {
                            float pv = 0; long pw = c;
                            for (long w = std::max<long>(0, c - 2); w <= std::min<long>(valid - 1, c + 2); w++) if (rho_[(size_t)w] > pv) { pv = rho_[(size_t)w]; pw = w; }
                            if (pv >= thr2) bestDev = pw - u;
                        }
                        dev = std::max<long>(-40, std::min<long>(40, bestDev));
                        score += best; n++;
                        if (best >= thr2) good++;
                    }
                    if (n < 5 || good < std::max(5, (int)(0.8 * n))) continue;
                    if (score > hypScore) { hypScore = score; hypU0 = an.u; hypJ0 = j0; hypN = n; hypRot = rot != 0; }
                }
            }
        }
        if (hypN == 0 || hypScore <= bestScore) continue;
        bestScore = hypScore;
        // the chain: the anchor's own peak, followed through the frames (it moves with the clock offset), at most 6 symbols from the prediction
        res.ok = true; res.header = h; res.rotates = hypRot; res.hits = hypN;
        res.starts.clear();
        const long S0 = hypU0 + (hypRot ? sph[(size_t)hypJ0] : 0);
        double meanRho = 0;
        long dev = 0;
        for (int i = 0; i < hypN; i++) {
            const int sp = hypRot ? sph[(size_t)((hypJ0 + i) % F)] : 0;
            const long u = S0 + (long)i * Lf - sp, c = u + dev;
            long bu = c; float bv = -1;
            for (long w = std::max<long>(0, c - 3); w <= std::min<long>(valid - 1, c + 3); w++) if (rho_[(size_t)w] > bv) { bv = rho_[(size_t)w]; bu = w; }
            if (bv >= thr2) dev = bu - u;
            res.starts.push_back(bu + sp);
            meanRho += std::sqrt(std::max(0.f, bv));
        }
        res.metric = (float)(meanRho / hypN);
        res.start = res.starts.back();
        res.frame = hypRot ? (hypJ0 + hypN - 1) % F : 0;
    }
    if (!res.ok) return false;
    float coherence = 0;
    res.cfoHz = refineCfo(r, res, coherence);
    res.coherence = coherence;
    if (coherence < kMinCoherence) { res = AcqResult(); return false; }   // a chain of peaks, but no header behind it that adds up at any offset
    return true;
}

} // namespace dect2::dtmb
