// 4.0 Lane B group 3: AIS coverage / name cache, airband ATIS tracker, pager keywords and groups, callsigns in text, mode guess, spot summary,
// ISM burst classifier. Simulated data only (no recorded signals).
#include "dect2/ais/ais_coverage.h"
#include "dect2/airband/airband_atis.h"
#include "dect2/pager/pager_watch.h"
#include "dect2/ham/ham_callsign.h"
#include "dect2/ham/ham_modeguess.h"
#include "dect2/ham/ham_spotsum.h"
#include "dect2/ism/ism_classify.h"
#include "dect2/adsb/adsb_loiter.h"
#include "dect2/ham/ham_clearfreq.h"
#include "dect2/ham/ham_bandhop.h"
#include "dect2/tones/dtmf_numbers.h"
#include "dect2/acars/acars_load.h"
#include "dect2/sarsat/sarsat_incidents.h"
#include <cstdio>
#include <random>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using namespace dect2;

static void aisTest() {
    AisCoverage cv(36); cv.setStation(55.0, 21.0);
    CHECK(cv.sectorCount() == 36);
    cv.add(0, 0, 1); cv.add(95, 21, 1);   // ignored
    // a ship 50 km due east (sector 9 = 90..100 deg) and ships to the north up to 120 km; antimeridian-free
    double la, lo;
    track::gcDestination(55.0, 21.0, 92, 50000, la, lo); cv.add(la, lo, 10);
    for (int i = 1; i <= 20; i++) { track::gcDestination(55.0, 21.0, 5, 6000.0 * i, la, lo); cv.add(la, lo, 10 + i); }
    auto s = cv.sectors(100, 3600);
    CHECK(s[9].reports == 1 && std::fabs(s[9].maxKm - 50) < 1.0);
    CHECK(s[0].reports == 20 && std::fabs(s[0].maxKm - 120) < 2.0);
    CHECK(s[0].typicalKm < s[0].maxKm && s[0].typicalKm > 100);   // p90 of 6..120 km
    CHECK(cv.sectors(100, 5)[0].reports == 0);                       // window
    // blind sector: reports at 80 and 100 degrees but none at 90
    AisCoverage c2(36); c2.setStation(55, 21);
    track::gcDestination(55, 21, 75, 30000, la, lo); c2.add(la, lo, 1);
    track::gcDestination(55, 21, 95, 30000, la, lo); c2.add(la, lo, 1);
    track::gcDestination(55, 21, 85, 30000, la, lo); c2.add(la, lo, 1);
    track::gcDestination(55, 21, 105, 30000, la, lo); c2.add(la, lo, 1);
    track::gcDestination(55, 21, 115, 30000, la, lo); c2.add(la, lo, 1);
    // bearings 75 (sector 7), 85 (8), 95 (9), 105 (10), 115 (11): none blind; remove 95 case:
    AisCoverage c3(36); c3.setStation(55, 21);
    for (double b : {75.0, 85.0, 105.0, 115.0}) { track::gcDestination(55, 21, b, 30000, la, lo); c3.add(la, lo, 1); }
    auto bl = c3.blindSectors(2, 100);
    CHECK(bl.size() == 1 && bl[0] == 9);
    CHECK(c2.blindSectors(2, 100).empty());
    // type mix
    AisTypeMix m; for (int t : {70, 71, 79, 80, 60, 30, 37, 36, 0, 99}) m.add(t);
    CHECK(m.total() == 10 && m.ranked()[0].first == AisClass::Cargo && m.ranked()[0].second == 3);
    CHECK(aisTypeClass(52) == AisClass::Tug && aisTypeClass(35) == AisClass::Military);
    // name cache: part A name, part B call sign; blanks never erase; '@' padding
    AisNameCache nc;
    nc.learn(123456789, "MAERSK@@@@@", "", 0, 0, 10);
    nc.learn(123456789, "", "OXAB2", 71, 300, 20);
    nc.learn(123456789, "@@@@", "", 0, 0, 30);
    CHECK(nc.label(123456789) == "MAERSK" && nc.find(123456789)->callsign == "OXAB2" && nc.find(123456789)->shipType == 71 && nc.find(123456789)->lengthM == 300);
    CHECK(nc.label(42) == "42");
    nc.prune(25); CHECK(nc.size() == 1); nc.prune(35); CHECK(nc.size() == 0);
}

