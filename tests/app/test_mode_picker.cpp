// Mode Browser logic: filter, tags, tabs, hidden modes, recent list, grid layout and keyboard moves, radio fit (app/ui/mode_picker_logic.h).
#include "../../app/ui/mode_picker_logic.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
using namespace modepick;

static Item mk(const char* name, int g, const char* text, bool hidden = false, bool fav = false, bool active = false) {
    Item it; it.name = name; it.groupIdx = g; it.text = lower(std::string(name) + " " + text); it.tags = tagsOf(g, name);
    it.hidden = hidden; it.fav = fav; it.active = active; return it;
}
int main() {
    std::vector<Item> v = {
        mk("FM", 1, "broadcast radio fm stereo"), mk("ADS-B", 2, "aircraft 1090 mhz"), mk("AIS", 3, "ship transponders 162"),
        mk("FT8", 10, "weak signal ft8 hf", false, true), mk("APRS", 11, "packet aprs 144", true), mk("DAB", 1, "digital radio", true, false, true),
        mk("Marine VHF", 3, "voice channel 16"), mk("Plugin X", 14, "plugin"),
    };
    Query q;
    check(filter(v, q).size() == 7, "hidden APRS leaves the list, active DAB stays");
    q.showHidden = true;
    check(filter(v, q).size() == 8, "show hidden lists all");
    q.showHidden = false; q.text = "aprs";
    check(filter(v, q).size() == 1, "a search finds a hidden mode");
    q.text = "1090 aircraft";
    check(filter(v, q).size() == 1, "every word must match");
    q.text = "ship"; check(filter(v, q).size() == 1 && v[filter(v, q)[0]].name == "AIS", "search blurb text");
    q = Query();
    q.tags = TAviation; check(filter(v, q).size() == 1, "tag filter");
    q.tags = TMarine | TVoice; check(filter(v, q).size() == 1 && v[filter(v, q)[0]].name == "Marine VHF", "tags are AND");
    q = Query(); q.tab = TabFav; check(filter(v, q).size() == 1 && v[filter(v, q)[0]].name == "FT8", "favourites tab");
    q = Query(); q.group = 1; check(filter(v, q).size() == 2, "rail group");
    check(countWithTag(v, Query(), 1) == 1 && countWithTag(v, Query(), 3) == 1, "chip counts");
    // recent
    std::vector<std::string> r;
    for (int i = 0; i < 15; i++) recentPush(r, "m" + std::to_string(i));
    check(r.size() == 12 && r[0] == "m14", "recent capped");
    recentPush(r, "m10"); check(r.size() == 12 && r[0] == "m10" && std::count(r.begin(), r.end(), "m10") == 1, "recent dedupe");
    check(recentParse(recentJoin(r)) == r, "recent round trip");
    q = Query(); q.tab = TabRecent; std::vector<std::string> rr = {"FT8", "FM"}; q.recent = &rr;
    auto f = filter(v, q);
    check(f.size() == 2 && v[f[0]].name == "FT8" && v[f[1]].name == "FM", "recent order");
    // fit
    Radio rtl{true, 24e6, 1766e6, 3.2e6}, none;
    check(fit(none, {20e6, 522}).ok, "no radio: ok");
    check(!fit(rtl, {20e6, 522}).ok && fit(rtl, {20e6, 522}).reason == "needs 20 Msps radio", "RTL-SDR vs 20 Msps");
    check(!fit(rtl, {2e6, 5}).ok, "HF on a 24 MHz minimum radio");
    check(fit(rtl, {2e6, 100}).ok && fit(rtl, {0, 0}).ok, "fits / unknown");
    // columns
    Cols c = columns(900, 10, 150, 220);
    check(c.n * c.tileW + (c.n - 1) * 10 <= 900.5f && c.tileW <= 220.5f && c.tileW >= 149.f, "columns 900");
    check(columns(100, 10, 150, 220).n == 1, "narrow gives one column");
    check(columns(2000, 10, 150, 220).tileW <= 220.5f, "wide caps tile width");
    // keyboard: sections 5 and 3 with 3 columns
    std::vector<int> s = {5, 3};
    check(moveFocus(s, 3, 0, Down) == 3, "down in section");
    check(moveFocus(s, 3, 4, Down) == 6, "down from the last row to the next section keeps the column");
    check(moveFocus(s, 3, 2, Down) == 4, "down into a partial row clamps to its last tile");
    check(moveFocus(s, 3, 6, Up) == 4, "up crosses the header");
    check(moveFocus(s, 3, 0, Up) == 0 && moveFocus(s, 3, 7, Down) == 7, "edges");
    check(moveFocus(s, 3, 0, End) == 7 && moveFocus(s, 3, 7, Home) == 0, "home/end");
    check(moveFocus(s, 3, 0, Left) == 0 && moveFocus(s, 3, 7, Right) == 7, "left/right clamp");
    check(moveFocus({}, 3, 0, Down) == 0, "empty");
    // tags
    check(tagsOf(2, "ADS-B") & TAviation, "aviation group tag");
    check(tagsOf(3, "Marine VHF") & TVoice, "name override");
    check(tagsOf(14, "x") == TPlugins, "plugin group");
    for (int g = 0; g < 17; g++) check(g == 13 || g == 16 || tagsOfGroup(g) != 0, "every group but General/Custom has a tag");
    printf(fails ? "%d FAILED\n" : "mode picker: all ok\n", fails);
    return fails ? 1 : 0;
}
