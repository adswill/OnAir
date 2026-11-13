// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC/packet codes: Golay header, BCH of line 625, Hamming 8/4, CRC, interleaving, companded sound blocks, SI data groups.
#include "dect2/mac/mac_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
using namespace dect2::mac;
#include "../common/test_util.h"

int main() {
    std::mt19937 rng(7);
    // Golay (23,12): every 12-bit word, three errors corrected
    {
        int bad = 0;
        for (int w = 0; w < 4096; w++) {
            uint8_t info[12], cw[23], out[12];
            for (int i = 0; i < 12; i++) info[i] = (w >> i) & 1;
            golay23Encode(info, cw);
            for (int k = 0; k < 3; k++) cw[rng() % 23] ^= 1;
            golay23Decode(cw, out);
            if (memcmp(info, out, 12)) bad++;
        }
        CHECK(bad == 0, "golay: %d words wrong", bad);
    }
    // BCH (71,57) / (94,80): clean, one error corrected, two detected (mostly)
    for (int n : {71, 94}) {
        const int k = n - 14;
        int bad = 0, det = 0;
        for (int t = 0; t < 300; t++) {
            uint8_t m[94], cw[94], c2[94];
            for (int i = 0; i < k; i++) m[i] = rng() & 1;
            bchEncode(m, k, cw);
            memcpy(c2, cw, 94);
            if (bchDecode(c2, n) != 0) bad++;
            c2[rng() % n] ^= 1;
            if (bchDecode(c2, n) != 1 || memcmp(c2, cw, (size_t)n)) bad++;
            const int a = (int)(rng() % n); int b = (int)(rng() % n); if (b == a) b = (a + 1) % n;
            c2[a] ^= 1; c2[b] ^= 1;
            if (bchDecode(c2, n) == -1 || memcmp(c2, cw, (size_t)n)) det++;
        }
        CHECK(bad == 0, "bch %d: %d failures", n, bad);
        CHECK(det > 250, "bch %d: two errors seen in only %d of 300", n, det);
    }
    // Hamming 8/4 (Part 3 Table 4 values) and its single-error correction
    {
        CHECK(ham84(0) == 0x15 && ham84(1) == 0x02 && ham84(15) == 0xEA, "ham84 table: %02X %02X %02X", ham84(0), ham84(1), ham84(15));
        int bad = 0;
        for (int d = 0; d < 16; d++) for (int b = -1; b < 8; b++) if (unham84((uint8_t)(ham84(d) ^ (b >= 0 ? 1 << b : 0))) != d) bad++;
        CHECK(bad == 0, "unham84: %d", bad);
    }
    // interleaving is a permutation; packets survive the round trip
    {
        const auto& p = interleave();
        std::vector<int> seen(kPacketBits, 0);
        for (int v : p) seen[(size_t)v]++;
        bool ok = p.size() == (size_t)kPacketBits && p[0] == 0 && p[1] == 94 && p[8] == 1;
        for (int s : seen) ok &= s == 1;
        CHECK(ok, "interleave");
        Packet a; a.address = 345; a.ci = 2;
        for (int i = 0; i < kPacketBytes; i++) a.data[i] = (uint8_t)(i * 13 + 1);
        uint8_t bits[kPacketBits];
        packetBits(a, bits);
        bits[5] ^= 1; bits[400] ^= 1;
        Packet b;
        parsePacket(bits, b);
        CHECK(b.address == 345 && b.ci == 2, "packet header %d %d", b.address, b.ci);
    }
    // companded sound: a tone through the 14/10 law, scale factors from the parity bits
    {
        int16_t x[64], y[64];
        double err = 0, sig = 0;
        for (int blk = 0; blk < 20; blk++) {
            const double amp = 8000 * std::pow(0.5, blk % 7);
            for (int i = 0; i < 64; i++) x[i] = (int16_t)std::lround(amp * std::sin(0.2 * (blk * 64 + i)));
            uint8_t b[90];
            soundEncode(x, false, b);
            int rg[2];
            const int e = soundDecode(b, false, y, rg);
            CHECK(e == 0, "sound block %d: %d parity errors", blk, e);
            for (int i = 0; i < 64; i++) { err += (x[i] - y[i]) * (double)(x[i] - y[i]); sig += (double)x[i] * x[i]; }
        }
        const double snr = 10 * std::log10(sig / (err + 1));
        CHECK(snr > 45, "companding SNR %.1f dB", snr);
        printf("  companded sound: %.1f dB\n", snr);
    }
    // a service-identification data group through the assembler
    {
        SiCommand c; c.ci = kCiTv;
        SiParam s; s.pi = kPiSref; s.field = {1, 'T', 'E', 'S', 'T'};
        SiParam pg; pg.pi = kPiPg;
        SiParam ac; ac.pi = kPiAccm; ac.field = {0x81, 0x5A, kCaEuroCrypt};
        pg.group = {ac};
        c.params = {s, pg};
        auto pks = siDataGroup(3, 5, {c});
        SiAssembler as;
        int tg = -1; std::vector<SiCommand> out; bool got = false;
        int ci = 0;
        for (auto& p : pks) { p.ci = ci++ & 3; got |= as.push(p, tg, out); }
        CHECK(got && tg == 3 && out.size() == 1 && out[0].params.size() == 2, "SI group");
        if (got) {
            CHECK(siText(out[0].params[0].field, 1) == "TEST", "SI name '%s'", siText(out[0].params[0].field, 1).c_str());
            CHECK(out[0].params[1].group.size() == 1 && out[0].params[1].group[0].field[2] == kCaEuroCrypt, "PG / ACCM");
        }
        pks[0].data[30] ^= 4;
        got = false;
        for (auto& p : pks) got |= as.push(p, tg, out);
        CHECK(!got, "a damaged packet was accepted");
    }
    // line 625 special data
    {
        Sdf s; s.chid = 0xBEEF; s.mvscg = 0x1 | 0x2 | 0x4 | 0x10 | (0x6 << 5); s.cafcnt = 0x12345;
        Rdf r[5]; for (int k = 0; k < 5; k++) r[k] = {k + 1, 77, false, 1, 2, 3, 4, 5, 6, true};
        uint8_t b[546];
        line625Encode(s, r, 0x1F, b);
        b[20] ^= 1; b[200] ^= 1;
        Sdf s2; Rdf r2[5]; bool ok = false;
        const int good = line625Decode(b, s2, r2, ok);
        CHECK(ok && good == 6 && s2.chid == 0xBEEF && s2.cafcnt == 0x12345 && r2[2].fcnt == 77 && r2[4].tdmcid == 5, "line 625 (%d good)", good);
        CHECK(s2.wide() && s2.scrambled() && s2.controlled() && s2.singleCut(), "MVSCG flags");
    }
    printf(fails ? "mac proto: %d FAILED\n" : "mac proto: all passed\n", fails);
    return fails ? 1 : 0;
}
