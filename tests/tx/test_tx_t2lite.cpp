// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-T2-Lite in the transmit window (tx_params.cpp, profile / transmitter groups / 1.7 MHz rows; t2TxPlan in t2gen.cpp):
//  1. the panel's rows can only hold T2-Lite combinations EN 302 755 annex I allows: every combination of values, after txParamsFix (what the
//     panel does after each change), obeys the annex, checked against the annex tables written out again here (not t2profile.cpp's copy);
//     every allowed combination is left as it is (so each can be chosen), and switching back to T2-Base gives a valid T2-Base set;
//  2. every FFT / guard / pilot combination of tables I.5 and I.6, and every constellation / code rate / rotation of table I.4, makes a
//     transmitter generator (makeTxGenerator) whose plan keeps to 4 Mbit/s (I.10), 2^18 TI cells (I.8) and 250 ms frames (cl. 8.3.1);
//  3. round trips: the panel's values -> the transmitter's plan -> T2Generator -> T2Receiver, SISO and MISO, code rates 1/3 and 2/5, QPSK to
//     256-QAM, 1.7 and 8 MHz: the L1 says T2-Lite (S1 011 / 100, T2_BASE_LITE 0) and every baseband frame comes back identical; a T2-Base
//     signal inside the Lite rules signals T2_BASE_LITE 1;
//  4. makeTxGenerator at 1.7 MHz into a radio that cannot go below 2 Msps (resampled) still decodes.
// Simulated signals only: no real T2-Lite receiver or capture is involved.
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_params.h"
#include "dect2/t2/t2.h"
#include "dect2/t2/t2gen.h"
#include "dect2/t2/t2rx.h"
#include "dect2/common/stable_random.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;

static int failures = 0;
static void fail(const char* fmt, ...) {   // one printf with a constant format
    std::array<char, 512> b{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b.data(), b.size(), fmt, ap);
    va_end(ap);
    std::printf("FAIL: %s\n", b.data());
    failures++;
}
#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

using Num = std::map<std::string, double>;

// ---------------------------------------------------------------- annex I, written out from the PDF (EN 302 755 V1.4.1 pages 171-173)

// the panel's choice indexes: t2.fft 1K 2K 4K 8K 16K 32K; t2.gi 1/32 1/16 1/8 1/4 1/128 19/128 19/256; t2.pp PP1..PP8
static const char* const kGiName[] = {"1/32", "1/16", "1/8", "1/4", "1/128", "19/128", "19/256"};
// Tables I.5 (SISO) and I.6 (MISO) as printed: rows 16K, 8K, "4K, 2K"; columns 1/128 1/32 1/16 19/256 1/8 19/128 1/4; "" = NA
static const char* const kI5[3][7] = {
    {"PP7", "PP7 PP6", "PP4 PP5", "PP2 PP4 PP5", "PP2 PP3", "PP2 PP3", "PP1"},
    {"PP7", "PP7 PP4", "PP4 PP5", "PP4 PP5", "PP2 PP3", "PP2 PP3", "PP1"},
    {"", "PP7 PP4", "PP4 PP5", "", "PP2 PP3", "", "PP1"},
};
static const char* const kI6[3][7] = {
    {"PP4 PP5", "PP4", "PP3", "PP3", "PP1", "PP1", ""},
    {"PP4 PP5", "PP4 PP5", "PP3", "PP3", "PP1", "PP1", ""},
    {"", "PP4 PP5", "PP3", "", "PP1", "", ""},
};
static const char* const kPrintedGi[] = {"1/128", "1/32", "1/16", "19/256", "1/8", "19/128", "1/4"};

