// Leak check for the user's own water meter (wmbus_leak.h): ten simulated days of a household meter (1 L register steps, a reading
// every 2 minutes with lost frames, showers, toilets and a dishwasher by day, nothing at night), then a 3 L/h leak from day 6; a meter
// with long reception gaps; a replaced meter; the time-zone offset; save / load. Simulated only: no real meter log was used.
#include "dect2/wmbus/wmbus_leak.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

static int kinds(const std::vector<WaterLeakCheck::Finding>& v, const char* k, int* maxSev = nullptr) {
    int n = 0; for (const auto& f : v) if (f.kind == k) { n++; if (maxSev) *maxSev = std::max(*maxSev, f.severity); } return n;
}

int main() {
    CHECK([] { double l; return WaterLeakCheck::litresFrom(123.456, "123.456 m3", l) && std::fabs(l - 123456) < 1e-6; }(), "m3");
    CHECK([] { double l; return WaterLeakCheck::litresFrom(80, "80 L", l) && l == 80; }(), "L");
    CHECK([] { double l; return !WaterLeakCheck::litresFrom(5, "5 kWh", l); }(), "kWh is no volume");
    const int tz = 7200;   // UTC+2
    for (unsigned seed = 1; seed <= 3; seed++) {
        std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 1);
        WaterLeakCheck w; w.tzOffsetSec = tz;
        std::vector<WaterLeakCheck::Finding> before, after;
        const int64_t t0 = 1767225600 - tz;   // local midnight
        const int64_t leakFrom = t0 + 6 * 86400 + 14 * 3600;
        double vol = 482113.0;   // litres
        std::string saved;
        for (int64_t t = t0; t < t0 + 10 * 86400; t += 120) {
            const double localH = std::fmod((double)(t + tz) / 3600.0, 24.0);
            // daytime use: a toilet flush (6 L) or a tap now and then, a shower in the morning and evening
            if (localH >= 6.5 && localH < 23) {
                if (u(r) < 0.04) vol += 6;
                if (u(r) < 0.02) vol += 2 + 5 * u(r);
                if ((std::fabs(localH - 7) < 0.02 || std::fabs(localH - 21.5) < 0.02)) vol += 60;
            }
            if (t >= leakFrom) vol += 3.0 * 120 / 3600;   // 3 L/h
            if (u(r) < 0.15) continue;   // lost frame
            const double reg = std::floor(vol) / 1000.0;   // m3 with three decimals
            double litres; WaterLeakCheck::litresFrom(reg, "x m3", litres);
            auto& out = t < leakFrom ? before : after;
            w.add(t, litres, out);
            if (t % 600 == 0) w.tick(t, out);
            if (t == t0 + 5 * 86400) saved = w.save();
        }
        CHECK(before.empty(), "seed %u: %zu findings before the leak (%s)", seed, before.size(), before.empty() ? "" : before[0].text.c_str());
        int sev = -1;
        CHECK(kinds(after, "leak_night", &sev) >= 2 && sev == 1, "seed %u: night findings %d, severity %d", seed, kinds(after, "leak_night"), sev);
        sev = -1;
        CHECK(kinds(after, "leak_continuous", &sev) >= 2 && sev == 2, "seed %u: continuous findings %d, severity %d", seed, kinds(after, "leak_continuous"), sev);
        // the night flow measured: about 0 before, about 3 L/h after
        int okBefore = 0, okAfter = 0;
        for (const auto& [d, f] : w.nights()) {
            const int64_t nightEnd = d * 86400 + 5 * 3600 - tz;
            if (nightEnd < leakFrom) okBefore += f < 0.6; else okAfter += std::fabs(f - 3) < 0.8;
        }
        CHECK(okBefore >= 5 && okAfter >= 3, "seed %u: night flows %d / %d right", seed, okBefore, okAfter);
        // the first night finding comes on the first night of the leak
        for (const auto& f : after) if (f.kind == "leak_night") { CHECK(std::fabs(f.value - 3) < 0.8, "seed %u: night flow %.2f", seed, f.value); break; }
        // save / load: the copy saved on day 5 knows the baseline
        WaterLeakCheck g; g.tzOffsetSec = tz;
        CHECK(g.load(saved) && g.nights().size() >= 4 && g.readings() > 1500, "seed %u: load %zu nights %zu readings", seed, g.nights().size(), g.readings());
    }
    // a meter heard only now and then (gaps of 5 h) and with a steady flow: no continuous-flow alarm without the coverage
    {
        WaterLeakCheck w; std::vector<WaterLeakCheck::Finding> out;
        double v = 1000;
        for (int64_t t = 0; t < 4 * 86400; t += 120) { v += 0.2; if ((t / 3600) % 6 < 1) w.add(1767225600 + t, std::floor(v), out); }
        CHECK(kinds(out, "leak_continuous") == 0, "gappy meter: %d continuous findings", kinds(out, "leak_continuous"));
    }
    // a replaced meter (the register starts again near 0): a reset, no finding
    {
        WaterLeakCheck w; std::vector<WaterLeakCheck::Finding> out;
        for (int i = 0; i < 100; i++) w.add(1767225600 + i * 120, 5000 + i / 2, out);
        for (int i = 0; i < 100; i++) w.add(1767225600 + 12000 + i * 120, 3, out);
        CHECK(w.resets() == 1 && out.empty(), "replaced meter: %d resets, %zu findings", w.resets(), out.size());
    }
    return testSummary("wmbus_leak");
}
