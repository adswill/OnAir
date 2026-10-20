#include "dect2/lbj/lbj_pocsag.h"
#include "dect2/common/bits.h"
#include <algorithm>

namespace dect2::lbj {

namespace {
constexpr uint32_t kGen = 0x769;   // x^10+x^9+x^8+x^6+x^5+x^3+1
inline int pop(uint32_t v) { return dect2::bits::popcount(v); }
// POCSAG numeric set by the 4-bit value (sent least significant bit first)
const char kNum[17] = "0123456789*U -)(";
} // namespace

uint32_t bchRemainder(uint32_t word) {
    uint32_t r = word >> 1;
    for (int i = 30; i >= 10; i--)
        if ((r >> i) & 1) r ^= kGen << (i - 10);
    return r & 0x3FF;
}

bool codewordValid(uint32_t word) { return bchRemainder(word) == 0 && (pop(word) & 1) == 0; }

uint32_t encodeCodeword(uint32_t data21) {
    uint32_t w;
    uint32_t v = (data21 & 0x1FFFFF) << 10;
    uint32_t r = v;
    for (int i = 30; i >= 10; i--)
        if ((r >> i) & 1) r ^= kGen << (i - 10);
    w = ((data21 & 0x1FFFFF) << 11) | ((r & 0x3FF) << 1);
    if (pop(w) & 1) w |= 1;
    return w;
}

CwResult correctCodeword(uint32_t word, int maxFix) {
    CwResult r;
    if (codewordValid(word)) { r.ok = true; r.word = word; return r; }
    for (int i = 0; i < 32 && maxFix >= 1; i++) {
        const uint32_t c = word ^ (1u << i);
        if (codewordValid(c)) { r.ok = true; r.word = c; r.fixed = 1; return r; }
    }
    for (int i = 0; i < 32 && maxFix >= 2; i++)
        for (int j = i + 1; j < 32; j++) {
            const uint32_t c = word ^ (1u << i) ^ (1u << j);
            if (codewordValid(c)) { r.ok = true; r.word = c; r.fixed = 2; return r; }
        }
    return r;
}

CwResult correctSoft(uint32_t word, const float* rel) {
    CwResult hard = correctCodeword(word, 2);
    // the eight least reliable positions
    int idx[32];
    for (int i = 0; i < 32; i++) idx[i] = i;
    std::partial_sort(idx, idx + 8, idx + 32, [&](int a, int b) { return rel[a] < rel[b]; });
    auto cost = [&](uint32_t from, uint32_t to) {
        float c = 0;
        const uint32_t d = from ^ to;
        for (int i = 0; i < 32; i++) if ((d >> i) & 1) c += rel[i] + 0.15f;   // each flip also costs a little: fewer flips preferred
        return c;
    };
    CwResult best;
    float bestCost = 1e9f;
    if (hard.ok) { best = hard; bestCost = cost(word, hard.word); }
    // flip 0..2 of the eight least reliable bits, then allow one more hard correction
    for (int mask = 1; mask < 256; mask++) {
        if (pop((uint32_t)mask) > 2) continue;
        uint32_t w = word;
        for (int k = 0; k < 8; k++) if ((mask >> k) & 1) w ^= 1u << idx[k];
        const CwResult r = correctCodeword(w, 1);
        if (!r.ok) continue;
        const float c = cost(word, r.word);
        if (c < bestCost) { bestCost = c; best = r; best.fixed = pop(word ^ r.word); }
    }
    return best;
}

uint32_t addressWord(uint32_t ric, int function, int* slot) {
    if (slot) *slot = (int)(ric & 7);
    const uint32_t data = (((ric >> 3) & 0x3FFFF) << 2) | (uint32_t)(function & 3);
    return encodeCodeword(data);                      // flag bit 0 = address
}

uint32_t messageWord(uint32_t data20) { return encodeCodeword(0x100000u | (data20 & 0xFFFFF)); }

std::string numericDigits(uint32_t data20) {
    std::string s;
    for (int i = 0; i < 5; i++) {
        const uint32_t n = (data20 >> (16 - 4 * i)) & 0xF;
        const uint32_t rev = ((n & 1) << 3) | ((n & 2) << 1) | ((n & 4) >> 1) | ((n & 8) >> 3);
        s.push_back(kNum[rev]);
    }
    return s;
}

std::vector<Page> pagesFromBurst(const Burst& b) {
    std::vector<Page> out;
    Page cur;
    bool open = false;
    auto flush = [&] { if (open) out.push_back(cur); open = false; };
    for (size_t i = 0; i < b.words.size(); i++) {
        const int slot = (int)((i % 16) / 2);
        CwResult c = b.rel.size() >= (i + 1) * 32 ? correctSoft(b.words[i], &b.rel[i * 32]) : correctCodeword(b.words[i], 2);
        if (c.ok && c.fixed >= 3) c.ok = false;      // three or more flips: the word is a guess, not a decode
        if (!c.ok) {
            if (open) { cur.missing++; cur.digits += "?????"; }
            continue;
        }
        const uint32_t w = c.word;
        if (w == kIdleWord) { flush(); continue; }
        if ((w >> 31) == 0) {
            flush();
            cur = Page();
            cur.ric = ((((w >> 13) & 0x3FFFF)) << 3) | (uint32_t)slot;
            cur.function = (int)((w >> 11) & 3);
            cur.fixed = c.fixed;
            cur.addrFixedTwo = c.fixed == 2;
            open = true;
        } else if (open) {
            cur.digits += numericDigits((w >> 11) & 0xFFFFF);
            cur.fixed += c.fixed;   // corrections in the extension matter too
        }
    }
    flush();
    return out;
}

} // namespace dect2::lbj
