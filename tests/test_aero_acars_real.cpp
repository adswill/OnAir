// Aero ACARS block layer, known answers.
// Source of the real block: the comment in JAERO's ParserISU::parse (JAERO/aerol.cpp), a captured ADS-C contract request
// (label A6) uplinked to HB-JHM. Its block check (93 AB) was verified here against CRC-16/KERMIT over the raw bytes
// from the mode to the ETX (the method acarsdec uses). CRC-16/KERMIT check value: the CRC catalogue lists 0x2189 for "123456789".
#include "dect2/aero_acars.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char* kReal =
    "FF FF 01 32 AE C8 C2 AD 4A C8 CD 15 C1 B6 51 02 2F D0 49 CB 43 D0 D9 C1 AE C1 C4 D3 AE C8 C2 AD 4A C8 CD B0 37 B0 34 "
    "B0 C2 B0 B0 B0 43 B0 B0 B0 C4 B0 31 B0 45 B0 31 31 B0 B0 B0 34 34 B0 C4 83 93 AB 7F";

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    std::istringstream is(s);
    std::string t;
    while (is >> t) v.push_back(uint8_t(strtol(t.c_str(), nullptr, 16)));
    return v;
}

int main() {
    CHECK(aeroAcarsCrc((const uint8_t*)"123456789", 9) == 0x2189, "KERMIT check value");

    const std::vector<uint8_t> real = hex(kReal);
    CHECK(real.size() == 67, "real block length %zu", real.size());
    AeroAcarsBlock b;
    CHECK(aeroParseAcarsBlock(real, b), "parse real block");
    CHECK(b.parityOk, "parity of the real block");
    CHECK(b.crcOk, "block check of the real block");
    CHECK(b.mode == "2", "mode '%s'", b.mode.c_str());
    CHECK(b.registration == ".HB-JHM", "registration '%s'", b.registration.c_str());
    CHECK(b.tak == 0x15, "tak %02X", b.tak);
    CHECK(b.label == "A6", "label '%s'", b.label.c_str());
    CHECK(b.blockId == 'Q', "block id %c", b.blockId);
    CHECK(b.hasText && !b.moreToCome, "flags");
    CHECK(b.text == "/PIKCPYA.ADS.HB-JHM07040B000C000D010E011000440D", "text '%s'", b.text.c_str());
    CHECK(aeroAcarsLabelText("A6") == "ADS-C uplink", "label text");

    // the builder reproduces the real bytes exactly
    AeroAcarsBlock c = b;
    c.registration = "HB-JHM";                         // builder pads with '.'
    const std::vector<uint8_t> built = aeroBuildAcarsBlock(c);
    CHECK(built == real, "builder output equals the real block (%zu bytes)", built.size());

    // errors are seen
    { auto v = real; v[30] ^= 0x01; AeroAcarsBlock x; CHECK(aeroParseAcarsBlock(v, x) && !x.parityOk && !x.crcOk, "one bit flipped"); }
    { auto v = real; v[64] ^= 0x10; AeroAcarsBlock x; CHECK(aeroParseAcarsBlock(v, x) && x.parityOk && !x.crcOk, "bad BCS"); }
    { auto v = real; v[0] = 0x00; AeroAcarsBlock x; CHECK(!aeroParseAcarsBlock(v, x), "no pre-key"); }
    { auto v = real; v.resize(10); AeroAcarsBlock x; CHECK(!aeroParseAcarsBlock(v, x), "short"); }

    // ETB, no text, general response label, DEL in the label
    AeroAcarsBlock g;
    g.mode = "2"; g.registration = "A6-EEB"; g.label = "_d"; g.blockId = 'A'; g.hasText = false; g.moreToCome = false;
    auto gv = aeroBuildAcarsBlock(g);
    AeroAcarsBlock gp;
    CHECK(gv.size() == 19 && aeroParseAcarsBlock(gv, gp), "no-text block %zu", gv.size());
    CHECK(gp.label == "_d" && gp.crcOk && gp.parityOk && !gp.hasText, "no-text block fields");
    CHECK(gv[13] == 0x7F || (gv[13] & 0x7F) == 0x7F, "DEL byte of label _d");
    CHECK(aeroAcarsLabelText("_d") == "General response" && aeroAcarsLabelText("QQ").empty(), "label texts");

    AeroAcarsBlock m;
    m.mode = "2"; m.registration = "N123AB"; m.label = "H1"; m.blockId = 'B'; m.moreToCome = true; m.hasText = true;
    m.text = std::string(220, 'x');
    auto mv = aeroBuildAcarsBlock(m);
    AeroAcarsBlock mp;
    CHECK(aeroParseAcarsBlock(mv, mp) && mp.moreToCome && mp.crcOk && mp.text.size() == 220, "ETB block of 220 characters");

    if (fails) return 1;
    printf("aero_acars_real OK\n");
    return 0;
}
