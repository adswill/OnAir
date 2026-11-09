// First-use hints and first-Start decisions (app/ui/ease_logic.h, docs/EASE-3.8.md).
#include "../../app/ui/ease_logic.h"
#include "dect2/hfdig/clock_comp.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); fails++; } }
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
using namespace ease;

int main() {
    // E3
    check(passLine("Meteor-M N2-4", "14:32", 63.2, 41 * 60, 55 * 60) == "Next pass: Meteor-M N2-4 at 14:32, 63 deg, in 41 min", "next pass line");
    check(passLine("ISS", "14:32", 40, -30, 300) == "Overhead now: ISS, peak 40 deg, sets in 5 min", "pass under way");
    check(passLine("X", "01:00", 20, 2 * 3600 + 5 * 60, 2 * 3600 + 900).find("2 h 05 min") != std::string::npos, "hours and minutes");
    check(passLine("X", "01:00", 20, 10, 600).find("1 min") != std::string::npos, "under a minute reads 1 min");
    check(passLine("X", "01:00", 20, -900, -5).find("sets in 1 min") != std::string::npos, "a pass just ending is not negative");
    // E4: the figures computed in the proposal for Vilnius (54.69 N, 25.28 E), spherical Earth: within half a degree
    struct { const char* n; double lon, el; } v[] = {{"GOES-19", -75.2, -14.5}, {"GK-2A", 128.2, -15.8}, {"FY-4A", 104.7, -2.6}, {"FY-2H", 79.0, 11.5}, {"Alphasat", 25.0, 27.6}, {"QO-100", 25.9, 27.6}};
    for (auto& s : v) { GeoView g = geoView(54.69, 25.28, {s.n, s.lon}); check(near(g.elDeg, s.el, 0.6), s.n); check(g.above == (s.el > 0), "above matches the sign"); }
    { GeoView g = geoView(54.69, 25.28, {"QO-100", 25.9}); check(near(g.azDeg, 180, 2), "QO-100 is due south of Vilnius"); check(near(g.skewDeg, 0, 1), "no skew due south"); check(!strcmp(compassName(g.azDeg), "south"), "compass"); }
    { GeoView e = geoView(50, 0, {"E", 30}), w = geoView(50, 0, {"W", -30}); check(e.skewDeg < 0 && w.skewDeg > 0 && near(e.skewDeg, -w.skewDeg, 1e-9), "skew sign flips east to west"); check(near(e.skewDeg, -std::atan(std::sin(30 * M_PI / 180) / std::tan(50 * M_PI / 180)) * 180 / M_PI, 1e-9), "skew formula"); }
    { GeoView g = geoView(0.1, 0, {"E", 30}); check(near(g.skewDeg, 90, 1e-9), "equator: no divide by zero"); }
    { GeoView a = geoView(40, 179, {"S", -179}); check(a.above, "date line: 2 deg apart, visible"); }
    for (double lat : {-80.0, -35.0, 0.0, 35.0, 80.0}) for (double lon : {-170.0, -45.0, 0.0, 90.0, 170.0}) {
        GeoView g = geoView(lat, lon, {"s", 19.2}); check(g.azDeg >= 0 && g.azDeg < 360 && g.elDeg >= -90 && g.elDeg <= 90, "angles in range"); (void)compassName(g.azDeg); check(!std::isnan(g.skewDeg), "skew defined");
    }
    check(geoSatsOf(60).size() == 2 && geoSatsOf(59).empty(), "geostationary modes only");
    check(geoLine(geoView(54.69, 25.28, {"GK-2A", 128.2}), false).find("below your horizon") != std::string::npos, "below the horizon says so");
    check(geoLine(geoView(54.69, 25.28, {"QO-100", 25.9}), true).find("LNB skew") != std::string::npos, "Ku satellites give a skew");
    { std::vector<GeoView> vs = {geoView(54.69, 25.28, {"low", -75.2}), geoView(54.69, 25.28, {"high", 25.9})}; check(geoBest(vs) == 1, "highest wins"); check(geoBest({}) == -1, "empty"); }
    // E5, E6, E7
    { FirstIn i; i.realRadio = i.emptyList = i.atDefaultFreq = true; check(firstScanWanted(i), "fresh start scans");
      FirstIn d = i; d.done = true; check(!firstScanWanted(d), "once only");
      FirstIn u = i; u.atDefaultFreq = false; check(!firstScanWanted(u), "a chosen frequency is left alone");
      FirstIn l = i; l.emptyList = false; check(!firstScanWanted(l), "an existing list is left alone");
      FirstIn r = i; r.realRadio = false; check(!firstScanWanted(r), "not for files or the test signal");
      FirstIn re = i; re.recording = true; check(!firstScanWanted(re), "not while recording");
      FirstIn t = i; t.transmitting = true; check(!firstScanWanted(t), "not while transmitting");
      FirstIn b2 = i; b2.busy = true; check(!firstScanWanted(b2), "not over another scan"); }
    { std::vector<SvcPick> v = {{7, 5, false}, {9, 1, true}, {3, 2, false}, {4, 0, false}};
      check(pickFirstService(v) == 2, "lowest LCN television service, radio skipped");
      check(pickFirstService({{4, 0, false}, {1, 0, false}}) == 1, "no LCN: lowest id");
      check(pickFirstService({{9, 1, true}, {4, 0, false}}) == 1, "television without number before radio");
      check(pickFirstService({{9, 1, true}}) == 0, "radio when nothing else");
      check(pickFirstService({}) == -1, "empty");
      check(pickFirstService({{-1, 1, false}}) == -1, "invalid id"); }
    check(strongestIdx({1.f, 9.f, 3.f}) == 1 && strongestIdx({}) == -1, "strongest");
    // E8: a steady DT moves the clock, a lone odd decode does not, silence asks for one wider search
    { dect2::ClockComp c; check(c.slotEnded(5, 1.3) == 0, "first slot only measures"); double s = c.slotEnded(6, 1.2); check(near(s, 1.2, 1e-6) && near(c.total(), -1.2, 1e-6), "steady: applied"); check(c.slotEnded(5, 0.1) == 0, "small DT is left alone");
      dect2::ClockComp d; d.slotEnded(5, -1.0); check(near(d.slotEnded(5, -0.9), -0.9, 1e-6) && near(d.total(), 0.9, 1e-6), "negative direction");
      dect2::ClockComp e; e.slotEnded(5, 1.0); check(e.slotEnded(5, -1.0) == 0, "opposite signs are not steady");
      dect2::ClockComp f; f.slotEnded(1, 0.7); check(f.slotEnded(1, 0.7) == 0, "one station with a modest DT never moves the clock");
      dect2::ClockComp g; g.slotEnded(1, 2.5); check(near(g.slotEnded(1, 2.5), 2.5, 1e-6), "a large DT counts even on one decode");
      dect2::ClockComp w; for (int i = 0; i < 3; i++) { w.slotEnded(0, 0); check(!w.wantWide(), "not yet"); } w.slotEnded(0, 0); check(w.wantWide(), "four silent slots ask for the search"); w.slotEnded(0, 0); check(!w.wantWide(), "once only"); w.searchFound(3.5); check(near(w.total(), -3.5, 1e-9), "search result counted");
      dect2::ClockComp x; x.slotEnded(0, 0); x.slotEnded(3, 0.1); x.slotEnded(0, 0); x.slotEnded(0, 0); x.slotEnded(0, 0); check(!x.wantWide(), "a decode resets the silence"); }
    // the sweep: a wrong clock of any size up to 5 s ends within 0.5 s of right (signals reach the slot clock as DT = error until corrected)
    for (double err = -5; err <= 5; err += 0.25) { dect2::ClockComp c; double e = err; for (int i = 0; i < 12; i++) { const double s = c.slotEnded(4, e); e -= s; } check(std::fabs(e) < 0.5, "clock error sweep converges"); }
    if (fails == 0) std::printf("ease: all passed\n");
    return fails != 0;
}
