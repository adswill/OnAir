// PACTOR-II / -III / -IV physical layer (see pactorhf_dsp.h and docs/pactorhf.md).
#include "dect2/pactorhf/pactorhf_dsp.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include "dect2/common/mathconst.h"

namespace dect2 { namespace pactorhf {

namespace {
using dect2::math::kPi;
using cd = std::complex<double>;

// x * exp(-j 2 pi fc t)
void mixDown(const float* x, size_t n, double fc, std::vector<cd>& z) {
    z.resize(n);
    const cd rot = std::polar(1.0, -2 * kPi * fc / kFs);
    cd ph(1, 0);
    for (size_t i = 0; i < n; i++) {
        z[i] = ph * (double)x[i];
        ph *= rot;
        if ((i & 1023) == 1023) ph /= std::abs(ph);
    }
}

// moving sum over w samples, centred (output i covers i-w/2 .. i-w/2+w-1)
void boxcar(const std::vector<cd>& z, int w, std::vector<cd>& out) {
    const size_t n = z.size();
    out.assign(n, cd(0, 0));
    cd acc(0, 0);
    for (size_t i = 0; i < n + (size_t)w; i++) {
        if (i < n) acc += z[i];
        if (i >= (size_t)w) acc -= z[i - (size_t)w];
        const size_t o = i - (size_t)w / 2;
        if (i + 0 >= (size_t)w - 1 && o < n) out[o] = acc;
    }
}

// z-score style strength of the clock line: |sum e exp(-j2 pi n b/fs)|^2 / sum (e - mean)^2
double lineStat(const std::vector<double>& e, double baud, cd* ph = nullptr, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > e.size()) to = e.size();
    if (to <= from + 16) return 0;
    double mean = 0;
    for (size_t i = from; i < to; i++) mean += e[i];
    mean /= (double)(to - from);
    double var = 0;
    cd c(0, 0);
    const cd rot = std::polar(1.0, -2 * kPi * baud / kFs);
    cd p = std::polar(1.0, -2 * kPi * baud / kFs * (double)from);
    for (size_t i = from; i < to; i++) {
        const double v = e[i] - mean;
        var += v * v;
        c += v * p;
        p *= rot;
        if (((i - from) & 1023) == 1023) p /= std::abs(p);
    }
    if (ph) *ph = c;
    if (var <= 1e-30) return 0;
    return std::norm(c) / var;
}

double toDb(double r) { return 10 * std::log10(std::max(r, 1e-9)); }

int boxWidth(double baud) { return std::max(2, (int)std::lround(0.7 * kFs / baud)); }

void envelopeOf(const float* x, size_t n, double fc, double baud, std::vector<double>& e) {
    std::vector<cd> z, f;
    mixDown(x, n, fc, z);
    boxcar(z, boxWidth(baud), f);
    e.resize(n);
    for (size_t i = 0; i < n; i++) e[i] = std::norm(f[i]);
}


// radix-2 FFT, in place, n a power of two
void fftInPlace(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const cd wl = std::polar(1.0, -2 * kPi / (double)len);
        for (size_t i = 0; i < n; i += len) {
            cd w(1, 0);
            for (size_t k = 0; k < len / 2; k++) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

struct Peak { double hz, db, pw = 0; };

std::vector<Peak> scanLines(const float* x, size_t n, double lo, double hi, double step, double baud) {
    std::vector<Peak> all;
    std::vector<double> e;
    for (double fc = lo; fc <= hi + 1e-9; fc += step) {
        envelopeOf(x, n, fc, baud, e);
        double m = 0;
        for (double v : e) m += v;
        all.push_back({fc, toDb(lineStat(e, baud)), m / (double)std::max<size_t>(1, e.size())});
    }
    return all;
}

std::vector<Peak> localMaxima(const std::vector<Peak>& a, double minDb, int halfWidth) {
    std::vector<Peak> out;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].db < minDb) continue;
        bool top = true;
        for (int k = -halfWidth; k <= halfWidth && top; k++) {
            const long j = (long)i + k;
            if (k == 0 || j < 0 || j >= (long)a.size()) continue;
            if (a[(size_t)j].db > a[i].db || (a[(size_t)j].db == a[i].db && j < (long)i)) top = false;
        }
        if (top) out.push_back(a[i]);
    }
    return out;
}

// refine a carrier frequency by the line strength (parabolic over +-12 Hz in 3 Hz steps)
double refineLine(const float* x, size_t n, double fc, double baud) {
    std::vector<double> e;
    double bestF = fc, bestV = -1;
    for (double f = fc - 12; f <= fc + 12.01; f += 3) {
        envelopeOf(x, n, f, baud, e);
        const double v = lineStat(e, baud);
        if (v > bestV) { bestV = v; bestF = f; }
    }
    return bestF;
}

struct Timing { double t0 = 0, sps = 80; double ppm = 0; double snrDb = 0; };

// common symbol timing for a set of carriers: the phase of the 100 baud line of the summed envelopes, per block, fitted by a line
Timing findTiming(const std::vector<std::vector<double>>& envs, double baud) {
    Timing t;
    const double sps0 = kFs / baud;
    const size_t n = envs.empty() ? 0 : envs[0].size();
    const size_t blk = (size_t)(sps0 * 200);
    std::vector<double> tb, pb, wb;
    cd total(0, 0);
    for (size_t b0 = 0; b0 + blk <= n; b0 += blk) {
        cd c(0, 0);
        for (const auto& e : envs) {
            cd cc;
            lineStat(e, baud, &cc, b0, b0 + blk);
            c += cc;
        }
        total += c;
        // line phase: e ~ cos(2 pi baud (t - tau)) => c = |c| exp(-j 2 pi baud tau) up to the sample origin
        const double ph = std::arg(c);
        pb.push_back(ph);
        tb.push_back((double)b0 + blk / 2.0);
        wb.push_back(std::abs(c));
    }
    if (pb.empty()) {
        t.t0 = 0; return t;
    }
    // unwrap
    for (size_t i = 1; i < pb.size(); i++) {
        while (pb[i] - pb[i - 1] > kPi) pb[i] -= 2 * kPi;
        while (pb[i] - pb[i - 1] < -kPi) pb[i] += 2 * kPi;
    }
    // weighted line fit pb = a + b * t
    double sw = 0, st = 0, sp = 0, stt = 0, stp = 0;
    for (size_t i = 0; i < pb.size(); i++) {
        const double w = wb[i];
        sw += w; st += w * tb[i]; sp += w * pb[i]; stt += w * tb[i] * tb[i]; stp += w * tb[i] * pb[i];
    }
    double a = pb[0], b = 0;
    if (pb.size() >= 3) {
        const double den = sw * stt - st * st;
        if (std::abs(den) > 1e-12) { b = (sw * stp - st * sp) / den; a = (sp - b * st) / sw; }
    } else a = sp / sw;
    // phase(t) = a + b t = -2 pi baud tau(t)  where the envelope max sits at time tau (mod symbol); the max is a symbol centre
    // the position of symbol k: t_k = tau0 + k * sps ; the line phase advances by -2 pi per symbol at the nominal rate
    // a drift b/(-2 pi) samples (of symbol time) per sample => the true symbol period is sps0 * (1 + b*sps0/(2 pi) )...
    const double dPhiPerSample = b;                       // rad per sample of excess phase
    const double spsTrue = sps0 / (1.0 + dPhiPerSample * sps0 / (2 * kPi));   // sign fixed below by the tests
    t.sps = spsTrue;
    t.ppm = (sps0 / spsTrue - 1.0) * 1e6;
    t.t0 = -a * spsTrue / (2 * kPi);   // the envelope maximum is a symbol centre
    while (t.t0 < 48) t.t0 += spsTrue;
    while (t.t0 >= 48 + spsTrue) t.t0 -= spsTrue;
    t.snrDb = 0;
    return t;
}

double wrapPi(double a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; }

struct SymbolRun {
    std::vector<cd> s;      // symbol samples
};

// integrate one symbol at time t (samples, fractional) over w samples with a raised-cosine taper
cd symbolAt(const std::vector<cd>& z, double t, int w, bool rect = false) {
    const long c = (long)std::floor(t);
    cd acc(0, 0);
    double sw = 0;
    for (int i = -w / 2; i < w / 2; i++) {
        const long j = c + i;
        if (j < 0 || j >= (long)z.size()) continue;
        const double wt = rect ? 1.0 : 0.5 + 0.5 * std::cos(kPi * (i + 0.5) / (w / 2.0 + 0.5));
        acc += z[(size_t)j] * wt; sw += wt;
    }
    return sw > 0 ? acc / sw : acc;
}

} // namespace

