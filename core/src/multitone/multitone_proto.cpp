// Multitone paging: FSK slicing of a discriminator recording and the POCSAG-polynomial BCH search on the bits (see multitone_rx.h).
// Nothing here knows the Multitone format (it is not public); it only measures: baud rate, bits, and which 32-bit words pass the
// BCH(31,21) + even parity check (generator x^10+x^9+x^8+x^6+x^5+x^3+1, as POCSAG) at the best alignment.
#include "dect2/multitone/multitone_rx.h"
#include "dect2/multitone/multitone_tel.h"
#include "dect2/pager/pager_proto.h"
#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace multitone {

namespace {
using dect2::math::kPi;

struct Edge { double t; };   // seconds from the start of the recording

double pctOf(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    const size_t i = std::min(v.size() - 1, (size_t)(q * (double)v.size()));
    std::nth_element(v.begin(), v.begin() + (long)i, v.end());
    return v[i];
}

// edges of a two-level signal: crossings of the mid level, with hysteresis so that noise does not make pairs
std::vector<double> findEdges(const std::vector<float>& v, double rate, double mid, double hyst) {
    std::vector<double> e;
    if (v.size() < 4) return e;
    int state = v[0] > mid ? 1 : 0;
    size_t lastOpp = 0;   // last index on the other side of mid than the current state
    for (size_t i = 1; i < v.size(); i++) {
        const double x = v[i];
        if (state == 0) {
            if (x <= mid) lastOpp = i;
            else if (x > mid + hyst) {
                const size_t a = lastOpp;
                const double fa = v[a], fb = v[a + 1];
                const double fr = fb != fa ? (mid - fa) / (fb - fa) : 0.0;
                e.push_back(((double)a + std::max(0.0, std::min(1.0, fr))) / rate);
                state = 1; lastOpp = i;
            }
        } else {
            if (x >= mid) lastOpp = i;
            else if (x < mid - hyst) {
                const size_t a = lastOpp;
                const double fa = v[a], fb = v[a + 1];
                const double fr = fb != fa ? (mid - fa) / (fb - fa) : 0.0;
                e.push_back(((double)a + std::max(0.0, std::min(1.0, fr))) / rate);
                state = 0; lastOpp = i;
            }
        }
    }
    return e;
}

double coherenceAt(const std::vector<double>& e, double baud, double* phase = nullptr) {
    double re = 0, im = 0;
    for (double t : e) { const double a = 2 * kPi * baud * t; re += std::cos(a); im += std::sin(a); }
    if (phase) *phase = std::atan2(im, re);
    return e.empty() ? 0 : std::sqrt(re * re + im * im) / (double)e.size();
}

// The baud rate from the edge times. Edge intervals are whole bit times (plus jitter), so the lower end of the interval distribution gives a first
// guess; around it (-20% to +25%) the coherence of the edges against a bit clock is searched in steps much finer than 1 / (length of the
// recording), then the best clock wins. A tone (every interval about the same) is told from data by the intervals: at least 12% of them
// must be longer than 1.5 bit times.
bool estimateBaud(const std::vector<double>& edgesIn, double& baud, double& coh) {
    if (edgesIn.size() < 12) return false;
    std::vector<double> iv;
    for (size_t i = 1; i < edgesIn.size(); i++) iv.push_back(edgesIn[i] - edgesIn[i - 1]);
    std::vector<double> sv = iv;
    std::sort(sv.begin(), sv.end());
    const double t0 = sv[sv.size() * 15 / 100];
    if (t0 <= 0) return false;
    const double b0 = 1.0 / t0;
    std::vector<double> e = edgesIn;
    if (e.size() > 1200) { std::vector<double> s; const double st = (double)e.size() / 1200.0; for (double x = 0; x < (double)e.size(); x += st) s.push_back(e[(size_t)x]); e.swap(s); }
    const double L = std::max(0.05, edgesIn.back() - edgesIn.front());
    const double step = 0.2 / L;
    double bb = 0, bc = 0;
    for (double b = std::max(60.0, 0.8 * b0); b <= std::min(12000.0, 1.25 * b0); b += step) { const double c = coherenceAt(e, b); if (c > bc) { bc = c; bb = b; } }
    if (bc < 0.25) return false;
    double fb = bb, fc = 0;
    for (double b = bb - 2 * step; b <= bb + 2 * step; b += step / 8) { const double c = coherenceAt(edgesIn, b); if (c > fc) { fc = c; fb = b; } }
    // a clock at half the rate fits whole-bit-time edges almost as well when the data has only long intervals; the clock at B itself is the lowest that fits all
    const double half = fb * 0.5;
    if (half >= 60) {
        double hc = 0, hb = half;
        for (double b = half * 0.97; b <= half * 1.03; b += step / 4) { const double c = coherenceAt(edgesIn, b); if (c > hc) { hc = c; hb = b; } }
        if (hc >= 0.8 * fc) { fb = hb; fc = hc; }
    }
    baud = fb; coh = fc;
    int longI = 0, n = 0;
    for (double d : iv) { n++; if (d * fb > 1.5) longI++; }
    return n > 0 && (double)longI / n >= 0.12 && fc >= 0.3;
}
} // namespace

