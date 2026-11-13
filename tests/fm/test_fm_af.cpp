// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AF follow: the strongest AF with the same PI is taken; a regional variant (another PI) never is; a probe that is not better returns.
#include "dect2/fm/fm_af.h"
#include <cstdio>
#include <map>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    std::map<double, float> levels = {{95.1, -30}, {102.3, -50}};
    auto lv = [&](double m, float& d) { auto i = levels.find(m); if (i == levels.end()) return false; d = i->second; return true; };
    {   // manual: the strongest in span, confirmed by the PI
        FmAfFollow f;
        auto c = f.update(1, 100.0, 0x2201, 40, -45, {95.1, 102.3}, lv);
        CHECK(c.type == FmAfCommand::None, "idle");
        f.requestBest();
        c = f.update(2, 100.0, 0x2201, 40, -45, {95.1, 102.3}, lv);
        CHECK(c.type == FmAfCommand::Tune && c.mhz == 95.1, "strongest AF");
        c = f.update(3, 95.1, 0x2201, 40, -30, {100.0}, lv);
        CHECK(c.confirmed, "PI proved");
    }
    {   // a regional variant: PI differs, go back, and do not try it again
        FmAfFollow f;
        f.requestBest();
        auto c = f.update(1, 100.0, 0x2201, 40, -45, {95.1}, lv);
        CHECK(c.type == FmAfCommand::Tune, "tune");
        c = f.update(2, 95.1, 0x2301, 40, -30, {95.1}, lv);
        CHECK(c.type == FmAfCommand::Back && c.mhz == 100.0, "wrong PI goes back");
        f.requestBest();
        c = f.update(30, 100.0, 0x2201, 40, -45, {95.1}, lv);
        CHECK(c.type == FmAfCommand::None, "a rejected AF is not tried again (type %d)", c.type);
    }
    {   // auto: only when weak, with a margin
        FmAfFollow f; FmAfConfig cfg; cfg.autoFollow = true; f.setConfig(cfg);
        auto c = f.update(1, 100.0, 0x2201, 40, -45, {95.1}, lv);
        CHECK(c.type == FmAfCommand::None, "strong: stays");
        c = f.update(2, 100.0, 0x2201, 15, -28, {95.1}, lv);
        CHECK(c.type == FmAfCommand::None, "AF not 6 dB better: stays");
        c = f.update(3, 100.0, 0x2201, 15, -45, {95.1}, lv);
        CHECK(c.type == FmAfCommand::Tune, "weak and a better AF: goes");
    }
    {   // no PI after the retune: back
        FmAfFollow f; f.requestBest();
        f.update(1, 100.0, 0x2201, 40, -45, {95.1}, lv);
        auto c = f.update(5, 95.1, 0, 10, -30, {}, lv);
        CHECK(c.type == FmAfCommand::Back, "timeout goes back");
    }
    {   // a probe outside the capture: accepted only when better
        FmAfFollow f; FmAfConfig cfg; cfg.autoFollow = true; f.setConfig(cfg);
        auto c = f.update(100, 100.0, 0x2201, 15, -45, {88.8}, lv);
        CHECK(c.type == FmAfCommand::Tune && c.mhz == 88.8, "probe");
        c = f.update(101, 88.8, 0x2201, 16, -44, {}, lv);
        CHECK(c.type == FmAfCommand::None && !c.confirmed, "not 3 dB better yet");
        c = f.update(104, 88.8, 0x2201, 16, -44, {}, lv);
        CHECK(c.type == FmAfCommand::Back, "probe not better: back");
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
