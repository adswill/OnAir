// Settings window rules: the table is sound, the search finds rows across categories, reset goes to the default, export / import round trip.
#include "../../app/ui/settings_table.h"
#include <cstdio>
#include <cstring>
#include <set>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
using namespace settings;
int main() {
    const std::vector<Info>& t = table();
    std::set<std::string> keys;
    bool dup = false, inRange = true, choiceOk = true, tipOk = true;
    for (const Info& i : t) {
        dup |= !keys.insert(i.key).second;
        if ((i.kind == Kind::Int || i.kind == Kind::Float) && i.hi >= i.lo && (i.def < i.lo || i.def > i.hi)) { inRange = false; printf("range %s\n", i.key); }
        if (i.kind == Kind::Choice) {
            if (!i.choices) choiceOk = false;
            else if (i.values) { choiceOk &= split(i.values, ',').size() == split(i.choices, '|').size(); choiceOk &= choiceValue(i, choiceIndex(i, i.def)) == i.def; }
            else choiceOk &= i.def >= 0 && i.def < (double)split(i.choices, '|').size();
        }
        if (!i.label[0] || !i.cat[0]) tipOk = false;
    }
    check(!dup, "keys are unique"); check(inRange, "defaults inside their limits"); check(choiceOk, "choices and defaults agree"); check(tipOk, "label and category present");
    check(categories(t).size() >= 8, "categories");

    // search: every word, any category; empty query with a category is that category only
    auto in = [&](const std::vector<size_t>& v, const char* k) { for (size_t n : v) if (std::string(t[n].key) == k) return true; return false; };
    check(in(filter(t, "update beta", ""), "updBeta"), "search finds the beta row");
    check(!in(filter(t, "update beta", ""), "wfGamma"), "search leaves others out");
    check(in(filter(t, "BATTERY", ""), "powerLowMode"), "case-insensitive, search words");
    check(in(filter(t, "latitude", "Audio"), "adsbLat"), "a search ignores the selected category");
    check(filter(t, "zzzqqq", "").empty(), "no match");
    check(!in(filter(t, "", kAudioC), "updCheck") && in(filter(t, "", kAudioC), "popTop"), "category filter");

    // reset and changed marker
    const Info* gamma = nullptr;
    for (const Info& i : t) if (!strcmp(i.key, "wfGamma")) gamma = &i;
    Val v = defaultOf(*gamma);
    check(isDefault(*gamma, v), "default is default");
    v.d = 1.5;
    check(!isDefault(*gamma, v), "changed");
    v = defaultOf(*gamma);
    check(isDefault(*gamma, v), "reset");
    check(clampTo(*gamma, 9) == gamma->hi && clampTo(*gamma, 0) == gamma->lo, "clamp");

    // export / import round trip over every row
    std::vector<Val> vals;
    for (const Info& i : t) vals.push_back(defaultOf(i));
    for (size_t k = 0; k < t.size(); k++) {
        if (t[k].kind == Kind::Bool) vals[k].d = t[k].def == 0;
        else if (t[k].kind == Kind::Int || t[k].kind == Kind::Float) vals[k].d = t[k].hi >= t[k].lo ? (t[k].lo + t[k].hi) / 2 : 7;
        else if (t[k].kind == Kind::Choice) vals[k].d = choiceValue(t[k], 1);
        else if (t[k].kind == Kind::Text) vals[k].s = "a=b\nline two\\x";
        if (t[k].kind == Kind::Int) vals[k].d = clampTo(t[k], vals[k].d);
    }
    const std::string txt = exportText(t, vals);
    std::vector<Val> back;
    for (const Info& i : t) back.push_back(defaultOf(i));
    const int n = importText(t, back, txt);
    bool same = true;
    for (size_t k = 0; k < t.size(); k++) {
        if (t[k].kind == Kind::Custom || t[k].kind == Kind::Link) continue;
        if (t[k].kind == Kind::Text) same &= back[k].s == vals[k].s;
        else same &= std::fabs(back[k].d - vals[k].d) < 1e-6;
        if (!same) { printf("round trip %s\n", t[k].key); break; }
    }
    check(same, "export / import round trip");
    check(n > 40, "import count");
    // import is tolerant: unknown keys, junk numbers and out-of-range values
    std::vector<Val> z = back;
    importText(t, z, "nothing=1\nwfGamma=abc\nwfBright=99\n# comment\nbad line\n");
    size_t gi = 0, bi = 0;
    for (size_t k = 0; k < t.size(); k++) { if (!strcmp(t[k].key, "wfGamma")) gi = k; if (!strcmp(t[k].key, "wfBright")) bi = k; }
    check(z[gi].d == back[gi].d, "junk number ignored");
    check(z[bi].d == t[bi].hi, "imported number clamped");
    check(changedCount(t, vals, kDisplay) > 5 && changedCount(t, std::vector<Val>(), kDisplay) == 0, "changed count");
    if (fails) printf("FAILED %d\n", fails); else printf("settings ok (%d rows)\n", (int)t.size());
    return fails ? 1 : 0;
}
