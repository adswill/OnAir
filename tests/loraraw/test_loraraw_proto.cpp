// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw): the coding chain for every spreading factor and coding rate. Frames are encoded (mesh_lora.h) and decoded again symbol by symbol:
// SF5 - SF12, 4/5 - 4/8, CRC on / off, low data rate optimisation on / off, explicit and implicit header, payloads of 1 to 255 bytes, a bin error
// at 4/7 and 4/8 (corrected), a damaged payload (reported through the CRC), the air time against the Semtech formula and the LDRO rule.
#include "dect2/mesh/mesh_lora.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2::lora;
#include "../common/test_util.h"

static bool roundTrip(const Params& p, const std::vector<uint8_t>& pl, int flipBin = -1, int* corr = nullptr) {
    std::vector<uint16_t> d = encode(p, pl.data(), pl.size());
    if (flipBin >= 0 && flipBin < (int)d.size()) d[flipBin] ^= 1;
    FrameDecoder fd;
    if (p.implicitHdr) fd.startImplicit(p.sf, p.ldro, (int)pl.size(), p.cr, p.crc); else fd.start(p.sf, p.ldro);
    for (uint16_t b : d) fd.push(b);
    if (!fd.done() || (int)d.size() != fd.needed()) return false;
    if (!p.implicitHdr && (fd.header().len != (int)pl.size() || fd.header().cr != p.cr || fd.header().crc != p.crc)) return false;
    std::vector<uint8_t> out;
    bool crcOk = !p.crc;
    int c = 0;
    if (!fd.finish(out, crcOk, c)) return false;
    if (corr) *corr = c;
    return out == pl && (!p.crc || crcOk);
}

int main() {
    std::mt19937 g(7);
    int n = 0, bad = 0;
    for (int sf = 5; sf <= 12; sf++)
        for (int cr = 5; cr <= 8; cr++)
            for (int crc = 0; crc < 2; crc++)
                for (int ldro = 0; ldro < 2; ldro++)
                    for (int impl = 0; impl < 2; impl++)
                        for (int len : {1, 2, 7, 16, 33, 64, 128, 255}) {
                            Params p;
                            p.sf = sf; p.cr = cr; p.crc = crc; p.ldro = ldro; p.implicitHdr = impl; p.implicitLen = len;
                            std::vector<uint8_t> pl((size_t)len);
                            for (auto& b : pl) b = (uint8_t)g();
                            n++;
                            if (!roundTrip(p, pl)) { bad++; if (bad < 6) printf("  sf %d cr %d crc %d ldro %d implicit %d len %d\n", sf, cr, crc, ldro, impl, len); }
                        }
    CHECK(bad == 0, "%d of %d round trips failed", bad, n);
    printf("  %d coding round trips\n", n);
    // a single bit error in a payload codeword is corrected at 4/7 and 4/8 (Hamming)
    for (int cr : {7, 8}) {
        Params p; p.sf = 9; p.cr = cr; p.crc = true;
        std::vector<uint8_t> pl(20);
        for (auto& b : pl) b = (uint8_t)g();
        int c = 0;
        const bool r = roundTrip(p, pl, 8 + cr * 2, &c);
        CHECK(r && c >= 1, "4/%d: one bin error not corrected (ok %d, corrected %d)", cr, r, c);
    }
    {   // a damaged payload is not passed off as good
        Params p; p.sf = 8; p.cr = 5; p.crc = true;
        std::vector<uint8_t> pl(20, 0x5A);
        auto d = encode(p, pl.data(), pl.size());
        d[14] ^= 0x15; d[15] ^= 0x2A;
        FrameDecoder fd; fd.start(p.sf, false);
        for (uint16_t b : d) fd.push(b);
        std::vector<uint8_t> out; bool ok = true; int c = 0;
        const bool fin = fd.done() && fd.finish(out, ok, c);
        CHECK(!fin || !ok || out != pl, "damage not noticed");
    }
    {   // SF7, 125 kHz, 10 bytes, 4/5, CRC, 8 preamble symbols, explicit header: 40.25 symbols of 1.024 ms
        Params p; p.sf = 7; p.bwHz = 125e3; p.cr = 5; p.crc = true; p.preamble = 8; p.ldro = false;
        const double t = airSeconds(p, 10);
        CHECK(std::fabs(t - 0.0412) < 0.0005, "air time %.5f s", t);
        CHECK(autoLdro(12, 125e3) && autoLdro(11, 125e3) && !autoLdro(10, 125e3) && !autoLdro(7, 125e3) && !autoLdro(11, 250e3) && autoLdro(12, 250e3), "LDRO rule");
    }
    printf(fails ? "loraraw proto: %d FAILED\n" : "loraraw proto: all passed\n", fails);
    return fails ? 1 : 0;
}