static void atisTest() {
    CHECK(atisInformation("Vilnius information Kilo, wind 270 at 8 knots") == 'K');
    CHECK(atisInformation("this is Heathrow ATIS Alfa") == 'A');
    CHECK(atisInformation("information X-ray") == 'X');
    CHECK(atisInformation("information November, QNH 1013") == 'N');
    CHECK(atisInformation("no letter here, information is available") == 0);
    AtisTracker tr;
    CHECK(!tr.feed(118005000, "", "information Kilo", 0));
    CHECK(!tr.feed(118005000, "", "information Kilo", 60));
    CHECK(tr.isAtis(118005000));
    CHECK(tr.feed(118005000, "", "information Lima", 3600));
    CHECK(tr.find(118005000)->letter == 'L' && tr.find(118005000)->changes == 1 && tr.ageS(118005000, 3700) == 100);
    CHECK(!tr.isAtis(120000000));
    tr.feed(121500000, "ATIS west", "", 5); CHECK(tr.isAtis(121500000) && tr.atisChannels().size() == 2);
    // 8.33 channel and its 25 kHz name map to the same 1 kHz bucket
    CHECK(!tr.feed(118005800, "", "information Lima", 3700) && tr.find(118005000)->broadcasts == 3);
    auto v = volmetStations("London VOLMET. EGLL wind 250 over 10 knots, EHAM CAVOK, echo delta delta foxtrot");
    CHECK(v.size() == 3 && v[0] == "EGLL" && v[1] == "EHAM" && v[2] == "EDDF");
}

static void pagerTest() {
    PagerKeywords k; k.set({"fire", "*alarm", "structure fire", "  ", "MEDICAL"});
    CHECK(k.size() == 4);
    auto h = k.match("Structure Fire at 12 Main St, 2nd alarm");
    CHECK(h.size() == 3);   // fire (whole word), *alarm, structure fire
    CHECK(k.match("firefighter training").empty());
    CHECK(k.match("FALSEALARM test").size() == 1);
    CHECK(k.match("medical aid").size() == 1);
    // groups: capcodes 100,101,102 are always paged together; 200 alone; 300+301 paged together twice only (below minCo)
    PagerGroups g;
    std::mt19937 rng(3); double t = 0;
    for (int i = 0; i < 6; i++) { t += 600 + rng() % 300; g.add(100, t); g.add(101, t + 2); g.add(102, t + 5); }
    for (int i = 0; i < 6; i++) { t += 600; g.add(200, t); }
    for (int i = 0; i < 2; i++) { t += 600; g.add(300, t); g.add(301, t + 3); }
    auto gr = g.groups();
    CHECK(gr.size() == 1 && gr[0].capcodes.size() == 3 && gr[0].capcodes[0] == 100 && gr[0].incidents == 6);
    // a roll call of 80 capcodes inside one burst does not link everything
    PagerGroups r; for (uint32_t c = 1; c <= 80; c++) r.add(1000 + c, 10 + c * 0.1);
    CHECK(r.groups().empty());
}

