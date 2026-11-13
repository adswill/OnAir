// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HamSlotTx with a fake rig: gate, PTT order, audio produced, abort releases PTT, auto-sequence only advances after a real send.
#include "dect2/ham/ham.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2::ham;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

struct Fake : HamRigIo {
    std::vector<std::string> ev;
    long frames = 0; bool stall = false; std::atomic<bool> flushed{false};
    bool ptt(bool on, std::string&) override { ev.push_back(on ? "ptt1" : "ptt0"); return true; }
    bool beginAudio(int, std::string&) override { ev.push_back("begin"); return true; }
    int write(const float*, int n) override { if (ev.back() != "audio") ev.push_back("audio"); frames += n; return n; }
    int pending() override { return stall ? 1 : 0; }
    void flushAudio() override { flushed = true; }
    void endAudio() override { ev.push_back("end"); }
};
static HamTxCheck gate() { HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = 14.074e6; g.audioHz = 1500; return g; }
static void balanced(const Fake& f, int& ok) { int on = 0, off = 0; for (auto& e : f.ev) { on += e == "ptt1"; off += e == "ptt0"; } ok = on == off; }

int main() {
    std::string err;
    { Fake f; HamSlotTx t(f); HamTxCheck g = gate(); g.armed = false;
      CHECK(!t.send(g, 0, "CQ LY1ABC KO24", 1500, 0.5, err) && f.ev.empty() && f.frames == 0); }
    { Fake f; HamSlotTx t(f); HamTxCheck g = gate(); g.dialHz = 100e6;
      CHECK(t.send(g, 0, "CQ LY1ABC KO24", 1500, 0.5, err) && !f.ev.empty()); }   // 100 MHz: a warning, then it sends
    { Fake f; HamSlotTx t(f); t.send(gate(), 0, "this is not encodable at all, really", 1500, 0.5, err); int ok; balanced(f, ok); CHECK(ok); }
    for (int m = 0; m < 3; m++) {
        Fake f; HamSlotTx t(f);
        CHECK(t.send(gate(), m, "CQ LY1ABC KO24", 1500, 0.5, err));
        CHECK(f.ev.size() >= 5 && f.ev.front() == "begin" && f.ev[1] == "ptt1" && f.ev[2] == "audio" && f.ev[f.ev.size() - 2] == "ptt0" && f.ev.back() == "end");
        const double sec = f.frames / 48000.0;
        CHECK(m == 0 ? (sec > 12 && sec < 13.5) : m == 1 ? (sec > 4.5 && sec < 6) : (sec > 2 && sec < 3.75));
        CHECK(!t.busy());
    }
    { Fake f; f.stall = true; HamSlotTx t(f); bool ok = true;
      std::thread th([&] { ok = t.send(gate(), 0, "CQ LY1ABC KO24", 1500, 0.5, err); });
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      t.abort(); th.join();
      int b; balanced(f, b);
      CHECK(!ok && f.flushed && err == "halted" && f.ev.back() == "end" && b);
      int on = 0; for (auto& e : f.ev) on += e == "ptt1"; CHECK(on == 1); }
    // the engine advances only after a real send
    { QsoConfig c; c.myCall = "LY1ABC"; c.myGrid = "KO24"; QsoEngine e(c); e.startCq(0);
      Fake f; HamSlotTx t(f); HamTxCheck g = gate(); g.armed = false;
      const std::string m = e.messageForSlot(0);
      if (t.send(g, 0, m, 1500, 0.5, err)) e.txSent(0);
      CHECK(e.state() == QsoEngine::Cq && f.frames == 0);
      e.onDecode(1, "LY1ABC K1ABC FN42", -5, 0, 1000);
      CHECK(e.state() == QsoEngine::Tx2Report);
      g.armed = true;
      const std::string m2 = e.messageForSlot(15);
      if (t.send(g, 0, m2, 1500, 0.5, err)) e.txSent(15);
      CHECK(f.frames > 0); }
    printf(fails ? "ham_tx: %d FAILED\n" : "ham_tx: all passed\n", fails);
    return fails ? 1 : 0;
}
