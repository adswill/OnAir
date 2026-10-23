// ADS-B display options: altitude colours, the drop time and the pref keys (dect2/adsb/adsb_display.h).
#include "dect2/adsb/adsb_display.h"
#include "dect2/adsb/adsb_demod.h"
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstring>
using namespace dect2::adsbd;
#include "../common/test_util.h"
static bool same(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
int main() {
    const Rgb lo = altRgb(true, 0, false), mid = altRgb(true, 20000, false), hi = altRgb(true, 40000, false), gnd = altRgb(true, 0, true);
    CHECK(lo.r > lo.b && lo.r > 200, "low is warm (%d,%d,%d)", lo.r, lo.g, lo.b);
    CHECK(hi.b > hi.r - 1 && hi.b > 200 && hi.r < 200 + 30, "high is cool (%d,%d,%d)", hi.r, hi.g, hi.b);
    CHECK(!same(lo, mid) && !same(mid, hi) && !same(lo, hi), "the scale changes with height");
    CHECK(same(altRgb(true, 90000, false), altRgb(true, 39999, false)), "above 40000 ft stays at the top colour");
    CHECK(same(altRgb(true, -500, false), lo), "below sea level is the lowest colour");
    CHECK(gnd.r == gnd.g - 4 && gnd.g == gnd.b - 4, "ground is grey");
    CHECK(same(altRgb(true, 35000, true), gnd) && same(altRgb(false, 0, true), gnd), "ground wins over altitude");
    CHECK(!same(altRgb(false, 0, false), gnd), "no altitude is not grey");
    // the scale is continuous: neighbouring heights give close colours
    for (int ft = 0; ft < 39000; ft += 500) {
        const Rgb a = altRgb(true, ft, false), b = altRgb(true, ft + 500, false);
        CHECK(std::abs(a.r - b.r) < 30 && std::abs(a.g - b.g) < 30 && std::abs(a.b - b.b) < 30, "jump at %d ft", ft);
    }
    // drop time
    CHECK(!expired(1e6, 0), "0 = never drops");
    CHECK(!expired(60, 60) && expired(60.1, 60) && !expired(10, 60), "60 s");
    CHECK(clampDropSec(0) == 0 && clampDropSec(-5) == 0 && clampDropSec(1) == kMinDropSec && clampDropSec(99999) == kMaxDropSec && clampDropSec(90) == 90, "drop clamp");
    CHECK(clampTrackWidth(0) == 1 && clampTrackWidth(9) == 6 && clampTrackWidth(3) == 3, "width clamp");
    CHECK(trackerDropSec(0) > 1e8 && trackerDropSec(45) == 45, "tracker drop");
    // defaults keep the old behaviour
    Options o;
    CHECK(o.altColour && o.dropSec == 60 && o.trackWidth == 2 && o.labels, "defaults");
    CHECK(!strcmp(kPrefLabels, "adsbLabels"), "labels pref key");
    {   // #85: the demodulator clock runs on through a new start and can be moved on by the time the receiver was stopped
        dect2::AdsbDemod d;
        d.configure(2.4e6);
        std::vector<dect2::cf32> z(240000, dect2::cf32(0, 0));
        d.feed(z.data(), z.size());
        const double t1 = d.timeSec();
        d.configure(2.4e6);
        CHECK(std::fabs(d.timeSec() - t1) < 1e-3, "clock kept through configure: %.4f -> %.4f", t1, d.timeSec());
        d.advance(30);
        CHECK(std::fabs(d.timeSec() - t1 - 30) < 1e-3, "advance 30 s: %.4f", d.timeSec() - t1);
        d.configure(10e6);
        CHECK(std::fabs(d.timeSec() - t1 - 30) < 1e-3, "clock kept through a rate change: %.4f", d.timeSec() - t1);
    }
    CHECK(!strcmp(kPrefAltColour, "adsbAltColour") && !strcmp(kPrefTrackWidth, "adsbTrackWidth") && !strcmp(kPrefDropSec, "adsbDropSec"), "pref keys");
    printf(fails ? "FAILED\n" : "adsb display ok\n");
    return fails ? 1 : 0;
}
