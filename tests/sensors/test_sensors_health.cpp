// Sensor health: interval, silence, outliers, id collision, falling battery. Simulated reading series.
#include <cmath>
#include "dect2/sensors/sensors_health.h"
#include <random>
#include "../common/test_util.h"
using namespace dect2;

static void feed(SensorDevice& d, int64_t t, const char* key, double v) {
    SensorSeries* s = nullptr; for (auto& e : d.history) if (e.key == key) s = &e;
    if (!s) { d.history.push_back(SensorSeries()); s = &d.history.back(); s->key = key; }
    s->pts.push_back({t, v}); d.lastSeen = t;
    sensorHealthMessage(d.health, d, t);
}

int main() {
    std::mt19937 r(2); std::normal_distribution<double> g(0, 0.15);
    {   // a sensor every 60 s, repeated three times within a second; then quiet
        SensorDevice d;
        for (int i = 0; i < 30; i++) { const int64_t t = 1000 + i * 60; for (int k = 0; k < 3; k++) feed(d, t + (k ? 1 : 0), "temperature_C", 21 + g(r)); }
        CHECK(std::fabs(d.health.intervalSec - 60) < 2, "interval %.1f", d.health.intervalSec);
        CHECK(!d.health.outlier && !d.health.collision, "healthy");
        sensorHealthTick(d.health, 1000 + 29 * 60 + 200); CHECK(!d.health.silent, "not yet silent");
        sensorHealthTick(d.health, 1000 + 29 * 60 + 400); CHECK(d.health.silent, "silent");
    }
    {   SensorDevice d; for (int i = 0; i < 30; i++) feed(d, i * 60, "temperature_C", 21 + g(r));
        feed(d, 30 * 60, "temperature_C", 45);
        CHECK(d.health.outlier && d.health.detail == "temperature_C", "outlier"); }
    {   SensorDevice d; for (int i = 0; i < 30; i++) feed(d, i * 60, "temperature_C", (i % 2 ? 5 : 22) + g(r));
        CHECK(d.health.collision, "two levels"); }
    {   SensorDevice d; for (int i = 0; i < 40; i++) feed(d, i * 3600, "humidity", 40 + i * 0.5 + g(r)); CHECK(!d.health.outlier && !d.health.collision, "a slow ramp is fine"); }
    {   SensorDevice d; for (int i = 0; i < 36; i++) feed(d, i * 3600, "battery_V", 3.0 - i * 0.01); CHECK(d.health.batteryFalling, "battery"); }
    {   SensorDevice d; for (int i = 0; i < 36; i++) feed(d, i * 3600, "battery_V", 3.0 + g(r) * 0.02); CHECK(!d.health.batteryFalling, "steady battery"); }
    {   SensorDevice d; feed(d, 0, "rain_mm", 1); sensorHealthTick(d.health, 100000); CHECK(!d.health.silent, "no interval yet"); }
    return testSummary("sensors_health");
}