const char* nearestStandardBaud(double baud) {
    static const struct { double b; const char* n; } t[] = {{300, "300"}, {512, "512"}, {600, "600"}, {1200, "1200"}, {2400, "2400"}, {4800, "4800"}, {9600, "9600"}};
    for (const auto& x : t) if (std::fabs(baud / x.b - 1) < 0.015) return x.n;
    return "";
}

FskResult sliceFfsk(const std::vector<float>& discIn, double rate) {
    FskResult r;
    if (discIn.size() < (size_t)(0.2 * rate)) return r;
    // the audio is below about 1.2 kHz: a boxcar of 1 / 3000 s (two passes) takes the noise off the zero crossings
    std::vector<float> disc(discIn.size());
    {
        const size_t L = std::max<size_t>(2, (size_t)(rate / 3000.0));
        std::vector<float> t1(discIn.size());
        for (int pass = 0; pass < 2; pass++) {
            const std::vector<float>& in = pass == 0 ? discIn : t1;
            std::vector<float>& out = pass == 0 ? t1 : disc;
            double acc = 0;
            for (size_t i = 0; i < in.size(); i++) {
                acc += in[i];
                if (i >= L) acc -= in[i - L];
                out[i] = (float)(acc / (double)std::min(L, i + 1));
            }
        }
    }
    const double lo = pctOf(disc, 0.05), hi = pctOf(disc, 0.95), mid = pctOf(disc, 0.5);
    if (hi - lo < 0.1) return r;
    const std::vector<double> e = findEdges(disc, rate, mid, 0.12 * (hi - lo));
    if (e.size() < 60) return r;
    std::vector<double> iv;
    for (size_t i = 1; i < e.size(); i++) iv.push_back(e[i] - e[i - 1]);
    std::vector<double> sv = iv;
    std::sort(sv.begin(), sv.end());
    const double p10 = sv[sv.size() / 10];
    std::vector<double> sh;
    for (double d : iv) if (d >= 0.75 * p10 && d <= 1.3 * p10) sh.push_back(d);
    if (sh.size() < 20) return r;
    std::sort(sh.begin(), sh.end());
    const double u = sh[sh.size() / 2];
    // the intervals of one half cycle of the high tone (u) and of the low tone (2u)
    int n1 = 0, n2 = 0;
    for (double d : iv) { if (std::fabs(d / u - 1) < 0.3) n1++; else if (std::fabs(d / u - 2) < 0.4) n2++; }
    const double tot = (double)iv.size();
    if ((n1 + n2) / tot < 0.85 || n1 / tot < 0.15 || n2 / tot < 0.1) return r;
    {   // grammar: between two 2u intervals the 1u intervals come in pairs (a high tone symbol is two half cycles)
        int runs1 = 0, even = 0, cnt = 0;
        bool seen2 = false;
        for (double d : iv) {
            if (std::fabs(d / u - 1) < 0.3) cnt++;
            else if (std::fabs(d / u - 2) < 0.4) { if (seen2) { runs1++; if (cnt % 2 == 0) even++; } seen2 = true; cnt = 0; }
        }
        if (runs1 < 10 || (double)even < 0.9 * runs1) return r;
    }
    // symbols: a 2u interval is the low tone, two u intervals the high tone; the parity of the start is the one with fewer misfits
    std::vector<std::vector<int>> runs(1);
    for (double d : iv) {
        if (std::fabs(d / u - 1) < 0.3) runs.back().push_back(1);
        else if (std::fabs(d / u - 2) < 0.4) runs.back().push_back(2);
        else if (!runs.back().empty()) runs.emplace_back();
    }
    int errAll = 0;
    for (const auto& run : runs) {
        if (run.size() < 24) continue;
        int bestErr = 1 << 30; std::vector<uint8_t> bestBits;
        for (int par = 0; par < 2; par++) {
            std::vector<uint8_t> bits; int err = 0;
            for (size_t i = (size_t)par; i < run.size();) {
                if (run[i] == 2) { bits.push_back(0); i++; }
                else if (i + 1 < run.size() && run[i + 1] == 1) { bits.push_back(1); i += 2; }
                else { err++; bits.push_back(0); i++; }
            }
            if (err < bestErr) { bestErr = err; bestBits = bits; }
        }
        errAll += bestErr;
        r.bits.insert(r.bits.end(), bestBits.begin(), bestBits.end());
    }
    r.nBits = (int)r.bits.size();
    int changes = 0;
    for (size_t i = 1; i < r.bits.size(); i++) changes += r.bits[i] != r.bits[i - 1];
    if (r.nBits < 64 || errAll > 0.03 * r.nBits || changes < 0.1 * r.nBits) { r.bits.clear(); r.nBits = 0; return r; }   // (steady tones are not data)
    r.found = true; r.ffsk = true;
    r.baud = 1.0 / (2.0 * u); r.hiHz = 1.0 / (2.0 * u); r.loHz = 0.5 * r.hiHz;
    r.coherence = 1.0 - (double)errAll / (double)r.nBits;
    return r;
}