static void callTest() {
    auto c = hamFindCallsigns("CQ CQ DE LY1ABC LY1ABC K");
    CHECK(c.size() == 2 && c[0] == "LY1ABC");
    c = hamFindCallsigns("de dl/ly1abc/p tu 599 5nn");   // lower case input
    CHECK(c.size() == 1);
    c = hamFindCallsigns("RST 599 QTH VILNIUS NAME JONAS TEST1");
    CHECK(c.empty());
    c = hamFindCallsigns("9A1AA 4X6TT 3DA0RS W1AW K1ABC/4 JA1YKX");
    CHECK(c.size() == 6);
    auto l = hamParseCall("CQ DX CQ DX DE 9A1AA 9A1AA K");
    CHECK(l.kind == HamCallLine::Cq && l.from == "9A1AA" && l.cqType == "DX");
    l = hamParseCall("LY1ABC DE DL2XYZ UR 599 5NN");
    CHECK(l.kind == HamCallLine::Reply && l.to == "LY1ABC" && l.from == "DL2XYZ" && l.report == "599");
    l = hamParseCall("hello world"); CHECK(l.kind == HamCallLine::None);
    HamCallHeard h; h.feed("CQ TEST DE W1AW", 10); h.feed("W1AW DE LY1ABC 599", 20); h.feed("CQ DE JA1YKX K", 30);
    auto lst = h.list();
    CHECK(lst.size() == 3 && lst[0].call == "JA1YKX" && h.size() == 3);
    bool w1awCq = false; for (auto& e : lst) if (e.call == "W1AW") w1awCq = e.cq && e.count == 2;
    CHECK(w1awCq);
}

static void guessTest() {
    HamSigFeatures f; f.onOff = true; f.bwHz = 100; f.symbolRateBd = 15;
    auto g = hamGuessMode(f); CHECK(!g.empty() && g[0].mode == "CW");
    f = {}; f.bwHz = 62; f.tones = 1; f.symbolRateBd = 31.25;
    g = hamGuessMode(f); CHECK(!g.empty() && g[0].mode == "PSK31");
    f = {}; f.tones = 2; f.toneSpacingHz = 170; f.symbolRateBd = 45.45; f.bwHz = 250;
    g = hamGuessMode(f); CHECK(!g.empty() && g[0].mode == "RTTY" && g[0].score > 0.9);
    f.toneSpacingHz = 850; f.symbolRateBd = 50; g = hamGuessMode(f); CHECK(g[0].mode == "RTTY");
    f.toneSpacingHz = 300; g = hamGuessMode(f); CHECK(g.empty());   // not a standard shift
    f = {}; f.tones = 8; f.toneSpacingHz = 6.25; f.slotS = 15; f.bwHz = 50;
    g = hamGuessMode(f); CHECK(g[0].mode == "FT8");
    f = {}; f.tones = 4; f.toneSpacingHz = 20.83; f.slotS = 7.5; g = hamGuessMode(f); CHECK(g[0].mode == "FT4");
    f = {}; f.tones = 4; f.toneSpacingHz = 1.4648; f.slotS = 120; g = hamGuessMode(f); CHECK(g[0].mode == "WSPR");
    f = {}; f.tones = 32; f.bwHz = 1000; g = hamGuessMode(f); CHECK(!g.empty() && g[0].mode == "Olivia");
    f = {}; f.tones = 16; f.symbolRateBd = 15.625; f.bwHz = 316; g = hamGuessMode(f); CHECK(g[0].mode == "MFSK16");
    // tolerance: slightly wrong measurements still classify (clock error of 3 %)
    f = {}; f.tones = 8; f.toneSpacingHz = 6.25 * 1.03; f.slotS = 15.2; g = hamGuessMode(f); CHECK(!g.empty() && g[0].mode == "FT8");
}

static void spotTest() {
    HamSpotSummary s; s.setStation(55, 21);
    double la, lo;
    for (int i = 0; i < 6; i++) { track::gcDestination(55, 21, 135, 3000000 + i * 500000, la, lo); s.add({"20m", 100.0 + i, -10.0 + i, la, lo, true}); }
    for (int i = 0; i < 3; i++) { track::gcDestination(55, 21, 270, 1500000, la, lo); s.add({"40m", 100.0 + i, -5.0, la, lo, true}); }
    s.add({"40m", -1000.0, -5, la, lo, true});   // too old
    s.add({"15m", 100, -20, 0, 0, false});   // no position: counted, no distance
    auto v = s.summary(200, 300);
    CHECK(v.size() == 3 && v[0].band == "20m" && v[0].spots == 6);
    CHECK(v[0].sector[3] == 6 && v[0].openSectors == 1 && v[0].farthestKm > 5400 && v[0].farthestKm < 5600);
    CHECK(v[0].line.find("open to SE") != std::string::npos);
    CHECK(v[1].band == "40m" && v[1].spots == 3 && v[1].sector[6] == 3);
    CHECK(v[2].farthestKm == 0 && v[2].spots == 1);
}

