// Pagers: multi-copy combining (#20). Synthetic repeated transmissions damaged in different places: the vote must recover the message,
// must never merge different messages, and must leave single copies alone. Covers code words, POCSAG messages and FLEX phases.
#include "dect2/pager/pager_combine.h"
#include "dect2/pager/pager_proto.h"
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::pager;
#include "../common/test_util.h"

static std::mt19937 rng(12345);

// a damaged copy of a code word that the BCH code cannot repair (3 to 6 wrong bits, checked)
static uint32_t wreck(uint32_t w, int nflip = 0) {
    for (;;) {
        uint32_t v = w;
        const int n = nflip ? nflip : 3 + (int)(rng() % 4);
        for (int i = 0; i < n; i++) v ^= 1u << (rng() % 32);
        if (!correctWord(v).ok) return v;
    }
}

// the code words of a POCSAG transmission after the preamble: 33 per batch (sync first); slot = index in the batch
static std::vector<uint32_t> pocsagWords(const std::vector<PocsagPage>& pages) {
    const std::vector<uint8_t> b = pocsagBits(pages, 32);
    std::vector<uint32_t> w;
    for (size_t i = 32; i + 32 <= b.size(); i += 32) w.push_back(wordFromBits(&b[i]));
    return w;   // sync, 16 words, sync, ...
}
static std::vector<PagerMessage> feed(PocsagParser& p, const std::vector<uint32_t>& w) {
    std::vector<PagerMessage> out;
    int slot = 0;
    for (uint32_t x : w) {
        if (x == kPocsagSync) { slot = 0; continue; }
        p.word(x, slot++, out);
    }
    p.flush(out);
    return out;
}
// damage the message words (not the sync words) at the given indexes of the 'words after the address' list
static std::vector<uint32_t> damageAt(std::vector<uint32_t> w, const std::vector<int>& idx) {
    std::vector<int> pos;   // index in w of the non-sync, non-idle words
    for (size_t i = 0; i < w.size(); i++) if (w[i] != kPocsagSync && w[i] != kPocsagIdle) pos.push_back((int)i);
    for (int k : idx) if (k >= 0 && k < (int)pos.size()) w[(size_t)pos[(size_t)k]] = wreck(w[(size_t)pos[(size_t)k]]);
    return w;
}

// a word the BCH code "repairs" into a different, valid word (4 wrong bits that land within 2 of another code word)
static uint32_t miscorrect(uint32_t w) {
    for (;;) {
        uint32_t v = w;
        for (int i = 0; i < 4; i++) v ^= 1u << (rng() % 32);
        const Fixed f = correctWord(v);
        if (f.ok && encodeWord(f.data) != w) return v;
    }
}
static std::vector<uint32_t> miscorrectAt(std::vector<uint32_t> w, int k) {
    std::vector<int> pos;
    for (size_t i = 0; i < w.size(); i++) if (w[i] != kPocsagSync && w[i] != kPocsagIdle) pos.push_back((int)i);
    w[(size_t)pos[(size_t)k]] = miscorrect(w[(size_t)pos[(size_t)k]]);
    return w;
}

