// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "star on GitHub" card: when it may show (core/include/dect2/engine/star_prompt.h).
#include "dect2/engine/star_prompt.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string slurp(const std::string& rel) { std::ifstream f(std::string(ONAIR_SOURCE_DIR) + "/" + rel, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main() {
    {   // not_on_first_launch: not at start, not after a decode in it
        StarState s; s.today = 100;
        CHECK(!starShouldShow(s, StarEvent::Launch), "first launch shows");
        CHECK(!starShouldShow(s, StarEvent::Decode), "a decode in the first launch shows");
        CHECK(!starShouldShow(s, StarEvent::Frame), "first launch frame shows");
    }
    {   // shows_after_third_launch_or_first_decode
        StarState a; a.today = 100;
        starShouldShow(a, StarEvent::Launch);
        CHECK(!starShouldShow(a, StarEvent::Launch), "second launch with nothing shows");
        CHECK(starShouldShow(a, StarEvent::Decode), "a decode in the second launch does not show");
        StarState b; b.today = 100;   // third launch
        starShouldShow(b, StarEvent::Launch); starShouldShow(b, StarEvent::Launch);
        CHECK(starShouldShow(b, StarEvent::Launch), "third launch does not show");
        CHECK(!starShouldShow(b, StarEvent::Frame), "shows twice in one session");
        StarState c; c.today = 100; c.decoded = true;   // a decode in an earlier launch, now the second
        starShouldShow(c, StarEvent::Launch);
        CHECK(starShouldShow(c, StarEvent::Launch), "decoded earlier, second launch does not show");
    }
    {   // never_after_star_or_never, and the settings switch
        StarState a; a.launches = 5; a.today = 100;
        starOnChoice(a, StarChoice::Star);
        a.shownThisSession = false;
        CHECK(!starShouldShow(a, StarEvent::Launch), "shows after Star");
        StarState b; b.launches = 5; b.today = 100;
        starOnChoice(b, StarChoice::Never);
        CHECK(!starShouldShow(b, StarEvent::Launch), "shows after Never");
        StarState c; c.launches = 5; c.today = 100; c.enabled = false;
        CHECK(!starShouldShow(c, StarEvent::Launch), "shows with the setting off");
    }
    {   // later_returns_at_most_twice: 30 days, then 60 days, then never
        StarState s; s.launches = 3; s.today = 1000;
        CHECK(starShouldShow(s, StarEvent::Frame), "first show");
        starOnChoice(s, StarChoice::Later);
        s.shownThisSession = false; s.today = 1029;
        CHECK(!starShouldShow(s, StarEvent::Launch), "back after 29 days");
        s.shownThisSession = false; s.today = 1030;
        CHECK(starShouldShow(s, StarEvent::Frame), "no return after 30 days");
        starOnChoice(s, StarChoice::Later);
        s.shownThisSession = false; s.today = 1089;
        CHECK(!starShouldShow(s, StarEvent::Frame), "second return after 59 days");
        s.today = 1090;
        CHECK(starShouldShow(s, StarEvent::Frame), "no second return after 60 days");
        starOnChoice(s, StarChoice::Later);
        s.shownThisSession = false; s.today = 5000;
        CHECK(!starShouldShow(s, StarEvent::Frame), "a third return");
        CHECK(s.shown == 3, "shown %d times", s.shown);
    }
    {   // no_network: the logic opens no connection and no browser; only the app does, on the click
        const std::string src = slurp("core/src/engine/star_prompt.cpp") + slurp("core/include/dect2/engine/star_prompt.h");
        for (const char* bad : {"socket", "curl", "openUrl", "system(", "popen", "httplib", "connect("})
            CHECK(src.find(bad) == std::string::npos, "star_prompt uses %s", bad);
        const std::string u = starUrl();
        CHECK(u.rfind("https://github.com/", 0) == 0, "url %s", u.c_str());
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("star_prompt ok\n");
    return 0;
}
