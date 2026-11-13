// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW code table, callsign pattern and timing decoder: perfect keying from 5 to 50 WPM, Farnsworth spacing, hand keying (jitter, heavy
// dashes), a change of speed, noise spikes and drop-outs.
#include "dect2/cw/cw_morse.h"
#include "dect2/common/stable_random.h"
#include "dect2/cw/cw_gen.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#endif
using namespace dect2;
#include "../common/test_util.h"

static std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s;
}

// the durations (seconds) through the timing decoder; jitter added when asked
static std::string decode(const std::vector<double>& d, cw::Timing& tm, std::mt19937* rng = nullptr, double jitter = 0) {
    dect2::normal_distribution<double> nd(0, 1);
    std::string out;
    for (size_t i = 0; i < d.size(); i++) {
        double ms = d[i] * 1000;
        if (rng && jitter > 0) ms *= std::max(0.5, 1 + jitter * nd(*rng));
        const bool on = i % 2 == 0;
        if (on) tm.mark(ms);
        else { for (double t = 10; t < ms; t += 10) tm.idle(t); tm.space(ms); }
        out += tm.take();
    }
    tm.idle(5000);
    out += tm.take();
    return trim(out);
}

int main() {
    // table
    for (const char* t : {"A", "Z", "0", "9", "?", "/", "<AR>", "<SK>", "<BT>", "<KN>", "<HH>"}) {
        const std::string c = cw::encodeSymbol(t);
        CHECK(!c.empty() && cw::decodeSymbol(c) && std::string(cw::decodeSymbol(c)) == t, "round trip %s", t);
    }
    CHECK(cw::encodeSymbol("+") == ".-.-." && cw::encodeSymbol("=") == "-...-", "AR / BT aliases");
    CHECK(cw::decodeSymbol("......---") == nullptr, "unknown code");
    // callsigns
    for (const char* c : {"LY2ABC", "K1ABC", "9A1AA", "DL1XYZ/P", "2E0ABC", "W1AW", "OH2/LY2ABC", "VK2ABC/MM"}) CHECK(cw::isCallsign(c), "callsign %s", c);
    for (const char* c : {"5NN", "TU", "CQ", "599", "TEST", "73", "QRZ?", "ABCDEFG", "K"}) CHECK(!cw::isCallsign(c), "not a callsign %s", c);
    const auto cs = cw::callsigns("CQ DE LY2ABC LY2ABC K DL1XYZ 5NN");
    CHECK(cs.size() == 2 && cs[0] == "LY2ABC" && cs[1] == "DL1XYZ", "callsigns of a text: %zu", cs.size());

    const std::string msg = "CQ CQ DE LY2ABC LY2ABC <AR> K";
    // every speed, perfect keying
    for (double wpm : {5.0, 8.0, 13.0, 20.0, 28.0, 35.0, 42.0, 50.0}) {
        cw::Timing tm;
        const std::string got = decode(cwKeying(msg + " " + msg, wpm), tm);
        CHECK(got == msg + " " + msg, "%.0f WPM: \"%s\"", wpm, got.c_str());
        CHECK(std::fabs(tm.wpm() - wpm) < 0.05 * wpm, "%.0f WPM measured %.1f", wpm, tm.wpm());
    }
    // Farnsworth: characters at 18 WPM, 8 WPM overall; and 25 / 12
    for (auto fw : {std::pair<double, double>{18, 8}, {25, 12}, {20, 5}}) {
        cw::Timing tm;
        const std::string text = "TEST DE OK1XX OK1XX UR 579 579 <BT> " + msg;
        const std::string got = decode(cwKeying(text, fw.first, fw.second), tm);
        CHECK(got.find(msg) != std::string::npos && got.find("UR 579 579") != std::string::npos, "Farnsworth %.0f/%.0f: \"%s\"", fw.first, fw.second, got.c_str());
        CHECK(std::fabs(tm.wpm() - fw.first) < 0.08 * fw.first && tm.farnsworth(), "Farnsworth %.0f/%.0f: %.1f WPM, flag %d", fw.first, fw.second, tm.wpm(), tm.farnsworth());
    }
    // hand keying: 10 % jitter, dashes 3.6 dots
    {
        std::mt19937 rng(5);
        int ok = 0;
        for (double wpm : {12.0, 18.0, 25.0}) {
            cw::Timing tm;
            const std::string got = decode(cwKeying(msg + " " + msg + " " + msg, wpm, 0, 3.6), tm, &rng, 0.10);
            if (got.find("DE LY2ABC LY2ABC") != std::string::npos) ok++;
            else printf("  hand keyed %.0f WPM: \"%s\"\n", wpm, got.c_str());
        }
        CHECK(ok == 3, "hand keying: %d of 3", ok);
    }
    // a change of speed: 15 WPM, then 40 WPM
    {
        cw::Timing tm;
        std::vector<double> d = cwKeying(msg, 15);
        const std::vector<double> d2 = cwKeying("TEST OK1XX OK1XX TEST OK1XX", 40);
        d.insert(d.end(), d2.begin(), d2.end());
        const std::string got = decode(d, tm);
        CHECK(got.find(msg) == 0 && got.find("OK1XX TEST OK1XX") != std::string::npos && std::fabs(tm.wpm() - 40) < 3, "speed change: \"%s\" %.1f WPM", got.c_str(), tm.wpm());
    }
    // spikes and drop-outs, through the debouncing keyer (a millisecond at a time)
    for (double wpm : {12.0, 25.0, 40.0}) {
        cw::Timing tm;
        cw::Keyer k;
        std::vector<char> ms;
        for (int i = 0; i < 300; i++) ms.push_back(0);
        const auto d = cwKeying(msg + " " + msg, wpm);
        for (size_t i = 0; i < d.size(); i++) for (int j = 0; j < (int)std::lround(d[i] * 1000); j++) ms.push_back(i % 2 == 0);
        for (int i = 0; i < 3000; i++) ms.push_back(0);
        std::mt19937 rng(9);
        for (int n = 0; n < (int)(ms.size() / 400); n++) {   // a 4 ms flip every 0.4 s on average
            const size_t at = rng() % (ms.size() - 10);
            for (int j = 0; j < 4; j++) ms[at + j] = !ms[at + j];
        }
        std::string got;
        for (char v : ms) { k.sample(v != 0, tm); got += tm.take(); }
        got = trim(got);
        CHECK(got == msg + " " + msg, "glitches %.0f WPM: \"%s\"", wpm, got.c_str());
    }
    {   // noise from a busy band (key-downs and -ups of every length) on a thread with a small stack: replay and resync fed push() again,
        // which could start another replay, and that recursion ran a Windows thread (1 MB stack) out after a few seconds on 40 m
        auto noise = [] {
            std::mt19937 rng(7);
            dect2::uniform_real_distribution<double> d(1, 400);
            for (int run = 0; run < 40; run++) {
                cw::Timing tm;
                for (int i = 0; i < 20000; i++) {
                    const double ms = d(rng) * (rng() % 7 == 0 ? 6 : 1);
                    if (i & 1) tm.space(ms); else tm.mark(ms);
                    if (i % 97 == 0) tm.idle(d(rng) * 3);
                    if (i % 500 == 0) (void)tm.take();
                }
            }
        };
#ifndef _WIN32
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024);
        pthread_t th;
        auto fn = [](void* f) -> void* { (*(decltype(noise)*)f)(); return nullptr; };
        CHECK(pthread_create(&th, &at, fn, &noise) == 0, "thread");
        pthread_join(th, nullptr);
        pthread_attr_destroy(&at);
#else
        std::thread(noise).join();
#endif
        CHECK(true, "noise on a 256 kB stack");
    }
    printf(fails ? "cw morse: %d FAILED\n" : "cw morse: all passed\n", fails);
    return fails ? 1 : 0;
}
