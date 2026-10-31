// Pagers: multi-copy combining (see pager_combine.h).
#include "dect2/pager/pager_combine.h"
#include "dect2/pager/pager_proto.h"
#include <algorithm>

namespace dect2 {
namespace pager {

namespace {
inline int pop32(uint32_t v) { return __builtin_popcount(v); }
}

CombinedWord combineWord(const uint32_t* raw, int n) {
    CombinedWord r;
    if (n <= 0) return r;
    struct Cand { uint32_t data; int bits; int votes; };
    Cand c[16];
    int nc = 0, invalid = 0;
    for (int i = 0; i < n; i++) {
        const Fixed f = correctWord(raw[i]);
        if (!f.ok) { invalid++; continue; }
        int k = 0;
        while (k < nc && c[k].data != f.data) k++;
        if (k == nc) { if (nc == 16) continue; c[nc++] = {f.data, f.bits, 0}; }
        c[k].votes++;
        c[k].bits = std::min(c[k].bits, f.bits);
    }
    if (nc > 0) {
        int best = 0;
        for (int k = 1; k < nc; k++)
            if (c[k].votes > c[best].votes || (c[k].votes == c[best].votes && c[k].bits < c[best].bits)) best = k;
        r.raw = encodeWord(c[best].data);
        r.ok = true;
        r.agree = c[best].votes;
        r.voted = invalid > 0 || nc > 1;
        return r;
    }
    // no copy is valid on its own
    r.raw = raw[0];
    if (n >= 3) {
        uint32_t maj = 0;
        for (int b = 0; b < 32; b++) {
            int ones = 0;
            for (int i = 0; i < n; i++) ones += (int)((raw[i] >> b) & 1u);
            if (2 * ones > n || (2 * ones == n && ((raw[0] >> b) & 1u))) maj |= 1u << b;
        }
        r.raw = maj;
        const Fixed f = correctWord(maj);
        if (f.ok) {
            r.raw = encodeWord(f.data); r.ok = true; r.voted = true;
            for (int i = 0; i < n; i++) if (pop32(raw[i] ^ r.raw) <= 4) r.agree++;
        }
        return r;
    }
    if (n == 2) {
        const uint32_t d = raw[0] ^ raw[1];
        const int pc = pop32(d);
        if (pc == 0 || pc > 10) return r;
        int pos[10], np = 0;
        for (int b = 0; b < 32; b++) if ((d >> b) & 1u) pos[np++] = b;
        uint32_t found = 0; int nfound = 0;
        for (uint32_t s = 0; s < (1u << np); s++) {
            uint32_t m = 0;
            for (int k = 0; k < np; k++) if ((s >> k) & 1u) m |= 1u << pos[k];
            const uint32_t cand = raw[0] ^ m;
            const Fixed f = correctWord(cand);
            if (f.ok && f.bits == 0) { if (nfound == 0 || cand != found) { found = cand; nfound++; } }
        }
        if (nfound == 1) { r.raw = found; r.ok = true; r.voted = true; r.agree = 0; }
    }
    return r;
}

namespace {
bool validWord(uint32_t w, uint32_t& canon) {
    const Fixed f = correctWord(w);
    if (!f.ok) return false;
    canon = encodeWord(f.data);
    return true;
}
}

CopyMatch comparePocsagCopies(const PocsagCopy& a, const PocsagCopy& b) {
    CopyMatch m;
    const size_t n = std::min(a.words.size(), b.words.size());
    for (size_t i = 1; i < n; i++) {
        m.overlap++;
        uint32_t x, y;
        if (validWord(a.words[i], x) && validWord(b.words[i], y)) { if (x == y) m.equal++; else m.differ++; }
    }
    return m;
}

bool pocsagSameMessage(const PocsagCopy& a, const PocsagCopy& b) {
    if (a.words.empty() || b.words.empty()) return false;
    uint32_t x, y;
    if (!validWord(a.words[0], x) || !validWord(b.words[0], y) || x != y) return false;
    const CopyMatch m = comparePocsagCopies(a, b);
    if (m.overlap == 0) {
        // a copy that is only an address word because a message word was repaired into an address / idle word (its terminator needed repair) can
        // still be a cut-short copy of a message with text; a tone-only page ended by a clean idle word never matches
        if (a.words.size() == 1 && a.hasTerm && b.words.size() >= 2) { const Fixed f = correctWord(a.term); return !f.ok || f.bits > 0; }
        return false;                              // two tone-only pages say nothing to combine
    }              // two tone-only pages say nothing to combine
    if (m.equal < std::max(1, (m.overlap + 2) / 3)) return false;   // at least a third of the words must be seen good in both
    return m.differ * 10 <= m.equal;
}

MergedCopy mergePocsagCopies(const std::vector<const PocsagCopy*>& copies) {
    MergedCopy out;
    if (copies.empty()) return out;
    const PocsagCopy& pri = *copies[0];
    bool extend = !pri.hasTerm;
    if (pri.hasTerm) { const Fixed f = correctWord(pri.term); extend = !f.ok || f.bits > 0; }
    size_t len = pri.words.size();
    if (extend) for (const PocsagCopy* c : copies) len = std::max(len, c->words.size());
    out.words.resize(len);
    out.valid.assign(len, 0);
    for (size_t i = 0; i < len; i++) {
        uint32_t raws[16];
        int n = 0;
        for (const PocsagCopy* c : copies) if (i < c->words.size() && n < 16) raws[n++] = c->words[i];
        const CombinedWord cw = combineWord(raws, n);
        uint32_t priCanon = 0;
        const bool priHad = i < pri.words.size();
        const bool priValid = priHad && validWord(pri.words[i], priCanon);
        if (i == 0) { out.words[0] = priValid ? priCanon : pri.words[0]; out.valid[0] = priValid ? 1 : 0; continue; }   // the address word is what the copies were matched on
        if (cw.ok) { out.words[i] = cw.raw; out.valid[i] = 1; }
        else { out.words[i] = priHad ? pri.words[i] : cw.raw; out.valid[i] = 0; }
        if (cw.ok ? (!priValid || cw.raw != priCanon) : false) out.recovered++;
    }
    return out;
}

// ---------------------------------------------------------------- FLEX

void flexPhaseWords(const uint8_t* bits, uint32_t* words88) {
    for (int i = 0; i < kFlexPhaseWords; i++) {
        uint32_t w = 0;
        for (int j = 0; j < 32; j++) w = (w << 1) | (uint32_t)(bits[(i / 8) * 256 + j * 8 + i % 8] & 1u);
        words88[i] = w;
    }
}
void flexPhaseFromWords(const uint32_t* words88, uint8_t* bits) {
    for (int i = 0; i < kFlexPhaseWords; i++)
        for (int j = 0; j < 32; j++) bits[(i / 8) * 256 + j * 8 + i % 8] = (uint8_t)((words88[i] >> (31 - j)) & 1u);
}

int FlexPhaseCombiner::process(int key, uint8_t* bits, int64_t nowSec) {
    std::vector<uint32_t> cur(kFlexPhaseWords);
    flexPhaseWords(bits, cur.data());
    hist_.erase(std::remove_if(hist_.begin(), hist_.end(), [&](const Entry& e) { return nowSec - e.wall > windowSec || nowSec < e.wall; }), hist_.end());
    bool curValid[kFlexPhaseWords];
    int nBad = 0;
    for (int i = 0; i < kFlexPhaseWords; i++) { uint32_t c; curValid[i] = validWord(cur[i], c); nBad += curValid[i] ? 0 : 1; }
    std::vector<size_t> match;
    for (size_t k = hist_.size(); k-- > 0;) {
        const Entry& e = hist_[k];
        if (e.key != key) continue;
        int eq = 0, df = 0;
        for (int i = 0; i < kFlexPhaseWords; i++) {
            uint32_t a, b;
            if (curValid[i] && validWord(e.words[(size_t)i], b) && validWord(cur[(size_t)i], a)) { if (a == b) eq++; else df++; }
        }
        if (eq >= 16 && df * 10 <= eq) match.push_back(k);
        if ((int)match.size() >= maxCopies - 1) break;
    }
    int rec = 0;
    if (nBad > 0 && !match.empty()) {
        for (int i = 0; i < kFlexPhaseWords; i++) {
            if (curValid[i]) continue;
            uint32_t raws[16];
            int n = 0;
            raws[n++] = cur[(size_t)i];
            for (size_t k : match) if (n < 16) raws[n++] = hist_[k].words[(size_t)i];
            const CombinedWord cw = combineWord(raws, n);
            if (cw.ok) { cur[(size_t)i] = cw.raw; rec++; }
        }
        if (rec > 0) { flexPhaseFromWords(cur.data(), bits); phasesCombined++; wordsRecovered += rec; }
    }
    // remember this phase (merged); keep at most maxCopies of one message (the oldest goes)
    if ((int)match.size() >= maxCopies - 1 && !match.empty()) hist_.erase(hist_.begin() + (long)match.back());
    hist_.push_back({key, nowSec, cur});
    if (hist_.size() > 48) hist_.erase(hist_.begin());
    return rec;
}

} // namespace pager
} // namespace dect2
