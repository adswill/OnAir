// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DSC decoder on bits: call types x dot pattern lengths x end-of-sequence variants x random and burst bit errors, bit slips, truncated phasing,
// leading noise, repeated back-to-back sequences, soft confidence. Each case is run with many seeds; the rate of exact decodes is reported.
#include "dect2/marine/marine_dsc.h"
#include <cstdio>
#include <random>
using namespace dect2; using namespace dect2::marine;

struct Call { const char* name; std::vector<int> body; int eos; };
static int fails = 0;

static int run(const Call& c, int dots, int flip, int burst, int slip, int lead, int repeats, unsigned seed, bool vhf, int* wrongOut) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> frame = dscFrameBits(c.body, c.eos, dots);
    std::vector<uint8_t> bits;
    for (int i = 0; i < lead; i++) bits.push_back((uint8_t)(rng() & 1));
    for (int r = 0; r < repeats; r++) { bits.insert(bits.end(), frame.begin(), frame.end()); for (int i = 0; i < 30; i++) bits.push_back((uint8_t)(i & 1)); }
    std::vector<float> soft;
    for (uint8_t b : bits) soft.push_back(b ? 0.9f : -0.9f);
    for (size_t i = 0; i < soft.size(); i++) if (flip && (int)(rng() % 1000) < flip) soft[i] = -soft[i] * 0.3f;   // wrong bit, low confidence
    if (burst) { const size_t at = (lead + dots + 40) + rng() % 200; for (int k = 0; k < burst && at + k < soft.size(); k++) soft[at + k] = (rng() & 1) ? 0.2f : -0.2f; }
    if (slip) { const size_t at = lead + dots + 60 + rng() % 150; if (slip > 0) soft.insert(soft.begin() + at, 0.5f); else soft.erase(soft.begin() + at); }
    DscDecoder d(false, vhf); int ok = 0, wrong = 0;
    d.setCallback([&](const DscCall& x) { if (x.eccOk) { if (x.symbols == c.body) ok++; else wrong++; } });
    for (float s : soft) d.pushBit(s);
    if (wrongOut) *wrongOut += wrong;
    return ok;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const Call calls[] = {
        {"distress", dscBuildDistress("232123456", 103, 50.85, -1.3, 12, 34, 109), 127},
        {"allships", dscBuildAllShips("235012345", 108, 109, 126, 82910, -1), 127},
        {"individual", dscBuildIndividual("002320064", 100, "232123456", 109, 126, 82910, 82910), 117},
        {"individual EOS122", dscBuildIndividual("002320064", 100, "232123456", 109, 126, 82910, 82910), 122},
        {"dist ack", dscBuildDistressAck("002320064", "232123456", 103, 50.85, -1.3, 12, 34, 109), 127},
        {"allships eos117", dscBuildAllShips("235012345", 110, 109, 126, -1, -1), 117},
    };
    struct Case { const char* what; int flip, burst, slip, lead, repeats; double minRate; };
    const Case cases[] = {
        {"clean", 0, 0, 0, 0, 1, 1.0}, {"leading noise 300 bits", 0, 0, 0, 300, 1, 1.0}, {"two sequences back to back", 0, 0, 0, 0, 2, 1.0},
        {"1 pct bit errors", 10, 0, 0, 0, 1, 0.9}, {"3 pct bit errors", 30, 0, 0, 0, 1, 0.3}, {"burst of 12 bits", 0, 12, 0, 0, 1, 0.8},
        {"burst of 25 bits", 0, 25, 0, 0, 1, 0.3}, {"one bit inserted", 0, 0, 1, 0, 1, 0.0}, {"one bit lost", 0, 0, -1, 0, 1, 0.0},
    };
    for (int vhf = 0; vhf < 2; vhf++) for (const Call& c : calls) for (int dots : {200, 100, 20, 8}) {
        if (vhf == 0 && dots == 8) continue;
        for (const Case& k : cases) {
            int ok = 0, tot = 0, wrong = 0;
            for (unsigned s = 1; s <= 40; s++) { ok += run(c, dots, k.flip, k.burst, k.slip, k.lead, k.repeats, s, vhf, &wrong) > 0; tot++; }
            const double rate = (double)ok / tot;
            const bool good = rate >= k.minRate && (wrong == 0 || (k.flip >= 30 && wrong <= 2));   // 3 pct errors: the 7-bit check can pass by chance
            if (!good || k.minRate == 0.0) printf("%s %-16s dots=%-3d %-28s %2d/%d exact, wrong=%d\n", good ? "info" : "FAIL", c.name, dots, k.what, ok, tot, wrong);
            if (!good) fails++;
        }
    }
    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
