// Wide session, three TV channels in one capture (the "two or three DVB channels per radio" case), off the 8 MHz grid:
// one 28 Msps capture at 522 MHz holds DVB-T2 (-8.4 MHz), DVB-T (+0.3 MHz) and DVB-T2 (+8.2 MHz). Each is found, identified and decoded at once.
// (the file the test is derived from, test_wide_session_tv.cpp: Wide multi-signal decode with TV in the capture (core/include/dect2/engine/wide_session.h): one 20 Msps capture at 522 MHz holds a DVB-T2 channel
// (-4 MHz) and a DVB-T channel (+4 MHz), side by side, and two narrowband carriers in the gaps outside them (a DMR base station at +8.9 MHz and a
// pager carrier at -9.0 MHz on a known channel). The session finds the two TV slots, identifies DVB-T2 and DVB-T from the signal, starts a multi-channel
// receiver of the right standard on each, leaves the TV zones out of the narrowband search, finds and identifies the DMR carrier, and decodes all
// four at once. A TV entry can be switched off (its receiver stops, the others go on).
#include "dect2/dmr/dmr_gen.h"
#include "dect2/engine/modes.h"
#include "dect2/engine/multitv.h"
#include "dect2/engine/wide_session.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/t2/t2rx.h"
#include "multitv_testkit.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
using namespace dect2;
using namespace multitvtest;
#include "../common/test_util.h"

static const double kFs = 28.0e6, kCentre = 522.0e6;

static bool hasName(const std::vector<MultiService>& v, const char* name) {
    for (auto& s : v) if (s.svc.name == name) return true;
    return false;
}

int main() {
    std::vector<ChanSpec> ch = {{false, -8.4e6, 8, 11, "Wide A", 0, 0, 0x1001, 1}, {true, +0.3e6, 8, 22, "Wide B", -1, 0, 0x1002, 2}, {false, +8.2e6, 8, 33, "Wide C", 0, 0, 0x1003, 3}};
    std::vector<std::unique_ptr<ChanSource>> tv;
    for (auto& c : ch) tv.emplace_back(new ChanSource(c, kFs));
    WideSession ws;
    WsConfig cfg; cfg.spanRate = kFs; cfg.centerHz = kCentre; cfg.cpuBudget = 0; cfg.blockingTv = true;
    ws.configure(cfg);
    ws.useFrequencyDatabase(false);
    const size_t blk = (size_t)(kFs * 0.05);
    std::vector<cf32> sum(blk), tmp(blk);
    std::mt19937 rng(9);
    std::normal_distribution<float> nd(0.f, 0.0003f);
    double t = 0;
    auto pull = [&](double secs) {
        const double end = t + secs;
        for (; t < end; t += 0.05) {
            std::fill(sum.begin(), sum.end(), cf32(0, 0));
            for (auto& s : tv) { s->pull(blk, tmp); for (size_t i = 0; i < blk; i++) sum[i] += tmp[i]; }
            for (size_t i = 0; i < blk; i++) sum[i] = sum[i] * 0.12f + cf32(nd(rng), nd(rng));
            ws.feed(sum.data(), blk);
        }
    };
    auto servicesOk = [&]() {
        MultiTv* a = ws.tv(false); MultiTv* b = ws.tv(true);
        return a && b && hasName(a->services(), "Wide A") && hasName(a->services(), "Wide C") && hasName(b->services(), "Wide B");
    };
    auto dump = [&]() {
        for (const WsEntry& e : ws.entries())
            printf("  id %d %-10s %-14s %+9.1f kHz  %5.1f dB  [%s] %s | %s\n", e.id, wsKindName(e.kind), wsStateName(e.state), e.offsetHz / 1e3, e.snrDb, e.locked ? "locked" : "-", e.info.c_str(), e.identity.c_str());
    };
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    for (int round = 0; round < 8 && !ok; round++) {
        pull(2.0);
        ws.wait();
        for (int i = 0; i < 40 && !ok; i++) { ok = servicesOk(); if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    }
    pull(3.0);
    ws.wait();
    printf("after %.1f s of signal (wall %.0f s):\n", t, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    dump();
    int nt2 = 0, nt = 0;
    for (const WsEntry& e : ws.entries()) { nt2 += e.kind == WsKind::DvbT2 && e.state == WsState::Running; nt += e.kind == WsKind::DvbT && e.state == WsState::Running; }
    CHECK(nt2 == 2 && nt == 1, "running TV entries: %d DVB-T2, %d DVB-T (want 2 and 1)", nt2, nt);
    const double want[3] = {-8.4e6, +0.3e6, +8.2e6};
    for (double w : want) {
        bool found = false;
        for (const WsEntry& e : ws.entries()) if (wsIsTv(e.kind) && std::fabs(e.offsetHz - w) < 5e5) found = true;
        CHECK(found, "no TV entry near %+.1f MHz", w / 1e6);
    }
    CHECK(servicesOk(), "the service lists of the three channels");
    int stray = 0;   // a channel off the 8 MHz grid sticks out of its slot: no narrowband entry may be made of it
    for (const WsEntry& e : ws.entries()) if (!wsIsTv(e.kind) && std::fabs(e.offsetHz) < 12.6e6) stray++;
    CHECK(stray == 0, "%d narrowband entries made of the TV channels' edges", stray);
    return testSummary("wide_multi_tv");
}
