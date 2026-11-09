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

static void testAutoBand() {
    // no decodes at all: rotates every dwell
    {
        AutoBand a({433.92, 868.3});
        int last = a.update(0, 0), changes = 0;
        for (int t = 1; t <= 120; t++) { const int c = a.update(t, 0); if (c != last) changes++; last = c; }
        CHECK(changes == 4); CHECK(!a.settled());
    }
    // devices only on the second band: must settle there, then keep sampling the first and come back
    {
        AutoBand a({433.92, 868.3});
        uint64_t n = 0; int on1 = 0, on0 = 0;
        for (int t = 0; t < 3600; t++) { const int c = a.update(t, n); if (c == 1 && t % 7 == 0) n++; (c ? on1 : on0)++; }
        CHECK(a.settled()); CHECK(on1 > on0 * 5);
        CHECK(on0 >= 200);    // it still looks at the other band
    }
    // first band first
    {
        AutoBand a({433.92, 868.3});
        uint64_t n = 0;
        for (int t = 0; t < 1000; t++) { const int c = a.update(t, n); if (c == 0 && t % 11 == 0) n++; }
        CHECK(a.settled()); CHECK(a.score(0) > 0); CHECK(a.score(1) == 0);
    }
    // a device that goes away and a new one elsewhere: the radio ends up on the new band
    {
        AutoBand a({433.92, 868.3});
        uint64_t n = 0; int c = 0;
        for (int t = 0; t < 2000; t++) { c = a.update(t, n); if (c == 0 && t % 5 == 0) n++; }
        for (int t = 2000; t < 14000; t++) { c = a.update(t, n); if (c == 1 && t % 5 == 0) n++; }
        int on1 = 0;
        for (int t = 14000; t < 15000; t++) { c = a.update(t, n); if (c == 1) { on1++; if (t % 5 == 0) n++; } }
        CHECK(on1 > 800);
    }
    // a counter that restarts is not a burst of decodes
    {
        AutoBand a({433.92, 868.3});
        a.update(0, 1000); a.update(1, 0); a.update(2, 0);
        CHECK(a.score(0) == 0);
    }
    { AutoBand a({433.92}); CHECK(a.update(0, 0) == 0); CHECK(a.update(1000, 5) == 0); }
    CHECK(ismBandsFor("LT")[1] == 868.3); CHECK(ismBandsFor("US")[1] == 915.0); CHECK(ismBandsFor("")[0] == 433.92);
}

static void testHunt() {
    FreqHunt h({153.025, 466.075, 169.65}, 20);
    CHECK(h.update(0, 100) == 0);
    CHECK(h.update(19, 100) == 0);
    CHECK(h.update(20, 100) == 1);
    CHECK(h.update(30, 101) == 1); CHECK(h.found());     // a decode: stays
    CHECK(h.update(500, 900) == 1);
    FreqHunt n({1.0, 2.0}, 10);
    n.update(0, 0); n.update(10, 0); n.update(20, 0);
    CHECK(n.finished()); CHECK(!n.found());
    FreqHunt r({1.0, 2.0}, 10);                           // a counter restart is not a decode
    r.update(0, 50); r.update(1, 0); CHECK(!r.found());
    FreqHunt e({}, 10); e.update(0, 0); CHECK(e.finished());
    CHECK(freqRegionApplies("GLOBAL", "LT")); CHECK(freqRegionApplies("EU", "LT")); CHECK(freqRegionApplies("DE", "DE"));
    CHECK(!freqRegionApplies("DE", "LT")); CHECK(freqRegionApplies("UK", "GB")); CHECK(!freqRegionApplies("NA", "LT"));
    CHECK(freqRegionApplies("US", ""));
}

static void testDrmHits() {
    std::vector<DrmHit> h;
    addDrmHit(h, 7325, 10); addDrmHit(h, 11650, 20); addDrmHit(h, 7325.2, 15); addDrmHit(h, 9500, 12);
    CHECK(h.size() == 3); CHECK(h[0].khz == 11650); CHECK(h[1].khz == 7325 && h[1].snrDb == 15); CHECK(h[2].khz == 9500);
}

static void testAlign() {
    AlignMeter m;
    CHECK(!m.hasPeak()); CHECK(m.fraction() == 0);
    for (int i = 0; i < 60; i++) m.update(10, true);
    CHECK(std::fabs(m.level() - 10) < 0.1); CHECK(std::fabs(m.peak() - 10) < 0.1);
    for (int i = 0; i < 60; i++) m.update(20, true);
    const float p = m.peak(); CHECK(p > 19.5f);
    for (int i = 0; i < 60; i++) m.update(5, true);
    CHECK(std::fabs(m.level() - 5) < 0.2);
    CHECK(m.peak() == p);                                 // held
    m.update(0, false); CHECK(m.level() == 0); CHECK(m.peak() == p);
    m.resetPeak(); CHECK(!m.hasPeak());
    m.update(40, true); CHECK(m.fraction() == 1.0f); CHECK(m.toneHz() == 1500.0);
    m.update(0, false); CHECK(m.toneHz() == 300.0);
}

static void testStation() {
    std::string c, l;
    CHECK(stationFill(c, l, {"", "LY2XX"}, "", true, 54.69, 25.28));
    CHECK(c == "LY2XX"); CHECK(l.size() == 6); CHECK(l.rfind("KO24", 0) == 0);
    std::string c2 = "LY1ME", l2 = "KO25";          // never replaces what was typed
    CHECK(!stationFill(c2, l2, {"LY2XX"}, "AA00", true, 54.69, 25.28));
    CHECK(c2 == "LY1ME"); CHECK(l2 == "KO25");
    std::string c3, l3;                             // no position: the known grid; no callsign anywhere: stays empty
    CHECK(stationFill(c3, l3, {}, "KO14", false, 0, 0)); CHECK(c3.empty()); CHECK(l3 == "KO14");
    std::string c4, l4;
    CHECK(!stationFill(c4, l4, {}, "", false, 0, 0)); CHECK(l4.empty());
}

int testNavtex();
int testRepeaters();

int main() {
    testSonde(); testGeo(); testPreselect(); testFollowAndDsc(); testRegions(); testAutoBand(); testHunt(); testDrmHits(); testAlign(); testStation();
    fails += testNavtex();
    fails += testRepeaters();
    if (!fails) printf("ease ok\n");
    return fails ? 1 : 0;
}
