// Long sensor history (round 4 #26): daily min/max/mean over three days of readings every 60 s (a sine over the day), rain per day from the
// counter across a reset and midnight, month and year totals, the local day, save and load in the middle, two sensors.
#include "dect2/sensors/sensors_archive.h"
#include "../common/test_util.h"
#include <cmath>
using namespace dect2;

int main() {
    const int64_t d0 = 1'799'971'200;   // 2027-01-15 00:00 UTC
    SensorArchive a;
    double rain = 100;
    for (int64_t t = 0; t < 3 * 86400; t += 60) {
        const double temp = 5 + 3 * std::sin(2 * 3.14159265 * (double)t / 86400);
        if (t % 3600 == 0) rain += 0.5;            // 12 mm a day
        if (t == 86400 + 43200) rain = 0.3;        // a new battery at noon of day 2: the counter starts again
        a.add("Nexus-TH|1|1", d0 + t, {{"temperature_C", temp}, {"humidity", 60}, {"rain_mm", rain}, {"id", 1}});
        if (t == 86400) { SensorArchive b; b.parse(a.serialize()); a = b; }   // a restart at midnight
    }
    a.add("Other|2|1", d0 + 10, {{"temperature_C", -20}});
    const auto* d = a.days("Nexus-TH|1|1");
    CHECK(d && d->size() == 3, "three days");
    if (d && d->size() == 3) {
        const auto& t0 = (*d)[0].values.at("temperature_C");
        CHECK(std::fabs(t0.min - 2) < 0.01 && std::fabs(t0.max - 8) < 0.01 && std::fabs(t0.mean() - 5) < 0.01 && t0.n == 1440, "day 1 min %.2f max %.2f mean %.2f n %u", t0.min, t0.max, t0.mean(), t0.n);
        CHECK(!(*d)[0].values.count("id") && !(*d)[0].values.count("rain_mm"), "ids and counters are not values");
        CHECK(std::fabs((*d)[0].rainMm - 11.5) < 1e-6 && std::fabs((*d)[1].rainMm - 11.5) < 1e-6 && std::fabs((*d)[2].rainMm - 12) < 1e-6, "rain per day %.1f %.1f %.1f", (*d)[0].rainMm, (*d)[1].rainMm, (*d)[2].rainMm);
    }
    CHECK(std::fabs(a.rainTotal("Nexus-TH|1|1", "2027-01") - 35) < 1e-6 && std::fabs(a.rainTotal("Nexus-TH|1|1", "2027") - 35) < 1e-6 && a.rainTotal("Nexus-TH|1|1", "2026") == 0, "month and year totals");
    CHECK(a.keys().size() == 2 && a.days("Other|2|1")->size() == 1 && !a.days("nope"), "two sensors");
    SensorArchive z; z.setTzOffset(3600 * 3);
    z.add("k", d0 - 1800, {{"temperature_C", 1}});
    CHECK(z.days("k") && (*z.days("k"))[0].day == "2027-01-15", "local day at UTC+3");
    SensorArchive r; r.parse(a.serialize() + "junk\nD\tbad\n");
    CHECK(r.days("Nexus-TH|1|1") && r.days("Nexus-TH|1|1")->size() == 3 && std::fabs((*r.days("Nexus-TH|1|1"))[2].values.at("temperature_C").max - (*d)[2].values.at("temperature_C").max) < 1e-3, "round trip");
    return testSummary("sensors_archive");
}
