// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF utility FSK / MFSK modes: each decoder reads its generator's text back with frequency offset, noise and sample clock error (simulated only).
#include "dect2/hfutil/hfu_fsk.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::hfu;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static std::vector<cf32> noisy(std::vector<cf32> v, double cfo, double snrDb, uint32_t seed) {
    if (const char* e = getenv("HFU_SNR")) snrDb = atof(e);
    double ph = 0;
    const double st = 2 * M_PI * cfo / 24000;
    double p = 0;
    for (auto& s : v) { p += std::norm(s); }
    p /= (double)v.size();
    for (auto& s : v) { s *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += st; }
    if (snrDb < 90) {
        const double n = p / std::pow(10.0, snrDb / 10) * 24000.0 / 3000.0;
        std::mt19937 g(seed);
        std::normal_distribution<float> d(0, (float)std::sqrt(n / 2));
        for (auto& s : v) s += cf32(d(g), d(g));
    }
    return v;
}

struct R { std::string text; std::vector<std::string> ev; FskStatus st; };
static R run(FskKind k, const std::vector<cf32>& s) {
    FskRx rx(k);
    R r;
    rx.setTextSink([&](const std::string& t) { r.text += t; });
    rx.setEventSink([&](const std::string& t) { r.ev.push_back(t); });
    const size_t step = 2048;
    for (size_t i = 0; i < s.size(); i += step) rx.push(s.data() + i, std::min(step, s.size() - i));
    r.st = rx.status();
    return r;
}

static std::string squeeze(const std::string& t) { std::string o; int run = 0; char last = 0; for (char c : t) { run = c == last ? run + 1 : 0; last = c; if (run < 4) o += c; } return o.size() > 200 ? o.substr(0, 200) : o; }
static bool hasText(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
static const char* kMsg = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 12345 ONAIR HF UTILITY TEST MESSAGE";

static void testArq() {
    struct T { FskKind k; double baud, shift; int cyc; double snr, cfo; const char* name; } tt[] = {{FskKind::ArqE, 96, 400, 8, 14, 35, "ARQ-E 96/8"}, {FskKind::ArqE, 184.6, 400, 4, 16, -60, "ARQ-E 184/4"}, {FskKind::ArqE, 48, 170, 8, 14, 20, "ARQ-E 48/8"}, {FskKind::ArqN, 96, 850, 0, 14, 25, "ARQ-N 96"}, {FskKind::ArqN, 192, 400, 0, 16, -40, "ARQ-N 192"}};
    for (const auto& t : tt) {
        FskGenCfg c;
        c.kind = t.k; c.baud = t.baud; c.shift = t.shift; c.cycle = t.cyc; c.idleChars = 200;
        auto s = noisy(genFsk(c, kMsg), t.cfo, t.snr, 3);
        auto r = run(t.k, s);
        CHECK(hasText(r.text, kMsg), "%s: got '%s' (detail %s)", t.name, squeeze(r.text).c_str(), r.st.detail.c_str());
        if (getenv("HFU_V")) for (auto& e : r.ev) std::printf("   ev: %s\n", e.c_str());
        std::printf("%s: %zu chars, baud %.1f, %s\n", t.name, r.text.size(), r.st.baud, r.st.detail.c_str());
    }
}

static void testBauer() {
    struct T { FskKind k; int depth; double baud, snr, cfo; const char* name; } tt[] = {{FskKind::Autospec, 1, 68.5, 12, 30, "Autospec 68.5"}, {FskKind::Autospec, 1, 137, 14, -30, "Autospec 137"}, {FskKind::Spread, 11, 102.7, 12, 15, "Spread 11"}, {FskKind::Spread, 21, 137, 14, -20, "Spread 21"}, {FskKind::Spread, 51, 68.5, 12, 10, "Spread 51"}};
    for (const auto& t : tt) {
        FskGenCfg c;
        c.kind = t.k; c.baud = t.baud; c.shift = t.k == FskKind::Spread ? 410 : 270; c.depth = t.depth; c.idleChars = 150;
        auto s = noisy(genFsk(c, kMsg), t.cfo, t.snr, 5);
        auto r = run(t.k, s);
        CHECK(hasText(r.text, kMsg), "%s: got '%s' (%s)", t.name, squeeze(r.text).c_str(), r.st.detail.c_str());
        std::printf("%s: %s, %.1f Bd\n", t.name, r.st.detail.c_str(), r.st.baud);
    }
}

static void testGTor() {
    for (int b : {100, 200, 300}) for (int golay = 0; golay < 2; golay++) {
        FskGenCfg c;
        c.kind = FskKind::GTor; c.gtorBaud = b; c.gtorGolay = golay;
        std::string msg = "G-TOR test frame at the given speed, with a CRC check.";
        auto s = noisy(genFsk(c, msg), 25.0, 22, 7);
        auto r = run(FskKind::GTor, s);
        CHECK(hasText(r.text, msg), "G-TOR %d golay=%d: got '%s' %zu events", b, golay, r.text.substr(0, 80).c_str(), r.ev.size());
        std::printf("G-TOR %d Bd %s: %zu chars\n", b, golay ? "Golay form" : "data form", r.text.size());
    }
}

static void testEfr() {
    FskGenCfg c;
    c.kind = FskKind::Efr;
    const std::string msg = "ABC";
    auto s = noisy(genFsk(c, msg), 40.0, 15, 9);
    auto r = run(FskKind::Efr, s);
    bool ok = false;
    for (auto& e : r.ev) if (hasText(e, "EFR telegram ok") && hasText(e, "address 12 / 34") && hasText(e, "414243")) ok = true;
    CHECK(ok, "EFR: %zu events, text '%s', %s", r.ev.size(), r.text.c_str(), r.ev.empty() ? "" : r.ev[0].c_str());
    std::printf("EFR telegrams: %zu\n", r.ev.size());
}

static void testMfsk() {
    struct T { FskKind k; double ms, snr, cfo; const char* name; } tt[] = {{FskKind::Piccolo6, 50, 16, 40, "Piccolo Mk VI"}, {FskKind::Coquelet8, 37.5, 16, -30, "Coquelet-8 37.5"}, {FskKind::Coquelet8, 75, 14, 20, "Coquelet-8 75"}};
    for (const auto& t : tt) {
        FskGenCfg c;
        c.kind = t.k; c.toneMs = t.ms; c.centre = 0; c.idleChars = 40;
        auto s = noisy(genFsk(c, kMsg), t.cfo, t.snr, 11);
        auto r = run(t.k, s);
        CHECK(hasText(r.text, kMsg), "%s: got '%s'", t.name, squeeze(r.text).c_str());
        std::printf("%s: %zu chars, %s\n", t.name, r.text.size(), r.st.detail.c_str());
    }
}

static void testNoise() {
    std::vector<cf32> z(240000);
    std::mt19937 g(1);
    std::normal_distribution<float> d(0, 0.3f);
    for (auto& s : z) s = cf32(d(g), d(g));
    for (FskKind k : {FskKind::ArqE, FskKind::Autospec, FskKind::Spread, FskKind::GTor, FskKind::Efr, FskKind::Piccolo6, FskKind::Coquelet8}) {
        auto r = run(k, z);
        CHECK(r.text.size() < 12, "kind %d: decoded %zu chars from noise", (int)k, r.text.size());
    }
}

int main() {
    testArq();
    testBauer();
    testGTor();
    testEfr();
    testMfsk();
    testNoise();
    if (fails) std::printf("%d FAILED\n", fails); else std::printf("OK\n");
    return fails ? 1 : 0;
}
