// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The safety layer of the ham transmitter: the country band plans, the gate on every path (licence x Arm x band: only all three key anything),
// the duty-cycle and burst figures (warnings only), no stall stop, the hard stop on an output error, the TX audit log and the slot wait. Fake radios only.
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_safety.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>
#include "../common/test_util.h"
using namespace dect2::ham;

namespace {
struct Fake : HamRigIo {
    int prepares = 0, ptt1 = 0, ptt0 = 0, writes = 0;
    long frames = 0;
    bool refusePrepare = false, failWrite = false, stall = false;
    double preparedHz = 0;
    bool prepare(const HamTxCheck& g, std::string& err) override { prepares++; preparedHz = g.dialHz; if (refusePrepare) { err = "radio says no"; return false; } return true; }
    bool ptt(bool on, std::string&) override { (on ? ptt1 : ptt0)++; return true; }
    bool beginAudio(int, std::string&) override { return true; }
    int write(const float*, int n) override {
        writes++;
        if (failWrite && writes > 3) return -1;
        if (stall) return 0;
        frames += n;
        return n;
    }
    int pending() override { return 0; }
    void flushAudio() override {}
    void endAudio() override {}
    std::string lastError() const override { return "sound card unplugged"; }
    const char* kind() const override { return "test"; }
};
HamTxCheck gate(double dial = 14.074e6) {
    HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = dial; g.audioHz = 1500; return g;
}
struct FakeClock { double t = 1767225600; double operator()() const { return t; } };
}  // namespace

