// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pager keyword alerts (app/pager/pager_keywords.h): plain and regular-expression keywords, case, the capcode name, comments, bad lines, limits.
#include "../../app/pager/pager_keywords.h"
#include "../common/test_util.h"
#include <string>

using dect2::PagerKeywords;

int main() {
    PagerKeywords k;
    CHECK(k.parse("") == 0 && k.empty(), "an empty list is empty");
    CHECK(!k.match("fire at the mill", ""), "nothing matches an empty list");

    CHECK(k.parse("# my words\nFire\n\n  structure fire  \nre:\\bMVA\\b|crash\nre:(unclosed\n") == 1, "one bad regular expression is counted");
    CHECK(k.size() == 3, "comments, empty lines and the bad line are not keywords (%zu)", k.size());
    int where = -1;
    const std::string* h = k.match("FIRE ALARM station 4", "", &where);
    CHECK(h && *h == "Fire" && where == 0, "plain keyword, any case");
    CHECK(!k.match("a firework", "") == false, "a plain keyword is a piece of text, so 'firework' contains 'fire'");
    h = k.match("Car MVA on route 9", "");
    CHECK(h && h->compare(0, 3, "re:") == 0, "regular expression matches");
    CHECK(!k.match("a mvalue", ""), "\\b keeps MVA from matching inside a word");
    h = k.match("CRASH reported", "");
    CHECK(h != nullptr, "regular expression is case-insensitive");
    CHECK(!k.match("all quiet", "Station 3"), "no keyword in text or name");

    CHECK(k.parse("Station 3") == 0, "name keyword list");
    h = k.match("all quiet", "Fire Station 3", &where);
    CHECK(h && where == 1, "a keyword is found in the capcode name (%d)", where);
    CHECK(k.match("station 3 call", "", &where) && where == 0, "text first");

    std::string many;
    for (int i = 0; i < 500; i++) many += "word" + std::to_string(i) + "\n";
    k.parse(many);
    CHECK(k.size() == PagerKeywords::kMaxKeywords, "list is capped (%zu)", k.size());
    CHECK(k.parse("re:" + std::string(300, 'a')) == 1 && k.empty(), "a very long pattern is refused");
    k.parse("re:(a+)+b");   // a classic backtracking pattern on a long text must still return
    const std::string long_text(3000, 'a');
    (void)k.match(long_text, "");
    k.parse("tail");
    CHECK(!k.match(std::string(PagerKeywords::kMaxScan + 10, 'x') + "tail", ""), "only the first part of a huge text is scanned");
    k.parse("A\r\nB\r\n");
    CHECK(k.size() == 2 && k.match("b", ""), "Windows line ends");
    return testSummary("pager_keywords");
}
