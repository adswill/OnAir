// Wide multi-signal decode (core/include/dect2/engine/wide_session.h): ONE 2.4 Msps capture at 125 MHz holds five different signals at once, all off
// their channel raster: a DMR base station (-603 kHz), a P25 control/voice carrier (+401.7 kHz), a pager carrier (POCSAG 1200, on a known channel,
// -996 kHz), an APRS packet carrier (on a known channel, +1097 kHz) and an air-band AM station (125.2123 MHz, two tones). The session finds every
// carrier, identifies it (DMR and P25 from their frame syncs, pager and APRS from the known channel list, AM from the air band), starts the decoder of
// each and the decoders deliver their results. Then: a switched-off entry stops and comes back, the CPU guard drops the lowest ranked entries (never a
// pinned or listened-to one, the pinned AM station's sound keeps playing) and gives them back when the load falls, and a carrier with nothing
// recognisable stays listed as unidentified and can be given a kind by hand.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/engine/modes.h"
#include "dect2/engine/wide_session.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/packet/packet_gen.h"
#include "dect2/t2/t2rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
using namespace dect2;
#include "../common/test_util.h"

static const double kFs = 2.4e6, kCentre = 125.0e6;

// the offset a synth puts its signal at when cfoHz = 0 (some synths include the receiver's tuning offset): the power centroid within +-120 kHz
struct Src { std::unique_ptr<ModeSynth> s; float gain; double shiftHz = 0; std::complex<double> ph{1, 0}; };

static double goertzelMono(const std::vector<float>& x, size_t from, size_t to, double f, double rate) {
    const double w = 2 * M_PI * f / rate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < to && i < x.size(); i++) { const double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    const size_t n = to > from ? std::min(to, x.size()) - from : 0;
    return n ? std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (double)n : 0;
}

