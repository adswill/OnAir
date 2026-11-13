// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// My meter and my tyres (meter_usage.h, tpms_watch.h): simulated register readings with gaps, repeats, a wrap and a reset; TPMS readings
// through day/night temperature swings with a slow leak on one tyre.
#include "dect2/sensors/meter_usage.h"
#include "dect2/sensors/tpms_watch.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    const int64_t day0 = 1767225600;   // 2026-01-01 00:00 UTC
    for (double loss : {0.0, 0.5}) {
        std::mt19937 r(1); std::uniform_real_distribution<double> u(0, 1);
        MeterUsage m; m.tzOffsetSec = 7200;   // Lithuania in winter
        double reg = 12345.0; double trueDay[10] = {};
        for (int64_t t = day0; t < day0 + 10 * 86400; t += 60) {   // 0.5 kWh an hour at night, 1.5 by day (local)
            const int lh = (int)(((t + 7200) / 3600) % 24);
            const double rate = (lh >= 7 && lh < 23 ? 1.5 : 0.5) / 60;
            reg += rate;
            trueDay[std::min<int64_t>(9, (t + 7200) / 86400 - (day0 + 7200) / 86400)] += rate;
            if (u(r) < loss) continue;
            m.add(t, std::round(reg * 10) / 10);   // the register shows 0.1 kWh
            if (u(r) < 0.3) m.add(t, std::round(reg * 10) / 10);   // repeats
        }
        const int64_t now = day0 + 10 * 86400 - 60;
        auto d = m.daily(now, 10);
        CHECK(d.size() == 10, "10 days");
        const double expectFull = 16 * 1.5 + 8 * 0.5;   // 28 kWh a full local day
        int ok = 0; for (size_t i = 1; i + 1 < d.size(); i++) ok += std::fabs(d[i].use - expectFull) < 0.6;
        CHECK(ok >= 7, "loss %.1f: full days at 28 kWh: %d of 8 (%.2f %.2f)", loss, ok, d[1].use, d[5].use);
        CHECK(std::fabs(m.dailyAverage(now) - expectFull) < 1.5, "average %.2f", m.dailyAverage(now));
        auto h = m.hourly(now, 24); double day = 0, night = 0; for (auto& b : h) { const int lh = (int)(((b.start + 7200) / 3600) % 24); (lh >= 7 && lh < 23 ? day : night) += b.use; }
        CHECK(std::fabs(day - 24) < 1.5 && std::fabs(night - 4) < 1.5, "hours: day %.2f night %.2f", day, night);
        CHECK(m.resets() == 0, "no reset");
    }
    {   // a 5-digit counter wraps 99990 -> 15; a replaced meter starts at 3; a gap of 6 h is spread over its hours
        MeterUsage m; m.add(day0, 99990); m.add(day0 + 600, 15);
        CHECK(std::fabs(m.today(day0 + 600) - 25) < 1e-6 && m.resets() == 0, "wrap %.2f", m.today(day0 + 600));
        m.add(day0 + 1200, 3); CHECK(m.resets() == 1 && std::fabs(m.today(day0 + 1200) - 25) < 1e-6, "reset");
        m.add(day0 + 1200 + 6 * 3600, 9);
        auto h = m.hourly(day0 + 1200 + 6 * 3600, 8); int nz = 0; for (auto& b : h) nz += b.use > 0.5 && b.use < 1.5; CHECK(nz >= 5, "spread over hours %d", nz);
        m.add(day0, 1); CHECK(m.lastValue() == 9, "old frame ignored");
        SensorDevice sd; sd.history.push_back({"energy_kWh", {{1, 10.0}, {2, 11.5}}}); double v = 0; std::string un;
        CHECK(sensorMeterRegister(sd, v, un) && v == 11.5 && un == "kWh", "register from fields");
    }

    // ---- tyres
    {
        CHECK(std::fabs(tpmsCorrectTo20C(220, 20) - 220) < 1e-9, "no change at 20 C");
        CHECK(tpmsCorrectTo20C(220, -10) > 245 && tpmsCorrectTo20C(220, 50) < 195, "gas law");
        for (double noise : {1.0, 3.0}) {
            std::mt19937 r(4); std::normal_distribution<double> g(0, 1);
            TpmsWatch w;
            const char* ids[4] = {"0A1B2C3D", "0A1B2C3E", "0A1B2C3F", "0A1B2C40"};
            for (int k = 0; k < 4; k++) w.mark(ids[k], "car", k == 2 ? "rear left" : "");
            for (int64_t t = day0; t < day0 + 6 * 86400; t += 900) {
                const double tempC = 5 + 12 * std::sin((double)(t - day0) / 86400 * 2 * M_PI);   // -7 .. 17 C
                for (int k = 0; k < 4; k++) {
                    double p20 = 230 - (k == 2 ? 5.0 * (double)(t - day0) / 86400 : 0.2 * (double)(t - day0) / 86400);   // rear left loses 5 kPa a day
                    const double gauge = (p20 + 101.325) * ((tempC + 273.15) / 293.15) - 101.325 + noise * g(r);
                    w.add(ids[k], t, gauge, tempC + 0.5 * g(r));
                }
                w.add("FFFFFFFF", t, 100, 20);   // someone else's car: never followed
            }
            const auto& T = w.tyres();
            CHECK(T.size() == 4 && !w.isMine("FFFFFFFF"), "only mine");
            const TpmsTyre& rl = T.at(ids[2]);
            CHECK(rl.leak && std::fabs(rl.kpaPerDay + 5) < 0.6 && rl.text.find("rear left: slow leak") == 0, "noise %.0f: leak %.2f '%s'", noise, rl.kpaPerDay, rl.text.c_str());
            CHECK(rl.imbalance, "rear left below the others after 6 days");
            for (int k : {0, 1, 3}) CHECK(!T.at(ids[k]).leak && !T.at(ids[k]).low && T.at(ids[k]).text.empty(), "tyre %d healthy (%.2f/day)", k, T.at(ids[k]).kpaPerDay);
        }
        TpmsWatch w; w.mark("X", "van"); w.add("X", day0, 150, 20); CHECK(w.tyres().at("X").low, "low");
    }
    return testSummary("sensors_mine");
}
