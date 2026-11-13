// The mode and manufacturer filters of the wireless M-Bus receiver (setPhyMask, setMfrFilter, the Mode and Maker choices of the Wireless M-Bus screen).
// The simulated meters of wmbus_gen (all modes of the 868 MHz plan, and mode N of the 169 MHz plan) go, with carrier errors and noise, into one receiver without a view and one per view, from the same samples:
//   - a view lists exactly the meters and the frames of the receiver without a view that belong to its modes, and no others;
//   - the meter counts of a view are the sums over its meters; every view of a mode on the air is not empty, and one of a mode that is not on the air is;
//   - noise alone gives nothing in any view.
// Simulated signals only: no over-the-air recording was available.
#include "dect2/wmbus/wmbus_gen.h"
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/wmbus/wmbus_tel.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <algorithm>
#include <set>
#include <utility>
#include <vector>
using namespace dect2;
using namespace dect2::wmbus;
#include "../common/test_util.h"

static std::vector<WmbusTelemetry> runViews(const WmbusGenConfig& c, double seconds, const std::vector<std::pair<uint32_t, std::string>>& masks) {
    WmbusGenerator g(c);
    std::vector<std::unique_ptr<WmbusLiveReceiver>> rx;
    for (const auto& m : masks) { rx.emplace_back(new WmbusLiveReceiver); rx.back()->configure(c.rate); rx.back()->setCenterHz(c.centreHz); rx.back()->setPhyMask(m.first); rx.back()->setMfrFilter(m.second); }
    std::vector<cf32> b(20000);
    for (size_t done = 0; done < (size_t)(seconds * c.rate); done += b.size()) { g.generate(b.data(), b.size()); for (auto& r : rx) r->feed(b.data(), b.size()); }
    std::vector<WmbusTelemetry> out(rx.size());
    for (size_t i = 0; i < rx.size(); i++) rx[i]->telemetry(out[i], 0);
    return out;
}
static std::set<std::string> devKeys(const WmbusTelemetry& t, uint32_t m, const std::string& mfr = "") { std::set<std::string> s; for (auto& d : t.devices) if ((!m || ((m >> d.phy) & 1u)) && (mfr.empty() || d.mfr == mfr)) s.insert(d.mfr + d.id + d.type + std::to_string(d.phy)); return s; }
static size_t logCount(const WmbusTelemetry& t, uint32_t m, const std::string& mfr = "") { size_t n = 0; for (auto& l : t.log) if ((!m || ((m >> l.phy) & 1u)) && (mfr.empty() || l.mfr == mfr)) n++; return n; }