int main() {
    std::string why;
    // ---- country band plans ----
    {
        const auto lt = hamCountryBands("lt");
        CHECK(hamCountryKnown("LT") && hamCountryPlanName("LT") == "Lithuania", "Lithuania is known: %s", hamCountryPlanName("LT").c_str());
        CHECK(!lt.empty(), "Lithuania has amateur bands");
        bool b40 = false;
        for (const auto& b : lt) if (b.loHz <= 7.0e6 && b.hiHz >= 7.2e6) b40 = b.hiHz <= 7.2e6 + 1;
        CHECK(b40, "Region 1: 40 m ends at 7.2 MHz");
        CHECK(hamSignalInCountryBand("LT", 14.074e6, 1500, 50, why), "LT 20 m FT8 inside (%s)", why.c_str());
        CHECK(!hamSignalInCountryBand("LT", 7.25e6, 1500, 50, why) && why.find("Lithuania") != std::string::npos, "LT 7.25 MHz outside: %s", why.c_str());
        CHECK(hamSignalInCountryBand("US", 7.25e6, 1500, 50, why), "US 7.25 MHz inside (%s)", why.c_str());
        CHECK(!hamSignalInCountryBand("LT", 3.85e6, 1500, 50, why), "LT 3.85 MHz outside (80 m is 3.5 .. 3.8 in Region 1)");
        CHECK(hamSignalInCountryBand("US", 3.85e6, 1500, 50, why), "US 3.85 MHz inside");
        CHECK(!hamSignalInCountryBand("XX", 14.074e6, 1500, 50, why) && why.find("no band plan") != std::string::npos, "unknown country refused: %s", why.c_str());
        CHECK(!hamSignalInCountryBand("AE", 14.074e6, 1500, 50, why), "a plan without amateur bands refuses: %s", why.c_str());
        CHECK(hamSignalInCountryBand("eu", 14.074e6, 1500, 50, why), "a plan id works too");
        for (const char* c : {"DE", "GB", "US", "AU", "JP", "LT"})
            for (const auto& b : hamCountryBands(c)) CHECK(hamBandAt((b.loHz + b.hiHz) / 2) != nullptr, "%s %s lies inside the ITU table", c, b.name.c_str());
        HamTxCheck g = gate(7.25e6);
        CHECK(hamTxAllowed(g, why) && why.empty(), "no country: the ITU union allows 7.25 MHz, no warning");
        g.country = "LT";
        CHECK(hamTxAllowed(g, why) && why.find("Lithuania") != std::string::npos && why.find("responsible") != std::string::npos, "with the country: a warning, not a refusal (%s)", why.c_str());
        g = gate(); g.dialHz = std::nan("");
        CHECK(!hamTxAllowed(g, why), "NaN frequency refused");
        g = gate(); g.bwHz = std::nan("");
        CHECK(!hamTxAllowed(g, why), "NaN bandwidth refused");
        g = gate(); g.bwHz = -100;
        CHECK(!hamTxAllowed(g, why), "negative bandwidth refused");
    }
    // ---- the gate on both HamSlotTx paths: only Arm keys; no licence or a band outside the plan is a warning, the burst goes out ----
    for (int path = 0; path < 2; path++) {
        int keyed = 0;
        for (int m = 0; m < 8; m++) {
            Fake f; HamSlotTx tx(f);
            HamTxCheck g = gate(m & 4 ? 14.074e6 : 10.2e6);   // 10.2 MHz: above 30 m
            g.licenceConfirmed = m & 1; g.armed = m & 2; g.country = "LT";
            std::string err;
            const bool ok = path == 0 ? tx.send(g, 2, "CQ LY1ABC KO24", 1500, 0.5, err)
                                      : tx.sendAudio(g, std::vector<float>(48000, 0.3f), 100, 0.5, err, "PSK31");
            const bool arm = (m & 2) != 0;
            CHECK(ok == arm && (f.ptt1 > 0) == arm && (f.prepares > 0) == arm && (f.frames > 0) == arm,
                  "path %d licence %d arm %d band %d: sent %d, prepared %d, keyed %d", path, m & 1, (m >> 1) & 1, (m >> 2) & 1, ok, f.prepares, f.ptt1);
            keyed += f.ptt1;
        }
        CHECK(keyed == 4, "path %d: keyed in the 4 armed combinations of 8", path);
    }
    {   // the radio's prepare() sees the checked frequency; a refusal keys nothing
        Fake f; HamSlotTx tx(f); std::string err;
        CHECK(tx.send(gate(14.0745e6), 0, "CQ LY1ABC KO24", 1500, 0.5, err) && f.preparedHz == 14.0745e6, "prepare got %.0f", f.preparedHz);
        Fake r; r.refusePrepare = true; HamSlotTx t2(r);
        CHECK(!t2.send(gate(), 0, "CQ LY1ABC KO24", 1500, 0.5, err) && err == "radio says no" && r.ptt1 == 0 && r.frames == 0, "prepare refused: nothing keyed");
    }
    // ---- hard stop ----
    {
        Fake f; f.failWrite = true; HamSlotTx tx(f); std::string err;
        CHECK(!tx.send(gate(), 0, "CQ LY1ABC KO24", 1500, 0.5, err) && err == "sound card unplugged" && f.ptt1 == 1 && f.ptt0 == 1, "output error: stopped, PTT released (%s)", err.c_str());
    }
    {   // no stall stop (3.2): an output that takes no audio keeps the burst going until the operator halts it
        Fake f; f.stall = true; HamSlotTx tx(f); std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        std::thread halt([&] { std::this_thread::sleep_for(std::chrono::milliseconds(3600)); tx.abort(); });
        const bool ok = tx.send(gate(), 2, "CQ LY1ABC KO24", 1500, 0.5, err);
        halt.join();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!ok && err.find("stalled") == std::string::npos && s > 3.5 && f.ptt0 >= 1, "a stalled output is not stopped by OnAir: only Halt ended it after %.1f s (%s)", s, err.c_str());
    }
    // ---- no burst-length and no duty-cycle limit (3.2): the figures are only warnings in the audit log ----
    {
        Fake f; HamSlotTx tx(f); std::string err;
        txAuditClear();
        CHECK(tx.sendAudio(gate(), std::vector<float>(48000 * 201, 0.3f), 100, 0.5, err) && f.ptt1 == 1, "a 201 s burst is sent (%s)", err.c_str());
        const auto log = txAuditRecent(5);
        CHECK(!log.empty() && log.back().result == "sent" && log.back().reason.find("long burst") != std::string::npos, "and the audit log carries the warning");
    }
    {   // FT8 every slot (a runaway sequencer) goes out; the audit log warns about the duty cycle
        Fake f; HamSlotTx tx(f); FakeClock c; tx.setClock(std::ref(c)); std::string err;
        int sent = 0;
        bool warned = false;
        txAuditClear();
        for (int s = 0; s < 40; s++) { c.t += 15; if (tx.send(gate(), 0, "CQ LY1ABC KO24", 1500, 0.5, err)) sent++; }
        for (const auto& e : txAuditRecent(100)) warned |= e.reason.find("Duty cycle") != std::string::npos;
        CHECK(sent == 40, "every slot: all %d sent (%s)", sent, err.c_str());
        CHECK(warned, "the duty-cycle warning is in the audit log");
        CHECK(tx.onAirSec(0) > 0.6 * 600, "on air %.0f s of 600 (accounted, not limited)", tx.onAirSec(0));
        CHECK(hamDutyLimit(-1).maxFraction == 0.5 && hamDutyLimit(0).maxBurstSec == 15 && hamDutyLimit(3).windowSec == 1200, "the usual figures per mode (for the warnings)");
    }
    // ---- the audit log ----
    {
        txAuditClear();
        const std::string path = "onair_test_tx_audit_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()) + ".log";
        txAuditSetFile(path);
        Fake f; HamSlotTx tx(f); std::string err;
        HamTxCheck g = gate(); g.armed = false;
        tx.send(g, 0, "CQ LY1ABC KO24", 1500, 0.5, err);
        tx.send(gate(), 0, "CQ LY1ABC KO24", 1500, 0.5, err);
        tx.sendAudio(gate(), std::vector<float>(4800, 0.2f), 100, 0.5, err, "RTTY");
        const auto a = txAuditRecent();
        CHECK(a.size() == 3, "three attempts logged: %zu", a.size());
        if (a.size() == 3) {
            CHECK(a[0].result == "refused" && a[0].reason.find("armed") != std::string::npos && a[0].message == "CQ LY1ABC KO24", "refusal logged: %s", txAuditLine(a[0]).c_str());
            CHECK(a[1].result == "sent" && a[1].mode == "FT8" && a[1].path == "test" && a[1].seconds > 12 && a[1].dialHz == 14.074e6, "send logged: %s", txAuditLine(a[1]).c_str());
            CHECK(a[2].mode == "RTTY" && a[2].result == "sent", "audio send logged");
        }
        FILE* fp = fopen(path.c_str(), "rb");
        std::string text;
        if (fp) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, fp)) > 0) text.append(b, n); fclose(fp); }
        int lines = 0;
        for (char ch : text) lines += ch == '\n';
        CHECK(lines == 3 && text.find("FT8 sent 14074000+1500 Hz") != std::string::npos && text.find("LY1ABC") != std::string::npos, "the audit file: %s", text.c_str());
        txAuditSetFile("");
        std::remove(path.c_str());
        CHECK(txAuditClean("A\nB\"C\x01") == "A B'C", "control characters and quotes cleaned");
        CHECK(txAuditClean(std::string(500, 'X')).size() == 160, "long text cut");
    }
    // ---- the slot wait ----
    {
        struct Lat : Fake { double latencySec() const override { return 0.5; } } f;
        HamSlotTx tx(f); std::string err;
        const double t0 = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        CHECK(tx.waitForSlot(t0 + 0.6, err), "wait: %s", err.c_str());
        const double t1 = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        CHECK(t1 - t0 > 0.05 && t1 - t0 < 0.3, "started %.3f s after asking, the latency taken off (slot 0.6 s ahead, 0.5 s latency)", t1 - t0);
        CHECK(!tx.waitForSlot(t0 - 10, err) && err.find("late") != std::string::npos, "a missed slot: %s", err.c_str());
        bool ok = true;
        std::thread th([&] { std::string e; ok = tx.waitForSlot(t1 + 30, e); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        tx.abort(); th.join();
        CHECK(!ok, "Halt ends the wait");
    }
    return testSummary("ham_safety");
}
