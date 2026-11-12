// The AIS station-kind entries (families 738, 807, 810, 813, 816, 857; ais_rx.h: aisViews): views onto the AIS receiver that report one kind of station:
// Class A, Class B, base stations, aids to navigation, search and rescue aircraft, and the devices with MMSI 970 / 972 / 974 (AIS-SART, man overboard, EPIRB-AIS).
//   - the simulated port of ais_gen (class A vessels, a class B, a base station, aids, an aircraft) goes, with a carrier error and noise, into one receiver without a view and one per view, from the same samples:
//     a view lists exactly the stations of its class that the receiver without a view lists, its messages, application messages and sentences belong to them, and the views together list every station;
//   - bursts built from position reports of ships and of MMSI 970123456, 972123456 and 974123456: the device view lists exactly the three devices and nothing else, the Class A view lists every position report;
//   - noise alone gives nothing in any view.
// Simulated signals only (no recording of an AIS-SART was available; the burst bits are those of a type 1 message).
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/ais/ais_testutil.h"
#include <cstdio>
#include <memory>
#include <set>
using namespace dect2;
using namespace dect2::ais;
using namespace dect2::aistest;
#include "../common/test_util.h"

static std::set<uint32_t> mmsis(const AisTelemetry& t) { std::set<uint32_t> s; for (const auto& v : t.vessels) s.insert(v.mmsi); return s; }
static std::set<uint32_t> mmsisOfClass(const AisTelemetry& t, uint32_t mask) { std::set<uint32_t> s; for (const auto& v : t.vessels) if ((mask >> v.cls) & 1u) s.insert(v.mmsi); return s; }

static std::vector<AisTelemetry> runAll(const std::vector<cf32>& x, double rate, const std::vector<std::pair<uint32_t, bool>>& views) {
    std::vector<std::unique_ptr<AisReceiver>> rx;
    for (auto& v : views) { rx.emplace_back(new AisReceiver); rx.back()->configure(rate); rx.back()->setStationView(v.first, v.second); }
    for (size_t o = 0; o < x.size(); o += 16384) for (auto& r : rx) r->feed(x.data() + o, std::min<size_t>(16384, x.size() - o));
    std::vector<AisTelemetry> out(rx.size());
    for (size_t i = 0; i < rx.size(); i++) rx[i]->telemetry(out[i], 0);
    return out;
}

