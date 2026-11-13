// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL known answer from the air: a real 600 bit/s aircraft downlink (tests/data/hfdl/real_burst.h) through the receiver must give its
// performance data. The loopback tests (generator into receiver) cannot see a convention that both sides get wrong the same way, such as
// the order of the two code polynomials; this one can.
#include "dect2/hfdl/hfdl_rx.h"
#include "../data/hfdl/real_burst.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> base64(const std::string& s) {
    std::vector<uint8_t> o;
    unsigned acc = 0; int bits = 0;
    for (char c : s) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((uint8_t)(acc >> bits)); }
    }
    return o;
}

int main() {
    std::string text;
    for (const char* l : kHfdlRealBurst) text += l;
    const std::vector<uint8_t> raw = base64(text);
    std::vector<cf32> x(raw.size() / 2);
    for (size_t i = 0; i < x.size(); i++) x[i] = cf32((float)(int8_t)raw[2 * i] / 128.f, (float)(int8_t)raw[2 * i + 1] / 128.f);
    CHECK(x.size() == 32400, "%zu samples", x.size());

    HfdlReceiver rx;
    rx.configure(kHfdlRealRate);
    rx.setCenterHz(kHfdlRealCentreHz);
    rx.setChannels({kHfdlRealDialHz});
    for (size_t i = 0; i < x.size(); i += 1000) rx.feed(x.data() + i, std::min<size_t>(1000, x.size() - i));
    HfdlTelemetry t;
    rx.telemetry(t, ~0ull);

    int perf = 0;
    for (const auto& m : t.messages) {
        printf("  %s | %s\n", m.kind.c_str(), m.summary.c_str());
        if (m.flight.find("CES202") != std::string::npos && m.hasPos && std::fabs(m.lat + 33.93) < 0.1 && std::fabs(m.lon - 151.17) < 0.1) perf++;
    }
    printf("  good %llu, bad %llu, 600 bit/s %llu, SNR %.1f dB, cfo %+.1f Hz\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
           (unsigned long long)t.byRate[1], t.snrDb, t.cfoHz);
    CHECK(t.blocksOk == 1 && t.blocksBad == 0, "good %llu bad %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    CHECK(t.byRate[1] == 1, "not decoded at 600 bit/s");
    CHECK(perf >= 1, "no performance data from CES202 near -33.93, 151.17");
    CHECK(t.snrDb > 8, "SNR %.1f dB", t.snrDb);
    printf("hfdl real: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
