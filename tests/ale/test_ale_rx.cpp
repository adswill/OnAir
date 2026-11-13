// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE receiver against generated calls: a TWS sounding and a long-address TIS sounding, an individual call (TO .. TIS, an LQA,
// answered and acknowledged), a net call answered by two members, an AMD message, a group call; clean, then at 10 dB SNR in 3 kHz
// with +50 / -50 Hz tuning errors and 2 % of the bits turned over (the vote and Golay put them right); noise only gives nothing.
#include "dect2/ale/ale_gen.h"
#include "dect2/ale/ale_proto.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/ale/ale_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

constexpr double kRate = 250000;
constexpr double kOff = -20000;

static AleTelemetry run(const std::vector<AleGenCall>& calls, double snr, double cfo, double ber, uint32_t seed, double secs = -1) {
    AleGenConfig c;
    c.rate = kRate; c.dialOffsetHz = kOff; c.cfoHz = cfo; c.snrDb = snr; c.bitErrorRate = ber; c.seed = seed; c.calls = calls;
    AleGenerator g(c);
    AleReceiver r;
    r.setStartTime(0);
    r.configure(kRate);
    r.setSignalOffset(kOff);
    r.setDialHz(14.109e6);
    if (secs < 0) secs = g.durationSec() + 1.5;
    std::vector<cf32> x(25000);
    for (double t = 0; t < secs; t += 0.1) { g.generate(x.data(), x.size()); r.feed(x.data(), x.size()); }
    r.flush();
    AleTelemetry out;
    r.telemetry(out, 0);
    return out;
}

static const AleEvent* find(const AleTelemetry& t, const char* type, const char* from, const char* to = nullptr) {
    for (const auto& e : t.events) if (e.type == type && e.from == from && (!to || e.to == to)) return &e;
    return nullptr;
}

static void show(const AleTelemetry& t) {
    if (getenv("ALE_WORDS")) for (const auto& w : t.words) printf("      %8.4f %-4s %s v%d g%d\n", w.utc, ale::typeName(w.type), w.text.c_str(), w.voteErrs, w.golayErrs);
    for (const auto& e : t.events)
        printf("    %6.2f s  %-12s %-10s -> %-14s %s  ber %.3f  %+5.1f dB  %+5.1f Hz  %d words%s%s%s%s\n", e.utc, e.type.c_str(), e.from.c_str(), e.to.c_str(), e.tis ? "TIS" : "TWS",
               e.ber, e.snrDb, e.cfoHz, e.words, e.amd.empty() ? "" : "  AMD: ", e.amd.c_str(), e.cmd.empty() ? "" : "  ", e.cmd.c_str());
    printf("    %llu words, %llu missed, %zu stations\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.stations.size());
}

static void checkAll(const AleTelemetry& t, const char* name, double cfo, bool noisy) {
    printf("  %s\n", name);
    show(t);
    const AleEvent* e = find(t, "Sounding", "LY2ABC");
    CHECK(e && !e->tis && e->to.empty(), "%s: TWS sounding", name);
    e = find(t, "Sounding", "DL5ABCDEF");
    CHECK(e && e->tis, "%s: TIS sounding, long address", name);
    e = find(t, "Individual", "LY2ABC", "OH2XYZ");
    CHECK(e && e->tis && e->cmd.find("LQA BER 2 SINAD 24 MP 1") != std::string::npos, "%s: individual call TO OH2XYZ TIS LY2ABC with its LQA", name);
    if (e) CHECK(std::fabs(e->dialHz - 14.109e6) < 1, "%s: dial", name);
    if (e && std::fabs(cfo) > 1) CHECK(std::fabs(e->cfoHz - cfo) < 20, "%s: offset %.1f Hz, should be %.0f", name, e->cfoHz, cfo);
    CHECK(find(t, "Response", "OH2XYZ", "LY2ABC"), "%s: the response", name);
    CHECK(find(t, "Ack", "LY2ABC", "OH2XYZ"), "%s: the acknowledgement", name);
    CHECK(find(t, "Net", "K1ABC", "HFN"), "%s: net call", name);
    CHECK(find(t, "Net response", "W2XYZ", "K1ABC") && find(t, "Net response", "VE3ABC", "K1ABC"), "%s: both net members answered", name);
    e = find(t, "Individual", "G4XYZ", "LY2ABC");
    CHECK(e && e->amd == "QSY 14109 USB PSE", "%s: AMD \"%s\"", name, e ? e->amd.c_str() : "-");
    e = find(t, "Group", "EA1ABC");
    CHECK(e && e->to == "F5XYZ, I2ABC", "%s: group call", name);
    int heard = 0;
    for (const auto& s : t.stations) if (s.address == "LY2ABC") heard = s.count;
    CHECK(heard == 3, "%s: LY2ABC heard %d times", name, heard);
    CHECK(t.events.size() == 11, "%s: %zu events", name, t.events.size());
    if (noisy) {
        float b = 0; int n = 0;
        for (const auto& x : t.events) { b += x.ber; n++; }
        if (n) CHECK(b / n > 0.01 && b / n < 0.04, "%s: BER %.3f, should be near 0.02", name, b / n);
    }
}

int main() {
    const auto calls = aleTestCalls();
    checkAll(run(calls, 25, 0, 0, 1), "clean (25 dB)", 0, false);
    checkAll(run(calls, 10, 50, 0.02, 2), "10 dB, +50 Hz, 2 % bit errors", 50, true);
    checkAll(run(calls, 10, -50, 0.02, 3), "10 dB, -50 Hz, 2 % bit errors", -50, true);
    // noise only
    const AleTelemetry n = run({}, 10, 0, 0, 4, 30);
    printf("  noise only: %llu words, %zu events\n", (unsigned long long)n.blocksOk, n.events.size());
    CHECK(n.blocksOk == 0 && n.events.empty(), "noise only: %llu words", (unsigned long long)n.blocksOk);
    printf(fails ? "ale rx: %d FAILED\n" : "ale rx: all passed\n", fails);
    return fails ? 1 : 0;
}
