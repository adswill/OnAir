// My station: verdicts, the automatic radio choice, the assignment of several modes and the single-capture plan.
#include "dect2/station/station.h"
#include <cmath>
#include <cstdio>
using namespace dect2::station;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static Radio radio(const char* id, double lo, double hi, double rate, bool bias = false) {
    Radio r; r.id = id; r.name = id; r.minHz = lo; r.maxHz = hi; r.maxRateHz = rate; r.biasTee = bias; return r;
}
static Antenna ant(const char* n, double lo, double hi, uint8_t f = 0, const char* radioId = "") {
    Antenna a; a.name = n; a.loHz = lo; a.hiHz = hi; a.flags = f; a.radioId = radioId; return a;
}
static ModeNeed need(double hz, double rate, uint8_t f = 0) { ModeNeed n; n.freqHz = hz; n.minRateHz = rate; n.flags = f; return n; }
static bool has(const Verdict& v, Missing m) { for (Missing x : v.missing) if (x == m) return true; return false; }

static Station base() {
    Station s;
    s.radios.push_back(radio("rtl", 24e6, 1766e6, 3.2e6));
    s.radios.push_back(radio("hackrf", 1e6, 6000e6, 20e6));
    s.antennas.push_back(ant("whip", 70e6, 1000e6));
    s.antennas.push_back(ant("1090", 1030e6, 1100e6, 0, "rtl"));
    return s;
}

static void testVerdicts() {
    Station s = base();
    // no radios: no claim
    { Station e; CHECK(assess(e, need(100e6, 2e6)).level == Level::Unknown); }
    Verdict v = assess(s, need(100e6, 2e6));
    CHECK(v.level == Level::Ready); CHECK(v.radio == 0);   // the narrow radio is enough, the wide one stays free
    CHECK(s.antennas[(size_t)v.antenna].name == "whip");
    v = assess(s, need(1090e6, 2e6));
    CHECK(v.level == Level::Ready); CHECK(s.antennas[(size_t)v.antenna].name == "1090"); CHECK(v.radio == 0);
    // only the wide radio has the rate
    v = assess(s, need(100e6, 8e6)); CHECK(v.level == Level::Ready); CHECK(v.radio == 1);
    // only the wide radio tunes it, and no antenna is listed for 5.8 GHz
    v = assess(s, need(5800e6, 2e6)); CHECK(v.level == Level::Partial); CHECK(v.radio == 1); CHECK(has(v, Missing::AntennaBand));
    // nobody reaches it
    v = assess(s, need(10000e6, 2e6)); CHECK(v.level == Level::Cannot); CHECK(has(v, Missing::RadioRange));
    // too fast for every radio
    v = assess(s, need(100e6, 40e6)); CHECK(v.level == Level::Cannot); CHECK(has(v, Missing::RadioRate));
    // a dish mode with no dish, then with one; the radio's range is checked on the IF
    v = assess(s, need(1200e6, 2e6, ADish)); CHECK(v.level == Level::Partial); CHECK(has(v, Missing::Dish));
    s.antennas.push_back(ant("dish", 950e6, 2150e6, ADish));
    v = assess(s, need(1200e6, 2e6, ADish)); CHECK(v.level == Level::Ready); CHECK(s.antennas[(size_t)v.antenna].name == "dish");
    // a dish does not count for a terrestrial mode at a frequency inside its IF range
    Station d; d.radios.push_back(radio("r", 1e6, 6000e6, 20e6)); d.antennas.push_back(ant("dish", 950e6, 2150e6, ADish));
    v = assess(d, need(1090e6, 2e6)); CHECK(v.level == Level::Partial); CHECK(has(v, Missing::AntennaBand));
    // an active antenna without a bias tee is a note, with one it is ready
    Station g; g.radios.push_back(radio("r", 1e6, 6000e6, 20e6)); g.antennas.push_back(ant("patch", 1160e6, 1610e6, AActive));
    v = assess(g, need(1575e6, 2e6, AActive)); CHECK(v.level == Level::Partial); CHECK(has(v, Missing::NoAntennaPower));
    g.radios[0].biasTee = true;
    v = assess(g, need(1575e6, 2e6, AActive)); CHECK(v.level == Level::Ready);
    // no antennas entered: only the radio is judged
    Station r; r.radios.push_back(radio("r", 24e6, 1766e6, 3.2e6));
    v = assess(r, need(100e6, 2e6)); CHECK(v.level == Level::Ready); CHECK(v.antenna == -1);
    // band edges: 5 % outside still counts, 30 % does not
    Station m; m.radios.push_back(radio("r", 1e6, 6000e6, 20e6)); m.antennas.push_back(ant("a", 100e6, 200e6));
    CHECK(assess(m, need(205e6, 2e6)).level == Level::Ready);
    CHECK(assess(m, need(260e6, 2e6)).level == Level::Partial);
    CHECK(assess(m, need(95e6, 2e6)).level == Level::Ready);
    CHECK(assess(m, need(60e6, 2e6)).level == Level::Partial);
    // an unknown frequency or rate makes no claim about either
    CHECK(assess(m, need(0, 0)).level == Level::Ready);
}

