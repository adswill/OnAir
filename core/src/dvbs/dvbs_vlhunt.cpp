// Frame search for DVB-S2X VL-SNR frames (EN 302 307-2 clause 5.5.2.5), see dvbs_s2hunt.h. The PLHEADER of the ordinary search is 90 symbols, too few to
// stand out of the noise at -9 dB; the 990 symbols of PLHEADER and VL-SNR header are known up to the 10 mode patterns, so they are searched for with the
// products of neighbouring symbols (no carrier phase or frequency needed), summed over the frames of the window. The carrier frequency is then taken
// from the coherent sum over the known symbols.
#include "dvbs_s2hunt.h"
#include "dvbs_vlsnr.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace dect2 {
namespace dvbs {

namespace {

constexpr int kKnown = 90 + kVlHeaderSymbols;

struct VlCand {
    int set, walsh;
    std::vector<cf32> ref;       // the 990 known symbols
    std::vector<cf32> dref;      // products of neighbouring symbols, index k (1 .. 989) = ref[k] conj(ref[k - 1])
};

const std::vector<VlCand>& candidates() {
    static const std::vector<VlCand> c = [] {
        std::vector<VlCand> v;
        for (int set = 1; set <= 2; set++) {
            for (int w = 0; w < 16; w++) {
                const int mi = vlModeByWalsh(w);
                if (!((mi >= 0 && vlMode(mi).set == set) || (w == kVlDummyWalsh && set == 2))) continue;
                VlCand x;
                x.set = set; x.walsh = w;
                x.ref.resize((size_t)kKnown);
                s2PlHeader(set == 1 ? 64 : 65, false, true, x.ref.data());
                vlHeaderSymbols(w, &x.ref[90]);
                x.dref.assign((size_t)kKnown, cf32());
                for (int k = 1; k < kKnown; k++) x.dref[(size_t)k] = x.ref[(size_t)k] * std::conj(x.ref[(size_t)k - 1]);
                v.push_back(std::move(x));
            }
        }
        return v;
    }();
    return c;
}

} // namespace

bool vlHunt(const cf32* z, size_t n, S2HuntResult& out, bool force) {
    // the search is heavy (about 30 FFTs): every other attempt of the caller
    static std::atomic<unsigned> calls{0};
    if ((calls.fetch_add(1) & 1) != 0 && !force) return false;
    if (n < 40000) return false;
    const std::vector<VlCand>& C = candidates();
    size_t N = 1;
    while (N < n) N <<= 1;
    Fft fft((int)N);
    std::vector<cf32> d(N, cf32()), fd, fr(N), cc;
    for (size_t i = 1; i < n; i++) d[i] = z[i] * std::conj(z[i - 1]);

    double bestZ = 0; int bestInv = 0; size_t bestC = 0, bestP = 0; int bestJ = 0;
    struct Pk { double z; int inv; size_t c, p; };
    std::vector<Pk> pk;
    for (int inv = 0; inv < 2; inv++) {
        if (inv) for (size_t i = 0; i < n; i++) d[i] = std::conj(d[i]);
        fd = d;
        fft.forward(fd.data());
        for (size_t ci = 0; ci < C.size(); ci++) {
            const VlCand& c = C[ci];
            const int L = vlFrameSymbols(c.set);
            if (n < (size_t)(L + kKnown + 8)) continue;
            std::fill(fr.begin(), fr.end(), cf32());
            for (int k = 1; k < kKnown; k++) fr[(size_t)k] = c.dref[(size_t)k];
            fft.forward(fr.data());
            cc.resize(N);
            for (size_t k = 0; k < N; k++) cc[k] = fd[k] * std::conj(fr[k]);
            fft.inverse(cc.data());
            // frame sums of the real part: the position of the header in the first frame length
            const size_t cnt = std::min<size_t>((size_t)L, n - (size_t)kKnown);
            std::vector<float> S(cnt);
            std::vector<int> Jv(cnt);
            double sum = 0, sum2 = 0; size_t m = 0;
            int Jmin = 1 << 30;
            for (size_t p = 0; p < cnt; p++) {
                double s = 0; int J = 0;
                for (size_t q = p; q + (size_t)kKnown <= n; q += (size_t)L) { s += cc[q].real(); J++; }
                Jv[p] = J;
                S[p] = (float)(s / std::sqrt((double)J));     // noise of the sum grows as sqrt(J)
                Jmin = std::min(Jmin, J);
            }
            for (size_t p = 0; p < cnt; p++) { sum += S[p]; sum2 += (double)S[p] * S[p]; m++; }
            const double mean = sum / (double)m, sd = std::sqrt(std::max(1e-12, sum2 / (double)m - mean * mean));
            // the peak is 990 symbols wide at the most: mean and sd are those of the noise
            size_t bp = 0;
            for (size_t p = 1; p < cnt; p++) if (S[p] > S[bp]) bp = p;
            const double zs = (S[bp] - mean) / sd;
            if (zs >= 4.5 && Jv[bp] >= 2) pk.push_back({zs, inv, ci, bp});
            if (zs > bestZ && Jv[bp] >= 2) { bestZ = zs; bestInv = inv; bestC = ci; bestP = bp; bestJ = Jv[bp]; }
        }
    }
    int weakAgree = 0;
    for (const Pk& a : pk) if (a.inv == bestInv && a.c != bestC && C[a.c].set == C[bestC].set && (a.p > bestP ? a.p - bestP : bestP - a.p) <= 2) weakAgree++;
    weakAgree++;
    // 7 sd is clear of the noise (the largest of 600 000 values reaches 5); a signal at -5 dB over eight frames is only 5.4 to 6 when the symbol clock
    // wanders a little, so from 5 up the peak is taken when the same position and three mode patterns agree (the patterns of one set share
    // most of their symbols, a signal shows in several of them, noise in one)
    if (bestZ < 7.0 && !(bestZ >= 5.0 && weakAgree >= 3)) return false;

    // ---- the carrier frequency and phase: the coherent sum over the known symbols at each trial frequency, around zero
    const VlCand& c = C[bestC];
    const int L = vlFrameSymbols(c.set);
    std::vector<cf32> x((size_t)kKnown);
    for (int k = 0; k < kKnown; k++) {
        const cf32 s = bestInv ? std::conj(z[bestP + (size_t)k]) : z[bestP + (size_t)k];
        x[(size_t)k] = s * std::conj(c.ref[(size_t)k]);
    }
    auto sumAt = [&](double w) {
        const cf32 stp = std::polar(1.0f, (float)(-w));
        cf32 rot(1.f, 0.f), acc(0, 0);
        for (int k = 0; k < kKnown; k++) { acc += x[(size_t)k] * rot; rot *= stp; if ((k & 127) == 127) rot /= std::abs(rot); }
        return acc;
    };
    double bw = 0, bm = -1;
    for (double w = -0.16; w <= 0.16; w += 0.0015) { const double mg = std::abs(sumAt(w)); if (mg > bm) { bm = mg; bw = w; } }
    double w0 = bw;
    for (int it = 0; it < 3; it++) {
        const double st = 0.0015 / std::pow(3.0, it + 1);
        for (int j = -3; j <= 3; j++) { const double w = bw + j * st; const double mg = std::abs(sumAt(w)); if (mg > bm) { bm = mg; w0 = w; } }
        bw = w0;
    }
    const cf32 acc = sumAt(bw);
    const double amp = std::abs(acc) / kKnown;
    // Es/N0 from the residual of the known symbols
    double res = 0;
    {
        const cf32 stp = std::polar(1.0f, (float)(-bw));
        cf32 rot = std::polar(1.0f, (float)(-std::arg(acc)));
        for (int k = 0; k < kKnown; k++) { res += std::norm(x[(size_t)k] * rot - (float)amp); rot *= stp; if ((k & 127) == 127) rot /= std::abs(rot); }
    }
    res /= kKnown;
    out = S2HuntResult();
    out.ok = true;
    out.firstPos = out.pos = bestP;
    out.lastPos = bestP + (size_t)(bestJ - 1) * (size_t)L;
    out.frameLen = L;
    out.modcod = c.set == 1 ? 64 : 65;
    out.shortFrame = false; out.pilots = true;
    out.phi = bw; out.theta = std::arg(acc);
    out.plsScore = (float)std::min(1.0, amp); out.plsSecond = 0.f;
    out.sofScore = (float)amp;
    out.headers = bestJ;
    out.snrDb = res > 1e-6 ? (float)(10.0 * std::log10(amp * amp / res)) : 30.f;
    out.candidates = 1;
    out.inverted = bestInv != 0;
    return true;
}

} // namespace dvbs
} // namespace dect2
