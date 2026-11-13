// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab stage 5a: preamble, sync word and frame length. Bursty input: the bursts are aligned at the end of their preamble and
// the leading bits they all agree on are the sync word. Continuous input: the commonest non-trivial 24-bit window, grown to the
// pattern its occurrences share, and the bit-stream autocorrelation for the frame period.
#include "dect2/signallab/signallab.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace dect2::signallab {

size_t preambleLength(const Bits& b, size_t from) {
    if (from >= b.size()) return 0;
    size_t i = from + 1;
    while (i < b.size() && b[i] != b[i - 1]) i++;
    return i - from;
}

std::vector<size_t> findPattern(const Bits& b, const Bits& pat, int maxErr, size_t from) {
    std::vector<size_t> out;
    const size_t L = pat.size();
    if (!L || b.size() < L) return out;
    for (size_t i = from; i + L <= b.size(); i++) {
        int e = 0;
        for (size_t k = 0; k < L && e <= maxErr; k++) e += b[i + k] != pat[k];
        if (e <= maxErr) { out.push_back(i + L); i += L - 1; }
    }
    return out;
}

std::vector<double> bitAutocorr(const Bits& b, size_t maxLag) {
    std::vector<double> r(maxLag + 1, 0.0);
    const size_t n = b.size();
    if (n < 4) return r;
    const size_t N = nextPow2(2 * n);
    std::vector<std::complex<double>> a(N, 0.0);
    for (size_t i = 0; i < n; i++) a[i] = b[i] ? 1.0 : -1.0;
    fft(a);
    for (auto& v : a) v = std::norm(v);
    fft(a, true);
    for (size_t l = 0; l <= maxLag && l < n; l++) r[l] = a[l].real() / (double)(n - l);
    return r;
}

namespace {
// the start and length of the longest periodic run (period 2: 1010..., 4: 0011... from QPSK, 6, 8) starting in the first part of
// the stream; the base pattern must hold both values
std::pair<size_t, size_t> longestPreamble(const Bits& s) {
    size_t best = 0, at = 0;
    const size_t lim = std::max<size_t>(16, s.size() * 2 / 5);
    // a constant run (an alternating preamble after NRZI or Manchester decoding) counts too, when long
    for (size_t i = 0; i < lim && i < s.size();) {
        size_t e = i + 1;
        while (e < s.size() && s[e] == s[i]) e++;
        if (e - i >= 16 && e - i > best) { best = e - i; at = i; }
        i = e;
    }
    for (size_t p : {2, 4, 6, 8}) {
        size_t i = p;
        while (i < s.size() && i < lim + p) {
            if (s[i] != s[i - p]) { i++; continue; }
            const size_t st = i - p;
            size_t e = i;
            while (e < s.size() && s[e] == s[e - p]) e++;
            bool mixed = false;
            for (size_t k = st + 1; k < st + p; k++) mixed |= s[k] != s[st];
            const size_t len = e - st;
            if (mixed && len >= std::max<size_t>(12, 4 * p) && len > best + (p > 2 ? 4 : 0)) { best = len; at = st; }
            i = e + 1;
        }
    }
    return {at, best};
}

double agree(const Bits& a, size_t pa, const Bits& b, size_t pb, size_t n, bool inv) {
    size_t ok = 0, t = 0;
    for (size_t k = 0; k < n; k++) {
        if (pa + k >= a.size() || pb + k >= b.size()) break;
        t++;
        ok += (a[pa + k] ^ (inv ? 1 : 0)) == b[pb + k];
    }
    return t ? (double)ok / (double)t : 0;
}

// runs, alternations and any pattern of period up to 8 (preambles: 1010, the 0011 of QPSK, ...) are not sync words
bool trivial(uint32_t v, int bits) {
    const uint32_t m = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    v &= m;
    for (int p = 1; p <= 8; p++) if (((v >> p) ^ v) & (m >> p) ? false : true) return true;
    return false;
}
}

