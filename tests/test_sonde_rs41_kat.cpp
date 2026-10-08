// RS41: known answers. The example frame is the one printed in rs1729/RS rs41/rs41.txt (de-whitened bytes; the printed copy has a stray
// nibble in the parity and loses the last zeros, repaired here: the parity is then confirmed by our own RS encoder, which is a check
// of the code's polynomial and root choice, and every block CRC is confirmed). Plus RS error correction and the symbol path.
#include "dect2/sonde_rs41.h"
#include "dect2/sonde_geo.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char* kFrameHex =
    "8635f44093df1a602c87e0fa0521e8943d9cef4c7a67393f6d39fb546461f2111b6447ab79a746c80350cda5344157f8c0c12234f46902220f792816174b3139"
    "33303239331a00000300000a00002f0007322ce53e31991abf12dada3eb68468c16755d51c7a2a15310216060245f302000d08a31607821e08bb210219060243"
    "f302000000000000000000000000000000220d7c1e0807d03cdc071fd81ddb19d70a8d0eb602b60cb518d40692ff00ff00ff001c277d59b8d83301ff0f881f0f"
    "38f4fe18b283038735ff000000003eb8ff4947201e6e3aff55415f13fc6e005440440cf100009e9f7406f85800832b631719d70010bebc172a8b000000000000"
    "00000000000000000000000000000000a48b7b15366181193ef05d07e1245b1be0f721f801f60804107b0b76110000000000000000000000000000000000ecc7";

static std::vector<uint8_t> unhex(const std::string& h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16));
    return v;
}

