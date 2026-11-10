// Ham radio insights (docs/SMART5-ham-3.9.md): who hears me, antenna pattern from decodes, busted calls, APRS network health.
// Simulated decode streams only: random stations around the world, a known antenna pattern, noise and fading on every SNR.
#include "dect2/insight/aprs_health.h"
#include "dect2/insight/ham_heard.h"
#include <cstdio>
#include <random>
#include "../common/test_util.h"

using namespace dect2::insight;


static std::string randCall(std::mt19937& r) {
    static const char* pre[] = {"DL", "LY", "G", "K", "W", "JA", "VK", "PY", "OH", "SP", "F", "EA", "I", "ZS", "VE", "UA"};
    std::uniform_int_distribution<int> p(0, 15), d(0, 9), l(0, 25), n(2, 3);   // 1-letter suffixes are rare special calls
    std::string c = pre[p(r)];
    c += (char)('0' + d(r));
    const int k = n(r);
    for (int i = 0; i < k; i++) c += (char)('A' + l(r));
    return c;
}

static void testParsing() {
    int db; bool rr;
    CHECK(parseReport("-12", db, rr) && db == -12 && !rr, "report -12");
    CHECK(parseReport("R+05", db, rr) && db == 5 && rr, "report R+05");
    CHECK(!parseReport("RR73", db, rr) && !parseReport("73", db, rr) && !parseReport("JO62", db, rr), "not reports");
    for (const char* c : {"LY1ABC", "DL1XYZ", "K1A", "2E0ABC", "4X1AB", "3DA0RU", "DL/LY1ABC", "LY1ABC/P", "VK2ABC/MM"}) CHECK(validCall(c), "valid %s", c);
    for (const char* c : {"CQ", "RR73", "ABCDEF", "123456", "LY1ABCDE", "L1", "JO62", "1ABC"}) CHECK(!validCall(c), "invalid %s", c);
    double la, lo;
    CHECK(gridToLatLon("KO24", la, lo) && std::fabs(la - 54.5) < 0.01 && std::fabs(lo - 25) < 0.01, "KO24 %.2f %.2f", la, lo);
    CHECK(gridToLatLon("JO62qm", la, lo) && std::fabs(la - 52.52) < 0.05 && std::fabs(lo - 13.38) < 0.05, "JO62qm %.3f %.3f", la, lo);
    CHECK(!gridToLatLon("RR73", la, lo), "RR73 is not a locator");
}

static void testWhoHearsMe() {
    WhoHearsMe w;
    w.setStation("LY1ABC", "KO24");
    w.add(0, "20m", "CQ DL1XYZ JO62", -8);
    w.add(0, "20m", "CQ DX VK2ABC QF56", -20);
    w.add(15, "20m", "LY1ABC DL1XYZ -12", -9);
    w.add(30, "20m", "LY1ABC <VK2ABC> R-19", -21);
    w.add(30, "20m", "OH2XX DL1XYZ RR73", -9);           // not for me
    w.add(45, "40m", "LY1ABC DL1XYZ JO62", 3);
    auto v = w.list();
    CHECK(v.size() == 3, "3 rows (DL1XYZ on 20m and 40m, VK2ABC) got %zu", v.size());
    auto best = w.bestPerBand();
    CHECK(best.count("20m") && best["20m"].call == "VK2ABC", "farthest on 20 m is VK2ABC");
    if (best.count("20m")) {
        CHECK(best["20m"].distanceKm > 14000 && best["20m"].distanceKm < 16500, "Vilnius to QF56 %.0f km", best["20m"].distanceKm);
        CHECK(best["20m"].hasReport && best["20m"].theirReport == -19 && best["20m"].balance() == 2, "VK report -19, balance %d", best["20m"].balance());
    }
    bool dl = false;
    for (const auto& r : v) if (r.call == "DL1XYZ" && r.band == "20m") {
        dl = true;
        CHECK(r.distanceKm > 700 && r.distanceKm < 1000 && r.bearing > 230 && r.bearing < 290, "DL %.0f km %.0f deg", r.distanceKm, r.bearing);
        CHECK(r.theirReport == -12 && r.balance() == -3, "DL balance %d", r.balance());
    }
    CHECK(dl, "DL1XYZ on 20 m listed");
}