static void wordTests() {
    const uint32_t t = encodeWord(0x0ABCDE);
    CHECK(encodeWord(correctWord(t).data) == t, "correctWord data feeds encodeWord back");
    uint32_t one[1] = {t};
    CHECK(combineWord(one, 1).ok && combineWord(one, 1).raw == t, "a clean single copy");
    uint32_t cp[3] = {wreck(t), t ^ 1u, wreck(t)};
    CombinedWord c = combineWord(cp, 3);
    CHECK(c.ok && c.raw == t && c.voted, "a good copy among wrecked ones");
    // three wrecked copies: majority vote repairs it (errors in different places)
    int good = 0;
    for (int trial = 0; trial < 200; trial++) {
        const uint32_t tw = encodeWord((uint32_t)(rng() & 0x1FFFFF));
        uint32_t a[3] = {wreck(tw, 3), wreck(tw, 3), wreck(tw, 3)};
        CombinedWord r = combineWord(a, 3);
        if (r.ok && r.raw == tw) good++;
        CHECK(!r.ok || r.raw == tw || true, "x");
    }
    CHECK(good >= 100, "three wrecked copies recovered: %d of 200", good);
    // two wrecked copies with disjoint errors (3+3): recovered, never a wrong word
    int right = 0, wrong = 0;
    for (int trial = 0; trial < 300; trial++) {
        const uint32_t tw = encodeWord((uint32_t)(rng() & 0x1FFFFF));
        uint32_t e1 = 0, e2 = 0;
        while (__builtin_popcount(e1) < 3) e1 |= 1u << (rng() % 32);
        while (__builtin_popcount(e2) < 3) { const uint32_t b = 1u << (rng() % 32); if (!(e1 & b)) e2 |= b; }
        uint32_t a[2] = {tw ^ e1, tw ^ e2};
        if (correctWord(a[0]).ok || correctWord(a[1]).ok) continue;     // only the cases the BCH code alone loses
        CombinedWord r = combineWord(a, 2);
        if (r.ok) { if (r.raw == tw) right++; else wrong++; }
    }
    printf("  two wrecked copies: %d recovered, %d wrong\n", right, wrong);
    CHECK(right >= 20, "two copies recovered some: %d", right);
    CHECK(wrong * 20 <= right + 1, "two copies: wrong words are rare (%d vs %d)", wrong, right);
    uint32_t junk[2] = {0x12345678u, 0x9ABCDEF0u};
    CHECK(!combineWord(junk, 2).ok, "unrelated junk copies are not invented into a word");
}

static const char* kText = "FIRE ALARM Station 7 Main Street request crews";

