// The ADS-B / Mode S message layer on top of the Rust crate rust/onair_adsb: the same functions as adsb_msg.cpp, which keeps getBits,
// setBits and toHex (helpers that take no length).
#include "dect2/adsb/adsb_msg.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/adsb.h"
#include <cstdio>
#include <cstdlib>

namespace dect2::adsb {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t m = 0, c = 0;
        onair_adsb_struct_sizes(&m, &c);
        dect2::rsAbiCheck("onair_adsb", {m, c}, {sizeof(OnairAdsbMsg), sizeof(OnairAdsbCommB)});
    }
} abiCheck;

inline const char* cs(const uint8_t* p) { return reinterpret_cast<const char*>(p); }

CommB toCommB(const OnairAdsbCommB& r) {
    CommB c;
    c.callsign.assign((const char*)r.callsign, r.callsign_len);
    c.bds = r.bds;
    c.hasSelAlt = r.has_sel_alt != 0;
    c.selAltMcpFt = r.sel_alt_mcp_ft;
    c.hasSelAltFms = r.has_sel_alt_fms != 0;
    c.selAltFmsFt = r.sel_alt_fms_ft;
    c.hasBaro = r.has_baro != 0;
    c.baroMb = r.baro_mb;
    c.hasRoll = r.has_roll != 0;
    c.rollDeg = r.roll_deg;
    c.hasTrack = r.has_track != 0;
    c.trackDeg = r.track_deg;
    c.hasGs = r.has_gs != 0;
    c.gsKt = r.gs_kt;
    c.hasTrackRate = r.has_track_rate != 0;
    c.trackRateDps = r.track_rate_dps;
    c.hasTas = r.has_tas != 0;
    c.tasKt = r.tas_kt;
    c.hasHeading = r.has_heading != 0;
    c.headingDeg = r.heading_deg;
    c.hasIas = r.has_ias != 0;
    c.iasKt = r.ias_kt;
    c.hasMach = r.has_mach != 0;
    c.mach = r.mach;
    c.hasBaroRate = r.has_baro_rate != 0;
    c.baroRateFpm = r.baro_rate_fpm;
    c.hasInertialRate = r.has_inertial_rate != 0;
    c.inertialRateFpm = r.inertial_rate_fpm;
    c.subnetVersion = r.subnet_version;
    c.acasOperating = r.acas_operating != 0;
    c.continuation = r.continuation != 0;
    c.gicbMask = r.gicb_mask;
    c.ara = r.ara;
    c.rac = r.rac;
    c.raTerminated = r.ra_terminated != 0;
    c.multipleThreats = r.multiple_threats != 0;
    c.tti = r.tti;
    c.threatIcao = r.threat_icao;
    c.fom = r.fom;
    c.hasWind = r.has_wind != 0;
    c.windKt = r.wind_kt;
    c.windDirDeg = r.wind_dir_deg;
    c.hasSat = r.has_sat != 0;
    c.satC = r.sat_c;
    c.hasPressure = r.has_pressure != 0;
    c.pressureHpa = r.pressure_hpa;
    c.hasHumidity = r.has_humidity != 0;
    c.humidityPct = r.humidity_pct;
    c.turbulence = r.turbulence;
    c.windShear = r.wind_shear;
    c.microburst = r.microburst;
    c.icing = r.icing;
    c.wakeVortex = r.wake_vortex;
    c.hasRadioHeight = r.has_radio_height != 0;
    c.radioHeightFt = r.radio_height_ft;
    return c;
}
} // namespace

uint32_t crc24(const uint8_t* m, int nbits) { return onair_adsb_crc24(m, nbits); }
uint32_t crcParity(const uint8_t* m, int nbits) { return onair_adsb_crc_parity(m, nbits); }
int dfLength(int df) { return onair_adsb_df_length(df); }
int singleBitPosition(int nbits, uint32_t rem) { return onair_adsb_single_bit_position(nbits, rem); }
uint32_t singleBitSyndrome(int nbits, int pos) { return onair_adsb_single_bit_syndrome(nbits, pos); }

