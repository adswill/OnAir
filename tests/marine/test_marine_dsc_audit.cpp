// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DSC audit: MMSI kinds and countries, call kinds, symbol repair with the two copies and the check character, noise and bit offsets.
#include "dect2/marine/marine_dsc.h"
#include "dect2/marine/marine_mmsi.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::marine;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<DscCall> run(const std::vector<uint8_t>& bits, bool invert, int skip, double ber, int seed, bool vhf = false) {
    std::vector<DscCall> got;
    DscDecoder d(invert, vhf);
    d.setCallback([&](const DscCall& c) { got.push_back(c); });
    std::mt19937 rng(seed);
    for (int i = 0; i < 30 + skip; i++) d.pushBit((rng() & 1) ? 0.3f : -0.3f);
    for (uint8_t b : bits) {
        int v = b;
        if (ber > 0 && (rng() % 100000) < ber * 100000) v ^= 1;
        d.pushBit(v ? 0.9f : -0.9f);
    }
    for (int i = 0; i < 600; i++) d.pushBit((rng() & 1) ? 0.3f : -0.3f);
    return got;
}

static void setSlot(std::vector<uint8_t>& bits, int dots, int slot, int sym) {
    const unsigned b = dscSymbolBits(sym);
    for (int i = 0; i < 10; i++) bits[(size_t)(dots + 10 * slot + i)] = (uint8_t)((b >> (9 - i)) & 1);
}
static void flipSlot(std::vector<uint8_t>& bits, int dots, int slot, unsigned mask) {
    for (int i = 0; i < 10; i++) if ((mask >> i) & 1) bits[(size_t)(dots + 10 * slot + i)] ^= 1;
}
// a bit pattern of three flipped bits that leaves the symbol invalid (the complement of a symbol is valid again, so it is no damage)
static unsigned badMask(unsigned base) {
    int sv;
    for (unsigned m = 7; m < 1024; m++) { if (__builtin_popcount(m) != 3) continue; if (!dscSymbolDecode(base ^ m, sv)) return m; }
    return 7;
}
static int dxSlot(int k) { return k == 0 ? 12 : 14 + 2 * k; }   // body position k

static void killSlot(std::vector<uint8_t>& bits, int dots, const std::vector<int>& body, int k) {
    const unsigned raw = dscSymbolBits(body[(size_t)k]);
    for (int slot : {dxSlot(k), dxSlot(k) + 5}) {
        unsigned m = badMask(raw);
        if (slot != dxSlot(k)) { m = 0; for (unsigned c = 7; c < 1024; c++) { int sv; if (__builtin_popcount(c) == 3 && c != badMask(raw) && !dscSymbolDecode(raw ^ c, sv)) { m = c; break; } } }
        for (int i = 0; i < 10; i++) if ((m >> (9 - i)) & 1) bits[(size_t)(dots + 10 * slot + i)] ^= 1;
    }
}

