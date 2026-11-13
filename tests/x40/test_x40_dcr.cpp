// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DCR (Japan's digital convenience radio, ARIB STD-T98 volume 3, 4-level FSK). What is checked:
//  1. the channel plan of the standard (Tables 3-4 to 3-6): counts, grid, the calling channel;
//  2. the standard's own interoperability test frames (chapter 7, whitened bits in hexadecimal: synchronous burst with the call sign, 24 audio frames, the end frame;
//     user codes 1 and 511, privacy-scrambled call) through the radio receiver: user code, call type, call sign, end of call, voice frames counted;
//  3. frames built from the field definitions (all user codes of a sweep, manufacturer numbers, normal / scrambled calls, multi-unit manufacturer messages, data
//     blocks) through the receiver with impairments: carrier offset, off-centre tuning, symbol clock error, reversed polarity, deviation error, sample rates of 48 kHz to 1 Msps;
//  4. a noise sweep (calls found; wrong calls counted) and noise / carrier / tone signals that must give nothing;
//  5. scan: three channels of the 351 MHz plan at different levels in one 500 ksps capture, the false-start tests (noise, a carrier on a channel).
// Simulated signals only, and the standard's test frames: no recording of a real DCR radio was available.
#include "x40_gen_dcr.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

using namespace dcrgen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// one transmission: SB0, n audio frames, the end frame
struct Tx { int uc, mfr, comm; std::string csm; int n; };
static Syms txSyms(const Tx& t) {
    FrameGen g; g.rng = 777 + (uint32_t)t.uc;
    g.sb0(t.comm, t.uc, t.mfr, t.csm); g.audio(t.comm, t.uc, t.mfr, t.n); g.end(t.comm, t.uc, t.mfr);
    return g.s;
}
static std::vector<Tx> sweep(int count, unsigned seed) {
    std::vector<Tx> v;
    const int ucs[] = {1, 0, 511, 256, 255, 100, 2, 384, 77, 5, 9, 33};
    const int mfrs[] = {0, 5, 127, 64, 0, 33, 1, 0};
    unsigned r = seed;
    for (int i = 0; i < count; i++) {
        r = r * 1664525u + 1013904223u;
        Tx t; t.uc = ucs[i % 12]; t.mfr = mfrs[i % 8]; t.comm = i % 3 == 2 ? 1 : 0; t.n = 6;
        t.csm = ""; for (int k = 0; k < 9; k++) { t.csm += (char)('0' + (int)((r >> (k + 4)) % 10)); r = r * 22695477u + 1u; }
        v.push_back(t);
    }
    return v;
}
static std::vector<Seg> segsOf(const std::vector<Tx>& txs, double gap = 0.6) {
    std::vector<Seg> s;
    for (const Tx& t : txs) { Seg g; g.sym = txSyms(t); g.gapSec = gap; s.push_back(g); }
    return s;
}
// the calls found, in the order sent: "UC n" ids of the Call rows (rows come newest first); `label` is the scan prefix ("" in single-channel mode)
static std::vector<std::string> callIds(const X40Telemetry& t, const std::string& label = "") {
    std::vector<std::string> v;
    for (auto it = t.rows.rbegin(); it != t.rows.rend(); ++it) if (it->kind == "Call" && (label.empty() ? it->id.rfind("UC ", 0) == 0 : it->id.rfind(label + " UC ", 0) == 0)) v.push_back(it->id);
    return v;
}
// how many of the expected calls were found in order, and how many calls were not expected
static void score(const X40Telemetry& t, const std::vector<Tx>& txs, int& found, int& wrong, const std::string& label = "") {
    const std::vector<std::string> got = callIds(t, label);
    found = 0; wrong = 0;
    size_t gi = 0;
    for (const Tx& x : txs) {
        const std::string want = (label.empty() ? std::string() : label + " ") + "UC " + std::to_string(x.uc);
        for (size_t k = gi; k < got.size(); k++) if (got[k] == want) { found++; gi = k + 1; break; }
    }
    wrong = std::max(0, (int)got.size() - found);
}

