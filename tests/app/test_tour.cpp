// The first-run tour (app/ui/tour_logic.h): the step list, the anchor registry, skipping steps whose part is not on screen, and when it starts.
#include "../../app/ui/tour_logic.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string readAll(const std::filesystem::path& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main() {
    int n = 0;
    const tour::Step* st = tour::steps(&n);
    CHECK(n >= 8 && n <= 11, "8 to 11 steps, got %d", n);
    std::set<std::string> anchors;
    for (int i = 0; i < n; i++) {
        CHECK(st[i].anchor && *st[i].anchor && st[i].title && *st[i].title && st[i].text && *st[i].text, "step %d has an anchor, a title and a text", i);
        CHECK(anchors.insert(st[i].anchor).second, "step %d: anchor %s is used once", i, st[i].anchor);
        CHECK(!std::strchr(st[i].text, '!') && !std::strchr(st[i].title, '!'), "step %d: no exclamation marks", i);
        int sentences = 0;
        for (const char* c = st[i].text; *c; c++) sentences += *c == '.' && (c[1] == ' ' || c[1] == 0);
        CHECK(sentences >= 1 && sentences <= 2, "step %d: one or two sentences, got %d", i, sentences);
        int pct = 0;
        for (const char* c = st[i].text; *c; c++) pct += *c == '%';
        CHECK(pct == 0 || (pct == 1 && std::strstr(st[i].text, "%d")), "step %d: at most one %%d", i);
    }
    // order: the source and the tuning come before the modes and the spectrum; the help comes last
    auto at = [&](const char* a) { for (int i = 0; i < n; i++) if (!std::strcmp(st[i].anchor, a)) return i; return -1; };
    CHECK(at("source") == 0 && at("source") < at("tuning") && at("tuning") < at("modes") && at("modes") < at("spectrum") && at("help") == n - 1, "order");

    // every anchor is reported by some tour::mark / tourMark call in the interface
    std::string all;
    for (auto& e : std::filesystem::recursive_directory_iterator(std::string(DECT2_SOURCE_DIR) + "/app"))
        if (e.is_regular_file() && (e.path().extension() == ".cpp")) all += readAll(e.path());
    for (const std::string& a : anchors) {
        const bool legacy = a == "modes" || a == "spectrum" || a == "panel";   // reported through the older rectangle table (tgMin / tgMax) in tour.cpp
        CHECK(legacy || all.find("tourMark(\"" + a + "\"") != std::string::npos, "anchor %s is reported by the interface", a.c_str());
    }
    CHECK(all.find("legacy(\"modes\", TgSwitch)") != std::string::npos && all.find("legacy(\"spectrum\", TgMain)") != std::string::npos && all.find("legacy(\"panel\", TgRight)") != std::string::npos, "legacy anchors");

    // visibility: a step with no (or a stale, or an empty) rectangle is skipped
    tour::Anchors reg;
    const long f = 100;
    CHECK(tour::visibleCount(reg, f) == 0 && tour::next(reg, f, -1, +1) == -1, "nothing reported, nothing shown");
    for (int i = 0; i < n; i++) reg.set(st[i].anchor, tour::Rect{0, 0, 10, 10}, f);
    CHECK(tour::visibleCount(reg, f) == n && tour::next(reg, f, -1, +1) == 0 && tour::next(reg, f, 0, +1) == 1 && tour::next(reg, f, 0, -1) == -1 && tour::next(reg, f, n - 1, +1) == -1, "all shown");
    reg.set(st[2].anchor, tour::Rect{0, 0, 0, 0}, f);   // empty rectangle
    reg.set(st[4].anchor, tour::Rect{0, 0, 10, 10}, f - 5);   // not reported for a while
    CHECK(tour::visibleCount(reg, f) == n - 2 && tour::next(reg, f, 1, +1) == 3 && tour::next(reg, f, 3, +1) == 5 && tour::next(reg, f, 5, -1) == 3, "absent steps are skipped in both directions");
    CHECK(tour::position(reg, f, 3) == 3 && tour::position(reg, f, 5) == 4, "positions count only the shown steps");
    CHECK(tour::shown(reg, f + 1, 0) && !tour::shown(reg, f + 2, 0), "a rectangle counts for one more frame");

    // the preference and the start rule: first start only, never after an update, never behind the setup or with arguments
    CHECK(std::string(tour::prefKey()) == "wizardDone", "the key of the earlier tour is kept");
    CHECK(tour::autoStart(true, false, false), "first start");
    CHECK(!tour::autoStart(true, true, false), "not again once seen (also after an update)");
    CHECK(!tour::autoStart(false, false, false), "not with command-line arguments");
    CHECK(!tour::autoStart(true, false, true), "after the setup");
    // #491: one guide at a time; the tour is in front of "Get me listening", which comes once the tour is skipped
    CHECK(tour::guideInFront(false, true, true, false) == tour::Guide::Tour, "the tour is in front of Get me listening");
    CHECK(tour::guideInFront(false, false, true, false) == tour::Guide::Listen, "Get me listening after the tour was skipped");
    CHECK(tour::guideInFront(true, true, true, true) == tour::Guide::Setup, "the setup first");
    CHECK(tour::guideInFront(false, false, true, true) == tour::Guide::Listen, "a lesson card waits for Get me listening");
    CHECK(tour::guideInFront(false, false, false, true) == tour::Guide::Lesson && tour::guideInFront(false, false, false, false) == tour::Guide::None, "the lesson card, then none");

    // the guided tours of the Help menu: four of them, each step points at a reported part and reads like the first-run tour
    int nt = 0;
    const tour::Tour* ts = tour::tours(&nt);
    CHECK(nt == 4, "four guided tours, got %d", nt);
    for (int t = 0; t < nt; t++) {
        CHECK(ts[t].title && *ts[t].title && ts[t].n >= 2 && ts[t].n <= 4, "tour %d: a title and 2 to 4 steps", t);
        CHECK(tour::count(&ts[t]) == ts[t].n, "tour %d: count", t);
        for (int i = 0; i < ts[t].n; i++) {
            const tour::Step& s = ts[t].steps[i];
            CHECK(s.anchor && *s.anchor && s.title && *s.title && s.text && *s.text, "tour %d step %d is complete", t, i);
            CHECK(anchors.count(s.anchor) == 1, "tour %d step %d: anchor %s is an interface part", t, i, s.anchor);
            CHECK(!std::strchr(s.text, '!') && !std::strchr(s.text, '%'), "tour %d step %d: no exclamation marks or format codes", t, i);
            int sentences = 0;
            for (const char* c = s.text; *c; c++) sentences += *c == '.' && (c[1] == ' ' || c[1] == 0);
            CHECK(sentences >= 1 && sentences <= 2, "tour %d step %d: one or two sentences, got %d", t, i, sentences);
        }
    }
    // a guided tour shows only its own steps
    tour::Anchors reg2;
    for (int i = 0; i < ts[1].n; i++) reg2.set(ts[1].steps[i].anchor, tour::Rect{0, 0, 10, 10}, f);
    CHECK(tour::visibleCount(reg2, f, &ts[1]) == ts[1].n && tour::next(reg2, f, -1, +1, &ts[1]) == 0 && tour::next(reg2, f, ts[1].n - 1, +1, &ts[1]) == -1, "a guided tour counts its own steps");

    std::printf(failures ? "FAILED: %d\n" : "tour: ok\n", failures);
    return failures ? 1 : 0;
}
