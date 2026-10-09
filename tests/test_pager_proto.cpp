// Pagers, bit level: the BCH(31,21) code word (one and two wrong bits repaired, three refused), POCSAG and FLEX pages through the encoders and the parsers.
#include "dect2/pager_proto.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::pager;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void bchTests() {
    CHECK(encodeWord(kPocsagIdle >> 11) == kPocsagIdle, "the idle code word is not a valid word: %08x", encodeWord(kPocsagIdle >> 11));
    srand(7);
    int single = 0, dbl = 0, triple = 0, refused3 = 0;
    for (int t = 0; t < 400; t++) {
        const uint32_t data = (uint32_t)rand() & 0x1FFFFF;
        const uint32_t w = encodeWord(data);
        CHECK(correctWord(w).ok && correctWord(w).bits == 0 && correctWord(w).data == data, "clean word %06x", data);
        const int a = rand() % 32, b = (a + 1 + rand() % 31) % 32, c = (b + 1 + rand() % 30) % 32;
        const Fixed f1 = correctWord(w ^ (1u << a));
        if (f1.ok && f1.bits == 1 && f1.data == data) single++;
        const Fixed f2 = correctWord(w ^ (1u << a) ^ (1u << b));
        if (f2.ok && f2.bits == 2 && f2.data == data) dbl++;
        if (c != a && c != b) {
            triple++;
            const Fixed f3 = correctWord(w ^ (1u << a) ^ (1u << b) ^ (1u << c));
            if (!f3.ok) refused3++;
        }
    }
    CHECK(single == 400, "1-bit corrections %d of 400", single);
    CHECK(dbl == 400, "2-bit corrections %d of 400", dbl);
    CHECK(refused3 == triple, "3-bit errors refused: %d of %d", refused3, triple);
    printf("  BCH: %d single, %d double repaired, %d of %d triple errors refused\n", single, dbl, refused3, triple);
}

static void pocsagTests() {
    std::vector<PocsagPage> pages;
    PocsagPage a; a.ric = 1234567; a.fn = 3; a.type = kPagerAlpha; a.text = "OnAir test: hello, {world}!"; pages.push_back(a);
    PocsagPage n; n.ric = 1234560; n.fn = 0; n.type = kPagerNumeric; n.text = "123-456 U 789"; pages.push_back(n);
    PocsagPage t; t.ric = 7; t.fn = 1; t.type = kPagerTone; pages.push_back(t);
    const std::vector<uint8_t> bits = pocsagBits(pages);
    size_t i = 576;
    std::vector<PagerMessage> out;
    PocsagParser p;
    int batches = 0;
    while (i + 32 * 17 <= bits.size()) {
        CHECK(wordFromBits(&bits[i]) == kPocsagSync, "sync word of batch %d", batches);
        i += 32;
        for (int s = 0; s < 16; s++, i += 32) {
            uint32_t w = wordFromBits(&bits[i]);
            if (batches == 0 && s == 15) w ^= 0x00110000;              // two wrong bits in a code word
            p.word(w, s, out);
        }
        batches++;
    }
    p.flush(out);
    CHECK(out.size() == 3, "%zu pages", out.size());
    for (const PagerMessage& m : out) {
        if (m.address == 1234567) CHECK(m.type == kPagerAlpha && m.function == 3 && m.text == a.text, "alpha '%s'", m.text.c_str());
        else if (m.address == 1234560) CHECK(m.type == kPagerNumeric && m.function == 0 && m.text == n.text, "numeric '%s'", m.text.c_str());
        else if (m.address == 7) CHECK(m.type == kPagerTone && m.function == 1 && m.text.empty(), "tone");
        else CHECK(false, "unexpected address %u", m.address);
    }
    CHECK(p.failed == 0 && p.fixed == 1, "counters ok %d fixed %d failed %d", p.ok, p.fixed, p.failed);
    printf("  POCSAG: %zu pages in %d batches, %d code words repaired\n", out.size(), batches, p.fixed);
}

static void flexTests() {
    int cycle = 0, frame = 0;
    CHECK(flexFiwCheck(flexFiwData(9, 77), cycle, frame) && cycle == 9 && frame == 77, "FIW");
    CHECK(!flexFiwCheck(flexFiwData(9, 77) ^ 0x10, cycle, frame), "FIW checksum");
    CHECK(flexModeForCode(0xB068 ^ 0x0101, 3) && flexModeForCode(0xB068 ^ 0x0101, 3)->speed == kFlex3200_4, "mode for a damaged sync code");
    CHECK(!flexModeForCode(0x1234, 3), "a made-up code");
    std::vector<FlexPage> pages(4);
    pages[0].cap = 1234567; pages[0].type = kPagerAlpha; pages[0].text = "OnAir test FLEX: hello";
    pages[1].cap = 1234568; pages[1].type = kPagerNumeric; pages[1].text = "555-0123 U 45";
    pages[2].cap = 1234569; pages[2].type = kPagerTone;
    pages[3].cap = 77; pages[3].type = kPagerAlpha; pages[3].text = "x";
    std::vector<uint8_t> bits = flexPhaseBits(pages);
    CHECK(bits.size() == 2816, "phase bits %zu", bits.size());
    bits[100] ^= 1; bits[1500] ^= 1; bits[1501] ^= 1;                      // wrong bits in the phase
    std::vector<PagerMessage> out;
    FlexPhaseStat st;
    flexDecodePhase(bits.data(), out, st);
    CHECK(out.size() == 4 && st.biwOk && st.failed == 0, "%zu pages, failed %d", out.size(), st.failed);
    for (const PagerMessage& m : out) {
        if (m.address == 1234567) CHECK(m.type == kPagerAlpha && m.text == pages[0].text, "alpha '%s'", m.text.c_str());
        else if (m.address == 1234568) CHECK(m.type == kPagerNumeric && m.text == pages[1].text, "numeric '%s'", m.text.c_str());
        else if (m.address == 1234569) CHECK(m.type == kPagerTone, "tone");
        else if (m.address == 77) CHECK(m.type == kPagerAlpha && m.text == "x", "short alpha '%s'", m.text.c_str());
        else CHECK(false, "unexpected capcode %u", m.address);
    }
    printf("  FLEX: %zu pages, %d code words ok, %d repaired\n", out.size(), st.ok, st.fixed);
}

int main() {
    bchTests();
    pocsagTests();
    flexTests();
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