int main() {
    const double offDmr = -603000, offP25 = +401700, offPag = -996000, offApr = +1097000, offAm = +212300;
    std::vector<Src> src;
    // the synths make their signal around 0 Hz (some a little off it); each is moved to its place here, with an exact mixer (the DMR generator's own
    // carrier offset only holds for small offsets)
    auto add = [&](std::unique_ptr<ModeSynth> s, double off, float gain, double fixedBase) {
        const double base = fixedBase;
        auto s2 = std::move(s);
        src.push_back({std::move(s2), gain, off - base});
    };
    SynthConfig cd; cd.snrDb = 28;
    add(makeDmrSynth(cd, kFs), offDmr, 0.6f, 0);   // the receivers' tuning offset: P25, pager and packet make their signal 50 kHz below 0 (the radio sits 50 kHz above the channel)
    SynthConfig cp; cp.snrDb = 28;
    add(makeP25Synth(cp, kFs), offP25, 0.6f, -50000);
    SynthConfig cg; cg.snrDb = 30; cg.modeOpt[2] = kPocsag1200 + 1; cg.modeOpt[3] = 21;
    add(makePagerSynth(cg, kFs), offPag, 0.6f, -50000);
    SynthConfig ck; ck.snrDb = 30;
    add(makePacketSynth(ck, kFs), offApr, 0.6f, -50000);
    for (const Src& q : src) printf("synth shifted by %.0f Hz\n", q.shiftHz);

    WideSession ws;
    WsConfig cfg; cfg.spanRate = kFs; cfg.centerHz = kCentre; cfg.cpuBudget = 0.8; cfg.tv = false;
    ws.configure(cfg);
    ws.useFrequencyDatabase(false);
    ws.setKnownChannels({{kCentre + offPag, WsKind::Pager}, {kCentre + offApr, WsKind::Packet}});
    std::vector<float> snd;
    ws.setAudioSink([&](const float* s, size_t n) { snd.insert(snd.end(), s, s + 2 * n); });

    const size_t blk = (size_t)(kFs * 0.05);
    std::vector<cf32> sum(blk), tmp(blk);
    uint32_t rng = 777;
    double amPh1 = 0, amPh2 = 0;
    double t = 0;
    auto run = [&](double secs) {
        const double end = t + secs;
        for (; t < end; t += 0.05) {
            std::fill(sum.begin(), sum.end(), cf32(0, 0));
            for (Src& s : src) {
                s.s->generate(tmp.data(), blk);
                const std::complex<double> st = std::polar(1.0, 2 * M_PI * s.shiftHz / kFs);
                std::complex<double> ph = s.ph;
                for (size_t i = 0; i < blk; i++) { sum[i] += tmp[i] * cf32((float)ph.real(), (float)ph.imag()) * s.gain; ph *= st; }
                s.ph = ph / std::abs(ph);
            }
            for (size_t i = 0; i < blk; i++) {   // air-band AM: carrier 0.12, two tones (700 / 1300 Hz), 60 % modulation, at +212.3 kHz
                const double m = 1.0 + 0.3 * std::sin(amPh1) + 0.3 * std::sin(amPh2);
                const double ph = 2 * M_PI * offAm * (t * kFs + (double)i) / kFs;
                sum[i] += cf32((float)(0.12 * m * std::cos(ph)), (float)(0.12 * m * std::sin(ph)));
                amPh1 += 2 * M_PI * 700.0 / kFs; amPh2 += 2 * M_PI * 1300.0 / kFs;
                rng = rng * 1664525u + 1013904223u;
                sum[i] += cf32(((rng >> 8) & 0xffff) / 65536.f - .5f, ((rng >> 12) & 0xffff) / 65536.f - .5f) * 0.0004f;
            }
            ws.feed(sum.data(), blk);
        }
    };
    auto find = [&](WsKind k) -> WsEntry {
        for (const WsEntry& e : ws.entries()) if (e.kind == k) return e;
        return WsEntry();
    };
    auto dump = [&]() {
        for (const WsEntry& e : ws.entries())
            printf("  id %d %-10s %-14s %+9.1f kHz  %5.1f dB  rank %5.1f  cost %.3f  [%s] %s | %s\n", e.id, wsKindName(e.kind), wsStateName(e.state), e.offsetHz / 1e3, e.snrDb, e.rank, e.cost,
                   e.locked ? "locked" : "-", e.info.c_str(), e.identity.c_str());
        const WsStatus st = ws.status();
        printf("  load %.2f budget %.2f running %d dropped %d (events %llu)\n", st.load, st.budget, st.running, st.droppedForCpu, (unsigned long long)st.dropEvents);
    };

    // ---- 1. find, identify, decode
    run(14.0);
    ws.wait();
    run(1.0);
    printf("after 15 s:\n"); dump();
    struct Want { WsKind k; double off; const char* name; };
    const Want want[] = {{WsKind::Dmr, offDmr, "DMR"}, {WsKind::P25, offP25, "P25"}, {WsKind::Pager, offPag, "pager"}, {WsKind::Packet, offApr, "APRS"}, {WsKind::Am, offAm, "AM"}};
    for (const Want& w : want) {
        const WsEntry e = find(w.k);
        CHECK(e.kind == w.k, "%s not found/identified", w.name);
        if (e.kind != w.k) continue;
        CHECK(std::fabs(e.offsetHz - w.off) < 3000, "%s at %+.1f kHz (wanted %+.1f)", w.name, e.offsetHz / 1e3, w.off / 1e3);
        CHECK(e.state == WsState::Running, "%s is %s", w.name, wsStateName(e.state));
        CHECK(std::fabs(e.absHz - (kCentre + e.offsetHz)) < 1, "%s absolute frequency", w.name);
    }
    {   // the decoders delivered
        RxTelemetry rt; uint64_t seq = 0;
        CHECK(ws.modes().telemetry(10, rt, seq) && (!rt.dmr.callLog.empty() || rt.dmr.cc >= 0 || !rt.dmr.link.empty()), "DMR decoder delivered nothing (cc %d)", rt.dmr.cc);
        seq = 0; RxTelemetry rp;
        CHECK(ws.modes().telemetry(51, rp, seq) && (rp.p25.nac >= 0 || !rp.p25.log.empty()), "P25 decoder delivered nothing (nac %d)", rp.p25.nac);
        seq = 0; RxTelemetry rg;
        ws.modes().telemetry(24, rg, seq);
        CHECK(rg.pager.messagesTotal >= 1, "pager decoder: %llu pages", (unsigned long long)rg.pager.messagesTotal);
        seq = 0; RxTelemetry rk;
        ws.modes().telemetry(25, rk, seq);
        CHECK(rk.packet.blocksOk >= 2, "APRS decoder: %llu frames", (unsigned long long)rk.packet.blocksOk);
        printf("  DMR cc %d, P25 nac %03X, pager %llu pages, APRS %llu frames\n", rt.dmr.cc, rp.p25.nac, (unsigned long long)rg.pager.messagesTotal, (unsigned long long)rk.packet.blocksOk);
    }
    {   // AM station heard
        const size_t from = snd.size() / 2 * 2 / 4;
        std::vector<float> mono;
        for (size_t i = from; i + 1 < snd.size(); i += 2) mono.push_back(snd[i] + snd[i + 1]);
        const double a700 = goertzelMono(mono, 0, mono.size(), 700, 48000), a1300 = goertzelMono(mono, 0, mono.size(), 1300, 48000), off = goertzelMono(mono, 0, mono.size(), 2500, 48000);
        printf("  AM sound: 700 Hz %.4f, 1300 Hz %.4f, 2500 Hz %.4f (%zu samples)\n", a700, a1300, off, mono.size());
        CHECK(a700 > 0.005 && a1300 > 0.005 && a700 > 4 * off && a1300 > 4 * off, "air-band AM sound missing");
    }
    CHECK(ws.status().running >= 5, "%d running", ws.status().running);

    // ---- 2. switch an entry off and on
    const int dmrId = find(WsKind::Dmr).id;
    ws.setEnabled(dmrId, false);
    run(2.0);
    CHECK(find(WsKind::Dmr).state == WsState::Off, "DMR entry %s after switching it off", wsStateName(find(WsKind::Dmr).state));
    run(10.0);
    CHECK(ws.modes().liveCount(10) == 0, "the DMR decoder still runs %zu carriers after the entry was switched off", ws.modes().liveCount(10));
    CHECK(find(WsKind::P25).state == WsState::Running && find(WsKind::Pager).state == WsState::Running, "other entries stopped");
    ws.setEnabled(dmrId, true);
    run(5.0);
    CHECK(find(WsKind::Dmr).state == WsState::Running, "DMR entry %s after switching it on", wsStateName(find(WsKind::Dmr).state));
    CHECK(ws.modes().liveCount(10) >= 1, "DMR decoder did not restart");

    // ---- 3. the CPU guard: pinned DMR and the listened-to AM stay, the rest are dropped one by one, then come back
    const int amId = find(WsKind::Am).id;
    ws.setPinned(dmrId, true);
    ws.setListen(amId);
    ws.setLoadOverride(2.0);
    const size_t sndMark = snd.size();
    run(9.0);
    printf("guard, load 2.0 for 9 s:\n"); dump();
    int droppedN = 0;
    for (const WsEntry& e : ws.entries()) droppedN += e.state == WsState::Dropped;
    CHECK(droppedN == 3, "%d entries dropped by the guard", droppedN);
    CHECK(find(WsKind::Dmr).state == WsState::Running, "the pinned DMR entry was dropped");
    CHECK(find(WsKind::Am).state == WsState::Running, "the listened-to AM entry was dropped");
    CHECK(find(WsKind::P25).state == WsState::Dropped && find(WsKind::Pager).state == WsState::Dropped && find(WsKind::Packet).state == WsState::Dropped, "P25 / pager / APRS must all be dropped (%s, %s, %s)", wsStateName(find(WsKind::P25).state), wsStateName(find(WsKind::Pager).state), wsStateName(find(WsKind::Packet).state));
    CHECK(ws.status().tooSlow && ws.status().droppedForCpu == droppedN && ws.status().dropEvents >= (uint64_t)droppedN, "status does not show the drops");
    CHECK(!find(WsKind::Pager).why.empty(), "a dropped entry says nothing about why");
    {   // the AM sound kept playing through the drops (its tone present in the last seconds)
        std::vector<float> mono;
        for (size_t i = sndMark; i + 1 < snd.size(); i += 2) mono.push_back(snd[i] + snd[i + 1]);
        const double a = goertzelMono(mono, mono.size() / 3, mono.size(), 700, 48000);
        printf("  AM tone during the drops: %.4f over %zu samples\n", a, mono.size());
        CHECK(mono.size() > 48000 * 6 && a > 0.005, "the sound of the entries that stay was disturbed by the guard");
    }
    ws.setLoadOverride(0.1);
    run(24.0);
    printf("guard released:\n"); dump();
    droppedN = 0;
    for (const WsEntry& e : ws.entries()) droppedN += e.state == WsState::Dropped;
    CHECK(droppedN == 0 && !ws.status().tooSlow, "%d entries still dropped after the load fell", droppedN);
    CHECK(find(WsKind::Pager).state == WsState::Running && find(WsKind::Packet).state == WsState::Running, "dropped entries did not come back");

    // ---- 4. no limit: with the budget off nothing is dropped at any load
    ws.setBudget(0);
    ws.setLoadOverride(5.0);
    run(4.0);
    droppedN = 0;
    for (const WsEntry& e : ws.entries()) droppedN += e.state == WsState::Dropped;
    CHECK(droppedN == 0, "the guard dropped %d entries with the budget switched off", droppedN);

    return testSummary("wide_session");
}