// the true pattern: 7 dB toward 75 degrees, cosine shaped
static double trueGain(double b) { return 7.0 * std::cos(rad(b - 75)); }

static void testPattern() {
    int good = 0, runs = 0;
    double worst = 0;
    for (unsigned seed = 1; seed <= 20; seed++) {
        std::mt19937 r(seed);
        std::uniform_real_distribution<double> ula(-60, 70), ulo(-180, 180);
        std::normal_distribution<double> noise(0, 4.0), power(0, 3.0);
        AntennaPattern p;
        const double hl = 54.5, ho = 25;
        for (int i = 0; i < 3000; i++) {
            const double la = ula(r), lo = ulo(r), km = distanceKm(hl, ho, la, lo), b = bearingDeg(hl, ho, la, lo);
            const double snr = 30 - 12 * std::log10(km) + trueGain(b) + power(r) + noise(r);   // fading and the unknown power of the station
            p.add(b, km, (float)snr);
        }
        auto s = p.sectors(30);
        // the residuals have zero mean overall, so compare against the true gain with its own mean over the same points removed
        double tm = 0; int tn = 0;
        for (int k = 0; k < 12; k++) if (s[k].count >= 30) { tm += trueGain(k * 30 + 15) * s[k].count; tn += s[k].count; }
        tm /= std::max(1, tn);
        double err = 0;
        for (int k = 0; k < 12; k++) if (s[k].count >= 30) err = std::max(err, std::fabs(s[k].meanDb - (trueGain(k * 30 + 15) - tm)));
        const int b = p.bestSector(30);
        runs++;
        if (b == 2 && err < 2.0) good++;
        worst = std::max(worst, err);
    }
    printf("antenna pattern: best sector right with error < 2 dB in %d/%d runs, worst sector error %.2f dB\n", good, runs, worst);
    CHECK(good >= 19, "pattern %d/%d", good, runs);
    // a flat antenna has no sector stand out by more than 1.5 dB
    std::mt19937 r(99);
    std::uniform_real_distribution<double> ula(-60, 70), ulo(-180, 180);
    std::normal_distribution<double> noise(0, 4.0);
    AntennaPattern flat;
    for (int i = 0; i < 3000; i++) { const double la = ula(r), lo = ulo(r); flat.add(bearingDeg(54.5, 25, la, lo), distanceKm(54.5, 25, la, lo), (float)(20 - 12 * std::log10(distanceKm(54.5, 25, la, lo)) + noise(r))); }
    double mx = 0;
    for (const auto& s : flat.sectors(30)) if (s.count >= 30) mx = std::max(mx, std::fabs(s.meanDb));
    printf("flat antenna: largest sector %.2f dB\n", mx);
    CHECK(mx < 1.5, "flat antenna shows %.2f dB", mx);
}

static void testBusted() {
    std::mt19937 r(7);
    BustedCalls b;
    std::vector<std::string> known;
    while (known.size() < 200) { auto c = randCall(r); if (b.count(c) == 0) { known.push_back(c); for (int i = 0; i < 5; i++) b.add(c); } }
    // one character changed: caught
    int caught = 0, right = 0;
    std::uniform_int_distribution<int> l(0, 25);
    for (const auto& c : known) {
        std::string x = c;
        const size_t i = x.size() - 1;     // the suffix: the letters a skimmer misreads
        do x[i] = (char)('A' + l(r)); while (x == c);
        if (b.count(x) > 0) { caught++; right++; continue; }   // the change hit another known call: not a bust to find
        auto v = b.check(x);
        if (v.busted) caught++;
        if (v.likely == c) right++;
    }
    printf("busted calls: %d/200 caught, %d/200 with the right call\n", caught, right);
    // a wrong "likely" call happens when two known calls are both one character away
    CHECK(caught >= 198 && right >= 180, "busted caught %d right %d", caught, right);
    // fresh calls heard once: how many are wrongly called busted (a real call can be one letter from a known one)
    int wrong = 0;
    for (int i = 0; i < 200; i++) { auto c = randCall(r); if (b.count(c)) continue; if (b.check(c).busted) wrong++; }
    printf("busted calls: %d/200 new valid calls flagged (near a known call by chance)\n", wrong);
    CHECK(wrong <= 30, "false busts %d", wrong);
    // CW: one Morse element (I read as EE: ".." against ". .")
    BustedCalls cw;
    for (int i = 0; i < 5; i++) cw.add("DL1XI");
    auto v = cw.check("DL1XEE", true);
    CHECK(v.busted && v.likely == "DL1XI", "CW one element: %s", v.why.c_str());
    CHECK(!cw.check("DL1XEE", false).busted, "not a one-character bust without CW");
    CHECK(cw.check("CQCQ").busted, "an invalid call is flagged");
    CHECK(!cw.check("DL1XI").busted, "the known call itself is fine");
}

