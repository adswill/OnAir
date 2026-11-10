// ADS-B overhead log (round 4 #8): straight passes at several cross-track distances with range jitter, a gap in reception inside one pass, a long
// gap (two passes), two aircraft at once, the altitude limit, a flight that stays outside, the day of a pass and the CSV round trip.
#include "dect2/adsb/adsb_overhead.h"
#include "../common/test_util.h"
#include <cmath>
#include <random>
using namespace dect2;

namespace {
// an aircraft on a straight line at `crossKm` from the receiver, at t seconds from the closest moment, 220 kt
AdsbAircraft at(uint32_t icao, double crossKm, double t, std::mt19937& rng, double jitterKm, int alt = 3000) {
    const double along = 220 * 1.852 / 3600 * t;
    std::normal_distribution<double> nd(0, jitterKm);
    const double km = std::max(0.0, std::hypot(crossKm, along) + (jitterKm > 0 ? nd(rng) : 0));
    AdsbAircraft a;
    a.icao = icao; a.callsign = "TEST" + std::to_string(icao & 0xff); a.hasRange = true; a.distNm = (float)(km / 1.852); a.hasAlt = true; a.altFt = alt; a.ageSec = 0.5f;
    return a;
}
}  // namespace

int main() {
    std::mt19937 rng(7);
    const int64_t t0 = 1'800'000'000;
    // one pass at several cross-track distances, with and without jitter
    for (double cross : {0.2, 1.0, 2.5}) for (double jit : {0.0, 0.05}) {
        AdsbOverheadLog log;
        log.setRadiusKm(3.0);
        std::vector<AdsbOverheadPass> ended;
        for (int s = -200; s <= 200; s++) {
            const auto e = log.update({at(0x4840D6, cross, s, rng, jit)}, t0 + s, [](const AdsbAircraft&) { return AdsbOverheadInfo{"B738", "PH-BXA"}; });
            ended.insert(ended.end(), e.begin(), e.end());
            if (s == 0) CHECK(log.now().size() == 1, "now overhead at the closest moment");
        }
        CHECK(ended.size() == 1 && log.done().size() == 1 && log.now().empty(), "cross %.1f km jitter %.2f: one pass (%zu)", cross, jit, ended.size());
        if (ended.size() != 1) continue;
        const AdsbOverheadPass& p = ended[0];
        CHECK(std::fabs(p.closestKm - cross) < 0.05 + 3 * jit && std::llabs(p.closestUtc - t0) <= (jit > 0 ? 20 : 0) && p.type == "B738" && p.registration == "PH-BXA" &&
              p.callsign == "TEST214" && p.hasAlt && p.altFt == 3000, "cross %.1f: closest %.2f km at %+lld s", cross, p.closestKm, (long long)(p.closestUtc - t0));
    }
    // reception gap of 60 s inside the radius: one pass; a gap of 10 minutes: two
    {
        AdsbOverheadLog log;
        log.setRadiusKm(5);
        for (int s = 0; s < 40; s++) log.update({at(1, 0.5, s - 20, rng, 0)}, t0 + s);
        for (int s = 100; s < 120; s++) log.update({at(1, 0.5, 2, rng, 0)}, t0 + s);
        for (int s = 120; s < 400; s++) log.update({}, t0 + s);
        CHECK(log.done().size() == 1, "a 60 s gap is one pass (%zu)", log.done().size());
        for (int s = 1000; s < 1010; s++) log.update({at(1, 0.5, 0, rng, 0)}, t0 + s);
        for (int s = 1010; s < 1200; s++) log.update({}, t0 + s);
        CHECK(log.done().size() == 2, "after 10 minutes a second pass (%zu)", log.done().size());
    }
    // two aircraft at once, one high above the limit, one passing outside
    {
        AdsbOverheadLog log;
        log.setRadiusKm(3);
        log.setMaxAltFt(10000);
        for (int s = -150; s <= 150; s++)
            log.update({at(10, 0.8, s, rng, 0), at(11, 1.5, s + 30, rng, 0), at(12, 0.3, s, rng, 0, 36000), at(13, 4.0, s, rng, 0)}, t0 + s);
        for (int s = 151; s < 400; s++) log.update({}, t0 + s);
        bool a = false, b = false, c = false, d = false;
        for (const auto& p : log.done()) { a |= p.icao == 10; b |= p.icao == 11; c |= p.icao == 12; d |= p.icao == 13; }
        CHECK(log.done().size() == 2 && a && b && !c && !d, "two passes, not the high one nor the one outside (%zu)", log.done().size());
    }
    // the day and the CSV
    CHECK(adsbOverheadDay(1'800'000'000, 0) == "2027-01-15" && adsbOverheadDay(1'800'000'000 - 8 * 3600 - 1, 0) == "2027-01-14" && adsbOverheadDay(1'799'990'000, 3 * 3600) == "2027-01-15",
          "day of a pass: %s", adsbOverheadDay(1'800'000'000, 0).c_str());
    AdsbOverheadPass p;
    p.icao = 0xAE1234; p.callsign = "RCH,401"; p.registration = "05-5145"; p.type = "C17 \"Globemaster\""; p.firstUtc = t0 - 60; p.closestUtc = t0; p.lastUtc = t0 + 61; p.closestKm = 1.234; p.hasAlt = true; p.altFt = 2500;
    AdsbOverheadPass g = p; g.icao = 0x3C6444; g.ground = true; g.hasAlt = false; g.callsign = "";
    const auto back = adsbOverheadFromCsv(adsbOverheadCsv({p, g}) + "garbage\n,,,\n");
    CHECK(back.size() == 2 && back[0].icao == 0xAE1234 && back[0].callsign == "RCH,401" && back[0].type == "C17 \"Globemaster\"" && back[0].closestUtc == t0 && back[0].firstUtc == t0 - 60 &&
          back[0].lastUtc == t0 + 61 && std::fabs(back[0].closestKm - 1.23) < 0.01 && back[0].altFt == 2500 && back[1].ground && !back[1].hasAlt, "CSV round trip (%zu)", back.size());
    return testSummary("adsb_overhead");
}
