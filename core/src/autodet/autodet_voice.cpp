// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Digital voice auto-detection (autodet.h): which land-mobile system a narrowband signal is, from its frame syncs.
//
// Each signal is brought to baseband twice (48 kHz class for 2400 / 4800 / 6000 Bd, 144 kHz class for TETRA's 18000 Bd) and turned into
// one soft level per symbol: the phase advance over one symbol period, from the unwrapped FM discriminator. For 4FSK that is 2 pi f T (four
// levels), for pi/4-DQPSK and H-DQPSK it is the phase step itself (+-1, +-3 in units of pi/4), for GMSK two levels. The symbol timing is the
// phase (of eight per symbol) that maximises the level energy, chosen per block of 256 symbols so a clock error does not smear it. The
// levels are normalised per block (mean removed: a frequency offset; the mean magnitude set to 2, i.e. outer levels at +-3).
// Then the frame syncs are searched with a symbol-error count: a hit has at most L/8 wrong symbols (outer pattern symbols must be clearly
// outer, inner ones clearly inner, NaN = no signal counts as wrong), both polarities. Short syncs (M17 8, NXDN 10, TETRA 11, dPMR 12
// symbols) count only when another hit is a whole number of frames away.
#include "autodet_internal.h"
#include "dect2/dmr/dmr_proto.h"
#include "dect2/dpmr/dpmr_proto.h"
#include "dect2/dstar/dstar_proto.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/fusion/fusion_proto.h"
#include "dect2/m17/m17_proto.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/p25p2/p25p2_proto.h"
#include "dect2/tetra/tetra_phy.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

