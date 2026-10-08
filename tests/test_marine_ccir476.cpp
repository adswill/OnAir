// CCIR 476 / SITOR-B / NAVTEX at the bit level: the code table, the FEC time diversity, framing, damage and polarity.
#include "dect2/marine_navtex.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::marine;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> makeBits(const std::string& body, int idleBefore, int idleAfter) {
    std::vector<int> tok(idleBefore, -1);
    for (uint8_t c : navtexEncode(body)) tok.push_back(c);
    for (int i = 0; i < idleAfter; i++) tok.push_back(-1);
    return codesToBits(sitorBSlots(tok));
}

static uint32_t lev(const std::string& a, const std::string& b) {
    std::vector<uint32_t> p(b.size() + 1), q(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) p[j] = (uint32_t)j;
    for (size_t i = 1; i <= a.size(); i++) {
        q[0] = (uint32_t)i;
        for (size_t j = 1; j <= b.size(); j++) q[j] = std::min({p[j] + 1, q[j - 1] + 1, p[j - 1] + (a[i - 1] != b[j - 1])});
        p.swap(q);
    }
    return p[b.size()];
}

struct Got { std::vector<NavtexMessage> m; };

static Got run(const std::vector<uint8_t>& bits, bool invert, float sure, int skip = 0, double ber = 0, int seed = 1, int burstEvery = 0) {
    Got g;
    NavtexDecoder d(invert);
    d.setCallback([&](const NavtexMessage& m) { g.m.push_back(m); });
    std::mt19937 rng(seed);
    for (int i = 0; i < 5; i++) d.pushBit((rng() & 1) ? 0.3f : -0.3f);       // noise before
    for (int i = 0; i < skip; i++) d.pushBit((rng() & 1) ? 0.3f : -0.3f);
    size_t n = 0;
    for (uint8_t b : bits) {
        int v = b;
        if (ber > 0 && (rng() % 100000) < ber * 100000) v ^= 1;
        if (burstEvery && (n % (size_t)burstEvery) < 7 && n > 400) v = (int)(rng() & 1);
        d.pushBit(v ? sure : -sure);
        n++;
    }
    for (int i = 0; i < 300; i++) d.pushBit((rng() & 1) ? 0.3f : -0.3f);     // noise after
    d.flush();
    return g;
}

