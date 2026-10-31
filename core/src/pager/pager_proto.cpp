// POCSAG and FLEX at the bit level (see pager_proto.h).
#include "dect2/pager/pager_proto.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ctime>
#include "dect2/common/bits.h"

namespace dect2 {
namespace pager {

namespace {
const char kNumPocsag[] = "0123456789*U -][";
const char kNumFlex[] = "0123456789 U -][";
} // namespace

namespace {

PagerMessage newMessage() {
    PagerMessage m;
    m.wallTime = (int64_t)std::time(nullptr);
    return m;
}

} // namespace

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

void PocsagParser::reset() { hasTerm_ = false; have_ = false; damaged_ = false; gap_ = 0; bits_.clear(); raw_.clear(); ok = fixed = failed = 0; }

void PocsagParser::finish(std::vector<PagerMessage>& out) {
    gap_ = 0;                                       // a gap at the end of a page is not message text
    if (!have_) return;
    have_ = false;
    PagerMessage m = newMessage();
    if (combine) combineCopies(m.wallTime);
    m.copies = copies_;
    m.combined = combinedNow_;
    m.flex = false;
    m.address = ric_;
    m.function = fn_;
    m.fixed = fixedBits_;
    m.damaged = damaged_;
    if (bits_.empty()) {
        m.type = kPagerTone;
    } else {
        m.numText = numericFromBits(bits_, false);
        m.alphaText = alphaFromBits(bits_);
        m.looksAlpha = pocsagLooksAlpha(bits_);
        m.type = kPagerNumeric;
        pocsagApplyRead(m, readMode);
    }
    out.push_back(std::move(m));
}

// Multi-copy combining (#20): see pager_combine.h. raw_ holds the open message; on a match with an earlier copy the lost words are
// taken from it and bits_ / damaged_ are rebuilt from the merged words.
void PocsagParser::combineCopies(int64_t wall) {
    copies_ = 1; combinedNow_ = false;
    if (clockOverride >= 0) wall = clockOverride;
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [&](const Group& g) { return wall - g.wall > combineWindowSec || wall < g.wall; }), groups_.end());
    PocsagCopy cur;
    cur.words = raw_;
    cur.hasTerm = hasTerm_; cur.term = term_;
    Group* hit = nullptr;
    std::vector<const PocsagCopy*> use;
    use.push_back(&cur);
    for (Group& g : groups_) {
        if (g.ric != ric_ || g.fn != fn_) continue;
        for (size_t k = g.copies.size(); k-- > 0;)
            if (pocsagSameMessage(cur, g.copies[k])) { use.push_back(&g.copies[k]); hit = &g; }
        if (hit) break;
    }
    if (use.size() > 1) {
        copies_ = (int)use.size();
        const MergedCopy mc = mergePocsagCopies(use);
        if (mc.recovered > 0) {
            // rebuild the message bits; a word that is still lost leaves a gap, the lost tail is dropped
            bits_.clear(); fixedBits_ = 0; damaged_ = false;
            int gap = 0;
            for (size_t i = 1; i < mc.words.size(); i++) {
                const PocsagWord c = pocsagClassify(mc.words[i]);
                if (!mc.valid[i] || c.kind != PocsagWord::Data) { gap++; damaged_ = true; continue; }
                for (; gap > 0; gap--) for (int b = 0; b < 20; b++) bits_.push_back(0);
                for (int b = 19; b >= 0; b--) bits_.push_back((uint8_t)((c.a >> b) & 1));
                fixedBits_ += c.bits;
            }
            combinedNow_ = true;
            combinedMessages++;
            recoveredWords += mc.recovered;
        }
    }
    if (hit) {
        hit->wall = wall;
        hit->copies.push_back(cur);
        if (hit->copies.size() > 4) hit->copies.erase(hit->copies.begin());
    } else {
        Group g; g.ric = ric_; g.fn = fn_; g.wall = wall; g.copies.push_back(cur);
        groups_.push_back(std::move(g));
        if (groups_.size() > 64) groups_.erase(groups_.begin());
    }
}

void PocsagParser::clearCopies() { groups_.clear(); raw_.clear(); combinedMessages = recoveredWords = 0; }

void PocsagParser::flush(std::vector<PagerMessage>& out) { hasTerm_ = false; finish(out); }

void PocsagParser::word(uint32_t raw, int slot, std::vector<PagerMessage>& out) {
    const PocsagWord c = pocsagClassify(raw);
    if (c.kind == PocsagWord::Failed) {
        // a message word we cannot repair keeps the page open: its 20 bits become a gap, filled in when a later message word arrives
        failed++;
        if (have_) { damaged_ = true; gap_++; raw_.push_back(raw); }
        return;
    }
    if (c.fixed) fixed++; else ok++;
    if (c.kind == PocsagWord::Idle) { term_ = raw; hasTerm_ = true; finish(out); hasTerm_ = false; return; }
    if (c.kind == PocsagWord::Address) {
        term_ = raw; hasTerm_ = true;
        finish(out);
        hasTerm_ = false;
        have_ = true; damaged_ = false;
        ric_ = (c.a << 3) | (uint32_t)(slot >> 1);
        fn_ = (int)c.b;
        fixedBits_ = c.bits;
        bits_.clear();
        raw_.assign(1, raw);
    } else if (have_) {
        raw_.push_back(raw);
        for (; gap_ > 0; gap_--) for (int i = 0; i < 20; i++) bits_.push_back(0);     // keeps the later bits where they belong
        for (int i = 19; i >= 0; i--) bits_.push_back((uint8_t)((c.a >> i) & 1));
        fixedBits_ += c.bits;
    }
}
// ---------------------------------------------------------------- FLEX

