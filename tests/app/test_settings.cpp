// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Settings window rules: the table is sound, the search finds rows across categories, reset goes to the default, export / import round trip.
#include "../../app/ui/settings_table.h"
#include "../../app/ui/settings_profile.h"
#include "../../app/ui/notify_logic.h"
#include "../../app/adsb/tile_url.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
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
        if (!i.tip[0]) { tipOk = false; printf("no help text: %s\n", i.key); }   // 3.7: every setting has a one-line help
    }
    check(!dup, "keys are unique"); check(inRange, "defaults inside their limits"); check(choiceOk, "choices and defaults agree"); check(tipOk, "label, category and help present");
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
    // ---------------------------------------------------------------- 4.0
    // the new pages are there and every search finds its rows
    {
        const std::vector<std::string> c = categories(t);
        for (const char* want : {kUnits, kKeys, kNotify, kPerf, kProfiles}) check(std::find(c.begin(), c.end(), want) != c.end(), "a 4.0 page");
        check(in(filter(t, "shortcut", ""), "keysEditor") && in(filter(t, "keybinding", ""), "keysEditor"), "search finds the key editor");
        check(in(filter(t, "nautical", ""), "adsbUnits") && in(filter(t, "fft", ""), "specFft") && in(filter(t, "quiet", ""), "notifyQuiet"), "search: units, FFT, quiet hours");
        check(in(filter(t, "profile", ""), "profiles") && in(filter(t, "tile server", ""), "mapTileUrl"), "search: profiles, tile server");
        check(!in(filter(t, "", kGeneral), "powerLowMode") && in(filter(t, "", kPerf), "powerLowMode") && in(filter(t, "", kPerf), "gpuGlobal"), "power and GPU moved to Performance, keys unchanged");
        // the 3.9 keys are all still there (a renamed key would lose every saved value)
        for (const char* k39 : {"uiLang", "powerLowMode", "discordOn", "uiTheme", "wfMap3", "specAvgSec", "holdDecay", "tuningMode", "fmDeemph", "staleAdsb", "adsbLat", "updCheck", "udpHost", "recFormat", "gpuGlobal", "perfOverlay", "timeDisplay"})
            check(in(filter(t, "", ""), k39), "a 3.9 key is kept");
        // a float choice still finds its entry (the waterfall speed is a float field)
        const Info* wr = nullptr;
        for (const Info& i : t) if (!strcmp(i.key, "wfRate")) wr = &i;
        check(wr && choiceIndex(*wr, (double)(1.f / 60.f)) == 7 && choiceIndex(*wr, (double)0.1f) == 6, "float choice values");
        // import of a file from 3.9 (fewer keys) leaves the new rows at their defaults
        std::vector<Val> v39;
        for (const Info& i : t) v39.push_back(defaultOf(i));
        importText(t, v39, "# OnAir settings\nwfGamma=0.8\nuiTheme=1\n");
        size_t gi2 = 0, fi = 0;
        for (size_t k = 0; k < t.size(); k++) { if (!strcmp(t[k].key, "wfGamma")) gi2 = k; if (!strcmp(t[k].key, "specFft")) fi = k; }
        check(std::fabs(v39[gi2].d - 0.8) < 1e-9 && v39[fi].d == 0, "an older export loads, new keys at their defaults");
    }
    // search highlight
    {
        auto h = highlightSpans("Waterfall colour intensity", "COLOUR wat");
        check(h.size() == 2 && h[0].first == 0 && h[0].second == 3 && h[1].first == 10 && h[1].second == 16, "every word marked, case-insensitive, sorted");
        h = highlightSpans("aaaa", "aa a");
        check(h.size() == 1 && h[0].first == 0 && h[0].second == 4, "overlapping matches merge");
        check(highlightSpans("Grid", "").empty() && highlightSpans("Grid", "zz").empty(), "no query, no match: nothing marked");
    }
    // profiles: compose / split round trip, an old export as a profile, file names
    {
        const std::string st = "wfGamma=0.8\nupdMirror=a\\nb\n", keys = "start = Space\nmute = F9\n";
        std::string s2, k2;
        check(profileSplit(profileCompose(st, keys), s2, k2) && s2 == st && k2 == keys, "profile round trip");
        check(profileSplit("# OnAir settings\nwfGamma=1.2\n", s2, k2) && s2 == "wfGamma=1.2\n" && k2.empty(), "an old settings export reads as a profile");
        check(!profileSplit("hello\nworld\n", s2, k2), "not a profile");
        check(profileSplit("[settings]\r\nwfGamma=1\r\n[later]\r\nx=1\r\n[keys]\r\nstart = F1\r\n", s2, k2) && s2 == "wfGamma=1\n" && k2 == "start = F1\n", "CRLF and unknown sections");
        check(profileFileName("Night") == "Night.onairprofile" && profileFileName("../../etc/x") == "______etc_x.onairprofile" && profileFileName(" / ").empty() && profileFileName("").empty(), "safe file names");
        check(profileNameOf("Night.onairprofile") == "Night" && profileNameOf("Night.txt").empty() && profileNameOf(".onairprofile").empty(), "names from files");
        // a profile carries every setting: export -> compose -> split -> import gives the same values
        std::vector<Val> pv;
        for (const Info& i : t) pv.push_back(defaultOf(i));
        for (size_t k = 0; k < t.size(); k++) if (t[k].kind == Kind::Bool) pv[k].d = t[k].def == 0;
        std::string ps, pk;
        profileSplit(profileCompose(exportText(t, pv), ""), ps, pk);
        std::vector<Val> pb;
        for (const Info& i : t) pb.push_back(defaultOf(i));
        importText(t, pb, ps);
        bool same2 = true;
        for (size_t k = 0; k < t.size(); k++) if (t[k].kind == Kind::Bool && pb[k].d != pv[k].d) same2 = false;
        check(same2, "a profile round-trips every switch");
    }
    // the map tile server and the quiet hours
    {
        check(tileurl::effective("") == tileurl::kDefault && tileurl::effective("ftp://x/{z}/{x}/{y}") == tileurl::kDefault && tileurl::effective("https://t/{z}/{x}.png") == tileurl::kDefault, "a bad template falls back to OpenStreetMap");
        check(tileurl::expand("https://t.example/{z}/{x}/{y}.png", 7, 70, 41) == "https://t.example/7/70/41.png" && tileurl::expand(tileurl::kDefault, 1, 2, 3) == "https://tile.openstreetmap.org/1/2/3.png", "expand");
        check(tileurl::cacheName(tileurl::kDefault) == "osm" && tileurl::cacheName("https://a/{z}/{x}/{y}") != tileurl::cacheName("https://b/{z}/{x}/{y}") && tileurl::cacheName("https://a/{z}/{x}/{y}").rfind("tiles-", 0) == 0, "cache folder per server, OSM keeps the old one");
        check(notifyset::inQuiet(23 * 60, 22 * 60, 7 * 60) && notifyset::inQuiet(3 * 60, 22 * 60, 7 * 60) && !notifyset::inQuiet(12 * 60, 22 * 60, 7 * 60), "quiet hours over midnight");
        check(notifyset::inQuiet(13 * 60, 12 * 60, 14 * 60) && !notifyset::inQuiet(14 * 60, 12 * 60, 14 * 60) && !notifyset::inQuiet(5, 300, 300), "quiet hours in a day; from == to is none");
    }
    if (fails) printf("FAILED %d\n", fails); else printf("settings ok (%d rows)\n", (int)t.size());
    return fails ? 1 : 0;
}
