// Pure first-use helpers: launch windows, geostationary picks, what a preselection leaves alone.
#include "dect2/ease/ease.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace dect2::ease;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void testSonde() {
    const int64_t day = 20000 * 86400LL;   // a midnight UTC
    SondeWindow w = sondeWindow(day + 9 * 3600);          // 09:00, 2 h 15 before 11:15
    CHECK(!w.inWindow); CHECK(w.secsToLaunch == 2 * 3600 + 15 * 60);
    w = sondeWindow(day + 10 * 3600 + 45 * 60);           // the window opens 30 min before
    CHECK(w.inWindow); CHECK(w.secsToLaunch == 30 * 60);
    w = sondeWindow(day + 12 * 3600);                     // in flight
    CHECK(w.inWindow); CHECK(w.secsToLaunch == -45 * 60);
    w = sondeWindow(day + 13 * 3600 + 15 * 60);           // two hours after: closed, the 23:15 launch is next
    CHECK(!w.inWindow); CHECK(w.secsToLaunch == 10 * 3600);
    // every second across two days: the launch is never more than 12 h away and the id only rises, one per window
    int64_t lastId = sondeWindow(day).id; int changes = 0, inW = 0;
    for (int64_t t = day; t < day + 2 * 86400; t += 60) {
        const SondeWindow x = sondeWindow(t);
        CHECK(x.secsToLaunch <= 43200 && x.secsToLaunch >= -7200);
        CHECK(x.id == lastId || x.id == lastId + 1);
        if (x.id != lastId) changes++;
        lastId = x.id; inW += x.inWindow;
    }
    CHECK(changes == 4);
    CHECK(inW == 4 * 150);   // 2.5 h per window
    // before the epoch the arithmetic must not break
    w = sondeWindow(-3600); CHECK(w.secsToLaunch > 0 || w.inWindow);

    SondeWindow o; o.inWindow = true; o.id = 7;
    CHECK(sondeAutoStartDue(o, 6, true, false));
    CHECK(!sondeAutoStartDue(o, 7, true, false));   // once per window
    CHECK(!sondeAutoStartDue(o, 6, false, false));  // off unless ticked
    CHECK(!sondeAutoStartDue(o, 6, true, true));    // never over a running receiver
    o.inWindow = false; CHECK(!sondeAutoStartDue(o, 6, true, false));
}

static void testGeo() {
    const double inmc[] = {25.0, -98.0, -54.0, 143.5};
    GeoPick p = highestGeo(54.69, 25.28, inmc, 4);        // Vilnius: Alphasat, due south
    CHECK(p.index == 0); CHECK(std::fabs(p.elDeg - 27.6) < 0.3); CHECK(std::string(compass8(p.azDeg)) == "south");
    p = highestGeo(40.7, -74.0, inmc, 4);                 // New York: 54W is highest of these
    CHECK(p.index == 2);
    p = highestGeo(-33.9, 151.2, inmc, 4);                // Sydney
    CHECK(p.index == 3);
    p = highestGeo(89.5, 0.0, inmc, 4);                   // nothing above 5 degrees at the pole
    CHECK(p.index < 0);
    CHECK(highestGeo(0, 0, nullptr, 0).index < 0);
    CHECK(std::string(compass8(0)) == "north"); CHECK(std::string(compass8(359)) == "north");
    CHECK(std::string(compass8(-90)) == "west"); CHECK(std::string(compass8(136)) == "south-east");
}

static void testPreselect() {
    using P = Preselect;
    CHECK(preselectAction(false, true, false, 1537.7, 1537.7) == P::Pick);
    CHECK(preselectAction(true, true, false, 1537.7, 1537.7) == P::Done);       // chosen (or decided) earlier
    CHECK(preselectAction(false, false, false, 1537.7, 1537.7) == P::Wait);     // no position yet
    CHECK(preselectAction(false, true, true, 1537.7, 1537.7) == P::Wait);       // never retune a running receiver
    CHECK(preselectAction(false, true, false, 1541.45, 1537.7) == P::LeaveAlone); // tuned by hand
}

static void testFollowAndDsc() {
    std::vector<WefaxSlot> v = {{"a", 1000}, {"b", 2000}, {"a", 3000}};
    CHECK(wefaxFollowPick(v, "", 900) == 0);
    CHECK(wefaxFollowPick(v, "b", 900) == 1);
    CHECK(wefaxFollowPick(v, "", 1061) == 1);          // more than a minute late: the next one
    CHECK(wefaxFollowPick(v, "a", 1500) == 2);
    CHECK(wefaxFollowPick(v, "c", 900) == -1);
    CHECK(wefaxFollowPick({}, "", 900) == -1);
    CHECK(!wefaxFollowDue(1000, 700, 120, 0, false));   // too early
    CHECK(wefaxFollowDue(1000, 900, 120, 0, false));
    CHECK(!wefaxFollowDue(1000, 900, 120, 1000, false)); // once per broadcast
    CHECK(!wefaxFollowDue(1000, 900, 120, 0, true));     // never over a chart under way
    CHECK(dscWorkingKhz("8291.0 kHz") == 8291.0);
    CHECK(dscWorkingKhz("2182.0 kHz") == 2182.0);
    CHECK(dscWorkingKhz("VHF ch 16") == 0);
    CHECK(dscWorkingKhz("") == 0);
    CHECK(dscWorkingKhz("kHz") == 0);
    CHECK(dscWorkingKhz("90000.0 kHz") == 0);            // not MF/HF
    CHECK(dscWorkingKhz("500.0 kHz") == 0);
}

static void testRegions() {
    CHECK(modeNotUsedIn("ATSC", "LT"));
    CHECK(!modeNotUsedIn("ATSC", "US"));
    CHECK(!modeNotUsedIn("ATSC", ""));                 // no country: no tag
    CHECK(!modeNotUsedIn("FM radio", "LT"));           // no tag on modes that are not in the table
    CHECK(modeNotUsedIn("LBJ", "DE")); CHECK(!modeNotUsedIn("LBJ", "CN"));
    CHECK(modeNotUsedIn("UAT", "CA")); CHECK(!modeNotUsedIn("UAT", "US"));
    CHECK(!modeNotUsedIn("NOAA Weather Radio SAME", "CA"));
    CHECK(!modeNotUsedIn("ISDB-T", "BR"));
    CHECK(modeNotUsedIn("ATSC", "U"));                 // a partial code does not match "US"
}

int testNavtex();

int main() {
    testSonde(); testGeo(); testPreselect(); testFollowAndDsc(); testRegions();
    fails += testNavtex();
    if (!fails) printf("ease ok\n");
    return fails ? 1 : 0;
}