int main() {
    // the code: 35 words with four marks, 29 characters and 6 service signals
    int valid = 0, chars = 0;
    for (unsigned c = 0; c < 128; c++) {
        if (ccir476Valid(c)) {
            valid++;
            if (ccir476Char((uint8_t)c, false) || ccir476Char((uint8_t)c, true)) chars++;
        }
    }
    CHECK(valid == 35, "35 valid words, got %d", valid);
    CHECK(chars == 29, "29 characters, got %d", chars);
    CHECK(!ccir476Valid(0) && !ccir476Valid(0x7F) && !ccir476Valid(0x0E), "invalid words");
    CHECK(kAlpha == 0x0F && ccir476Valid(kAlpha) && ccir476Valid(kBeta) && ccir476Valid(kRep) && ccir476Valid(kLtrs) && ccir476Valid(kFigs), "service signals have four marks");
    // known letters (M.476 table): A = 1000111, B = 1110010, space = 1011100
    CHECK(ccir476Char(0x47, false) == 'A' && ccir476Char(0x72, false) == 'B' && ccir476Char(0x5C, false) == ' ', "A B space");
    CHECK(ccir476Char(0x2E, true) == '1' && ccir476Char(0x2D, true) == '0' && ccir476Char(0x74, true) == '5', "figures");
    // the order on the air (B = 1 = the higher tone): the least significant bit goes first, as fldigi navtex.cxx (bytes_to_code)
    // reads them. M.476 writes phasing signal 1 (alpha) as BBBBYYY; with fldigi's values beta is BBYYBBY, the repeat signal
    // YBBYYBB and the letter A BBBYYYB. A decoder that read the first bit as the most significant one fails every word here.
    {
        auto onAir = [](uint8_t c) { std::string s; for (uint8_t b : codesToBits({c})) s += b ? 'B' : 'Y'; return s; };
        CHECK(onAir(kAlpha) == "BBBBYYY", "alpha on the air: %s", onAir(kAlpha).c_str());
        CHECK(onAir(kBeta) == "BBYYBBY", "beta on the air: %s", onAir(kBeta).c_str());
        CHECK(onAir(kRep) == "YBBYYBB", "repeat signal on the air: %s", onAir(kRep).c_str());
        CHECK(onAir(0x47) == "BBBYYYB", "A on the air: %s", onAir(0x47).c_str());
        // the decoder fed with hand-written B/Y words (not through codesToBits): phasing, then "ZCZC" in the slot layout
        const char* alpha = "BBBBYYY"; const char* rq = "YBBYYBB";
        std::vector<std::string> words;
        for (int i = 0; i < 40; i++) { words.push_back(rq); words.push_back(alpha); }
        std::vector<uint8_t> bits;
        for (const auto& w : words) for (char ch : w) bits.push_back(ch == 'B');
        NavtexDecoder d;
        for (uint8_t b : bits) d.pushBit(b ? 0.9f : -0.9f);
        CHECK(d.locked(), "locks on hand-written phasing words");
    }
    // the figure row follows the ITA2 digit positions Q W E R T Y U I O P = 1 2 3 4 5 6 7 8 9 0
    const char* ltrs = "QWERTYUIOP"; const char* digs = "1234567890";
    for (int i = 0; i < 10; i++) { uint8_t cl, cf; ccir476Code(ltrs[i], false, cl); ccir476Code(digs[i], true, cf); CHECK(cl == cf, "digit %c on the same word as %c", digs[i], ltrs[i]); }
    // text round trip through the shifts
    {
        const std::string t = "NOW IS THE TIME FOR ALL GOOD MEN 12.5 KT, GALE 8 (STORM)\nLINE TWO - 4209.5 KHZ?";
        const auto codes = navtexEncode(t);
        std::string back; bool figs = false;
        for (uint8_t c : codes) { if (c == kLtrs) figs = false; else if (c == kFigs) figs = true; else { const int ch = ccir476Char(c, figs); if (ch && ch != '\r') back += (char)ch; } }
        CHECK(back == t, "text round trip: '%s'", back.c_str());
    }
    // the slot layout: DX at even places, the retransmission five places later at odd places
    {
        std::vector<int> tok = {-1, -1, 0x47, 0x72, 0x5C, 0x47, -1};
        const auto s = sitorBSlots(tok);
        CHECK(s[4] == 0x47 && s[4 + 5] == 0x47 && s[6] == 0x72 && s[6 + 5] == 0x72, "retransmission five places later");
        CHECK(s[0] == kBeta && s[1] == kAlpha, "idle");
    }
    const std::string body = "ZCZC EA01\r\n010530 UTC OCT 26\r\nNAVAREA I WARNING 123.\r\nSOUTH NORTH SEA. WRECK 53-28.4N 003-12.7E MARKED BY LIGHT BUOY.\r\nNNNN\r\n";
    const std::string text = "010530 UTC OCT 26\nNAVAREA I WARNING 123.\nSOUTH NORTH SEA. WRECK 53-28.4N 003-12.7E MARKED BY LIGHT BUOY.";
    const auto bits = makeBits(body, 60, 24);
    {   // clean
        Got g = run(bits, false, 1.f);
        CHECK(g.m.size() == 1, "one message, got %zu", g.m.size());
        if (!g.m.empty()) {
            const auto& m = g.m[0];
            CHECK(m.complete && m.station == 'E' && m.subject == 'A' && m.number == 1, "header %c %c %d", m.station, m.subject, m.number);
            CHECK(m.text == text, "text: '%s'", m.text.c_str());
            CHECK(m.errors == 0, "no errors");
            CHECK(m.subjectName == std::string("Navigational warning"), "subject name");
        }
    }
    {   // inverted line: the decoder with the other polarity reads it, the wrong one does not
        std::vector<uint8_t> inv = bits; for (auto& b : inv) b ^= 1;
        Got g = run(inv, true, 1.f), w = run(inv, false, 1.f);
        CHECK(g.m.size() == 1 && g.m[0].text == text, "inverted polarity");
        CHECK(w.m.empty(), "wrong polarity gives nothing");
    }
    for (int skip = 1; skip < 7; skip++) {
        Got g = run(bits, false, 1.f, skip);
        CHECK(g.m.size() == 1 && g.m[0].text == text, "bit offset %d", skip);
    }
    // bit errors: with hard bits at 1 % a character is lost only when both copies are hit (about 0.5 % of the characters)
    {
        int msgs = 0, complete = 0; uint32_t wrong = 0, chs = 0;
        for (int seed = 1; seed <= 40; seed++) {
            Got g = run(bits, false, 0.8f, 0, 0.01, seed);
            if (g.m.empty()) continue;
            msgs++; if (g.m[0].complete) complete++;
            const std::string& t = g.m[0].text;
            wrong += lev(t, text);
            chs += (uint32_t)text.size();
        }
        printf("BER 1%%: %d/40 messages (%d complete), %u of %u characters wrong or unreadable (%.2f %%)\n", msgs, complete, wrong, chs, 100.0 * wrong / chs);
        CHECK(msgs >= 39 && complete >= 38 && wrong < 0.012 * chs, "1%% bit errors: %d messages, %d complete, %u wrong", msgs, complete, wrong);
    }
    // a burst that kills whole characters every 70 bits is covered by the retransmission 5 places later
    {
        Got g = run(bits, false, 1.f, 0, 0, 3, 140);
        CHECK(g.m.size() == 1 && g.m[0].complete, "bursts");
        if (!g.m.empty()) printf("bursts of 7 random bits every 140 bits: %u errors in %u characters\n", g.m[0].errors, g.m[0].chars);
        if (!g.m.empty()) CHECK(g.m[0].cer < 0.05, "burst CER %.3f", g.m[0].cer);
    }
    // two messages in one transmission
    {
        std::vector<int> tok(40, -1);
        for (uint8_t c : navtexEncode("ZCZC AB02\r\nGALE WARNING 7\r\nNNNN\r\n")) tok.push_back(c);
        for (int i = 0; i < 30; i++) tok.push_back(-1);
        for (uint8_t c : navtexEncode("ZCZC EE03\r\nFORECAST: SOUTHWEST 5 TO 7\r\nNNNN\r\n")) tok.push_back(c);
        for (int i = 0; i < 20; i++) tok.push_back(-1);
        Got g = run(codesToBits(sitorBSlots(tok)), false, 1.f);
        CHECK(g.m.size() == 2, "two messages, got %zu", g.m.size());
        if (g.m.size() == 2) CHECK(g.m[0].subject == 'B' && g.m[0].number == 2 && g.m[1].subject == 'E' && g.m[1].number == 3 && g.m[1].text == "FORECAST: SOUTHWEST 5 TO 7", "second message");
    }
    // noise only: no lock, no message
    {
        NavtexDecoder d; int n = 0; d.setCallback([&](const NavtexMessage&) { n++; });
        std::mt19937 rng(9);
        for (int i = 0; i < 400000; i++) d.pushBit((rng() & 1) ? 1.f : -1.f);
        CHECK(n == 0, "no message from noise");
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
