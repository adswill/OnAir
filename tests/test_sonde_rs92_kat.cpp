// RS92: known answers from the example frame printed in rs1729/RS rs92/rs92.txt (bytes as sent after Manchester decoding and byte
// framing): the block CRCs, the Reed-Solomon parity (a check that RS92 uses the same code as the RS41), the fields. Then the symbol path
// (Manchester, byte framing, both polarities, bit errors) and builder -> decoder round trips.
#include "dect2/sonde_rs92.h"
#include "dect2/sonde_rs41.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char* kFrameHex =
    "2a2a2a2a2a106510e81820204b34393533393334006100083d3d07b342bb3e9809d3bc3f754c963bebfb690cca2b0fd9670f00670f8c0f11a458111e"
    "8810da410d30430ddc84673db01c852167ab07298b3cb05a536e0fffcf4faf7f4f3fff8fcfffffffff7fe5918aef20e8110175879900d0e0e900061f"
    "8c048aa393009edea1fe2557dc0019549f04b5160d00ae998b00e12a4200370b8608cacc1900b3e08905bd60040126da9303e1d4b4006d04a0057346"
    "9700d6a98b00699f120195828f046ed7680503030000000000000000b27dff0202000200f0be2a40a7cd69b9ed0668ec12182e8560ea6dd0733612a1";

static std::vector<uint8_t> unhex(const std::string& h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16));
    return v;
}

int main() {
    std::vector<uint8_t> doc = unhex(kFrameHex);
    CHECK(doc.size() == 240, "frame length %zu", doc.size());
    // block CRCs (lengths are in 16 bit words)
    const int starts[4] = {0x06, 0x2A, 0x46, 0xC4};
    for (int s : starts) {
        const int bytes = 2 * doc[(size_t)s + 1];
        const uint16_t c = (uint16_t)(doc[(size_t)s + 2 + (size_t)bytes] | (doc[(size_t)s + 3 + (size_t)bytes] << 8));
        CHECK(rs41Crc16(&doc[(size_t)s + 2], bytes) == c, "CRC of block 0x%02X", doc[(size_t)s]);
    }
    // parity
    {
        uint8_t par[24];
        rs41RsParity(&doc[6], 210, par);
        CHECK(std::memcmp(par, &doc[216], 24) == 0, "published parity");
    }
    Rs92Decoder dec;
    {
        std::vector<uint8_t> f = doc;
        SondeFix fx;
        CHECK(dec.decodeFrame(f.data(), fx), "decodeFrame");
        CHECK(fx.crcOk && fx.corrected == 0, "crc %d corrected %d", (int)fx.crcOk, fx.corrected);
        CHECK(fx.serial == "K4953934" && fx.frame == 6376, "serial '%s' frame %d", fx.serial.c_str(), fx.frame);
        CHECK(fx.sats == 12, "tracked channels %d", fx.sats);
        CHECK(!fx.hasPos && !fx.hasTemp, "position and temperature stay off");
        CHECK(dec.towSeconds() == 562371, "time of week %d", dec.towSeconds());
        CHECK(dec.calibrationDone() == 1, "calibration %d", dec.calibrationDone());
        CHECK(fx.note.find("calibrating 1/32") == 0, "note '%s'", fx.note.c_str());
        // corrupt 12 bytes: the code repairs them
        std::mt19937 rng(5);
        std::vector<uint8_t> g = doc;
        for (int i = 0; i < 12; i++) g[(size_t)(10 + (int)(rng() % 200))] ^= (uint8_t)(1 + rng() % 255);
        SondeFix f2;
        dec.decodeFrame(g.data(), f2);
        CHECK(f2.crcOk && f2.corrected >= 8 && g == doc, "12 byte errors: crc %d corrected %d", (int)f2.crcOk, f2.corrected);
    }
    // symbol path: noise before, inverted polarity, bit errors
    for (int inv = 0; inv < 2; inv++) {
        std::vector<uint8_t> sym = rs92Symbols(doc);
        std::mt19937 rng(9 + (unsigned)inv);
        for (int i = 0; i < 10; i++) sym[200 + rng() % (sym.size() - 400)] ^= 1;
        if (inv) for (auto& s : sym) s ^= 1;
        std::vector<uint8_t> all(555);
        for (auto& a : all) a = (uint8_t)(rng() & 1);
        all.insert(all.end(), sym.begin(), sym.end());
        Rs92Decoder d2;
        std::vector<SondeFix> out;
        d2.push(all.data(), all.size(), 0.0, out);
        CHECK(out.size() == 1 && out[0].crcOk && out[0].serial == "K4953934", "symbols (inverted %d): %zu fixes", inv, out.size());
        if (out.size() == 1) CHECK(out[0].corrected >= 1, "repaired %d", out[0].corrected);
    }
    // builder -> decoder over 33 frames: calibration progress, frequency, time
    {
        Rs92Decoder d3;
        for (int fr = 0; fr < 33; fr++) {
            Rs92Truth t; t.serial = "P1234567"; t.frame = 100 + fr; t.unixTime = 1780272000.0 + fr; t.sats = 8; t.freqHz = 405.3e6;
            std::vector<uint8_t> sym = rs92Symbols(rs92Frame(t, fr));
            std::vector<SondeFix> out;
            d3.push(sym.data(), sym.size(), fr * 1.0, out);
            CHECK(out.size() == 1 && out[0].crcOk && out[0].frame == 100 + fr && out[0].sats == 8, "frame %d", fr);
        }
        CHECK(d3.calibrationDone() == 32, "calibration %d", d3.calibrationDone());
        CHECK(std::fabs(d3.announcedFreqHz() - 405.3e6) < 1.0, "frequency %.0f", d3.announcedFreqHz());
        CHECK(d3.towSeconds() == (int)std::floor(1780272000.0 + 32 + 18 - 315964800.0 - 604800.0 * std::floor((1780272000.0 + 32 + 18 - 315964800.0) / 604800.0)), "tow %d", d3.towSeconds());
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