static void testBusyAndUnplugged() {
    Station s = base();
    Verdict v = assess(s, need(100e6, 2e6), {true, false});
    CHECK(v.radio == 1); CHECK(v.level == Level::Ready);
    v = assess(s, need(100e6, 2e6), {true, true});
    CHECK(v.radio == -1); CHECK(v.level == Level::Cannot); CHECK(has(v, Missing::NoRadioPlugged));
    s.radios[0].present = false;
    v = assess(s, need(100e6, 2e6));
    CHECK(v.radio == 1);
    s.radios[1].present = false;
    v = assess(s, need(100e6, 2e6)); CHECK(v.radio == -1);
}

static void testPick() {
    Station s = base();
    // the current one does the job: never moved, even with a "better" one around
    CHECK(pickRadio(s, need(100e6, 2e6), 1) == 1);
    CHECK(pickRadio(s, need(100e6, 2e6), 0) == 0);
    // the current one cannot: moved
    CHECK(pickRadio(s, need(100e6, 8e6), 0) == 1);
    CHECK(pickRadio(s, need(5800e6, 2e6), 0) == 1);
    // the current one is busy or gone: moved
    CHECK(pickRadio(s, need(100e6, 2e6), 0, {true, false}) == 1);
    Station g = s; g.radios[0].present = false;
    CHECK(pickRadio(g, need(100e6, 2e6), 0) == 1);
    // nobody can do it: stays
    CHECK(pickRadio(s, need(10000e6, 2e6), 0) == 0);
    // not one of the station's radios (test signal, file)
    CHECK(pickRadio(s, need(100e6, 2e6), -1) == -1);
    // the only other is busy: stays
    CHECK(pickRadio(s, need(100e6, 8e6), 0, {false, true}) == 0);
}

static void testAssign() {
    Station s = base();
    // the wide mode must get the wide radio even when listed second
    std::vector<int> a = assignRadios(s, {need(100e6, 2e6), need(100e6, 8e6)});
    CHECK(a[0] == 0); CHECK(a[1] == 1);
    a = assignRadios(s, {need(100e6, 8e6), need(100e6, 2e6)});
    CHECK(a[0] == 1); CHECK(a[1] == 0);
    // three modes, two radios: the first two get them
    a = assignRadios(s, {need(100e6, 2e6), need(120e6, 2e6), need(130e6, 2e6)});
    CHECK(a[0] >= 0); CHECK(a[1] >= 0); CHECK(a[2] == -1); CHECK(a[0] != a[1]);
    // an impossible mode does not take a radio from a possible one
    a = assignRadios(s, {need(10000e6, 2e6), need(100e6, 2e6)});
    CHECK(a[0] == -1); CHECK(a[1] == 0);
    // busy radios are left alone
    a = assignRadios(s, {need(100e6, 2e6), need(120e6, 2e6)}, {false, true});
    CHECK(a[0] == 0); CHECK(a[1] == -1);
    // Ready beats Partial across the set: mode 0 can only be Partial on the wide radio, mode 1 Ready on it
    Station t;
    t.radios.push_back(radio("n", 24e6, 1766e6, 3.2e6)); t.radios.push_back(radio("w", 1e6, 6000e6, 20e6));
    t.antennas.push_back(ant("low", 80e6, 110e6, 0, "w")); t.antennas.push_back(ant("high", 1000e6, 1200e6, 0, "n"));
    a = assignRadios(t, {need(100e6, 2e6), need(1090e6, 2e6)});
    CHECK(a[0] != a[1]); CHECK(a[0] >= 0); CHECK(a[1] >= 0);
    // sizes: no radios, no modes
    Station e;
    CHECK(assignRadios(e, {need(100e6, 2e6)})[0] == -1);
    CHECK(assignRadios(s, {}).empty());
    // many radios: 12 radios and 12 modes finish and all get one
    Station big;
    std::vector<ModeNeed> many;
    for (int i = 0; i < 12; i++) { big.radios.push_back(radio(("r" + std::to_string(i)).c_str(), 1e6, 6000e6, 20e6)); many.push_back(need(100e6 + i * 1e6, 2e6)); }
    a = assignRadios(big, many);
    int got = 0, seen = 0;
    for (int r : a) { if (r >= 0) { got++; CHECK(!((seen >> r) & 1)); seen |= 1 << r; } }
    CHECK(got == 12);
}

