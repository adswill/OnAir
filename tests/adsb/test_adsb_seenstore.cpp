// The heard-over-time store (round 4 #11, #23, #33): unique and new identities per day over three days, a restart in the middle (save and load),
// the time zone at midnight, a late report of an earlier day, the daily and all-time farthest, the outline per sector and band, bad input.
#include "dect2/common/seen_store.h"
#include "../common/test_util.h"
#include <cmath>
#include <string>
using namespace dect2;

int main() {
    const int64_t d0 = 1'799'971'200;   // 2027-01-15 00:00 UTC
    SeenStore s;
    // day 1: A, B, C; A twice; positions
    CHECK(s.seen("A", d0 + 100, "KLM1") && s.seen("B", d0 + 200) && s.seen("C", d0 + 300) && !s.seen("A", d0 + 400), "first time ever, once");
    s.position("A", d0 + 100, 120.5, 45, 2);
    s.position("B", d0 + 200, 300.2, 359.9, 1);
    s.position("C", d0 + 300, 80, 182, 0);
    s.position("C", d0 + 310, 5001, 182, 0);   // nonsense: ignored
    s.messages(d0 + 300, 1500);
    const SeenDay* t = s.today(d0 + 500);
    CHECK(t && t->unique == 3 && t->fresh == 3 && t->positions == 3 && t->messages == 1500 && std::fabs(t->maxKm - 300.2) < 1e-6 && t->maxId == "B", "day 1 counts");
    CHECK(s.newToday("A", d0 + 500) && s.get("A")->name == "KLM1", "new today, name kept");
    // restart: save, load into a new store
    SeenStore r;
    CHECK(r.parse(s.serialize()) && r.uniqueEver() == 3 && r.days().size() == 1 && r.days()[0].unique == 3 && std::fabs(r.farthestKm() - 300.2) < 1e-3 && r.farthestId() == "B" &&
          std::fabs(r.outline(1)[35] - 300.2f) < 0.01 && std::fabs(r.outline(2)[4] - 120.5f) < 0.01 && std::fabs(r.outline(0)[18] - 80) < 0.01, "round trip");
    // day 2 after the restart: A again (not new), D new; A twice that day counts once
    CHECK(!r.seen("A", d0 + 86400 + 10) && r.seen("D", d0 + 86400 + 20) && !r.seen("A", d0 + 86400 + 30), "day 2 seen");
    const SeenDay* t2 = r.today(d0 + 86400 + 40);
    CHECK(t2 && t2->unique == 2 && t2->fresh == 1 && r.get("A")->days == 2 && !r.newToday("A", d0 + 86400 + 40) && r.newToday("D", d0 + 86400 + 40), "day 2 counts");
    // the hours, kinds and the farthest per band survive the round trip
    {
        SeenStore h;
        h.seen("V1", d0 + 3600 * 5 + 10, "ALPHA", "Cargo"); h.seen("V1", d0 + 3600 * 5 + 900); h.seen("V1", d0 + 3600 * 7); h.seen("V2", d0 + 3600 * 7 + 5, "", "Tanker");
        h.position("V1", d0 + 3600 * 7, 40, 10, 0); h.position("V2", d0 + 3600 * 7, 25, 10, 1); h.position("V1", d0 + 3600 * 7 + 9, 30, 10, 0);
        SeenStore h2;
        CHECK(h2.parse(h.serialize()) && h2.days()[0].hours[5] == 1 && h2.days()[0].hours[7] == 2 && h2.days()[0].hours[6] == 0 && h2.get("V2")->kind == "Tanker" && h2.get("V1")->kind == "Cargo" &&
              h2.farthestKm(0) == 40 && h2.farthestId(1) == "V2" && h2.farthestKm(1) == 25, "hours, kinds and per-band farthest");
    }
    // a late report for day 1 goes to day 1
    r.position("B", d0 + 1000, 10, 90, 0);
    CHECK(r.days().size() == 2 && r.days()[0].positions == 4, "late report on its own day");
    // the time zone: 23:30 UTC is the next day at UTC+2
    SeenStore z;
    z.setTzOffset(7200);
    z.seen("X", d0 + 86400 - 1800);
    CHECK(z.dayOf(d0 + 86400 - 1800) == "2027-01-16" && z.today(d0 + 86400 - 1800) && z.days().size() == 1 && z.days()[0].day == "2027-01-16", "local day %s", z.dayOf(d0 + 86400 - 1800).c_str());
    // many days: only the newest kMaxDays are kept
    SeenStore m;
    for (int d = 0; d < (int)SeenStore::kMaxDays + 20; d++) m.seen("id" + std::to_string(d % 7), d0 + (int64_t)d * 86400);
    CHECK(m.days().size() == SeenStore::kMaxDays && m.uniqueEver() == 7, "day cap %zu", m.days().size());
    // CSV
    const std::string csv = seenDaysCsv(r);
    CHECK(csv.rfind("day,unique,new,positions,messages,max_km,max_id\n2027-01-15,3,3,4,1500,300.2,B\n2027-01-16,2,1,", 0) == 0, "CSV: %s", csv.c_str());
    // not a store
    SeenStore bad;
    CHECK(!bad.parse("hello\nI\tA\t1\n") && !bad.seen("", d0), "bad file refused, empty id ignored");
    CHECK(r.parse(r.serialize() + "I\t\t1\t2\n" + "D\tshort\n" + "O\t9\t1\n") && r.uniqueEver() == 4, "bad lines skipped");
    return testSummary("adsb_seenstore");
}
