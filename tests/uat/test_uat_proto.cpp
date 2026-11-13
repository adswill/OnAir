// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/uat/uat_proto.h"
#include "dect2/uat/uat_decode_c.h"
#include "dect2/uat/uat_frame.h"
extern "C" {
void init_fec(void);
int correct_adsb_frame(uint8_t* to, int* rs_errors);
}
#include <cstdio>
#include <cstring>

using namespace dect2;
#include "../common/test_util.h"

static void testFecVector() {
    init_fec();
    uint8_t buf[UAT_SHORT_FRAME_BYTES];
    const char* hex = "FF8196782DD44238C1453855F89980C7524F5970940ED83AD89CE7A9BEF8761BBCD9FCC817D82E2D1ACF90CA78DA3C49";
    for (int i = 0; i < UAT_SHORT_FRAME_BYTES; i++) {
        const int hi = hex[i * 2] >= 'A' ? hex[i * 2] - 'A' + 10 : hex[i * 2] - '0';
        const int lo = hex[i * 2 + 1] >= 'A' ? hex[i * 2 + 1] - 'A' + 10 : hex[i * 2 + 1] - '0';
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    int rs = -1;
    const int ft = correct_adsb_frame(buf, &rs);
    CHECK(ft == 1 && rs >= 0 && rs <= 6, "DO-282 short frame FEC ft=%d rs=%d", ft, rs);
    uat_adsb_mdb mdb;
    memset(&mdb, 0, sizeof mdb);
    uat_decode_adsb_mdb(buf, &mdb);
    CHECK(mdb.has_sv && mdb.position_valid, "decoded SV position");
    CHECK(mdb.address == 0x7E6987, "address %06X", mdb.address);
}

static void testDecodeFrame() {
    UatRawFrame raw;
    raw.uplink = false;
    raw.dataBytes = UAT_SHORT_FRAME_DATA_BYTES;
    raw.timeSec = 1.0;
    const char* expect = "007E6987D2D74238C1453855F89980C7524F";
    for (int i = 0; i < UAT_SHORT_FRAME_DATA_BYTES; i++) {
        const int hi = expect[i * 2] >= 'A' ? expect[i * 2] - 'A' + 10 : expect[i * 2] - '0';
        const int lo = expect[i * 2 + 1] >= 'A' ? expect[i * 2 + 1] - 'A' + 10 : expect[i * 2 + 1] - '0';
        raw.data[i] = (uint8_t)((hi << 4) | lo);
    }
    UatDecoded out;
    CHECK(uatDecodeFrame(raw, out), "decode");
    CHECK(out.kind == UatDecoded::Kind::Adsb && out.ac.hasPos, "adsb position");
    CHECK(out.ac.icao == 0x7E6987, "icao");
}

int main() {
    testFecVector();
    testDecodeFrame();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("uat proto OK\n");
    return 0;
}