// is PP(pp+1) allowed in T2-Lite with FFT choice fft and guard choice gi?
static bool annexPilot(int fft, int gi, int pp, bool miso) {
    int row = -1;
    if (fft == 4) row = 0;            // 16K
    else if (fft == 3) row = 1;       // 8K
    else if (fft == 1 || fft == 2) row = 2;   // 2K, 4K
    if (row < 0) return false;        // 1K, 32K: I.7.1
    int col = -1;
    for (int c = 0; c < 7; c++) if (!strcmp(kPrintedGi[c], kGiName[gi])) col = c;
    const std::string cell = (miso ? kI6 : kI5)[row][col];
    const std::string name = "PP" + std::to_string(pp + 1);
    return pp != 7 && cell.find(name) != std::string::npos;   // PP8: I.7.2
}
// table I.1 (code rates), I.4 (256-QAM pairs and rotation), I.3 (16 200 frames only)
static bool annexOk(const Num& v, std::string* why = nullptr) {
    auto g = [&](const char* k) { return (int)std::lround(v.at(k)); };
    const int fft = g("t2.fft"), gi = g("t2.gi"), pp = g("t2.pp"), mod = g("t2.plpMod"), cod = g("t2.plpCod");
    const bool miso = g("t2.miso") == 1, rot = g("t2.rot") == 1, shortF = g("t2.short") == 1;
    std::string w;
    if (!annexPilot(fft, gi, pp, miso)) w = "FFT / guard / pilot";
    else if (cod == 4 || cod == 5) w = "code rate 4/5 or 5/6";
    else if (mod == 3 && (cod == 2 || cod == 3)) w = "256-QAM 2/3 or 3/4";
    else if (mod == 3 && rot) w = "rotated 256-QAM";
    else if (!shortF) w = "64 800-bit frames";
    if (why) *why = w;
    return w.empty();
}
static std::string describe(const Num& v) {
    std::string o;
    for (const char* k : {"t2.profile", "t2.miso", "t2.fft", "t2.gi", "t2.pp", "t2.plpMod", "t2.plpCod", "t2.rot", "t2.short"})
        o += std::string(k + 3) + "=" + std::to_string((int)std::lround(v.at(k))) + " ";
    return o;
}

// ---------------------------------------------------------------- 1. the panel model

static const TxModeSpec& t2Mode() { return *txModeById("dvbt2"); }
static const std::vector<TxParamDef>& rows() { static TxParamCtx c; return txParams(t2Mode(), c); }
static Num defaults() {
    Num n;
    for (const auto& d : rows()) if (!d.custom && d.type != TxPType::Text && d.type != TxPType::File) n[d.key] = d.def;
    return n;
}

static void testTable() {
    const auto& R = rows();
    const TxParamDef* prof = txParamFind(R, "t2.profile");
    const TxParamDef* miso = txParamFind(R, "t2.miso");
    const TxParamDef* bw = txParamFind(R, "dvb.bw");
    CHECK(prof && prof->choices.size() == 2 && prof->level == TxPLevel::Simple, "a Profile row (T2-Base / T2-Lite) in the Simple view");
    CHECK(miso && miso->choices.size() == 2, "a Transmitter groups row (SISO / MISO)");
    CHECK(bw && bw->choices.size() == 5 && bw->values.size() == 5 && bw->values[4] == 1.7, "DVB-T2 offers the 1.7 MHz channel");
    TxParamCtx c;
    const TxParamDef* tbw = txParamFind(txParams(*txModeById("dvbt"), c), "dvb.bw");
    CHECK(tbw && tbw->choices.size() == 4, "DVB-T does not offer 1.7 MHz");
    // the rows a rule depends on come before the rows it rules (txParamsFix goes in table order)
    auto pos = [&](const char* k) { for (size_t i = 0; i < R.size(); i++) if (!strcmp(R[i].key, k)) return (int)i; return -1; };
    CHECK(pos("t2.profile") < pos("t2.fft") && pos("t2.miso") < pos("t2.fft") && pos("t2.fft") < pos("t2.gi") && pos("t2.gi") < pos("t2.pp"), "row order");

    // the profile and MISO rows set S1
    for (int p = 0; p < 2; p++)
        for (int m = 0; m < 2; m++) {
            Num n = defaults();
            n["t2.profile"] = p; n["t2.miso"] = m;
            TxRequest r; r.modeId = "dvbt2";
            txParamsApplyNumbers(R, n, r);
            const int want = p ? (m ? 4 : 3) : (m ? 1 : 0);
            CHECK(r.synth.tx.s1 == want, "profile %d MISO %d: S1 %d, expected %d", p, m, r.synth.tx.s1, want);
            if (p) CHECK(r.synth.tx.plpShort && r.synth.tx.plpCod != 4 && r.synth.tx.plpCod != 5, "T2-Lite from the defaults: short frames, no 4/5 or 5/6");
        }
}