int main() {
    // ---- 1. the plan
    {
        const auto& p = dcr::plan();
        CHECK(dcr::channelsOfBand(351).size() == 97, "351 MHz plan has %zu channels (82 + 15 = 97)", dcr::channelsOfBand(351).size());
        auto hz = [&](const char* l) { for (auto& c : p) if (c.label == l) return c.hz; return -1.0; };
        CHECK(std::fabs(hz("351 MHz ch 1") - 351.2e6) < 1, "ch 1");
        CHECK(std::fabs(hz("351 MHz ch 15") - 351.2875e6) < 1, "calling channel 15 is 351.2875 MHz (%.1f)", hz("351 MHz ch 15"));
        CHECK(std::fabs(hz("351 MHz ch 70") - 351.63125e6) < 1, "ch 70");
        CHECK(std::fabs(hz("351 MHz ch 71") - 351.03125e6) < 1, "ch 71");
        CHECK(std::fabs(hz("351 MHz ch 82") - 351.1e6) < 1, "ch 82");
        CHECK(std::fabs(hz("351 MHz S1") - 351.16875e6) < 1 && std::fabs(hz("351 MHz S5") - 351.19375e6) < 1, "S1 - S5");
        CHECK(std::fabs(hz("351 MHz S6") - 351.10625e6) < 1 && std::fabs(hz("351 MHz S15") - 351.1625e6) < 1, "S6 - S15");
        CHECK(std::fabs(hz("467 MHz ch 65") - 467.4e6) < 1 && std::fabs(hz("467 MHz ch 66") - 465.096875e6) < 1 && std::fabs(hz("467 MHz ch 75") - 465.153125e6) < 1, "467 / 465 MHz");
        CHECK(std::fabs(hz("154 MHz ch 28 (data)") - 154.6125e6) < 1, "154 MHz ch 28");
        CHECK(std::fabs(hz("relay D10 input") - 465.090625e6) < 1 && std::fabs(hz("relay D10 output") - 468.853125e6) < 1 && std::fabs(hz("relay D1 output") - 468.796875e6) < 1, "relay pairs");
        std::set<long long> seen; int dup = 0;
        for (const dcr::Channel* c : dcr::channelsOfBand(351)) { const long long k = std::llround(c->hz); if (!seen.insert(k).second) dup++; if (std::fmod((double)std::llround(c->hz - 351.03125e6), 6250.0) != 0) dup += 100; }
        CHECK(dup == 0, "351 MHz channels repeat or are off the 6.25 kHz grid (%d)", dup);
        const dcr::Channel* c = dcr::findChannel(351.2875e6 + 300);
        CHECK(c && c->label == "351 MHz ch 15" && c->calling, "findChannel near the calling channel");
        CHECK(dcr::findChannel(100e6) == nullptr, "100 MHz is not a DCR channel");
        printf("plan: %zu channels, 351 MHz band %zu\n", p.size(), dcr::channelsOfBand(351).size());
    }

    // ---- 2. the standard's test frames
    for (double fs : {250000.0, 48000.0, 1000000.0}) {
        for (int i = 0; i < 4; i++) {
            const SpecSignal& g = specSignals()[i];
            Seg s; s.sym = specTransmission(g, 24); s.gapSec = 0.5;
            Imp m; m.fs = fs; m.snrDb = 99;
            const auto x = capture({s}, m, 0.3, 2.0);
            const auto t = runDcr(x, fs);
            const std::string uc = "UC " + std::to_string(g.uc);
            const X40Row* call = findRow(t, "Call", uc.c_str(), g.comm ? "privacy scrambled" : "normal");
            CHECK(call != nullptr, "%.0f sps, %s: no call row for %s", fs, g.name, uc.c_str());
            CHECK(findRow(t, "Call sign", "100000001") != nullptr, "%.0f sps, %s: call sign 100000001 not shown", fs, g.name);
            CHECK(findRow(t, "Call end", uc.c_str(), "end-of-communication message") != nullptr, "%.0f sps, %s: no end of communication", fs, g.name);
            const unsigned long long vf = voiceCount(t);
            CHECK(vf >= 88 && vf <= 100, "%.0f sps, %s: %llu voice frames counted (96 sent)", fs, g.name, vf);
            if (fs == 250000.0) printf("  %s: %s | voice frames %llu | %s\n", g.name, call ? call->text.c_str() : "-", vf, t.headline.c_str());
        }
    }

    // ---- 3. field round trips with impairments
    struct Case { const char* name; double fs, cfo, ppm, snr, dev, offset; bool inv; };
    const Case cases[] = {
        {"clean 250 ksps", 250000, 0, 0, 99, 1, 0, false},
        {"clean 48 ksps", 48000, 0, 0, 99, 1, 0, false},
        {"clean 1 Msps", 1000000, 0, 0, 99, 1, 0, false},
        {"carrier +2000 Hz", 250000, 2000, 0, 99, 1, 0, false},
        {"carrier -2000 Hz", 250000, -2000, 0, 99, 1, 0, false},
        {"symbol clock +150 ppm", 250000, 0, 150, 99, 1, 0, false},
        {"symbol clock -150 ppm", 250000, 0, -150, 99, 1, 0, false},
        {"reversed polarity", 250000, 0, 0, 99, 1, 0, true},
        {"deviation x 0.85", 250000, 0, 0, 99, 0.85, 0, false},
        {"deviation x 1.15", 250000, 0, 0, 99, 1.15, 0, false},
        {"off-centre +40 kHz, carrier +1500 Hz, 80 ppm", 250000, 1500, 80, 99, 1, 40000, false},
        {"off-centre -60 kHz, reversed", 250000, -800, -60, 99, 1, -60000, true},
        {"24 dB in 6.25 kHz, carrier +1000 Hz", 250000, 1000, 0, 24, 1, 0, false},
        {"20 dB, 100 ppm, reversed", 250000, 0, 100, 20, 1, 0, true},
    };
    for (const Case& c : cases) {
        const auto txs = sweep(8, 11);
        Imp m; m.fs = c.fs; m.cfoHz = c.cfo; m.clockPpm = c.ppm; m.snrDb = c.snr; m.devScale = c.dev; m.offsetHz = c.offset; m.invert = c.inv; m.seed = 5;
        const auto x = capture(segsOf(txs), m, 0.3, 2.0);
        const auto t = runDcr(x, c.fs, 0, c.offset);
        int found, wrong; score(t, txs, found, wrong);
        const int csm = countRows(t, "Call sign"), ends = countRows(t, "Call end");
        printf("  %-48s calls %d / 8, wrong %d, call signs %d, call ends %d, bit errors %s\n", c.name, found, wrong, csm, ends, fact(t, "bit errors").c_str());
        const bool noisy = c.snr < 99;
        CHECK(found >= (noisy ? 7 : 8) && wrong == 0, "%s: %d of 8 calls found, %d wrong", c.name, found, wrong);
        CHECK(csm >= (noisy ? 6 : 8), "%s: %d of 8 call signs", c.name, csm);
        // the first call is UC 1, comm normal; the third is privacy scrambled (i % 3 == 2)
        if (!noisy) CHECK(findRow(t, "Call", "UC 511", "privacy scrambled") != nullptr, "%s: the scrambled call of user code 511 not marked", c.name);
    }

    // ---- 3b. manufacturer messages (1, 2 and 4 units), data blocks, an idle call
    {
        FrameGen g; g.rng = 4242;
        std::vector<uint8_t> ex(54); for (size_t i = 0; i < ex.size(); i++) ex[i] = (uint8_t)((i * 7 + 3) % 5 < 2);
        g.sb0(0, 21, 3, "123456789");
        g.mfrMessage(2, 0, 21, 3, 1, {}, 6);                // manufacturer format 1, one unit
        g.mfrMessage(5, 0, 22, 4, 2, ex, 4);                // format 4, two units (18 extra bits)
        g.mfrMessage(9, 0, 23, 5, 4, ex, 3);                // format 8, four units (54 extra bits)
        std::vector<uint8_t> da(80), db(80);
        for (size_t i = 0; i < 80; i++) { da[i] = (uint8_t)((i * 5 + 1) % 3 == 0); db[i] = (uint8_t)((i * 3 + 2) % 7 < 3); }
        g.data(0, 24, 0, da, db, 6);
        Seg sg; sg.sym = g.s; sg.gapSec = 0.5;
        for (double fs : {250000.0, 48000.0}) {
            Imp m; m.fs = fs; m.cfoHz = 900; m.snrDb = 99;
            const auto x = capture({sg}, m, 0.3, 2.5);
            const auto t = runDcr(x, fs);
            auto hx = [](const std::vector<uint8_t>& b, size_t from, size_t n) { std::string s; char q[4]; for (size_t i = from; i < from + n; i += 8) { int v = 0; for (size_t k = 0; k < 8; k++) v = (v << 1) | (i + k < from + n ? b[i + k] : 0); snprintf(q, sizeof q, "%02X", v); s += q; } return s; };
            CHECK(findRow(t, "Manufacturer message", "UC 21", "manufacturer format 1, manufacturer 3, user code 21, 0 extra bits") != nullptr, "%.0f: one-unit manufacturer message", fs);
            CHECK(findRow(t, "Manufacturer message", "UC 22", ("18 extra bits: " + hx(ex, 0, 18)).c_str()) != nullptr, "%.0f: two-unit manufacturer message (%s)", fs, hx(ex, 0, 18).c_str());
            CHECK(findRow(t, "Manufacturer message", "UC 23", ("54 extra bits: " + hx(ex, 0, 54)).c_str()) != nullptr, "%.0f: four-unit manufacturer message", fs);
            CHECK(findRow(t, "Data", "TCH1", hx(da, 0, 80).c_str()) != nullptr && findRow(t, "Data", "TCH2", hx(db, 0, 80).c_str()) != nullptr, "%.0f: data blocks of both traffic channels", fs);
            CHECK(findRow(t, "Call sign", "123456789") != nullptr, "%.0f: call sign 123456789", fs);
        }
    }

    // ---- 4. noise sweep, and what must give nothing
    printf("noise sweep (carrier over noise in 6.25 kHz): calls found of 8 / wrong calls / SACCH good frames\n");
    for (double snr : {30.0, 22.0, 18.0, 14.0, 10.0, 8.0, 6.0, 5.0, 4.0, 3.0, 2.0}) {
        int found = 0, wrong = 0; unsigned long long good = 0;
        for (unsigned seed = 1; seed <= 2; seed++) {
            const auto txs = sweep(8, 20 + seed);
            Imp m; m.fs = 250000; m.snrDb = snr; m.seed = seed; m.cfoHz = 500;
            const auto t = runDcr(capture(segsOf(txs), m, 0.3, 2.0), 250000);
            int f, w; score(t, txs, f, w); found += f; wrong += w;
            unsigned long long seen, sg; frameCounts(t, seen, sg); good += sg;
        }
        printf("  %4.0f dB: %2d of 16 / %d wrong / %llu good SACCH\n", snr, found, wrong, good);
        if (snr >= 18) CHECK(found >= 15 && wrong == 0, "%.0f dB: %d of 16 found, %d wrong", snr, found, wrong);
        CHECK(wrong <= 1, "%.0f dB: %d wrong calls", snr, wrong);
    }
    {   // noise only: 150 s in chunks (no calls, no rows, no good frame); then noise with a carrier on the channel and with an FM tone of 1 kHz (+-2.5 kHz)
        struct R { const char* name; int mode; };
        for (const R r : {R{"noise only", 0}, R{"noise + carrier 1 kHz off-centre", 1}, R{"noise + FM tone (1 kHz, 2.5 kHz deviation)", 2}}) {
            X40Receiver rx; rx.setVariant(x40::Dcr); rx.setCenterHz(0); rx.configure(250000);
            Rng rng(99 + (unsigned)r.mode);
            std::vector<cf32> blk(4096);
            double ph = 0, ph2 = 0;
            const size_t total = (size_t)(250000.0 * 150);
            for (size_t n = 0; n < total; n += blk.size()) {
                for (auto& v : blk) {
                    v = cf32(0.02f * rng.g(), 0.02f * rng.g());
                    if (r.mode == 1) { ph += 2 * kPi * 1000.0 / 250000.0; v += cf32(0.1f * (float)std::cos(ph), 0.1f * (float)std::sin(ph)); }
                    if (r.mode == 2) { ph2 += 2 * kPi * 1000.0 / 250000.0; ph += 2 * kPi * 2500.0 * std::sin(ph2) / 250000.0 / 1.0; v += cf32(0.1f * (float)std::cos(ph), 0.1f * (float)std::sin(ph)); }
                }
                rx.feed(blk.data(), blk.size());
            }
            X40Telemetry t; rx.telemetry(t, 0);
            unsigned long long seen, sg; frameCounts(t, seen, sg);
            printf("  %-44s 150 s: %zu rows, %llu frames seen, %llu good SACCH\n", r.name, t.rows.size(), seen, sg);
            // no message may come out of noise (a SACCH message counts only when it repeats, x40_dcr.cpp confirm()); a single SACCH frame passes its
            // CRC-6 once in 64 noise frames, so the frames that pass are held to twice that rate instead of none (MinGW's maths gave 1 of 24)
            CHECK(t.rows.empty() && sg <= 1 + seen / 32, "%s: %zu rows, %llu good SACCH of %llu frames", r.name, t.rows.size(), sg, seen);
        }
    }

    // ---- 5. scan
    {
        const double centre = 351.3e6, fs = 500000;
        const dcr::Channel *ca = nullptr, *cb = nullptr, *cc = nullptr;
        for (auto& c : dcr::plan()) { if (c.label == "351 MHz ch 15") ca = &c; if (c.label == "351 MHz ch 40") cb = &c; if (c.label == "351 MHz S3") cc = &c; }
        const dcr::Channel* chs[3] = {ca, cb, cc};
        const double amps[3] = {0.05, 0.03, 0.02};
        std::vector<Tx> txs[3];
        std::vector<cf32> x;
        for (int k = 0; k < 3; k++) {
            txs[k] = sweep(4, 31 + (unsigned)k);
            for (auto& t : txs[k]) t.uc = 10 * (k + 1) + (int)(&t - &txs[k][0]);   // user codes 10 - 13, 20 - 23, 30 - 33
            Imp m; m.fs = fs; m.offsetHz = chs[k]->hz - centre; m.amp = amps[k]; m.cfoHz = 300.0 * (k - 1); m.snrDb = 99;
            const auto f = shape(segsOf(txs[k], 0.5 + 0.2 * k), 0.3 + 0.35 * k, 1.0, false);
            if (x.empty()) x.assign((size_t)((double)f.size() / 96000.0 * fs) + (size_t)(3.5 * fs), cf32(0, 0));
            render(x, f, m);
        }
        Imp nm; nm.fs = fs; nm.amp = 0.05; nm.snrDb = 30; nm.seed = 9; addNoise(x, nm);
        const auto t = runDcr(x, fs, centre);
        printf("  scan: %s | %s\n", t.headline.c_str(), fact(t, "mode").c_str());
        for (int k = 0; k < 3; k++) {
            int found, wrong; score(t, txs[k], found, wrong, chs[k]->label);
            printf("  %-16s calls %d / 4, wrong %d\n", chs[k]->label.c_str(), found, wrong);
            CHECK(found == 4 && wrong == 0, "scan %s: %d of 4 calls, %d wrong", chs[k]->label.c_str(), found, wrong);
        }
        int other = 0; for (auto& r : t.rows) if (r.kind == "Call" && r.id.rfind(ca->label + " ", 0) != 0 && r.id.rfind(cb->label + " ", 0) != 0 && r.id.rfind(cc->label + " ", 0) != 0) other++;
        CHECK(other == 0, "scan: %d calls on other channels", other);
        CHECK(fact(t, "mode").find("scan:") == 0, "scan mode not reported (%s)", fact(t, "mode").c_str());
        CHECK(fact(t, "channel").find("351 MHz") != std::string::npos || fact(t, "channel").find("not on the DCR plan") != std::string::npos, "channel fact: %s", fact(t, "channel").c_str());
        // the same capture, tuned off the plan (450 MHz): one channel at the centre, no scan
        const auto t2 = runDcr(x, fs, 450e6);
        CHECK(fact(t2, "mode") == "single channel", "off-plan tuning: %s", fact(t2, "mode").c_str());
    }
    {   // scan false starts: noise only; a carrier on a channel; a 1 kHz FM tone on a channel (a decoder may start, nothing may be reported)
        const double fs = 500000, centre = 351.3e6;
        for (int mode = 0; mode < 3; mode++) {
            X40Receiver rx; rx.setVariant(x40::Dcr); rx.setCenterHz(centre); rx.configure(fs);
            Rng rng(5 + (unsigned)mode);
            std::vector<cf32> blk(4096);
            double ph = 0, ph2 = 0;
            const double off = dcr::findChannel(351.44375e6)->hz - centre;
            for (size_t n = 0; n < (size_t)(fs * 40); n += blk.size()) {
                for (auto& v : blk) {
                    v = cf32(0.02f * rng.g(), 0.02f * rng.g());
                    if (mode == 1) { ph += 2 * kPi * off / fs; v += cf32(0.2f * (float)std::cos(ph), 0.2f * (float)std::sin(ph)); }
                    if (mode == 2) { ph2 += 2 * kPi * 1000.0 / fs; ph += 2 * kPi * (off + 2500.0 * std::sin(ph2)) / fs; v += cf32(0.2f * (float)std::cos(ph), 0.2f * (float)std::sin(ph)); }
                }
                rx.feed(blk.data(), blk.size());
            }
            X40Telemetry t; rx.telemetry(t, 0);
            unsigned long long seen, sg; frameCounts(t, seen, sg);
            int dec = -1, inSpan = 0; sscanf(fact(t, "mode").c_str(), "scan: %d plan channels in the span, %d with a decoder", &inSpan, &dec);
            printf("  scan false-start %d: %zu rows, %llu good SACCH, %d plan channels in the span, %d decoders at the end\n", mode, t.rows.size(), sg, inSpan, dec);
            CHECK(t.rows.empty() && sg == 0, "scan false start %d: %zu rows", mode, t.rows.size());
            if (mode == 0) CHECK(dec == 0, "scan on noise started %d decoders", dec);
        }
    }

    printf(fails ? "x40 dcr: %d FAILED\n" : "x40 dcr: ok\n", fails);
    return fails ? 1 : 0;
}
