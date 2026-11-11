// 4.0 Lane B group 3: AIS coverage / name cache, airband ATIS tracker, pager keywords and groups, callsigns in text, mode guess, spot summary,
// ISM burst classifier. Simulated data only (no recorded signals).
#include "dect2/ais/ais_coverage.h"
#include "dect2/airband/airband_atis.h"
#include "dect2/pager/pager_watch.h"
#include "dect2/ham/ham_callsign.h"
#include "dect2/ham/ham_modeguess.h"
#include "dect2/ham/ham_spotsum.h"
#include "dect2/ism/ism_classify.h"
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
    HamHeard h; h.feed("CQ TEST DE W1AW", 10); h.feed("W1AW DE LY1ABC 599", 20); h.feed("CQ DE JA1YKX K", 30);
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

int main() {
    aisTest(); atisTest(); pagerTest(); callTest(); guessTest(); spotTest(); ismTest();
    printf(fails ? "feat40c: %d FAILED\n" : "feat40c: all passed\n", fails);
    return fails != 0;
}