// every combination of the values that matter, fixed: inside the annex, and an allowed one is left untouched
static void testCombinations() {
    const auto& R = rows();
    long checked = 0, allowed = 0, moved = 0;
    std::vector<int> fftUsed(6, 0);
    Num base = defaults();
    base["t2.profile"] = 1;
    for (int mi = 0; mi < 2; mi++)
        for (int fft = 0; fft < 6; fft++)
            for (int gi = 0; gi < 7; gi++)
                for (int pp = 0; pp < 8; pp++)
                    for (int mod = 0; mod < 4; mod++)
                        for (int cod = 0; cod < 8; cod++)
                            for (int rot = 0; rot < 2; rot++)
                                for (int sh = 0; sh < 2; sh++) {
                                    // the full product is 86 016 sets; every FFT / guard / pilot with every 4th constellation set and
                                    // every constellation set with every 7th pilot set keeps it to about 15 000 and still covers each value
                                    const int pilotSet = (fft * 7 + gi) * 8 + pp, modSet = (mod * 8 + cod) * 4 + rot * 2 + sh;
                                    if (pilotSet % 7 != 0 && modSet % 4 != 0) continue;
                                    Num n = base;
                                    n["t2.miso"] = mi; n["t2.fft"] = fft; n["t2.gi"] = gi; n["t2.pp"] = pp;
                                    n["t2.plpMod"] = mod; n["t2.plpCod"] = cod; n["t2.rot"] = rot; n["t2.short"] = sh;
                                    const Num before = n;
                                    const bool ok0 = annexOk(n);
                                    txParamsFix(R, n);
                                    checked++;
                                    std::string why;
                                    if (!annexOk(n, &why)) { fail("after the fix still against annex I (%s): %s", why.c_str(), describe(n).c_str()); continue; }
                                    if (ok0) { allowed++; CHECK(n == before, "an allowed set was changed: %s -> %s", describe(before).c_str(), describe(n).c_str()); }
                                    else moved++;
                                    fftUsed[(size_t)n.at("t2.fft")]++;
                                    // no value the panel holds is greyed out
                                    for (const auto& d : R) if (d.block) {
                                        const auto it = n.find(d.key);
                                        if (it != n.end() && txParamBlocked(d, R, n, (int)std::lround(it->second))) { fail("%s is held although blocked: %s", d.key, describe(n).c_str()); break; }
                                    }
                                    // back to T2-Base: a valid Base set, the Lite-only rates gone
                                    Num b = n;
                                    b["t2.profile"] = 0;
                                    txParamsFix(R, b);
                                    const int cb = (int)b.at("t2.plpCod");
                                    CHECK(cb >= 0 && cb <= 5, "back to T2-Base keeps the Lite-only code rate %d", cb);
                                    for (const auto& d : R) if (d.block && txParamBlocked(d, R, b, (int)std::lround(b.at(d.key)))) { fail("T2-Base holds a blocked %s", d.key); break; }
                                }
    CHECK(fftUsed[0] == 0 && fftUsed[5] == 0, "T2-Lite never ends on 1K or 32K");
    CHECK(fftUsed[1] && fftUsed[2] && fftUsed[3] && fftUsed[4], "every T2-Lite FFT size can be held");
    std::printf("panel: %ld value sets fixed (%ld allowed as they were, %ld moved), all inside annex I\n", checked, allowed, moved);

    // every allowed pilot combination of tables I.5 / I.6 can be chosen: set it and nothing moves
    int combos = 0;
    for (int mi = 0; mi < 2; mi++)
        for (int fft = 0; fft < 6; fft++)
            for (int gi = 0; gi < 7; gi++)
                for (int pp = 0; pp < 8; pp++) {
                    if (!annexPilot(fft, gi, pp, mi == 1)) continue;
                    Num n = base;
                    n["t2.miso"] = mi; n["t2.fft"] = fft; n["t2.gi"] = gi; n["t2.pp"] = pp; n["t2.short"] = 1; n["t2.plpCod"] = 0;
                    const Num before = n;
                    txParamsFix(R, n);
                    CHECK(n == before, "table I.%d: FFT %d, guard %s, PP%d cannot be chosen", mi ? 6 : 5, fft, kGiName[gi], pp + 1);
                    combos++;
                }
    // tables I.5 / I.6 as counted from the PDF: SISO 2K 7, 4K 7, 8K 12, 16K 13 (39); MISO 2K 4, 4K 4, 8K 8, 16K 7 (23)
    CHECK(combos == 39 + 23, "%d allowed FFT / guard / pilot combinations, the tables have 62", combos);
}