// The bits shared from a start point on (majAt(k), k >= 0; k < 0 reaches back into the preamble) for Lc bits: where the sync word
// really starts and ends is not sure. The preamble can swallow its first bits (a periodic preamble up to 7 of them), a constant
// header can follow it. Sync words are mostly whole bytes: the default rounds to one, the variants (other starts and lengths, a
// little lower score) are tried by the checksum search. rate(start, pattern) gives the score and the hits of a pattern.
template <class MajAt, class Rate>
void addVariants(std::vector<SyncCandidate>& out, MajAt majAt, size_t Lc, int minLen, int maxLen, Rate rate) {
    auto make = [&](long start, size_t len, double pen) {
        SyncCandidate c;
        for (long k = start; k < start + (long)len; k++) { const int b = majAt(k); if (b < 0) return; c.pattern.push_back((uint8_t)b); }
        for (const auto& e : out) if (e.pattern == c.pattern) return;
        const auto sh = rate(start, c.pattern);
        c.score = sh.first - pen; c.hits = sh.second;
        out.push_back(c);
    };
    const size_t L = std::min(Lc, (size_t)maxLen), r = L % 8;
    if (L >= 16 && r && r <= 3) make(0, L - r, 0);
    else if (r >= 5) make(-(long)(8 - r), L + (8 - r), 0);
    else make(0, L, 0);
    double pen = 0.01;
    for (long back = 0; back <= 7; back++)
        for (size_t len = 8; len <= (size_t)maxLen; len += 8)
            if (len + 8 > (size_t)minLen && (len >= 16 || Lc < 16) && len <= Lc + (size_t)back) { make(-back, len, pen); pen += 0.002; }
}

