// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham operation beyond FT8 / FT4 / FT2: JT65 (A B C), JT9 and JT4 QSOs and the WSPR beacon. Two engines talk to each other through the real
// transmitter (HamSlotTx into a recording fake rig, nothing is sent anywhere), the audio goes through the real decoder, the decoded text
// goes to the other engine. Also timing, the mode bandwidth in the TX gate, the beacon schedule and its limits.
#include "../hfdig/hfdig_wsjt_testutil.h"
#include "dect2/ham/ham.h"
#include <regex>
#include "../common/test_util.h"
#include "../common/test_long.h"
using namespace dect2;
using namespace dect2::ham;
using namespace wsjttest;

struct Rec : HamRigIo {   // records the audio of a burst; no radio, no sound card
    std::vector<float> a;
    int ptt1 = 0, ptt0 = 0;
    bool ptt(bool on, std::string&) override { (on ? ptt1 : ptt0)++; return true; }
    bool beginAudio(int, std::string&) override { a.clear(); return true; }
    int write(const float* m, int n) override { a.insert(a.end(), m, m + n); return n; }
    int pending() override { return 0; }
    void flushAudio() override {}
    void endAudio() override {}
};
static HamTxCheck gate(double dial = 14.076e6) {
    HamTxCheck g; g.licenceConfirmed = true; g.armed = true; g.call = "LY1ABC"; g.grid = "KO24"; g.dialHz = dial; g.audioHz = 1500; return g;
}
// send msg through HamSlotTx, then decode the recorded audio (48 kHz -> 8 kHz) as a receiver would; "" when it did not decode
static std::string overAir(int mode, int sub, const std::string& msg, double hz, float* snr = nullptr, double level = 0.4) {
    Rec r; HamSlotTx t(r); std::string err;
    if (!t.send(gate(), mode, msg, hz, level, err, sub)) { printf("send failed: %s (%s)\n", err.c_str(), msg.c_str()); return ""; }
    const double P = ftxPeriod(mode), t0 = wsjttest::t0Of(mode, 0);
    std::vector<float> a((size_t)((t0 + P + 4) * 8000), 0.f);
    static std::vector<double> h;   // 8 kHz out of 48 kHz: windowed sinc, cut-off 3.5 kHz
    if (h.empty()) {
        const int N = 96;
        for (int k = -N; k <= N; k++) {
            const double x = k * 2 * 3500.0 / 48000.0 * 3.141592653589793, w = 0.5 + 0.5 * std::cos(3.141592653589793 * k / (N + 1));
            h.push_back((k == 0 ? 1.0 : std::sin(x) / x) * w * 2 * 3500.0 / 48000.0);
        }
    }
    const long N = (long)(h.size() / 2);
    for (size_t o = 0; o < a.size(); o++) {
        const long c = (long)(o - (size_t)(t0 * 8000)) * 6;
        if (c < 0 && -c > N) continue;
        double acc = 0;
        for (long k = -N; k <= N; k++) { const long i = c + k; if (i >= 0 && i < (long)r.a.size()) acc += h[(size_t)(k + N)] * r.a[(size_t)i]; }
        a[o] = (float)acc;
    }
    ftxtest::Gauss g(7);
    for (auto& v : a) v += (float)(ftxtest::kSigma * g.g());
    Imp f; f.sub = sub;
    const auto d = decode(a, mode, f);
    if (d.empty()) return "";
    if (snr) *snr = d[0].snrDb;
    return d[0].msg;
}

