// NB-IoT cell search and decode (nbiot_phy.h): NPSS detection, timing and carrier / clock offset, NSSS (cell id), NPBCH (MIB-NB), SIB1-NB.
#include "dect2/nbiot/nbiot_phy.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/dsp/fftutil.h"
#include "../cell/cell_lte_int.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace dect2::nbiot {

namespace {

constexpr double kF240 = 240000.0;
constexpr int kHyp = 2500;          // carrier offset hypotheses (Hz) of the first detection
constexpr float kMinNpssDefault = 3.6f;
constexpr float kMinNsss = 2.6f;

struct Tmpl {
    cf32 c16[16], c128[128];        // conjugated useful part of one NPSS symbol (no cover code)
    int o240[15];                   // useful start of symbol l at 240 ksps
    int g[15];                      // S(l) S(l+1)
    Tmpl() {
        cf32 t[128];
        npssSymbol(16, t);
        for (int i = 0; i < 16; i++) c16[i] = std::conj(t[i]);
        npssSymbol(128, t);
        for (int i = 0; i < 128; i++) c128[i] = std::conj(t[i]);
        for (int l = 0; l < 14; l++) o240[l] = (int)std::lround(symUseful(l) / 8.0);
        for (int l = 3; l < 13; l++) g[l] = npssCover(l) * npssCover(l + 1);
    }
};
const Tmpl& tm() { static const Tmpl t; return t; }

std::vector<cf32> toBase240(const cf32* x, size_t n, double fs, double shiftHz) {
    const size_t nOut = (size_t)std::max(0.0, std::floor((double)n / fs * kF240) - 2);
    return cell::resampleBlock(x, n, fs, kF240, shiftHz, kF240, 0, nOut, 1.0);
}

struct Det { double hHz = 0; int phi = 0; float score = 0; };

// The folded NPSS metric of a 240 ksps stream over the carrier offset hypotheses
Det detect240(const std::vector<cf32>& y, double rangeHz) {
    Det best;
    const Tmpl& T = tm();
    const int N = (int)y.size();
    const int o13 = T.o240[13];
    if (N < 2 * 2400 + o13 + 32) return best;
    std::vector<double> pw((size_t)N + 1, 0.0);
    for (int i = 0; i < N; i++) pw[(size_t)i + 1] = pw[(size_t)i] + (double)std::norm(y[(size_t)i]);
    std::vector<cf32> Z((size_t)N - 15), tpl(16);
    std::vector<float> F(2400);
    const int nh = (int)(rangeHz / kHyp);
    std::vector<float> peaks((size_t)(2 * nh + 1), 0.f);
    std::vector<int> phis((size_t)(2 * nh + 1), 0);
    float bestScore = 0;
    int bestIdx = -1;
    for (int hi = -nh; hi <= nh; hi++) {
        const double h = (double)hi * kHyp;
        for (int m = 0; m < 16; m++) { const double ph = -2 * M_PI * h * m / kF240; tpl[(size_t)m] = T.c16[m] * cf32((float)std::cos(ph), (float)std::sin(ph)); }
        for (int n = 0; n + 16 <= N; n++) {
            cf32 s(0, 0);
            const cf32* p = &y[(size_t)n];
            for (int m = 0; m < 16; m++) s += p[m] * tpl[(size_t)m];
            Z[(size_t)n] = s;
        }
        std::fill(F.begin(), F.end(), 0.f);
        const int nmax = N - 16 - o13;
        for (int n = 0; n < nmax; n++) {
            cf32 s(0, 0);
            for (int l = 3; l < 13; l++) s += (float)T.g[l] * Z[(size_t)(n + T.o240[l])] * std::conj(Z[(size_t)(n + T.o240[l + 1])]);
            const double p = pw[(size_t)(n + o13 + 16)] - pw[(size_t)n];
            F[(size_t)(n % 2400)] += (float)(std::abs(s) / (p + 1e-20));
        }
        int pk = 0;
        for (int i = 1; i < 2400; i++) if (F[(size_t)i] > F[(size_t)pk]) pk = i;
        double sum = 0; int cnt = 0;
        for (int i = 0; i < 2400; i++) { int d = std::abs(i - pk); d = std::min(d, 2400 - d); if (d > 10) { sum += F[(size_t)i]; cnt++; } }
        const float sc = (float)(F[(size_t)pk] / (sum / std::max(1, cnt) + 1e-20));
        peaks[(size_t)(hi + nh)] = sc; phis[(size_t)(hi + nh)] = pk;
        if (sc > bestScore) { bestScore = sc; bestIdx = hi + nh; }
    }
    if (bestIdx < 0) return best;
    double h = (double)(bestIdx - nh) * kHyp;
    if (bestIdx > 0 && bestIdx + 1 < (int)peaks.size()) {   // the parabola through the neighbouring hypotheses
        const float a = peaks[(size_t)bestIdx - 1], b = peaks[(size_t)bestIdx], c = peaks[(size_t)bestIdx + 1];
        const float den = a - 2 * b + c;
        if (den < 0) h += kHyp * 0.5 * (a - c) / den;
    }
    best.hHz = h; best.phi = phis[(size_t)bestIdx]; best.score = bestScore;
    return best;
}

// ---------------------------------------------------------------- the aligned carrier
struct Aligned {
    std::vector<cf32> buf;          // 16 samples of padding, the frames, 16 samples of padding
    int frames = 0;
    double cfoHz = 0, clockPpm = 0;
    cf32* y() { return buf.data() + 16; }
    const cf32* sub(int f, int sf) const { return buf.data() + 16 + (size_t)f * kFrame + (size_t)sf * kSubframe; }
};

// the differential NPSS metric of the subframe starting at s (kFs)
float npssMetric(const cf32* y, size_t n, long s) {
    const Tmpl& T = tm();
    if (s < 0 || (size_t)(s + symUseful(13) + kFft) > n) return 0;
    cf32 Z[14];
    for (int l = 3; l < 14; l++) {
        cf32 a(0, 0);
        const cf32* p = y + s + symUseful(l);
        for (int m = 0; m < kFft; m++) a += p[m] * T.c128[m];
        Z[l] = a;
    }
    cf32 acc(0, 0);
    for (int l = 3; l < 13; l++) acc += (float)T.g[l] * Z[l] * std::conj(Z[l + 1]);
    return std::abs(acc);
}

bool align(const std::vector<cf32>& y240, const Det& d, Aligned& out) {
    // the carrier offset removed, then 8 times the rate
    std::vector<cf32> z = y240;
    cell::mixDown(z.data(), z.size(), d.hHz, kF240);
    const cell::Resampler rs(0.47, 14);
    std::vector<cf32> y1(z.size() * 8);
    rs.run(z.data(), z.size(), 0.0, 0.125, y1.data(), y1.size());
    const size_t N1 = y1.size();
    // the timing of the NPSS in every frame: windows of 8 frames, a line through their peaks
    const long p0 = (long)d.phi * 8 + 4;
    double a0 = (double)p0, L = (double)kFrame;
    std::vector<std::pair<double, double>> pts;   // frame index, position
    const int maxFrames = (int)((double)(N1 - (size_t)p0) / kFrame);
    if (maxFrames < 4) return false;
    const int W = 8;
    for (int j0 = 0; j0 + 2 <= maxFrames; j0 += W) {
        const int jn = std::min(W, maxFrames - j0);
        // the first window searches wide: the coarse timing is a mean over the whole block, and with a 50 ppm sample clock the first frames
        // sit up to 100 samples (at 1.92 Msps) from it. +-20 then held a side peak of the NPSS, 60 samples off, and the cell's NSSS and MIB failed.
        const int R = pts.empty() ? 128 : 20;
        std::vector<double> S((size_t)(2 * R + 1), 0.0);
        int used = 0;
        for (int dl = -R; dl <= R; dl++) {
            double sum = 0;
            used = 0;
            for (int j = j0; j < j0 + jn; j++) {
                const long s = (long)std::lround(a0 + L * j) + dl;
                if (s < 0 || (size_t)(s + symUseful(13) + kFft + 2) > N1) continue;
                sum += npssMetric(y1.data(), N1, s);
                used++;
            }
            S[(size_t)(dl + R)] = sum;
        }
        if (used < 2) continue;
        const int nS = 2 * R + 1;
        int pk = 0;
        for (int i = 1; i < nS; i++) if (S[(size_t)i] > S[(size_t)pk]) pk = i;
        std::vector<double> sorted = S;
        std::nth_element(sorted.begin(), sorted.begin() + R, sorted.end());
        if (S[(size_t)pk] < 2.2 * sorted[(size_t)R]) continue;
        double frac = 0;
        if (pk > 0 && pk < nS - 1) { const double a = S[(size_t)pk - 1], b = S[(size_t)pk], c = S[(size_t)pk + 1]; const double den = a - 2 * b + c; if (den < 0) frac = 0.5 * (a - c) / den; }
        const double jc = j0 + 0.5 * (jn - 1);
        pts.push_back({jc, a0 + L * jc + (pk - R) + frac});
        if (pts.size() >= 2) {   // least squares line through the points
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (const auto& p : pts) { sx += p.first; sy += p.second; sxx += p.first * p.first; sxy += p.first * p.second; }
            const double n = (double)pts.size(), den = n * sxx - sx * sx;
            if (den > 1e-9) {
                double l = (n * sxy - sx * sy) / den;
                l = std::max(kFrame * (1 - 300e-6), std::min(kFrame * (1 + 300e-6), l));
                L = l; a0 = (sy - L * sx) / n;
            }
        } else a0 = pts[0].second - L * jc;
    }
    if (pts.empty()) return false;
    const double scale = L / kFrame;
    double A = a0 - 5.0 * kSubframe * scale;   // frame 0 starts five subframes before its NPSS
    while (A < 24) A += L;
    const int nF = (int)std::floor(((double)N1 - 40.0 - A) / L);
    if (nF < 4) return false;
    out.frames = nF;
    out.clockPpm = (scale - 1.0) * 1e6;   // input samples per output sample: > 1 means the input clock runs fast... reported as the frame length error
    out.buf.assign((size_t)nF * kFrame + 32, cf32(0, 0));
    const cell::Resampler rs2(0.47, 14);
    rs2.run(y1.data(), N1, A, scale, out.buf.data() + 16, (size_t)nF * kFrame);
    out.cfoHz = d.hHz;
    return true;
}

// the carrier offset left after the alignment, from the phase of the NPSS symbols over lags of 1, 5 and 10 symbols
double refineCfo(Aligned& al) {
    const Tmpl& T = tm();
    const int nF = al.frames;
    std::vector<std::array<cf32, 14>> Z((size_t)nF);
    for (int f = 0; f < nF; f++) {
        const cf32* s = al.sub(f, 5);
        for (int l = 3; l < 14; l++) {
            cf32 a(0, 0);
            const cf32* p = s + symUseful(l);
            for (int m = 0; m < kFft; m++) a += p[m] * T.c128[m];
            Z[(size_t)f][(size_t)l] = a;
        }
    }
    double total = 0;
    for (int lag : {1, 5, 10}) {
        cf32 acc(0, 0);
        double tsum = 0; int tn = 0;
        for (int f = 0; f < nF; f++)
            for (int l = 3; l + lag < 14; l++) {
                const double t0 = symUseful(l) / (double)kFs, t1 = symUseful(l + lag) / (double)kFs;
                const cf32 z0 = Z[(size_t)f][(size_t)l] * std::polar(1.f, (float)(-2 * M_PI * total * t0));
                const cf32 z1 = Z[(size_t)f][(size_t)(l + lag)] * std::polar(1.f, (float)(-2 * M_PI * total * t1));
                acc += (float)(npssCover(l) * npssCover(l + lag)) * z0 * std::conj(z1);
                tsum += t1 - t0; tn++;
            }
        if (!tn || std::abs(acc) <= 0) continue;
        total += -std::arg(acc) / (2 * M_PI * (tsum / tn));
    }
    return total;
}

// ---------------------------------------------------------------- NSSS
struct NsssTable {
    std::vector<cf32> d;   // [pci][mfi][132]... only the n-dependent part without theta: pci -> 132 values
    NsssTable() {
        d.resize(504 * 132);
        cf32 t[132];
        for (int pci = 0; pci < 504; pci++) { nsssSeq(pci, 0, t); for (int n = 0; n < 132; n++) d[(size_t)pci * 132 + (size_t)n] = t[n]; }
    }
};
const NsssTable& nsssTable() { static const NsssTable t; return t; }

struct NsssResult { int pci = -1, j0 = 0; float score = 0; };

NsssResult findNsss(const Aligned& al) {
    NsssResult r;
    const int nF = al.frames;
    const NsssTable& tb = nsssTable();
    std::vector<float> C((size_t)nF * 504 * 4, 0.f);
    static const cf32 rot[4] = {cf32(1, 0), cf32(0, 1), cf32(-1, 0), cf32(0, -1)};   // e^{+j 2 pi m / 4}
    cf32 Ym[4][132];
    for (int f = 0; f < nF; f++) {
        Grid g;
        demodSubframe(al.sub(f, 9), g);
        cf32 Y[132];
        double e = 0;
        for (int n = 0; n < 132; n++) { Y[n] = g.at(3 + n / 12, n % 12); e += std::norm(Y[n]); }
        if (e <= 0) continue;
        for (int m = 0; m < 4; m++) for (int n = 0; n < 132; n++) Ym[m][n] = Y[n] * rot[(m * n) % 4];
        for (int pci = 0; pci < 504; pci++) {
            const cf32* d = &tb.d[(size_t)pci * 132];
            for (int m = 0; m < 4; m++) {
                cf32 s(0, 0);
                for (int n = 0; n < 132; n++) s += Ym[m][n] * std::conj(d[n]);
                C[((size_t)f * 504 + (size_t)pci) * 4 + (size_t)m] = (float)(std::norm(s) / (132.0 * e));
            }
        }
    }
    // the score of (identity, n_f mod 8 of frame 0): the even frames' matches, the shift stepping every even frame
    std::vector<float> S(504 * 8, 0.f);
    double sum = 0;
    int best = -1;
    for (int pci = 0; pci < 504; pci++)
        for (int j0 = 0; j0 < 8; j0++) {
            double s = 0;
            for (int f = 0; f < nF; f++) if ((f + j0) % 2 == 0) s += C[((size_t)f * 504 + (size_t)pci) * 4 + (size_t)(((f + j0) / 2) % 4)];
            S[(size_t)pci * 8 + (size_t)j0] = (float)s;
            sum += s;
            if (best < 0 || s > S[(size_t)best]) best = pci * 8 + j0;
        }
    const double mean = (sum - S[(size_t)best]) / (504.0 * 8 - 1);
    r.pci = best / 8; r.j0 = best % 8; r.score = (float)(S[(size_t)best] / (mean + 1e-20));
    return r;
}

// ---------------------------------------------------------------- the broadcast channels
float dbOf(double p) { return (float)(10.0 * std::log10(std::max(p, 1e-20))); }

} // namespace