bool gillhamToAltitude(unsigned code, int& altFt) { int32_t a = 0; if (!onair_adsb_gillham_to_altitude(code, &a)) return false; altFt = a; return true; }
bool altitudeToGillham(int altFt, unsigned& code) { uint32_t c = 0; if (!onair_adsb_altitude_to_gillham(altFt, &c)) return false; code = c; return true; }
bool decodeAc13(unsigned ac, int& altFt) { int32_t a = 0; if (!onair_adsb_decode_ac13(ac, &a)) return false; altFt = a; return true; }
bool decodeAlt12(unsigned alt12, int& altFt) { int32_t a = 0; if (!onair_adsb_decode_alt12(alt12, &a)) return false; altFt = a; return true; }
int decodeId13(unsigned id) { return onair_adsb_decode_id13(id); }
unsigned encodeId13(int sq) { return onair_adsb_encode_id13(sq); }
char charFrom6bit(unsigned c) { return (char)onair_adsb_char_from_6bit(c); }
int charTo6bit(char ch) { return onair_adsb_char_to_6bit((uint8_t)ch); }
std::string categoryCode(int tc, int ca) {
    uint8_t b[2];
    const size_t n = onair_adsb_category_code(tc, ca, b);
    return std::string((const char*)b, n);
}
std::string categoryName(int tc, int ca) { return cs(onair_adsb_category_name(tc, ca)); }
const char* emergencyName(int s) { return cs(onair_adsb_emergency_name(s)); }

int cprNl(double lat) { return onair_adsb_cpr_nl(lat); }
bool cprGlobalAirborne(int latE, int lonE, int latO, int lonO, bool oddLatest, double& lat, double& lon) {
    double a, b;
    if (!onair_adsb_cpr_global_airborne(latE, lonE, latO, lonO, oddLatest, &a, &b)) return false;
    lat = a; lon = b;
    return true;
}
bool cprLocalAirborne(int latCpr, int lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon) {
    double a, b;
    if (!onair_adsb_cpr_local_airborne(latCpr, lonCpr, odd, refLat, refLon, &a, &b)) return false;
    lat = a; lon = b;
    return true;
}
bool cprGlobalSurface(int latE, int lonE, int latO, int lonO, bool oddLatest, double refLat, double refLon, double& lat, double& lon) {
    double a, b;
    if (!onair_adsb_cpr_global_surface(latE, lonE, latO, lonO, oddLatest, refLat, refLon, &a, &b)) return false;
    lat = a; lon = b;
    return true;
}
bool cprLocalSurface(int latCpr, int lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon) {
    double a, b;
    if (!onair_adsb_cpr_local_surface(latCpr, lonCpr, odd, refLat, refLon, &a, &b)) return false;
    lat = a; lon = b;
    return true;
}
void cprEncodeAirborne(double lat, double lon, bool odd, int& latCpr, int& lonCpr) {
    int32_t a = 0, b = 0;
    onair_adsb_cpr_encode_airborne(lat, lon, odd, &a, &b);
    latCpr = a; lonCpr = b;
}
void cprEncodeSurface(double lat, double lon, bool odd, int& latCpr, int& lonCpr) {
    int32_t a = 0, b = 0;
    onair_adsb_cpr_encode_surface(lat, lon, odd, &a, &b);
    latCpr = a; lonCpr = b;
}

double distanceNm(double lat1, double lon1, double lat2, double lon2) { return onair_adsb_distance_nm(lat1, lon1, lat2, lon2); }
double bearingDeg(double lat1, double lon1, double lat2, double lon2) { return onair_adsb_bearing_deg(lat1, lon1, lat2, lon2); }
double isaPressureHpa(double altFt) { return onair_adsb_isa_pressure_hpa(altFt); }
double isaTempC(double altFt) { return onair_adsb_isa_temp_c(altFt); }
double magDeclinationDeg(double lat, double lon) { return onair_adsb_mag_declination_deg(lat, lon); }
void windTriangle(double trk, double gs, double hdg, double tas, double& fromDeg, double& kt) {
    double f = 0, k = 0;
    onair_adsb_wind_triangle(trk, gs, hdg, tas, &f, &k);
    fromDeg = f; kt = k;
}
double satFromTasMach(double tas, double mach) { return onair_adsb_sat_from_tas_mach(tas, mach); }

