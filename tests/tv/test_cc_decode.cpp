// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

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
    {   // at(): what is on screen at a picture time, for the overlay over the picture
        Tx t; t.ctl(0x20); t.ctl(0x2E); t.text("HELLO WORLD"); t.ctl(0x2F);
        Tx e; e.ctl(0x2C);
        CaptionDecoder d;
        d.feed(t.d.data(), (int)t.d.size() / 3, 1.0);
        CHECK(d.any() && d.at(0.5).empty() && d.at(1.0) == "HELLO WORLD" && d.at(3.0) == "HELLO WORLD", "pop-on shown from its start, before it is erased: '%s'", d.at(3.0).c_str());
        d.feed(e.d.data(), (int)e.d.size() / 3, 4.0);
        CHECK(d.at(2.0) == "HELLO WORLD" && d.at(4.5).empty(), "pop-on: the cue covers 1 to 4 s, nothing after the erase");
        CaptionDecoder n;
        CHECK(!n.any() && n.at(1.0).empty(), "no caption data: nothing");
    }
    {   // at(): roll-up shows the row above with the new one; a row long before is not joined
        Tx r1; r1.ctl(0x25); r1.text("FIRST LINE");
        Tx cr; cr.ctl(0x2D);
        Tx r2; r2.text("SECOND");
        CaptionDecoder d;
        d.feed(r1.d.data(), (int)r1.d.size() / 3, 10.0);
        CHECK(d.at(10.5) == "FIRST LINE", "roll-up: the row being built: '%s'", d.at(10.5).c_str());
        d.feed(cr.d.data(), (int)cr.d.size() / 3, 12.0);
        d.feed(r2.d.data(), (int)r2.d.size() / 3, 12.5);
        CHECK(d.at(12.2) == "FIRST LINE", "roll-up: after the carriage return the row stays until the next one comes: '%s'", d.at(12.2).c_str());
        CHECK(d.at(12.6) == "FIRST LINE\nSECOND", "roll-up: two rows while the second is built: '%s'", d.at(12.6).c_str());
        d.feed(cr.d.data(), (int)cr.d.size() / 3, 14.0);
        CHECK(d.at(13.0) == "FIRST LINE\nSECOND", "roll-up: two rows once both are closed: '%s'", d.at(13.0).c_str());
        CHECK(d.at(15.0) == "FIRST LINE\nSECOND", "roll-up: they stay after the second carriage return: '%s'", d.at(15.0).c_str());
        CHECK(d.at(25.0).empty(), "roll-up: gone 10 s after the last row: '%s'", d.at(25.0).c_str());
        CHECK(d.at(11.0) == "FIRST LINE", "roll-up: before the second row, the first alone");
        Tx r3; r3.text("LATER");
        d.feed(r3.d.data(), (int)r3.d.size() / 3, 30.0);
        CHECK(d.at(30.5) == "LATER", "roll-up: a row 16 s after the last is shown alone: '%s'", d.at(30.5).c_str());
        CHECK(d.at(9.0).empty(), "nothing before the first row");
    }
    {   // at(): roll-up rows ended by a repeated roll-up command (as one US encoder does) instead of a carriage return
        Tx a; a.ctl(0x25); a.text("ONE");
        Tx b; b.ctl(0x25); b.text("TWO");
        Tx c; c.ctl(0x25);
        CaptionDecoder d;
        d.feed(a.d.data(), (int)a.d.size() / 3, 1.0);
        d.feed(b.d.data(), (int)b.d.size() / 3, 2.0);
        d.feed(c.d.data(), (int)c.d.size() / 3, 3.0);
        CHECK(d.at(2.5) == "ONE\nTWO" && d.at(4.0) == "ONE\nTWO", "roll-up by RU2: two rows, kept after the last: '%s' '%s'", d.at(2.5).c_str(), d.at(4.0).c_str());
    }
    printf(fails ? "cc FAILED\n" : "cc passed\n");
    return fails ? 1 : 0;
}
