// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT A-field: the CRC, the header fields, the RFPI split, the carrier raster.
#include "dect2/dect/dect_proto.h"
#include <cmath>
#include <cstdio>
#include "../common/test_util.h"
int main() {
    using namespace dect;
    // the sync words of a base station and a handset are each other's complement
    CHECK((kSyncRfp ^ kSyncPp) == 0xFFFFFFFFu, "sync words");
    const uint64_t tail = 0x0100A5C3D1ULL;
    const uint64_t f = makeAField(TaNt, 1, 0, 0, tail);
    AField a = parseAField(f);
    CHECK(a.crcOk && !a.crcInverted && a.ta == TaNt && a.q1 == 1 && a.tail == tail, "parse: ta %d tail %llx crc %d", a.ta, (unsigned long long)a.tail, a.crcOk);
    a = parseAField(makeAField(TaQt, 0, 3, 1, 0x1122334455ULL, true));
    CHECK(a.crcOk && a.crcInverted && a.ta == TaQt && a.ba == 3 && a.q2 == 1, "complemented CRC form");
    for (int bit = 0; bit < 64; bit++) { a = parseAField(f ^ (1ULL << bit)); CHECK(!a.crcOk, "a single flipped bit (%d) passes the CRC", bit); }
    // the CRC is linear: crc(a ^ b) == crc(a) ^ crc(b) with a zero start value
    CHECK((rcrc(0x123456789ABCULL) ^ rcrc(0x0F0F0F0F0F0FULL)) == rcrc(0x123456789ABCULL ^ 0x0F0F0F0F0F0FULL), "linearity");
    const Rfpi r = rfpiOf(((uint64_t)0 << 35) | ((uint64_t)0x0123 << 19) | ((uint64_t)0x1ABCD << 2));
    CHECK(r.arc == 0 && r.emc == 0x0123 && r.fpn == 0x1ABCD, "RFPI split: emc %x fpn %x", r.emc, r.fpn);
    CHECK(carrierOf(1897.344) == 0 && carrierOf(1881.792) == 9 && carrierOf(1890.0) == -1 && carrierOf(1896.0) == -1 && carrierOf(1895.616) == 1, "carrier raster");
    CHECK(carrierMhz(9) > 1881.79 && carrierMhz(9) < 1881.80, "carrier 9 %.3f", carrierMhz(9));
    // regional bands against EN 300 175-2: Europe Fc = 1897.344 - c x 1.728 (c 0..9); the others Fc = 1881.792 + c x 1.728 (c 10..63; table F.1)
    for (int c = 0; c < 10; c++) CHECK(std::fabs(carrierMhz(0, c) - (1897.344 - c * 1.728)) < 1e-9 && std::fabs(carrierMhz(0, c) - carrierMhz(c)) < 1e-9, "eu %d", c);
    CHECK(band(1).count == 5 && std::fabs(carrierMhz(1, 0) - 1921.536) < 1e-6 && std::fabs(carrierMhz(1, 4) - 1928.448) < 1e-6, "us 1921.536-1928.448");
    for (int n = 0; n < 5; n++) CHECK(std::fabs(carrierMhz(1, n) - (1921.536 + n * 1.728)) < 1e-6 && carrierMhz(1, n) - 0.864 >= 1920 && carrierMhz(1, n) + 0.864 <= 1930, "us %d", n);
    CHECK(band(2).count == 5 && std::fabs(carrierMhz(2, 0) - 1911.168) < 1e-6 && carrierMhz(2, 4) + 0.864 <= 1920, "brazil");
    CHECK(band(3).count == 11 && std::fabs(carrierMhz(3, 10) - 1928.448) < 1e-6 && carrierMhz(3, 0) - 0.864 >= 1910, "latin america");
    CHECK(band(4).count == 14 && std::fabs(carrierMhz(4, 0) - 903.744) < 1e-6 && std::fabs(carrierMhz(4, 13) - 926.208) < 1e-6, "us ism 900");
    int cn = -1;
    CHECK(bandOf(1921.536, &cn) == 1 && cn == 0 && bandOf(1928.448, &cn) == 1 && cn == 4 && bandOf(1930.176) == -1 && bandOf(1919.808, &cn) == 3 && cn == 5 && bandOf(1909.440) == -1, "bandOf us");
    CHECK(bandOf(1897.344, &cn) == 0 && cn == 0 && bandOf(1881.792, &cn) == 0 && cn == 9 && bandOf(1915.0) == -1, "bandOf eu");
    CHECK(bandOf(1911.168, &cn) == 2 && cn == 0 && bandOf(1918.080, &cn) == 2 && cn == 4 && bandOf(1923.264, &cn) == 1 && cn == 1, "bandOf br/la");   // the first band with the carrier wins: Brazil, then the US
    CHECK(carrierOf(1, 1920.0) == -1 && carrierOf(1, 1922.4) == -1 && carrierOf(1, 1923.264) == 1, "us raster");
    for (int b = 0; b < kBands; b++) {
        double plan[32];
        const int k = scanPlan(b, plan, 32);
        bool asc = k == band(b).count;
        for (int i = 1; i < k; i++) asc = asc && plan[i] > plan[i - 1];
        CHECK(asc && std::fabs(plan[0] - bandLowMhz(b)) < 1e-9 && std::fabs(plan[k - 1] - bandHighMhz(b)) < 1e-9, "scan plan %d", b);
        for (int i = 0; i < k; i++) CHECK(band(b).loMhz <= plan[i] - 0.864 + 1e-9 && plan[i] + 0.864 <= band(b).hiMhz + 1e-9, "band %d carrier %d inside the allocation", b, i);
    }
    if (!fails) printf("dect proto ok\n");
    return fails ? 1 : 0;
}
