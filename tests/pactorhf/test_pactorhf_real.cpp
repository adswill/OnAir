// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PACTOR-II on the owner's recording (13:52, 8532.8 kHz USB, 01.11.2026; 26 s at 8 kHz, 8 bit): two D-PSK carriers 200 Hz apart at 100 baud and a frame
// that repeats every 362 symbols (3.62 s). The symbols themselves are too noisy for exact checks (the channel is poor); what is measured is the structure.
#include "dect2/pactorhf/pactorhf_dsp.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const std::string path = std::string(DECT2_SOURCE_DIR) + "/tests/data/pactorhf/owner_pactor2_8k_s8.raw";
    std::ifstream f(path, std::ios::binary);
    CHECK(f.good(), "cannot open %s", path.c_str());
    if (!f.good()) return testSummary("pactorhf real");
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<float> x(raw.size());
    for (size_t i = 0; i < raw.size(); i++) x[i] = (float)(int8_t)raw[i] / 100.f;
    CHECK(x.size() == 208000, "%zu samples", x.size());
    for (int variant : {0, 2}) {
        const pactorhf::Analysis a = pactorhf::analyse(x.data(), x.size(), variant);
        CHECK(a.found && a.variant == 2 && a.nCarriers == 2, "variant %d: found %d as %d with %d carriers", variant, a.found, a.variant, a.nCarriers);
        CHECK(std::fabs(a.spacingHz - 200) < 3, "spacing %.1f Hz", a.spacingHz);
        CHECK(a.centreHz > 850 && a.centreHz < 910, "centre %.1f Hz", a.centreHz);
        CHECK(std::fabs(a.baud - 100) < 0.1, "baud %.3f", a.baud);
        CHECK(a.frame.found && a.frame.period == 362, "frame period %d symbols", a.frame.period);
        printf("  carriers %.1f / %.1f Hz, %.3f baud, frame %d symbols = %.3f s, %d copies\n", a.car.size() > 1 ? a.car[0].hz : 0, a.car.size() > 1 ? a.car[1].hz : 0, a.baud, a.frame.period, a.frame.periodSec, a.frame.copies);
    }
    CHECK(!pactorhf::analyse(x.data(), x.size(), 4).found, "PACTOR-IV mode must not claim this recording");
    return testSummary("pactorhf real");
}
