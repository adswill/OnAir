// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// C ABI of the Rust crate rust/onair_adsb (ADS-B / Mode S message layer). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
// Out parameters are written only when the function returns true. Strings
// returned as `const uint8_t*` are static and NUL-terminated ("" when none).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a BDS code inference found in the 56 bit MB field of a DF20 / DF21 message (the same fields as dect2::adsb::CommB; the callsign has no NUL).
typedef struct OnairAdsbCommB {
    int32_t bds;   // 0x10, 0x17, 0x20, 0x30, 0x40, 0x44, 0x45, 0x50, 0x60, or 0 when the field is not one of them (or fits more than one).
    uint8_t callsign[9];   // 2,0: the aircraft identification (trailing spaces removed).
    uint8_t callsign_len;
    bool has_sel_alt;
    int32_t sel_alt_mcp_ft;
    bool has_sel_alt_fms;
    int32_t sel_alt_fms_ft;
    bool has_baro;
    double baro_mb;
    bool has_roll;
    double roll_deg;
    bool has_track;
    double track_deg;
    bool has_gs;
    int32_t gs_kt;
    bool has_track_rate;
    double track_rate_dps;
    bool has_tas;
    int32_t tas_kt;
    bool has_heading;
    double heading_deg;
    bool has_ias;
    int32_t ias_kt;
    bool has_mach;
    double mach;
    bool has_baro_rate;
    int32_t baro_rate_fpm;
    bool has_inertial_rate;
    int32_t inertial_rate_fpm;
    int32_t subnet_version;   // 1,0 data link capability: Mode S subnetwork version, ACAS operating, the continuation flag.
    bool acas_operating;
    bool continuation;
    uint32_t gicb_mask;   // 1,7: 24 bits, bit 23 (first transmitted) = 0,5 ... bit 0 = 6,0 (Annex 10 IV table A-2-23).
    uint32_t ara;
    uint32_t rac;
    bool ra_terminated;
    bool multiple_threats;
    int32_t tti;
    uint32_t threat_icao;   // Valid when `tti` is 1.
    int32_t fom;   // 4,4 figure of merit / source 0 - 4.
    bool has_wind;
    int32_t wind_kt;
    double wind_dir_deg;
    bool has_sat;
    double sat_c;
    bool has_pressure;
    int32_t pressure_hpa;
    bool has_humidity;
    double humidity_pct;
    int32_t turbulence;   // 4,4 and 4,5: 0 nil, 1 light, 2 moderate, 3 severe (-1 not sent).
    int32_t wind_shear;   // 4,5 hazard levels 0 - 3 (-1 not sent).
    int32_t microburst;
    int32_t icing;
    int32_t wake_vortex;
    bool has_radio_height;
    int32_t radio_height_ft;
} OnairAdsbCommB;

// The decoded message (the same fields as dect2::adsb::Msg).
typedef struct OnairAdsbMsg {
    int32_t df;
    int32_t nbits;
    uint32_t icao;   // DF11 / 17 / 18 / 19: the AA field. Address / parity formats: set by the caller from the remainder.
    int32_t ca;   // Capability (DF11, DF17) or control field (DF18).
    bool ground;   // On the ground (flight status, surface position, capability).
    bool alert;
    bool spi;
    int32_t fs;   // Flight status (DF4 / 5 / 20 / 21), -1 when none.
    int32_t vs;   // Vertical status (DF0 / 16), -1 when none.
    bool has_alt;   // Barometric altitude (DF0 / 4 / 16 / 20, TC 9-18), or GNSS height (TC 20-22).
    int32_t alt_ft;
    bool alt_gnss;
    bool has_squawk;   // DF5 / 21, TC 28.
    int32_t squawk;
    int32_t tc;   // Extended squitter type code 1 - 31 (-1 none) and subtype.
    int32_t st;
    bool has_ident;   // TC 1 - 4: identification; the category set is 'A' to 'D'.
    uint8_t callsign[9];
    uint8_t callsign_len;
    uint8_t cat_set;
    int32_t cat_code;
    bool has_cpr;   // TC 5-8, 9-18, 20-22.
    bool cpr_odd;
    bool surface;
    int32_t cpr_lat;
    int32_t cpr_lon;
    bool has_move;   // Surface movement and track (TC 5-8).
    double move_kt;
    bool has_track;
    double track_deg;
    bool has_vel;   // TC 19: speed kind 0 ground speed, 1 indicated airspeed, 2 true airspeed.
    double speed_kt;
    int32_t speed_kind;
    bool has_heading;   // Track for ground speed, magnetic heading for airspeed.
    double heading_deg;
    bool heading_is_track;
    bool has_vrate;
    int32_t vrate_fpm;
    bool vrate_baro;
    bool has_gnss_diff;   // GNSS height minus barometric altitude.
    int32_t gnss_diff_ft;
    int32_t emergency;   // TC 28: emergency state, 0 none, -1 not sent.
    bool has_sel_alt;
    int32_t sel_alt_ft;
    bool sel_alt_fms;
    bool has_baro;
    double baro_mb;
    bool has_sel_hdg;
    double sel_hdg_deg;
    int32_t ts_nacp;
    bool autopilot;
    bool vnav;
    bool alt_hold;
    bool approach;
    bool lnav;
    bool ts_mode_valid;
    int32_t adsb_version;
    int32_t nacp;
    int32_t nic_supp_a;
    int32_t sil;
    int32_t gva;
    int32_t nic_baro;
    int32_t nacv;
    int32_t cap_acas;
    int32_t cap_1090in;
    int32_t cap_uatin;
    int32_t ra_active;
    int32_t ident_on;
    int32_t atc_svc;
    int32_t length_code;
    bool has_ra_bcast;
    uint32_t ra_ara;
    uint32_t ra_rac;
    bool ra_term;
    bool ra_mte;
    int32_t ra_tti;
    uint32_t ra_threat;
    OnairAdsbCommB commb;   // DF20 / 21 Comm-B.
} OnairAdsbMsg;