FskResult sliceFsk(const std::vector<float>& disc, double rate) {
    FskResult r;
    { FskResult f = sliceFfsk(disc, rate); if (f.found) return f; }
    if (disc.size() < (size_t)(0.2 * rate)) return r;
    const double lo = pctOf(disc, 0.05), hi = pctOf(disc, 0.95);
    if (hi - lo < 0.12) return r;                         // under about 1 kHz peak to peak at 48 kHz: not FSK
    const double mid = 0.5 * (lo + hi), hyst = 0.15 * (hi - lo);
    {   // two-level data sits near its two levels; a tone (a sine) spends only about 43% of its time beyond 75% of its swing
        size_t farBy = 0;
        for (float v : disc) if (std::fabs(v - mid) > 0.375 * (hi - lo)) farBy++;
        if ((double)farBy / (double)disc.size() < 0.6) return r;
    }
    const std::vector<double> e = findEdges(disc, rate, mid, hyst);
    double baud = 0, coh = 0;
    if (!estimateBaud(e, baud, coh)) return r;
    double ph = 0;
    coherenceAt(e, baud, &ph);
    const double T = 1.0 / baud;
    // the bit clock: boundaries at (ph / 2 pi + k) T; a bit is read at the centre, the clock follows the edges
    double t0 = ph / (2 * kPi) * T;                       // a boundary
    const double endT = (double)disc.size() / rate;
    while (t0 > e.front() - 2 * T) t0 -= T;
    while (t0 < e.front() - T) t0 += T;
    r.found = true; r.baud = baud; r.coherence = coh;
    size_t ei = 0;
    double c = t0 + 0.5 * T;
    for (; c < endT - T; c += T) {
        const double bnd = c - 0.5 * T;
        while (ei < e.size() && e[ei] < bnd - 0.5 * T) ei++;
        // an edge within a quarter bit of the boundary pulls the clock
        if (ei < e.size() && std::fabs(e[ei] - bnd) < 0.25 * T) c += 0.15 * (e[ei] - bnd);
        const double pos = c * rate;
        const size_t i = (size_t)std::max(0.0, pos);
        if (i + 1 >= disc.size()) break;
        const double x = disc[i] + (disc[i + 1] - disc[i]) * (pos - (double)i);
        r.bits.push_back(x > mid ? 1 : 0);
    }
    r.nBits = (int)r.bits.size();
    return r;
}

