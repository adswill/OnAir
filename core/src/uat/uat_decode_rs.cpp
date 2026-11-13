// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UAT message decoding on top of the Rust crate rust/onair_uat: uat_decode_adsb_mdb and uat_decode_uplink_mdb of dump978's uat_decode.c (which
// keeps the display functions; the decoding half of the C code was replaced by this Rust implementation). The Rust side fills flat records; this file copies them into the C structs with
// their bit fields and the pointers into the application data.
#include "dect2/common/rs_abi.h"
#include "dect2/rs/uat.h"
#include "dect2/uat/uat_decode_c.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[2];
        onair_uat_struct_sizes(s);
        dect2::rsAbiCheck("onair_uat", s, {sizeof(OnairUatAdsb), sizeof(OnairUatUplink)});
    }
} abiCheck;
} // namespace

extern "C" void uat_decode_adsb_mdb(uint8_t* frame, struct uat_adsb_mdb* mdb) {
    OnairUatAdsb r;
    onair_uat_decode_adsb(frame, UAT_LONG_FRAME_DATA_BYTES, &r);
    memset(mdb, 0, sizeof *mdb);
    mdb->has_sv = r.has_sv; mdb->has_ms = r.has_ms; mdb->has_auxsv = r.has_auxsv;
    mdb->position_valid = r.position_valid; mdb->ns_vel_valid = r.ns_vel_valid; mdb->ew_vel_valid = r.ew_vel_valid;
    mdb->speed_valid = r.speed_valid; mdb->dimensions_valid = r.dimensions_valid;
    mdb->mdb_type = r.mdb_type; mdb->address_qualifier = (address_qualifier_t)r.address_qualifier; mdb->address = r.address;
    mdb->lat = r.lat; mdb->lon = r.lon;
    mdb->altitude_type = (altitude_type_t)r.altitude_type; mdb->altitude = r.altitude;
    mdb->nic = r.nic; mdb->airground_state = (airground_state_t)r.airground_state;
    mdb->ns_vel = r.ns_vel; mdb->ew_vel = r.ew_vel;
    mdb->track_type = (track_type_t)r.track_type; mdb->track = r.track; mdb->speed = r.speed;
    mdb->vert_rate_source = (altitude_type_t)r.vert_rate_source; mdb->vert_rate = r.vert_rate;
    mdb->length = r.length; mdb->width = r.width; mdb->position_offset = r.position_offset;
    mdb->utc_coupled = r.utc_coupled; mdb->tisb_site_id = r.tisb_site_id;
    mdb->emitter_category = r.emitter_category; mdb->callsign_type = (callsign_type_t)r.callsign_type;
    memcpy(mdb->callsign, r.callsign, sizeof mdb->callsign);
    mdb->emergency_status = r.emergency_status; mdb->uat_version = r.uat_version; mdb->sil = r.sil; mdb->transmit_mso = r.transmit_mso;
    mdb->nac_p = r.nac_p; mdb->nac_v = r.nac_v; mdb->nic_baro = r.nic_baro;
    mdb->has_cdti = r.has_cdti; mdb->has_acas = r.has_acas; mdb->acas_ra_active = r.acas_ra_active;
    mdb->ident_active = r.ident_active; mdb->atc_services = r.atc_services;
    mdb->heading_type = (heading_type_t)r.heading_type;
    mdb->sec_altitude_type = (altitude_type_t)r.sec_altitude_type; mdb->sec_altitude = r.sec_altitude;
}

extern "C" void uat_decode_uplink_mdb(uint8_t* frame, struct uat_uplink_mdb* mdb) {
    OnairUatUplink r;
    onair_uat_decode_uplink(frame, UAT_UPLINK_FRAME_DATA_BYTES, &r);
    mdb->position_valid = r.position_valid;
    mdb->lat = r.lat; mdb->lon = r.lon;
    mdb->utc_coupled = r.utc_coupled; mdb->app_data_valid = r.app_data_valid;
    mdb->slot_id = r.slot_id; mdb->tisb_site_id = r.tisb_site_id;
    if (!r.app_data_valid) return;
    memcpy(mdb->app_data, frame + 8, sizeof mdb->app_data);
    mdb->num_info_frames = r.num_info_frames;
    for (unsigned i = 0; i < r.num_info_frames; i++) {
        uat_uplink_info_frame& f = mdb->info_frames[i];
        const OnairUatInfo& s = r.info[i];
        f.length = s.length; f.type = s.kind; f.data = mdb->app_data + s.data_off;
        f.is_fisb = s.is_fisb;
        if (!s.is_fisb) continue;
        fisb_apdu& a = f.fisb;
        a.monthday_valid = s.fisb.monthday_valid; a.seconds_valid = s.fisb.seconds_valid;
        a.hours = s.fisb.hours; a.minutes = s.fisb.minutes;
        if (s.fisb.monthday_valid) { a.month = s.fisb.month; a.day = s.fisb.day; }
        if (s.fisb.seconds_valid) a.seconds = s.fisb.seconds;
        a.length = s.fisb.length; a.data = mdb->app_data + s.fisb.data_off;
        a.a_flag = s.fisb.a_flag; a.g_flag = s.fisb.g_flag; a.p_flag = s.fisb.p_flag; a.s_flag = s.fisb.s_flag;
        a.product_id = s.fisb.product_id;
    }
    if (r.tail_valid && r.num_info_frames < UPLINK_MAX_INFO_FRAMES) {
        mdb->info_frames[r.num_info_frames].length = r.tail_length;
        mdb->info_frames[r.num_info_frames].type = r.tail_kind;
    }
}
