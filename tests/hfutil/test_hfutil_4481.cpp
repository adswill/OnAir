// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// STANAG 4481 FSK (hfutil/hfu_4481.h): the tone plan of Annex C, single-channel and multi-channel broadcasts with plain ITA2 text and with
// synchronous (encrypted-like) channels, at every speed, with a tuning error, noise in 3 kHz, a full band of 16 channels and noise only.
// Simulated signals only.
#include "dect2/hfutil/hfu_4481.h"
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/hfutil/hfutil_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/impair.h"

namespace {

const char* kMsg = "ZCZC NAVAL BROADCAST TEST 0123456789 THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG NNNN";

void noise3k(std::vector<cf32>& x, double snrDb, unsigned seed) {
    double p = 0; for (auto& v : x) p += std::norm(v); p /= (double)x.size();
    const double n0 = p / std::pow(10.0, snrDb / 10) * (24000.0 / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, (float)std::sqrt(n0 / 2));
    for (auto& v : x) v += cf32(g(rng), g(rng));
}

struct Out { std::string text[17]; hfu::S4481Status st; };
Out run(const std::vector<cf32>& x) {
    Out o;
    hfu::Stanag4481Rx rx;
    rx.setTextSink([&](int ch, const std::string& s) { if (ch >= 0 && ch <= 16) o.text[ch] += s; });
    for (size_t i = 0; i < x.size(); i += 4096) {
        rx.push(x.data() + i, std::min<size_t>(4096, x.size() - i));
        if (i <= x.size() * 7 / 10 && i + 4096 > x.size() * 7 / 10) o.st = rx.status();   // while the channels are still sending
    }
    for (auto& c : o.st.channels) printf("  ch %d: %.1f/%.1f Hz %s %.2f Bd, %llu bits, %llu chars, %llu errors, %.1f dB\n", c.n, c.markHz, c.spaceHz, c.startStop ? "start-stop" : "sync", c.baud,
                                        (unsigned long long)c.bits, (unsigned long long)c.chars, (unsigned long long)c.framingErrors, c.snrDb);
    return o;
}
const hfu::S4481Channel* chan(const Out& o, int n) { for (auto& c : o.st.channels) if (c.n == n) return &c; return nullptr; }

}  // namespace