static std::vector<IsmPulse> mkPwm(std::mt19937& rng, double jit, int rows, int bits) {
    std::vector<IsmPulse> p; std::uniform_real_distribution<double> j(-jit, jit);
    std::mt19937 data(5);
    for (int r = 0; r < rows; r++)
        for (int b = 0; b < bits; b++) {
            const bool one = data() & 1;
            p.push_back({(one ? 400 : 200) * (1 + j(rng)), (b == bits - 1 ? 9000.0 : 300.0) * (1 + j(rng) * (b == bits - 1 ? 0.0 : 1.0))});
        }
    return p;
}
static void ismTest() {
    std::mt19937 rng(11);
    for (double jit : {0.0, 0.05, 0.1}) {
        auto c = ismClassify(mkPwm(rng, jit, 4, 36));
        CHECK(c.coding == "PWM" && c.rows == 4 && c.bitsPerRow == 36);
        CHECK(std::fabs(c.shortUs - 200) < 40 && std::fabs(c.longUs - 400) < 60);
        CHECK(c.flex.find("OOK_PWM") != std::string::npos);
    }
    // PPM: constant 100 us pulse, gap 500 / 1000 us
    std::vector<IsmPulse> p; std::mt19937 d(9); std::uniform_real_distribution<double> j(-0.06, 0.06);
    for (int r = 0; r < 3; r++) for (int b = 0; b < 40; b++) p.push_back({100 * (1 + j(rng)), b == 39 ? 8000.0 : ((d() & 1) ? 1000 : 500) * (1 + j(rng))});
    auto c = ismClassify(p);
    CHECK(c.coding == "PPM" && c.rows == 3 && c.bitsPerRow == 40 && std::fabs(c.shortUs - 500) < 60 && std::fabs(c.longUs - 1000) < 100);
    // Manchester: unit 250 us, pulses and gaps of 250 or 500
    std::vector<IsmPulse> m; std::mt19937 e(4);
    for (int k = 0; k < 60; k++) m.push_back({(e() & 1) ? 500.0 : 250.0, (k == 59) ? 9000.0 : ((e() & 1) ? 500.0 : 250.0)});
    c = ismClassify(m); CHECK(c.coding == "Manchester" && std::fabs(c.shortUs - 250) < 30);
    // noise: uniformly random pulse and gap lengths must not be reported as a coded protocol with confidence
    { std::uniform_real_distribution<double> u(80, 2000); int claims = 0;
      for (int k = 0; k < 200; k++) { std::vector<IsmPulse> n; for (int i = 0; i < 40; i++) n.push_back({u(rng), u(rng)}); if (ismClassify(n).confidence >= 0.8) claims++; }
      CHECK(claims <= 10); }
    // too short to say
    CHECK(ismClassify({{100, 100}, {100, 100}}).coding == "unknown");
}