// ---------------------------------------------------------------- 2. generators for every allowed combination

static TxRequest request(const Num& n, double bw) {
    TxRequest r;
    r.modeId = "dvbt2";
    Num m = n;   // the channel width is the panel's dvb.bw row: 8 7 6 5 1.7 MHz
    const double widths[] = {8, 7, 6, 5, 1.7};
    for (int i = 0; i < 5; i++) if (std::fabs(widths[i] - bw) < 1e-9) m["dvb.bw"] = i;
    txParamsApplyNumbers(rows(), m, r);
    CHECK(std::fabs(r.bandwidthMhz - bw) < 1e-9, "the dvb.bw row gives %.1f MHz, not %.1f", r.bandwidthMhz, bw);
    return r;
}

static void checkPlan(const Num& n, double bw, const char* what) {
    TxRequest r = request(n, bw);
    const T2TxPlan p = t2TxPlan(r.synth.tx, bw);
    const bool lite = r.synth.tx.s1 >= 3;
    CHECK(p.frameSec > 0 && p.frameSec <= 0.25 + 1e-9, "%s: frame %.1f ms", what, p.frameSec * 1e3);
    CHECK(p.blocks > 0, "%s: no FEC block in a frame", what);
    if (lite) {
        CHECK(p.plpBps <= 4e6 + 1, "%s: PLP %.3f Mbit/s, more than 4", what, p.plpBps / 1e6);
        CHECK(p.tsBps <= 4e6 + 1, "%s: TS %.3f Mbit/s, more than 4", what, p.tsBps / 1e6);
        const int ti = r.synth.tx.plpTi;
        PlpFec f; f.shortFrame = true; f.rate = r.synth.tx.plpCod; f.mod = r.synth.tx.plpMod;
        if (ti > 0) CHECK((long)((p.blocks + ti - 1) / ti) * fecDims(f).cellsPerBlock <= (1L << 18), "%s: a TI block over 2^18 cells", what);
        CHECK(!p.params.baseLite, "%s: a T2-Lite signal must not set T2_BASE_LITE", what);
    }
}

