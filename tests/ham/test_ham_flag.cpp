// Callsign -> ISO country code for the flag chip (#708): portable forms, prefixed calls, parts of countries, every built-in entity mapped.
#include "dect2/ham/ham_flag.h"
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    CHECK(callIso("DL1ABC") == "DE");
    CHECK(callIso("G4ABC") == "GB");
    CHECK(callIso("GM4ABC") == "GB");               // Scotland is part of GB
    CHECK(callIso("W1AW") == "US");
    CHECK(callIso("KH6ABC") == "US");               // Hawaii
    CHECK(callIso("JA1ABC") == "JP");
    CHECK(callIso("LY1ABC/P") == "LT");             // portable suffixes do not change the country
    CHECK(callIso("LY1ABC/M") == "LT");
    CHECK(callIso("EA8/G4ABC") == "ES");            // the prefix in front is where the station is (Canary Islands)
    CHECK(callIso("DL/LY1ABC") == "DE");
    CHECK(callIso("LY1ABC/MM") == "");              // maritime mobile: no country
    CHECK(callIso("") == "");
    CHECK(callIso("<...>") == "");
    // every entity of the built-in table has a code (a new row in the table needs a row in ham_flag.cpp)
    for (const DxccEntity& e : DxccDb::builtin().entities()) {
        if (entityIso(e.name).empty()) { printf("FAIL no ISO code for entity %s\n", e.name.c_str()); fails++; }
    }
    // a user file that spells the name differently falls back to the built-in table
    DxccDb u; std::string err;
    CHECK(u.loadCty("Fed. Rep. of Germany:  14:  28:  EU:   51.00:   -10.00:    -1.0:  DL:\n    DA,DB,DC,DD,DF,DG,DH,DJ,DK,DL;\n", err));
    CHECK(callIso("DL1ABC", u) == "DE");
    CHECK(isoFlagEmoji("DE") == "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA");   // U+1F1E9 U+1F1EA
    CHECK(isoFlagEmoji("de").empty() && isoFlagEmoji("D").empty() && isoFlagEmoji("").empty());
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