int main() {
    std::vector<uint8_t> doc = unhex(kFrameHex);
    CHECK(doc.size() == 320, "example frame length %zu", doc.size());

    // whitening: the on-air header is 10 B6 CA 11 22 96 12 F8 (rs1729 header bit string), first mask bytes
    const uint8_t* mk = rs41Mask();
    CHECK(mk[0] == 0x96 && mk[1] == 0x83 && mk[2] == 0x3E && mk[3] == 0x51 && mk[15] == 0x26 && mk[63] == 0xC1, "mask");
    const uint8_t air[8] = {0x10, 0xB6, 0xCA, 0x11, 0x22, 0x96, 0x12, 0xF8};
    for (int i = 0; i < 8; i++) CHECK((doc[(size_t)i] ^ mk[i]) == air[i], "header byte %d", i);

    // CRC of every block of the published frame
    int blocks = 0;
    for (int pos = 0x39; pos + 4 <= 0x140; ) {
        const int bl = doc[(size_t)pos + 1];
        const uint16_t c = (uint16_t)(doc[(size_t)pos + 2 + (size_t)bl] | (doc[(size_t)pos + 3 + (size_t)bl] << 8));
        CHECK(rs41Crc16(&doc[(size_t)pos + 2], bl) == c, "CRC of block 0x%02X at 0x%X", doc[(size_t)pos], pos);
        blocks++;
        pos += bl + 4;
    }
    CHECK(blocks == 6, "blocks %d", blocks);

    // RS parity of the published frame
    for (int w = 0; w < 2; w++) {
        uint8_t data[132], par[24];
        for (int i = 0; i < 132; i++) data[i] = doc[(size_t)(56 + 2 * i + w)];
        rs41RsParity(data, 132, par);
        CHECK(std::memcmp(par, &doc[(size_t)(8 + 24 * w)], 24) == 0, "published parity of codeword %d", w + 1);
    }

    // RS decoding: up to 12 errors per codeword, 13 or more are refused (a wrong "correction" would be a failure of the code, so count)
    {
        std::mt19937 rng(7);
        int okAll = 0, tried = 0, refused = 0, wrong = 0;
        for (int trial = 0; trial < 400; trial++) {
            uint8_t cw[24 + 132];
            uint8_t data[132];
            for (auto& d : data) d = (uint8_t)rng();
            rs41RsParity(data, 132, cw);
            std::memcpy(cw + 24, data, 132);
            uint8_t orig[156];
            std::memcpy(orig, cw, 156);
            const int ne = trial % 16;       // 0..15
            int used[16];
            for (int e = 0; e < ne; e++) {
                int p; bool dup;
                do { p = (int)(rng() % 156); dup = false; for (int k = 0; k < e; k++) dup |= used[k] == p; } while (dup);
                used[e] = p;
                cw[p] ^= (uint8_t)(1 + rng() % 255);
            }
            const int r = rs41RsDecode(cw, 156);
            tried++;
            if (ne <= 12) {
                if (r == ne && std::memcmp(cw, orig, 156) == 0) okAll++;
                else CHECK(false, "RS %d errors: result %d", ne, r);
            } else {
                if (r < 0) refused++; else wrong++;
            }
        }
        CHECK(okAll > 300, "RS corrections ok %d", okAll);
        CHECK(refused > 3 * wrong, "13..15 errors: refused %d, mis-corrected %d", refused, wrong);
        printf("rs: %d ok of <=12 errors, >12 errors: %d refused, %d mis-corrected (of %d trials)\n", okAll, refused, wrong, tried);
    }

    // decode the published frame as bytes
    Rs41Decoder dec;
    {
        std::vector<uint8_t> f = doc;
        SondeFix fx;
        CHECK(dec.decodeFrame(f.data(), 320, fx), "decodeFrame");
        CHECK(fx.crcOk, "crcOk");
        CHECK(fx.serial == "K1930293", "serial '%s'", fx.serial.c_str());
        CHECK(fx.frame == 5910, "frame %d", fx.frame);
        CHECK(fx.hasPos && std::fabs(fx.lat - 46.050263) < 2e-5 && std::fabs(fx.lon - 16.110771) < 2e-5 && std::fabs(fx.altM - 28410.0) < 1.0,
              "position %.6f %.6f %.1f", fx.lat, fx.lon, fx.altM);
        CHECK(fx.sats == 8, "sats %d", fx.sats);
        CHECK(fx.hasVel && std::fabs(fx.hSpeed - 13.60) < 0.05 && std::fabs(fx.vSpeed + 36.17) < 0.05 && std::fabs(fx.headingDeg - 272.75) < 0.1,
              "velocity h %.2f v %.2f dir %.1f", fx.hSpeed, fx.vSpeed, fx.headingDeg);
        CHECK(fx.hasTime && std::fabs(fx.unixTime - 1404736656.0) < 0.01, "time %.2f", fx.unixTime);
        CHECK(std::fabs(fx.batteryV - 2.6) < 1e-6, "battery %.2f", fx.batteryV);
        CHECK(!fx.hasTemp && fx.note == "calibrating 1/51", "calibration note '%s' temp %d", fx.note.c_str(), (int)fx.hasTemp);
        CHECK(fx.corrected == 0, "corrected %d", fx.corrected);
    }
    // the same through the symbol path, with the polarity inverted, a preamble and bit errors that RS must repair
    {
        std::vector<uint8_t> sym = rs41Symbols(doc, true);
        std::mt19937 rng(3);
        for (int i = 0; i < 12; i++) sym[320 + 64 + (size_t)(rng() % (sym.size() - 400))] ^= 1;
        for (auto& s : sym) s ^= 1;
        std::vector<uint8_t> all(777, 0);
        for (size_t i = 0; i < all.size(); i++) all[i] = (uint8_t)(rng() & 1);
        all.insert(all.end(), sym.begin(), sym.end());
        Rs41Decoder d2;
        std::vector<SondeFix> out;
        d2.push(all.data(), all.size(), 0.0, out);
        CHECK(out.size() == 1, "frames from symbols: %zu", out.size());
        if (out.size() == 1) {
            CHECK(out[0].crcOk && out[0].serial == "K1930293" && out[0].frame == 5910, "symbol path crc %d serial '%s'", (int)out[0].crcOk, out[0].serial.c_str());
            CHECK(out[0].corrected >= 1, "bit errors repaired: %d", out[0].corrected);
            CHECK(out[0].hasPos && std::fabs(out[0].altM - 28410.0) < 1.0, "alt");
        }
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
