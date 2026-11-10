// HF utility insight (docs/SMART5-hfutil-3.9.md): overdue broadcasts, learned from when a station is decoded.
// Simulated schedules only: three broadcasts a day with minutes of jitter, 10 % of them not decoded, stray decodes, days tuned elsewhere.
#include "dect2/insight/schedule_watch.h"
#include <cstdio>
#include <random>
#include <set>
#include "../common/test_util.h"

using namespace dect2::insight;

static const double kDay0 = 1767571200;   // 2026-01-05 00:00 UTC

int main() {
    const double f = 10100.8e3, other = 7880e3;
    const int slots[3] = {4 * 60 + 30, 10 * 60 + 30, 16 * 60 + 30};
    int alertsTrue = 0, alertsFalse = 0, missesJudged = 0, alertsElsewhere = 0;
    for (unsigned seed = 1; seed <= 10; seed++) {
        std::mt19937 r(seed);
        std::uniform_real_distribution<double> u(0, 1);
        std::uniform_int_distribution<int> jit(-2, 2), anyMin(0, 1439);
        ScheduleWatch w;
        std::vector<Insight> out;
        const int elsewhereDay = 9;
        for (int d = 0; d < 14; d++) {
            const bool tunedHere = d != elsewhereDay;
            std::set<int> missed;
            std::vector<int> decodes;
            for (int k = 0; k < 3; k++) {
                if (!tunedHere) continue;
                if (u(r) < 0.1) { missed.insert(k); continue; }
                decodes.push_back(slots[k] + jit(r));
            }
            if (tunedHere && u(r) < 0.7) {   // a stray decode (a test chart, a repeat), away from the broadcasts
                const int m = anyMin(r);
                bool near = false;
                for (int k = 0; k < 3; k++) near |= std::abs(m - slots[k]) < 15;
                if (!near) decodes.push_back(m);
            }
            for (int m = 0; m < 1440; m++) {
                const double t = kDay0 + d * 86400.0 + m * 60;
                for (int dm : decodes) if (dm == m) w.decoded(t + 20, "DWD", f + 30);
                out.clear();
                w.tick(t + 30, {tunedHere ? f : other}, out);
                for (const auto& i : out) {
                    if (!tunedHere) { alertsElsewhere++; continue; }
                    // which slot: the one just before
                    int which = -1;
                    for (int k = 0; k < 3; k++) if (m >= slots[k] && m <= slots[k] + 90) which = k;
                    if (which >= 0 && missed.count(which)) alertsTrue++;
                    else { alertsFalse++; printf("  false: seed %u day %d %02d:%02d %s\n", seed, d, m / 60, m % 60, i.text.c_str()); }
                }
            }
            if (d >= 5) missesJudged += (int)missed.size();
        }
        const auto sch = w.schedule("DWD", f);
        bool ok = sch.size() == 3;
        for (size_t k = 0; ok && k < 3; k++) ok = std::abs(sch[k] - slots[k]) <= 2;
        CHECK(ok, "seed %u: learned schedule has %zu slots", seed, sch.size());
    }
    printf("overdue: %d alerts for missed broadcasts (%d missed from day 5 on), %d false, %d while tuned elsewhere\n", alertsTrue, missesJudged, alertsFalse,
           alertsElsewhere);
    CHECK(alertsFalse == 0 && alertsElsewhere == 0, "false %d elsewhere %d", alertsFalse, alertsElsewhere);
    CHECK(alertsTrue >= missesJudged, "every missed broadcast from day 5 on is reported (%d of %d)", alertsTrue, missesJudged);
    return testSummary("hfdig_overdue");
}
