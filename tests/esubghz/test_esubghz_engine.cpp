// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat through the engine (setStandard(206)): the synthetic source plays the test conversation, the key is set through
// engine.esubghz(), and the receiver reports plain and decrypted lines through latestRx().
#include "dect2/esubghz/esubghz_gen.h"
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/esubghz/esubghz_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = esubghzTuning();
    CHECK(mt.stdMode == 206 && modeTuningById("esubghz") && modeTuningById("esubghz")->stdMode == 206 && modeTuning(206), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 206;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(206);
    e.esubghz().setKey(esubghz::keyFromPassword(kEsubghzTestPassword));
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 205, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 9.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 205) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e.esubghz().noteSent("OnAir: a line sent", true, 58);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    while (e.latestRx(rx, seq)) seq = rx.seq;
    const EsubghzTelemetry& m = rx.esubghz;
    printf("  %d reports: %s\n", reports, esubghzSummary(m).c_str());
    for (const auto& c : m.chat) printf("    [%d %d] %s\n", c.dir, c.kind, c.text.c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    int plain = 0, dec = 0;
    bool longOk = false, sentOk = false;
    for (const auto& c : m.chat) {
        if (c.dir == 0 && c.kind == 0) plain++;
        if (c.dir == 0 && c.kind == 1) { dec++; if (c.packets >= 2 && c.text.find("more than one packet") != std::string::npos) longOk = true; }
        if (c.dir == 1 && c.text == "OnAir: a line sent") sentOk = true;
    }
    CHECK(plain >= 4 && dec >= 2 && longOk, "%d plain, %d decrypted, long line %d", plain, dec, (int)longOk);
    CHECK(sentOk && m.sent == 1, "the sent line is in the chat");
    CHECK(m.replays == 0 && m.unreadable == 0, "%llu replays, %llu unreadable", (unsigned long long)m.replays, (unsigned long long)m.unreadable);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("esubghz engine");
}