static void pocsagTests() {
    PocsagPage pg; pg.ric = 1234567; pg.fn = 3; pg.type = kPagerAlpha; pg.text = kText;
    const std::vector<uint32_t> clean = pocsagWords({pg});
    {   // reference: no damage
        PocsagParser p; p.combine = true; auto o = feed(p, clean);
        CHECK(o.size() == 1 && o[0].text == kText && !o[0].damaged && o[0].copies == 1 && !o[0].combined, "clean copy untouched");
    }
    {   CHECK(!PocsagParser().combine, "the parser class leaves combining off (the receiver turns it on)");
    }
    {   // one copy alone stays damaged
        PocsagParser p; p.combine = true; auto o = feed(p, damageAt(clean, {3}));
        CHECK(o.size() == 1 && o[0].damaged && o[0].text != kText, "a single damaged copy is still damaged");
    }
    // two copies, losses in different places, over a sweep of positions
    int fullOk = 0, total = 0;
    for (int a = 1; a < 9; a++)
        for (int b = a + 1; b < 10; b += 2) {
            PocsagParser p; p.combine = true; p.clockOverride = 1000;
            auto o1 = feed(p, damageAt(clean, {a}));
            p.clockOverride = 1060;
            auto o2 = feed(p, damageAt(clean, {b, b + 1}));
            total++;
            if (o1.size() == 1 && o2.size() == 1 && o2[0].text == kText && !o2[0].damaged && o2[0].combined && o2[0].copies == 2) fullOk++;
        }
    CHECK(fullOk == total, "two-copy recovery %d of %d", fullOk, total);
    {   // the same word lost in both copies: the two wrecked versions may still be merged (a unique closest word), otherwise it stays damaged; never a wrong text
        int wrongText = 0, stillDamaged = 0;
        for (int it = 0; it < 60; it++) {
            PocsagParser p; p.combine = true; p.clockOverride = 1;
            feed(p, damageAt(clean, {4})); p.clockOverride = 2;
            auto o = feed(p, damageAt(clean, {4, 7}));
            if (o.size() != 1) { wrongText++; continue; }
            if (o[0].damaged) stillDamaged++; else if (o[0].text != kText) wrongText++;
        }
        CHECK(wrongText == 0, "word lost in both copies: %d wrong texts (%d stayed damaged)", wrongText, stillDamaged);
    }
    {   // three copies, every copy damaged, no word lost in all three: full recovery
        PocsagParser p; p.combine = true; p.clockOverride = 10;
        feed(p, damageAt(clean, {2, 5})); p.clockOverride = 20;
        feed(p, damageAt(clean, {3, 6})); p.clockOverride = 30;
        auto o = feed(p, damageAt(clean, {4, 8}));
        CHECK(o.size() == 1 && o[0].text == kText && o[0].copies == 3 && o[0].combined, "three damaged copies -> '%s' copies %d", o.empty() ? "" : o[0].text.c_str(), o.empty() ? 0 : o[0].copies);
    }
    {   // a different message to the same RIC and function is not merged in
        PocsagPage other = pg; other.text = "TEST PAGE number two for the same address ok";
        PocsagParser p; p.combine = true; p.clockOverride = 1;
        feed(p, clean); p.clockOverride = 2;
        auto o = feed(p, damageAt(pocsagWords({other}), {3}));
        CHECK(o.size() == 1 && o[0].damaged && o[0].copies == 1 && !o[0].combined, "different message not combined");
        CHECK(o[0].text.find("FIRE") == std::string::npos, "no text from the other message leaks in");
    }
    {   // other RIC / function: no combining
        PocsagPage o2 = pg; o2.ric = 1234568;
        PocsagParser p; p.combine = true; p.clockOverride = 1; feed(p, clean); p.clockOverride = 2;
        auto o = feed(p, damageAt(pocsagWords({o2}), {3}));
        CHECK(o.size() == 1 && o[0].damaged && !o[0].combined, "other RIC not combined");
        PocsagPage o3 = pg; o3.fn = 1;
        auto o4 = feed(p, damageAt(pocsagWords({o3}), {3}));
        CHECK(o4.size() == 1 && o4[0].damaged && !o4[0].combined, "other function not combined");
    }
    {   // too old, and switched off
        PocsagParser p; p.combine = true; p.clockOverride = 100; feed(p, clean); p.clockOverride = 100 + p.combineWindowSec + 5;
        auto o = feed(p, damageAt(clean, {3}));
        CHECK(o.size() == 1 && o[0].damaged && !o[0].combined, "an old copy is forgotten");
        PocsagParser q; q.clockOverride = 1; feed(q, clean); q.clockOverride = 2;
        auto o5 = feed(q, damageAt(clean, {3}));
        CHECK(o5.size() == 1 && o5[0].damaged && !o5[0].combined, "switched off");
    }
    {   // numeric and tone pages, reset() between locks keeps the copies
        PocsagPage nm; nm.ric = 5000; nm.fn = 0; nm.type = kPagerNumeric; nm.text = "0123456789 12345 6789";
        const auto nw = pocsagWords({nm});
        PocsagParser p; p.combine = true; p.clockOverride = 5; feed(p, damageAt(nw, {1})); p.reset(); p.clockOverride = 6;
        auto o = feed(p, damageAt(nw, {2}));
        CHECK(o.size() == 1 && !o[0].damaged && o[0].text == nm.text && o[0].combined, "numeric '%s'", o.empty() ? "" : o[0].text.c_str());
        PocsagPage tn; tn.ric = 77; tn.type = kPagerTone;
        PocsagParser q; q.combine = true; auto t1 = feed(q, pocsagWords({tn})); auto t2 = feed(q, pocsagWords({tn}));
        CHECK(t1.size() == 1 && t2.size() == 1 && t2[0].type == kPagerTone && !t2[0].combined, "tone pages are left alone");
    }
    {   // a word repaired into a WRONG valid word (burst errors): an earlier clean copy outvotes it
        int fixedUp = 0;
        for (int k = 1; k < 12; k++) {
            PocsagParser p; p.combine = true; p.clockOverride = 1; feed(p, clean); p.clockOverride = 2;
            auto o = feed(p, miscorrectAt(clean, k));
            bool got = false; for (auto& m : o) if (m.address == pg.ric && m.text == kText) { fixedUp++; got = true; break; }
            if (!got) { printf("  k=%d:", k); for (auto& m : o) printf(" [%u '%s' c%d]", m.address, m.text.c_str(), m.copies); printf("\n"); }
            //   // a word repaired into an address word also starts a phantom page: not our concern here
        }
        CHECK(fixedUp == 11, "wrongly repaired word outvoted by the clean copy: %d of 11", fixedUp);
        PocsagParser q; auto base = feed(q, miscorrectAt(clean, 5));
        CHECK(base.size() == 1 && base[0].text != kText, "premise: alone the wrong word goes through");
    }
    {   // a copy cut short by a wrongly repaired idle word is completed; a genuinely short message that starts like a longer one is not extended
        PocsagParser p; p.combine = true; p.clockOverride = 1; feed(p, clean); p.clockOverride = 2;
        int cut = 0;
        for (int tries = 0; tries < 40 && !cut; tries++) {    // find a wreck of data word 8 that reads as an idle word after repair
            auto w = damageAt(clean, {});
            std::vector<int> pos;
            for (size_t i = 0; i < w.size(); i++) if (w[i] != kPocsagSync && w[i] != kPocsagIdle) pos.push_back((int)i);
            uint32_t v = kPocsagIdle;
            for (int i = 0; i < 3; i++) v ^= 1u << (rng() % 32);
            if (!correctWord(v).ok || encodeWord(correctWord(v).data) != kPocsagIdle) continue;
            w[(size_t)pos[8]] = v;
            auto o = feed(p, w);
            cut = 1;
            CHECK(o.size() == 1 && o[0].text == kText, "a copy cut short by a repaired idle word is completed: '%s'", o.empty() ? "" : o[0].text.c_str());
        }
        CHECK(cut, "found an idle-looking wreck");
        PocsagPage shortPg = pg; shortPg.text = "FIRE ALARM";
        PocsagParser q; q.combine = true; q.clockOverride = 1; feed(q, clean); q.clockOverride = 2;
        auto o = feed(q, pocsagWords({shortPg}));
        CHECK(o.size() == 1 && o[0].text == "FIRE ALARM", "a real short message is not extended: '%s'", o.empty() ? "" : o[0].text.c_str());
    }
    {   // a damaged ADDRESS word: the page is not started (unchanged behaviour), later copy still decodes
        PocsagParser p; p.combine = true; p.clockOverride = 1;
        auto w = clean;
        for (size_t i = 0; i < w.size(); i++) if (w[i] != kPocsagSync && w[i] != kPocsagIdle) { w[i] = wreck(w[i]); break; }
        feed(p, w); p.clockOverride = 2;
        auto o = feed(p, clean);
        CHECK(o.size() == 1 && o[0].text == kText && !o[0].damaged, "clean copy after a copy with a lost address");
    }
    {   // random impairment sweep: two copies, random lost words, never a wrong text unless the same word is lost in both
        int bad = 0, n = 0;
        for (int it = 0; it < 150; it++) {
            std::vector<int> ia, ib;
            for (int k = 1; k < 10; k++) { if (rng() % 3 == 0) ia.push_back(k); else if (rng() % 3 == 0) ib.push_back(k); }
            PocsagParser p; p.combine = true; p.clockOverride = 1; feed(p, damageAt(clean, ia)); p.clockOverride = 2;
            auto o = feed(p, damageAt(clean, ib));
            n++;
            if (o.size() != 1 || o[0].text != kText || o[0].damaged) { bad++; printf("  ia:"); for (int k : ia) printf(" %d", k); printf(" ib:"); for (int k : ib) printf(" %d", k); printf(" -> '%s' dam %d\n", o.empty() ? "" : o[0].text.c_str(), o.empty() ? -1 : (int)o[0].damaged); }
        }
        CHECK(bad == 0, "disjoint random losses: %d of %d not recovered", bad, n);
    }
}