std::vector<SyncCandidate> findSyncCandidates(const std::vector<Bits>& streams, int minLen, int maxLen) {
    std::vector<SyncCandidate> out;
    // ---- bursts: align at the preamble end
    std::vector<const Bits*> ss;
    for (const Bits& s : streams) if (s.size() >= (size_t)minLen + 8) ss.push_back(&s);
    if (ss.size() >= 2) {
        std::vector<size_t> pe;
        for (const Bits* s : ss) { const auto p = longestPreamble(*s); pe.push_back(p.second >= 8 ? p.first + p.second : 0); }
        // each burst: the shift (-3..3) and polarity that best match the first burst after its preamble
        const size_t W = (size_t)maxLen + 16;
        std::vector<long> pos(ss.size());
        std::vector<bool> inv(ss.size(), false);
        pos[0] = (long)pe[0];
        for (size_t j = 1; j < ss.size(); j++) {
            double best = -1;
            for (int d = -3; d <= 3; d++) {
                const long p = (long)pe[j] + d;
                if (p < 0) continue;
                for (int iv = 0; iv < 2; iv++) {
                    const double a = agree(*ss[0], pe[0], *ss[j], (size_t)p, (size_t)maxLen, iv);
                    if (a > best + 1e-9 || (std::fabs(a - best) < 1e-9 && d == 0 && !iv)) { best = a; pos[j] = p; inv[j] = iv; }
                }
            }
        }
        // majority bit and agreement at offset k from the aligned preamble end (k < 0: the preamble's tail)
        auto majAt = [&](long k, double& ag) -> int {
            int ones = 0, n = 0;
            for (size_t j = 0; j < ss.size(); j++) {
                const long p = pos[j] + k;
                if (p < 0 || p >= (long)ss[j]->size()) continue;
                ones += ((*ss[j])[(size_t)p] ^ (inv[j] ? 1 : 0)); n++;
            }
            if (n < (int)ss.size() * 3 / 4 || n < 2) { ag = 0; return -1; }
            ag = std::max(ones, n - ones) / (double)n;
            return ones * 2 > n;
        };
        size_t Lc = 0;
        double aggSum = 0;
        for (size_t k = 0; k < W; k++) { double ag = 0; const int b = majAt((long)k, ag); if (b < 0 || ag < 0.85) break; aggSum += ag; Lc++; }
        if (Lc >= (size_t)minLen) {
            addVariants(out, [&](long k) { double ag = 0; return majAt(k, ag); }, Lc, minLen, maxLen, [&](long start, const Bits& pat) {
                int hits = 0;
                for (size_t j = 0; j < ss.size(); j++) if (pos[j] + start >= 0 && agree(pat, 0, *ss[j], (size_t)(pos[j] + start), pat.size(), inv[j]) >= 0.9) hits++;
                return std::make_pair((double)hits / (double)ss.size() * (aggSum / (double)Lc) * std::min(1.0, (double)pat.size() / 16.0) + 0.5, hits);
            });
        }
    }
    // ---- repeated windows (continuous streams, or several frames per burst)
    {
        constexpr int K = 16;
        std::unordered_map<uint32_t, std::vector<std::pair<int, size_t>>> occ;   // window -> (stream, end position)
        for (size_t si = 0; si < streams.size(); si++) {
            const Bits& s = streams[si];
            uint32_t v = 0;
            for (size_t i = 0; i < s.size(); i++) {
                v = ((v << 1) | s[i]) & ((1u << K) - 1);
                if (i + 1 >= (size_t)K && !trivial(v, K)) { auto& o = occ[v]; if (o.size() < 4096) o.push_back({(int)si, i + 1}); }
            }
        }
        std::vector<std::pair<size_t, uint32_t>> top;
        for (auto& [k, v] : occ) if (v.size() >= 3) top.push_back({v.size(), k});
        std::sort(top.begin(), top.end(), std::greater<>());
        size_t totalBits = 0;
        for (const Bits& s : streams) totalBits += s.size();
        for (size_t t = 0; t < top.size() && t < 6; t++) {
            const auto& o = occ[top[t].second];
            const uint32_t inv = (~top[t].second) & ((1u << K) - 1);
            const auto it = occ.find(inv);
            const size_t ninv = it == occ.end() ? 0 : it->second.size();
            // grow left while the occurrences agree
            auto bitAt = [&](const std::pair<int, size_t>& p, long off) -> int {
                const long i = (long)p.second + off;
                const Bits& s = streams[(size_t)p.first];
                return i >= 0 && i < (long)s.size() ? s[(size_t)i] : -1;
            };
            long left = -(long)K;
            while (-left < 2 * maxLen) {
                int ones = 0, n = 0;
                for (const auto& p : o) { const int b = bitAt(p, left - 1); if (b >= 0) { ones += b; n++; } }
                if (n < (int)o.size() * 3 / 4 || std::max(ones, n - ones) < 0.9 * n) break;
                left--;
            }
            long right = 0;
            while (right < maxLen) {
                int ones = 0, n = 0;
                for (const auto& p : o) { const int b = bitAt(p, right); if (b >= 0) { ones += b; n++; } }
                if (n < (int)o.size() * 3 / 4 || std::max(ones, n - ones) < 0.9 * n) break;
                right++;
            }
            Bits R;
            for (long k = left; k < right; k++) {
                int ones = 0, n = 0;
                for (const auto& p : o) { const int b = bitAt(p, k); if (b >= 0) { ones += b; n++; } }
                R.push_back((uint8_t)(ones * 2 > n));
            }
            const bool inverted = ninv > o.size();
            if (inverted) R = invert(R);
            // the sync word sits after the preamble: start where the alternation stops
            size_t lead = preambleLength(R, 0), run = 1;
            while (run < R.size() && R[run] == R[0]) run++;
            lead = std::max(lead, run);   // or where a constant run (a decoded preamble) stops
            const auto lp = longestPreamble(R);   // or a periodic one (period 4, 6, 8)
            if (lp.first == 0) lead = std::max(lead, lp.second);
            const size_t rs = lead >= 6 ? lead : 0;
            if (R.size() < rs + (size_t)minLen) continue;
            // regular spacing counts
            std::vector<size_t> d;
            for (size_t i = 1; i < o.size(); i++) if (o[i].first == o[i - 1].first) d.push_back(o[i].second - o[i - 1].second);
            double reg = 0;
            if (d.size() >= 2) {
                std::sort(d.begin(), d.end());
                const size_t med = d[d.size() / 2];
                size_t near = 0;
                for (size_t v : d) if (v + 2 >= med && v <= med + 2) near++;
                reg = (double)near / (double)d.size();
            }
            const double expect = (double)totalBits / (double)(1u << K);
            const int hits = (int)(inverted ? ninv : o.size());
            const size_t before = out.size();
            addVariants(out, [&](long k) { const long i = (long)rs + k; return i >= 0 && i < (long)R.size() ? (int)R[(size_t)i] : -1; }, R.size() - rs, minLen, maxLen,
                        [&](long, const Bits& pat) {
                            return std::make_pair(std::min(1.0, (double)hits / (expect + 3) / 4.0) * std::min(1.0, (double)pat.size() / 24.0) * (0.6 + 0.4 * reg), hits);
                        });
            for (size_t i = before; i < out.size(); i++) out[i].inverted = inverted;
            if (out.size() > before) break;   // the commonest window found the frame; the rest are its neighbours
        }
    }
    std::sort(out.begin(), out.end(), [](const SyncCandidate& a, const SyncCandidate& b) { return a.score > b.score; });
    return out;
}