static void testGenerators() {
    int made = 0;
    std::string err;
    Num base = defaults();
    base["t2.profile"] = 1;
    base["t2.short"] = 1;
    base["t2.plpCod"] = 0;
    for (int mi = 0; mi < 2; mi++)
        for (int fft = 1; fft <= 4; fft++)
            for (int gi = 0; gi < 7; gi++)
                for (int pp = 0; pp < 7; pp++)
                    for (int ext = 0; ext < 2; ext++) {
                        if (!annexPilot(fft, gi, pp, mi == 1) || (ext && fft < 3)) continue;
                        Num n = base;
                        n["t2.miso"] = mi; n["t2.fft"] = fft; n["t2.gi"] = gi; n["t2.pp"] = pp; n["t2.ext"] = ext;
                        const double bw = (made % 2) ? 1.7 : 8;
                        std::array<char, 96> wb{};
                        snprintf(wb.data(), wb.size(), "%s FFT %d guard %s PP%d%s %.1f MHz", mi ? "MISO" : "SISO", fft, kGiName[gi], pp + 1, ext ? " ext" : "", bw);
                        const char* what = wb.data();
                        TxRequest r = request(n, bw);
                        auto g = makeTxGenerator(t2Mode(), r, 0, err);
                        CHECK(g != nullptr, "%s: no generator: %s", what, err.c_str());
                        if (g) CHECK(std::fabs(g->sampleRate() - nativeRateHz(bw)) < 1, "%s: %.0f Hz", what, g->sampleRate());
                        checkPlan(n, bw, what);
                        made++;
                    }
    for (int mod = 0; mod < 4; mod++)
        for (int cod : {6, 7, 0, 1, 2, 3})
            for (int rot = 0; rot < 2; rot++)
                for (double bw : {8.0, 1.7}) {
                    Num n = base;
                    n["t2.plpMod"] = mod; n["t2.plpCod"] = cod; n["t2.rot"] = rot;
                    if (!annexOk(n)) continue;
                    std::array<char, 96> wb{};
                    snprintf(wb.data(), wb.size(), "constellation %d code rate %d rotation %d %.1f MHz", mod, cod, rot, bw);
                    const char* what = wb.data();
                    TxRequest r = request(n, bw);
                    auto g = makeTxGenerator(t2Mode(), r, 0, err);
                    CHECK(g != nullptr, "%s: no generator: %s", what, err.c_str());
                    checkPlan(n, bw, what);
                    made++;
                }
    // the 4 Mbit/s cap at work: 16K QPSK 2/5 in 8 MHz would carry about 5.6 Mbit/s
    Num big = base;
    big["t2.fft"] = 4; big["t2.gi"] = 0; big["t2.pp"] = 6; big["t2.plpMod"] = 0; big["t2.plpCod"] = 7;
    TxRequest rb = request(big, 8);
    const T2TxPlan pb = t2TxPlan(rb.synth.tx, 8);
    CHECK(pb.capped && pb.fitBps > 4e6 && pb.plpBps <= 4e6 && pb.plpBps > 3.8e6, "16K QPSK 2/5 8 MHz: %.2f Mbit/s would fit, %.2f sent", pb.fitBps / 1e6, pb.plpBps / 1e6);
    bool warned = false;
    for (const auto& w : txParamWarnings(t2Mode(), rb)) warned |= w.find("4 Mbit/s") != std::string::npos && w.find("TS mux rate") != std::string::npos;
    CHECK(warned, "the panel warns about the 4 Mbit/s cap with the TS mux rate to use");
    // 1.7 MHz: the automatic frame would be about 300 ms
    Num narrow = base;
    TxRequest rn = request(narrow, 1.7);
    const T2TxPlan pn = t2TxPlan(rn.synth.tx, 1.7);
    CHECK(pn.shortened && pn.frameSec <= 0.25, "1.7 MHz: frame %.0f ms (shortened %d)", pn.frameSec * 1e3, (int)pn.shortened);
    std::printf("generators: %d T2-Lite configurations made\n", made);
}

// ---------------------------------------------------------------- 3. round trips through the receiver

struct Trip {
    const char* name;
    int profile, miso, fft, gi, pp, mod, cod, rot, ti;
    double bw;
    bool expectBaseLite = false;
};