int main() {
    CHECK(hfu::s4481ChannelHz(1) == 425 && hfu::s4481ChannelHz(3) == 765 && hfu::s4481ChannelHz(16) == 2975, "sub-carriers (255 + 170 n) Hz");
    // single channel, every speed; the mark on a frequency ending in 75 Hz, the space 850 Hz above
    for (double baud : {45.45, 50.0, 75.0, 100.0}) {
        hfu::S4481GenCfg g; g.baud = baud;
        auto x = hfu::genStanag4481(g, {{0, kMsg, 0, 1}}, 14.0 * 50 / baud);
        impair::shift(x, 23, 24000);
        noise3k(x, 15, 2);
        const Out o = run(x);
        const auto* c = chan(o, 0);
        printf("single %.2f Bd: mode %d, %s, text \"%.40s...\"\n", baud, o.st.mode, c ? (c->startStop ? "start-stop" : "sync") : "-", o.text[0].c_str());
        CHECK(o.st.mode == 1 && c && c->startStop && std::fabs(c->baud - baud) < 0.1, "single %.2f Bd: mode %d", baud, o.st.mode);
        CHECK(o.text[0].find(kMsg + 5) != std::string::npos, "single %.2f Bd: text \"%s\"", baud, o.text[0].c_str());
        if (c) CHECK(std::fabs(c->spaceHz - c->markHz - 850) < 10 && std::fabs(c->markHz - 1298) < 10, "single: tones %.0f / %.0f", c->markHz, c->spaceHz);
    }
    // multi-channel: plain text on channels 3 and 11, a synchronous stream on 7
    {
        hfu::S4481GenCfg g; g.multi = true; g.baud = 75;
        auto x = hfu::genStanag4481(g, {{3, kMsg, 0, 1}, {7, "", 900, 2}, {11, kMsg, 0, 3}}, 13.0);
        impair::shift(x, -8, 24000);
        noise3k(x, 20, 4);
        const Out o = run(x);
        const auto* c3 = chan(o, 3), *c7 = chan(o, 7), *c11 = chan(o, 11);
        printf("multi: mode %d, %zu channels on; ch7 %s %.2f Bd %llu bits\n", o.st.mode, o.st.channels.size(), c7 && c7->startStop ? "start-stop" : "sync", c7 ? c7->baud : 0, c7 ? (unsigned long long)c7->bits : 0ull);
        CHECK(o.st.mode == 2 && o.st.channels.size() == 3, "multi: mode %d, %zu channels", o.st.mode, o.st.channels.size());
        CHECK(c3 && c3->startStop && o.text[3].find(kMsg + 5) != std::string::npos, "multi ch 3: \"%s\"", o.text[3].c_str());
        CHECK(c11 && c11->startStop && o.text[11].find(kMsg + 5) != std::string::npos, "multi ch 11: \"%s\"", o.text[11].c_str());
        CHECK(c7 && !c7->startStop && std::fabs(c7->baud - 75) < 0.1 && o.text[7].size() < 5, "multi ch 7: sync stream at 75 Bd (%.2f), %zu chars", c7 ? c7->baud : 0, o.text[7].size());
    }
    // a full band: all 16 channels, plain text at 50 Bd
    {
        hfu::S4481GenCfg g; g.multi = true; g.baud = 50;
        std::vector<hfu::S4481Content> all;
        for (int n = 1; n <= 16; n++) all.push_back({n, std::string("CHANNEL ") + std::to_string(n) + " " + kMsg, 0, (unsigned)n});
        auto x = hfu::genStanag4481(g, all, 22.0);
        noise3k(x, 25, 6);
        const Out o = run(x);
        int ok = 0;
        for (int n = 1; n <= 16; n++) ok += o.text[n].find(kMsg + 5) != std::string::npos;
        printf("full band: %zu channels on, %d with the text\n", o.st.channels.size(), ok);
        CHECK(o.st.channels.size() == 16 && ok == 16, "full band: %zu on, %d texts", o.st.channels.size(), ok);
    }
    // noise only
    {
        std::vector<cf32> x(24000 * 6, cf32(0.f, 0.f));
        noise3k(x, 0, 9);
        for (auto& v : x) v += cf32(0, 0);
        const Out o = run(x);
        int chars = 0; for (auto& t : o.text) chars += (int)t.size();
        CHECK(o.st.mode == 0 && chars < 4, "noise: mode %d, %d chars", o.st.mode, chars);
    }
    // through the HF utility receiver at 250 ksps
    {
        hfu::S4481GenCfg g;
        const auto b = hfu::genStanag4481(g, {{0, kMsg, 0, 1}}, 13.0);
        const double rate = 250000;
        std::vector<cf32> x((size_t)((double)b.size() * rate / 24000.0));
        for (size_t i = 0; i < x.size(); i++) {
            const double q = (double)i * 24000.0 / rate;
            const size_t k = (size_t)q;
            const float fr = (float)(q - (double)k);
            const cf32 v = k + 1 < b.size() ? b[k] * (1 - fr) + b[k + 1] * fr : cf32(0, 0);
            const double a = 2 * M_PI * -20000.0 * (double)i / rate;
            x[i] = v * cf32((float)std::cos(a), (float)std::sin(a));
        }
        HfUtilReceiver rx(kHfuKind4481);
        rx.configure(rate);
        rx.setSignalOffset(-20000);
        for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        HfUtilTelemetry t;
        rx.telemetry(t, 0);
        printf("receiver: %s\n", t.mode.c_str());
        for (auto& e : t.events) printf("  event %s\n", e.text.c_str());
        CHECK(t.text.find(kMsg + 5) != std::string::npos, "receiver text \"%s\"", t.text.c_str());
    }
    return testSummary("hfutil_4481");
}
