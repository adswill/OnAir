// POCSAG and FLEX at the bit level (see pager_proto.h).
#include "dect2/pager_proto.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>

namespace dect2 {
namespace pager {

namespace {

constexpr uint32_t kGen = 0x769;               // x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1

inline int parity32(uint32_t v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (int)(v & 1); }

// remainder of a 31-bit polynomial divided by the generator
uint32_t remainder31(uint32_t c) {
    for (int i = 30; i >= 10; i--)
        if ((c >> i) & 1) c ^= kGen << (i - 10);
    return c & 0x3FF;
}

// syndrome -> the positions (0 to 30, from the least significant bit) of one or two wrong bits; 255 = none
struct SynTable {
    std::array<uint8_t, 1024> p1, p2;
    SynTable() {
        p1.fill(255); p2.fill(255);
        uint32_t s[31];
        for (int i = 0; i < 31; i++) s[i] = remainder31(1u << i);
        for (int i = 0; i < 31; i++) p1[s[i]] = (uint8_t)i;
        for (int i = 0; i < 31; i++)
            for (int j = i + 1; j < 31; j++) {
                const uint32_t x = s[i] ^ s[j];
                if (p1[x] == 255 && p2[x] == 255) { p1[x] = (uint8_t)i; p2[x] = (uint8_t)j; }
            }
    }
};

const SynTable& synTable() { static const SynTable t; return t; }

const char kNumPocsag[] = "0123456789*U -][";
const char kNumFlex[] = "0123456789 U -][";

std::string alphaFromBits(const std::vector<uint8_t>& bits) {
    std::string s;
    for (size_t i = 0; i + 7 <= bits.size(); i += 7) {
        int c = 0;
        for (int k = 0; k < 7; k++) c |= (bits[i + (size_t)k] & 1) << k;       // the first bit is the least significant
        if (c >= 0x20 && c < 0x7F) s += (char)c;
        else if (c == '\n' || c == '\r' || c == '\t') s += (char)c;
        else if (c == 0 || c == 4 || c == 3) s += '\0';                         // padding: dropped below
    }
    while (!s.empty() && s.back() == '\0') s.pop_back();
    s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
    return s;
}

std::string numericFromBits(const std::vector<uint8_t>& bits, const char* table, bool dropFill) {
    std::string s;
    for (size_t i = 0; i + 4 <= bits.size(); i += 4) {
        int d = 0;
        for (int k = 0; k < 4; k++) d |= (bits[i + (size_t)k] & 1) << k;
        if (dropFill && d == 12) continue;
        s += table[d];
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

PagerMessage newMessage() {
    PagerMessage m;
    m.wallTime = (int64_t)std::time(nullptr);
    return m;
}

} // namespace

uint32_t rev21(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 21; i++) r |= ((v >> i) & 1u) << (20 - i);
    return r;
}

uint32_t encodeWord(uint32_t data21) {
    uint32_t c = (data21 & 0x1FFFFF) << 10;
    c |= remainder31(c);
    return (c << 1) | (uint32_t)parity32(c);
}

Fixed correctWord(uint32_t w) {
    Fixed f;
    uint32_t c = w >> 1;
    const uint32_t s = remainder31(c);
    int n = 0;
    if (s) {
        const SynTable& t = synTable();
        if (t.p1[s] == 255) return f;
        c ^= 1u << t.p1[s]; n++;
        if (t.p2[s] != 255) { c ^= 1u << t.p2[s]; n++; }
    }
    const int pbad = (w & 1) != (uint32_t)parity32(c) ? 1 : 0;
    if (n + pbad > 2) return f;
    f.ok = true; f.bits = n + pbad; f.data = c >> 10;
    return f;
}

// ---------------------------------------------------------------- POCSAG

std::vector<uint8_t> pocsagBits(const std::vector<PocsagPage>& pages, int preambleBits) {
    std::vector<std::array<uint32_t, 16>> batches;
    std::array<uint32_t, 16> blank;
    blank.fill(kPocsagIdle);
    size_t b = 0;
    int p = 0;
    batches.push_back(blank);
    auto place = [&](uint32_t w) {
        if (p == 16) { batches.push_back(blank); b++; p = 0; }
        batches[b][(size_t)p++] = w;
    };
    for (const PocsagPage& pg : pages) {
        const int slot = 2 * (int)(pg.ric & 7);
        if (p > slot) { batches.push_back(blank); b++; p = 0; }
        p = slot;
        place(encodeWord((((pg.ric >> 3) & 0x3FFFF) << 2) | (uint32_t)(pg.fn & 3)));
        std::vector<uint8_t> bits;
        if (pg.type == kPagerNumeric) {
            for (char ch : pg.text) {
                const char* q = std::strchr(kNumPocsag, ch);
                const int d = (q && ch) ? (int)(q - kNumPocsag) : 12;
                for (int k = 0; k < 4; k++) bits.push_back((uint8_t)((d >> k) & 1));
            }
            while (bits.size() % 20 || bits.empty()) for (int k = 0; k < 4; k++) bits.push_back((uint8_t)((12 >> k) & 1));
        } else if (pg.type == kPagerAlpha) {
            auto put = [&](int c) { for (int k = 0; k < 7; k++) bits.push_back((uint8_t)((c >> k) & 1)); };
            for (char ch : pg.text) put((unsigned char)ch & 0x7F);
            put(4);
            while (bits.size() % 20) bits.push_back(0);
        }
        for (size_t i = 0; i < bits.size(); i += 20) {
            uint32_t d = 1u << 20;
            for (size_t k = 0; k < 20; k++) d |= (uint32_t)bits[i + k] << (19 - k);
            place(encodeWord(d));
        }
    }
    std::vector<uint8_t> out;
    for (int i = 0; i < preambleBits; i++) out.push_back((uint8_t)((i & 1) ^ 1));      // 1010...
    auto put32 = [&](uint32_t w) { for (int i = 31; i >= 0; i--) out.push_back((uint8_t)((w >> i) & 1)); };
    for (const auto& bt : batches) {
        put32(kPocsagSync);
        for (uint32_t w : bt) put32(w);
    }
    return out;
}

void PocsagParser::reset() { have_ = false; damaged_ = false; bits_.clear(); ok = fixed = failed = 0; }

void PocsagParser::finish(std::vector<PagerMessage>& out) {
    if (!have_) return;
    have_ = false;
    PagerMessage m = newMessage();
    m.flex = false;
    m.address = ric_;
    m.function = fn_;
    m.fixed = fixedBits_;
    m.damaged = damaged_;
    if (bits_.empty()) {
        m.type = kPagerTone;
    } else if (fn_ == 0) {
        m.type = kPagerNumeric;
        m.text = numericFromBits(bits_, kNumPocsag, false);
    } else {
        m.type = kPagerAlpha;
        m.text = alphaFromBits(bits_);
    }
    out.push_back(std::move(m));
}

void PocsagParser::flush(std::vector<PagerMessage>& out) { finish(out); }

void PocsagParser::word(uint32_t raw, int slot, std::vector<PagerMessage>& out) {
    const Fixed c = correctWord(raw);
    if (!c.ok) {
        failed++;
        if (have_) { damaged_ = true; finish(out); }
        return;
    }
    if (c.bits) fixed++; else ok++;
    if (c.data == (kPocsagIdle >> 11) && encodeWord(c.data) == kPocsagIdle) { finish(out); return; }
    if ((c.data >> 20) == 0) {
        finish(out);
        have_ = true; damaged_ = false;
        ric_ = (((c.data >> 2) & 0x3FFFF) << 3) | (uint32_t)(slot >> 1);
        fn_ = (int)(c.data & 3);
        fixedBits_ = c.bits;
        bits_.clear();
    } else if (have_) {
        for (int i = 19; i >= 0; i--) bits_.push_back((uint8_t)((c.data >> i) & 1));
        fixedBits_ += c.bits;
    }
}

// ---------------------------------------------------------------- FLEX

namespace {
// 0x4C7C is a second code for 6400 bits/s that some transmitters use
const FlexMode kModes[] = {
    {0x870C, 1600, 2, kFlex1600_2}, {0x7B18, 3200, 2, kFlex3200_2}, {0xB068, 1600, 4, kFlex3200_4}, {0xDEA0, 3200, 4, kFlex6400_4}, {0x4C7C, 3200, 4, kFlex6400_4},
};
inline int pop16(uint32_t v) { int n = 0; for (; v; v &= v - 1) n++; return n; }
}

const FlexMode* flexModeForCode(uint16_t code, int maxDistance) {
    const FlexMode* best = nullptr;
    int bd = maxDistance + 1;
    for (const FlexMode& m : kModes) {
        const int d = pop16((uint32_t)(m.code ^ code));
        if (d < bd) { bd = d; best = &m; }
    }
    return best;
}

const FlexMode* flexModeForSpeed(int speed) {
    for (const FlexMode& m : kModes) if (m.speed == speed) return &m;
    return nullptr;
}

static int fiwSum(uint32_t f) {
    int s = 0;
    for (int i = 0; i < 20; i += 4) s += (int)((f >> i) & 0xF);
    return s + (int)((f >> 20) & 1);
}

uint32_t flexFiwData(int cycle, int frame) {
    uint32_t f = ((uint32_t)(cycle & 15) << 4) | ((uint32_t)(frame & 127) << 8);
    f |= (uint32_t)((0xF - fiwSum(f)) & 0xF);
    return f;
}

bool flexFiwCheck(uint32_t f, int& cycle, int& frame) {
    if ((fiwSum(f) & 0xF) != 0xF) return false;
    cycle = (int)((f >> 4) & 15);
    frame = (int)((f >> 8) & 127);
    return true;
}

std::vector<uint8_t> flexPhaseBits(const std::vector<FlexPage>& pages) {
    uint32_t f[kFlexPhaseWords] = {};
    const int n = (int)pages.size();
    if (n > 0) {
        const int aoff = 1, voff = 1 + n;
        f[0] = ((uint32_t)voff << 10) | ((uint32_t)(aoff - 1) << 8);
        int next = voff + n;                       // the next free message word
        for (int k = 0; k < n; k++) {
            const FlexPage& pg = pages[(size_t)k];
            f[aoff + k] = pg.cap + 0x8000;
            uint32_t vec = 0;
            if (pg.type == kPagerNumeric) {
                std::vector<uint8_t> bits(2, 0);   // two header bits
                for (char ch : pg.text) {
                    const char* q = std::strchr(kNumFlex, ch);
                    const int d = (q && ch) ? (int)(q - kNumFlex) : 10;
                    for (int i = 0; i < 4; i++) bits.push_back((uint8_t)((d >> i) & 1));
                }
                const int words = std::min(8, ((int)bits.size() + 20) / 21);
                while ((int)bits.size() + 4 <= words * 21) for (int i = 0; i < 4; i++) bits.push_back((uint8_t)((12 >> i) & 1));
                if (next + words > kFlexPhaseWords) continue;
                for (int w = 0; w < words; w++)
                    for (int i = 0; i < 21; i++) {
                        const size_t idx = (size_t)(w * 21 + i);
                        if (idx < bits.size() && bits[idx]) f[next + w] |= 1u << i;
                    }
                vec = (3u << 4) | ((uint32_t)next << 7) | ((uint32_t)(words - 1) << 14);
                next += words;
            } else if (pg.type == kPagerAlpha) {
                std::vector<int> ch;
                for (char c : pg.text) ch.push_back((unsigned char)c & 0x7F);
                const int nc = (int)ch.size();
                const int len = 1 + (std::max(nc, 1) - 1 + 2) / 3;
                if (next + len > kFlexPhaseWords) continue;
                auto at = [&](int i) { return i < nc ? ch[(size_t)i] : 3; };       // 3 pads
                f[next] = (3u << 11) | ((uint32_t)at(0) << 14);                  // complete message (K), first character
                for (int w = 1; w < len; w++) {
                    const int c0 = 1 + 3 * (w - 1);
                    f[next + w] = (uint32_t)at(c0) | ((uint32_t)at(c0 + 1) << 7) | ((uint32_t)at(c0 + 2) << 14);
                }
                vec = (5u << 4) | ((uint32_t)next << 7) | ((uint32_t)len << 14);
                next += len;
            } else {
                vec = 2u << 4;                     // tone only
            }
            f[voff + k] = vec;
        }
    }
    std::vector<uint8_t> bits((size_t)kFlexPhaseWords * 32);
    for (int blk = 0; blk < 11; blk++)
        for (int w = 0; w < 8; w++) {
            const uint32_t W = encodeWord(rev21(f[blk * 8 + w]));
            for (int j = 0; j < 32; j++) bits[(size_t)(blk * 256 + j * 8 + w)] = (uint8_t)((W >> (31 - j)) & 1);
        }
    return bits;
}

void flexDecodePhase(const uint8_t* bits, std::vector<PagerMessage>& out, FlexPhaseStat& st) {
    uint32_t f[kFlexPhaseWords];
    int fx[kFlexPhaseWords];
    for (int blk = 0; blk < 11; blk++)
        for (int w = 0; w < 8; w++) {
            uint32_t W = 0;
            for (int j = 0; j < 32; j++) W = (W << 1) | (bits[blk * 256 + j * 8 + w] & 1u);
            const Fixed c = correctWord(W);
            const int i = blk * 8 + w;
            if (c.ok) { f[i] = rev21(c.data); fx[i] = c.bits; if (c.bits) st.fixed++; else st.ok++; }
            else { f[i] = 0; fx[i] = -1; st.failed++; }
        }
    if (fx[0] < 0 || f[0] == 0 || f[0] == 0x1FFFFF) return;
    st.biwOk = true;
    const int aoff = (int)((f[0] >> 8) & 3) + 1, voff = (int)((f[0] >> 10) & 0x3F);
    if (voff < aoff || voff >= kFlexPhaseWords) return;
    int i = aoff, j = voff;
    while (i < voff && j < kFlexPhaseWords) {
        const uint32_t a = f[i];
        if (fx[i] < 0 || a == 0 || a == 0x1FFFFF) { i++; j++; continue; }
        const bool longAddr = a < 0x8001 || a > 0x1E0000;
        int used = 1;
        uint32_t cap = a - 0x8000;
        int fixedBits = fx[i];
        if (longAddr) {
            // a long address takes two words (and two vector words); the capcode formula is an approximation
            if (i + 1 >= voff || fx[i + 1] < 0) { i += 2; j += 2; continue; }
            used = 2;
            cap = a + ((f[i + 1] ^ 0x1FFFFFu) << 15);
            fixedBits += fx[i + 1];
        }
        if (j >= kFlexPhaseWords || fx[j] < 0) { i += used; j += used; continue; }
        const uint32_t viw = f[j];
        fixedBits += fx[j];
        i += used; j += used;
        const int vtype = (int)((viw >> 4) & 7), mw1 = (int)((viw >> 7) & 0x7F);
        PagerMessage m = newMessage();
        m.flex = true;
        m.address = cap;
        m.function = -1;
        if (vtype == 2) {
            m.type = kPagerTone;
        } else if (vtype == 5 || vtype == 0) {
            const int len = (int)((viw >> 14) & 0x7F);
            if (len < 1 || mw1 >= kFlexPhaseWords) continue;
            const int mw2 = std::min(mw1 + len - 1, kFlexPhaseWords - 1);
            if (vtype == 0) { m.type = kPagerSecure; m.text = "secure, not shown"; }
            else {
                m.type = kPagerAlpha;
                std::vector<uint8_t> bits7;
                auto put = [&](uint32_t c) { for (int k = 0; k < 7; k++) bits7.push_back((uint8_t)((c >> k) & 1)); };
                if (fx[mw1] < 0) m.damaged = true; else { put((f[mw1] >> 14) & 0x7F); fixedBits += fx[mw1]; }
                for (int w = mw1 + 1; w <= mw2; w++) {
                    if (fx[w] < 0) { m.damaged = true; continue; }
                    fixedBits += fx[w];
                    put(f[w] & 0x7F); put((f[w] >> 7) & 0x7F); put((f[w] >> 14) & 0x7F);
                }
                m.text = alphaFromBits(bits7);
            }
        } else if (vtype == 3 || vtype == 4 || vtype == 7) {
            const int cnt = (int)((viw >> 14) & 7) + 1;
            if (mw1 >= kFlexPhaseWords) continue;
            std::vector<uint8_t> nb;
            int skip = vtype == 7 ? 10 : 2;
            for (int w = mw1; w < mw1 + cnt && w < kFlexPhaseWords; w++) {
                if (fx[w] < 0) { m.damaged = true; continue; }
                fixedBits += fx[w];
                for (int k = 0; k < 21; k++) {
                    if (skip > 0) { skip--; continue; }
                    nb.push_back((uint8_t)((f[w] >> k) & 1));
                }
            }
            m.type = kPagerNumeric;
            m.text = numericFromBits(nb, kNumFlex, true);
        } else if (vtype == 6) {
            m.type = kPagerBinary;
            m.text = "binary data";
        } else {
            continue;                              // short instruction (group messages): nothing to show
        }
        m.fixed = fixedBits;
        out.push_back(std::move(m));
    }
}

} // namespace pager
} // namespace dect2
