// CEA-608 and 708 caption decoding from hand-built cc_data (simulated).
#include "dect2/tv/cc_decode.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static uint8_t par(uint8_t b) { int n = 0; for (int i = 0; i < 7; i++) n += (b >> i) & 1; return (n & 1) ? b : (uint8_t)(b | 0x80); }
struct Tx { std::vector<uint8_t> d; void pair(uint8_t a, uint8_t b) { d.push_back(0xFC); d.push_back(par(a)); d.push_back(par(b)); } void ctl(uint8_t b) { pair(0x14, b); pair(0x14, b); } void text(const char* s) { while (s[0] && s[1]) { pair((uint8_t)s[0], (uint8_t)s[1]); s += 2; } if (s[0]) pair((uint8_t)s[0], 0); } };
int main() {
    {   // pop-on
        Tx t; t.ctl(0x20); t.ctl(0x2E); t.pair(0x14, 0x70); t.pair(0x14, 0x70); t.text("HELLO WORLD"); t.ctl(0x2F); CaptionDecoder d; d.feed(t.d.data(), (int)t.d.size() / 3, 1.0); Tx u; u.ctl(0x20); u.ctl(0x2F); d.feed(u.d.data(), (int)u.d.size() / 3, 4.0);
        CHECK(d.cues().size() == 1 && d.cues()[0].text == "HELLO WORLD" && d.cues()[0].start == 1.0 && d.cues()[0].end == 4.0, "pop-on: %zu", d.cues().size());
    }
    {   // roll-up with carriage return, special character, repeated control codes
        Tx t; t.ctl(0x25); t.text("FIRST LINE"); t.ctl(0x2D); t.text("SECOND"); t.pair(0x11, 0x37); t.ctl(0x2D); CaptionDecoder d; d.feed(t.d.data(), (int)t.d.size() / 3, 5.0);
        CHECK(d.cues().size() == 2 && d.cues()[0].text == "FIRST LINE", "roll-up");
        if (d.cues().size() == 2) CHECK(d.cues()[1].text == "SECOND\xE2\x99\xAA", "special '%s'", d.cues()[1].text.c_str());
    }
    {   // parity error dropped, wrong channel ignored
        Tx t; t.ctl(0x20); t.text("GOOD"); t.d[t.d.size() - 2] ^= 0x01; t.pair(0x1C, 0x2C); t.ctl(0x2F); CaptionDecoder d; d.feed(t.d.data(), (int)t.d.size() / 3, 1.0); Tx u; u.ctl(0x2F); d.feed(u.d.data(), (int)u.d.size() / 3, 2.0);
        CHECK(!d.cues().empty() && d.cues()[0].text.find("GO") != std::string::npos, "parity");
    }
    {   // 708: service 1 text across a packet, a CR closes the line; window commands skipped
        std::vector<uint8_t> b = {0x98, 0, 0, 0, 0, 0, 0, 'C', 'a', 'p', 't', 'i', 'o', 'n', 0x0D, 'L', 'i', 'n', 'e', 0x0D, 0x03};
        std::vector<uint8_t> pkt = {0x00};
        pkt.push_back((uint8_t)(1 << 5 | (int)b.size()));
        pkt.insert(pkt.end(), b.begin(), b.end());
        while (pkt.size() % 2 != 0) pkt.push_back(0);
        std::vector<uint8_t> d;
        int code = ((int)pkt.size() + 1) / 2;
        pkt[0] = (uint8_t)(1 << 6 | code);
        for (size_t i = 0; i < pkt.size(); i += 2) { d.push_back(i == 0 ? 0xFF : 0xFE); d.push_back(pkt[i]); d.push_back(pkt[i + 1]); }
        CaptionDecoder c; c.feed(d.data(), (int)d.size() / 3, 7.0);
        CHECK(c.cc708Packets() == 1 && c.cues().size() == 2 && c.cues()[0].text == "Caption" && c.cues()[1].text == "Line", "708 got %zu cues", c.cues().size());
    }
    printf(fails ? "cc FAILED\n" : "cc passed\n");
    return fails ? 1 : 0;
}