std::vector<Probe> probe(const cf32* x, size_t n, double fs, const std::vector<double>& offsetsHz, double maxCfoHz, float minScore) {
    std::vector<Probe> out;
    const size_t m = std::min(n, (size_t)(0.08 * fs));
    if (m < (size_t)(0.05 * fs)) return out;
    for (double off : offsetsHz) {
        const std::vector<cf32> y = toBase240(x, m, fs, off);
        const Det d = detect240(y, maxCfoHz);
        if (d.score >= minScore) out.push_back({off, d.score});
    }
    return out;
}

std::vector<CellResult> searchDecode(const cf32* x, size_t n, double fs, double offsetHz, const SearchOpts& o) {
    std::vector<CellResult> res;
    const float minNpss = o.minNpss > 0 ? o.minNpss : kMinNpssDefault;
    const std::vector<cf32> y240 = toBase240(x, n, fs, offsetHz);
    const Det d = detect240(y240, o.maxCfoHz);
    if (d.score < minNpss) return res;
    Aligned al;
    if (!align(y240, d, al)) return res;
    const double dcfo = refineCfo(al);
    cell::mixDown(al.buf.data(), al.buf.size(), dcfo, (double)kFs);
    al.cfoHz += dcfo;
    const NsssResult ns = findNsss(al);
    if (ns.pci < 0 || ns.score < kMinNsss) return res;
    CellResult cr;
    cr.found.pci = ns.pci;
    cr.found.cfoHz = al.cfoHz;
    cr.found.clockPpm = al.clockPpm;
    cr.found.npssScore = d.score;
    cr.found.nsssScore = ns.score;
    cr.found.nfMod8 = ns.j0;
    cr.frames = al.frames;
    const int pci = ns.pci, nF = al.frames, J0 = ns.j0;
    // ---- NPBCH: subframe 0 of every frame, both antenna port hypotheses
    const std::vector<std::pair<int, int>> pbRes = npbchRes(pci);
    std::vector<std::array<std::vector<float>, 2>> L((size_t)nF);   // LLRs per frame for 1 and 2 ports
    double rsrpAcc = 0, nvAcc = 0; int nm = 0;
    for (int f = 0; f < nF; f++) {
        Grid g;
        demodSubframe(al.sub(f, 0), g);
        for (int P = 1; P <= 2; P++) {
            Chan c;
            if (!estimateChannel(g, pci, 0, P, c)) continue;
            if (P == 1) { rsrpAcc += c.rsrp; nvAcc += c.nv; nm++; }
            demapQpsk(g, c, pbRes, P, L[(size_t)f][(size_t)(P - 1)]);
        }
    }
    if (nm) { cr.meas.levelDb = dbOf(rsrpAcc / nm * 12.0); cr.meas.snrDb = dbOf(rsrpAcc / std::max(nvAcc, 1e-20)); }
    const Bits scr = cell::gold((uint32_t)pci, 1600);
    // groups of 8 frames from a multiple of 80 ms
    std::vector<int> groups;
    for (int j = (8 - J0) % 8; j + 8 <= nF; j += 8) groups.push_back(j);
    std::vector<std::array<std::vector<float>, 2>> seg(groups.size());
    for (size_t g = 0; g < groups.size(); g++)
        for (int P = 0; P < 2; P++) {
            seg[g][(size_t)P].assign(200, 0.f);
            for (int t = 0; t < 8; t++) {
                const auto& v = L[(size_t)(groups[g] + t)][(size_t)P];
                if (v.size() == 200) for (int i = 0; i < 200; i++) seg[g][(size_t)P][(size_t)i] += v[(size_t)i];
            }
        }
    bool got = false;
    int mibP = 0, mibSfn0 = 0;
    for (int P = 1; P <= 2 && !got; P++)
        for (int i0 = 0; i0 < 8 && !got; i0++) {
            size_t g = 0;
            while (g < groups.size() && !got) {
                size_t g2 = g;
                std::vector<float> full(1600, 0.f);
                const int bFirst = (i0 + (int)g) % 8;
                while (g2 < groups.size()) {
                    const int b = (i0 + (int)g2) % 8;
                    if (g2 > g && b == 0) break;
                    for (int t = 0; t < 200; t++) {
                        const size_t k = (size_t)(200 * b + t);
                        full[k] += scr[k] ? -seg[g2][(size_t)(P - 1)][(size_t)t] : seg[g2][(size_t)(P - 1)][(size_t)t];
                    }
                    g2++;
                }
                const std::vector<float> d3 = cell::convRateRecover(full.data(), 1600, 50);
                Bits b = cell::viterbiTb(d3.data(), 50);
                if (cell::checkCrc(b, cell::kCrc16, 16, P == 2 ? 0xFFFFu : 0u)) {
                    b.resize(34);
                    Mib mb;
                    if (mibParse(b, mb)) {
                        // SFN of the first frame of this run: the 4 most significant bits and the position in the 640 ms period
                        const int sfnRun = mb.sfnMsb * 64 + 8 * bFirst;
                        const int sfn0 = ((sfnRun - groups[g]) % 1024 + 1024) % 1024;
                        if (sfn0 % 8 == J0) { cr.mib.ok = true; cr.mib.mib = mb; cr.mib.ports = P; cr.mib.sfn = sfn0; got = true; mibP = P; mibSfn0 = sfn0; }
                    }
                }
                g = g2;
            }
        }
    if (!got || !o.wantSib1) { res.push_back(cr); return res; }
    // ---- SIB1-NB: subframe 4 of every other frame of the repetitions
    const Mib& mib = cr.mib.mib;
    const int reps = sib1Reps(mib.sched), tbs = sib1Tbs(mib.sched);
    if (!reps || !tbs) { res.push_back(cr); return res; }
    const int step = 256 / reps, s0 = sib1StartFrame(reps, pci);
    std::vector<int> blocks;   // first frame of each repetition whose 8 subframes are all inside the capture
    for (int j = 0; j + 15 <= nF; j++) {
        const int pos = (mibSfn0 + j) % 256;
        for (int r = 0; r < reps; r++) if (pos == s0 + step * r) blocks.push_back(j);
    }
    if (blocks.empty()) { res.push_back(cr); return res; }
    const bool inband = mib.mode == kInbandSame || mib.mode == kInbandDiff;
    struct Hyp { int crsPorts, crsShift; };
    std::vector<Hyp> hyps;
    if (!inband) hyps.push_back({0, 0});
    else if (mib.mode == kInbandSame) hyps.push_back({mibP, pci % 6});
    else for (int sh = 0; sh < 6; sh++) hyps.push_back({mib.numCrs ? 4 : mibP, sh});
    // the subframes' grids and channels once
    std::vector<int> need;
    for (int j : blocks) for (int i = 0; i < 8; i++) need.push_back(j + 2 * i);
    std::sort(need.begin(), need.end());
    need.erase(std::unique(need.begin(), need.end()), need.end());
    std::vector<Grid> grids(need.size());
    std::vector<Chan> chans(need.size());
    std::vector<char> chOk(need.size(), 0);
    for (size_t k = 0; k < need.size(); k++) {
        demodSubframe(al.sub(need[k], 4), grids[k]);
        chOk[k] = estimateChannel(grids[k], pci, 4, mibP, chans[k]) ? 1 : 0;
    }
    auto indexOf = [&](int f) { return (size_t)(std::lower_bound(need.begin(), need.end(), f) - need.begin()); };
    for (const Hyp& h : hyps) {
        const std::vector<std::pair<int, int>> res4 = npdschRes(pci, mibP, inband, h.crsPorts, h.crsShift);
        const size_t nb = res4.size() * 2, E = nb * 8;
        std::vector<float> acc(E, 0.f);
        int nblk = 0;
        for (int j : blocks) {
            const uint32_t cinit = (0xFFFFu << 14) + (uint32_t)(((mibSfn0 + j) & 1) << 13) + (4u << 9) + (uint32_t)pci;
            const Bits c = cell::gold(cinit, E);
            bool all = true;
            std::vector<float> v;
            for (int i = 0; i < 8 && all; i++) {
                const size_t k = indexOf(j + 2 * i);
                if (!chOk[k]) { all = false; break; }
                const size_t before = v.size();
                demapQpsk(grids[k], chans[k], res4, mibP, v);
                if (v.size() - before != nb) all = false;
            }
            if (!all || v.size() != E) continue;
            for (size_t i = 0; i < E; i++) acc[i] += c[i] ? -v[i] : v[i];
            nblk++;
        }
        if (!nblk) continue;
        cr.sib1Tries++;
        const size_t K = (size_t)tbs + 24;
        const std::vector<float> d3 = cell::convRateRecover(acc.data(), E, K);
        Bits b = cell::viterbiTb(d3.data(), K);
        if (!cell::checkCrc(b, cell::kCrc24A, 24)) continue;
        b.resize((size_t)tbs);
        Sib1 s;
        if (sib1Parse(b, s)) { cr.sib1Ok = true; cr.sib1 = s; break; }
    }
    res.push_back(cr);
    return res;
}

} // namespace dect2::nbiot
