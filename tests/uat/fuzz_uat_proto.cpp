// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Seeded mutation run of the UAT message decoder, meant for DECT2_SANITIZE=ON: downlink and uplink decodes of any bytes
// (full-size buffers, as the callers have) must never crash or hang. Also a libFuzzer target with -DONAIR_LIBFUZZER.
#include "dect2/uat/uat_decode_c.h"
#include "dect2/uat/uat_frame.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

static void one(const uint8_t* d, size_t n) {
    uint8_t f[UAT_UPLINK_FRAME_BYTES];
    memset(f, 0, sizeof f);
    memcpy(f, d, n < sizeof f ? n : sizeof f);
    uat_adsb_mdb a;
    uat_decode_adsb_mdb(f, &a);
    static uat_uplink_mdb u;
    memset(&u, 0, sizeof u);
    uat_decode_uplink_mdb(f, &u);
    if (u.app_data_valid) {
        for (unsigned i = 0; i < u.num_info_frames; i++) {
            const auto& fr = u.info_frames[i];
            if (fr.data < u.app_data || fr.data + fr.length > u.app_data + sizeof u.app_data) abort();
            if (fr.is_fisb && (fr.fisb.data < u.app_data || fr.fisb.data + fr.fisb.length > u.app_data + sizeof u.app_data)) abort();
        }
    }
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { one(d, n); return 0; }
#else
int main() {
    const char* it = getenv("ONAIR_FUZZ_ITERS");
    const long iters = it ? atol(it) : 50000;
    std::mt19937 g(4242);
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> s(UAT_UPLINK_FRAME_DATA_BYTES);
        for (auto& b : s) b = (uint8_t)(g() % 3 ? g() : (g() & 1 ? 0xFF : 0));
        if (g() % 2) s[6] |= 0x20;
        s.resize(g() % 2 ? s.size() : g() % s.size());
        one(s.data(), s.size());
    }
    printf("fuzz_uat_proto: %ld inputs, no crash\n", iters);
    return 0;
}
#endif
