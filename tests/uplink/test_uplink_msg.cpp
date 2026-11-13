// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S uplink message layer: every uplink format encoded and decoded, the AP field giving the address back (parity XOR CRC), the UF11
// interrogator codes (II and SI through CL), the Comm-B register from RR / DI / RRS, the UF16 resolution message, bit errors.
#include "dect2/uplink/uplink_msg.h"
#include <cstdio>
#include <random>

using namespace dect2::uplink;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Uplink dec(const Frame& f) { Uplink u; CHECK(decode(f.b, f.nbits, u), "decode UF%d", f.b[0] >> 3); return u; }

int main() {
    std::mt19937 rng(3);
    for (int i = 0; i < 2000; i++) {   // AP: the address back for any address and any content
        const uint32_t a = rng() & 0xFFFFFF;
        Frame f = uf45(4 + (i & 1), a, rng() & 7, rng() & 31, rng() & 7, rng() & 0xFFFF);
        CHECK(dec(f).addr == a, "UF4/5 address %06X", a);
        uint8_t ma[7]; for (auto& x : ma) x = rng();
        f = uf2021(20 + (i & 1), a, 0, 0, 0, 0, ma);
        CHECK(dec(f).addr == a && dec(f).nbits == 112, "UF20/21 address");
        uint8_t mc[10]; for (auto& x : mc) x = rng();
        f = uf24(a, rng() & 3, rng() & 15, mc);
        Uplink u = dec(f);
        CHECK(u.uf == 24 && u.addr == a, "UF24 address %06X uf %d", u.addr, u.uf);
    }
    {   // UF11: II and SI codes
        Uplink u = dec(uf11(0, 3, 0));
        CHECK(u.uf == 11 && u.allCallOk && u.ii == 3 && u.si == -1 && u.pr == 0, "UF11 II 3");
        u = dec(uf11(9, 1, 2));
        CHECK(u.allCallOk && u.si == 17 && u.ii == -1 && u.pr == 9, "UF11 SI 17 (CL 2): si %d", u.si);
        u = dec(uf11(0, 15, 4));
        CHECK(u.si == 63, "UF11 SI 63");
        Frame f = uf11(0, 3, 0);
        for (int bit = 0; bit < 56; bit++) {   // any single bit error breaks the all-call parity
            Frame g = f; g.b[bit / 8] ^= 0x80 >> (bit % 8);
            Uplink v; decode(g.b, 56, v);
            CHECK(!(v.uf == 11 && v.allCallOk), "UF11 bit %d error not seen", bit);
        }
    }
    {   // Comm-B registers
        Uplink u = dec(uf45(4, 0x4CA2B1, 0, 16 + 4, 7, sdIis(5, 0)));
        CHECK(u.bds1 == 4 && u.bds2 == 0 && u.ii == 5 && bdsText(u.bds1, u.bds2) == "4,0", "BDS 4,0 / IIS 5");
        u = dec(uf45(5, 0x4CA2B1, 0, 16 + 1, 7, sdIis(2, 7)));
        CHECK(u.bds1 == 1 && u.bds2 == 7, "BDS 1,7 via RRS");
        u = dec(uf45(5, 0x4CA2B1, 0, 16 + 6, 3, sdSis(40, 1, 0)));
        CHECK(u.si == 40 && u.lss == 1 && u.bds1 == 6 && u.ii == -1, "DI 3 SIS 40");
        u = dec(uf45(4, 0x4CA2B1, 0, 16, 0, 0));
        CHECK(u.bds1 == 0, "air-initiated Comm-B");
        u = dec(uf45(4, 0x4CA2B1, 0, 2, 0, 0));
        CHECK(u.bds1 == -1, "RR 2: no Comm-B");
    }
    {   // ACAS
        Uplink u = dec(uf0(0x3C6586, 1, 1, 0x30));
        CHECK(u.uf == 0 && u.addr == 0x3C6586 && u.rl == 1 && u.aq == 1 && u.ds == 0x30, "UF0");
        uint8_t mu[7];
        raMu(mu, 0x4CA2B1, 1, 2, 1, 0, 0, 0x15, 0x9);
        u = dec(uf16(0x3C6586, 0, 0, mu));
        CHECK(u.uf == 16 && u.ra && u.mid == 0x4CA2B1 && u.addr == 0x3C6586 && u.vrc == 1 && u.cvc == 2 && u.mtb == 1 && u.hsb == 0x15 && u.vsb == 9, "UF16 RA");
        CHECK(describe(u).find("4CA2B1") != std::string::npos, "UF16 text: %s", describe(u).c_str());
    }
    {   // length and undefined formats
        Frame f = uf11(0, 3, 0);
        Uplink u;
        CHECK(!decode(f.b, 112, u), "UF11 as 112 bits");
        f.b[0] = 7 << 3;
        CHECK(!decode(f.b, 56, u), "UF7 undefined");
    }
    printf(fails ? "uplink msg: %d FAILED\n" : "uplink msg: all passed\n", fails);
    return fails ? 1 : 0;
}
