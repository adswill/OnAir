// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE: the synchronisation and broadcast helpers of the cell search: PSS correlation and tracking, SSS detection,
// the PBCH / MIB decode and combining, PCFICH and the reconstruction of known signals. cell_lte_search.cpp drives them.
#include "cell_lte_sync.h"
#include "dect2/cell/cell_dsp.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace dect2::cell {
using namespace lte_rx;

namespace lte_rx {

const std::array<std::vector<cf32>, 3>& pssTime() {
    static const std::array<std::vector<cf32>, 3> T = [] {
        std::array<std::vector<cf32>, 3> r;
        Fft f(kNfft6);
        for (int n2 = 0; n2 < 3; n2++) {
            cf32 d[62];
            ltePss(n2, d);
            std::vector<cf32> b((size_t)kNfft6, cf32(0, 0));
            for (int n = 0; n < 62; n++) b[(size_t)lteBin(n + 5, 6, kNfft6)] = d[n];
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

// SSS sequences: [nid1][subframe 0 / 5][62]
const std::vector<float>& sssTable() {
    static const std::vector<float> T = [] {
        std::vector<float> t((size_t)168 * 2 * 3 * 62);
        for (int n2 = 0; n2 < 3; n2++)
            for (int n1 = 0; n1 < 168; n1++)
                for (int w = 0; w < 2; w++) lteSss(n1, n2, w ? 5 : 0, &t[(((size_t)n2 * 168 + (size_t)n1) * 2 + (size_t)w) * 62]);
        return t;
    }();
    return T;
}
inline const float* sssSeq(int n2, int n1, int w) { return &sssTable()[(((size_t)n2 * 168 + (size_t)n1) * 2 + (size_t)w) * 62]; }

// cross-correlation c[t] = sum conj(tpl[i]) y[t + i], t = 0 .. n - L, by FFT blocks
void xcorr(const cf32* y, size_t n, const std::vector<cf32>& tpl, std::vector<cf32>& c) {
    const int L = (int)tpl.size(), N = 4096, step = N - L;
    c.assign(n >= (size_t)L ? n - (size_t)L + 1 : 0, cf32(0, 0));
    if (c.empty()) return;
    static thread_local std::unique_ptr<Fft> f;
    if (!f) f.reset(new Fft(N));
    std::vector<cf32> T((size_t)N, cf32(0, 0)), B((size_t)N);
    for (int i = 0; i < L; i++) T[(size_t)i] = tpl[(size_t)i];
    f->forward(T.data());
    for (auto& v : T) v = std::conj(v) / (float)N;
    for (size_t s = 0; s < c.size(); s += (size_t)step) {
        for (int i = 0; i < N; i++) B[(size_t)i] = (s + (size_t)i < n) ? y[s + (size_t)i] : cf32(0, 0);
        f->forward(B.data());
        for (int i = 0; i < N; i++) B[(size_t)i] *= T[(size_t)i];
        f->inverse(B.data());
        for (int t = 0; t <= step && s + (size_t)t < c.size(); t++) c[s + (size_t)t] = B[(size_t)t];
    }
}

cf32 dotConj(const cf32* y, const cf32* tpl, int L) {
    cf32 a(0, 0);
    for (int i = 0; i < L; i++) a += std::conj(tpl[i]) * y[i];
    return a;
}

std::vector<cf32> withCfo(const std::vector<cf32>& t, double hz, double fs) {
    std::vector<cf32> o(t.size());
    for (size_t i = 0; i < t.size(); i++) { const double a = 2 * M_PI * hz * (double)i / fs; o[i] = t[i] * cf32((float)std::cos(a), (float)std::sin(a)); }
    return o;
}


// PSS peaks: the folded correlation energy over the first half frames, for each NID2 and carrier offset hypothesis. Several peaks per NID2
// (cells of other sites arrive at other times), the strongest first; a peak blocks +-64 samples (33 us: its echoes) around it.
std::vector<Cand> pssSearch(const std::vector<cf32>& y, double maxCfo, float minDb, int halves, int perNid2) {
    std::vector<Cand> out;
    const size_t nUse = std::min(y.size(), (size_t)(halves * kHalf + kNfft6));
    if (nUse < (size_t)(kHalf + kNfft6)) return out;
    const double step = 7500;
    const int nh = std::max(0, (int)std::ceil(maxCfo / step - 0.34));
    std::vector<cf32> c;
    std::vector<std::vector<float>> folds;
    std::vector<double> means;
    for (int n2 = 0; n2 < 3; n2++) {
        folds.clear(); means.clear();
        for (int h = -nh; h <= nh; h++) {
            xcorr(y.data(), nUse, withCfo(pssTime()[(size_t)n2], h * step, kSyncRate), c);
            std::vector<float> fold((size_t)kHalf, 0.f);
            for (size_t t = 0; t < c.size(); t++) fold[t % kHalf] += std::norm(c[t]);
            double mean = 0;
            for (float v : fold) mean += v;
            means.push_back(std::max(1e-30, mean / kHalf));
            folds.push_back(std::move(fold));
        }
        for (int k = 0; k < perNid2; k++) {
            int bh = 0, bt = 0; float bdb = -100;
            for (size_t h = 0; h < folds.size(); h++) {
                const auto& f = folds[h];
                const int pk = (int)(std::max_element(f.begin(), f.end()) - f.begin());
                const float db = (float)(10 * std::log10(std::max(1e-30, (double)f[(size_t)pk]) / means[h]));
                if (db > bdb) { bdb = db; bh = (int)h; bt = pk; }
            }
            if (bdb < (k == 0 ? minDb : std::max(minDb, 12.0f))) break;   // a later peak must stand clear of the first one's sidelobes
            Cand cd; cd.nid2 = n2; cd.cfo = (bh - nh) * step; cd.t0 = bt; cd.db = bdb;
            out.push_back(cd);
            for (auto& f : folds) for (int d = -64; d <= 64; d++) f[(size_t)((bt + d + kHalf) % kHalf)] = 0;
        }
    }
    std::sort(out.begin(), out.end(), [](const Cand& a, const Cand& b) { return a.db > b.db; });
    return out;
}


// follow the PSS through the block: timing per half frame, clock (slope) and fine carrier offset
bool pssTrack(const std::vector<cf32>& y, const Cand& cd, Track& tr) {
    const auto& P = pssTime()[(size_t)cd.nid2];
    std::vector<cf32> tpl = withCfo(P, cd.cfo, kSyncRate);
    std::vector<double> ms, ts;
    std::vector<cf32> cs;
    cf32 split(0, 0);
    double pred = cd.t0, slope = kHalf;
    for (int m = 0;; m++) {
        const int c0 = (int)std::lround(pred);
        if (c0 - 12 < 0) { pred += slope; continue; }
        if ((size_t)(c0 + 12 + kNfft6) >= y.size()) break;
        int bi = 0; float bv = -1;
        float v[25];
        for (int d = -12; d <= 12; d++) { v[d + 12] = std::norm(dotConj(&y[(size_t)(c0 + d)], tpl.data(), kNfft6)); if (v[d + 12] > bv) { bv = v[d + 12]; bi = d; } }
        double frac = 0;
        if (bi > -12 && bi < 12) { const float a = v[bi + 11], b = v[bi + 12], cc = v[bi + 13]; const float den = a - 2 * b + cc; if (den < 0) frac = 0.5 * (a - cc) / den; }
        const int at = c0 + bi;
        const cf32 c1 = dotConj(&y[(size_t)at], tpl.data(), 64), c2 = dotConj(&y[(size_t)at + 64], tpl.data() + 64, 64);
        split += c2 * std::conj(c1);
        ms.push_back(m); ts.push_back(at + frac); cs.push_back(c1 + c2);
        // the expected place of the next one, from the fit so far
        if (ms.size() >= 2) {
            double sm = 0, st = 0, smm = 0, smt = 0; const double k = (double)ms.size();
            for (size_t i = 0; i < ms.size(); i++) { sm += ms[i]; st += ts[i]; smm += ms[i] * ms[i]; smt += ms[i] * ts[i]; }
            const double den = k * smm - sm * sm;
            if (den > 0) { slope = (k * smt - sm * st) / den; slope = std::max(kHalf * (1 - 1e-4), std::min(kHalf * (1 + 1e-4), slope)); }
            pred = ts.back() + slope;
        } else pred = at + frac + slope;
    }
    if (ms.size() < 2) return false;
    double sm = 0, st = 0, smm = 0, smt = 0; const double k = (double)ms.size();
    for (size_t i = 0; i < ms.size(); i++) { sm += ms[i]; st += ts[i]; smm += ms[i] * ms[i]; smt += ms[i] * ts[i]; }
    const double den = k * smm - sm * sm;
    tr.slope = den > 0 ? (k * smt - sm * st) / den : kHalf;
    if (std::fabs(tr.slope / kHalf - 1) > 1e-4) tr.slope = kHalf;
    tr.t0 = (st - tr.slope * sm) / k;
    tr.nHalf = (int)ms.back() + 1;
    tr.t.resize((size_t)tr.nHalf);
    for (int m = 0; m < tr.nHalf; m++) tr.t[(size_t)m] = tr.t0 + tr.slope * m;
    // fine offset: the phase across the two halves of each PSS, then between PSS 5 ms apart (unambiguous to +-100 Hz)
    double cfo = cd.cfo + std::arg(split) / (2 * M_PI * 64 / kSyncRate);
    std::vector<cf32> t2 = withCfo(P, cfo, kSyncRate);
    cf32 acc(0, 0), prev(0, 0);
    for (int m = 0; m < tr.nHalf; m++) {
        const long at = std::lround(tr.t[(size_t)m]);
        if (at < 0 || (size_t)(at + kNfft6) > y.size()) { prev = cf32(0, 0); continue; }
        const cf32 c = dotConj(&y[(size_t)at], t2.data(), kNfft6);
        acc += c * std::conj(prev);
        prev = c;
    }
    const double dt = tr.slope / kSyncRate;
    if (std::abs(acc) > 0) cfo += std::arg(acc) / (2 * M_PI * dt);
    tr.cfo = cfo;
    return true;
}


void fft128(const cf32* x, std::vector<cf32>& out) {
    static thread_local std::unique_ptr<Fft> f;
    if (!f) f.reset(new Fft(kNfft6));
    out.assign(x, x + kNfft6);
    f->forward(out.data());
}


// +-6 subcarrier moving sum (the channel hardly changes over 90 kHz; noise and other cells' sequences average out)
inline void smooth62(const cf32* in, cf32* out) {
    for (int n = 0; n < 62; n++) {
        cf32 h(0, 0);
        for (int k = std::max(0, n - 6); k <= std::min(61, n + 6); k++) h += in[k];
        out[n] = h / (float)(std::min(61, n + 6) - std::max(0, n - 6) + 1);
    }
}

// the cell identity groups heard behind one PSS timing, the most significant first: time-synchronised cells of the same NID2 share the
// PSS. The known cells at this timing are cancelled first (their sequences and channels, estimated from the received symbols).
std::vector<SssResult> sssDetect(const std::vector<cf32>& ys, int nid2, const Track& tr, const std::vector<KnownSync>& known) {
    std::vector<SssResult> all;
    struct Hyp { bool ext, tdd; int off; };
    const Hyp hyps[4] = {{false, false, 128 + 9}, {true, false, 128 + 32}, {false, true, 412}, {true, true, 480}};
    cf32 pss[62];
    ltePss(nid2, pss);
    SssResult best;
    std::vector<cf32> P, S;
    for (const Hyp& h : hyps) {
        std::vector<double> acc((size_t)168 * 2, 0.0);
        double norm = 0;
        int used = 0;
        for (int m = 0; m < tr.nHalf; m++) {
            const long p = std::lround(tr.t[(size_t)m]), s = p - h.off;
            if (s < 0 || (size_t)(p + kNfft6) > ys.size()) continue;
            fft128(&ys[(size_t)p], P);
            fft128(&ys[(size_t)s], S);
            cf32 Pv[62], Sv[62], tmp[62], hs[62], hSame[62];
            for (int n = 0; n < 62; n++) { const int b = lteBin(n + 5, 6, kNfft6); Pv[n] = P[(size_t)b]; Sv[n] = S[(size_t)b]; hSame[n] = cf32(0, 0); }
            for (const KnownSync& k : known) {
                if ((size_t)m >= k.t.size() || std::fabs(k.t[(size_t)m] - tr.t[(size_t)m]) > 2.5 || k.off != h.off) continue;
                const float* d = sssSeq(k.nid2, k.nid1, (m % 2) == k.parity ? 0 : 1);
                for (int n = 0; n < 62; n++) tmp[n] = Sv[n] * d[n];
                smooth62(tmp, hs);
                for (int n = 0; n < 62; n++) Sv[n] -= hs[n] * d[n];
                if (k.nid2 == nid2) {   // the same PSS: it carries both channels; take the known cell's (from its SSS) out of the estimate below
                    for (int n = 0; n < 62; n++) hSame[n] += hs[n];
                    continue;
                }
                cf32 kp[62];
                ltePss(k.nid2, kp);
                for (int n = 0; n < 62; n++) tmp[n] = Pv[n] * std::conj(kp[n]);
                smooth62(tmp, hs);
                for (int n = 0; n < 62; n++) Pv[n] -= hs[n] * kp[n];
            }
            for (int n = 0; n < 62; n++) tmp[n] = Pv[n] * std::conj(pss[n]);
            smooth62(tmp, hs);
            for (int n = 0; n < 62; n++) hs[n] -= hSame[n];
            float z[62];
            double e = 0;
            for (int n = 0; n < 62; n++) {
                z[n] = (Sv[n] * std::conj(hs[n])).real();
                e += (double)z[n] * z[n];
            }
            norm += std::sqrt(e * 62);
            used++;
            for (int n1 = 0; n1 < 168; n1++) {
                const float* d0 = sssSeq(nid2, n1, 0);
                const float* d5 = sssSeq(nid2, n1, 1);
                double c0 = 0, c5 = 0;
                for (int n = 0; n < 62; n++) { c0 += z[n] * d0[n]; c5 += z[n] * d5[n]; }
                if (m % 2 == 0) { acc[(size_t)n1 * 2] += c0; acc[(size_t)n1 * 2 + 1] += c5; }
                else { acc[(size_t)n1 * 2] += c5; acc[(size_t)n1 * 2 + 1] += c0; }
            }
        }
        if (!used || norm <= 0) continue;
        // significance: the score against the spread of all 336 hypotheses (noise gives about 1, the largest of 1344 about 3.5; a strong
        // cell's own sequence cross-correlations raise the spread, so the cells under it stand out once it is cancelled)
        double ms = 0;
        for (double v : acc) ms += v * v;
        const double rms = std::sqrt(ms / (double)acc.size());
        for (int i = 0; i < 168 * 2; i++) {
            const float sc = (float)(acc[(size_t)i] / norm), sg = rms > 0 ? (float)(acc[(size_t)i] / rms) : 0.f;
            bool isKnown = false;
            for (const KnownSync& k : known) if (k.nid2 == nid2 && k.nid1 == i / 2) isKnown = true;
            if (!isKnown && (sc > 0.25f || sg >= kSssSigExtra)) { SssResult r; r.ok = true; r.score = sc; r.sig = sg; r.nid1 = i / 2; r.sf0Parity = i % 2; r.ext = h.ext; r.tdd = h.tdd; all.push_back(r); }
            if (sc > best.score) { best.score = sc; best.sig = sg; best.nid1 = i / 2; best.sf0Parity = i % 2; best.ext = h.ext; best.tdd = h.tdd; }
        }
    }
    std::sort(all.begin(), all.end(), [](const SssResult& a, const SssResult& b) { return a.sig > b.sig; });
    std::vector<SssResult> out;
    for (const auto& r : all) {
        if (out.size() >= 3 || (!out.empty() && r.sig < kSssSigExtra)) break;
        bool dup = false;
        for (const auto& o : out) if (o.nid1 == r.nid1) dup = true;
        if (!dup) out.push_back(r);
    }
    if (out.empty()) { best.ok = false; out.push_back(best); }
    return out;
}

std::vector<std::pair<int, int>> pbchRes(int pci, bool ext) {
    const int N = lteSymbPerSlot(ext);
    std::vector<std::pair<int, int>> res;
    for (int l = N; l < N + 4; l++)
        for (int kk = 0; kk < 72; kk++) {
            const bool rs = (l - N <= 1) || (ext && l - N == 3);
            if (rs && (kk % 3) == (pci % 6) % 3) continue;
            res.push_back({l, kk});
        }
    return res;
}

// gather the received values and the per-port channel of a list of REs, and undo transmit diversity
void txdRes(const LteGrid& y, const LteChan& ch, const std::vector<std::pair<int, int>>& res, int ports, std::vector<float>& llr) {
    std::vector<cf32> r(res.size());
    std::vector<std::vector<cf32>> h((size_t)ports, std::vector<cf32>(res.size()));
    for (size_t i = 0; i < res.size(); i++) {
        r[i] = y.at(res[i].first, res[i].second);
        for (int p = 0; p < ports; p++) h[(size_t)p][i] = ch.h[(size_t)p].at(res[i].first, res[i].second);
    }
    std::vector<const cf32*> hp;
    for (auto& v : h) hp.push_back(v.data());
    llr.clear();
    lteTxdDecode(r.data(), hp, r.size(), ports, ch.noiseVar, llr);
}

uint32_t mibMask(int ports) { return ports == 1 ? 0u : ports == 2 ? 0xFFFFu : 0x5555u; }

// soft agreement of a decoded word, re-encoded, with the received values (1 = all agree, about 0 = noise)
float agreement(const float* e, const Bits& enc) {
    double s = 0, a = 0;
    for (size_t i = 0; i < enc.size(); i++) { s += e[i] * (enc[i] ? -1.0 : 1.0); a += std::fabs(e[i]); }
    return a > 0 ? (float)(s / a) : 0.f;
}


MibTry mibDecode(const std::vector<std::array<std::vector<float>, 3>>& llr, int pci, bool ext) {
    MibTry out;
    const int M = ltePbchBits(ext), per = M / 4;
    const Bits cs = gold((uint32_t)pci, (size_t)M);
    static const int portsOf[3] = {1, 2, 4};
    const int nf = (int)llr.size();
    std::vector<float> e((size_t)M);
    // every 40 ms period (4 frames, the frame of segment j0 first) and its single frames; a 16-bit CRC alone passes noise now and then, so
    // a MIB counts when two decodes from different frames agree (the same set-up, the SFN advancing with the frames)
    std::vector<MibTry> got;
    std::vector<std::pair<int, int>> used;   // frames of each decode
    for (int span = 1; span <= 4; span++)
        for (int f0 = 0; f0 < nf; f0++)
            for (int j0 = 0; j0 + span <= 4; j0++) {
                if (f0 + span > nf) continue;
                for (int pi = 0; pi < 3; pi++) {
                    bool have = true;
                    for (int q = 0; q < span; q++) if (llr[(size_t)(f0 + q)][(size_t)pi].size() < (size_t)per) have = false;
                    if (!have) continue;
                    std::fill(e.begin(), e.end(), 0.f);
                    for (int q = 0; q < span; q++) {
                        const int seg = j0 + q;
                        const auto& L = llr[(size_t)(f0 + q)][(size_t)pi];
                        for (int i = 0; i < per; i++) e[(size_t)(seg * per + i)] = cs[(size_t)(seg * per + i)] ? -L[(size_t)i] : L[(size_t)i];
                    }
                    const std::vector<float> d = convRateRecover(e.data(), (size_t)M, 40);
                    const Bits b = viterbiTb(d.data(), 40);
                    uint32_t par = 0;
                    for (int i = 24; i < 40; i++) par = (par << 1) | b[(size_t)i];
                    if ((crc(b.data(), 24, kCrc16, 16) ^ par) != mibMask(portsOf[pi])) continue;
                    const Bits enc = convRateMatch(convEncode(b), (size_t)M);
                    double s = 0, a = 0;
                    for (int q = 0; q < span; q++) for (int i = 0; i < per; i++) { const size_t k = (size_t)((j0 + q) * per + i); s += e[k] * (enc[k] ? -1.0 : 1.0); a += std::fabs(e[k]); }
                    if (a <= 0 || s / a < 0.2) continue;
                    int v = 0;
                    for (int i = 0; i < 3; i++) v = (v << 1) | b[(size_t)i];
                    if (v > 5) continue;
                    MibTry t;
                    t.ok = true;
                    t.mib.ok = true;
                    t.mib.nRb = lteBwIndexToRb(v);
                    t.mib.phichDur = b[3];
                    t.mib.phichRes = (b[4] << 1) | b[5];
                    int s8 = 0;
                    for (int i = 6; i < 14; i++) s8 = (s8 << 1) | b[(size_t)i];
                    t.mib.sfn = (s8 * 4 + j0) & 1023;
                    t.mib.ports = portsOf[pi];
                    for (int i = 14; i < 24; i++) t.mib.mibTail = (t.mib.mibTail << 1) | b[(size_t)i];
                    t.frame = f0;
                    for (size_t k = 0; k < got.size(); k++) {
                        const MibTry& o = got[k];
                        const bool apart = f0 >= used[k].second || f0 + span <= used[k].first;
                        if (apart && o.mib.nRb == t.mib.nRb && o.mib.ports == t.mib.ports && o.mib.phichDur == t.mib.phichDur && o.mib.phichRes == t.mib.phichRes && o.mib.mibTail == t.mib.mibTail &&
                            ((o.mib.sfn - o.frame - (t.mib.sfn - t.frame)) & 1023) == 0)
                            return o;
                    }
                    got.push_back(t);
                    used.push_back({f0, f0 + span});
                }
            }
    return out;
}

// The MIB over every frame of the block at once: the 40 ms periods carry the same MIB but for the SFN, which advances by one each period.
// The code is linear, so period P's soft bits are turned into period 0's by flipping them where the code word of the SFN difference
// (s0 + P) xor s0 has a one; each start s0 (with the frame's place in its period) is a hypothesis, and only a decode whose own SFN gives the
// same differences counts (with the CRC, the spare bits at zero and a valid bandwidth: about 34 bits checked, noise does not pass).
const Bits& sfnDeltaWord(int d, int M) {
    static thread_local std::vector<Bits> cache[2];
    std::vector<Bits>& c = cache[M == 1920 ? 0 : 1];
    if (c.empty()) {
        c.resize(256);
        for (int v = 0; v < 256; v++) {
            Bits b(24, 0);
            for (int i = 0; i < 8; i++) b[(size_t)(6 + i)] = (uint8_t)((v >> (7 - i)) & 1);
            appendCrc(b, kCrc16, 16, 0);
            c[(size_t)v] = convRateMatch(convEncode(b), (size_t)M);
        }
    }
    return c[(size_t)(d & 255)];
}

MibTry mibCombine(const std::vector<std::array<std::vector<float>, 3>>& llr, int pci, bool ext) {
    MibTry out;
    const int M = ltePbchBits(ext), per = M / 4;
    const int nf = (int)llr.size();
    if (nf < 2) return out;
    const Bits cs = gold((uint32_t)pci, (size_t)M);
    static const int portsOf[3] = {1, 2, 4};
    std::vector<float> e((size_t)M);
    float bestAg = 0;
    for (int a = 0; a < 4; a++) {
        const int nP = (nf - 1 + a) / 4 + 1;
        // the hypotheses s0 grouped by the differences they give (decoded once per group)
        std::vector<std::vector<int>> keys;
        std::vector<std::vector<int>> members;
        for (int s0 = 0; s0 < 256; s0++) {
            std::vector<int> k((size_t)nP);
            for (int P = 0; P < nP; P++) k[(size_t)P] = ((s0 + P) & 255) ^ s0;
            size_t i = 0;
            while (i < keys.size() && keys[i] != k) i++;
            if (i == keys.size()) { keys.push_back(k); members.emplace_back(); }
            members[i].push_back(s0);
        }
        for (int pi = 0; pi < 3; pi++)
            for (size_t gi = 0; gi < keys.size(); gi++) {
                std::fill(e.begin(), e.end(), 0.f);
                int used = 0;
                for (int f = 0; f < nf; f++) {
                    const auto& L = llr[(size_t)f][(size_t)pi];
                    if (L.size() < (size_t)per) continue;
                    used++;
                    const int seg = (f + a) % 4, P = (f + a) / 4;
                    const Bits& D = sfnDeltaWord(keys[gi][(size_t)P], M);
                    for (int i = 0; i < per; i++) {
                        const size_t k = (size_t)(seg * per + i);
                        const float v = (cs[k] ^ D[k]) ? -L[(size_t)i] : L[(size_t)i];
                        e[k] += v;
                    }
                }
                if (used < 2) continue;
                const std::vector<float> d = convRateRecover(e.data(), (size_t)M, 40);
                const Bits b = viterbiTb(d.data(), 40);
                uint32_t par = 0;
                for (int i = 24; i < 40; i++) par = (par << 1) | b[(size_t)i];
                if ((crc(b.data(), 24, kCrc16, 16) ^ par) != mibMask(portsOf[pi])) continue;
                bool spare = true;   // bits 14 - 19 are the LTE-M fields (schedulingInfoSIB1-BR, systemInfoUnchanged-BR), 20 - 23 spare
                for (int i = 20; i < 24; i++) if (b[(size_t)i]) spare = false;
                int tail = 0;
                for (int i = 14; i < 24; i++) tail = (tail << 1) | b[(size_t)i];
                if (lteMibSib1Br(tail) > 18) spare = false;   // reserved values: not a MIB
                if (!spare) continue;
                int v = 0;
                for (int i = 0; i < 3; i++) v = (v << 1) | b[(size_t)i];
                if (v > 5) continue;
                int s8 = 0;
                for (int i = 6; i < 14; i++) s8 = (s8 << 1) | b[(size_t)i];
                if (std::find(members[gi].begin(), members[gi].end(), s8) == members[gi].end()) continue;
                const float ag = agreement(e.data(), convRateMatch(convEncode(b), (size_t)M));
                if (ag < 0.05f || ag <= bestAg) continue;
                bestAg = ag;
                out.ok = true;
                out.mib.ok = true;
                out.mib.nRb = lteBwIndexToRb(v);
                out.mib.phichDur = b[3];
                out.mib.phichRes = (b[4] << 1) | b[5];
                out.mib.sfn = (s8 * 4 + a) & 1023;
                out.mib.ports = portsOf[pi];
                out.mib.mibTail = tail;
                out.frame = 0;
            }
    }
    return out;
}



int pcfichDecode(const LteGrid& g, const LteChan& ch, const LteCellCfg& c, int sf) {
    const LteControl c1 = lteControl(c, 1, sf);
    std::vector<std::pair<int, int>> res;
    for (const auto& rg : c1.pcfich) for (int i = 0; i < 4; i++) res.push_back({rg.l, rg.k[i]});
    std::vector<float> L;
    txdRes(g, ch, res, c.ports, L);
    if (L.size() < 32) return 0;
    const Bits pcs = gold((uint32_t)((sf + 1) * (2 * c.pci + 1) * 512 + c.pci), 32);
    int cfi = 1; float bc = -1e30f;
    for (int k = 0; k < 3; k++) {
        float s = 0;
        for (int i = 0; i < 32; i++) s += (pcs[(size_t)i] ? -L[(size_t)i] : L[(size_t)i]) * (kCfiCode[k][i] ? -1.f : 1.f);
        if (s > bc) { bc = s; cfi = k + 1; }
    }
    return cfi;
}

// the known transmissions of a decoded cell in one subframe of the SI stage: its SIB1 PDSCH (subframe 5 of even frames) and its decoded DCIs
void knownParts(const Decoded& d, const LteCellCfg& c, int fr, int sf, int sfn, int cfi, std::vector<LteTxPart>& parts) {
    parts.clear();
    auto qp = [](const Bits& b) { std::vector<cf32> v; for (size_t i = 0; i + 1 < b.size(); i += 2) v.push_back(qpsk(b[i], b[i + 1])); return v; };
    if (sf == 5 && sfn % 2 == 0 && d.haveSib1 && !d.sib1Tb.empty() && cfi > 0) {
        std::vector<int> p0, p1;
        ltePrbs(d.sib1, c.nRb, p0, p1);
        bool ok = !p0.empty();
        for (int v : p0) if (v < 0 || v >= c.nRb) ok = false;
        for (int v : p1) if (v < 0 || v >= c.nRb) ok = false;
        Bits tb = d.sib1Tb;
        appendCrc(tb, kCrc24A, 24);
        const int B = (int)tb.size(), K = turboSizeFor(B), F = K - B;
        if (ok && K > 0) {
            LteTxPart pt;
            pt.res = ltePdschRes(c, sf, cfi, p0, p1);
            Bits cb((size_t)F, 0); cb.insert(cb.end(), tb.begin(), tb.end());
            const int NL = c.ports > 1 ? 2 : 1;
            const size_t G = pt.res.size() * 2, E = (size_t)(NL * 2) * (G / (size_t)(NL * 2));
            const int k = (sfn / 2) % 4, rv = ((3 * k + 1) / 2) % 4;
            Bits e = turboRateMatch(turboEncode(cb), K, F, E, rv);
            const Bits cs = gold(((uint32_t)kSiRnti << 14) + ((uint32_t)sf << 9) + (uint32_t)c.pci, e.size());
            for (size_t i = 0; i < e.size(); i++) e[i] ^= cs[i];
            pt.d = qp(e);
            parts.push_back(std::move(pt));
        }
    }
    for (const auto& ev : d.dcis) {
        if (ev.fr != fr || ev.sf != sf) continue;
        const LteControl ctl = lteControl(c, ev.cfi, sf);
        const size_t tot = ctl.pdcchQuad.size() * 8, off = (size_t)ev.cce * 72;
        if (off + ev.coded.size() > tot) continue;
        const Bits cs = gold((uint32_t)(sf * 512 + c.pci), tot);
        Bits e(ev.coded);
        for (size_t i = 0; i < e.size(); i++) e[i] ^= cs[off + i];
        const std::vector<cf32> sym = qp(e);
        for (size_t q = 0; q * 4 + 3 < sym.size(); q++) {
            const LteReg& rg = ctl.pdcchQuad[off / 8 + q];
            LteTxPart pt;
            for (int i = 0; i < 4; i++) pt.res.push_back({rg.l, rg.k[i]});
            pt.d.assign(sym.begin() + (long)(4 * q), sym.begin() + (long)(4 * q + 4));
            parts.push_back(std::move(pt));
        }
    }
}

LteCellCfg cfgOf(const Decoded& d, int nRbGrid) {
    LteCellCfg c;
    c.pci = d.pci; c.extCp = d.ext; c.tdd = false; c.nRb = nRbGrid; c.ports = d.ports; c.phichDur = d.phichDur; c.phichRes = d.phichRes; c.mibTail = d.mibTail;
    return c;
}

void addCrs(std::vector<LteGrid>& port, const LteCellCfg& c, int sf) {
    const int N = lteSymbPerSlot(c.extCp), nsym = 2 * N, nsc = 12 * c.nRb;
    cf32 r[2 * kMaxRb];
    for (int p = 0; p < c.ports; p++) for (int lsf = 0; lsf < nsym; lsf++) {
        const int l = lsf % N, ns = 2 * sf + lsf / N;
        const bool has = (p <= 1) ? (l == 0 || l == N - 3) : (l == 1);
        if (!has) continue;
        lteCrsSeq(c.pci, ns, l, c.extCp, r);
        for (int k = 0; k < nsc; k++) if (lteCrsAt(c, sf, lsf, k, p)) port[(size_t)p].at(lsf, k) = r[k / 6 + kMaxRb - c.nRb];
    }
}


}   // namespace lte_rx

} // namespace dect2::cell
