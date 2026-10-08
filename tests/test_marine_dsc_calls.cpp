// DSC calls at the bit level: dot pattern, phasing hunt, DX/RX time diversity, ECC, polarity, bit offsets, damage.
#include "dect2/marine_dsc.h"
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

int main() {
    const auto distress = dscBuildDistress("232123456", 103, 50.85, -1.3, 12, 34, 109);
    const auto allShips = dscBuildAllShips("232123456", 108, 109, 126, 82910, -1);
    const auto indiv = dscBuildIndividual("002320064", 100, "232123456", 109, 126, 82910, 82910);
    const auto ack = dscBuildDistressAck("002320064", "232123456", 103, 50.85, -1.3, 12, 34, 109);
    {
        const auto got = run(dscFrameBits(distress, 127, 200), false, 0, 0, 1);
        CHECK(got.size() == 1, "one call, got %zu", got.size());
        if (!got.empty()) {
            const auto& c = got[0];
            CHECK(c.eccOk && c.format == 112 && c.fromMmsi == "232123456" && c.nature == 103 && c.hasPos && std::abs(c.lat - 50.85) < 1e-6 && std::abs(c.lon + 1.3) < 1e-6 && c.utcHour == 12 && c.utcMin == 34 && c.erasures == 0, "distress fields: %s", c.text.c_str());
            CHECK(c.eos == 127, "eos");
        }
    }
    {   // all four kinds in a row on one line, different dot patterns
        std::vector<uint8_t> all;
        auto add = [&](const std::vector<uint8_t>& b) { all.insert(all.end(), b.begin(), b.end()); for (int i = 0; i < 50; i++) all.push_back((uint8_t)(i & 1)); };
        add(dscFrameBits(allShips, 127, 20)); add(dscFrameBits(indiv, 117, 200)); add(dscFrameBits(ack, 127, 200)); add(dscFrameBits(distress, 127, 200));
        const auto got = run(all, false, 0, 0, 2);
        CHECK(got.size() == 4, "four calls, got %zu", got.size());
        if (got.size() == 4) {
            CHECK(got[0].format == 116 && got[0].category == 108 && got[0].freqRx == "8291.0 kHz" && got[0].eccOk, "all ships: %s", got[0].text.c_str());
            CHECK(got[1].format == 120 && got[1].to == "002320064" && got[1].eos == 117 && got[1].eccOk, "individual: %s", got[1].text.c_str());
            CHECK(got[2].format == 116 && got[2].distressMmsi == "232123456" && got[2].telecmd1 == 110 && got[2].eccOk, "ack: %s", got[2].text.c_str());
            CHECK(got[3].format == 112 && got[3].eccOk, "distress");
        }
    }
    {   // inverted line (B and Y swapped): the other polarity decodes; the wrong one finds nothing
        auto bits = dscFrameBits(distress, 127, 200);
        for (auto& b : bits) b ^= 1;
        const auto a = run(bits, true, 0, 0, 3), w = run(bits, false, 0, 0, 3);
        CHECK(a.size() == 1 && a[0].eccOk && a[0].format == 112, "inverted");
        CHECK(w.empty(), "wrong polarity: %zu calls", w.size());
    }
    for (int skip = 1; skip < 10; skip++) {
        const auto got = run(dscFrameBits(distress, 127, 200), false, skip, 0, 4);
        CHECK(got.size() == 1 && got[0].eccOk, "bit offset %d", skip);
    }
    {   // a short dot pattern (20 bits) as used on VHF and for acknowledgements
        const auto got = run(dscFrameBits(indiv, 117, 20), false, 0, 0, 5, true);
        CHECK(got.size() == 1 && got[0].eccOk && got[0].vhf, "20 dots");
    }
    {   // damage: kill a whole DX symbol and a whole RX symbol: the other copy gives the symbol
        auto bits = dscFrameBits(distress, 127, 200);
        const size_t base = 200 + 10 * 16;                       // first information symbol after the phasing and the format specifiers is slot 16
        for (int k = 0; k < 10; k++) bits[base + 10 * 3 + k] ^= 1;          // DX symbol of slot 19 (pos), complement bits (likely invalid)
        for (int k = 0; k < 10; k++) bits[base + 10 * 8 + 10 * 5 + k] ^= (k % 3 == 0);
        const auto got = run(bits, false, 0, 0, 6);
        CHECK(got.size() == 1 && got[0].eccOk, "damaged symbols repaired by the repeat");
    }
    {   // bit errors, 0.5 %: count calls with a good ECC
        int good = 0, any = 0;
        for (int seed = 1; seed <= 60; seed++) {
            const auto got = run(dscFrameBits(distress, 127, 200), false, 0, 0.005, 100 + seed);
            if (!got.empty()) any++;
            if (!got.empty() && got[0].eccOk && got[0].fromMmsi == "232123456" && got[0].hasPos) good++;
        }
        printf("BER 0.5%%: %d of 60 calls with a good ECC (%d decoded at all)\n", good, any);
        CHECK(good >= 54, "BER 0.5%%: only %d good", good);
    }
    {   // noise only
        DscDecoder d; int n = 0; d.setCallback([&](const DscCall&) { n++; });
        std::mt19937 rng(11);
        for (int i = 0; i < 2000000; i++) d.pushBit((rng() & 1) ? 1.f : -1.f);
        CHECK(n == 0, "no call from noise: %d", n);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
