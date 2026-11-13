// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust UAT message decoder (rust/onair_uat through uat_decode_rs.cpp, the decode functions of dump978's uat_decode.c): the real DO-282
// vectors, then ONAIR_DIFF_ITERS (default 200000) random / structured / mutated downlink payloads of every message type and the same number of uplink frames
// (random application data, valid information frame chains with FIS-B headers of all four time formats, truncated / overrun chains), uplink decodes also into a
// pre-filled (dirty) struct to prove the same fields are written. Every decoded field goes into a digest (tests/common/golden.h; doubles rounded to 1e-6)
// compared with the one recorded from dump978's C decoder the Rust code replaced.
#include "dect2/uat/uat_decode_c.h"
#include "dect2/uat/uat_frame.h"
#include "../common/golden.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#define DECODE_ADSB uat_decode_adsb_mdb
#define DECODE_UPLINK uat_decode_uplink_mdb

static golden::Digest dig;
static std::mt19937 g(24681);

static void addAdsb(const uat_adsb_mdb& a) {
    dig.add(a.has_sv, a.has_ms, a.has_auxsv, a.position_valid, a.ns_vel_valid, a.ew_vel_valid, a.speed_valid, a.dimensions_valid, a.mdb_type);
    dig.add(a.address_qualifier, a.address, a.lat, a.lon, a.altitude_type, a.altitude, a.nic, a.airground_state, a.ns_vel);
    dig.add(a.ew_vel, a.track_type, a.track, a.speed, a.vert_rate_source, a.vert_rate, a.length, a.width, a.position_offset);
    dig.add(a.utc_coupled, a.tisb_site_id, a.emitter_category, a.callsign_type);
    dig.bytes(a.callsign, 9);
    dig.add(a.emergency_status, a.uat_version, a.sil, a.transmit_mso, a.nac_p, a.nac_v, a.nic_baro, a.has_cdti, a.has_acas, a.acas_ra_active);
    dig.add(a.ident_active, a.atc_services, a.heading_type, a.sec_altitude_type, a.sec_altitude);
}

static void addUplink(const uat_uplink_mdb& a) {
    dig.add(a.position_valid, a.utc_coupled, a.app_data_valid, a.lat, a.lon, a.slot_id, a.tisb_site_id);
    if (!a.app_data_valid) return;
    dig.bytes(a.app_data, sizeof a.app_data);
    dig.add(a.num_info_frames);
    for (unsigned i = 0; i < UPLINK_MAX_INFO_FRAMES; i++) {
        const auto& x = a.info_frames[i];
        dig.add(x.length, x.type, x.is_fisb);
        if (i >= a.num_info_frames) continue;   // slots past the count: only length / type are defined (compared above)
        dig.add((int64_t)(x.data - a.app_data));
        if (!x.is_fisb) continue;
        const auto& p = x.fisb;
        dig.add(p.a_flag, p.g_flag, p.p_flag, p.s_flag, p.monthday_valid, p.seconds_valid, p.product_id, p.month, p.day, p.hours, p.minutes);
        dig.add(p.seconds, p.length, (int64_t)(p.data - a.app_data));
    }
}