// A (LY1ABC) calls CQ, B (K1ABC) answers it by "double click"; run until both are done.
static void qso(int mode, int sub) {
    const double P = slotPeriod(mode);
    QsoConfig ca; ca.myCall = "LY1ABC"; ca.myGrid = "KO24"; ca.mode = mode; ca.txEven = true; ca.hold = true;
    QsoConfig cb; cb.myCall = "K1ABC"; cb.myGrid = "FN42"; cb.mode = mode; cb.hold = true;
    QsoEngine A(ca), B(cb);
    int la = 0, lb = 0;
    std::string modeA, modeB;
    A.onLog = [&](const QsoRecord& q) { la++; modeA = q.mode; };
    B.onLog = [&](const QsoRecord& q) { lb++; modeB = q.mode; };
    const double base = ftxtest::kT0;
    A.startCq(base);
    std::vector<std::string> sent;
    for (int k = 0; k < 10 && !(la && lb && A.state() == QsoEngine::Cq && !B.active()); k++) {
        const double slot = base + k * P;
        QsoEngine* tx = nullptr; QsoEngine* rx = nullptr;
        std::string m = A.messageForSlot(slot);
        if (!m.empty()) { tx = &A; rx = &B; }
        else { m = B.messageForSlot(slot); if (!m.empty()) { tx = &B; rx = &A; } }
        if (!tx) continue;
        float snr = 0;
        const std::string got = overAir(mode, sub, m, 1500, &snr);
        CHECK(got == m, "%s slot %d: sent \"%s\" heard \"%s\"", hamModeName(mode), k, m.c_str(), got.c_str());
        sent.push_back(m);
        tx->txSent(slot);
        if (got.empty()) break;
        rx->onDecode(slot + P - 1, got, (int)std::lround(snr), slot, 1500);
        if (k == 0) B.callStation(slot + P - 1, "LY1ABC", "KO24", (int)std::lround(snr), slot, 1500);   // B clicks the CQ
    }
    const char* n = hamModeName(mode);
    CHECK(sent.size() == 6, "%s: %d transmissions", n, (int)sent.size());
    if (sent.size() == 6) {
        CHECK(sent[0] == "CQ LY1ABC KO24", "%s CQ \"%s\"", n, sent[0].c_str());
        CHECK(sent[1] == "LY1ABC K1ABC FN42", "%s grid \"%s\"", n, sent[1].c_str());
        CHECK(std::regex_match(sent[2], std::regex("K1ABC LY1ABC [+-]\\d\\d")), "%s report \"%s\"", n, sent[2].c_str());
        CHECK(std::regex_match(sent[3], std::regex("LY1ABC K1ABC R[+-]\\d\\d")), "%s R-report \"%s\"", n, sent[3].c_str());
        CHECK(sent[4] == "K1ABC LY1ABC RRR", "%s roger \"%s\"", n, sent[4].c_str());
        CHECK(sent[5] == "LY1ABC K1ABC 73", "%s 73 \"%s\"", n, sent[5].c_str());
    }
    CHECK(la == 1 && lb == 1, "%s: logged %d / %d", n, la, lb);
    CHECK(modeA == n && modeB == n, "%s: logged as %s / %s", n, modeA.c_str(), modeB.c_str());
    CHECK(A.state() == QsoEngine::Cq && !B.active(), "%s: A is back to calling CQ, B idle", n);
}