// ABI version of this module (1).
uint32_t onair_adsb_version(void);
// sizeof(OnairAdsbMsg) and sizeof(OnairAdsbCommB) as Rust sees them (the C++ side checks them at start-up).
void onair_adsb_struct_sizes(size_t* msg, size_t* commb);

// CRC-24 (Annex 10 Vol. IV 3.1.2.3.3): remainder of a whole message / the parity of the first nbits - 24 bits; the length of a DF (0: not searched for).
uint32_t onair_adsb_crc24(const uint8_t* m, int32_t nbits);
uint32_t onair_adsb_crc_parity(const uint8_t* m, int32_t nbits);
int32_t onair_adsb_df_length(int32_t df);
int32_t onair_adsb_single_bit_position(int32_t nbits, uint32_t rem);
uint32_t onair_adsb_single_bit_syndrome(int32_t nbits, int32_t pos);

// Altitude, identity and character codes.
bool onair_adsb_decode_ac13(uint32_t ac, int32_t* alt);
bool onair_adsb_decode_alt12(uint32_t a12, int32_t* alt);
bool onair_adsb_gillham_to_altitude(uint32_t code, int32_t* alt);
bool onair_adsb_altitude_to_gillham(int32_t alt, uint32_t* code);
int32_t onair_adsb_decode_id13(uint32_t id);
uint32_t onair_adsb_encode_id13(int32_t squawk);
uint8_t onair_adsb_char_from_6bit(uint32_t c);
int32_t onair_adsb_char_to_6bit(uint32_t ch);
size_t onair_adsb_category_code(int32_t tc, int32_t ca, uint8_t* out);   // 2 bytes ("A3"), returns 2, or 0 when out of range
const uint8_t* onair_adsb_category_name(int32_t tc, int32_t ca);
const uint8_t* onair_adsb_emergency_name(int32_t state);

// Compact position reporting (17 bit, NZ = 15).
int32_t onair_adsb_cpr_nl(double lat);
bool onair_adsb_cpr_global_airborne(int32_t lat_e, int32_t lon_e, int32_t lat_o, int32_t lon_o, uint32_t odd_latest, double* lat, double* lon);
bool onair_adsb_cpr_local_airborne(int32_t lat_cpr, int32_t lon_cpr, uint32_t odd, double ref_lat, double ref_lon, double* lat, double* lon);
bool onair_adsb_cpr_global_surface(int32_t lat_e, int32_t lon_e, int32_t lat_o, int32_t lon_o, uint32_t odd_latest, double ref_lat, double ref_lon, double* lat, double* lon);
bool onair_adsb_cpr_local_surface(int32_t lat_cpr, int32_t lon_cpr, uint32_t odd, double ref_lat, double ref_lon, double* lat, double* lon);
void onair_adsb_cpr_encode_airborne(double lat, double lon, uint32_t odd, int32_t* lat_cpr, int32_t* lon_cpr);
void onair_adsb_cpr_encode_surface(double lat, double lon, uint32_t odd, int32_t* lat_cpr, int32_t* lon_cpr);

// Geometry and small physical models.
double onair_adsb_distance_nm(double lat1, double lon1, double lat2, double lon2);
double onair_adsb_bearing_deg(double lat1, double lon1, double lat2, double lon2);
double onair_adsb_mag_declination_deg(double lat, double lon);
void onair_adsb_wind_triangle(double track_deg, double gs_kt, double true_hdg_deg, double tas_kt, double* from_deg, double* wind_kt);
double onair_adsb_sat_from_tas_mach(double tas_kt, double mach);
double onair_adsb_isa_pressure_hpa(double alt_ft);
double onair_adsb_isa_temp_c(double alt_ft);

// Comm-B: mb = the 7 bytes of the MB field; alt_ft the altitude of the reply or -100000 (kNoAltitude).
void onair_adsb_infer_commb(const uint8_t* mb, int32_t alt_ft, OnairAdsbCommB* out);
size_t onair_adsb_ra_text(uint32_t ara, uint32_t rac, uint32_t terminated, uint8_t* out, size_t cap);   // SIZE_MAX when cap is too small

// Field extraction of a message of nbits bits (the caller has done the CRC and address checks); out is written even when false is returned.
bool onair_adsb_decode_msg(const uint8_t* bytes, int32_t nbits, OnairAdsbMsg* out);

#ifdef __cplusplus
}
#endif