static void hexTo(const char* hex, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        const int hi = hex[i * 2] >= 'A' ? hex[i * 2] - 'A' + 10 : hex[i * 2] - '0';
        const int lo = hex[i * 2 + 1] >= 'A' ? hex[i * 2 + 1] - 'A' + 10 : hex[i * 2 + 1] - '0';
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

static void checkAdsb(const uint8_t* fr, const char* what) {
    uint8_t a[UAT_UPLINK_FRAME_BYTES];
    memcpy(a, fr, 34);
    uat_adsb_mdb x;
    memset(&x, 0x5A, sizeof x);   // the decoder starts from a clean struct itself
    DECODE_ADSB(a, &x);
    addAdsb(x);
    (void)what;
}

static void checkUplink(const uint8_t* fr, bool dirty, const char* what) {
    uint8_t a[UAT_UPLINK_FRAME_BYTES];
    memcpy(a, fr, UAT_UPLINK_FRAME_DATA_BYTES);
    static uat_uplink_mdb x;
    memset(&x, dirty ? 0x6B : 0, sizeof x);
    DECODE_UPLINK(a, &x);
    addUplink(x);
    (void)what;
}

static void mutate(uint8_t* d, int n) {
    for (int k = (int)(g() % 5); k > 0; k--) {
        switch (g() % 4) {
        case 0: d[g() % n] = (uint8_t)g(); break;
        case 1: d[g() % n] ^= (uint8_t)(1u << (g() % 8)); break;
        case 2: d[g() % n] = 0; break;
        default: d[g() % n] = 0xFF; break;
        }
    }
}

// Application data: a chain of information frames (9 bit length, type), FIS-B ones with a random APDU header, ended by an empty frame, overrun or the end.
static void buildApp(uint8_t* app) {
    memset(app, 0, 424);
    if (g() % 6 == 0) { for (int i = 0; i < 424; i++) app[i] = (uint8_t)g(); return; }
    int pos = 0;
    while (pos + 2 <= 424) {
        int len = (g() % 10 == 0) ? (int)(g() % 512) : (int)(g() % 40);
        if (g() % 12 == 0) len = 0;
        const int type = (g() % 4 == 0) ? (int)(g() % 16) : 0;
        app[pos] = (uint8_t)(len >> 1);
        app[pos + 1] = (uint8_t)(((len & 1) << 7) | (g() & 0x70) | type);
        for (int i = 0; i < len && pos + 2 + i < 424; i++) app[pos + 2 + i] = (uint8_t)g();
        if (g() % 20 == 0) break;
        pos += len + 2;
    }
}

int main() {
    const char* itc = getenv("ONAIR_DIFF_ITERS");
    const long iters = itc ? atol(itc) : 200000;
    uint8_t v[UAT_UPLINK_FRAME_BYTES];
    memset(v, 0, sizeof v);
    hexTo("007E6987D2D74238C1453855F89980C7524F", v, 18);
    checkAdsb(v, "DO-282 vector");
    // every message type with random contents, then mutated copies of the real vector, then truncated (zero tail) payloads
    for (long i = 0; i < iters; i++) {
        memset(v, 0, sizeof v);
        for (int k = 0; k < 34; k++) v[k] = (uint8_t)g();
        if (g() % 3 == 0) v[0] = (uint8_t)((g() % 32) << 3 | (g() % 8));
        if (g() % 4 == 0) { hexTo("007E6987D2D74238C1453855F89980C7524F", v, 18); mutate(v, 18); }
        if (g() % 5 == 0) { v[12] = (uint8_t)((v[12] & 0x3f) | ((g() % 4) << 6)); }   // air / ground state
        if (g() % 7 == 0) memset(v + 17, 0, 17);
        if (g() % 9 == 0) memset(v + 4, g() % 2 ? 0 : 0xFF, 8);
        checkAdsb(v, "random");
    }
    uint8_t up[UAT_UPLINK_FRAME_BYTES];
    for (long i = 0; i < iters; i++) {
        memset(up, 0, sizeof up);
        for (int k = 0; k < 8; k++) up[k] = (uint8_t)g();
        if (g() % 4) up[6] |= 0x20;
        buildApp(up + 8);
        if (g() % 8 == 0) mutate(up, UAT_UPLINK_FRAME_DATA_BYTES);
        // FIS-B APDU headers: force the time format field and flags in the first information frame
        if (g() % 2 && (up[8] | (up[9] & 0x80)) != 0) { up[10 + 1] = (uint8_t)((up[11] & 0xFC) | (g() & 3)); up[10 + 2] = (uint8_t)((up[12] & 0x7F) | ((g() & 1) << 7)); }
        checkUplink(up, i % 2 == 1, "random");
    }
    return golden::check("uat_proto_rs", dig, 0xa3ef2897fdd4157aull, !itc);
}
