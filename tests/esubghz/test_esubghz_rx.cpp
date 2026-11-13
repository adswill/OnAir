// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat receiver: the modulator's deviation and rate; messages through noise, carrier offset and several radio rates into
// the receiver; plain, encrypted (with the key, without, a repeat), longer than one packet, and with the spectrum inverted.
#include "dect2/esubghz/esubghz_gen.h"
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/esubghz/esubghz_tel.h"
#include "dect2/tx/tx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Air {
    std::vector<cf32> iq;
    void silence(double sec, double rate) { iq.insert(iq.end(), (size_t)(sec * rate), cf32(0, 0)); }
    void message(const std::vector<uint8_t>& m, double rate, double off) {
        const auto v = esubghzMessageIq(m, rate, off, 0.3f, 0.003, 0.0);
        iq.insert(iq.end(), v.begin(), v.end());
    }
    void noise(double snrDb, double rate, uint32_t seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> nd(0, 1);
        const float sigma = (float)std::sqrt(0.09 / std::pow(10.0, snrDb / 10) * rate / 100e3 / 2);
        for (auto& v : iq) v += cf32(sigma * nd(rng), sigma * nd(rng));
    }
};

static EsubghzTelemetry run(const Air& air, double rate, double off, const esubghz::Key* key) {
    EsubghzReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(off);
    if (key) rx.setKey(*key);
    for (size_t i = 0; i < air.iq.size(); i += 4096) rx.feed(air.iq.data() + i, std::min<size_t>(4096, air.iq.size() - i));
    EsubghzTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static void deviationCheck() {
    const double rate = 2e6;
    std::vector<cf32> v;
    double ph = 0;
    std::vector<uint8_t> oct(40, 0x00);
    for (int i = 0; i < 20; i++) oct.push_back(0xFF);
    esubghzModulate(oct, rate, 0, 1.f, v, ph);
    auto meanHz = [&](size_t a, size_t b) { double s = 0; for (size_t i = a + 1; i < b; i++) s += std::arg(v[i] * std::conj(v[i - 1])); return s / (double)(b - a - 1) * rate / (2 * M_PI); };
    const double sps = rate / esubghz::kBaud;
    const double lo = meanHz((size_t)(40 * sps), (size_t)(300 * sps)), hi = meanHz((size_t)(340 * sps), (size_t)(460 * sps));
    CHECK(std::fabs(lo + esubghz::kDeviationHz) < 50 && std::fabs(hi - esubghz::kDeviationHz) < 50, "deviation %.0f / %.0f Hz", lo, hi);
    const size_t ramp = (size_t)std::lround(sps);
    CHECK(std::fabs((double)v.size() - 480 * sps - (double)ramp) < 2, "length %zu samples for 480 bits and the ramp", v.size());
    for (size_t i = ramp; i + ramp < v.size(); i++) if (std::fabs(std::abs(v[i]) - 1.f) > 1e-3f) { CHECK(false, "constant envelope between the ramps"); break; }
    float step = 0;
    for (size_t i = 1; i < v.size(); i++) step = std::max(step, std::fabs(std::abs(v[i]) - std::abs(v[i - 1])));
    CHECK(std::abs(v.front()) < 0.01f && std::abs(v.back()) < 0.05f && step < 0.05f, "ramps: first %.3f last %.3f step %.3f", (double)std::abs(v.front()), (double)std::abs(v.back()), (double)step);
}

int main() {
    deviationCheck();
    const esubghz::Key key = esubghz::keyFromPassword("onair");
    const std::string longLine = "Flipper: " + std::string(150, 'L') + " end";   // three packets plain
    struct Case { double rate, cfo, snr; bool inv; };
    const Case cases[] = {{2e6, 0, 30, false}, {2e6, 9000, 14, false}, {2e6, -12000, 20, false}, {250e3, 3000, 20, false}, {1.024e6, -5000, 20, false},
                          {10e6, 2000, 25, false}, {2e6, 0, 25, true}};
    for (const Case& c : cases) {
        const double off = -300e3 * (c.rate >= 1e6 ? 1 : 0);   // the slow rate has the channel at the centre
        Air air;
        air.silence(0.05, c.rate);
        air.message(esubghz::encodePlain("Flipper1: hello OnAir"), c.rate, off + c.cfo);
        air.silence(0.7, c.rate);
        esubghz::Sender f(0xABCDEFull);
        const auto e1 = f.encode(&key, "Flipper1: encrypted hi");
        air.message(e1, c.rate, off + c.cfo);
        air.silence(0.7, c.rate);
        air.message(esubghz::encodePlain(longLine), c.rate, off + c.cfo);
        air.silence(0.7, c.rate);
        air.message(e1, c.rate, off + c.cfo);   // the same encrypted message again: a replay
        air.silence(0.7, c.rate);
        air.noise(c.snr, c.rate, 3);
        if (c.inv) for (auto& z : air.iq) z = std::conj(z);
        const EsubghzTelemetry t = run(air, c.rate, c.inv ? -off : off, &key);
        printf("  %.3f Msps, cfo %+.0f Hz, SNR %.0f dB%s: %llu packets ok, %llu bad, %llu messages, cfo est %+.0f, snr est %.1f\n", c.rate / 1e6, c.cfo, c.snr,
               c.inv ? ", inverted" : "", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.messages, t.cfoHz, t.snrDb);
        CHECK(t.blocksOk == 8 && t.blocksBad == 0, "packets %llu ok, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.chat.size() == 4, "%zu chat lines", t.chat.size());
        if (t.chat.size() != 4) continue;
        CHECK(t.chat[0].kind == 0 && t.chat[0].text == "Flipper1: hello OnAir", "line 1 '%s'", t.chat[0].text.c_str());
        CHECK(t.chat[1].kind == 1 && t.chat[1].text == "Flipper1: encrypted hi", "line 2 kind %d '%s'", t.chat[1].kind, t.chat[1].text.c_str());
        CHECK(t.chat[2].kind == 0 && t.chat[2].text == longLine && t.chat[2].packets == 3, "line 3 (%d packets)", t.chat[2].packets);
        CHECK(t.chat[3].kind == 3, "line 4 is a replay (kind %d)", t.chat[3].kind);
        CHECK(std::fabs(t.cfoHz - (c.inv ? -c.cfo : c.cfo)) < 2500, "cfo estimate %.0f", t.cfoHz);
        if (c.inv) CHECK(!t.packets.empty() && t.packets[0].inverted, "inverted packets are flagged");
    }
    {   // without the key: the encrypted line is named, not shown
        Air air;
        air.silence(0.05, 2e6);
        esubghz::Sender f(5);
        air.message(f.encode(&key, "Flipper1: you cannot read this"), 2e6, -300e3);
        air.silence(0.7, 2e6);
        air.noise(25, 2e6, 9);
        const EsubghzTelemetry t = run(air, 2e6, -300e3, nullptr);
        CHECK(t.chat.size() == 1 && t.chat[0].kind == 2 && t.unreadable == 1, "no key: kind %d", t.chat.empty() ? -1 : t.chat[0].kind);
    }
    {   // the transmitter's generator (what the TX session sends) is what the receiver reads
        TxRequest r;
        r.text = "OnAir: via the TX row";
        r.opt["esubghz.ifHz"] = 300e3;
        std::string err;
        auto g = makeEsubghzTxGenerator(r, 2e6, err);
        CHECK(g && err.empty(), "generator: %s", err.c_str());
        Air air;
        air.silence(0.05, 2e6);
        std::vector<cf32> blk(8192);
        while (g && !g->finished()) { g->generate(blk.data(), blk.size()); air.iq.insert(air.iq.end(), blk.begin(), blk.end()); }
        air.silence(0.7, 2e6);
        const EsubghzTelemetry t = run(air, 2e6, 300e3, nullptr);
        CHECK(t.chat.size() == 1 && t.chat[0].text == "OnAir: via the TX row", "TX generator line: %s", t.chat.empty() ? "(none)" : t.chat[0].text.c_str());
        {   // the transmitter's own output with no silence before it (it keys 5 ms in): the first 2000 samples once set the noise floor from
            // the packet itself and nothing was heard (docs/QA-SWEEP-3.9.md)
            auto g0 = makeEsubghzTxGenerator(r, 2e6, err);
            Air a0;
            while (g0 && !g0->finished()) { g0->generate(blk.data(), blk.size()); a0.iq.insert(a0.iq.end(), blk.begin(), blk.end()); }
            a0.silence(0.7, 2e6);
            const EsubghzTelemetry t0 = run(a0, 2e6, 300e3, nullptr);
            CHECK(t0.chat.size() == 1 && t0.chat[0].text == "OnAir: via the TX row", "TX generator line without a lead: %s", t0.chat.empty() ? "(none)" : t0.chat[0].text.c_str());
        }
        r.payload = std::string(1100, 'x');
        CHECK(!makeEsubghzTxGenerator(r, 2e6, err) && !err.empty(), "a message longer than 1024 octets is refused");
    }
    {   // Clear in the middle of a three-packet message: the half received is dropped, not shown later or joined to the next message;
        // an encrypted line heard before Clear and sent again afterwards is still a replay
        const double rate = 2e6, off = -300e3;
        esubghz::Sender f(0x5EED);
        const auto e1 = f.encode(&key, "Flipper1: before Clear");
        Air before, after;
        before.silence(0.05, rate);
        before.message(e1, rate, off);
        before.silence(0.7, rate);
        const auto part = esubghzMessageIq(esubghz::encodePlain(longLine), rate, off, 0.3f, 0.003, 0.0);
        before.iq.insert(before.iq.end(), part.begin(), part.begin() + (long)(part.size() / 2));
        after.silence(0.7, rate);
        after.message(esubghz::encodePlain("Flipper1: after Clear"), rate, off);
        after.silence(0.7, rate);
        after.message(e1, rate, off);
        after.silence(0.7, rate);
        EsubghzReceiver rx;
        rx.configure(rate);
        rx.setSignalOffset(off);
        rx.setKey(key);
        auto feed = [&](const Air& air) { for (size_t i = 0; i < air.iq.size(); i += 4096) rx.feed(air.iq.data() + i, std::min<size_t>(4096, air.iq.size() - i)); };
        feed(before);
        EsubghzTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.chat.size() == 1 && t.blocksOk >= 2, "before Clear: %zu lines, %llu packets", t.chat.size(), (unsigned long long)t.blocksOk);
        rx.clearResults();
        feed(after);
        rx.telemetry(t, 0);
        CHECK(t.chat.size() == 2, "after Clear: %zu chat lines (wanted the new line and the replay)", t.chat.size());
        if (t.chat.size() == 2) {
            CHECK(t.chat[0].kind == 0 && t.chat[0].text == "Flipper1: after Clear" && t.chat[0].packets == 1, "after Clear: line 1 kind %d, %d packets, '%s'",
                  t.chat[0].kind, t.chat[0].packets, t.chat[0].text.c_str());
            CHECK(t.chat[1].kind == 3 && t.replays == 1, "after Clear: the line from before is a replay (kind %d)", t.chat[1].kind);
        }
    }
    {   // a weak signal: count how many of 40 single-packet lines come through at 9 dB
        Air air;
        air.silence(0.05, 2e6);
        for (int i = 0; i < 40; i++) { air.message(esubghz::encodePlain("Flipper: line " + std::to_string(i)), 2e6, -300e3 + 4000); air.silence(0.55, 2e6); }
        air.noise(9, 2e6, 11);
        const EsubghzTelemetry t = run(air, 2e6, -300e3, nullptr);
        printf("  9 dB: %zu of 40 lines\n", t.chat.size());
        CHECK(t.chat.size() >= 34, "only %zu of 40 lines at 9 dB", t.chat.size());
    }
    return testSummary("esubghz rx");
}