int main() {
    // ---- MMSI
    {
        MmsiInfo a = mmsiInfo("232123456");
        CHECK(a.kind == MmsiKind::Ship && a.mid == 232 && a.country == "United Kingdom", "ship uk");
        a = mmsiInfo("002320064");
        CHECK(a.kind == MmsiKind::CoastStation && a.mid == 232, "coast");
        a = mmsiInfo("023212345");
        CHECK(a.kind == MmsiKind::Group && a.mid == 232, "group");
        a = mmsiInfo("111232001");
        CHECK(a.kind == MmsiKind::SarAircraft && a.mid == 232, "sar");
        a = mmsiInfo("366123456");
        CHECK(a.country == "United States", "us");
        a = mmsiInfo("248064000");
        CHECK(a.country == "Malta" && a.kind == MmsiKind::Ship, "malta");
        a = mmsiInfo("970123456"); CHECK(a.kind == MmsiKind::AisSart, "sart");
        a = mmsiInfo("23212~456"); CHECK(a.partial && a.mid == 232, "partial");
        a = mmsiInfo("2~2123456"); CHECK(a.partial && a.country.empty(), "partial mid");
        a = mmsiInfo("12"); CHECK(a.kind == MmsiKind::Unknown, "short");
        ShipDb nm;
        CHECK(nm.importText("# c\n232123456\tMV Example\r\n002320064\tSolent Coastguard\nbad\n232123456\tRenamed\n") == 3 && nm.name("232123456") == "Renamed" && nm.name("002320064") == "Solent Coastguard" && nm.name("1") == "", "names");
    }
    const auto distress = dscBuildDistress("232123456", 103, 50.85, -1.3, 12, 34, 109);
    const auto indiv = dscBuildIndividual("002320064", 100, "232123456", 118, 126, -1, -1);
    // ---- kinds
    {
        auto k = [&](const std::vector<int>& body, int eos) { const auto g = run(dscFrameBits(body, eos, 200), false, 0, 0, 1); return g.empty() ? std::string("none") : g[0].kind; };
        CHECK(k(distress, 127) == "Distress alert", "kind distress: %s", k(distress, 127).c_str());
        CHECK(k(dscBuildDistressAck("002320064", "232123456", 103, 50.85, -1.3, 12, 34, 109), 127) == "Distress alert acknowledgement", "kind ack");
        CHECK(k(indiv, 117) == "Test call", "kind test: %s", k(indiv, 117).c_str());
        CHECK(k(indiv, 122) == "Test acknowledgement", "kind test ack");
        CHECK(k(dscBuildAllShips("232123456", 108, 109, 126, 82910, -1), 127) == "All ships safety call", "kind safety");
        CHECK(k(dscBuildIndividual("002320064", 100, "232123456", 100, 126, 156525, -1), 117) == "Individual call", "kind plain");
        const auto g = run(dscFrameBits(distress, 127, 200), false, 0, 0, 1);
        CHECK(!g.empty() && g[0].fromCountry == "United Kingdom" && g[0].fromType == "ship" && g[0].symbols.size() == distress.size(), "country/symbols");
        const auto g2 = run(dscFrameBits(indiv, 117, 200), false, 0, 0, 1);
        CHECK(!g2.empty() && g2[0].toType == "coast station" && g2[0].toCountry == "United Kingdom", "to coast: %s", g2.empty() ? "" : g2[0].toType.c_str());
    }
    // ---- repair
    const int dots = 200;
    {   // one bit wrong in both copies of a symbol: neither parity holds; the check character picks the single-bit fix
        auto bits = dscFrameBits(distress, 127, dots);
        flipSlot(bits, dots, dxSlot(7), 1u << 4); flipSlot(bits, dots, dxSlot(7) + 5, 1u << 2);
        const auto g = run(bits, false, 0, 0, 2);
        CHECK(g.size() == 1 && g[0].eccOk && g[0].fromMmsi == "232123456" && g[0].lat > 50.8 && g[0].lat < 50.9 && g[0].repaired >= 1, "single-bit in both copies");
    }
    {   // a symbol destroyed completely in both copies (several bits): rebuilt from the check character
        auto bits = dscFrameBits(distress, 127, dots);
        killSlot(bits, dots, distress, 3);
        const auto g = run(bits, false, 0, 0, 3);
        CHECK(g.size() == 1 && g[0].eccOk && g[0].recovered && g[0].fromMmsi == "232123456", "rebuilt: %s", g.empty() ? "no call" : g[0].text.c_str());
    }
    {   // two destroyed symbols: not recoverable, but the call is still reported with the check failed
        auto bits = dscFrameBits(distress, 127, dots);
        for (int k : {3, 9}) killSlot(bits, dots, distress, k);
        const auto g = run(bits, false, 0, 0, 4);
        CHECK(g.size() == 1 && !g[0].eccOk && g[0].erasures >= 1, "two lost");
    }
    {   // the DX copy is another valid symbol in two places (the RX copy is right): the check character chooses
        auto bits = dscFrameBits(distress, 127, dots);
        setSlot(bits, dots, dxSlot(2), 33); setSlot(bits, dots, dxSlot(8), 71);
        const auto g = run(bits, false, 0, 0, 5);
        CHECK(g.size() == 1 && g[0].eccOk && g[0].fromMmsi == "232123456" && g[0].repaired == 2, "two conflicts: %d", g.empty() ? -1 : g[0].repaired);
    }
    {   // a wrong symbol in both copies that happens to be valid is not repairable; the check says so
        auto bits = dscFrameBits(distress, 127, dots);
        setSlot(bits, dots, dxSlot(5), 12); setSlot(bits, dots, dxSlot(5) + 5, 12);
        const auto g = run(bits, false, 0, 0, 6);
        CHECK(g.size() == 1 && !g[0].eccOk, "undetectable-by-copies error flagged by the check");
    }
    {   // noise and offsets together, many seeds: the repair must not lower the good count
        int good = 0, bad = 0;
        for (int seed = 1; seed <= 60; seed++) {
            const auto g = run(dscFrameBits(distress, 127, dots), seed & 1, seed % 10, 0.01, 300 + seed, false);
            if (g.empty()) continue;
            (void)seed;
            if (g[0].eccOk) { if (g[0].fromMmsi == "232123456" && g[0].nature == 103) good++; else bad++; }
        }
        printf("BER 1%%: %d good, %d wrong with a good check\n", good, bad);
        CHECK(bad == 0, "false positives with a good check: %d", bad);
        CHECK(good >= 25, "BER 1%%: only %d good", good);
    }
    {   // noise only, long
        DscDecoder d; int n = 0; d.setCallback([&](const DscCall& c) { if (c.eccOk) n++; });
        std::mt19937 rng(21);
        for (int i = 0; i < 3000000; i++) d.pushBit((rng() & 1) ? 1.f : -1.f);
        CHECK(n == 0, "no checked call from noise: %d", n);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
