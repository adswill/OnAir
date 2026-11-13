// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/eczas/eczas_proto.h"
#include <cstdio>
using namespace dect2::eczas;

static int fails = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: ", __LINE__);                                                                         \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main() {
    // CRC matches sp5wwp/e-Czas main.c (poly 0x07, init 0)
    CHECK(crc8Poly07((const uint8_t*)"\x00\x00\x00\x00\x00", 5) == 0, "crc empty");

    const uint32_t periods = 86400 / 3; // one day after epoch
    uint8_t frame[kFrameBytes];
    encodeTimeFrameForTest(periods, 1, 0x04, 0, frame); // TZ +1h, leap-announce bit set in flags nibble
    TimePacket p;
    CHECK(decodeTimeFrame(frame, p), "decode");
    CHECK(p.status == DecodeStatus::CrcOk, "status %d", (int)p.status);
    CHECK(p.periods3s == periods, "periods %u", p.periods3s);
    CHECK(p.tzHours == 1, "tz %d", p.tzHours);
    CHECK(p.utcSec == kEpochUtc + 86400 && p.localSec == p.utcSec + 3600, "utc %lld local %lld", (long long)p.utcSec, (long long)p.localSec);
    CHECK(p.timeMarker == kTimeMarker, "marker 0x%x", p.timeMarker);
    CHECK(p.leapAnnounce, "leap announce");

    frame[11] ^= 1;
    CHECK(!decodeTimeFrame(frame, p) && p.status == DecodeStatus::BadCrc, "bad crc");
    frame[11] ^= 1;
    frame[2] = 0x61;
    CHECK(!decodeTimeFrame(frame, p) && p.status == DecodeStatus::WrongType, "wrong type");

    return fails ? 1 : 0;
}
