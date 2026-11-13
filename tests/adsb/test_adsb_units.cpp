// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B display units: conversions between aviation and metric, unit names and CSV headers.
#include "dect2/adsb/adsb_units.h"
#include <cmath>
#include <cstdio>
using namespace dect2::adsbu;
#include "../common/test_util.h"
int main() {
    Units av, kmh, ms;
    kmh.sys = ms.sys = System::Metric; ms.spd = Speed::MS;
    CHECK(altOut(av, 35000) == 35000 && speedOut(av, 450) == 450 && distOut(av, 100) == 100 && vrateOut(av, -1500) == -1500, "aviation unchanged");
    CHECK(std::fabs(altOut(kmh, 35000) - 10668) < 1e-9, "35000 ft = %.3f m", altOut(kmh, 35000));
    CHECK(std::fabs(speedOut(kmh, 100) - 185.2) < 1e-9, "100 kt = %.3f km/h", speedOut(kmh, 100));
    CHECK(std::fabs(speedOut(ms, 100) - 51.4444444) < 1e-6, "100 kt = %.4f m/s", speedOut(ms, 100));
    CHECK(std::fabs(distOut(kmh, 250) - 463) < 1e-9, "250 nm = %.3f km", distOut(kmh, 250));
    CHECK(std::fabs(vrateOut(kmh, 1968.5039) - 600.0) < 1e-3, "1968.5 ft/min = %.4f m/min", vrateOut(kmh, 1968.5039));
    CHECK(std::string(altUnit(av)) == "ft" && std::string(altUnit(kmh)) == "m", "alt unit");
    CHECK(std::string(speedUnit(av)) == "kt" && std::string(speedUnit(kmh)) == "km/h" && std::string(speedUnit(ms)) == "m/s", "speed unit");
    CHECK(std::string(distUnit(av)) == "nm" && std::string(distUnit(kmh)) == "km", "dist unit");
    CHECK(std::string(vrateUnit(av)) == "ft/min" && std::string(vrateUnit(ms)) == "m/min", "vrate unit");
    CHECK(altCol(av) == "alt_ft" && altCol(ms) == "alt_m" && speedCol(av) == "speed_kt" && speedCol(kmh) == "speed_kmh" && speedCol(ms) == "speed_ms", "cols");
    CHECK(vrateCol(av) == "vrate_fpm" && vrateCol(kmh) == "vrate_mmin" && distCol(av) == "dist_nm" && distCol(kmh) == "dist_km", "cols2");
    CHECK(fmt(vrateOut(ms, -1500), vrateDecimals(ms), true) == "-457" && fmt(12.4, 0) == "12" && fmt(5, 0, true) == "+5", "fmt %s", fmt(vrateOut(ms, -1500), 0, true).c_str());
    printf(fails ? "FAILED\n" : "adsb units ok\n");
    return fails ? 1 : 0;
}
