// Pagers, bit level: the BCH(31,21) code word (one and two wrong bits repaired, three refused), POCSAG and FLEX pages through the encoders and the parsers.
#include "dect2/pager/pager_proto.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::pager;
#include "../common/test_util.h"

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

// Runs pages through the encoder and a parser in the given read mode.
static std::vector<PagerMessage> decodePocsag(const std::vector<PocsagPage>& pages, int mode) {
    const std::vector<uint8_t> bits = pocsagBits(pages);
    std::vector<PagerMessage> out;
    PocsagParser p;
    p.readMode = mode;
    for (size_t i = 576; i + 32 * 17 <= bits.size(); ) {
        i += 32;
        for (int s = 0; s < 16; s++, i += 32) p.word(wordFromBits(&bits[i]), s, out);
    }
    p.flush(out);
    return out;
}

static void readModeTests() {
    // Australian emergency paging: text sent with function 0. The numeric reading OnAir showed before:
    const std::string garbage = "1522- 63U 186*5]37UU ]6*3 260*969[ -]2UU9]";
    PocsagPage g; g.ric = 1000; g.fn = 0; g.type = kPagerNumeric; g.text = garbage;
    PocsagPage t; t.ric = 2000; t.fn = 0; t.type = kPagerAlpha; t.text = "QD4639 Segment01 Segment02 RESPOND STRUCTURE FIRE 12 MAIN ST";
    PocsagPage tone; tone.ric = 3000; tone.fn = 0; tone.type = kPagerTone;
    std::vector<PocsagPage> pages = {g, t, tone};
    const char* const nums[] = {"123-456 U 789", "0412345678", "911", "12 34 56", "5551234-[22]", "000", "1234567890123", "U", "07 3333 4444 ]", "*99", "42-17", "3600"};
    for (int k = 0; k < 12; k++) { PocsagPage n; n.ric = 4000 + (uint32_t)k * 8; n.fn = 0; n.type = kPagerNumeric; n.text = nums[k]; pages.push_back(n); }
    for (int mode = 0; mode < kPocsagReadModes; mode++) {
        const std::vector<PagerMessage> out = decodePocsag(pages, mode);
        CHECK(out.size() == pages.size(), "mode %d: %zu pages", mode, out.size());
        for (const PagerMessage& m : out) {
            if (m.address == 3000) { CHECK(m.type == kPagerTone && m.text.empty(), "mode %d: tone page", mode); continue; }
            const bool wantAlpha = mode == kPocsagReadAlpha || (mode == kPocsagReadAuto && (m.address == 1000 || m.address == 2000));
            CHECK((m.type == kPagerAlpha) == wantAlpha, "mode %d address %u read as %s: '%s'", mode, m.address, pagerTypeName(m.type), m.text.c_str());
            CHECK(m.text == (wantAlpha ? m.alphaText : m.numText), "mode %d: text follows the type", mode);
            if (m.address == 1000) {
                CHECK(m.numText == garbage, "numeric reading '%s'", m.numText.c_str());
                if (mode == kPocsagReadAuto) CHECK(m.text.rfind("QD4639 Segment01 Segment", 0) == 0 && m.readHow == kPocsagHowAuto, "auto: '%s'", m.text.c_str());
            }
            if (m.address == 2000 && wantAlpha) CHECK(m.text == t.text, "text '%s'", m.text.c_str());
            if (m.address >= 4000 && !wantAlpha) CHECK(m.text == nums[(m.address - 4000) / 8], "numeric '%s'", m.text.c_str());
            if (mode == kPocsagReadAlpha || mode == kPocsagReadNumeric) CHECK(m.readHow == kPocsagHowForced, "forced reason");
            if (mode == kPocsagReadByFunction) CHECK(m.readHow == kPocsagHowFunction, "function reason");
        }
    }
    // a per-address override re-reads a decoded page
    std::vector<PagerMessage> out = decodePocsag({g}, kPocsagReadAuto);
    PagerMessage m = out.at(0);
    pocsagApplyRead(m, kPocsagReadNumeric, true);
    CHECK(m.type == kPagerNumeric && m.text == garbage && m.readHow == kPocsagHowAddress, "override numeric");
    pocsagApplyRead(m, kPocsagReadAlpha, true);
    CHECK(m.type == kPagerAlpha && m.readHow == kPocsagHowAddress && m.text.rfind("QD4639", 0) == 0, "override alpha");
    pocsagApplyRead(m, kPocsagReadAuto);
    CHECK(m.type == kPagerAlpha && m.readHow == kPocsagHowAuto, "override back to auto");
    CHECK(pagerApplyCharset("a[{|}~]", kPagerCharGerman) == "a\xC3\x84\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F\xC3\x9C", "German national characters");
    CHECK(pagerApplyCharset("a[{", kPagerCharAscii) == "a[{", "ASCII leaves the text alone");

    // random numeric pages and random noise words must rarely look like text
    srand(11);
    int numAlpha = 0, noiseAlpha = 0;
    const int trials = 20000;
    auto nib = [](std::vector<uint8_t>& b, int v) { for (int q = 0; q < 4; q++) b.push_back((uint8_t)((v >> q) & 1)); };
    for (int k = 0; k < trials; k++) {
        std::vector<uint8_t> b;
        const int digits = 3 + rand() % 30;
        for (int d = 0; d < digits; d++) nib(b, rand() % 4 ? rand() % 10 : 10 + rand() % 6);
        while (b.size() % 20) nib(b, 12);              // filled with spaces
        if (pocsagLooksAlpha(b)) numAlpha++;
        std::vector<uint8_t> nz;
        const int words = 1 + rand() % 8;
        for (int w = 0; w < words * 20; w++) nz.push_back((uint8_t)(rand() & 1));
        if (pocsagLooksAlpha(nz)) noiseAlpha++;
    }
    printf("  POCSAG read modes: random numeric pages taken for text %d of %d (%.3f%%), noise words %d of %d (%.3f%%)\n", numAlpha, trials, 100.0 * numAlpha / trials,
           noiseAlpha, trials, 100.0 * noiseAlpha / trials);
    CHECK(numAlpha * 200 < trials, "numeric pages taken for text: %d of %d", numAlpha, trials);
    CHECK(noiseAlpha * 200 < trials, "noise taken for text: %d of %d", noiseAlpha, trials);
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

// A frame built by hand, not with flexPhaseBits: a long address (capcode 4096 + ((0x1FFFFE ^ 0x1FFFFF) << 15) + 0x1F9000 = 2105344) with an
// alphanumeric "TEST" page, a short address 0x8000 + 1234567 with a numeric page "123" and a short one with a tone. The layout follows real
// frames (issues #95, #261): the BIW and the first vector word of each page carry the 4-bit check, and for a long address the message header is the
// second vector word, the text starting at the start index (the messages then tile the message field exactly).
static void handBuiltTest() {
    uint32_t f[kFlexPhaseWords] = {};
    f[0] = flexWithCheck(5u << 10);                      // BIW: address words from index 1 (bits 8-9 = 0), vector words from index 5 (bits 10-15)
    f[1] = 0x1000; f[2] = 0x1FFFFE;                      // long address (two words)
    f[3] = 0x8000 + 1234567;                             // short
    f[4] = 0x8000 + 77;                                  // short
    // vector words: types in bits 4-6, start word in bits 7-13, length in bits 14-20; the long address has two (5 and 6)
    f[5] = flexWithCheck((5u << 4) | (9u << 7) | (3u << 14));    // alphanumeric from word 9, length 3 (the header in word 6, two text words)
    f[6] = 3u << 11;                                     // header: continuation 0, fragment 3 (complete)
    f[7] = flexWithCheck((3u << 4) | (11u << 7));        // standard numeric from word 11, 1 word
    f[8] = flexWithCheck((2u << 4) | (1u << 7));         // tone only
    f[9] = ((uint32_t)'T' << 7) | ((uint32_t)'E' << 14);         // bits 0-6 are not text in the first word of a complete message
    f[10] = (uint32_t)'S' | ((uint32_t)'T' << 7) | (3u << 14);   // 3 is a filler
    // numeric: 2 header bits, then 4-bit digits from bit 0 upwards: 1, 2, 3, then filler 12
    f[11] = (1u << 2) | (2u << 6) | (3u << 10) | (12u << 14) | (12u << 18);
    std::vector<uint8_t> bits(2816);
    for (int blk = 0; blk < 11; blk++)
        for (int w = 0; w < 8; w++) {
            const uint32_t W = encodeWord(rev21(f[blk * 8 + w]));
            for (int j = 0; j < 32; j++) bits[(size_t)(blk * 256 + j * 8 + w)] = (uint8_t)((W >> (31 - j)) & 1);
        }
    std::vector<PagerMessage> out;
    FlexPhaseStat st;
    flexDecodePhase(bits.data(), out, st);
    bool alpha = false, num = false, tone = false;
    for (const PagerMessage& m : out) {
        if (m.address == 2105344) { alpha = m.type == kPagerAlpha && m.text == "TEST"; CHECK(alpha, "long address page '%s' type %d", m.text.c_str(), m.type); }
        else if (m.address == 1234567) { num = m.type == kPagerNumeric && m.text == "123"; CHECK(num, "numeric page '%s'", m.text.c_str()); }
        else if (m.address == 77) { tone = m.type == kPagerTone; CHECK(tone, "tone page type %d", m.type); }
        else CHECK(false, "unexpected capcode %u", m.address);
    }
    CHECK(alpha && num && tone, "hand-built frame: %zu pages", out.size());
    printf("  hand-built FLEX frame: %zu pages\n", out.size());
}

// Round trip of an alpha page per POCSAG speed: encode, flip the bits of one code word (hit = its index, -1 for none), parse.
static std::vector<PagerMessage> parseWithFlip(const std::vector<PocsagPage>& pages, int hit, uint32_t mask, PocsagParser& p) {
    const std::vector<uint8_t> bits = pocsagBits(pages);
    std::vector<PagerMessage> out;
    int k = 0;
    for (size_t i = 576; i + 32 * 17 <= bits.size(); i += 32 * 17) {
        for (int s = 0; s < 16; s++, k++) {
            uint32_t w = wordFromBits(&bits[i + 32 + 32 * (size_t)s]);
            if (k == hit) w ^= mask;
            p.word(w, s, out);
        }
    }
    p.flush(out);
    return out;
}

static void pocsagRoundTripTests() {
    // 512, 1200 and 2400 baud use the same code words: the bit layer carries the text, the baud only sets the symbol rate
    const char* const texts[] = {"OnAir test 512: hello", "OnAir test 1200: hello", "OnAir test 2400: hello"};
    const int bauds[] = {512, 1200, 2400};
    for (int s = 0; s < 3; s++) {
        PocsagPage a; a.ric = 1234563; a.fn = 3; a.type = kPagerAlpha; a.text = texts[s];
        const std::vector<PocsagPage> pages = {a};
        // clean: the text comes back whole and the page is not a tone page
        PocsagParser clean; clean.readMode = kPocsagReadAlpha;
        const std::vector<PagerMessage> out = parseWithFlip(pages, -1, 0, clean);
        CHECK(out.size() == 1, "%d baud: %zu pages", bauds[s], out.size());
        if (out.size() == 1) {
            CHECK(out[0].type == kPagerAlpha && out[0].text == texts[s] && !out[0].damaged, "%d baud: '%s' type %d", bauds[s], out[0].text.c_str(), out[0].type);
        }
        // a message word that cannot be repaired (3 wrong bits, refused by the code): the third message word (code word 9,
        // the address is code word 6) and the first one (code word 7)
        const std::vector<uint8_t> bits = pocsagBits(pages);
        auto refusedFlip = [&](int k) {
            const uint32_t w = wordFromBits(&bits[576 + 32 + 32 * (size_t)k]);
            for (int x = 0; x < 32; x++)
                for (int y = x + 1; y < 32; y++)
                    for (int z = y + 1; z < 32; z++) {
                        const uint32_t m3 = (1u << x) | (1u << y) | (1u << z);
                        if (pocsagClassify(w ^ m3).kind == PocsagWord::Failed) return m3;
                    }
            return 0u;
        };
        const uint32_t mask = refusedFlip(9), mask1 = refusedFlip(7);
        CHECK(mask != 0 && mask1 != 0, "%d baud: no 3-bit error refused", bauds[s]);
        PocsagParser mid; mid.readMode = kPocsagReadAlpha;
        const std::vector<PagerMessage> outMid = parseWithFlip(pages, 9, mask, mid);
        CHECK(outMid.size() == 1 && outMid[0].type == kPagerAlpha && outMid[0].damaged, "%d baud: damaged page kept (%zu pages)", bauds[s], outMid.size());
        if (outMid.size() == 1) {
            // the characters after the lost word come back exactly; the lost word's 20 bits are a gap (characters 5 and 8 may be dropped)
            CHECK(outMid[0].text.rfind("OnAir", 0) == 0, "%d baud: text before the gap '%s'", bauds[s], outMid[0].text.c_str());
            CHECK(outMid[0].text.find(std::string(texts[s]).substr(9)) != std::string::npos, "%d baud: text after the gap '%s'", bauds[s], outMid[0].text.c_str());
        }
        CHECK(mid.failed == 1, "%d baud: %d failed words", bauds[s], mid.failed);
        // the first message word lost: the page used to become a tone page here
        PocsagParser first; first.readMode = kPocsagReadAlpha;
        const std::vector<PagerMessage> outFirst = parseWithFlip(pages, 7, mask1, first);
        CHECK(outFirst.size() == 1 && outFirst[0].type != kPagerTone && !outFirst[0].text.empty(), "%d baud: first word lost gives a tone page", bauds[s]);
        if (outFirst.size() == 1) CHECK(outFirst[0].text.find(std::string(texts[s]).substr(3)) != std::string::npos, "%d baud: text after first gap '%s'", bauds[s], outFirst[0].text.c_str());
    }
}

int main() {
    bchTests();
    pocsagTests();
    pocsagRoundTripTests();
    readModeTests();
    flexTests();
    handBuiltTest();
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
