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
    return testSummary("pager_filter");
}
