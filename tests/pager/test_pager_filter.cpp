// Pagers: the "Decoded only" predicate hides tone-only pages and pages with no text, for POCSAG and FLEX alike.
#include "../../app/pager/pager_filter.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static PagerMessage msg(bool flex, int type, const char* text) {
    PagerMessage m; m.flex = flex; m.type = type; m.text = text; return m;
}

int main() {
    CHECK(!pagerIsDecoded(msg(false, kPagerTone, "")), "POCSAG tone hidden");
    CHECK(!pagerIsDecoded(msg(false, kPagerTone, "ignored")), "tone type wins over text");
    CHECK(pagerIsDecoded(msg(false, kPagerNumeric, "0123")), "numeric kept");
    CHECK(pagerIsDecoded(msg(false, kPagerAlpha, "FIRE CALL")), "alpha kept");
    CHECK(!pagerIsDecoded(msg(false, kPagerAlpha, "")), "empty alpha hidden");
    CHECK(!pagerIsDecoded(msg(false, kPagerAlpha, " \r\n")), "blank alpha hidden");
    CHECK(pagerIsDecoded(msg(true, kPagerAlpha, "FLEX text")), "FLEX text kept");
    CHECK(!pagerIsDecoded(msg(true, kPagerAlpha, "")), "FLEX empty hidden");
    CHECK(pagerIsDecoded(msg(false, kPagerAlpha, "secure, not shown")), "secure page kept");
    {   // heard-capcodes table
        std::vector<PagerMessage> v;
        auto add = [&](uint64_t ser, bool flex, uint32_t adr, int type, const char* t) { PagerMessage m = msg(flex, type, t); m.serial = ser; m.address = adr; m.wallTime = (int64_t)ser * 10; v.push_back(m); };
        add(1, false, 1234, kPagerAlpha, "first"); add(2, true, 1234, kPagerAlpha, "flex"); add(3, false, 99, kPagerNumeric, "55");
        add(4, false, 1234, kPagerTone, ""); add(5, false, 99, kPagerNumeric, "66"); add(6, false, 1234, kPagerAlpha, "last");
        const auto r = pagerHeardTable(v);
        CHECK(r.size() == 3, "same RIC on POCSAG and FLEX is two rows");
        CHECK(r[0].address == 1234 && !r[0].flex && r[0].count == 3 && r[0].lastText == "last" && r[0].lastWall == 60, "busiest first, last text kept");
        CHECK(r[1].address == 99 && r[1].count == 2 && r[1].lastText == "66", "second row");
        CHECK(r[2].flex && r[2].count == 1, "FLEX row last");
        v.erase(v.begin() + 5);
        CHECK(pagerHeardTable(v)[1].address == 1234 && pagerHeardTable(v)[1].lastText == "first", "a tone-only page does not erase the last text");
    }
    {   // #269: the capcode watch list
        const PagerWatch w = pagerParseWatch("1234, 99;  2000-2099 abc 3000-2990 77-");
        CHECK(w.ranges.size() == 5, "watch entries: %zu", w.ranges.size());
        CHECK(w.has(1234) && w.has(99) && w.has(2000) && w.has(2050) && w.has(2099) && !w.has(2100), "numbers and a range");
        CHECK(w.has(2995) && w.has(77) && !w.has(78), "a reversed range and a dangling dash");
        CHECK(pagerParseWatch("").empty() && pagerParseWatch("none").empty(), "empty list");
        CHECK(pagerParseWatch("99999999999").has(0xFFFFFFFFu), "too large a number is clamped");
    }
    {   // feat34-c: the Ignore list
        PagerIgnore w = pagerParseIgnore("836000-836999, 12 # a comment 777\n5000-5100 5050-5200 5201 83?00 9*7\nabc 3000-2990 99999999999");
        CHECK(w.has(836000) && w.has(836500) && w.has(836999) && !w.has(835999) && !w.has(837000), "range edges");
        CHECK(w.has(12) && !w.has(777), "a single number; a comment is skipped");
        CHECK(w.has(5000) && w.has(5150) && w.has(5200) && w.has(5201) && !w.has(5202) && !w.has(4999), "overlapping and touching ranges merge");
        CHECK(w.has(83100) && w.has(83900) && !w.has(8300) && !w.has(83110) && !w.has(831000), "? is one digit");
        CHECK(w.has(97) && w.has(9007) && w.has(912347) && !w.has(98) && !w.has(9), "* matches any digits, also none");
        CHECK(w.has(2995) && w.has(0xFFFFFFFFu), "reversed range, clamped number");
        CHECK(!w.has(0) && pagerParseIgnore("").empty() && pagerParseIgnore("none # x").empty(), "empty and junk");
        // canonical text round trip
        const PagerIgnore w2 = pagerParseIgnore(pagerIgnoreText(w));
        CHECK(w2.ranges == w.ranges && w2.patterns == w.patterns, "text round trip");
        // add and remove: a range is split
        PagerIgnore r = pagerParseIgnore("100-200");
        CHECK(r.remove(150) && !r.has(150) && r.has(149) && r.has(151) && r.ranges.size() == 2, "remove splits a range");
        CHECK(r.remove(100) && r.remove(200) && r.ranges.size() == 2 && r.ranges[0].first == 101 && r.ranges[1].second == 199, "remove at the ends");
        r.add(150, 150); r.add(100, 100); r.add(200, 200);
        CHECK(r.ranges.size() == 1 && r.ranges[0].first == 100 && r.ranges[0].second == 200, "adding the holes back merges to one range");
        PagerIgnore pw = pagerParseIgnore("83*");
        CHECK(!pw.remove(830) && pw.has(830), "a wildcard match cannot be removed one by one");
        // huge list: 200000 disjoint ranges, lookups stay exact
        std::string big;
        for (uint32_t i = 0; i < 200000; i++) { big += std::to_string(i * 20 + 5) + "-" + std::to_string(i * 20 + 9) + "\n"; }
        const PagerIgnore hg = pagerParseIgnore(big);
        CHECK(hg.ranges.size() == 200000, "huge list entries %zu", hg.ranges.size());
        size_t wrong = 0;
        for (uint32_t a = 0; a < 4000000; a += 3) { const bool exp = (a % 20) >= 5 && (a % 20) <= 9; if (hg.has(a) != exp) wrong++; }
        CHECK(wrong == 0, "huge list lookups wrong: %zu", wrong);
        // a huge list of single numbers that touch merges to one range
        std::string seq; for (uint32_t i = 1; i <= 50000; i++) seq += std::to_string(i) + " ";
        CHECK(pagerParseIgnore(seq).ranges.size() == 1, "consecutive numbers merge");
        {   // a wildcard with a dash is rejected whole (no stray entries); other malformed tokens too
            std::string warn;
            PagerIgnore x = pagerParseIgnore("83?00-84000 5 9*-12 -7 1-2-3 4294967296 20-10", &warn);
            CHECK(x.patterns.empty(), "wildcard range leaves no pattern");
            CHECK(!x.has(0) && !x.has(83) && !x.has(0) && !x.has(84000) && !x.has(100) && !x.has(7) && !x.has(2) && !x.has(3), "no stray entries");
            CHECK(x.has(5) && x.has(15) && x.has(0xFFFFFFFFu) && !x.has(21) && x.ranges.size() == 3, "valid tokens survive: %zu ranges", x.ranges.size());
            CHECK(warn.find("83?00-84000") != std::string::npos && warn.find("clamped") != std::string::npos, "warnings name the problems: %s", warn.c_str());
            std::string w2;
            pagerParseIgnore("836000-836999 12 8360* 83?00", &w2);
            CHECK(w2.empty(), "good input has no warning: %s", w2.c_str());
            PagerIgnore c = pagerParseIgnore("83*");
            CHECK(c.coveringPattern(830) == "83*" && c.coveringPattern(930).empty(), "covering pattern");
        }
        // table filtering: per protocol, show-ignored brings every page back
        PagerIgnore lists[2] = {pagerParseIgnore("1000-1999"), pagerParseIgnore("1500")};
        std::vector<PagerMessage> v;
        auto add = [&](bool flex, uint32_t adr) { PagerMessage m = msg(flex, kPagerAlpha, "x"); m.address = adr; v.push_back(m); };
        add(false, 1500); add(true, 1500); add(false, 2000); add(true, 1600); add(false, 1999); add(true, 1500);
        size_t vis = 0; for (const auto& m : v) if (pagerIgnoreVisible(lists, m, false)) vis++;
        CHECK(vis == 2, "visible with the list on: %zu", vis);
        CHECK(pagerIgnoreHidden(lists, v, false) == 4, "hidden count %zu", pagerIgnoreHidden(lists, v, false));
        vis = 0; for (const auto& m : v) if (pagerIgnoreVisible(lists, m, true)) vis++;
        CHECK(vis == v.size() && pagerIgnoreHidden(lists, v, true) == 0, "show ignored brings all %zu back", vis);
        CHECK(!pagerIsIgnored(lists, v[1]) == false && !pagerIsIgnored(lists, v[3]), "FLEX 1500 ignored, FLEX 1600 not");
        // the list the receivers read
        pagerIgnoreSet(true, lists[1]); pagerIgnoreSet(false, lists[0]);
        CHECK(pagerIgnored(false, 1500) && !pagerIgnored(true, 1600) && pagerIgnored(true, 1500), "receiver-side lookup per protocol");
        pagerIgnoreSet(false, PagerIgnore());
        CHECK(!pagerIgnored(false, 1500), "cleared");
    }
    return testSummary("pager_filter");
}
