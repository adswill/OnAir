// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Broadcast text profiles and ARIB STD-B24 designations and shifts (simulated strings).
#include "dect2/tv/dvb_text.h"
#include "dect2/rs/dvbsi.h"
#include <array>
#include <cstdio>
#include <limits>
#include <string>
using namespace dect2::dvbsi;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static std::string t(std::initializer_list<int> v) { std::string s; for (int x : v) s += (char)x; return aribText((const uint8_t*)s.data(), (int)s.size()); }
static void testTextAbi() {
    // Exercise the public C boundary from its C++ consumer, including the registered platform converter.
    const uint8_t raw[] = {'T', 'V', ' ', 0xD6, 0xD0};
    for (uint32_t legacy : {0u, 1u, 17u}) {
        std::array<uint8_t, 4 * sizeof(raw) + 16> a{}, b{}; // documented output capacity
        const size_t n = onair_dvbsi_dvb_text(raw, sizeof(raw), legacy, a.data(), a.size());
        const size_t m = onair_dvbsi_dvb_text_with_encoding(raw, sizeof(raw), legacy ? 1 : 0, b.data(), b.size());
        CHECK(n <= a.size() && m <= b.size(), "text ABI output bounds");
        if (n <= a.size() && m <= b.size()) {
            const std::string oldText((const char*)a.data(), n), newText((const char*)b.data(), m);
            CHECK(oldText == newText, "legacy text ABI maps every nonzero flag to GB");
            if (legacy) CHECK(newText == "TV 中", "text ABI uses the platform converter");
        }
        CHECK(onair_dvbsi_dvb_text_with_encoding(raw, sizeof(raw), 0, nullptr, 0) == std::numeric_limits<size_t>::max(), "empty ABI output reports overflow");
        CHECK(onair_dvbsi_dvb_text_with_encoding(nullptr, 10, 0, b.data(), b.size()) == 0, "null ABI input is empty");
    }
    std::array<uint8_t, 2> bounded = {0xA5, 0xA5};
    CHECK(onair_dvbsi_dvb_text_with_encoding(raw, sizeof(raw), 0, bounded.data(), 1) == std::numeric_limits<size_t>::max(), "short ABI output reports overflow");
    CHECK(bounded.at(1) == 0xA5, "text ABI respects the supplied output capacity");
}
int main() {
    testTextAbi();
    // alphanumeric through G1 (LS1), then G0 kanji (LS0): "NHK" + the kanji for "day" (JIS 0x467C)
    CHECK(t({0x0E, 'N', 'H', 'K'}) == "NHK", "LS1 alnum");
    CHECK(t({0x0E, 'A', 0x0F, 0x46, 0x7C}) == "A\xE6\x97\xA5", "kanji '%s'", t({0x0E, 'A', 0x0F, 0x46, 0x7C}).c_str());
    // hiragana in GR by default: 0x21 is the small a (U+3041)
    CHECK(t({0xA1}) == "\xE3\x81\x81", "hiragana");
    // katakana after designating G2 to katakana
    CHECK(t({0x1B, 0x2A, 0x31, 0xA1}) == "\xE3\x82\xA1", "katakana designate");
    // single shift SS3 for one katakana, then back
    CHECK(t({0x1D, 0x21, 0x0E, 'B'}) == "\xE3\x82\xA1" "B", "SS3");
    // CSI and colour controls skipped
    CHECK(t({0x0E, 'X', 0x90, 0x41, 0x9B, 0x20, 0x53, 'Y'}) == "XY", "controls '%s'", t({0x0E, 'X', 0x90, 0x41, 0x9B, 0x20, 0x53, 'Y'}).c_str());
    CHECK(looksArib((const uint8_t*)"\x1b(J", 3) && !looksArib((const uint8_t*)"plain", 5), "detect");
    const uint8_t day[] = {0x46, 0x7C};
    CHECK(dvbText(nullptr, 3, {TextEncoding::Isdb}).empty(), "null input stays empty with an ISDB profile");
    CHECK(dvbText(day, 2, {TextEncoding::Arib}) == "日", "7-bit ARIB Kanji uses the stream profile");
    CHECK(dvbText(day, 2, TextContext{TextEncoding::Isdb}.withLanguage("JPN")) == "日", "Japanese ISDB language profile");
    CHECK(dvbText(day, 2, {TextEncoding::Isdb, "JPN"}) == "日", "Japanese ISDB country profile");
    const uint8_t tagged[] = {0x15, 0xE6, 0x97, 0xA5};
    CHECK(dvbText(tagged, 4, {TextEncoding::Isdb}) == "日", "unresolved ISDB preserves an explicit DVB UTF-8 selector");
    const uint8_t cafe[] = {'C', 'a', 'f', 0xC2, 'e'};
    const uint8_t chinese[] = {0xD6, 0xD0};
    CHECK(dvbText(chinese, 2, {TextEncoding::Gb2312}) == "中", "explicit GB profile");
    CHECK(dvbText(cafe, 5) == "Café" && dvbText(day, 2) == "F|", "other streams keep the DVB default");
    const uint8_t brazil[] = {'C', 'a', 'f', 0xE9};
    CHECK(dvbText(brazil, 4, {TextEncoding::Isdb, "BRA"}) == "Café", "Brazilian ISDB Latin profile");
    CHECK(dvbText(tagged + 1, 3, {TextEncoding::Isdb, "PHL"}) == "日", "Philippine ISDB UTF-8 profile");
    const uint8_t nordic[] = {0x06, 0xA1, 0xAA};
    const uint8_t celtic[] = {0x0A, 0xA1, 0xA2};
    CHECK(dvbText(nordic, 3) == "ĄŠ", "ISO 8859-10 Nordic selector");
    CHECK(dvbText(celtic, 3) == "Ḃḃ", "ISO 8859-14 Celtic selector");
    printf(fails ? "arib FAILED\n" : "arib passed\n");
    return fails ? 1 : 0;
}
