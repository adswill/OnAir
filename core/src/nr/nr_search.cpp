// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells: SSB search and PBCH decoding in a block of samples (nr_phy.h searchDecode).
//
// One candidate SS_REF and one subcarrier spacing at a time: the block is brought down to 128 x SCS for the PSS search (three N_ID2, a few
// carrier offset hypotheses, correlation energy folded over the 20 ms SSB period), then each PSS peak is taken at 256 x SCS: fine timing
// and carrier offset from the PSS, N_ID1 from the SSS (equalised by the PSS), the SSB index from the PBCH DMRS, the channel from the DMRS,
// and the PBCH polar decoded (CRC24C) into the MIB. SS-RSRP, SS-RSRQ and SS-SINR come from the SSS and the DMRS.
#include "dect2/cell/cell_dsp.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/nr/nr_phy.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace dect2::nr {

namespace {

constexpr int kNd = 128;        // FFT size of the detection rate
constexpr int kNs = 256;        // FFT size of the SSB rate
constexpr int kFft = 4096;      // correlation block
constexpr double kPeriod = 0.020;
constexpr int kDelta = 4;       // the FFT window starts this far into the cyclic prefix (SSB rate samples)

// PSS useful part at 128 x SCS, unit energy, and its spectrum over a kFft block (conjugated, / kFft)
struct PssTpl { std::vector<cf32> t; std::vector<cf32> spec; };
const std::array<PssTpl, 3>& pssTpl() {
    static const std::array<PssTpl, 3> T = [] {
        std::array<PssTpl, 3> r;
        Fft f(kNd), g(kFft);
        for (int n2 = 0; n2 < 3; n2++) {
            float d[127];
            pss(n2, d);
            std::vector<cf32> b((size_t)kNd, cf32(0, 0));
            for (int n = 0; n < 127; n++) b[(size_t)((n - 64 + kNd) % kNd)] = d[n];   // k = 56 + n, bin k - 120
            f.inverse(b.data());
            float e = 0;
            for (auto& v : b) e += std::norm(v);
            for (auto& v : b) v /= std::sqrt(e);
            r[(size_t)n2].t = b;
            std::vector<cf32> s((size_t)kFft, cf32(0, 0));
            for (int i = 0; i < kNd; i++) s[(size_t)i] = b[(size_t)i];
            g.forward(s.data());
            for (auto& v : s) v = std::conj(v) / (float)kFft;
            r[(size_t)n2].spec = s;
        }
        return r;
    }();
    return T;
}

// PSS useful part at 256 x SCS (unit energy)
const std::array<std::vector<cf32>, 3>& pssTplS() {
    static const std::array<std::vector<cf32>, 3> T = [] {
        std::array<std::vector<cf32>, 3> r;
        Fft f(kNs);
        for (int n2 = 0; n2 < 3; n2++) {
            float d[127];
            pss(n2, d);
            std::vector<cf32> b((size_t)kNs, cf32(0, 0));
            for (int n = 0; n < 127; n++) b[(size_t)((n - 64 + kNs) % kNs)] = d[n];
            f.inverse(b.data());
            float e = 0;
            for (auto& v : b) e += std::norm(v);
            for (auto& v : b) v /= std::sqrt(e);
            r[(size_t)n2] = b;
        }
        return r;
    }();
    return T;
}

const std::vector<float>& sssTable() {   // [nid2][nid1][127]
    static const std::vector<float> T = [] {
        std::vector<float> t((size_t)3 * 336 * 127);
        for (int n2 = 0; n2 < 3; n2++) for (int n1 = 0; n1 < 336; n1++) sss(n1, n2, &t[((size_t)n2 * 336 + (size_t)n1) * 127]);
        return t;
    }();
    return T;
}

struct Peak { int nid2 = 0; double cfo = 0; long t = 0; float db = 0; };

// PSS peaks of the folded correlation energy: per N_ID2 up to `per` peaks (the beams of a burst, other cells), strongest first
std::vector<Peak> pssPeaks(const std::vector<cf32>& y, double fsD, double scs, double maxCfo, float minDb, int per) {
    std::vector<Peak> out;
    const long P = std::lround(kPeriod * fsD);
    if ((long)y.size() < kNd * 4) return out;
    const int shift = 16;   // a carrier offset of scs / 2 is 16 bins of the kFft block
    const int nh = std::max(0, (int)std::ceil(maxCfo / (scs / 2) - 0.3));
    const int step = kFft - kNd;
    const size_t nc = y.size() - (size_t)kNd + 1;
    static thread_local std::unique_ptr<Fft> f;
    if (!f) f.reset(new Fft(kFft));
    const int nhyp = 2 * nh + 1;
    std::vector<std::vector<float>> fold((size_t)(3 * nhyp), std::vector<float>((size_t)std::min<long>(P, (long)nc), 0.f));
    const size_t FL = fold[0].size();
    std::vector<cf32> B((size_t)kFft), C((size_t)kFft);
    for (size_t s = 0; s < nc; s += (size_t)step) {
        for (int i = 0; i < kFft; i++) B[(size_t)i] = s + (size_t)i < y.size() ? y[s + (size_t)i] : cf32(0, 0);
        f->forward(B.data());
        for (int n2 = 0; n2 < 3; n2++) {
            const auto& T = pssTpl()[(size_t)n2].spec;
            for (int h = -nh; h <= nh; h++) {
                // template at carrier offset h * scs / 2: its spectrum moved up by h * 16 bins
                const int sh = h * shift;
                for (int i = 0; i < kFft; i++) C[(size_t)i] = B[(size_t)i] * T[(size_t)((i - sh + 4 * kFft) % kFft)];
                f->inverse(C.data());
                auto& fo = fold[(size_t)(n2 * nhyp + h + nh)];
                for (int t = 0; t <= step && s + (size_t)t < nc; t++) fo[(s + (size_t)t) % FL] += std::norm(C[(size_t)t]);
            }
        }
    }
    for (int n2 = 0; n2 < 3; n2++) {
        std::vector<double> mean((size_t)nhyp);
        for (int h = 0; h < nhyp; h++) {
            double m = 0;
            for (float v : fold[(size_t)(n2 * nhyp + h)]) m += v;
            mean[(size_t)h] = std::max(1e-30, m / (double)FL);
        }
        for (int k = 0; k < per; k++) {
            int bh = 0; long bt = 0; float bdb = -100;
            for (int h = 0; h < nhyp; h++) {
                const auto& fo = fold[(size_t)(n2 * nhyp + h)];
                const long pk = (long)(std::max_element(fo.begin(), fo.end()) - fo.begin());
                const float db = (float)(10 * std::log10(std::max(1e-30, (double)fo[(size_t)pk]) / mean[(size_t)h]));
                if (db > bdb) { bdb = db; bh = h; bt = pk; }
            }
            if (bdb < minDb) break;
            Peak p; p.nid2 = n2; p.cfo = (bh - nh) * scs / 2; p.t = bt; p.db = bdb;
            out.push_back(p);
            for (int h = 0; h < nhyp; h++) {
                auto& fo = fold[(size_t)(n2 * nhyp + h)];
                for (long d = -kNd; d <= kNd; d++) fo[(size_t)((bt + d + (long)FL * 4) % (long)FL)] = 0;
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.db > b.db; });
    return out;
}

cf32 dotConj(const cf32* y, const cf32* t, int L) {
    cf32 a(0, 0);
    for (int i = 0; i < L; i++) a += std::conj(t[i]) * y[i];
    return a;
}

// One SSB taken out of the input: the four symbols' subcarriers 0..239 (power normalised: per RE)
struct Ssb { cf32 Y[4][240]; bool ok = false; };

struct Ctx {
    const cf32* x; size_t n; double fs, off, scs, fsS;
    Numerology nm;
};

// the samples at 256 x SCS starting `from` (input sample position), mixed down by off + cfo
std::vector<cf32> take(const Ctx& c, double from, double cfo, size_t len) {
    return cell::resampleBlock(c.x, c.n, c.fs, c.fsS, c.off + cfo, std::min(c.fsS, 248 * c.scs), from, len);
}

Ssb ssbAt(const std::vector<cf32>& w, long u0) {   // u0: PSS useful start within w
    Ssb s;
    static thread_local std::unique_ptr<Fft> f;
    if (!f) f.reset(new Fft(kNs));
    std::vector<cf32> b((size_t)kNs);
    for (int l = 0; l < 4; l++) {
        const long st = u0 + l * (kNs + 18) - kDelta;
        if (st < 0 || (size_t)(st + kNs) > w.size()) return s;
        for (int i = 0; i < kNs; i++) b[(size_t)i] = w[(size_t)(st + i)];
        f->forward(b.data());
        for (int k = 0; k < 240; k++) {
            const int kb = k - 120, bi = (kb + kNs) % kNs;
            const double a = 2 * M_PI * kb * kDelta / kNs;
            s.Y[l][k] = b[(size_t)bi] * cf32((float)std::cos(a), (float)std::sin(a)) / (float)kNs;
        }
    }
    s.ok = true;
    return s;
}

struct Chan { cf32 H[4][240]; float noise = 1e-12f; float dmrsP = 0; };

// channel from the DMRS of one SSB index hypothesis: smoothed over neighbouring DMRS, interpolated along k; the noise from the residual
Chan dmrsChannel(const Ssb& s, int pci, int issb) {
    Chan ch;
    cf32 r[144];
    pbchDmrs(pci, issb, r);
    const std::vector<Re> re = pbchDmrsRes(pci);
    std::vector<cf32> z(144), zs(144);
    for (int j = 0; j < 144; j++) z[(size_t)j] = s.Y[re[(size_t)j].l][re[(size_t)j].k] * std::conj(r[j]);
    double res = 0, pw = 0; int nres = 0;
    for (int j = 0; j < 144; j++) {
        cf32 a(0, 0); int c = 0;
        for (int d = -2; d <= 2; d++) {
            const int q = j + d;
            if (q < 0 || q >= 144 || re[(size_t)q].l != re[(size_t)j].l || std::abs(re[(size_t)q].k - re[(size_t)j].k) != 4 * std::abs(d)) continue;
            a += z[(size_t)q]; c++;
        }
        zs[(size_t)j] = a / (float)c;
        if (c == 5) { res += std::norm(z[(size_t)j] - zs[(size_t)j]); nres++; }
        pw += std::norm(zs[(size_t)j]);
    }
    ch.noise = (float)std::max(1e-14, nres ? res / nres * 5.0 / 4.0 : 1e-12);
    ch.dmrsP = (float)(pw / 144);
    // interpolate along k per symbol
    for (int l = 1; l <= 3; l++) {
        std::vector<int> ks; std::vector<cf32> hs;
        for (int j = 0; j < 144; j++) if (re[(size_t)j].l == l) { ks.push_back(re[(size_t)j].k); hs.push_back(zs[(size_t)j]); }
        for (int k = 0; k < 240; k++) {
            size_t i = 0;
            while (i + 1 < ks.size() && ks[i + 1] <= k) i++;
            if (k <= ks[0]) { ch.H[l][k] = hs[0]; continue; }
            if (i + 1 >= ks.size() || ks[i + 1] - ks[i] > 4) { ch.H[l][k] = hs[i]; continue; }   // past the last one, or the gap of symbol 2
            const float a = (float)(k - ks[i]) / (float)(ks[i + 1] - ks[i]);
            ch.H[l][k] = hs[i] * (1 - a) + hs[i + 1] * a;
        }
    }
    return ch;
}

float dmrsMetric(const Ssb& s, int pci, int issb) {
    cf32 r[144];
    pbchDmrs(pci, issb, r);
    const std::vector<Re> re = pbchDmrsRes(pci);
    float m = 0;
    for (int l = 1; l <= 3; l++) {
        cf32 acc(0, 0);
        for (int j = 0; j + 1 < 144; j++) {
            if (re[(size_t)j].l != l || re[(size_t)(j + 1)].l != l || re[(size_t)(j + 1)].k - re[(size_t)j].k != 4) continue;
            const cf32 a = s.Y[l][re[(size_t)j].k] * std::conj(r[j]), b = s.Y[l][re[(size_t)(j + 1)].k] * std::conj(r[j + 1]);
            acc += b * std::conj(a);
        }
        m += std::abs(acc);
    }
    return m;
}

} // namespace

std::vector<CellResult> searchDecode(const cf32* x, size_t n, double fs, double offsetHz, const SearchOpts& o) {
    std::vector<CellResult> out;
    const double scs = o.scsKhz * 1e3, fsD = kNd * scs, fsS = kNs * scs;
    if (fs < fsS * 0.97 || n < (size_t)(fs * 0.012)) return out;
    Ctx c{x, n, fs, offsetHz, scs, fsS, Numerology{}};
    c.nm.scsKhz = o.scsKhz; c.nm.nfft = kNs;
    const size_t nD = (size_t)std::floor((double)n * fsD / fs);
    const std::vector<cf32> y = cell::resampleBlock(x, n, fs, fsD, offsetHz, std::min(fsD, 130 * scs), 0, nD);
    const std::vector<Peak> peaks = pssPeaks(y, fsD, scs, o.maxCfoHz, o.minPssDb, 10);
    const long P = std::lround(kPeriod * fsD);
    const double toIn = fs / fsD;
    const size_t wlen = 4 * (kNs + 18) + 2 * 40;
    int tried = 0;
    struct Anchor { std::vector<double> pss; double cfo = 0, noise = 1; bool ok = false; };   // where the decoded SSB of each result sits
    std::vector<Anchor> anchors;
    for (const Peak& pk : peaks) {
        if (tried++ >= o.maxCells * 4) break;
        // the PSS of this peak in every period of the block
        std::vector<double> at;   // useful start of the PSS, input samples
        for (long t = pk.t; t + 4 * (kNd + 9) + kNd < (long)nD; t += P) at.push_back((double)t * toIn);
        if (at.empty()) continue;
        const auto& tS = pssTplS()[(size_t)pk.nid2];
        // fine timing and offset at the SSB rate
        double cfo = pk.cfo;
        std::vector<long> u0s(at.size(), 40);
        std::vector<std::vector<cf32>> ws(at.size());
        for (int pass = 0; pass < 2; pass++) {
            cf32 split(0, 0);
            for (size_t m = 0; m < at.size(); m++) {
                ws[m] = take(c, at[m] - 40 * fs / fsS, cfo, wlen);
                const std::vector<cf32>& w = ws[m];
                long best = 40; float bv = -1;
                for (long d = -6; d <= 6; d++) {
                    const long u = 40 + d;
                    if (u < 0 || (size_t)(u + kNs) > w.size()) continue;
                    const float v = std::norm(dotConj(&w[(size_t)u], tS.data(), kNs));
                    if (v > bv) { bv = v; best = u; }
                }
                u0s[m] = best;
                const cf32 c1 = dotConj(&w[(size_t)best], tS.data(), kNs / 2), c2 = dotConj(&w[(size_t)best + kNs / 2], tS.data() + kNs / 2, kNs / 2);
                split += c2 * std::conj(c1);
            }
            if (std::abs(split) > 0) cfo += std::arg(split) / (2 * M_PI * (kNs / 2) / fsS);
        }
        // the SSBs, and the SSS
        std::vector<Ssb> ss;
        for (size_t m = 0; m < at.size(); m++) { Ssb s = ssbAt(ws[m], u0s[m]); if (s.ok) ss.push_back(s); }
        if (ss.empty()) continue;
        float dP[127];
        pss(pk.nid2, dP);
        std::vector<float> score(336, 0.f);
        for (const Ssb& s : ss) {
            cf32 e[127];
            for (int i = 0; i < 127; i++) e[i] = s.Y[2][56 + i] * std::conj(s.Y[0][56 + i] * dP[i]);
            for (int n1 = 0; n1 < 336; n1++) {
                const float* d = &sssTable()[((size_t)pk.nid2 * 336 + (size_t)n1) * 127];
                cf32 a(0, 0);
                for (int i = 0; i < 127; i++) a += e[i] * d[i];
                score[(size_t)n1] += std::abs(a);
            }
        }
        const int nid1 = (int)(std::max_element(score.begin(), score.end()) - score.begin());
        double mu = 0, sd = 0;
        for (int i = 0; i < 336; i++) if (i != nid1) mu += score[(size_t)i];
        mu /= 335;
        for (int i = 0; i < 336; i++) if (i != nid1) sd += (score[(size_t)i] - mu) * (score[(size_t)i] - mu);
        sd = std::sqrt(sd / 334) + 1e-30;
        const float sig = (float)((score[(size_t)nid1] - mu) / sd);
        if (sig < 4.5f) continue;
        const int pci = 3 * nid1 + pk.nid2;
        // the SSB index from the DMRS, then the PBCH of each SSB until one decodes
        CellResult r;
        r.pci = pci; r.nid1 = nid1; r.nid2 = pk.nid2; r.scsKhz = o.scsKhz; r.cfoHz = cfo; r.pssDb = pk.db; r.sssSig = sig;
        std::array<float, 8> dm{};
        for (const Ssb& s : ss) for (int i = 0; i < 8; i++) dm[(size_t)i] += dmrsMetric(s, pci, i);
        const int issb = (int)(std::max_element(dm.begin(), dm.end()) - dm.begin());
        r.issbBar = issb;
        float dmNext = 0;
        for (int i = 0; i < 8; i++) if (i != issb) dmNext = std::max(dmNext, dm[(size_t)i]);
        const bool issbSure = dm[(size_t)issb] > 1.6f * dmNext;   // the DMRS alone tells the SSB index of a beam whose PBCH does not decode
        float dS[127];
        sss(nid1, pk.nid2, dS);
        // the residual carrier offset from the phase two symbols later: the SSS against the PSS, and the DMRS of symbol 3 against symbol 1
        // (unambiguous to +-SCS / 4)
        {
            cf32 acc(0, 0), rr[144];
            pbchDmrs(pci, issb, rr);
            const std::vector<Re> mre = pbchDmrsRes(pci);
            for (const Ssb& s : ss) {
                for (int i = 0; i < 127; i++) acc += s.Y[2][56 + i] * dS[i] * std::conj(s.Y[0][56 + i] * dP[i]);
                for (int j = 0; j < 60; j++) {   // symbol 1 holds DMRS 0..59, symbol 3 holds 84..143, at the same subcarriers
                    const cf32 z1 = s.Y[1][mre[(size_t)j].k] * std::conj(rr[j]), z3 = s.Y[3][mre[(size_t)(84 + j)].k] * std::conj(rr[84 + j]);
                    acc += z3 * std::conj(z1);
                }
            }
            if (std::abs(acc) > 0) r.cfoHz += std::arg(acc) / (2 * M_PI * 2 * (kNs + 18) / fsS);
        }
        double rsrp = 0, rssi = 0, noise = 0;
        const std::vector<Re> dre = pbchDataRes(pci);
        for (const Ssb& s : ss) {
            const Chan ch = dmrsChannel(s, pci, issb);
            double ps = 0, tot = 0;
            for (int i = 0; i < 127; i++) ps += std::norm(s.Y[2][56 + i]);
            for (int l = 0; l < 4; l++) for (int k = 0; k < 240; k++) tot += std::norm(s.Y[l][k]);
            rsrp += std::max(1e-15, ps / 127 - ch.noise);
            rssi += tot / 4;
            noise += ch.noise;
            if (r.hasMib && r.pbchTried >= 2) continue;
            std::vector<float> llr(864);
            for (size_t j = 0; j < dre.size(); j++) {
                const cf32 z = s.Y[dre[j].l][dre[j].k] * std::conj(ch.H[dre[j].l][dre[j].k]);
                llr[2 * j] = z.real() / ch.noise; llr[2 * j + 1] = z.imag() / ch.noise;
            }
            Mib m;
            r.pbchTried++;
            if (!r.hasMib && pbchDecode(llr.data(), pci, issb, m)) { r.hasMib = true; r.mib = m; r.pbchOk++; }
            else if (r.hasMib && pbchDecode(llr.data(), pci, issb, m)) r.pbchOk++;
        }
        const double k = (double)ss.size();
        rsrp /= k; rssi /= k; noise /= k;
        r.rsrpDb = (float)(10 * std::log10(rsrp));
        r.rsrqDb = (float)(10 * std::log10(20 * rsrp / std::max(1e-15, rssi)));
        r.sinrDb = (float)(10 * std::log10(rsrp / std::max(1e-15, noise)));
        Anchor an;
        if (r.hasMib) {
            an.ok = true; an.cfo = cfo; an.noise = noise;
            for (size_t m = 0; m < at.size(); m++) an.pss.push_back(at[m] + (double)(u0s[m] - 40) * fs / fsS);
        }
        if (r.hasMib) r.beams.push_back({r.mib.ssbIdx, r.rsrpDb});
        else if (issbSure) r.beams.push_back({-1 - issb, r.rsrpDb});   // DMRS index only: made an SSB index once the cell's Lmax is known
        // another peak of the same cell (another beam) joins it
        bool merged = false;
        for (size_t qi = 0; qi < out.size(); qi++) {
            auto& q = out[qi];
            if (q.pci != r.pci) continue;
            merged = true;
            if (an.ok && (!anchors[qi].ok || r.rsrpDb > q.rsrpDb)) anchors[qi] = an;
            if (!r.beams.empty()) {
                bool dup = false;
                for (const auto& b : q.beams) if (b.ssbIdx == r.beams[0].ssbIdx) dup = true;
                if (!dup) q.beams.push_back(r.beams[0]);
            }
            if (r.rsrpDb > q.rsrpDb && (r.hasMib || !q.hasMib)) { std::vector<Beam> b = q.beams; const bool had = q.hasMib; const Mib qm = q.mib; q = r; q.beams = b; if (!r.hasMib && had) { q.hasMib = true; q.mib = qm; } }
            else if (!q.hasMib && r.hasMib) { q.hasMib = true; q.mib = r.mib; q.issbBar = r.issbBar; }
            break;
        }
        if (!merged && (r.hasMib || sig >= 6.0f)) { out.push_back(r); anchors.push_back(an); }   // PSS / SSS alone must stand well clear of the other identities
        if ((int)out.size() >= o.maxCells) break;
    }
    // the other SSBs of every decoded cell, at the places the burst gives them: their PSS and SSS power, coherent over 16 subcarriers and
    // averaged over the periods (a beam far too weak for its own PSS peak is still measured)
    for (size_t qi = 0; qi < out.size(); qi++) {
        auto& q = out[qi];
        const Anchor& an = anchors[qi];
        if (!q.hasMib || !an.ok) continue;
        const int lm = q.mib.lmax == 4 && q.issbBar >= 4 ? 4 : 8;
        const int i0 = q.mib.ssbIdx;
        const int l0 = ssbFirstSymbol(i0, lm);
        if (l0 < 0) continue;
        float dP[127], dS[127];
        pss(q.nid2, dP); sss(q.nid1, q.nid2, dS);
        const auto& tS = pssTplS()[(size_t)q.nid2];
        for (int j = 0; j < lm; j++) {
            if (j == i0) continue;
            bool have = false;
            for (const auto& b : q.beams) if (b.ssbIdx == j) have = true;
            if (have) continue;
            const int lj = ssbFirstSymbol(j, lm);
            const double dj = (double)((c.nm.symStart(lj) + c.nm.cpLen(lj)) - (c.nm.symStart(l0) + c.nm.cpLen(l0))) * fs / fsS;
            double pw = 0; int np = 0;
            for (double t0 : an.pss) {
                const std::vector<cf32> w = take(c, t0 + dj - 40 * fs / fsS, an.cfo, wlen);
                long best = 40; float bv = -1;
                for (long d = -2; d <= 2; d++) { const float v = std::norm(dotConj(&w[(size_t)(40 + d)], tS.data(), kNs)); if (v > bv) { bv = v; best = 40 + d; } }
                const Ssb s = ssbAt(w, best);
                if (!s.ok) continue;
                for (int b0 = 0; b0 < 127; b0 += 16) {
                    const int len = std::min(16, 127 - b0);
                    cf32 a(0, 0), e(0, 0);
                    for (int i = b0; i < b0 + len; i++) { a += s.Y[0][56 + i] * dP[i]; e += s.Y[2][56 + i] * dS[i]; }
                    pw += (std::norm(a) + std::norm(e)) / ((double)len * len) - an.noise / len * 2;
                    np += 2;
                }
            }
            if (!np) continue;
            pw /= np;
            if (pw > an.noise * 0.125) q.beams.push_back({j, (float)(10 * std::log10(pw))});   // SS-SINR above -9 dB: noise alone stays near -20 dB
        }
    }
    for (auto& q : out) {
        // beams known from their DMRS index only: the SSB index under the cell's Lmax (dropped when no MIB gave it)
        std::vector<Beam> bs;
        for (const auto& b : q.beams) {
            Beam v = b;
            if (v.ssbIdx < 0) { if (!q.hasMib) continue; const int ib = -1 - v.ssbIdx; v.ssbIdx = q.mib.lmax == 4 ? (ib & 3) : ib; }
            bool dup = false;
            for (auto& o2 : bs) if (o2.ssbIdx == v.ssbIdx) { o2.rsrpDb = std::max(o2.rsrpDb, v.rsrpDb); dup = true; }
            if (!dup) bs.push_back(v);
        }
        q.beams = bs;
        std::sort(q.beams.begin(), q.beams.end(), [](const Beam& a, const Beam& b) { return a.ssbIdx < b.ssbIdx; });
    }
    return out;
}

} // namespace dect2::nr