static void loiterTest() {
    AdsbLoiterDetector d; double la, lo; bool orbit = false;
    // an aircraft in a holding circle: radius 5 km, 140 kt (72 m/s), 2 laps; fix every 5 s
    const double R = 5000, w = 72.0 / R;   // rad/s
    for (double t = 0; t < 900; t += 5) {
        track::gcDestination(55, 21, std::fmod(w * t * 180 / 3.14159265, 360.0), R, la, lo); orbit = d.add(1, t, la, lo);
    }
    CHECK(orbit && d.find(1) && d.find(1)->turns > 1.5 && d.find(1)->radiusKm > 4.5 && d.find(1)->radiusKm < 5.6 && d.find(1)->clockwise);
    CHECK(std::fabs(d.find(1)->centreLat - 55) < 0.05);
    // counter-clockwise
    AdsbLoiterDetector e; for (double t = 0; t < 900; t += 5) { track::gcDestination(40, 10, std::fmod(360 - w * t * 180 / 3.14159265, 360.0), R, la, lo); e.add(2, t, la, lo); }
    CHECK(e.find(2) && !e.find(2)->clockwise);
    // an airliner flying straight with a 30 degree bend is not an orbit; with GPS jitter (60 m) neither
    AdsbLoiterDetector s; std::mt19937 rng(8); std::normal_distribution<double> n(0, 0.0005);
    for (double t = 0; t < 900; t += 5) { track::gcDestination(50, 8, t < 450 ? 80 : 110, 230.0 * t, la, lo); s.add(3, t, la + n(rng), lo + n(rng)); }
    CHECK(!s.find(3));
    // a parked aircraft (ADS-B jitter on the ground) is not an orbit
    AdsbLoiterDetector p; for (double t = 0; t < 900; t += 5) p.add(4, t, 52.0 + n(rng), 5.0 + n(rng));
    CHECK(!p.find(4));
    // a very wide loop (a route that bends, 60 km) is not an orbit
    AdsbLoiterDetector wd; for (double t = 0; t < 900; t += 5) { track::gcDestination(45, 5, std::fmod(0.0012 * t * 180 / 3.14159265 * 4.0, 360.0), 60000, la, lo); wd.add(5, t, la, lo); }
    CHECK(!wd.find(5));
    d.expire(2000); CHECK(d.all().empty());
}
static void clearTest() {
    std::vector<SlotSignal> s;
    for (double f : {300.0, 500.0, 800.0, 1000.0, 1050.0, 2400.0}) s.push_back({f, 0, 50});
    for (double f : {600.0, 2000.0}) s.push_back({f, 1, 50});
    auto c = findClearFreq(s);
    CHECK(c.signals[0] == 6 && c.signals[1] == 2 && c.quieterParity == 1);
    // even: widest gap is 1100+..2400-: [1085, 2365] centre 1725
    CHECK(std::fabs(c.hz[0] - 1725) < 1 && c.gapHz[0] > 1200);
    // odd: gaps [200,565] [635,1965] [2035,2900]: widest is the middle one
    CHECK(c.hz[1] > 1200 && c.hz[1] < 1400);
    // no room: every 60 Hz occupied
    std::vector<SlotSignal> full; for (double f = 200; f < 2900; f += 60) full.push_back({f, 0, 50});
    auto fc = findClearFreq(full); CHECK(fc.hz[0] == 0 && fc.hz[1] > 0);
    // overlapping signals merge
    auto o = findClearFreq({{1000, 0, 50}, {1010, 0, 50}, {1020, 0, 50}}); CHECK(o.gapHz[0] > 0 && o.signals[0] == 3);
}
static void sarsatTest() {
    SarsatIncidents inc; sarsat::Beacon b; b.valid = true; b.hexId = "ADCD0123456789A"; b.beaconType = "EPIRB"; b.country = "Lithuania";
    std::mt19937 rng(6); double t = 1000;
    for (int i = 0; i < 6; i++) { b.hasPos = true; b.lat = 55.0 + i * 0.01; b.lon = 21.0; inc.add(b, t); inc.add(b, t + 0.5); t += 47 + rng() % 10; }
    sarsat::Beacon test = b; test.hexId = "A1B2C3D4E5F6071"; test.selfTest = true; test.hasPos = false; inc.add(test, 5); inc.add(test, 60);
    sarsat::Beacon bad = b; bad.valid = false; bad.hexId = "BAD"; inc.add(bad, 1);
    auto l = inc.list();
    CHECK(l.size() == 2 && l[0].hexId == "ADCD0123456789A" && l[0].bursts == 6 && l[0].alert && l[0].plausibleRepeat);
    CHECK(l[0].driftKm > 4.5 && l[0].driftKm < 6.5 && l[0].medianGapS > 46 && l[0].medianGapS < 58);
    CHECK(l[1].tests == 2 && !l[1].alert && l[1].bursts == 0);
    // a decoder echo every 5 s is not a plausible beacon
    SarsatIncidents e; for (int i = 0; i < 10; i++) e.add(b, 100 + 5.0 * i);
    CHECK(!e.list()[0].plausibleRepeat && !e.list()[0].alert);
}