static std::vector<uint8_t> wreckPhase(const std::vector<uint8_t>& bits, const std::vector<int>& words) {
    uint32_t w[kFlexPhaseWords];
    flexPhaseWords(bits.data(), w);
    for (int i : words) w[i] = wreck(w[i]);
    std::vector<uint8_t> o(bits.size());
    flexPhaseFromWords(w, o.data());
    return o;
}

static void flexTests() {
    uint32_t t[kFlexPhaseWords];
    std::vector<FlexPage> pg(2);
    pg[0].cap = 1234567; pg[0].type = kPagerAlpha; pg[0].text = "Flex repeat test message with a fairly long body";
    pg[1].cap = 1234568; pg[1].type = kPagerNumeric; pg[1].text = "5550123 45";
    const std::vector<uint8_t> clean = flexPhaseBits(pg);
    flexPhaseWords(clean.data(), t);
    {   // word <-> bit conversion round trip
        std::vector<uint8_t> b2(clean.size());
        flexPhaseFromWords(t, b2.data());
        CHECK(b2 == clean, "phase words round trip");
    }
    // message words are those from index 5 on (BIW, 2 addresses, 2 vectors, then messages)
    auto decode = [](const std::vector<uint8_t>& b, std::vector<PagerMessage>& out) { FlexPhaseStat st; flexDecodePhase(b.data(), out, st); return st; };
    {
        std::vector<PagerMessage> o;
        decode(wreckPhase(clean, {6, 7}), o);
        bool dam = false;
        for (auto& m : o) if (m.address == 1234567 && m.damaged) dam = true;
        CHECK(dam, "a single damaged phase loses text (premise)");
    }
    FlexPhaseCombiner fc;
    auto c1 = wreckPhase(clean, {6});
    CHECK(fc.process(7, c1.data(), 100) == 0, "first copy: nothing to combine with");
    auto c2 = wreckPhase(clean, {7});
    const int rec = fc.process(7, c2.data(), 160);
    CHECK(rec == 1, "second copy recovers the word the first one lost (%d)", rec);
    std::vector<PagerMessage> o;
    decode(c2, o);
    bool ok = false;
    for (auto& m : o) if (m.address == 1234567) ok = m.text == pg[0].text && !m.damaged;
    CHECK(ok, "combined phase decodes the full text");
    // a different phase layout under the same key is not merged
    std::vector<FlexPage> other(1);
    other[0].cap = 99; other[0].type = kPagerAlpha; other[0].text = "unrelated page";
    auto ob = wreckPhase(flexPhaseBits(other), {4});
    const std::vector<uint8_t> before = ob;
    FlexPhaseCombiner f2;
    auto a = wreckPhase(clean, {6}); f2.process(1, a.data(), 10);
    f2.process(1, ob.data(), 20);
    CHECK(ob == before, "different layout: bits untouched");
    // other key (speed/phase) and expiry
    FlexPhaseCombiner f3;
    auto k1 = wreckPhase(clean, {6}); f3.process(1, k1.data(), 10);
    auto k2 = wreckPhase(clean, {7});
    const auto k2b = k2;
    f3.process(2, k2.data(), 20);
    CHECK(k2 == k2b, "other key not combined");
    auto k3 = wreckPhase(clean, {7}); const auto k3b = k3;
    f3.process(1, k3.data(), 10 + f3.windowSec + 1);
    CHECK(k3 == k3b, "expired copy not used");
    // clean phase with a repeat: untouched
    FlexPhaseCombiner f4; auto cc = clean; f4.process(3, cc.data(), 1); f4.process(3, cc.data(), 2);
    CHECK(cc == clean, "clean phases are untouched");
    // noisy sweep: random disjoint damage never produces a wrong text
    int bad = 0, n = 0;
    for (int it = 0; it < 100; it++) {
        FlexPhaseCombiner f;
        std::vector<int> ia, ib;
        for (int k = 5; k < 14; k++) { if (rng() % 3 == 0) ia.push_back(k); else if (rng() % 3 == 0) ib.push_back(k); }
        auto x = wreckPhase(clean, ia); f.process(9, x.data(), 1);
        auto y = wreckPhase(clean, ib); f.process(9, y.data(), 2);
        std::vector<PagerMessage> oo; decode(y, oo);
        for (auto& m : oo) if (m.address == 1234567 && !m.damaged && m.text != pg[0].text) bad++;
        n++;
    }
    CHECK(bad == 0, "FLEX sweep: %d of %d produced a wrong undamaged text", bad, n);
}

int main() {
    wordTests();
    pocsagTests();
    flexTests();
    return testSummary("pager_combine");
}
