// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Whole span for DVB-S/S2 through MultiTv (sat mode): two transponders of different symbol rates and standards side by side in one 20 Msps capture, one
// of them off centre, plus the SDR's DC spike. Both must be found without being told where they are or how fast they run, both must lock and show
// their own services; the selected one's packets reach the sink and switching works.
#include "dect2/engine/multitv.h"
#include "dect2/dvbs/dvbs_gen.h"
#include <atomic>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double rate = 20e6;
    struct Spec { double fc, rs; int standard; };
    const Spec spec[2] = {{-4.2e6, 4.0e6, 2}, {5.4e6, 3.0e6, 1}};
    std::vector<std::unique_ptr<dvbs::DvbsSignal>> sig;
    for (const Spec& s : spec) {
        dvbs::DvbsSignalConfig c;
        c.tx.standard = s.standard; c.tx.symbolRate = s.rs; c.tx.rollOff = 0.35;
        if (s.standard == 1) c.tx.rate = 1;   // DVB-S 2/3
        c.sampleRate = rate; c.snrDb = 22; c.cfoHz = s.fc; c.level = 0.15; c.seed = (uint32_t)(s.fc > 0 ? 7 : 3);
        sig.emplace_back(new dvbs::DvbsSignal(c));
    }
    MultiTv m;
    MultiTvOptions o;
    o.sat = true; o.spanRate = rate; o.maxChannels = 3; o.blocking = true;
    CHECK(m.configure(o), "configure");
    std::atomic<int> sinkSlot{-1};
    std::atomic<uint64_t> sinkPk{0};
    m.setSink([&](int slot, const uint8_t*, size_t n, double, const TsSnapshot&) { sinkSlot = slot; sinkPk += n; });
    std::vector<cf32> a(1 << 16), b(1 << 16);
    const size_t total = (size_t)(rate * 6);
    for (size_t done = 0; done < total; done += a.size()) {
        sig[0]->generate(a.data(), a.size());
        sig[1]->generate(b.data(), b.size());
        for (size_t i = 0; i < a.size(); i++) a[i] += b[i] + cf32(0.004f, 0.004f);   // a DC offset: the spike at 0 Hz
        m.feed(a.data(), a.size());
    }
    m.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    RxTelemetry t;
    for (int i = 0; i < 3; i++) m.poll(t);
    auto rows = m.channels();
    printf("rows %zu\n", rows.size());
    for (const auto& r : rows) printf("  id %d  %+.3f MHz  Rs %.3f  running %d locked %d  services %d  %s  load %.0f %%\n", r.slot, r.offsetHz / 1e6, r.symbolRate / 1e6, r.running, r.locked, r.services, r.detail.c_str(), r.cpuPct);
    CHECK(rows.size() == 2, "two transponders expected, found %zu", rows.size());
    for (const auto& r : rows) {
        const bool first = r.offsetHz < 0;
        CHECK(r.sat, "row not marked as a transponder");
        CHECK(std::fabs(r.offsetHz - (first ? spec[0].fc : spec[1].fc)) < 150e3, "centre %.0f", r.offsetHz);
        CHECK(std::fabs(r.symbolRate / (first ? spec[0].rs : spec[1].rs) - 1) < 0.06, "symbol rate %.0f", r.symbolRate);
        CHECK(r.running, "no receiver on %.2f MHz", r.offsetHz / 1e6);
        CHECK(r.locked, "no lock on %.2f MHz", r.offsetHz / 1e6);
        CHECK(r.services >= 1, "no services on %.2f MHz", r.offsetHz / 1e6);
    }
    CHECK(sinkPk.load() > 0, "the selected transponder delivered no packets");
    if (rows.size() == 2) {
        const int other = rows[0].selected ? rows[1].slot : rows[0].slot;
        m.setSelected(other);
        CHECK(m.selected() == other, "switch");
    }
    return testSummary("multitv_sat");
}
