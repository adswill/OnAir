// The field insights through the insight hub (insight/field.h, hub.h): the automation events the decoders send ("adsb.aircraft",
// "ais.vessel", "msg.navtex") become Insight rows with the right area, kind and severity, and the interface feeds (FLARM, LoRaWAN,
// water meter) land in the same list. Simulated events only.
#include "dect2/insight/hub.h"
#include <cmath>
#include "../common/test_util.h"
using namespace dect2;
namespace in = dect2::insight;

static int count(const std::vector<in::Insight>& v, const char* kind) { int n = 0; for (const auto& i : v) if (i.kind == kind) n++; return n; }

int main() {
    in::Hub h;
    h.field().setHome(55.7, 21.1);
    const double t0 = 1767225600;
    // an aircraft squawking 7700 in a steep descent
    for (int i = 0; i < 30; i++) {
        automation::Event e("adsb.aircraft");
        e.at(t0 + i * 2, "ADS-B", 1090e6);
        e.str("hex", "4840D6").str("callsign", "KLM1023").num("squawk", 7700).num("alt_ft", 30000 - i * 230).num("vrate_fpm", -6900).num("heading_deg", 90).num("speed_kt", 420)
            .num("emergency", 1).flag("ground", false).num("lat", 55).num("lon", 22);
        h.onEvent(e);
    }
    // a NAVTEX warning and its cancellation
    {
        automation::Event e("msg.navtex"); e.at(t0 + 100, "NAVTEX", 518e3); e.str("source", "J").str("header", "JA12").str("text", "NAV WARN 45/26\nBALTIC SEA. BUOY OFF STATION.\n");
        h.onEvent(e);
        automation::Event c("msg.navtex"); c.at(t0 + 200, "NAVTEX", 518e3); c.str("source", "J").str("header", "JA13").str("text", "NAV WARN 46/26\nBUOY BACK.\nCANCEL 45/26.\n");
        h.onEvent(c);
    }
    // ducting: ships 150 - 250 km away
    for (int i = 0; i < 5; i++) {
        automation::Event e("ais.vessel"); e.at(t0 + 300 + i, "AIS", 162.025e6);
        e.num("mmsi", 265000000 + i).num("lat", 55.7).num("lon", 21.1 - (150 + 25 * i) / (111.32 * std::cos(55.7 * M_PI / 180))).str("class", "A");
        h.onEvent(e);
    }
    // an interface feed: a LoRaWAN device that cannot join
    for (int i = 0; i < 6; i++) { LwHealthFrame f; f.t = t0 + 400 + i * 60; f.mtype = 0; f.devEui = "A84041000000BEEF"; f.sf = 12; h.field().lorawan(f, h.pending()); }
    h.tick(t0 + 1000);
    const auto v = h.take();
    CHECK(count(v, "adsb_emergency") == 1 && count(v, "adsb_descent") == 1, "ADS-B: %d emergency, %d descent", count(v, "adsb_emergency"), count(v, "adsb_descent"));
    for (const auto& i : v) if (i.kind == "adsb_descent") CHECK(i.severity == 2 && i.area == "aviation" && i.hasPos, "descent severity %d", i.severity);
    CHECK(count(v, "navtex_in_force") == 2 && count(v, "navtex_cancelled") == 1, "NAVTEX: %d in force, %d cancelled", count(v, "navtex_in_force"), count(v, "navtex_cancelled"));
    CHECK(h.field().navtexInForce().inForce('J').size() == 1, "J has %zu in force", h.field().navtexInForce().inForce('J').size());
    CHECK(count(v, "ais_ducting") == 1, "ducting %d", count(v, "ais_ducting"));
    CHECK(count(v, "lorawan_join_storm") == 1, "join storm %d", count(v, "lorawan_join_storm"));
    for (const auto& i : v) { const auto e = in::toEvent(i); CHECK(e.type == "insight." + i.kind && e.f.count("text"), "event %s", e.type.c_str()); }
    CHECK(h.recent().size() == v.size(), "recent %zu", h.recent().size());
    return testSummary("marine_field_hub");
}