double clockLineDb(const float* x, size_t n, double fc, double baud) {
    std::vector<double> e;
    envelopeOf(x, n, fc, baud, e);
    return toDb(lineStat(e, baud));
}

int levelForCarriers(int n) {
    switch (n) { case 2: return 1; case 6: return 2; case 14: return 3; case 16: return 5; case 18: return 6; default: return 0; }
}

namespace {

// How well one carrier frequency fits a D-PSK signal: the 4th power of the differential phases (ambiguous by baud / 4 = 25 Hz, so the caller keeps
// the search within +- 12 Hz of a good guess) times the power squared. Timing is found on the segment itself.
double scoreCarrier(const float* x, size_t n, double f, size_t segN, bool rect = false) {
    const int win = rect ? 67 : 56;
    segN = std::min(n, segN);
    std::vector<double> e;
    envelopeOf(x, segN, f, 100, e);
    cd c;
    lineStat(e, 100, &c);
    double t0 = -std::arg(c) / (2 * kPi) * 80;
    while (t0 < 48) t0 += 80;
    std::vector<cd> z;
    mixDown(x, segN, f, z);
    cd acc(0, 0); double pw = 0; size_t kk = 0;
    cd prev = symbolAt(z, t0, win, rect);
    for (double t = t0 + 80; t + 40 < (double)segN; t += 80) {
        const cd sk = symbolAt(z, t, win, rect);
        const cd dd = sk * std::conj(prev);
        acc += std::pow(dd / (std::abs(dd) + 1e-12), 4); pw += std::norm(sk);
        prev = sk; kk++;
    }
    if (!kk) return 0;
    const double pm = pw / (double)kk;
    return std::abs(acc) / (double)kk * pm * pm;
}

// -------------------------------------------------------------------- the 100 baud multi-carrier demodulator
void demod100(const float* x, size_t n, const std::vector<double>& fcs, Analysis& a, bool rect) {
    const double baud = 100;
    const int w = rect ? 67 : boxWidth(baud);
    std::vector<std::vector<double>> envs(fcs.size());
    for (size_t c = 0; c < fcs.size(); c++) envelopeOf(x, n, fcs[c], baud, envs[c]);
    Timing tm = findTiming(envs, baud);
    a.clockPpm = tm.ppm;
    a.baud = baud * (kFs / baud / tm.sps);
    const size_t K = (size_t)std::max(0.0, std::floor(((double)n - 40 - tm.t0) / tm.sps)) ;
    a.car.clear();
    std::vector<std::vector<cd>> zs(fcs.size());
    for (size_t c = 0; c < fcs.size(); c++) {
        double fc = fcs[c];
        Carrier car;
        std::vector<cd> sy(K), z;
        double finalR = 0;
        int order = 4;
        std::vector<double> d(K ? K - 1 : 0);
        for (int it = 0; it < 7; it++) {
            mixDown(x, n, fc, z);
            for (size_t k = 0; k < K; k++) sy[k] = symbolAt(z, tm.t0 + (double)k * tm.sps, w, rect);
            // differential phases (power weighted off: all symbols count equally by phase only)
            for (size_t k = 1; k < K; k++) d[k - 1] = std::arg(sy[k] * std::conj(sy[k - 1]));
            auto R = [&](int m, double* ang) {
                cd s(0, 0);
                for (double v : d) s += std::polar(1.0, m * v);
                if (ang) *ang = std::arg(s) / m;
                return d.empty() ? 0.0 : std::abs(s) / (double)d.size();
            };
            double a2, a4, a8, a16;
            const double r2 = R(2, &a2), r4 = R(4, &a4), r8 = R(8, &a8), r16 = R(16, &a16);
            // the carrier frequency comes from the 4th power (valid for 2 and 4 phase steps) while it is still off; the order is only decided
            // when that has converged (a constant phase step per symbol would otherwise look like 8 or 16 steps)
            const double hzErr4 = a4 * baud / (2 * kPi);
            if ((it < 3 || std::abs(hzErr4) > 2.0) && std::abs(hzErr4) >= 0.15) {
                order = 4; finalR = r4;
                fc += hzErr4;
                continue;
            }
            if (r2 > 0.7) order = 2; else if (r4 > 0.5) order = 4; else if (r8 > 0.5) order = 8; else if (r16 > 0.4) order = 16; else order = 4;
            const double ang = order == 2 ? a2 : order == 4 ? a4 : order == 8 ? a8 : a16;
            finalR = order == 2 ? r2 : order == 4 ? r4 : order == 8 ? r8 : r16;
            const double hzErr = ang * baud / (2 * kPi);
            if (std::abs(hzErr) < 0.15) break;
            fc += hzErr;
        }
        // final symbols
        const double step = 2 * kPi / order;
        double angRes = 0;
        {
            cd s(0, 0);
            for (double v : d) s += std::polar(1.0, order * v);
            angRes = std::arg(s) / order;
        }
        car.hz = fc; car.order = order; car.r = finalR;
        car.sym.assign(K, 0);
        {   // the constant part of the phase step (a frequency error) is followed block by block, so a drifting oscillator does not matter
            const size_t B = 96;
            std::vector<double> off((K + B) / B + 1, angRes);
            for (size_t b = 0; b * B < d.size(); b++) {
                cd acc(0, 0);
                const size_t lo = b * B > B ? b * B - B : 0, hi = std::min(d.size(), b * B + 2 * B);   // overlapping blocks
                for (size_t k = lo; k < hi; k++) acc += std::polar(1.0, order * (d[k] - angRes));
                off[b] = angRes + (std::abs(acc) > 0.15 * (double)(hi - lo) ? std::arg(acc) / order : 0.0);
            }
            for (size_t k = 1; k < K; k++) {
                const double v = wrapPi(d[k - 1] - off[(k - 1) / B]);
                long q = (long)std::lround(v / step);
                q %= order; if (q < 0) q += order;
                car.sym[k] = (uint8_t)q;
            }
        }
        car.lineDb = toDb(lineStat(envs[c], baud));
        a.car.push_back(std::move(car));
    }
    a.symbols = K;
    a.startSec = tm.t0 / kFs;
    a.lengthSec = (double)K * tm.sps / kFs;
}

// -------------------------------------------------------------------- frame structure from repeats
void findFrames(Analysis& a) {
    FrameInfo fi;
    if (a.car.empty()) { a.frame = fi; return; }
    const size_t K = a.car[0].sym.size();
    const int order = a.car[0].order ? a.car[0].order : 4;
    const size_t nUse = std::min<size_t>(a.car.size(), 4);
    std::vector<size_t> use;
    for (size_t i = 0; i < nUse; i++) use.push_back(i * (a.car.size() - 1) / std::max<size_t>(1, nUse - 1));
    if (use.size() > 1 && use[0] == use[1]) use.resize(1);
    const int win = 24;
    struct Cand { int lag; double score; double mean; };
    std::vector<Cand> cands;
    const size_t maxLag = std::min<size_t>(K / 5, 3000);
    std::vector<double> prof, cnt;
    for (size_t L = 40; L <= maxLag; L++) {
        prof.assign(L, 0); cnt.assign(L, 0);
        double tot = 0, totN = 0;
        for (size_t c : use) {
            const auto& s = a.car[c].sym;
            for (size_t k = 1; k + L < K; k++) {
                const double eq = s[k] == s[k + L] ? 1.0 : 0.0;
                prof[k % L] += eq; cnt[k % L] += 1; tot += eq; totN += 1;
            }
        }
        if (totN < 4.0 * L * (double)use.size()) continue;
        // best cyclic window, as a z-score against chance (the number of comparisons differs with the lag)
        double bestZ = -1e9, bestMean = 0;
        std::vector<double> pe(L), pc(L);
        for (size_t i = 0; i < L; i++) { pe[i] = prof[i]; pc[i] = cnt[i]; }
        double se = 0, sc = 0;
        for (int i = 0; i < win; i++) { se += pe[(size_t)i % L]; sc += pc[(size_t)i % L]; }
        const double ch = 1.0 / order;
        auto zOf = [&](double e, double c) { return c > 0 ? (e - ch * c) / std::sqrt(c * ch * (1 - ch)) : -1e9; };
        bestZ = zOf(se, sc); bestMean = sc > 0 ? se / sc : 0;
        for (size_t i = 1; i < L; i++) {
            se += pe[(i + win - 1) % L] - pe[(i - 1) % L]; sc += pc[(i + win - 1) % L] - pc[(i - 1) % L];
            const double z = zOf(se, sc);
            if (z > bestZ) { bestZ = z; bestMean = se / sc; }
        }
        cands.push_back({(int)L, bestZ, tot / totN});
        (void)bestMean;
    }
    double top = 0;
    for (auto& c : cands) top = std::max(top, c.score);
    const double chance = 1.0 / order;
    if (top < 9.0) { a.frame = fi; return; }   // a window of 24 symbols that agrees far beyond chance (z of 9 over ~1500 lags and ~1500 positions is below 1e-9 for noise)
    int lag = 0;
    for (auto& c : cands) if (c.score >= top * 0.97) { lag = c.lag; break; }   // smallest lag that is as good as the best
    fi.found = true; fi.period = lag; fi.periodSec = lag * (kFs / (a.baud > 0 ? a.baud : 100)) / kFs;
    double mean = 0;
    for (auto& c : cands) if (c.lag == lag) mean = c.mean;
    fi.match = mean;
    (void)chance;
    // constant region: positions with agreement above 0.8, the longest cyclic run
    {
        std::vector<double> prof2(lag, 0), cnt2(lag, 0);
        for (size_t c : use) {
            const auto& s = a.car[c].sym;
            for (size_t k = 1; k + (size_t)lag < K; k++) { prof2[k % (size_t)lag] += s[k] == s[k + (size_t)lag]; cnt2[k % (size_t)lag] += 1; }
        }
        std::vector<char> good((size_t)lag);
        for (int i = 0; i < lag; i++) good[(size_t)i] = cnt2[(size_t)i] > 0 && prof2[(size_t)i] / cnt2[(size_t)i] > 0.5 + 0.5 * (1.0 / order);   // well above chance, also on a noisy channel
        int bestLen = 0, bestPos = 0;
        for (int st = 0; st < lag; st++) {
            if (good[(size_t)st] && good[(size_t)((st + lag - 1) % lag)]) continue;   // start at a run beginning
            int len = 0;
            while (len < lag && good[(size_t)((st + len) % lag)]) len++;
            if (len > bestLen) { bestLen = len; bestPos = st; }
            if (len >= lag) break;
        }
        fi.syncPos = bestPos; fi.syncLen = bestLen;
        // majority symbols of carrier 0 over that region, as hex (2 bits per symbol)
        const auto& s = a.car[0].sym;
        std::string hex;
        uint32_t acc = 0; int nb = 0;
        for (int i = 0; i < std::min(bestLen, 64); i++) {
            int votes[16] = {0};
            for (size_t k = (size_t)((bestPos + i) % lag); k < K; k += (size_t)lag) if (k > 0) votes[s[k] & 15]++;
            int m = 0;
            for (int q = 1; q < 16; q++) if (votes[q] > votes[m]) m = q;
            acc = (acc << 2) | (uint32_t)(m & 3); nb += 2;
            if (nb == 8) { char b[4]; snprintf(b, sizeof b, "%02X", acc & 0xFF); hex += b; acc = 0; nb = 0; }
        }
        fi.syncHex = hex;
        int copies = 0;
        for (size_t k = 0; k + (size_t)lag <= K; k += (size_t)lag) copies++;
        fi.copies = copies + 1;
    }
    a.frame = fi;
}

// -------------------------------------------------------------------- analysis of the 100 baud signals
bool analyse100(const float* x, size_t n, int variant, Analysis& out) {
    // use the last 12 s for the search, everything for the demodulation
    const size_t scanN = std::min<size_t>(n, (size_t)(12 * kFs));
    const float* xs = x + (n - scanN);
    auto coarse = scanLines(xs, scanN, 300, 3300, 10, 100);
    double maxDb = 0;
    for (auto& p : coarse) maxDb = std::max(maxDb, p.db);
    if (maxDb < 22) return false;   // noise alone reaches 14 .. 17 dB over ~600 candidate frequencies
    double maxPw = 0;
    for (auto& p : coarse) maxPw = std::max(maxPw, p.pw);
    for (auto& p : coarse) if (p.pw < 0.06 * maxPw) p.db = 0;   // a line without power behind it (leakage of the strong carriers, or noise)
    auto pk = localMaxima(coarse, std::max(10.0, maxDb - 12), 6);
    if (pk.size() < 2) return false;
    // refine all peaks
    for (auto& p : pk) {
        p.hz = refineLine(xs, scanN, p.hz, 100);
    }
    std::vector<Peak> sel;
    int kind = 0;   // 2 or 3
    double gridOff = 0;
    // PACTOR-III: the carriers sit on 480 + 120 n Hz (plus the tuning error of the radio). Score every grid point at every tuning error: a D-PSK carrier
    // on the grid shows a tight 4th power of its phase steps, the others do not.
    if (variant != 2) {
        const size_t segN = std::min<size_t>(n, (size_t)(6 * kFs));
        std::vector<std::vector<double>> sc;   // [offset][n + 1]
        std::vector<double> offs;
        for (double o = -60; o < 60; o += 8) offs.push_back(o);
        double bestSum = -1; size_t bestO = 0;
        for (size_t oi = 0; oi < offs.size(); oi++) {
            std::vector<double> row;
            double sum = 0;
            for (int g = -1; g <= 18; g++) { const double v = scoreCarrier(x, n, 480 + 120.0 * g + offs[oi], segN, true); row.push_back(v); sum += v; }
            sc.push_back(row);
            if (sum > bestSum) { bestSum = sum; bestO = oi; }
        }
        // fine offset around the best one
        double bo = offs[bestO], bs = bestSum;
        for (double o = offs[bestO] - 6; o <= offs[bestO] + 6.01; o += 2) {
            double sum = 0;
            for (int g = -1; g <= 18; g++) sum += scoreCarrier(x, n, 480 + 120.0 * g + o, segN, true);
            if (sum > bs) { bs = sum; bo = o; }
        }
        std::vector<double> row;
        double mx = 0;
        for (int g = -1; g <= 18; g++) { row.push_back(scoreCarrier(x, n, 480 + 120.0 * g + bo, segN, true)); mx = std::max(mx, row.back()); }
        std::vector<Peak> g3;
        for (int g = -1; g <= 18; g++) if (row[(size_t)(g + 1)] >= 0.3 * mx && mx > 0) g3.push_back({480 + 120.0 * g + bo, 0, row[(size_t)(g + 1)]});
        if (g3.size() >= 3 || (variant == 3 && g3.size() >= 2)) { sel = g3; kind = 3; gridOff = bo; }
    }
    // pair at 170 .. 270 Hz
    double bestPair = -1; size_t pa = 0, pb = 0;
    for (size_t i = 0; i < pk.size(); i++) for (size_t j = i + 1; j < pk.size(); j++) {
        const double dlt = pk[j].hz - pk[i].hz;
        if (dlt >= 170 && dlt <= 270) {
            const double sc = std::min(pk[i].db, pk[j].db);
            if (sc > bestPair) { bestPair = sc; pa = i; pb = j; }
        }
    }
    if (kind == 3) {
        // taken from the grid scan
    } else if (variant == 2 || (variant == 0 && bestPair > 0)) {
        if (bestPair < 0) return false;
        sel = {pk[pa], pk[pb]};
        kind = 2;
    } else return false;
    // fill gaps of the grid (a carrier that is weak under fading): check each 120 Hz position between the first and the last
    if (kind == 3 && sel.size() >= 2) {
        std::sort(sel.begin(), sel.end(), [](const Peak& a, const Peak& b) { return a.hz < b.hz; });
        std::vector<Peak> filled;
        for (size_t i = 0; i < sel.size(); i++) {
            filled.push_back(sel[i]);
            if (i + 1 < sel.size()) {
                const int gaps = (int)std::lround((sel[i + 1].hz - sel[i].hz) / 120.0) - 1;
                for (int g = 1; g <= gaps; g++) filled.push_back({sel[i].hz + 120.0 * g, 0});
            }
        }
        sel = filled;
        // 2 carriers of PACTOR-III level 1 are not necessarily neighbours; nothing to fill then
    }
    std::vector<double> fcs;
    for (auto& p : sel) fcs.push_back(p.hz);
    std::sort(fcs.begin(), fcs.end());
    {   // start frequencies from the standard: PACTOR-II is a pair 200 Hz apart, PACTOR-III sits on the grid 480 + 120 n
        const size_t segN = std::min<size_t>(n, (size_t)(8 * kFs));
        if (kind == 2) {
            const double c0 = (fcs[0] + fcs[1]) / 2;
            double bs = -1, bc = c0;
            for (double c = c0 - 24; c <= c0 + 24.01; c += 2) {
                const double sc = scoreCarrier(x, n, c - 100, segN) + scoreCarrier(x, n, c + 100, segN);
                if (sc > bs) { bs = sc; bc = c; }
            }
            fcs = {bc - 100, bc + 100};
        } else if (kind == 3 && fcs.size() >= 2) {
            std::vector<int> idx;
            for (double f : fcs) idx.push_back((int)std::lround((f - 480 - gridOff) / 120.0));
            double bs = -1, bo = gridOff;
            for (double o = gridOff - 6; o <= gridOff + 6.01; o += 2) {
                double sc = 0;
                const size_t step = std::max<size_t>(1, idx.size() / 4);
                for (size_t i = 0; i < idx.size(); i += step) sc += scoreCarrier(x, n, 480 + 120.0 * idx[i] + o, segN, true);
                if (sc > bs) { bs = sc; bo = o; }
            }
            for (size_t i = 0; i < fcs.size(); i++) fcs[i] = 480 + 120.0 * idx[i] + bo;
        }
    }
    Analysis a;
    a.variant = kind;
    a.nCarriers = (int)fcs.size();
    a.centreHz = (fcs.front() + fcs.back()) / 2;
    a.spacingHz = fcs.size() > 1 ? (fcs.back() - fcs.front()) / (double)(fcs.size() - 1) : 0;
    if (kind == 3) a.level = levelForCarriers(a.nCarriers);
    a.bwHz = (fcs.back() - fcs.front()) + (kind == 2 ? 100 : 130);
    // demodulate the whole buffer
    demod100(x, n, fcs, a, kind == 3);
    for (auto& c : a.car) a.snrDb = std::max(a.snrDb, c.lineDb);
    if (kind == 3 && a.nCarriers == 14 && !a.car.empty()) a.level = a.car[0].order == 2 ? 3 : 4;   // the same carrier count: DBPSK is level 3, DQPSK level 4
    a.found = true;
    findFrames(a);
    out = std::move(a);
    return true;
}

// -------------------------------------------------------------------- PACTOR-IV (1800 baud single carrier)
void lowpassTaps(std::vector<double>& h, double cutHz, int ntap) {
    h.resize((size_t)ntap);
    const int m = ntap / 2;
    double s = 0;
    for (int i = 0; i < ntap; i++) {
        const double t = i - m;
        const double sinc = t == 0 ? 2 * cutHz / kFs : std::sin(2 * kPi * cutHz / kFs * t) / (kPi * t);
        const double wnd = 0.54 - 0.46 * std::cos(2 * kPi * i / (ntap - 1));
        h[(size_t)i] = sinc * wnd; s += h[(size_t)i];
    }
    for (double& v : h) v /= s;
}

void baseband1500(const float* x, size_t n, double fc, std::vector<cd>& z) {
    static std::vector<double> h;
    if (h.empty()) lowpassTaps(h, 1350, 41);
    std::vector<cd> m;
    mixDown(x, n, fc, m);
    z.assign(n, cd(0, 0));
    const int nt = (int)h.size();
    for (size_t i = (size_t)nt; i < n; i++) {
        cd a(0, 0);
        for (int k = 0; k < nt; k++) a += m[i - (size_t)k] * h[(size_t)k];
        z[i - (size_t)nt / 2] = a;
    }
}

// The clock line over blocks of 2 s added as powers: a sender clock that is off by 50 ppm moves the symbols by a third of a symbol in 2 s (coherent
// over 10 s it would smear the line)
double blockLine(const std::vector<double>& e, double baud, size_t from, size_t to) {
    const size_t B = (size_t)(2 * kFs);
    double num = 0, den = 0;
    for (size_t b0 = from; b0 + B <= to; b0 += B) {
        cd c;
        const double r = lineStat(e, baud, &c, b0, b0 + B);
        if (r <= 0) continue;
        // lineStat returns |c|^2 / var: add |c|^2 and var separately
        double mean = 0;
        for (size_t i = b0; i < b0 + B; i++) mean += e[i];
        mean /= (double)B;
        double var = 0;
        for (size_t i = b0; i < b0 + B; i++) var += (e[i] - mean) * (e[i] - mean);
        num += r * var; den += var;
    }
    return den > 0 ? num / den : 0;
}

bool analyse1800(const float* x, size_t n, Analysis& out) {
    const size_t N = std::min<size_t>(n, (size_t)(10 * kFs));
    const float* xs = x + (n - N);
    const double baud = 1800;
    double bestDb = -1e9, bestF = 1500;
    std::vector<cd> z;
    std::vector<double> e(N);
    for (double fc = 1220; fc <= 1780; fc += 20) {
        baseband1500(xs, N, fc, z);
        for (size_t i = 0; i < N; i++) e[i] = std::norm(z[i]);
        const double db = toDb(blockLine(e, baud, 64, N - 64));
        if (db > bestDb) { bestDb = db; bestF = fc; }
    }
    if (bestDb < 9) return false;   // 2 s blocks added as powers: noise alone stays below about 5 dB
    // refine the centre: maximize the 4th/2nd order spectral purity later; the clock line is flat in fc, so use 1500 +- offset from the band edges
    // measure the power spectrum edges of the band around bestF
    baseband1500(xs, N, bestF, z);
    for (size_t i = 0; i < N; i++) e[i] = std::norm(z[i]);
    cd ph;
    const double lineBest = blockLine(e, baud, 64, N - 64);
    (void)lineBest;
    lineStat(e, baud, &ph, 64, N - 64);
    const double tau = -std::arg(ph) / (2 * kPi) * (kFs / baud);   // envelope maximum
    const double sps = kFs / baud;
    // symbol samples by linear interpolation of the filtered baseband
    std::vector<cd> s;
    for (double t = tau + sps * 8; t + 2 < (double)N - 64; t += sps) {
        const long i = (long)std::floor(t);
        const double f = t - (double)i;
        s.push_back(z[(size_t)i] * (1 - f) + z[(size_t)i + 1] * f);
    }
    if (s.size() < 500) return false;
    double pw = 0;
    for (auto& v : s) pw += std::norm(v);
    pw /= (double)s.size();
    for (auto& v : s) v /= std::sqrt(pw);
    // power-law lines: the m-th power of a PSK signal has a spectral line at m times the carrier error; its height tells the order
    auto peakM = [&](int m, double* fOut) {
        size_t nf = 1;
        while (nf < s.size() * 2) nf <<= 1;
        std::vector<cd> v(nf, cd(0, 0));
        for (size_t k = 0; k < s.size(); k++) { cd q(1, 0); for (int i = 0; i < m; i++) q *= s[k]; v[k] = q; }
        fftInPlace(v);
        size_t bi = 0; double bv = 0;
        for (size_t i = 0; i < nf; i++) { const double a2 = std::norm(v[i]); if (a2 > bv) { bv = a2; bi = i; } }
        double fr = (double)bi / (double)nf;
        if (fr > 0.5) fr -= 1.0;
        if (fOut) *fOut = fr / m;     // cycles per symbol of the carrier error
        return std::sqrt(bv) / (double)s.size();
    };
    double f2 = 0, f4 = 0, f8 = 0;
    const double p2 = peakM(2, &f2), p4 = peakM(4, &f4), p8 = peakM(8, &f8);
    // constant modulus (PSK) or not (QAM): the spread of the symbol power
    double m2 = 0, m4 = 0;
    for (auto& q : s) { const double e2 = std::norm(q); m2 += e2; m4 += e2 * e2; }
    m2 /= (double)s.size(); m4 /= (double)s.size();
    const double cv = m4 / (m2 * m2) - 1.0;
    int con = 0;
    double fOff = 0;
    if (p2 > 0.5) { con = 2; fOff = f2; }
    else if (p4 > 0.7 && cv < 0.28) { con = 4; fOff = f4; }
    else if (p8 > 0.5 && p4 < 0.3 && cv < 0.28) { con = 8; fOff = f8; }
    else if (cv >= 0.28) { con = 16; fOff = f4; }   // QAM (16 or 32 points): not told apart here
    Analysis a;
    a.found = true; a.variant = 4; a.baud = baud; a.nCarriers = 1;
    a.centreHz = bestF + fOff * baud;
    a.bwHz = 2400; a.lineDb = bestDb; a.snrDb = bestDb;
    a.constellation = con; a.symbols = s.size();
    a.lengthSec = (double)N / kFs;
    char b[160];
    snprintf(b, sizeof b, "1800 baud clock line %.1f dB; symbol power lines p2 %.2f p4 %.2f p8 %.2f, modulus spread %.2f", bestDb, p2, p4, p8, cv);
    a.note = b;
    out = std::move(a);
    return true;
}

} // namespace

Analysis analyse(const float* x, size_t n, int variant) {
    Analysis res;
    if (n < (size_t)(2 * kFs)) return res;
    if (variant == 4) { analyse1800(x, n, res); return res; }
    if (variant == 0) {   // the 1800 baud single carrier has a clock line that the 100 baud signals do not: try it first
        Analysis a4;
        if (analyse1800(x, n, a4) && a4.lineDb > 12) return a4;
    }
    Analysis a;
    if (analyse100(x, n, variant, a)) res = std::move(a);
    return res;
}

}} // namespace dect2::pactorhf