static void hopTest() {
    HamBandHop h; h.setBands({"40m", "20m", "15m", "10m"}); h.exclude("10m", true);
    std::map<std::string, int> slots; std::string prev; int changes = 0; double lastSwitch = 0, minDwell = 1e9;
    for (int i = 0; i < 960; i++) {   // 4 hours of 15 s slots
        const double t = i * 15.0;
        // 20 m is busy, 40 m quiet, 15 m silent; a spot is only heard while the receiver is on that band
        const std::string b = h.next(t);
        slots[b]++;
        if (b != prev) { if (!prev.empty()) { changes++; minDwell = std::min(minDwell, t - lastSwitch); } lastSwitch = t; prev = b; }
        if (b == "20m" && i % 2 == 0) h.spot(b, t);
        if (b == "40m" && i % 20 == 0) h.spot(b, t);
    }
    CHECK(slots["10m"] == 0);                                   // excluded
    CHECK(slots["20m"] > 10 * slots["15m"] && slots["40m"] >= slots["15m"] && slots["15m"] >= 16);   // busy band most, silent band still visited
    CHECK(minDwell >= 4 * 15.0 - 1e-9);                         // never switches before the minimum dwell
    CHECK(changes > 4 && changes < 400);
    // an opening is found: 15 m was silent for hours, then spots appear as soon as it is visited; it must be visited at least every 30 min
    HamBandHop g; g.setBands({"20m", "15m"}); double lastSeen15 = 0, maxGap = 0;
    for (int i = 0; i < 960; i++) { const double t = i * 15.0; const std::string b = g.next(t); if (b == "20m") g.spot(b, t); if (b == "15m") { maxGap = std::max(maxGap, t - lastSeen15); lastSeen15 = t; } }
    CHECK(maxGap < 2000 + 15 * 4);
}

static void dtmfTest() {
    DtmfNumbers d; double t = 0;
    auto send = [&](const char* k) { for (const char* c = k; *c; c++) { d.key(*c, t); t += 0.2; } };
    send("0037061234567"); t += 5; send("*21#"); t += 5; send("123"); t += 5; send("*21#");   // code repeated within 120 s
    d.tick(t + 10);
    auto& e = d.entries();
    CHECK(e.size() == 3);
    CHECK(e[0].keys == "0037061234567" && e[0].kind == "number" && e[1].keys == "*21#" && e[1].kind == "code" && e[1].times == 2 && e[2].kind == "short");
    // after 120 s the same code is a new entry; a star code without # is "star"
    send("*21#"); t += 300; send("*21#"); t += 3; send("*55"); d.tick(t + 10);
    CHECK(d.entries().size() == 5 && d.entries().back().kind == "star");
}
static void acarsLoadTest() {
    AcarsLoad l; std::mt19937 rng(1);
    for (int i = 0; i < 1000; i++) { const double t = i * 0.6; const int r = rng() % 100; l.add(r < 55 ? 131.550 : r < 85 ? 131.725 : r < 95 ? 130.025 : 129.125, rng() % 10 != 0, t); }
    auto v = l.ranking(600, 600);
    CHECK(v.size() == 4 && v[0].freqMHz == 131.55 && v[0].msgs > v[1].msgs && v[1].freqMHz == 131.725);
    CHECK(v[0].perMin > 50 && v[0].goodPct > 80 && v[0].goodPct < 98);
    CHECK(std::fabs(v[3].cumulativeSharePct - 100) < 1e-9);
    auto b = l.bestChannels(600, 600, 80); CHECK(b.size() == 2 && b[0] == 131.55);
    CHECK(l.ranking(600, 1).size() <= 1 && l.ranking(5000, 10).empty());
}

int main() {
    dtmfTest(); acarsLoadTest();
    hopTest();
    loiterTest(); clearTest(); sarsatTest();
    aisTest(); atisTest(); pagerTest(); callTest(); guessTest(); spotTest(); ismTest();
    printf(fails ? "feat40c: %d FAILED\n" : "feat40c: all passed\n", fails);
    return fails != 0;
}