int main() {
    int nv = 0;
    const AisView* views = aisViews(nv);
    CHECK(nv == 6, "%d views", nv);
    for (int i = 0; i < nv; i++) {
        CHECK(aisViewForActive(views[i].family + 1) == &views[i] && aisViewForStd(views[i].family + 2) == &views[i], "view %d lookup", i);
        const ModeTuning t = aisViewTuning(i);
        CHECK(t.stdMode == views[i].family + 2 && std::string(t.id) == views[i].id && t.defMhz == aisTuning().defMhz, "view %d tuning", i);
        for (int j = 0; j < i; j++) CHECK(views[j].family != views[i].family && std::strcmp(views[j].id, views[i].id) != 0, "views %d and %d clash", j, i);
    }
    CHECK(aisViewForActive(15) == nullptr && aisViewForStd(16) == nullptr, "the AIS mode is not a view");

    std::vector<std::pair<uint32_t, bool>> cfgs{{0, false}};
    for (int i = 0; i < nv; i++) cfgs.push_back({views[i].classMask, views[i].devicesOnly});

    // 1. the simulated port with carrier errors and noise
    for (double cfo : {0.0, 3000.0}) {
        SynthConfig sc; sc.snrDb = cfo != 0 ? 26 : 34; sc.cfoHz = cfo;
        const double rate = 2e6, secs = 30;
        auto synth = makeAisSynth(sc, rate);
        std::vector<cf32> x((size_t)(secs * rate));
        synth->generate(x.data(), x.size());
        const auto r = runAll(x, rate, cfgs);
        size_t total = 0;
        for (int i = 0; i < nv; i++) {
            if (views[i].devicesOnly) { CHECK(r[(size_t)i + 1].vessels.empty(), "cfo %.0f: the device view lists %zu stations of the port", cfo, r[(size_t)i + 1].vessels.size()); continue; }
            const AisTelemetry& t = r[(size_t)i + 1];
            const auto want = mmsisOfClass(r[0], views[i].classMask), got = mmsis(t);
            CHECK(want == got, "cfo %.0f: view %s lists %zu stations, the receiver without a view %zu of that class", cfo, views[i].id, got.size(), want.size());
            CHECK(t.vesselCount == t.vessels.size(), "view %s: vesselCount %u", views[i].id, t.vesselCount);
            for (const auto& rc : t.recent) CHECK(got.count(rc.mmsi), "view %s: a message of %u", views[i].id, rc.mmsi);
            for (const auto& a : t.apps) CHECK(got.count(a.mmsi), "view %s: an application message of %u", views[i].id, a.mmsi);
            uint64_t msgs = 0; for (auto& v : t.vessels) msgs += v.messages;
            CHECK(t.blocksOk == msgs, "view %s: blocksOk %llu vs %llu", views[i].id, (unsigned long long)t.blocksOk, (unsigned long long)msgs);
            total += got.size();
            printf("  cfo %5.0f: view %-10s %zu stations, %zu messages listed\n", cfo, views[i].id, got.size(), t.recent.size());
        }
        CHECK(total == r[0].vessels.size() && total >= 18, "cfo %.0f: the views list %zu stations, without a view %zu", cfo, total, r[0].vessels.size());
        CHECK(!mmsisOfClass(r[0], 1u << AIS_CLASS_BASE).empty() && !mmsisOfClass(r[0], 1u << AIS_CLASS_ATON).empty() && !mmsisOfClass(r[0], 1u << AIS_CLASS_SAR).empty() && !mmsisOfClass(r[0], 1u << AIS_CLASS_B).empty(), "cfo %.0f: the port is missing a class", cfo);
    }
    // 2. position reports of ships and of three distress devices
    {
        std::mt19937 rng(5);
        std::vector<AisBurstSpec> bursts;
        const uint32_t ids[] = {232123456, 244123456, 970123456, 972123456, 974123456, 366123456};
        double t0 = 0.2;
        for (int rep = 0; rep < 3; rep++) for (uint32_t id : ids) { AisBurstSpec b; b.payload = randomPosition(rng, id); b.channel = rep % 2 ? 'B' : 'A'; b.startSec = t0; b.level = 1.0; b.cfoHz = (rep - 1) * 800.0; bursts.push_back(b); t0 += 0.35; }
        AisRenderConfig rc; rc.rate = 2e6; rc.durationSec = t0 + 0.5; rc.snrDb = 28; rc.seed = 3;
        const auto x = renderAisBursts(bursts, rc);
        const auto r = runAll(x, rc.rate, cfgs);
        const std::set<uint32_t> devices{970123456, 972123456, 974123456};
        CHECK(mmsis(r[0]).size() == 6, "devices scene: %zu stations without a view", mmsis(r[0]).size());
        for (int i = 0; i < nv; i++) {
            const auto got = mmsis(r[(size_t)i + 1]);
            if (views[i].devicesOnly) { CHECK(got == devices, "the device view lists %zu stations (want the three devices)", got.size()); for (auto& v : r[(size_t)i + 1].vessels) CHECK(devices.count(v.mmsi), "device view: %u", v.mmsi); for (auto& rcd : r[(size_t)i + 1].recent) CHECK(devices.count(rcd.mmsi), "device view message of %u", rcd.mmsi); }
            else if (views[i].classMask == (1u << AIS_CLASS_A)) CHECK(got == mmsis(r[0]), "Class A view lists %zu of %zu position reports' stations", got.size(), mmsis(r[0]).size());
            else CHECK(got.empty(), "view %s lists %zu stations of position reports", views[i].id, got.size());
        }
        printf("  devices scene: %zu stations, the device view %zu\n", mmsis(r[0]).size(), mmsis(r[nv]).size());
    }
    // 3. noise alone
    {
        std::mt19937 g(1); std::normal_distribution<float> nd(0.f, 0.05f); std::vector<cf32> n((size_t)(2e6 * 10)); for (auto& v : n) v = cf32(nd(g), nd(g));
        const auto r = runAll(n, 2e6, cfgs);
        for (size_t i = 0; i < r.size(); i++) CHECK(r[i].vessels.empty(), "noise: receiver %zu lists %zu stations", i, r[i].vessels.size());
    }
    return testSummary("ais views");
}
