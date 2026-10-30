// The BLE tracker detector on the test signal: each kind of tracker is found, the phone is not, an alert is raised for the ones that stay,
// reaches the automation engine as "ble.tracker" and matches a watchlist of the Words kind.
#include "dect2/automation/engine.h"
#include "dect2/ble/ble_gen.h"
#include "dect2/bletrk/bletrk_gen.h"
#include "dect2/bletrk/bletrk_live.h"
#include "dect2/bletrk/bletrk_proto.h"
#include "dect2/bletrk/bletrk_tel.h"
#include "dect2/watchlist/watchlist.h"
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    std::mutex mu;
    std::vector<automation::Event> seen;
    automation::global().setTap([&](const automation::Event& e) { std::lock_guard<std::mutex> lk(mu); seen.push_back(e); }, automation::familyBit(automation::FBle));

    BleGenConfig c;
    c.snrDb = 30;
    c.pkts = bletrkTestSchedule(false, 0);
    BleGenerator g(c);
    BletrkLiveReceiver r;
    r.configure(c.rate);
    r.setCenterHz(0);
    r.setAlertSeconds(1.5);
    r.setNearDbfs(-100);          // everything counts as near
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(4.0 * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    BletrkTelemetry t;
    CHECK(r.telemetry(t, 0), "telemetry");
    printf("  %s, %zu trackers, %llu tracker packets, %zu alerts\n", bletrkSummary(t).c_str(), t.trackers.size(), (unsigned long long)t.trackerPackets, t.alerts.size());
    int kinds[bletrk::kKinds] = {};
    for (const auto& d : t.trackers) kinds[d.kind]++;
    for (int k = 1; k < bletrk::kKinds; k++) CHECK(kinds[k] == 1, "kind %s: %d", bletrk::kindName(k), kinds[k]);
    CHECK(t.trackers.size() == 5, "%zu trackers (the phone must not be one)", t.trackers.size());
    CHECK(t.followingNow == 5 && t.alerts.size() == 5, "following %d, alerts %zu", t.followingNow, t.alerts.size());
    bool fmSep = false;
    for (const auto& d : t.trackers) if (d.kind == bletrk::FindMy && d.separated && d.status == 0x10) fmSep = true;
    CHECK(fmSep, "Find My separated state");
    CHECK(t.trackers.size() && t.trackers[0].label.size() < 24, "labels hold no full address");
    {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        bool words = false;
        for (const auto& e : seen) {
            if (e.type != "ble.tracker") continue;
            n++;
            watchlist::List l;
            l.kind = watchlist::Kind::Words; l.values = {"tracker"};
            std::string m, id;
            if (watchlist::matchList(l, e, m, id)) words = true;
        }
        CHECK(n == 5, "ble.tracker events %d", n);
        CHECK(words, "a Words watchlist with 'tracker' matches the alert");
    }
    // the tracker that is marked as the user's own and a clear
    r.ignoreTracker(t.trackers[0].id, true);
    r.clearResults();
    for (size_t done = 0; done < (size_t)(0.5 * c.rate); done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    BletrkTelemetry t2;
    r.telemetry(t2, t.seq);
    CHECK(t2.alerts.empty() || t2.alerts.size() < 5, "alerts after clear: %zu", t2.alerts.size());
    automation::global().setTap(nullptr, 0);
    return testSummary("bletrk live");
}
