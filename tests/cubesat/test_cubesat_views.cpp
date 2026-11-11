// The CubeSat framings with a mode-list entry of their own (cubesat_rx.h: cubesatViews, families 840 .. 855): AX100 Mode 5, AX100 Mode 6,
// USP, NGHam, U482C and BEESAT / Mobitex. Each entry is the CubeSat telemetry receiver with the frames of one framing; these checks are
//  - the tables: family / active standard / engine standard (+1 / +2), unique ids, the tuning table finds each one;
//  - the receiver: one stream with every framing on the air, a view reports its own framing only (frames, counts), the unfiltered
//    receiver still reports all of them;
//  - the engine: each entry's own test signal, started through setStandard(family + 2), is decoded as that framing and as no other,
//    and the standard reported is family + 1. Generated signals only (no recording of these satellites was used).
// No sound.
#include "dect2/cubesat/cubesat_gen.h"
#include "dect2/cubesat/cubesat_proto.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/cubesat/cubesat_tel.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void tables() {
    int n = 0;
    const CubesatView* v = cubesatViews(n);
    CHECK(n == 6, "%d views", n);
    std::set<std::string> ids;
    std::set<int> fr, fam;
    for (int i = 0; i < n; i++) {
        ids.insert(v[i].id); fr.insert(v[i].framing); fam.insert(v[i].family);
        CHECK(v[i].family >= 840 && v[i].family <= 869 && (v[i].family % 3) == 0, "%s: family %d", v[i].id, v[i].family);
        CHECK(cubesatViewForActive(v[i].family + 1) == &v[i] && cubesatViewForStd(v[i].family + 2) == &v[i], "%s: lookup", v[i].id);
        const ModeTuning* mt = modeTuningById(v[i].id);
        CHECK(mt && mt->stdMode == v[i].family + 2 && modeTuning(v[i].family + 2) == mt, "%s: tuning table", v[i].id);
        CHECK(mt && mt->defMhz == cubesatTuning().defMhz && mt->sampleRate == cubesatTuning().sampleRate && mt->tuneOffsetHz == cubesatTuning().tuneOffsetHz,
              "%s: same radio settings as CubeSat telemetry", v[i].id);
        CHECK(v[i].framing >= 0 && v[i].framing < cubesat::kNumFramings, "%s: framing", v[i].id);
        const std::string sh = cubesat::framingShort(v[i].framing);
        CHECK(sh == v[i].shortName, "%s: short name %s vs %s", v[i].id, sh.c_str(), v[i].shortName);
    }
    CHECK((int)ids.size() == n && (int)fr.size() == n && (int)fam.size() == n, "ids, framings and families are unique");
    CHECK(cubesatViewForActive(62) == nullptr && cubesatViewForStd(63) == nullptr, "the shared entry is not a view");
    CHECK(modeTuningById("cubesat") && modeTuningById("cubesat")->stdMode == 63, "the shared entry stays");
}

// one stream with every framing, several receivers on it: the unfiltered one hears all of them, a view only its own
static void receiverFilter() {
    const double rate = 2e6;
    int n = 0;
    const CubesatView* v = cubesatViews(n);
    SynthConfig sc;
    sc.modeOpt[0] = 0; sc.snrDb = 30;
    std::unique_ptr<ModeSynth> g = makeCubesatSynth(sc, rate);
    CHECK(g != nullptr, "synth");
    if (!g) return;
    std::vector<std::unique_ptr<CubesatReceiver>> rx;
    for (int i = 0; i <= n; i++) {
        rx.push_back(std::make_unique<CubesatReceiver>());
        rx[i]->configure(rate);
        rx[i]->setSignalOffset(-cubesatTuning().tuneOffsetHz);
        rx[i]->setForwardKiss(false);
        if (i < n) rx[i]->setFramingMask(1u << v[i].framing);
    }
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(cubesatGenCycleSec(0) * 1.3 * rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        g->generate(buf.data(), buf.size());
        for (auto& r : rx) r->feed(buf.data(), buf.size());
    }
    for (int i = 0; i <= n; i++) {
        CubeTelemetry t;
        CHECK(rx[i]->telemetry(t, 0), "receiver %d: telemetry", i);
        if (i == n) {
            int kinds = 0;
            for (int k = 0; k < cubesat::kNumFramings; k++) kinds += t.perFraming[k] > 0;
            CHECK(kinds == cubesat::kNumFramings, "unfiltered: %d of %d framings heard", kinds, cubesat::kNumFramings);
            continue;
        }
        int others = 0;
        for (int k = 0; k < cubesat::kNumFramings; k++) if (k != v[i].framing) others += (int)t.perFraming[k];
        int wrongFrames = 0;
        for (const CubeFrame& f : t.frames) wrongFrames += f.framing != v[i].framing;
        printf("  %-8s %llu frames, %zu listed, others %d\n", v[i].id, (unsigned long long)t.perFraming[v[i].framing], t.frames.size(), others);
        CHECK(t.perFraming[v[i].framing] > 0, "%s: its own framing not heard", v[i].id);
        CHECK(others == 0 && wrongFrames == 0, "%s: %d counts and %d frames of other framings", v[i].id, others, wrongFrames);
        CHECK(t.blocksOk == t.perFraming[v[i].framing] && !t.frames.empty(), "%s: frame count %llu", v[i].id, (unsigned long long)t.blocksOk);
    }
}

// the engine entry of one view: its own test signal, decoded as that framing only
static void engineEntry(const CubesatView& v, int index) {
    const int stdc = v.family + 2;
    const ModeTuning mt = cubesatViewTuning(index);
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = stdc;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(stdc);
    CHECK(e.start(dev, t, fo), "%s: engine start", v.id);
    CHECK(e.activeStandard() == v.family + 1, "%s: active standard %d", v.id, e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    while (elapsed() < 25.0 && !(rx.cube.perFraming[v.framing] >= 2 && elapsed() > 3.0)) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard != v.family + 1) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const CubeTelemetry& c = rx.cube;
    int others = 0;
    for (int k = 0; k < cubesat::kNumFramings; k++) if (k != v.framing) others += (int)c.perFraming[k];
    printf("  engine %-8s %llu frames (%s), others %d, %s\n", v.id, (unsigned long long)c.perFraming[v.framing], v.shortName, others, cubesatSummary(c).c_str());
    CHECK(wrong == 0, "%s: %d reports with the wrong standard", v.id, wrong);
    CHECK(c.perFraming[v.framing] >= 1, "%s: no %s frame decoded", v.id, v.shortName);
    CHECK(others == 0, "%s: %d frames of other framings", v.id, others);
    CHECK(!c.frames.empty() && c.frames.front().framing == v.framing, "%s: the frame list", v.id);
    CHECK(rx.rateOk, "%s: rate", v.id);
    e.stop();
}

int main() {
    tables();
    receiverFilter();
    int n = 0;
    const CubesatView* v = cubesatViews(n);
    for (int i = 0; i < n; i++) engineEntry(v[i], i);
    printf(fails ? "cubesat views: %d FAILED\n" : "cubesat views: all passed\n", fails);
    return fails ? 1 : 0;
}