void analyseFraming(const std::vector<uint8_t>& bitsIn, MtBurst& b) {
    b.dupBits = 1; b.dataBaud = b.baud; b.markerBits = 0; b.marker.clear(); b.markerCount = 0; b.markerGap = 0;
    b.frameWords.clear(); b.doubledWords = 0; b.distinctWords = 0; b.halfEqualWords = 0;
    if (bitsIn.size() < 96) return;
    // 1. every bit twice in a row? (the sliced audio FFSK has a pair of equal symbols for each bit)
    std::vector<uint8_t> bits = bitsIn;
    for (int par = 0; par < 2 && b.dupBits == 1; par++) {
        size_t same = 0, tot = 0;
        for (size_t i = (size_t)par; i + 1 < bitsIn.size(); i += 2) { same += bitsIn[i] == bitsIn[i + 1]; tot++; }
        if (tot >= 48 && (double)same >= 0.95 * (double)tot) {
            b.dupBits = 2;
            bits.clear();
            for (size_t i = (size_t)par; i + 1 < bitsIn.size(); i += 2) bits.push_back(bitsIn[i]);
        }
    }
    b.dataBaud = b.baud / b.dupBits;
    const size_t n = bits.size();
    if (n < 64) return;
    // 2. a pattern of P bits that comes back at distances that are whole multiples of P
    std::string s(n, '0');
    for (size_t i = 0; i < n; i++) s[i] = bits[i] ? '1' : '0';
    struct Best { int P = 0; std::string pat; std::vector<size_t> pos; double score = 0; } best;
    for (int P = 8; P <= 32; P++) {
        std::map<std::string, std::vector<size_t>> m;
        for (size_t i = 0; i + (size_t)P <= n; i++) m[s.substr(i, (size_t)P)].push_back(i);
        for (auto& kv : m) {
            const auto& pos = kv.second;
            if (pos.size() < 4) continue;
            int changes = 0;
            for (size_t i = 1; i < kv.first.size(); i++) changes += kv.first[i] != kv.first[i - 1];
            if (changes < 5) continue;            // a run of one value is an idle pattern, not a marker
            size_t fit = 0, gaps = 0;
            std::vector<size_t> keep{pos[0]};
            for (size_t i = 1; i < pos.size(); i++) {
                const size_t g = pos[i] - keep.back();
                if (g < (size_t)P) continue;      // overlapping copy of the same occurrence
                gaps++;
                if (g % (size_t)P == 0) fit++;
                keep.push_back(pos[i]);
            }
            if (keep.size() < 4 || gaps == 0 || (double)fit < 0.8 * (double)gaps) continue;
            const double sc = (double)keep.size() * (double)P;
            if (sc > best.score + 1e-9) { best.P = P; best.pat = kv.first; best.pos = keep; best.score = sc; }
        }
    }
    if (best.P == 0) return;
    b.markerBits = best.P; b.marker = best.pat; b.markerCount = (int)best.pos.size();
    std::map<size_t, int> gc;
    for (size_t i = 1; i < best.pos.size(); i++) gc[best.pos[i] - best.pos[i - 1]]++;
    size_t gap = (size_t)best.P; int gn = 0;
    for (auto& kv : gc) if (kv.second > gn) { gn = kv.second; gap = kv.first; }
    b.markerGap = (int)gap;
    // 3. the words between the marker and the next place one gap on
    std::string prev;
    std::set<std::string> distinct;
    for (size_t p : best.pos) {
        if (p + gap > n || gap <= (size_t)best.P) continue;
        const std::string w = s.substr(p + (size_t)best.P, gap - (size_t)best.P);
        if (b.frameWords.size() < 64) b.frameWords.push_back(w);
        if (w == prev) b.doubledWords++;
        if (w.size() > 8 && (w.size() - 8) % 2 == 0) {   // a header byte, then a value sent twice?
            const size_t h = (w.size() - 8) / 2;
            int d = 0;
            for (size_t i = 0; i < h; i++) d += w[8 + i] != w[8 + h + i];
            if (d <= 1) b.halfEqualWords++;
        }
        distinct.insert(w); prev = w;
    }
    b.distinctWords = (int)distinct.size();
}