FrameInfo findFrames(const std::vector<Bits>& streams) {
    FrameInfo fi;
    fi.syncCandidates = findSyncCandidates(streams);
    std::vector<size_t> pl;
    for (const Bits& s : streams) { const auto p = longestPreamble(s); if (p.second >= 8) pl.push_back(p.second); }
    if (!pl.empty()) { std::sort(pl.begin(), pl.end()); fi.preambleBits = pl[pl.size() / 2]; }
    if (streams.size() == 1 && streams[0].size() > 256) {
        const auto r = bitAutocorr(streams[0], std::min<size_t>(4096, streams[0].size() / 3));
        std::vector<std::pair<double, size_t>> pk;
        for (size_t l = 24; l + 1 < r.size(); l++) if (r[l] > r[l - 1] && r[l] >= r[l + 1] && r[l] > 0.1) pk.push_back({r[l], l});
        std::sort(pk.begin(), pk.end(), std::greater<>());
        for (size_t i = 0; i < pk.size() && i < 5; i++) fi.autocorrPeaks.push_back(pk[i].second);
    }
    if (fi.syncCandidates.empty()) {
        for (size_t i = 0; i < streams.size(); i++) { fi.frames.push_back(streams[i]); fi.frameStart.push_back(0); fi.frameStream.push_back((int)i); }
        return fi;
    }
    FrameInfo f = framesFor(streams, fi.syncCandidates[0]);
    f.syncCandidates = std::move(fi.syncCandidates);
    f.preambleBits = fi.preambleBits;
    f.autocorrPeaks = std::move(fi.autocorrPeaks);
    return f;
}

FrameInfo framesFor(const std::vector<Bits>& streams, const SyncCandidate& sync, size_t frameBits) {
    FrameInfo fi;
    fi.sync = sync;
    const Bits& p = sync.pattern;
    const Bits pinv = invert(p);
    // short words must match exactly: one error allowed in 16 bits finds false syncs in the payload
    const int maxErr = p.size() >= 24 ? (int)p.size() / 12 : 0;
    for (size_t si = 0; si < streams.size(); si++) {
        const Bits& s = streams[si];
        std::vector<std::pair<size_t, bool>> at;
        if (p.empty()) at.push_back({0, false});
        else {
            for (size_t q : findPattern(s, p, maxErr)) at.push_back({q, false});
            for (size_t q : findPattern(s, pinv, maxErr)) at.push_back({q, true});
        }
        std::sort(at.begin(), at.end());
        for (size_t k = 0; k < at.size(); k++) {
            const size_t st = at[k].first;
            size_t en = k + 1 < at.size() ? at[k + 1].first - p.size() : s.size();
            if (frameBits) en = std::min(en, st + frameBits);
            if (en <= st + 8) continue;
            Bits f(s.begin() + (long)st, s.begin() + (long)en);
            if (at[k].second) f = invert(f);
            fi.frames.push_back(std::move(f));
            fi.frameStart.push_back(st);
            fi.frameStream.push_back((int)si);
        }
    }
    fi.sync.hits = (int)fi.frames.size();
    if (fi.frames.empty()) return fi;
    if (frameBits) { fi.frameBits = frameBits; return fi; }
    std::vector<size_t> len;
    for (const Bits& f : fi.frames) len.push_back(f.size());
    std::sort(len.begin(), len.end());
    // the last frame of a continuous stream is cut short by the end of the capture: judge on the others
    const size_t lmin = len.size() > 3 ? len[1] : len[0], lmed = len[len.size() / 2];
    fi.variableLength = lmed > lmin + 16;
    size_t fb = lmin;
    if (fb % 8 <= 4 && fb >= 16) fb -= fb % 8;
    fi.frameBits = fb;
    if (!fi.variableLength) {
        std::vector<Bits> keep; std::vector<size_t> ks; std::vector<int> kst;
        for (size_t i = 0; i < fi.frames.size(); i++) {
            if (fi.frames[i].size() < fb) continue;
            keep.push_back(Bits(fi.frames[i].begin(), fi.frames[i].begin() + (long)fb));
            ks.push_back(fi.frameStart[i]); kst.push_back(fi.frameStream[i]);
        }
        fi.frames.swap(keep); fi.frameStart.swap(ks); fi.frameStream.swap(kst);
    }
    return fi;
}

} // namespace dect2::signallab