std::string raText(unsigned ara, unsigned rac, bool terminated) {
    uint8_t b[128];
    const size_t n = onair_adsb_ra_text(ara, rac, terminated, b, sizeof b);
    return std::string((const char*)b, n == SIZE_MAX ? 0 : n);
}

CommB inferCommB(const uint8_t* mb, int altFt) {
    OnairAdsbCommB r;
    onair_adsb_infer_commb(mb, altFt, &r);
    return toCommB(r);
}

bool decodeMsg(const uint8_t* bytes, int nbits, Msg& m) {
    OnairAdsbMsg r;
    const bool ok = onair_adsb_decode_msg(bytes, nbits, &r);
    m = Msg();
    m.df = r.df;
    m.nbits = r.nbits;
    m.icao = r.icao;
    m.ca = r.ca;
    m.ground = r.ground != 0;
    m.alert = r.alert != 0;
    m.spi = r.spi != 0;
    m.fs = r.fs;
    m.vs = r.vs;
    m.hasAlt = r.has_alt != 0;
    m.altFt = r.alt_ft;
    m.altGnss = r.alt_gnss != 0;
    m.hasSquawk = r.has_squawk != 0;
    m.squawk = r.squawk;
    m.tc = r.tc;
    m.st = r.st;
    m.hasIdent = r.has_ident != 0;
    m.catCode = r.cat_code;
    m.hasCpr = r.has_cpr != 0;
    m.cprOdd = r.cpr_odd != 0;
    m.surface = r.surface != 0;
    m.cprLat = r.cpr_lat;
    m.cprLon = r.cpr_lon;
    m.hasMove = r.has_move != 0;
    m.moveKt = r.move_kt;
    m.hasTrack = r.has_track != 0;
    m.trackDeg = r.track_deg;
    m.hasVel = r.has_vel != 0;
    m.speedKt = r.speed_kt;
    m.speedKind = r.speed_kind;
    m.hasHeading = r.has_heading != 0;
    m.headingDeg = r.heading_deg;
    m.headingIsTrack = r.heading_is_track != 0;
    m.hasVrate = r.has_vrate != 0;
    m.vrateFpm = r.vrate_fpm;
    m.vrateBaro = r.vrate_baro != 0;
    m.hasGnssDiff = r.has_gnss_diff != 0;
    m.gnssDiffFt = r.gnss_diff_ft;
    m.emergency = r.emergency;
    m.hasSelAlt = r.has_sel_alt != 0;
    m.selAltFt = r.sel_alt_ft;
    m.selAltFms = r.sel_alt_fms != 0;
    m.hasBaro = r.has_baro != 0;
    m.baroMb = r.baro_mb;
    m.hasSelHdg = r.has_sel_hdg != 0;
    m.selHdgDeg = r.sel_hdg_deg;
    m.tsNacp = r.ts_nacp;
    m.autopilot = r.autopilot != 0;
    m.vnav = r.vnav != 0;
    m.altHold = r.alt_hold != 0;
    m.approach = r.approach != 0;
    m.lnav = r.lnav != 0;
    m.tsModeValid = r.ts_mode_valid != 0;
    m.adsbVersion = r.adsb_version;
    m.nacp = r.nacp;
    m.nicSuppA = r.nic_supp_a;
    m.sil = r.sil;
    m.gva = r.gva;
    m.nicBaro = r.nic_baro;
    m.nacv = r.nacv; m.capAcas = r.cap_acas; m.cap1090In = r.cap_1090in; m.capUatIn = r.cap_uatin;
    m.raActive = r.ra_active; m.identOn = r.ident_on; m.atcSvc = r.atc_svc; m.lengthCode = r.length_code;
    m.hasRaBcast = r.has_ra_bcast != 0; m.raAra = r.ra_ara; m.raRac = r.ra_rac; m.raTerm = r.ra_term != 0; m.raMte = r.ra_mte != 0; m.raTti = r.ra_tti; m.raThreat = r.ra_threat;
    m.catSet = (char)r.cat_set;
    m.callsign.assign((const char*)r.callsign, r.callsign_len);
    m.commb = toCommB(r.commb);
    return ok;
}

} // namespace dect2::adsb