static void roundTrip(const Trip& c) {
    Num n = defaults();
    n["t2.profile"] = c.profile; n["t2.miso"] = c.miso; n["t2.fft"] = c.fft; n["t2.gi"] = c.gi; n["t2.pp"] = c.pp;
    n["t2.plpMod"] = c.mod; n["t2.plpCod"] = c.cod; n["t2.rot"] = c.rot; n["t2.short"] = 1; n["t2.ti"] = c.ti;
    const Num asked = n;
    txParamsFix(rows(), n);
    CHECK(n == asked, "%s: the panel would not hold this set (%s)", c.name, describe(n).c_str());
    const TxRequest r = request(n, c.bw);
    const T2TxPlan plan = t2TxPlan(r.synth.tx, c.bw);   // what makeTxGenerator uses
    T2Generator gen(plan.params);
    const double fs = nativeRateHz(c.bw);
    T2Receiver rx;
    rx.configure(fs, c.bw);
    std::mutex mu;
    std::vector<PlpResult> res;
    rx.setPlpCallback([&](const PlpResult& p) { std::lock_guard<std::mutex> k(mu); res.push_back(p); });
    std::map<int, std::vector<std::vector<uint8_t>>> sent;
    std::mt19937 rng(11);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    std::vector<cf32> f, y;
    const int frames = c.bw < 2 ? 8 : 12;
    for (int i = 0; i < frames; i++) {
        gen.nextFrame(f);
        sent[i & 0xff] = gen.lastBbFrames();
        y = f;
        if (c.miso) {   // both transmitter groups, the second a little weaker and delayed by a few samples
            const auto& f2 = gen.lastTx2();
            for (size_t k = 5; k < y.size(); k++) y[k] += std::polar(0.7f, 0.9f) * f2[k - 5];
        }
        double pw = 0;
        for (auto& v : y) pw += std::norm(v);
        pw /= y.size();
        const float sg = (float)std::sqrt(pw / std::pow(10.0, 30 / 10.0) / 2);
        for (auto& v : y) v += cf32{nd(rng), nd(rng)} * sg;
        for (size_t o = 0; o < y.size(); o += 1 << 16) rx.feed(y.data() + o, std::min<size_t>(1 << 16, y.size() - o));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    RxTelemetry t;
    uint64_t q = 0;
    size_t seen = 0;
    for (int i = 0, still = 0; i < 1500 && still < 300; i++) {
        rx.telemetry(t, q);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        size_t k;
        { std::lock_guard<std::mutex> l(mu); k = res.size(); }
        still = k == seen ? still + 1 : 0;
        seen = k;
    }
    int fr = 0, same = 0, bad = 0;
    {
        std::lock_guard<std::mutex> l(mu);
        for (auto& p : res) {
            auto it = sent.find(p.t2Frame);
            if (it == sent.end() || !p.blocks) continue;
            fr++;
            for (size_t k = 0; k < p.frames.size() && k < it->second.size(); k++) {
                if (p.frames[k].bits.empty()) continue;
                (p.frames[k].bits == it->second[k] ? same : bad)++;
            }
        }
    }
    const int wantS1 = c.profile ? (c.miso ? 4 : 3) : (c.miso ? 1 : 0);
    std::printf("  %-44s %5.2f Mbit/s%s, %d frames, %d BB frames identical, %d wrong, S1 %d, T2_BASE_LITE %d\n", c.name, plan.plpBps / 1e6,
                plan.capped ? " (capped)" : "", fr, same, bad, t.l1preOk ? t.l1pre.s1 : -1, t.l1preOk ? t.l1pre.lite : -1);
    CHECK(t.l1preOk, "%s: no L1-pre", c.name);
    CHECK(t.l1preOk && t.l1pre.s1 == wantS1, "%s: L1-pre S1 %d, expected %d", c.name, t.l1pre.s1, wantS1);
    CHECK(t.l1preOk && t.l1pre.lite == (c.expectBaseLite ? 1 : 0), "%s: T2_BASE_LITE %d, expected %d", c.name, t.l1pre.lite, (int)c.expectBaseLite);
    CHECK(fr >= frames / 2 && same >= fr && bad == 0, "%s: %d frames, %d identical, %d wrong", c.name, fr, same, bad);
}

static void testRoundTrips() {
    // fft 1 2K, 2 4K, 3 8K, 4 16K; gi 0 1/32, 1 1/16, 2 1/8, 3 1/4, 4 1/128, 5 19/128, 6 19/256; pp 0 = PP1; mod 0 QPSK .. 3 256-QAM; cod 6 1/3, 7 2/5
    const Trip trips[] = {
        {"Lite SISO 8 MHz 8K 1/8 PP2 QPSK 1/3", 1, 0, 3, 2, 1, 0, 6, 1, 0, 8},
        {"Lite SISO 8 MHz 16K 1/32 PP7 QPSK 2/5 (cap)", 1, 0, 4, 0, 6, 0, 7, 1, 1, 8},
        {"Lite SISO 8 MHz 4K 1/16 PP4 16-QAM 1/3", 1, 0, 2, 1, 3, 1, 6, 1, 1, 8},
        {"Lite SISO 8 MHz 16K 19/256 PP2 256-QAM 3/5", 1, 0, 4, 6, 1, 3, 1, 0, 1, 8},
        {"Lite SISO 1.7 MHz 8K 1/4 PP1 16-QAM 2/5", 1, 0, 3, 3, 0, 1, 7, 1, 1, 1.7},
        {"Lite SISO 1.7 MHz 2K 1/32 PP7 256-QAM 1/3", 1, 0, 1, 0, 6, 3, 6, 0, 0, 1.7},
        {"Lite SISO 1.7 MHz 16K 1/128 PP7 64-QAM 2/5", 1, 0, 4, 4, 6, 2, 7, 1, 1, 1.7},
        {"Lite MISO 8 MHz 8K 1/8 PP1 QPSK 1/3", 1, 1, 3, 2, 0, 0, 6, 1, 0, 8},
        {"Lite MISO 8 MHz 2K 1/32 PP4 64-QAM 2/5", 1, 1, 1, 0, 3, 2, 7, 0, 1, 8},
        {"Lite MISO 1.7 MHz 16K 1/128 PP5 16-QAM 2/5", 1, 1, 4, 4, 4, 1, 7, 1, 1, 1.7},
        {"Lite MISO 1.7 MHz 8K 1/16 PP3 256-QAM 1/3", 1, 1, 3, 1, 2, 3, 6, 0, 0, 1.7},
        // T2-Base within every Lite rule (1.7 MHz keeps it under 4 Mbit/s): T2_BASE_LITE 1; the 8 MHz default carries far more: 0
        // (with 2 TI blocks: with 1, the ~42 QPSK blocks of a frame are 340 000 cells, over the 2^18 of I.8, and the bit is rightly 0)
        {"Base 1.7 MHz 8K 1/128 PP7 QPSK 1/2 short", 0, 0, 3, 4, 6, 0, 0, 1, 2, 1.7, true},
        {"Base 8 MHz 8K 1/128 PP7 64-QAM 2/3 short", 0, 0, 3, 4, 6, 2, 2, 1, 3, 8, false},
    };
    for (const Trip& t : trips) roundTrip(t);
}

// ---------------------------------------------------------------- 4. the transmitter at 1.7 MHz into a radio that needs 2 Msps

static void testResampled() {
    Num n = defaults();
    n["t2.profile"] = 1; n["t2.fft"] = 3; n["t2.gi"] = 2; n["t2.pp"] = 1; n["t2.plpMod"] = 1; n["t2.plpCod"] = 6; n["t2.short"] = 1; n["t2.ti"] = 2;   // (1 TI block: 70 x 4050 cells, over 2^18, capped)
    txParamsFix(rows(), n);
    TxRequest r = request(n, 1.7);
    TxRateLimits lim;
    lim.minHz = 2e6;
    const double rate = txRequestRate(t2Mode(), r, lim);
    CHECK(std::fabs(rate - 2e6) < 1, "1.7 MHz into a radio from 2 Msps: asks %.0f Hz", rate);
    std::string err;
    auto g = makeTxGenerator(t2Mode(), r, rate, err);
    CHECK(g != nullptr, "1.7 MHz resampled to 2 Msps: no generator: %s", err.c_str());
    if (!g) return;
    CHECK(r.fileNote.find("T2-Lite") == std::string::npos, "an uncapped T2-Lite signal has no cap note: %s", r.fileNote.c_str());
    T2Receiver rx;
    rx.configure(rate, 1.7);
    std::mutex mu;
    int blocksOk = 0, blocksBad = 0;
    rx.setPlpCallback([&](const PlpResult& p) { std::lock_guard<std::mutex> k(mu); blocksOk += p.blocksOk; blocksBad += p.bchFailed; });
    std::vector<cf32> buf(1 << 16);
    const size_t total = (size_t)(2.5 * rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        g->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    RxTelemetry t;
    uint64_t q = 0;
    for (int i = 0; i < 300; i++) { rx.telemetry(t, q); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    std::lock_guard<std::mutex> k(mu);
    std::printf("  makeTxGenerator 1.7 MHz at 2 Msps: L1 %d, S1 %d, %d blocks decoded, %d failed\n", (int)t.l1preOk, t.l1preOk ? t.l1pre.s1 : -1, blocksOk, blocksBad);
    CHECK(t.l1preOk && t.l1pre.s1 == 3, "resampled 1.7 MHz T2-Lite: L1 %d S1 %d", (int)t.l1preOk, t.l1pre.s1);
    CHECK(blocksOk > 20 && blocksBad == 0, "resampled 1.7 MHz T2-Lite: %d blocks, %d failed", blocksOk, blocksBad);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const bool quick = argc > 1 && !strcmp(argv[1], "--table");   // the table checks only
    testTable();
    testCombinations();
    testGenerators();
    if (!quick) {
        testRoundTrips();
        testResampled();
    }
    if (failures) { std::printf("tx t2lite: %d failure(s)\n", failures); return 1; }
    std::printf("tx t2lite: all checks passed\n");
    return 0;
}
