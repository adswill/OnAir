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
    return testSummary("sonde_sites");
}