void analyseBits(const std::vector<uint8_t>& bits, MtBurst& b) {
    b.nBits = (int)bits.size();
    // hex of the stream as sliced
    {
        std::string h;
        char c[4];
        for (size_t i = 0; i + 8 <= bits.size() && h.size() < 128; i += 8) {
            unsigned v = 0;
            for (int k = 0; k < 8; k++) v = (v << 1) | (bits[i + (size_t)k] & 1u);
            snprintf(c, sizeof c, "%02X", v);
            h += c;
        }
        b.bitsHex = h;
    }
    // POCSAG sync word anywhere, either polarity
    b.syncFound = false; b.syncAt = -1;
    if (bits.size() >= 32) {
        uint32_t w = 0;
        for (size_t i = 0; i < bits.size(); i++) {
            w = (w << 1) | (bits[i] & 1u);
            if (i < 31) continue;
            int m = 0;
            for (uint32_t d = w ^ pager::kPocsagSync; d; d &= d - 1) m++;
            int mi = 0;
            for (uint32_t d = ~w ^ pager::kPocsagSync; d; d &= d - 1) mi++;
            if (m <= 2 || mi <= 2) { b.syncFound = true; b.syncAt = (int)i - 31; break; }
        }
    }
    // Manchester (biphase) coding: every pair of sliced bits is 01 or 10 (a 1 is read as 10); the BCH search and the repeat test then run on the decoded bits
    std::vector<uint8_t> dec;
    b.manchester = false;
    {   // pairs that are 00 or 11 are clock slips: skip one bit and read on
        size_t slips = 0;
        std::vector<uint8_t> d;
        for (size_t i = 0; i + 1 < bits.size();) {
            if ((bits[i] & 1u) != (bits[i + 1] & 1u)) { d.push_back((bits[i] & 1u) ? 1 : 0); i += 2; }
            else { slips++; i += 1; }
        }
        if (!b.ffsk && d.size() >= 32 && (double)slips <= 0.15 * (double)d.size()) { b.manchester = true; dec.swap(d); }
    }
    const std::vector<uint8_t>& s = b.manchester ? dec : bits;
    b.dataBits = (int)s.size();
    // a pattern that repeats: the smallest shift (8 - 512 bits) at which 92% of the bits equal the bits that far on
    b.repeatBits = 0;
    for (int p = 8; p <= 512 && (size_t)p * 3 <= s.size(); p++) {
        size_t same = 0, n = s.size() - (size_t)p;
        for (size_t i = 0; i < n; i++) same += s[i] == s[i + (size_t)p];
        if ((double)same >= 0.92 * (double)n) { b.repeatBits = p; break; }
    }
    // the best alignment of valid 32-bit words
    int bestRun = 0, bestValid = 0, bestAlign = 0, bestPol = 1, bestTotal = (int)(s.size() / 32), bestRunStart = 0;
    // polarity: the complement of a word passes the check as well (all-ones is a code word, the parity is over 32 bits), so it cannot be told here
    for (int pol = 1; pol >= 1; pol--)
        for (int al = 0; al < 32; al++) {
            int run = 0, valid = 0, total = 0, bRun = 0, bStart = 0, runStart = 0;
            for (size_t i = (size_t)al; i + 32 <= s.size(); i += 32) {
                uint32_t w = 0;
                for (int k = 0; k < 32; k++) { const uint32_t x = s[i + (size_t)k] & 1u; w = (w << 1) | (pol > 0 ? x : 1u - x); }
                total++;
                const pager::Fixed f = pager::correctWord(w);
                if (w != 0 && w != 0xFFFFFFFFu && f.ok && f.bits == 0) {
                    if (run == 0) runStart = (int)((i - (size_t)al) / 32);
                    valid++; run++;
                    if (run > bRun) { bRun = run; bStart = runStart; }
                } else run = 0;
            }
            if (bRun > bestRun || (bRun == bestRun && valid > bestValid) || (al == 0 && pol == 1)) { bestRun = bRun; bestValid = valid; bestAlign = al; bestPol = pol; bestTotal = total; bestRunStart = bStart; }
        }
    b.bestRun = bestRun; b.validWords = bestValid; b.align = bestAlign; b.pol = bestPol; b.totalWords = bestTotal;
    b.words.clear(); b.wordOk.clear();
    const size_t first = bestRun >= 2 ? (size_t)bestAlign + (size_t)std::max(0, bestRunStart - 1) * 32 : (size_t)bestAlign;
    for (size_t i = first; i + 32 <= s.size() && b.words.size() < 32; i += 32) {
        uint32_t w = 0;
        for (int k = 0; k < 32; k++) { const uint32_t x = s[i + (size_t)k] & 1u; w = (w << 1) | (bestPol > 0 ? x : 1u - x); }
        const pager::Fixed f = pager::correctWord(w);
        b.words.push_back(w);
        b.wordOk.push_back(w != 0 && w != 0xFFFFFFFFu && f.ok && f.bits == 0 ? 1 : 0);
    }
    // the 16-bit word 0x605A (seen in all recordings of the real signal, twice in a row before the message), either polarity, up to 1 bit wrong
    b.syncCount = 0; b.sync16At.clear(); b.afterSync.clear();
    {
        std::vector<size_t> at;
        for (size_t i = 0; i + 16 <= bits.size(); i++) {
            uint32_t w = 0;
            for (int k = 0; k < 16; k++) w = (w << 1) | (bits[i + (size_t)k] & 1u);
            int m = 0, mi = 0;
            for (uint32_t d = (w ^ 0x605Au) & 0xFFFF; d; d &= d - 1) m++;
            for (uint32_t d = (w ^ 0x605Au ^ 0xFFFFu) & 0xFFFF; d; d &= d - 1) mi++;
            if (m <= 1 || mi <= 1) { at.push_back(i); i += 15; }
        }
        b.syncCount = (int)at.size();
        for (size_t i = 0; i < at.size() && i < 8; i++) b.sync16At.push_back((int)at[i]);
        // after the first pair of syncs 16 bits apart, or after the first one
        size_t from = 0; bool have = false;
        for (size_t i = 0; i + 1 < at.size(); i++) if (at[i + 1] - at[i] == 16) { from = at[i + 1] + 16; have = true; break; }
        if (!have && !at.empty()) { from = at[0] + 16; have = true; }
        if (have) {
            char c[8];
            for (size_t i = from, n = 0; i + 16 <= bits.size() && n < 12; i += 16, n++) {
                unsigned w = 0;
                for (int k = 0; k < 16; k++) w = (w << 1) | (bits[i + (size_t)k] & 1u);
                snprintf(c, sizeof c, "%04X ", w);
                b.afterSync += c;
            }
        }
    }
    char v[400];
    if (b.syncFound)
        snprintf(v, sizeof v, "POCSAG sync word 0x7CD215D8 found at bit %d: this stream is standard POCSAG framing, read it in the POCSAG mode. Not Multitone's own format (unverified)", b.syncAt);
    else if (bestRun >= 2)
        snprintf(v, sizeof v, "%d consecutive 32-bit words pass the POCSAG BCH(31,21) + parity check (%d of %d at bit offset %d): the same polynomial (an inverted stream passes too). "
                              "Header and framing are NOT known (Multitone's format is not public); unverified", bestRun, bestValid, bestTotal, bestAlign);
    else
        snprintf(v, sizeof v, "FSK data at %.1f baud%s; no run of valid BCH(31,21) words (best %d of %d): not POCSAG-coded, or too damaged. Format unknown; unverified", b.baud, b.manchester ? ", Manchester coded" : "", bestValid, bestTotal);
    b.verdict = v;
    if (b.ffsk) {
        char f[420];
        snprintf(f, sizeof f, "Audio FFSK: tones %.1f and %.1f Hz, %.1f baud; no BCH words. The 16-bit word 0x605A is in the stream %d times%s. The meaning of the words after it is not worked out; unverified.",
                 b.toneLoHz, b.toneHiHz, b.baud, b.syncCount, b.syncCount >= 2 ? " (as in the recordings of the real signal)" : "");
        if (!(b.syncFound || bestRun >= 2)) b.verdict = f;
    }
    if (b.manchester && bestRun >= 2) b.verdict += " (Manchester decoded.)";
    if (b.repeatBits > 0) { char r[96]; snprintf(r, sizeof r, " The bit pattern repeats every %d bits.", b.repeatBits); b.verdict += r; }
    analyseFraming(bits, b);
    if (b.markerBits > 0) {
        std::string t;
        if (b.dupBits > 1) { char d[96]; snprintf(d, sizeof d, " Every bit is sent twice (data rate %.1f baud).", b.dataBaud); t += d; }
        char m[300]; snprintf(m, sizeof m, " Framing measured: a %d-bit marker %s %d times, one every %d bits, %d different words after it (%d repeat the one before, %d are a first byte and a value sent twice). Their meaning is not worked out.", b.markerBits, b.marker.c_str(), b.markerCount, b.markerGap, b.distinctWords, b.doubledWords, b.halfEqualWords);
        b.verdict += t + m;
    }
}

} // namespace multitone
} // namespace dect2