int main() {
    // timing and mode facts
    CHECK(slotPeriod(4) == 60 && slotPeriod(5) == 60 && slotPeriod(10) == 60 && slotPeriod(3) == 120 && slotPeriod(0) == 15, "periods");
    CHECK(txStartDelay(4) == 1.0 && txStartDelay(5) == 1.0 && txStartDelay(10) == 1.0 && txStartDelay(3) == 1.0 && txStartDelay(0) == 0.5, "start delays");
    CHECK(!strcmp(hamModeName(4), "JT65") && !strcmp(hamModeName(5), "JT9") && !strcmp(hamModeName(10), "JT4") && !strcmp(hamModeName(3), "WSPR"), "names");
    CHECK(hamModeHasQso(4) && hamModeHasQso(5) && hamModeHasQso(10) && !hamModeHasQso(3) && hamModeUsesRRR(4) && !hamModeUsesRRR(0), "mode flags");
    CHECK(hamModeBwHz(4, 2) > hamModeBwHz(4, 0) && hamModeBwHz(5, 0) < 30, "bandwidths");
    CHECK(msgRRR("k1abc", "ly1abc") == "K1ABC LY1ABC RRR" && parseMessage("K1ABC LY1ABC RRR").kind == ParsedMsg::Rrr, "RRR");

    // the QSOs, each through the transmitter, the audio and the decoder
    qso(4, 0);    // JT65A
    qso(5, 0);    // JT9
    qso(10, 0);   // JT4A
    // JT65 B and C at strong, medium and weak levels (the strong ones used to be missed: the sync search was fooled by leakage)
    for (int sub : {1, 2})
        for (double level : {0.4, 0.05, 0.01}) {
            if (level == 0.05 && !testLong()) continue;   // the middle level only in the long run: strong and weak are the ones that broke
            float snr = 0;
            const std::string got = overAir(4, sub, "CQ LY1ABC KO24", 1200, &snr, level);
            CHECK(got == "CQ LY1ABC KO24", "JT65 sub %d level %.2f heard \"%s\"", sub, level, got.c_str());
        }

    // the gate uses the width of the mode: JT65C near the top of 20 m goes out with a warning (it runs past the band edge), JT9 there is in band
    {
        Rec r; HamSlotTx t(r); std::string err;
        HamTxCheck g = gate(14.349e6);
        { std::string w; HamTxCheck gw = g; gw.bwHz = hamModeBwHz(4, 2); gw.audioHz = 500; CHECK(hamTxAllowed(gw, w) && w.find("not an amateur band") != std::string::npos, "JT65C over the band edge: warned (%s)", w.c_str()); }
        CHECK(t.send(g, 4, "CQ LY1ABC KO24", 500, 0.4, err, 2) && r.ptt1 == 1, "JT65C over the band edge: warned, sent: %s", err.c_str());
        CHECK(t.send(g, 5, "CQ LY1ABC KO24", 500, 0.4, err) && r.ptt1 == 2 && r.ptt0 == 2, "JT9 at the same place: %s", err.c_str());
        HamTxCheck g2 = gate(); g2.armed = false;
        Rec r2; HamSlotTx t2(r2);
        CHECK(!t2.send(g2, 4, "CQ LY1ABC KO24", 1500, 0.4, err) && r2.ptt1 == 0 && r2.a.empty(), "disarmed: nothing keyed");
    }

    // the WSPR beacon
    {
        WsprBeacon b;
        b.cfg.myCall = "LY1ABC"; b.cfg.myGrid = "KO24"; b.cfg.dbm = 10; b.cfg.everyNth = 5; b.cfg.maxTx = 3;
        CHECK(b.message() == "LY1ABC KO24 10", "message \"%s\"", b.message().c_str());
        CHECK(b.messageForSlot(ftxtest::kT0).empty(), "not started: nothing");
        b.start();
        int due = 0;
        for (int k = 0; k < 10; k++) { if (!b.messageForSlot(ftxtest::kT0 + 120.0 * k).empty()) due++; }
        CHECK(due == 2, "N = 5 over 10 slots: %d due", due);
        b.cfg.everyNth = 1;
        int duty = 0;
        for (int k = 0; k < 10; k++) { if (!b.messageForSlot(ftxtest::kT0 + 120.0 * k).empty()) duty++; }
        CHECK(duty == 10, "N = 1 is every slot (the operator's choice, 3.2): %d", duty);
        b.cfg.everyNth = 2;
        for (int i = 0; i < 3; i++) b.txSent();
        CHECK(b.messageForSlot(ftxtest::kT0).empty() && !b.active() && b.stopReason() == "maxtx", "stops after maxTx");
        b.start(); b.cfg.dbm = 11;
        CHECK(b.message().empty() && b.messageForSlot(ftxtest::kT0).empty(), "11 dBm is not a WSPR power");
        b.cfg.dbm = 37; b.cfg.myGrid = "KO";
        CHECK(b.message().empty(), "bad grid");
        b.cfg.myGrid = "KO24"; b.cfg.audioHz = 3000;
        CHECK(b.audioHz() == 1600, "audio clamped to the WSPR window");
        for (int d : {0, 3, 7, 10, 13, 17, 20, 60}) CHECK(wsprDbmValid(d), "dBm %d", d);
        for (int d : {1, 5, 11, 61, -3}) CHECK(!wsprDbmValid(d), "dBm %d", d);
        // over the air: the beacon message through the transmitter and the WSPR decoder
        b.cfg.dbm = 10; b.cfg.audioHz = 1500;
        const std::string got = overAir(3, 0, b.message(), b.audioHz());
        CHECK(got == "LY1ABC KO24 10", "WSPR heard \"%s\"", got.c_str());
    }
    return testSummary("ham_modes");
}