namespace dect2 {
namespace autodet {

using namespace detail;

namespace {

constexpr int kPhases = 8;      // timing phases tried per symbol
constexpr int kBlock = 256;     // symbols per timing / normalisation block

// The soft levels at one symbol rate: NaN where the signal is off (a TDMA gap)
struct Levels {
    std::vector<float> s;       // normalised, outer levels at about +-3 (4-level) / +-1 (2-level)
    double unitRad = 0;         // the radian phase advance of level 1 (median over blocks)
    double eye = 0;             // best-phase energy over the mean over phases: > 1.1 a real symbol clock
    double mid = 0;             // share of the levels between 1.5 and 2.5: high for a two-level signal read as four levels
};

// windowed-sinc low-pass (Blackman), cutoff cutHz on each side: the channel filter of the symbol rate tried
std::vector<cf32> lowpass(const std::vector<cf32>& z, double rate, double cutHz) {
    if (z.empty() || cutHz <= 0 || cutHz >= rate * 0.45) return z;
    const double fc = cutHz / rate;
    const int taps = std::min(511, std::max(31, (int)(6.0 / fc) | 1));
    std::vector<float> h((size_t)taps);
    double hs = 0;
    for (int i = 0; i < taps; i++) {
        const double t = i - (taps - 1) / 2.0;
        const double sn = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
        h[(size_t)i] = (float)(sn * w); hs += sn * w;
    }
    for (float& v : h) v = (float)(v / hs);
    std::vector<cf32> y(z.size(), cf32(0, 0));
    const size_t H = (size_t)taps / 2;
    for (size_t i = H; i + H < z.size(); i++) {
        float re = 0, im = 0;
        const cf32* p = z.data() + i - H;
        for (int j = 0; j < taps; j++) { re += p[j].real() * h[(size_t)j]; im += p[j].imag() * h[(size_t)j]; }
        y[i] = cf32(re, im);
    }
    return y;
}

// rate: the baseband's rate; z the samples; baud the symbol rate; twoLevel: GMSK (mean magnitude -> 1)
Levels levels(const std::vector<cf32>& z, double rate, double baud, bool twoLevel) {
    Levels L;
    const size_t n = z.size();
    if (n < 64 || baud <= 0) return L;
    const double T = rate / baud;   // samples per symbol, any real number
    // unwrapped phase (double) and power
    std::vector<double> ph(n), pw(n);
    ph[0] = 0; pw[0] = std::norm(z[0]);
    for (size_t i = 1; i < n; i++) {
        ph[i] = ph[i - 1] + std::arg(std::complex<double>(z[i]) * std::conj(std::complex<double>(z[i - 1])));
        pw[i] = std::norm(z[i]);
    }
    std::vector<double> cpw(n + 1, 0);
    for (size_t i = 0; i < n; i++) cpw[i + 1] = cpw[i] + pw[i];
    auto at = [&](double t) { const size_t i = (size_t)t; if (i + 1 >= n) return ph[n - 1]; const double f = t - (double)i; return ph[i] * (1 - f) + ph[i + 1] * f; };
    const size_t nsym = (size_t)(((double)n - 2 * T - 2) / T);
    if (nsym < 64) return L;
    // power gate: the 90th percentile of the per-symbol power is "on"
    std::vector<double> sp(nsym);
    for (size_t k = 0; k < nsym; k++) { const size_t a = (size_t)(k * T), b = std::min(n, (size_t)((k + 1) * T) + 1); sp[k] = (cpw[b] - cpw[a]) / std::max<size_t>(1, b - a); }
    const double on = quantile(sp, 0.9);
    L.s.assign(nsym, NAN);
    double eyeNum = 0, eyeDen = 0;
    std::vector<double> units;
    for (size_t b0 = 0; b0 < nsym; b0 += kBlock) {
        const size_t b1 = std::min(nsym, b0 + kBlock);
        double best = -1; int bp = 0; double meanE = 0;
        std::vector<double> e(kPhases, 0);
        size_t valid = 0;
        for (size_t k = b0; k < b1; k++) if (sp[k] >= 0.15 * on) valid++;
        if (valid < (b1 - b0) / 4) continue;
        for (int p = 0; p < kPhases; p++) {
            const double off = T * p / kPhases;
            double s2 = 0, s1 = 0; size_t c = 0;
            for (size_t k = b0; k < b1; k++) {
                if (sp[k] < 0.15 * on) continue;
                const double t = (double)k * T + off;
                const double y = at(t + T) - at(t);
                s1 += y; s2 += y * y; c++;
            }
            const double m = c ? s1 / c : 0;
            e[(size_t)p] = c ? s2 / c - m * m : 0;
            meanE += e[(size_t)p] / kPhases;
            if (e[(size_t)p] > best) { best = e[(size_t)p]; bp = p; }
        }
        eyeNum += best * (double)valid; eyeDen += meanE * (double)valid;
        const double off = T * bp / kPhases;
        std::vector<double> y;
        y.reserve(b1 - b0);
        double s1 = 0; size_t c = 0;
        for (size_t k = b0; k < b1; k++) {
            const double t = (double)k * T + off;
            const double v = at(t + T) - at(t);
            y.push_back(v);
            if (sp[k] >= 0.15 * on) { s1 += v; c++; }
        }
        const double dc = c ? s1 / c : 0;
        double a1 = 0;
        for (size_t k = b0; k < b1; k++) if (sp[k] >= 0.15 * on) a1 += std::fabs(y[k - b0] - dc);
        a1 = c ? a1 / c : 0;
        const double unit = twoLevel ? a1 : a1 / 2;
        if (unit <= 1e-9) continue;
        units.push_back(unit);
        for (size_t k = b0; k < b1; k++) if (sp[k] >= 0.15 * on) L.s[k] = (float)((y[k - b0] - dc) / unit);
    }
    L.unitRad = units.empty() ? 0 : quantile(units, 0.5);
    L.eye = eyeDen > 0 ? eyeNum / eyeDen : 0;
    size_t nm = 0, nv = 0;
    for (float v : L.s) if (!std::isnan(v)) { nv++; const float a = std::fabs(v); nm += a > 1.5f && a < 2.5f; }
    L.mid = nv ? (double)nm / (double)nv : 0;
    return L;
}

// a sync pattern: levels +-1 / +-3 (4-level) or +-1 (2-level)
struct Pattern { int sys; const char* what; std::vector<int> p; bool twoLevel; int period; };   // period > 0: a hit must have a partner k * period away

// strict (the short syncs): hard decisions at 0 and +-2; otherwise outer pattern symbols must be clearly outer and inner ones clearly inner
int symErr(const std::vector<float>& s, size_t i, const std::vector<int>& p, bool twoLevel, float pol, bool strict) {
    const float lo = strict ? 2.f : 1.3f, hi = strict ? 2.f : 2.7f;
    int e = 0;
    for (size_t j = 0; j < p.size(); j++) {
        const float v = s[i + j] * pol;
        if (std::isnan(v)) { e++; continue; }
        const int q = p[j];
        if (twoLevel) { if ((v > 0) != (q > 0)) e++; continue; }
        if ((v > 0) != (q > 0)) { e++; continue; }
        const float a = std::fabs(v);
        if (std::abs(q) == 3 ? a < lo : a > hi) e++;
    }
    return e;
}

struct Hits { int n = 0; int pol = 1; std::vector<int> errs; };

// hits of one pattern in a level stream, the better polarity
Hits search(const std::vector<float>& s, const Pattern& pt) {
    Hits best;
    const size_t L = pt.p.size();
    if (s.size() < L + 1) return best;
    const bool strict = L < 16;   // a short sync is only believed when it is exact and repeats at the frame period
    const int maxErr = strict ? (L >= 11 ? 1 : 0) : (int)L / 8;
    for (int pol = 0; pol < 2; pol++) {
        const float sg = pol ? -1.f : 1.f;
        std::vector<long> at; std::vector<int> er;
        for (size_t i = 0; i + L <= s.size(); i++) {
            const int e = symErr(s, i, pt.p, pt.twoLevel, sg, strict);
            if (e > maxErr) continue;
            if (!at.empty() && (long)i - at.back() < (long)L) { if (e < er.back()) { at.back() = (long)i; er.back() = e; } continue; }
            at.push_back((long)i); er.push_back(e);
        }
        Hits h; h.pol = pol ? -1 : 1;
        if (pt.period > 0) {
            for (size_t a = 0; a < at.size(); a++) {
                bool ok = false;
                for (size_t b = 0; b < at.size() && !ok; b++) {
                    if (a == b) continue;
                    const long d = std::labs(at[a] - at[b]);
                    const long q = (d + pt.period / 2) / pt.period;
                    ok = q >= 1 && q <= 16 && std::labs(d - q * pt.period) <= (strict ? 1 : 2);
                }
                if (ok) { h.n++; h.errs.push_back(er[a]); }
            }
        } else { h.n = (int)at.size(); h.errs = er; }
        if (h.n > best.n) best = h;
    }
    return best;
}

std::vector<int> fromInt8(const int8_t* p, int n) { return std::vector<int>(p, p + n); }

// the sync patterns per symbol rate
struct RateSet { double baud; bool fast; bool twoLevel; double cutHz; std::vector<Pattern> pats; };   // cutHz: the low-pass in front (one side)

const std::vector<RateSet>& rateSets() {
    static const std::vector<RateSet> sets = [] {
        std::vector<RateSet> v;
        RateSet r4{4800, false, false, 5500, {}};
        for (int k = 0; k < dmr::kSyncCount; k++) { int8_t p[24]; dmr::syncSymbols(k, p); r4.pats.push_back({VoiceDmr, "DMR sync", fromInt8(p, 24), false, 0}); }
        r4.pats.push_back({VoiceP25, "P25 frame sync", std::vector<int>(p25::kSync, p25::kSync + p25::kSyncSym), false, 0});
        r4.pats.push_back({VoiceNxdn96, "NXDN frame sync word", std::vector<int>(nxdn::kFsw, nxdn::kFsw + nxdn::kFswSym), false, nxdn::kFrameSym});
        { int8_t p[fusion::kSyncDibits]; fusion::syncLevels(p); r4.pats.push_back({VoiceYsf, "System Fusion frame sync", fromInt8(p, fusion::kSyncDibits), false, 0}); }
        for (uint16_t w : {m17::kSyncLsf, m17::kSyncStream, m17::kSyncPacket, m17::kSyncBert}) {
            int8_t p[8]; m17::syncSymbols(w, p);
            r4.pats.push_back({VoiceM17, "M17 sync burst", fromInt8(p, 8), false, m17::kFrameSyms});
        }
        v.push_back(r4);
        RateSet r2{2400, false, false, 3000, {}};
        r2.pats.push_back({VoiceNxdn48, "NXDN frame sync word", std::vector<int>(nxdn::kFsw, nxdn::kFsw + nxdn::kFswSym), false, nxdn::kFrameSym});
        for (const int* p : {dpmr::kFs1, dpmr::kFs3, dpmr::kFs4}) r2.pats.push_back({VoiceDpmr, "dPMR frame sync", std::vector<int>(p, p + dpmr::kFsLong), false, 0});
        r2.pats.push_back({VoiceDpmr, "dPMR frame sync 2", std::vector<int>(dpmr::kFs2, dpmr::kFs2 + dpmr::kFsShort), false, 2 * dpmr::kFrameSym});
        v.push_back(r2);
        RateSet rd{4800, false, true, 4000, {}};
        rd.pats.push_back({VoiceDstar, "D-STAR data sync", std::vector<int>(), true, dstar::kSuperframe * 96});
        for (int i = 0; i < 24; i++) rd.pats.back().p.push_back(dstar::kDataSync[i] ? 1 : -1);
        v.push_back(rd);
        RateSet r6{6000, false, false, 4200, {}};
        {
            std::vector<int> p;
            for (int i = 0; i < p25p2::kIschDibits; i++) p.push_back(p25p2::dibitStep((int)((p25p2::kSyncWord >> (38 - 2 * i)) & 3)));
            r6.pats.push_back({VoiceP25p2, "P25 Phase 2 sync", p, false, 0});
        }
        v.push_back(r6);
        RateSet rt{18000, true, false, 13000, {}};
        auto dib = [](const uint8_t* b, int bits) { std::vector<int> p; for (int i = 0; i + 1 < bits; i += 2) p.push_back(tetra::dibitPhase(b[i], b[i + 1])); return p; };
        rt.pats.push_back({VoiceTetra, "TETRA synchronisation training sequence", dib(tetra::kTrainY, 38), false, 0});
        rt.pats.push_back({VoiceTetra, "TETRA normal training sequence", dib(tetra::kTrainN, 22), false, tetra::kSlotSyms});
        rt.pats.push_back({VoiceTetra, "TETRA normal training sequence 2", dib(tetra::kTrainP, 22), false, tetra::kSlotSyms});
        v.push_back(rt);
        return v;
    }();
    return sets;
}

struct SysEvidence { int hits = 0; double weight = 0; int pol = 1; double baud = 0; double unitRad = 0; std::vector<int> errs; std::string what; };

int medianInt(std::vector<int> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end()); return v[v.size() / 2]; }

// The sync hits at which a system counts as found: the sparse ones (a sync every 180 / 360 / 420 ms) need fewer
int needHits(int sys) { return sys == VoiceP25 || sys == VoiceP25p2 || sys == VoiceDstar ? 2 : 3; }

VoiceSignal decide(const std::vector<cf32>& slow, double slowRate, const std::vector<cf32>& fast, double fastRate, double bwHz) {
    VoiceSignal v;
    v.bwHz = bwHz;
    SysEvidence ev[VoiceCount];
    double bestEye = 0, bestEyeBaud = 0, eyeUnit = 0;
    for (const RateSet& rs : rateSets()) {
        const std::vector<cf32>& z = rs.fast ? fast : slow;
        const double r = rs.fast ? fastRate : slowRate;
        if (z.empty() || r < rs.baud * 4) continue;
        const Levels L = levels(lowpass(z, r, rs.cutHz), r, rs.baud, rs.twoLevel);
        if (L.s.empty()) continue;
        if (!rs.twoLevel && L.eye > bestEye) { bestEye = L.eye; bestEyeBaud = rs.baud; eyeUnit = L.unitRad; }
        for (const Pattern& pt : rs.pats) {
            // a two-level signal (GMSK) read as four levels sits at +-2: the outer-only syncs (DMR, P25) would match half of its symbols
            if (!pt.twoLevel && L.mid > 0.5) continue;
            const Hits h = search(L.s, pt);
            SysEvidence& e = ev[pt.sys];
            if (h.n <= 0) continue;
            // syncs of one system add up (DMR's voice and data syncs); the polarity of the most hits wins
            if (h.n > e.hits) { e.pol = h.pol; e.what = pt.what; }
            e.hits += h.n; e.baud = rs.baud; e.unitRad = L.unitRad;
            e.weight += h.n * (double)pt.p.size() * (pt.twoLevel ? 0.5 : 1.0);   // the evidence of a hit grows with the sync's length
            e.errs.insert(e.errs.end(), h.errs.begin(), h.errs.end());
        }
    }
    // NXDN48 and dPMR share 2400 Bd, NXDN96 and the 4800 Bd systems share their rate: the syncs decide
    int best = VoiceUnknown, second = VoiceUnknown;
    for (int s = 1; s < VoiceCount; s++) {
        if (ev[s].hits <= 0) continue;
        if (best == VoiceUnknown || ev[s].weight > ev[best].weight) { second = best; best = s; }
        else if (second == VoiceUnknown || ev[s].weight > ev[second].weight) second = s;
    }
    char buf[256];
    if (best != VoiceUnknown && ev[best].hits >= 1) {
        const SysEvidence& e = ev[best];
        v.sys = best; v.baud = e.baud; v.syncHits = e.hits; v.syncErr = medianInt(e.errs); v.inverted = e.pol < 0;
        const bool fsk = best != VoiceP25p2 && best != VoiceTetra && best != VoiceDstar;
        v.devHz = fsk ? 3 * e.unitRad * e.baud / (2 * M_PI) : 0;
        v.runnerUp = second;
        const int need = needHits(best);
        const int h2 = second != VoiceUnknown ? ev[second].hits : 0;
        const double w2 = second != VoiceUnknown ? ev[second].weight : 0;
        if (e.hits >= need && w2 * 4 <= e.weight) { v.verdict = VerdictSure; v.confidence = e.hits >= 2 * need && w2 * 10 <= e.weight ? 0.99 : 0.95; }
        else if (e.hits >= need || (e.hits >= 2 && w2 * 2 <= e.weight)) { v.verdict = VerdictLikely; v.confidence = 0.8; }
        else { v.verdict = VerdictUnsure; v.confidence = 0.45; }
        if (w2 > 0 && v.verdict != VerdictSure) v.confidence = std::min(v.confidence, e.weight / (e.weight + w2));
        snprintf(buf, sizeof buf, "%s found %d time%s (median %d symbol error%s), %.0f Bd", e.what.c_str(), e.hits, e.hits == 1 ? "" : "s", v.syncErr,
                 v.syncErr == 1 ? "" : "s", e.baud);
        v.why = buf;
        if (v.devHz > 0) { snprintf(buf, sizeof buf, ", outer deviation +-%.2f kHz", v.devHz / 1e3); v.why += buf; }
        if (v.inverted) v.why += ", inverted";
        if (second != VoiceUnknown) { snprintf(buf, sizeof buf, "; %s sync %d time%s", voiceSysName(second), h2, h2 == 1 ? "" : "s"); v.why += buf; }
        return v;
    }
    // no sync at all: a digital signal with a symbol clock, or an analogue FM voice channel
    double envVar = 0;
    {
        const std::vector<cf32> nb = lowpass(slow, slowRate, 6500);
        double m1 = 0, m2 = 0; size_t c = 0;
        for (size_t i = 64; i + 64 < nb.size(); i += 4) { const double p = std::norm(nb[i]); m1 += p; m2 += p * p; c++; }
        if (c && m1 > 0) { m1 /= c; m2 /= c; envVar = m2 / (m1 * m1) - 1; }
    }
    if (bestEye >= 1.25) {
        v.sys = VoiceUnknown; v.verdict = VerdictUnsure; v.confidence = 0; v.baud = bestEyeBaud;
        v.devHz = 3 * eyeUnit * bestEyeBaud / (2 * M_PI);
        snprintf(buf, sizeof buf, "a digital signal with a %.0f Bd symbol clock (+-%.1f kHz), but no frame sync of a known system", bestEyeBaud, v.devHz / 1e3);
        v.why = buf;
        return v;
    }
    if (envVar < 0.35) {
        v.sys = VoiceAnalogFm; v.verdict = VerdictLikely; v.confidence = bestEye < 1.12 ? 0.75 : 0.6;
        snprintf(buf, sizeof buf, "constant envelope, no symbol clock (eye %.2f) and no frame sync: an analogue FM channel", bestEye);
        v.why = buf;
        return v;
    }
    v.sys = VoiceUnknown; v.verdict = VerdictUnsure;
    snprintf(buf, sizeof buf, "no frame sync, no symbol clock (eye %.2f), envelope varies (%.2f)", bestEye, envVar);
    v.why = buf;
    return v;
}

VoiceSignal classifyAt(const cf32* x, size_t n, double rate, double centreHz, double bwHz) {
    double rs = 0, rf = 0;
    const double occ = std::max(7000.0, std::min(bwHz * 1.3, 16000.0));
    std::vector<cf32> slow = toBaseband(x, n, rate, centreHz, occ, 48000, rs);
    std::vector<cf32> fast;
    if (bwHz >= 14000 || bwHz <= 0) fast = toBaseband(x, n, rate, centreHz, std::max(24000.0, bwHz * 1.15), 144000, rf);
    return decide(slow, rs, fast, rf, bwHz);
}

} // namespace

VoiceSignal classifyVoice(const cf32* z, size_t n, double rate) {
    std::vector<double> ps = welch(z, n, 4096);
    const Occupied o = occupied(ps, rate, 6);
    return classifyAt(z, n, rate, o.ok ? o.centreHz : 0, o.ok ? o.bwHz : 0);
}

VoiceResult detectVoice(const cf32* x, size_t n, double rate, const VoiceOptions& o) {
    const auto t0 = std::chrono::steady_clock::now();
    VoiceResult r;
    std::vector<sigid::Region> regs = sigid::findRegions(x, n, rate, 24, o.thresholdDb);
    for (const sigid::Region& g : regs) {
        if (r.signals.size() >= o.maxSignals) break;
        if (g.bwHz < o.minBwHz * 0.5 || g.bwHz > o.maxBwHz) continue;
        if (std::fabs(g.centreHz) > rate / 2 - std::max(15e3, g.bwHz)) continue;   // at the edge of the radio's filter
        VoiceSignal v = classifyAt(x, n, rate, g.centreHz, g.bwHz);
        v.offHz = g.centreHz; v.rfHz = o.rfHz + g.centreHz; v.bwHz = g.bwHz; v.snrDb = g.snrDb;
        r.signals.push_back(v);
    }
    r.headline = voiceSummary(r);
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

std::string voiceSummary(const VoiceResult& r) {
    if (r.signals.empty()) return "no narrowband signal found";
    std::string s;
    char b[160];
    for (const VoiceSignal& v : r.signals) {
        if (!s.empty()) s += ", ";
        const char* name = v.sys == VoiceUnknown ? "unknown signal" : voiceSysName(v.sys);
        if (v.rfHz > 1e6) snprintf(b, sizeof b, "%s on %.4f MHz", name, v.rfHz / 1e6);
        else snprintf(b, sizeof b, "%s at %+.1f kHz", name, v.offHz / 1e3);
        s += b;
        if (v.verdict == VerdictUnsure) s += " (unsure)";
    }
    return s;
}

} // namespace autodet
} // namespace dect2
