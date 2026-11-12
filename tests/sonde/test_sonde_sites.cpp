// Radiosonde sites (round 4 #39): two launches 2 km apart are one site, one 40 km away another, a sonde first heard at 9 km is no launch,
// landings after a burst go to their site (not one still in the air, not one without a burst), renaming, the saved text.
#include "dect2/sonde/sonde_sites.h"
#include "../common/test_util.h"
using namespace dect2;

static SondeInfo flight(const char* serial, double lat, double lon, double startAlt, bool burst, bool landed) {
    SondeInfo s; s.serial = serial; s.trackIncluded = true;
    for (int i = 0; i < 300; i++) {
        SondeTrackPoint p; p.lat = (float)(lat + i * 1e-3); p.lon = (float)(lon + i * 2e-3);
        p.altM = (float)(burst && i > 200 ? 30000 - (i - 200) * 300 : startAlt + i * 150);
        if (landed && i == 299) p.altM = 400;
        s.track.push_back(p);
    }
    s.active = !landed; s.lastHeardS = landed ? 300 : 1;
    return s;
}

int main() {
    SondeSites st;
    const int64_t t = 1'800'000'000;
    st.update({flight("A1", 54.70, 25.30, 150, true, true), flight("A2", 54.71, 25.33, 160, false, false), flight("B1", 55.05, 25.30, 200, true, true), flight("C1", 54.70, 25.30, 9000, true, true)}, t);
    CHECK(st.sites().size() == 2, "two sites (%zu)", st.sites().size());
    if (st.sites().size() == 2) {
        CHECK(st.sites()[0].launches == 2 && st.sites()[0].landings.size() == 1 && st.sites()[1].launches == 1 && st.sites()[1].landings.size() == 1 && st.sites()[0].name == "Site 1", "launches and landings per site");
        CHECK(std::abs(st.sites()[0].lat - 54.705) < 1e-3, "the site is the mean of its launches");
    }
    st.update({flight("A2", 54.71, 25.33, 160, true, true)}, t + 7200);   // A2 has now come down
    CHECK(st.sites()[0].landings.size() == 2 && st.sites()[0].launches == 2, "a sonde counted once, its landing later");
    st.rename(1, "Kaunas");
    SondeSites r; r.parse(st.serialize() + "junk\n");
    CHECK(r.sites().size() == 2 && r.sites()[0].name == "Kaunas" && r.sites()[0].landings.size() == 2 && r.sites()[1].landings.size() == 1, "saved and read back");
    CHECK(r.sites()[0].launchTimes.size() == 2 && r.sites()[0].launchTimes[0] == t, "launch times kept and read back");
    r.update({flight("D1", 54.70, 25.31, 100, false, false)}, t + 9000);
    CHECK(r.sites()[0].launches == 3 && r.sites().size() == 2, "new launch at a known site after loading");
    // 4.0: names from the known launch sites (built-in IGRA 2 list, the user's file) instead of "Site 1"
    KnownSondeSites known;
    CHECK(known.all().size() > 500, "built-in list (%zu sites)", known.all().size());
    const SondeSiteRef* lb = known.nearest(52.2085, 14.1180, 1);
    CHECK(lb && lb->name == "Lindenberg" && lb->wmo == 10393, "Lindenberg in the list (%s)", lb ? lb->name.c_str() : "none");
    SondeSites g; g.setKnown(&known);
    g.update({flight("B9", 52.83, 9.95, 120, false, false)}, t);   // a first fix 2.4 km from Bergen (WMO 10238)
    CHECK(g.sites().size() == 1 && g.sites()[0].name == "Bergen", "a launch at Bergen is called Bergen (%s)", g.sites().empty() ? "-" : g.sites()[0].name.c_str());
    g.update({flight("Z1", 47.0, -30.0, 100, false, false)}, t);   // mid-Atlantic: no known site near
    CHECK(g.sites().size() == 2 && g.sites()[1].name == "Site 2", "no known site within 15 km keeps the placeholder (%s)", g.sites().size() > 1 ? g.sites()[1].name.c_str() : "-");
    // a 3.9 file: "Site 1" at Bergen and a name the user gave elsewhere
    SondeSites old;
    old.parse("S\t1\tSite 1\t52.80\t9.90\t12\t1800000000\nS\t2\tMy field\t52.2085\t14.1180\t3\t1800000000\nS\t3\tSite 3\t47\t-30\t1\t1800000000\n");
    CHECK(old.setKnown(&known) == 1 && old.sites()[0].name == "Bergen" && old.sites()[1].name == "My field" && old.sites()[2].name == "Site 3" && old.dirty(), "3.9 placeholder renamed, the user's name kept");
    // the user's own file wins near its sites
    KnownSondeSites mine;
    CHECK(mine.addUserText("# my sites\nBergen-Hohne test field; 52.80; 9.90; 70\nbad line\nNowhere, 47.0, -30.0\nToo far north; 91; 0\n") == 2, "two good lines read");
    const SondeSiteRef* u = mine.nearest(52.81, 9.92, KnownSondeSites::kNameKm);
    CHECK(u && u->user && u->name == "Bergen-Hohne test field", "a user site before a built-in one");
    SondeSites h; h.setKnown(&mine);
    h.update({flight("Z2", 47.01, -30.01, 100, false, false)}, t);
    CHECK(h.sites().size() == 1 && h.sites()[0].name == "Nowhere", "a launch at a user site takes its name");
    return testSummary("sonde_sites");
}