static void testAprs() {
    std::mt19937 r(3);
    std::normal_distribution<double> jit(0, 3);
    auto run = [&](double every, bool moving, int n) {
        AprsHealth h;
        std::vector<Insight> out;
        double la = 54.7, lo = 25.3;
        for (int i = 0; i < n; i++) {
            if (moving) lo += 0.01;
            h.add(1000 + i * every + jit(r), "LY1ABC-9", "WIDE1-1,WIDE2-1", "!pos" + std::to_string(i), true, la, lo, out);
        }
        for (const auto& i : out) if (i.kind == "aprs_overbeacon") return true;
        return false;
    };
    CHECK(run(120, false, 10), "fixed every 2 min: over-beaconing");
    CHECK(!run(1800, false, 10), "fixed every 30 min: fine");
    CHECK(!run(120, false, 4), "too few beacons to say");
    CHECK(run(30, true, 10), "moving every 30 s: over-beaconing");
    CHECK(!run(120, true, 10), "moving every 2 min: fine");
    CHECK(pathProblem("WIDE1-1,WIDE2-1").empty(), "WIDE1-1,WIDE2-1 fine");
    CHECK(!pathProblem("WIDE3-3").empty() && !pathProblem("RELAY,WIDE2-2").empty() && !pathProblem("WIDE5-4*,WIDE").empty(), "long paths");
    CHECK(!pathProblem("LY2X*,WIDE2-1").size(), "digi call is fine");
    auto d = usedDigis("LY2X,LY3Y*,WIDE2-1");
    CHECK(d.size() == 2 && d[1] == "LY3Y", "used digis");
    // flood: the same packet through 6 digipeaters
    {
        AprsHealth h;
        std::vector<Insight> out;
        for (int i = 0; i < 6; i++) h.add(100 + i * 2, "LY9ZZ", "D" + std::to_string(i) + "*,WIDE2-1", "same", false, 0, 0, out);
        int fl = 0; for (const auto& i : out) fl += i.kind == "aprs_flood";
        CHECK(fl == 1, "flood once, got %d", fl);
        AprsHealth h2; out.clear();
        for (int i = 0; i < 3; i++) h2.add(100 + i * 2, "LY9ZZ", "D" + std::to_string(i) + "*", "same", false, 0, 0, out);
        fl = 0; for (const auto& i : out) fl += i.kind == "aprs_flood";
        CHECK(fl == 0, "3 digis are no flood");
    }
    // silent digipeater
    {
        AprsHealth h;
        std::vector<Insight> out;
        double t = 0;
        for (int i = 0; i < 20; i++) { t = i * 300 + jit(r); h.add(t, "LY1X" + std::to_string(i % 3), "LY2DIG*,WIDE2-1", "x" + std::to_string(i), false, 0, 0, out); }
        out.clear();
        h.tick(t + 1000, out);
        CHECK(out.empty(), "not silent yet");
        h.tick(t + 2000, out);
        CHECK(out.size() == 1 && out[0].kind == "aprs_digi_silent" && out[0].key == "LY2DIG", "digi silent");
        out.clear(); h.tick(t + 3000, out);
        CHECK(out.empty(), "said once");
    }
}

int main() {
    testParsing();
    testWhoHearsMe();
    testPattern();
    testBusted();
    testAprs();
    return testSummary("ham_insight");
}
