// Sub-GHz sensors at home (round 4 #3, #5, #7): sensor names and the battery-change hint, the sensor.reading event from the real receiver on generated
// Nexus-TH signals (several rates, off the centre, noise, two sensors at once), the built-in threshold recipes and the per-sensor Alert rule on the engine.
#include "dect2/automation/engine.h"
#include "dect2/automation/recipes.h"
#include "dect2/sensors/sensors_gen.h"
#include "dect2/sensors/sensors_names.h"
#include "dect2/sensors/sensors_rx.h"
#include "dect2/sensors/sensors_tel.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
using namespace dect2;
namespace au = dect2::automation;
#include "../common/test_util.h"

namespace {

std::vector<cf32> sig(const PulseList& p, double rate, double shiftHz, double snrDb, uint32_t seed = 3) {
    std::vector<cf32> x = sensorsOokSignal(p, rate, 0.5, 20000, 60000);
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double sigma = snrDb >= 99 ? 0 : std::sqrt(0.5 * 0.5 / 2 / std::pow(10.0, snrDb / 10.0) * (rate / 250e3) / 2);
    double ph = 0;
    for (auto& v : x) {
        if (shiftHz != 0) { ph += 2 * math::kPi * shiftHz / rate; v *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
        if (sigma > 0) v += cf32(nd(rng) * (float)sigma, nd(rng) * (float)sigma);
    }
    return x;
}
void feedAll(SensorsReceiver& rx, const std::vector<cf32>& x) {
    for (size_t o = 0; o < x.size(); o += 8192) rx.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
    rx.flush();
}

std::mutex gMu;
std::vector<au::Event> gEvents;

double num(const au::Event& e, const char* k) { auto it = e.f.find(k); return it == e.f.end() || it->second.t != au::Value::Num ? NAN : it->second.n; }
std::string txt(const au::Event& e, const char* k) { auto it = e.f.find(k); return it == e.f.end() ? std::string() : it->second.text(); }

SensorDevice dev(const std::string& model, const std::string& id, const std::string& ch, int64_t first, int64_t last) {
    SensorDevice d;
    d.model = model; d.id = id; d.channel = ch; d.key = model + "|" + id + "|" + ch;
    d.firstSeen = first; d.lastSeen = last;
    return d;
}

void testNames() {
    SensorNames n;
    n.set("Nexus-TH|106|1", "  Garden\t", 1000);
    CHECK(n.get("Nexus-TH|106|1") == "Garden", "trimmed, control characters out: '%s'", n.get("Nexus-TH|106|1").c_str());
    n.set("Nexus-TH|106|1", std::string(100, 'x'));
    CHECK(n.get("Nexus-TH|106|1").size() == 60, "at most 60 bytes");
    n.set("Nexus-TH|106|1", "Gärten Süd");
    n.set("Acurite-Tower|12345|A", "Freezer", 2000);
    n.set("Acurite-Tower|999|B", "");
    CHECK(n.size() == 2 && !n.has("Acurite-Tower|999|B"), "an empty name removes");
    SensorNames r;
    r.parse(n.serialize() + "junk line\nno|bars\tX\t1\n\n");
    CHECK(r.size() == 2 && r.get("Nexus-TH|106|1") == "Gärten Süd" && r.all().at("Acurite-Tower|12345|A").lastSeen == 2000 && r.all().at("Acurite-Tower|12345|A").model == "Acurite-Tower",
          "round trip through the settings text, bad lines skipped");
    SensorDevice d = dev("Nexus-TH", "106", "1", 0, 0);
    CHECK(sensorLabel(d, r) == "Gärten Süd" && sensorLabel(dev("LaCrosse-TX", "7", "", 0, 0), r) == "LaCrosse-TX 7", "label");
    CHECK(r.move("Nexus-TH|106|1", "Nexus-TH|77|1", 3000) && !r.has("Nexus-TH|106|1") && r.get("Nexus-TH|77|1") == "Gärten Süd" && !r.move("nope|1|1", "x|1|1"), "move");
}

void testHints() {
    const int64_t t0 = 1'700'000'000;
    SensorNames n;
    n.set("Nexus-TH|106|1", "Garden", t0);
    SensorsTelemetry t;
    // the named one was last heard at t0; a new id on the same model and channel came 20 min later; another on channel 2; one before t0
    t.devices.push_back(dev("Nexus-TH", "77", "1", t0 + 1200, t0 + 3000));
    t.devices.push_back(dev("Nexus-TH", "55", "2", t0 + 1300, t0 + 3000));
    t.devices.push_back(dev("Nexus-TH", "44", "1", t0 - 500, t0 + 3000));
    t.devices.push_back(dev("Acurite-Tower", "1", "1", t0 + 600, t0 + 3000));
    auto h = n.hints(t, t0 + 3000);
    CHECK(h.size() == 1 && h[0].fromKey == "Nexus-TH|106|1" && h[0].toKey == "Nexus-TH|77|1" && h[0].name == "Garden", "one hint, to the same model and channel after it went quiet (%zu)", h.size());
    // still in the list and heard 60 s ago with a 60 s interval: still sending, no hint
    SensorDevice old = dev("Nexus-TH", "106", "1", t0 - 9000, t0 + 2940);
    old.health.intervalSec = 60;
    t.devices.push_back(old);
    n.touch(t);
    CHECK(n.hints(t, t0 + 3000).empty(), "no hint while the named sensor still sends");
    // quiet for 10 minutes now (3 intervals = 3 min): the new one appeared after it, so the hint comes back only for one first heard after t0+2940
    CHECK(n.hints(t, t0 + 3600).empty(), "a sensor heard before the named one went quiet is no candidate");
    t.devices.push_back(dev("Nexus-TH", "88", "1", t0 + 3100, t0 + 3500));
    h = n.hints(t, t0 + 3600);
    CHECK(h.size() == 1 && h[0].toKey == "Nexus-TH|88|1", "hint to the one first heard after the last hearing");
    // more than 6 h later: no hint
    SensorNames n2;
    n2.set("Nexus-TH|106|1", "Garden", t0);
    SensorsTelemetry t2;
    t2.devices.push_back(dev("Nexus-TH", "77", "1", t0 + 7 * 3600, t0 + 7 * 3600 + 100));
    CHECK(n2.hints(t2, t0 + 8 * 3600).empty(), "not after 6 hours");
    // two named sensors on one channel, one new sensor: offered for the closer one only
    SensorNames n3;
    n3.set("Nexus-TH|1|1", "A", t0);
    n3.set("Nexus-TH|2|1", "B", t0 + 1000);
    SensorsTelemetry t3;
    t3.devices.push_back(dev("Nexus-TH", "3", "1", t0 + 1100, t0 + 1200));
    h = n3.hints(t3, t0 + 5000);
    CHECK(h.size() == 1 && h[0].fromKey == "Nexus-TH|2|1", "the closest in time");
}

// the receiver's sensor.reading events, through the program-wide engine's tap
void testReceiverEvents() {
    au::global().setTap([](const au::Event& e) { std::lock_guard<std::mutex> lk(gMu); if (e.type == "sensor.reading") gEvents.push_back(e); }, au::familyBit(au::FSensor));
    struct Case { double rate, shift, snr, offset; const char* what; };
    const Case cases[] = {{250e3, 0, 99, 0, "250 ksps clean"}, {1e6, 0, 99, 0, "1 Msps"}, {250e3, 30e3, 99, 30e3, "30 kHz off the centre, tuned"},
                          {250e3, 4e3, 99, 0, "4 kHz carrier offset"}, {250e3, 0, 16, 0, "16 dB SNR"}, {2e6, -250e3, 18, -250e3, "2 Msps, 250 kHz off, 18 dB"}};
    for (const Case& c : cases) {
        { std::lock_guard<std::mutex> lk(gMu); gEvents.clear(); }
        SensorsReceiver rx;
        rx.setAutoLevel(true);
        rx.configure(c.rate);
        rx.setSignalOffset(c.offset);
        rx.setCenterHz(433.92e6);
        rx.setNames({{"Nexus-TH|106|1", "Freezer"}});
        int64_t clk = 1'800'000'000;
        rx.setClock([&] { return clk; });
        feedAll(rx, sig(sensorsNexusPulses(0x6a, 1, true, -14.5, 47, 12), c.rate, c.shift, c.snr));
        clk += 30;   // the next transmission, 30 s later, with a low battery
        feedAll(rx, sig(sensorsNexusPulses(0x6a, 1, false, -10.2, 48, 12), c.rate, c.shift, c.snr, 5));
        clk += 1;    // another sensor one second later
        feedAll(rx, sig(sensorsNexusPulses(0x21, 2, true, 3.0, 80, 12), c.rate, c.shift, c.snr, 9));
        std::lock_guard<std::mutex> lk(gMu);
        CHECK(gEvents.size() == 3, "%s: one event per transmission (%zu)", c.what, gEvents.size());
        if (gEvents.size() != 3) continue;
        const au::Event& a = gEvents[0];
        CHECK(txt(a, "key") == "Nexus-TH|106|1" && txt(a, "name") == "Freezer" && a.f.at("named").b && txt(a, "model") == "Nexus-TH" && txt(a, "id") == "106" && txt(a, "channel") == "1",
              "%s: names and ids (%s / %s)", c.what, txt(a, "key").c_str(), txt(a, "name").c_str());
        CHECK(std::fabs(num(a, "temperature_C") + 14.5) < 0.05 && num(a, "humidity") == 47 && num(a, "battery_ok") == 1 && !a.f.at("battery_low").b && a.mode == "Sensors" && a.freqHz == 433.92e6 &&
              a.time == 1'800'000'000.0, "%s: values of the first", c.what);
        const au::Event& b = gEvents[1];
        CHECK(std::fabs(num(b, "temperature_C") + 10.2) < 0.05 && num(b, "humidity") == 48 && b.f.at("battery_low").b, "%s: the second, battery low", c.what);
        const au::Event& d = gEvents[2];
        CHECK(txt(d, "id") == "33" && txt(d, "channel") == "2" && txt(d, "name") == "Nexus-TH 33" && !d.f.at("named").b && std::fabs(num(d, "temperature_C") - 3.0) < 0.05, "%s: an unnamed sensor (%s %s)",
              c.what, txt(d, "id").c_str(), txt(d, "name").c_str());
    }
    // nothing emitted when nobody wants the family
    au::global().setTap(nullptr, 0);
    { std::lock_guard<std::mutex> lk(gMu); gEvents.clear(); }
    SensorsReceiver rx;
    rx.setAutoLevel(true);
    rx.configure(250e3);
    feedAll(rx, sig(sensorsNexusPulses(0x6a, 1, true, 21.4, 47, 12), 250e3, 0, 99));
    SensorsTelemetry t;
    rx.telemetry(t, 0);
    std::lock_guard<std::mutex> lk(gMu);
    CHECK(gEvents.empty() && t.devices.size() == 1, "no event without a listener, the table still fills");
}

struct Sink : au::ActionSink {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> calls;
    bool perform(const std::string& k, const std::map<std::string, std::string>& p, std::string& d) override { calls.push_back({k, p}); d = "ok"; return true; }
    size_t count(const char* k) const { size_t n = 0; for (auto& c : calls) n += c.first == k; return n; }
};

au::Event reading(const std::string& key, const std::string& name, bool named, double t, std::map<std::string, double> v, bool batLow = false) {
    au::Event e("sensor.reading");
    e.at(t, "Sensors", 433.92e6);
    e.str("key", key).str("name", name).flag("named", named).flag("battery_low", batLow);
    for (auto& kv : v) e.num(kv.first, kv.second);
    return e;
}

au::Rule recipe(const char* name) {
    for (const auto& r : au::builtinRecipes()) if (r.kind == au::Recipe::KindRule && r.rule.name == name) { au::Rule x = r.rule; x.enabled = true; return x; }
    CHECK(false, "recipe '%s' missing", name);
    return au::Rule();
}

void testRules() {
    const double t0 = 1'800'000'000;
    struct R { const char* name; au::Event fire, quiet; };
    const R rs[] = {
        {"Freezer too warm", reading("k|1|1", "Chest Freezer", true, t0, {{"temperature_C", -10}}), reading("k|1|1", "Chest Freezer", true, t0, {{"temperature_C", -14.5}})},
        {"Frost", reading("k|2|1", "Garden", true, t0, {{"temperature_C", -0.5}}), reading("k|3|1", "Nexus-TH 3", false, t0, {{"temperature_C", -5}})},
        {"Water leak", reading("k|4|1", "Bathroom", true, t0, {{"detect_wet", 1}}), reading("k|4|1", "Bathroom", true, t0, {{"detect_wet", 0}})},
        {"Door or window opened", reading("k|5|1", "Back door", true, t0, {{"contact_open", 1}}), reading("k|5|1", "Back door", true, t0, {{"contact_open", 0}})},
        {"Sensor battery low", reading("k|6|1", "Garden", true, t0, {{"temperature_C", 4}}, true), reading("k|6|1", "Garden", true, t0, {{"temperature_C", 4}}, false)},
    };
    for (const R& r : rs) {
        au::Engine eng;
        Sink sink;
        double now = t0;
        eng.setSink(&sink);
        eng.setClock([&] { return now; });
        eng.setMaxActionsPerMinute(0);
        const au::Rule rule = recipe(r.name);
        CHECK(au::validateRule(rule).empty(), "%s validates: %s", r.name, au::validateRule(rule).c_str());
        eng.setRules({rule});
        eng.process(r.quiet);
        CHECK(sink.count("notify") == 0, "%s: quiet case does not fire", r.name);
        eng.process(r.fire);
        CHECK(sink.count("notify") == 1, "%s: fires (%zu)", r.name, sink.count("notify"));
        now += 10;
        eng.process(r.fire);
        CHECK(sink.count("notify") == 1, "%s: cooldown per sensor", r.name);
    }
    // the per-sensor Alert
    const au::Rule al = sensorAlertRule("Acurite-Tower|12345|A", "Greenhouse", "temperature_C", true, 30);
    CHECK(au::validateRule(al).empty() && al.enabled && al.trigger == "sensor.reading", "alert rule validates: %s", au::validateRule(al).c_str());
    au::Engine eng;
    Sink sink;
    double now = t0;
    eng.setSink(&sink);
    eng.setClock([&] { return now; });
    eng.setMaxActionsPerMinute(0);
    eng.setRules({al});
    eng.process(reading("Acurite-Tower|12345|A", "Greenhouse", true, t0, {{"temperature_C", 29.9}}));
    eng.process(reading("Acurite-Tower|999|A", "Other", true, t0, {{"temperature_C", 35}}));
    CHECK(sink.count("notify") == 0, "alert: below the limit or another sensor: nothing");
    eng.process(reading("Acurite-Tower|12345|A", "Greenhouse", true, t0, {{"temperature_C", 31.2}}));
    CHECK(sink.count("notify") == 1 && sink.count("sound") == 1, "alert fires with notify and sound");
    if (!sink.calls.empty()) CHECK(sink.calls[0].second["title"] == "Greenhouse: temperature_C 31.2", "title filled: %s", sink.calls[0].second["title"].c_str());
    now += 1700;
    eng.process(reading("Acurite-Tower|12345|A", "Greenhouse", true, now, {{"temperature_C", 32}}));
    CHECK(sink.count("notify") == 1, "quiet inside 30 min");
    now += 200;
    eng.process(reading("Acurite-Tower|12345|A", "Greenhouse", true, now, {{"temperature_C", 32}}));
    CHECK(sink.count("notify") == 2, "again after 30 min while it holds");
    const au::Rule bl = sensorAlertRule("k|1|1", "Pond", "temperature_C", false, 2);
    CHECK(au::conditionExpr(bl) == "key == 'k|1|1' and temperature_C < 2", "below: %s", au::conditionExpr(bl).c_str());
    CHECK(au::findEventInfo("sensor.reading") && au::familyOf("sensor.reading") == au::FSensor, "catalog and family");
}

}  // namespace

int main() {
    testNames();
    testHints();
    testReceiverEvents();
    testRules();
    return testSummary("sensors_home");
}
