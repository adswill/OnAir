// ATIS format (ETSI EN 300 698-1 annex B / C): the worked example of the standard, identity conversions, the frame the DSC decoder reads back.
#include "dect2/atis/atis_proto.h"
#include "dect2/marine/marine_dsc.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // Annex C, example 1: call sign PC8075, MID 244 -> 9 244 03 8075; example 2: the check symbol is 110
    const std::string id = atis::idFromCallsign("PC8075", 244);
    CHECK(id == "9244038075", "id %s", id.c_str());
    const auto body = atis::bodyFromDigits(id);
    CHECK(body == std::vector<int>({121, 92, 44, 3, 80, 75}), "body");
    CHECK(atis::checkSymbol(id) == 110, "ECC %d (the standard works out 110)", atis::checkSymbol(id));
    // the symbols of the example in their slots: DX 125 x6, 121, 121, 92, 44, 03, 80, 75, 127, ECC, 127, 127; RX 111 .. 104, 121, 121, 92, 44, 03, 80, 75, 127, ECC
    const auto slots = marine::dscFrameSymbols(body, atis::kEndOfSequence);
    const int dx[] = {125, 125, 125, 125, 125, 125, 121, 121, 92, 44, 3, 80, 75, 127, 110, 127, 127};
    const int rx[] = {111, 110, 109, 108, 107, 106, 105, 104, 121, 121, 92, 44, 3, 80, 75, 127, 110};
    bool layout = slots.size() == 34;
    for (int i = 0; layout && i < 17; i++) layout = slots[(size_t)(2 * i)] == dx[i] && slots[(size_t)(2 * i + 1)] == rx[i];
    CHECK(layout, "slot layout differs from annex C (size %zu)", slots.size());
    // 10-bit code of symbol 121 from table B.1: YBBYYYY BYB, bit 1 first
    {
        const unsigned b = marine::dscSymbolBits(121);
        std::string s;
        for (int i = 9; i >= 0; i--) s += ((b >> i) & 1) ? 'Y' : 'B';
        CHECK(s == "YBBYYYYBYB", "symbol 121 = %s", s.c_str());
        const unsigned b0 = marine::dscSymbolBits(0);
        std::string z; for (int i = 9; i >= 0; i--) z += ((b0 >> i) & 1) ? 'Y' : 'B';
        CHECK(z == "BBBBBBBYYY", "symbol 0 = %s", z.c_str());
        const unsigned b127 = marine::dscSymbolBits(127);
        std::string y; for (int i = 9; i >= 0; i--) y += ((b127 >> i) & 1) ? 'Y' : 'B';
        CHECK(y == "YYYYYYYBBB", "symbol 127 = %s", y.c_str());
    }
    // reading back
    const atis::Id d = atis::idFromSymbols(body);
    CHECK(d.valid && !d.partial && d.digits == id && d.standardZ && d.mid == 244, "read back");
    CHECK(d.country == "Netherlands (Kingdom of the)" || d.country.find("Netherlands") != std::string::npos, "country '%s'", d.country.c_str());
    CHECK(d.callsign == "PC8075" && d.firstLetterNote == "likely", "call sign %s", d.callsign.c_str());
    CHECK(d.mmsi == "244038075", "mmsi %s", d.mmsi.c_str());
    // an unreadable symbol, a wrong format specifier, too short
    std::vector<int> b2 = body; b2[3] = -1;
    const atis::Id p = atis::idFromSymbols(b2);
    CHECK(p.valid && p.partial && p.digits == "9244~~8075", "partial %s", p.digits.c_str());
    b2 = body; b2[0] = 120;
    CHECK(!atis::idFromSymbols(b2).valid, "wrong format specifier");
    CHECK(!atis::idFromSymbols({121, 1, 2}).valid, "too short");
    // a country with no known first letter, the 9 + MMSI form
    const atis::Id m = atis::idFromDigits(atis::idFromMmsi("207072225"));
    CHECK(m.valid && m.mmsi == "207072225" && m.country.find("Bulgaria") != std::string::npos, "mmsi form");
    CHECK(atis::idFromDigits("1234567890").standardZ == false, "does not start with 9");
    CHECK(atis::idFromCallsign("P1234", 244).empty() && atis::idFromCallsign("PC80X5", 244).empty(), "bad call signs");
    CHECK(atis::frameBits("12345").empty() && !atis::frameBits(id).empty(), "frame bits");
    // the frame: 20 dot bits + 34 symbols of 10 bits
    CHECK(atis::frameBits(id).size() == 20 + 34 * 10, "frame length %zu", atis::frameBits(id).size());

    // the DSC decoder reads the frame back, with errors repaired by the second copy and the check symbol
    for (int variant = 0; variant < 3; variant++) {
        std::vector<uint8_t> bits = atis::frameBits(id);
        if (variant == 1) bits[20 + 12 * 10 + 3] ^= 1;      // one bit error in the first copy of a symbol: the symbol is lost, the RX copy is used
        if (variant == 2) { bits[20 + 14 * 10 + 4] ^= 1; bits[20 + 20 * 10 + 6] ^= 1; }   // errors in two different symbols
        marine::DscDecoder dec(false, true);
        DscCall got; int n = 0;
        dec.setCallback([&](const DscCall& c) { got = c; n++; });
        for (int i = 0; i < 30; i++) dec.pushBit(0.f);
        for (uint8_t bit : bits) dec.pushBit(bit ? 0.9f : -0.9f);
        for (int i = 0; i < 100; i++) dec.pushBit(0.f);
        CHECK(n == 1 && got.eccOk && got.format == 121, "variant %d: calls %d ecc %d format %d", variant, n, (int)got.eccOk, got.format);
        const atis::Id r = atis::idFromSymbols(got.symbols);
        CHECK(r.valid && r.digits == id, "variant %d: id %s", variant, r.digits.c_str());
    }
    return testSummary("atis proto");
}
