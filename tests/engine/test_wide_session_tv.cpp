// Wide multi-signal decode with TV in the capture (core/include/dect2/engine/wide_session.h): one 20 Msps capture at 522 MHz holds a DVB-T2 channel
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

static const double kFs = 20.0e6, kCentre = 522.0e6;

static bool hasName(const std::vector<MultiService>& v, const char* name) {
    for (auto& s : v) if (s.svc.name == name) return true;
    return false;
}

int main() {
    const double offDmr = +8.9e6 + 3100, offPag = -9.0e6 + 1700;
    std::vector<ChanSpec> ch = {{false, -4e6, 8, 11, "Wide A", 0, 0, 0x1001, 1}, {true, +4e6, 8, 22, "Wide B", -1, 0, 0x1002, 2}};
    std::vector<std::unique_ptr<ChanSource>> tv;
    for (auto& c : ch) tv.emplace_back(new ChanSource(c, kFs));
    SynthConfig cd; cd.snrDb = 45;
    std::unique_ptr<ModeSynth> dmr = makeDmrSynth(cd, kFs);
    SynthConfig cg; cg.snrDb = 45; cg.modeOpt[2] = kPocsag1200 + 1; cg.modeOpt[3] = 21;
    std::unique_ptr<ModeSynth> pag = makePagerSynth(cg, kFs);
    // where the two synths put their signals with cfo = 0 (centroid), to place them
    auto base = [&](std::unique_ptr<ModeSynth> s) {
        std::vector<cf32> v((size_t)(kFs * 0.1));
        s->generate(v.data(), v.size());
        double best = 0, bf = 0;
        for (double f = -120000; f <= 120000; f += 2000) {
            const double w = 2 * M_PI * f / kFs; std::complex<double> a = 0;
            for (size_t i = 0; i < v.size(); i += 8) a += std::complex<double>(v[i]) * std::polar(1.0, -w * (double)i);   // complex input: the sign of f counts
            if (std::abs(a) > best) { best = std::abs(a); bf = f; }
        }
        return bf;
    };
    const double bDmr = base(makeDmrSynth(cd, kFs)), bPag = base(makePagerSynth(cg, kFs));
    const double shDmr = offDmr - bDmr, shPag = offPag - bPag;
    std::complex<double> phD{1, 0}, phP{1, 0};

    WideSession ws;
    WsConfig cfg; cfg.spanRate = kFs; cfg.centerHz = kCentre; cfg.cpuBudget = 0; cfg.blockingTv = true;
    ws.configure(cfg);
    ws.useFrequencyDatabase(false);
    ws.setKnownChannels({{kCentre + offPag, WsKind::Pager}});

    const size_t blk = (size_t)(kFs * 0.05);
    std::vector<cf32> sum(blk), tmp(blk);
    std::mt19937 rng(9);
    std::normal_distribution<float> nd(0.f, 0.0003f);
    double t = 0;
    auto find = [&](WsKind k) -> WsEntry { for (const WsEntry& e : ws.entries()) if (e.kind == k) return e; return WsEntry(); };
    auto pull = [&](double secs) {
        const double end = t + secs;
        for (; t < end; t += 0.05) {
            std::fill(sum.begin(), sum.end(), cf32(0, 0));
            for (auto& s : tv) { s->pull(blk, tmp); for (size_t i = 0; i < blk; i++) sum[i] += tmp[i]; }
            for (size_t i = 0; i < blk; i++) sum[i] *= 0.15f;
            dmr->generate(tmp.data(), blk);
            { const std::complex<double> st = std::polar(1.0, 2 * M_PI * shDmr / kFs); for (size_t i = 0; i < blk; i++) { sum[i] += tmp[i] * cf32((float)phD.real(), (float)phD.imag()) * 0.03f; phD *= st; } phD /= std::abs(phD); }
            pag->generate(tmp.data(), blk);
            { const std::complex<double> st = std::polar(1.0, 2 * M_PI * shPag / kFs); for (size_t i = 0; i < blk; i++) { sum[i] += tmp[i] * cf32((float)phP.real(), (float)phP.imag()) * 0.03f; phP *= st; } phP /= std::abs(phP); }
            for (size_t i = 0; i < blk; i++) sum[i] += cf32(nd(rng), nd(rng));
            ws.feed(sum.data(), blk);
        }
    };
    auto dump = [&]() {
        for (const WsEntry& e : ws.entries())
            printf("  id %d %-10s %-14s %+9.1f kHz  %5.1f dB  [%s] %s | %s\n", e.id, wsKindName(e.kind), wsStateName(e.state), e.offsetHz / 1e3, e.snrDb, e.locked ? "locked" : "-", e.info.c_str(), e.identity.c_str());
    };
    auto servicesOk = [&]() {
        MultiTv* a = ws.tv(false); MultiTv* b = ws.tv(true);
        return a && b && hasName(a->services(), "Wide A") && hasName(b->services(), "Wide B");
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
    const WsEntry t2 = find(WsKind::DvbT2), t1 = find(WsKind::DvbT), dm = find(WsKind::Dmr), pg = find(WsKind::Pager);
    CHECK(t2.kind == WsKind::DvbT2 && std::fabs(t2.offsetHz + 4e6) < 1e5, "DVB-T2 channel not found at -4 MHz (%s %+.0f kHz)", wsKindName(t2.kind), t2.offsetHz / 1e3);
    CHECK(t1.kind == WsKind::DvbT && std::fabs(t1.offsetHz - 4e6) < 1e5, "DVB-T channel not found at +4 MHz (%s %+.0f kHz)", wsKindName(t1.kind), t1.offsetHz / 1e3);
    CHECK(t2.state == WsState::Running && t1.state == WsState::Running, "TV entries are %s / %s", wsStateName(t2.state), wsStateName(t1.state));
    CHECK(servicesOk(), "the service lists of the two channels (T2 receiver %p, T receiver %p)", (void*)ws.tv(false), (void*)ws.tv(true));
    CHECK(dm.kind == WsKind::Dmr && std::fabs(dm.offsetHz - offDmr) < 4000, "DMR carrier next to the TV channels: %s %+.1f kHz", wsKindName(dm.kind), dm.offsetHz / 1e3);
    CHECK(pg.kind == WsKind::Pager && std::fabs(pg.offsetHz - offPag) < 4000, "pager carrier next to the TV channels: %s %+.1f kHz", wsKindName(pg.kind), pg.offsetHz / 1e3);
    int tvCarriers = 0;   // the TV zones are left out: no narrowband entry inside them
    for (const WsEntry& e : ws.entries()) if (!wsIsTv(e.kind) && std::fabs(e.offsetHz) < 7.9e6) tvCarriers++;
    CHECK(tvCarriers == 0, "%d narrowband entries inside the TV channels", tvCarriers);
    {
        RxTelemetry rt; uint64_t seq = 0;
        ws.modes().telemetry(10, rt, seq);
        CHECK(rt.dmr.cc >= 0 || !rt.dmr.callLog.empty() || !rt.dmr.link.empty(), "DMR decoder delivered nothing");
        seq = 0; RxTelemetry rg; ws.modes().telemetry(24, rg, seq);
        CHECK(rg.pager.messagesTotal >= 1, "pager decoder: %llu pages", (unsigned long long)rg.pager.messagesTotal);
    }

    // switch the DVB-T entry off: its receiver stops, the rest goes on
    ws.setEnabled(t1.id, false);
    pull(3.0);
    ws.wait();
    CHECK(find(WsKind::DvbT).state == WsState::Off, "DVB-T entry %s", wsStateName(find(WsKind::DvbT).state));
    CHECK(ws.tv(true) && ws.tv(true)->runningSlots().empty(), "the DVB-T receiver still runs");
    CHECK(ws.tv(false) && !ws.tv(false)->runningSlots().empty() && find(WsKind::DvbT2).state == WsState::Running, "the DVB-T2 receiver stopped with it");
    ws.setEnabled(t1.id, true);
    pull(3.0);
    ws.wait();
    CHECK(find(WsKind::DvbT).state == WsState::Running && ws.tv(true) && !ws.tv(true)->runningSlots().empty(), "DVB-T did not come back");
    return testSummary("wide_session_tv");
}