static void testText() {
    Station s = base();
    s.radios[1].biasTee = true;
    s.antennas.push_back(ant("na\tme", 1e6, 2e6, ADish | AActive, "hackrf"));
    Station b = fromText(toText(s));
    CHECK(b.radios.size() == 2); CHECK(b.antennas.size() == 3);
    CHECK(b.radios[0].id == "rtl"); CHECK(b.radios[1].biasTee); CHECK(!b.radios[0].biasTee);
    CHECK(std::fabs(b.radios[0].maxHz - 1766e6) < 1);
    CHECK(b.antennas[2].name == "na me"); CHECK(b.antennas[2].flags == (ADish | AActive)); CHECK(b.antennas[2].radioId == "hackrf");
    CHECK(!b.radios[0].present);   // the app marks the radios it finds
    // junk and future lines are skipped, CRLF is fine
    Station c = fromText("hello\n\nX\ta\tb\nR\tid\tname\t1\t2\t3\t0\r\nA\n");
    CHECK(c.radios.size() == 1); CHECK(c.radios[0].name == "name"); CHECK(c.antennas.empty());
    CHECK(fromText("").radios.empty());
    for (const AntennaPreset& p : antennaPresets()) CHECK(p.hiHz > p.loHz);
}

static void testPlan() {
    // two channels inside 2 Msps
    CapturePlan p = planCapture({{144.8e6, 25e3}, {145.5e6, 25e3}}, 20e6);
    CHECK(p.fits); CHECK(p.outside.empty());
    CHECK(p.centerHz - p.rateHz * 0.4 <= 144.8e6 - 12.5e3); CHECK(p.centerHz + p.rateHz * 0.4 >= 145.5e6 + 12.5e3);
    CHECK(p.rateHz < 2e6);
    // minimum rate respected
    p = planCapture({{144.8e6, 25e3}}, 20e6, 2e6); CHECK(p.fits); CHECK(p.rateHz == 2e6);
    // too far apart for the radio: the bigger group wins
    p = planCapture({{100e6, 200e3}, {162e6, 25e3}, {161.975e6, 25e3}}, 3.2e6);
    CHECK(!p.fits); CHECK(p.outside.size() == 1); CHECK(p.outside[0] == 0);
    // a tie goes to the earlier channel of the list
    p = planCapture({{162e6, 25e3}, {100e6, 25e3}}, 3.2e6);
    CHECK(!p.fits); CHECK(p.outside.size() == 1); CHECK(p.outside[0] == 1);
    // one channel wider than the radio
    p = planCapture({{600e6, 8e6}}, 3.2e6); CHECK(!p.fits); CHECK(p.outside.size() == 1); CHECK(p.rateHz == 0);
    // no limit
    p = planCapture({{100e6, 1e6}, {900e6, 1e6}}, 0); CHECK(p.fits); CHECK(p.rateHz > 800e6);
    // nothing
    CHECK(!planCapture({}, 20e6).fits);
    // the centre stays off a channel when there is room: a channel in the middle of three
    p = planCapture({{100e6, 25e3}, {100.5e6, 25e3}, {101e6, 25e3}}, 20e6, 4e6);
    CHECK(p.fits);
    CHECK(!(p.centerHz >= 100.5e6 - 12.5e3 && p.centerHz <= 100.5e6 + 12.5e3));
    CHECK(p.centerHz - p.rateHz * 0.4 <= 100e6 - 12.5e3); CHECK(p.centerHz + p.rateHz * 0.4 >= 101e6 + 12.5e3);
    // exactly filling the usable part
    p = planCapture({{100e6, 8e6}}, 10e6); CHECK(p.fits); CHECK(std::fabs(p.rateHz - 10e6) < 1);
    // every order of three channels gives the same window
    double f[3] = {430e6, 433.9e6, 435e6};
    int perm[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    for (auto& q : perm) {
        p = planCapture({{f[q[0]], 25e3}, {f[q[1]], 25e3}, {f[q[2]], 25e3}}, 20e6);
        CHECK(p.fits); CHECK(p.centerHz - p.rateHz * 0.4 <= 430e6 - 12.5e3); CHECK(p.centerHz + p.rateHz * 0.4 >= 435e6 + 12.5e3);
    }
}

int main() {
    testVerdicts(); testBusyAndUnplugged(); testPick(); testAssign(); testText(); testPlan();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("station ok\n");
    return 0;
}
