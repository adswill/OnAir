// ADS-B filter and search (round 4 #4): text over address, callsign, registration, type and operator; altitude band; squawks with prefixes;
// categories; ground / airborne; military blocks; every combination of the parts on a mixed traffic set; the settings text round trip.
#include "dect2/adsb/adsb_filter.h"
#include "../common/test_util.h"
#include <string>
#include <vector>
using namespace dect2;

namespace {
struct Ac { AdsbAircraft a; AdsbFilterInfo in; };
Ac mk(uint32_t icao, const char* cs, int alt, bool hasAlt, int sq, const char* cat, bool ground, const char* reg, const char* type, const char* op) {
    Ac x;
    x.a.icao = icao; x.a.callsign = cs; x.a.altFt = alt; x.a.hasAlt = hasAlt; x.a.hasSquawk = sq >= 0; x.a.squawk = sq < 0 ? 0 : sq; x.a.category = cat; x.a.ground = ground;
    x.in.registration = reg; x.in.type = type; x.in.operatorName = op;
    return x;
}
int count(const AdsbFilter& f, const std::vector<Ac>& v) { int n = 0; for (const Ac& x : v) n += adsbFilterMatch(f, x.a, x.in); return n; }
}  // namespace

int main() {
    const std::vector<Ac> v = {
        mk(0x4840D6, "KLM1023", 36000, true, 2341, "A3", false, "PH-BXA", "B738", "KLM"),
        mk(0xAE1234, "RCH401", 28000, true, 7700, "A5", false, "", "C17", "US Air Force"),
        mk(0x43C5A1, "ASCOT12", 0, false, -1, "A5", false, "ZZ336", "A332", "Royal Air Force"),
        mk(0x3C6444, "DLH4AB", 0, false, 1000, "A3", true, "D-AIZZ", "A320", "Lufthansa"),
        mk(0x4B1801, "", 1500, true, 7000, "B1", false, "HB-1234", "GLID", ""),
        mk(0x0D0001, "N123AB", 4500, true, 7500, "", false, "", "", ""),
    };
    AdsbFilter f;
    CHECK(!f.active() && count(f, v) == 6, "empty filter: everything");
    f.text = "klm"; CHECK(count(f, v) == 1, "callsign / operator, any case");
    f.text = "ae12"; CHECK(count(f, v) == 1, "hex part");
    f.text = "ph-bx"; CHECK(count(f, v) == 1, "registration");
    f.text = "a3"; CHECK(count(f, v) == 2, "type A332 and A320 (%d)", count(f, v));
    f.text = "air force"; CHECK(count(f, v) == 2, "two words, both needed");
    f.text = "air lufthansa"; CHECK(count(f, v) == 0, "words in different aircraft do not match");
    f = AdsbFilter(); f.altMinFt = 10000; CHECK(count(f, v) == 2, "above 10000 ft; no altitude fails");
    f = AdsbFilter(); f.altMaxFt = 5000; CHECK(count(f, v) == 3, "up to 5000 ft, the ground one counts as 0 (%d)", count(f, v));
    f = AdsbFilter(); f.altMinFt = 1000; f.altMaxFt = 5000; CHECK(count(f, v) == 2, "a band");
    f = AdsbFilter(); f.squawks = "7700"; CHECK(count(f, v) == 1, "one squawk");
    f.squawks = "7500, 7600,7700"; CHECK(count(f, v) == 2, "a list");
    f.squawks = "7*"; CHECK(count(f, v) == 3, "a prefix");
    f.squawks = "1000"; CHECK(count(f, v) == 1, "a leading-zero free squawk");
    f = AdsbFilter(); f.categories = "A5"; CHECK(count(f, v) == 2, "category");
    f.categories = "b"; CHECK(count(f, v) == 1, "a letter, any case");
    f.categories = "A"; CHECK(count(f, v) == 4, "letter A, empty category fails");
    f = AdsbFilter(); f.ground = 1; CHECK(count(f, v) == 1, "ground only");
    f.ground = 2; CHECK(count(f, v) == 5, "airborne only");
    f = AdsbFilter(); f.militaryOnly = true; CHECK(count(f, v) == 2, "military blocks");
    CHECK(adsbMilitaryAddress(0xAE0000) && adsbMilitaryAddress(0xAFFFFF) && !adsbMilitaryAddress(0xADFFFF) && adsbMilitaryAddress(0x43C000) && adsbMilitaryAddress(0x43CFFF) &&
          !adsbMilitaryAddress(0x43D000) && adsbMilitaryAddress(0x1AE0001), "block edges, the non-ICAO flag bit ignored");
    // every combination of the parts: the result is exactly the intersection of the parts alone
    std::vector<AdsbFilter> parts(6);
    parts[0].text = "a"; parts[1].altMinFt = 1000; parts[2].squawks = "7*"; parts[3].categories = "A"; parts[4].ground = 2; parts[5].militaryOnly = true;
    int bad = 0;
    for (int m = 0; m < 64; m++) {
        AdsbFilter c;
        if (m & 1) c.text = parts[0].text;
        if (m & 2) c.altMinFt = parts[1].altMinFt;
        if (m & 4) c.squawks = parts[2].squawks;
        if (m & 8) c.categories = parts[3].categories;
        if (m & 16) c.ground = parts[4].ground;
        if (m & 32) c.militaryOnly = true;
        for (const Ac& x : v) {
            bool want = true;
            for (int k = 0; k < 6; k++) if (m >> k & 1) want = want && adsbFilterMatch(parts[k], x.a, x.in);
            if (adsbFilterMatch(c, x.a, x.in) != want) bad++;
        }
    }
    CHECK(bad == 0, "64 combinations agree with the parts (%d differ)", bad);
    // settings text
    AdsbFilter s; s.text = "klm;=x"; s.altMinFt = 1000; s.altMaxFt = 30000; s.squawks = "7700,75*"; s.categories = "A3"; s.ground = 2; s.militaryOnly = true;
    const AdsbFilter r = adsbFilterFromText(adsbFilterToText(s));
    CHECK(r.text == "klm  x" && r.altMinFt == 1000 && r.altMaxFt == 30000 && r.squawks == "7700,75*" && r.categories == "A3" && r.ground == 2 && r.militaryOnly, "round trip");
    const AdsbFilter z = adsbFilterFromText("junk;ground=9;min=-50;mil=0");
    CHECK(!z.active(), "bad values fall back");
    return testSummary("adsb_filter");
}