int main() {
    struct V { const char* id; const char* name; uint32_t phyMask; double defMhz; bool plan169; const char* mfr = ""; };   // the filters the Wireless M-Bus screen offers
    static const V views[] = {
        {"wmbuss1", "Wireless M-Bus S1", 1u << (int)wmbus::Phy::S1, 868.625, false},
        {"wmbust1", "Wireless M-Bus T1", 1u << (int)wmbus::Phy::T1, 868.625, false},
        {"wmbusc1", "Wireless M-Bus C1", (1u << (int)wmbus::Phy::C1A) | (1u << (int)wmbus::Phy::C1B), 868.625, false},
        {"wmbusr2", "Wireless M-Bus R2", 1u << (int)wmbus::Phy::R2, 868.625, false},
        {"wmbusn1", "Wireless M-Bus N1 (Wize, 169 MHz)", (1u << (int)wmbus::Phy::N1) | (1u << (int)wmbus::Phy::N24), 169.4375, true, ""},
        {"wmbuskam", "Wireless M-Bus: Kamstrup", 0xFFFFFFFFu, 868.625, false, "KAM"},
        {"wmbuslgb", "Wireless M-Bus: Landis+Gyr", 0xFFFFFFFFu, 868.625, false, "LGB"},
        {"wmbusitw", "Wireless M-Bus: Itron", 0xFFFFFFFFu, 868.625, false, "ITW"},
        {"wmbussen", "Wireless M-Bus: Sensus", 0xFFFFFFFFu, 868.625, false, "SEN"},
    };
    const int nv = (int)(sizeof views / sizeof *views);

    struct Scene { const char* name; double snr, cfo; int plan; double seconds; };
    for (const Scene& sc : {Scene{"868 clean", 99, 0, 0, 8}, Scene{"868 cfo -12 kHz, 25 dB", 25, -12000, 0, 10}, Scene{"868 cfo +9 kHz, 20 dB", 20, 9000, 0, 12}, Scene{"169 clean", 99, 0, 1, 12}, Scene{"169 cfo +3 kHz, 22 dB", 22, 3000, 1, 14}}) {
        WmbusGenConfig c; c.snrDb = sc.snr; c.cfoHz = sc.cfo; c.gapSec = 0.05; c.plan = sc.plan; if (sc.plan) c.centreHz = 169.4375e6;
        std::vector<std::pair<uint32_t, std::string>> masks{{0u, ""}};
        for (int i = 0; i < nv; i++) masks.push_back({views[i].phyMask, views[i].mfr});
        const auto r = runViews(c, sc.seconds, masks);
        size_t sumDev = 0;
        for (int i = 0; i < nv; i++) {
            const WmbusTelemetry& t = r[(size_t)i + 1];
            const uint32_t m = views[i].phyMask;
            const std::string mf = views[i].mfr;
            CHECK(devKeys(t, 0) == devKeys(r[0], m, mf), "%s: view %s lists %zu meters, the receiver without a view %zu of its modes", sc.name, views[i].id, t.devices.size(), devKeys(r[0], m, mf).size());
            CHECK(t.log.size() == logCount(r[0], m, mf), "%s: view %s has %zu log lines, expected %zu", sc.name, views[i].id, t.log.size(), logCount(r[0], m, mf));
            uint64_t cnt = 0; for (auto& d : t.devices) cnt += d.count;
            CHECK(t.blocksOk == cnt, "%s: view %s counts %llu vs %llu", sc.name, views[i].id, (unsigned long long)t.blocksOk, (unsigned long long)cnt);
            const bool onAir = !devKeys(r[0], m, mf).empty();
            CHECK(t.dataValid == onAir, "%s: view %s dataValid %d, on the air %d", sc.name, views[i].id, (int)t.dataValid, (int)onAir);
            if (mf.empty()) sumDev += t.devices.size();
            printf("  %-24s %-10s %zu meters, %zu frames\n", sc.name, views[i].id, t.devices.size(), t.log.size());
        }
        CHECK(sumDev == r[0].devices.size(), "%s: the views list %zu meters, without a view %zu", sc.name, sumDev, r[0].devices.size());
        if (sc.plan == 0 && sc.snr >= 25) for (int i = 0; i < 4; i++) CHECK(!r[(size_t)i + 1].devices.empty(), "%s: view %s is empty", sc.name, views[i].id);
        if (sc.plan == 0 && sc.snr >= 25) { int kam = -1, sen = -1, lgb = -1, itw = -1; for (int i = 0; i < nv; i++) { const std::string m = views[i].mfr; const size_t n = r[(size_t)i + 1].devices.size(); if (m == "KAM") kam = (int)n; if (m == "SEN") sen = (int)n; if (m == "LGB") lgb = (int)n; if (m == "ITW") itw = (int)n; }
            CHECK(kam >= 1 && sen >= 1 && lgb == 0, "%s: Kamstrup %d Sensus %d Landis+Gyr %d (the simulated port has no Landis+Gyr meter)", sc.name, kam, sen, lgb); (void)itw; }
        if (sc.plan == 1) CHECK(!r[5].devices.empty() && r[1].devices.empty() && r[2].devices.empty(), "%s: the N1 view %zu, S1 %zu, T1 %zu", sc.name, r[5].devices.size(), r[1].devices.size(), r[2].devices.size());
    }
    {   // noise alone
        WmbusGenConfig c; c.silent = true; c.snrDb = 20;
        std::vector<std::pair<uint32_t, std::string>> masks{{0u, ""}};
        for (int i = 0; i < nv; i++) masks.push_back({views[i].phyMask, views[i].mfr});
        const auto r = runViews(c, 10, masks);
        for (size_t i = 0; i < r.size(); i++) CHECK(r[i].devices.empty() && r[i].blocksOk == 0, "noise: receiver %zu lists %zu meters", i, r[i].devices.size());
    }
    return testSummary("wmbus views");
}