uint32_t flexWithCheck(uint32_t f) {
    f &= 0x1FFFF0;
    int s = 0;
    for (int i = 4; i < 20; i += 4) s += (int)((f >> i) & 0xF);
    s += (int)((f >> 20) & 1);
    return f | (uint32_t)((0xF - s) & 0xF);
}

bool flexLongCap(uint32_t cap) { return cap > kFlexLongCapBase; }

void flexLongWords(uint32_t cap, uint32_t& w1, uint32_t& w2) {
    // the inverse of the decoder's cap = w1 + ((w2 ^ 0x1FFFFF) << 15) + 0x1F9000, with w1 from 1 to 0x8000 (below the short addresses)
    uint32_t v = cap - 0x1F9000u;
    uint32_t a = v & 0x7FFF;
    if (a == 0) { a = 0x8000; v -= 0x8000; }
    w1 = a;
    w2 = ((v >> 15) ^ 0x1FFFFFu) & 0x1FFFFF;
}

std::vector<uint8_t> flexPhaseBits(const std::vector<FlexPage>& pages) {
    uint32_t f[kFlexPhaseWords] = {};
    const int n = (int)pages.size();
    if (n > 0) {
        int naw = 0;                               // address words (two for a long address), and as many vector words
        for (const FlexPage& pg : pages) naw += flexLongCap(pg.cap) ? 2 : 1;
        const int aoff = 1, voff = 1 + naw;
        f[0] = flexWithCheck(((uint32_t)voff << 10) | ((uint32_t)(aoff - 1) << 8));
        int next = voff + naw;                     // the next free message word
        int ai = aoff;
        for (int k = 0; k < n; k++) {
            const FlexPage& pg = pages[(size_t)k];
            const bool lng = flexLongCap(pg.cap);
            const int slot = ai;
            if (lng) { flexLongWords(pg.cap, f[ai], f[ai + 1]); ai += 2; }
            else f[ai++] = pg.cap + 0x8000;
            const int vi = voff + (slot - aoff);   // the vector word(s) of this address
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
                const int inField = lng ? words - 1 : words;                  // a long address carries the first numeric word in its second vector word
                if (next + inField > kFlexPhaseWords) continue;
                uint32_t mw[8] = {};
                for (int w = 0; w < words; w++)
                    for (int i = 0; i < 21; i++) {
                        const size_t idx = (size_t)(w * 21 + i);
                        if (idx < bits.size() && bits[idx]) mw[w] |= 1u << i;
                    }
                for (int w = 0; w < words; w++) {
                    if (lng && w == 0) f[vi + 1] = mw[0];
                    else f[next + w - (lng ? 1 : 0)] = mw[w];
                }
                vec = (3u << 4) | ((uint32_t)next << 7) | ((uint32_t)(words - 1) << 14);
                next += inField;
            } else if (pg.type == kPagerAlpha) {
                std::vector<int> ch;
                for (char c : pg.text) ch.push_back((unsigned char)c & 0x7F);
                const int nc = (int)ch.size();
                const int words = 1 + (std::max(nc - 2, 0) + 2) / 3;          // the first message word holds two characters, the others three
                const int len = 1 + words;                                    // and the header word counts in the length
                const int inField = lng ? words : len;                        // a long address carries the header in its second vector word
                if (next + inField > kFlexPhaseWords) continue;
                auto at = [&](int i) { return i < nc ? ch[(size_t)i] : 3; };       // 3 pads
                const uint32_t hdr = 3u << 11;                                // header: complete message (continuation 0, fragment 3)
                const int t0 = lng ? next : next + 1;                         // the first text word
                if (lng) f[vi + 1] = hdr; else f[next] = hdr;
                f[t0] = ((uint32_t)at(0) << 7) | ((uint32_t)at(1) << 14);     // bits 0 to 6 of this word are not text
                for (int w = 1; w < words; w++) {
                    const int c0 = 2 + 3 * (w - 1);
                    f[t0 + w] = (uint32_t)at(c0) | ((uint32_t)at(c0 + 1) << 7) | ((uint32_t)at(c0 + 2) << 14);
                }
                vec = (5u << 4) | ((uint32_t)next << 7) | ((uint32_t)len << 14);
                next += inField;
            } else {
                vec = (2u << 4) | (1u << 7);       // tone only (bits 7 and 8 = 1; 0 would be a short numeric page)
            }
            f[vi] = flexWithCheck(vec);
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

} // namespace pager
} // namespace dect2
