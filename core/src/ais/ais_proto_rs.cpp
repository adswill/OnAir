// The AIS frame layer on top of the Rust crate rust/onair_proto: the functions of ais_proto.h; ais_proto.cpp keeps the
// generator side (putU, appendFcs, burstLineBits, toNmea).
#include "dect2/ais/ais_proto.h"
#include "dect2/ais/ais_tel.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/proto.h"
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace ais {

static_assert(AIS_CLASS_A == 0 && AIS_CLASS_B == 1 && AIS_CLASS_BASE == 2 && AIS_CLASS_ATON == 3 && AIS_CLASS_SAR == 4 && AIS_CLASS_OTHER == 5,
              "AisClass values are mirrored in rust/onair_proto/src/mmsi.rs");

namespace {
struct AbiCheck {
    AbiCheck() { dect2::rsAbiCheck("onair_proto", {onair_ais_msg_size()}, {sizeof(OnairAisMsg)}); }
} abiCheck;
}

const char* protoImplementation() { return "Rust (rust/onair_proto)"; }

uint32_t getU(const Bits& b, size_t pos, size_t len) { return onair_ais_get_u(b.data(), b.size(), pos, len); }
int32_t getS(const Bits& b, size_t pos, size_t len) { return onair_ais_get_s(b.data(), b.size(), pos, len); }
std::string getText(const Bits& b, size_t pos, size_t len) {
    std::string s(len / 6, '\0');
    s.resize(onair_ais_get_text(b.data(), b.size(), pos, len, (uint8_t*)&s[0], s.size()));
    return s;
}

uint16_t fcsRegister(const uint8_t* bits, size_t n) { return onair_ais_fcs_register(bits, n); }
bool fcsOk(const Bits& frame) { return onair_ais_fcs_ok(frame.data(), frame.size()); }

Bits stuff(const Bits& in) {
    Bits out(in.size() + in.size() / 5 + 1);
    out.resize(onair_ais_stuff(in.data(), in.size(), out.data(), out.size()));
    return out;
}
bool destuff(const Bits& in, Bits& out) {
    out.assign(in.size(), 0);
    const size_t n = onair_ais_destuff(in.data(), in.size(), out.data(), out.size());
    if (n == SIZE_MAX) { out.clear(); return false; }     // (the C++ version leaves the bits found so far; callers ignore them)
    out.resize(n);
    return true;
}
Bits nrziEncode(const Bits& data, int startLevel) {
    Bits out(data.size());
    onair_ais_nrzi_encode(data.data(), data.size(), startLevel, out.data(), out.size());
    return out;
}
Bits nrziDecode(const Bits& line, int prevLevel) {
    Bits out(line.size());
    onair_ais_nrzi_decode(line.data(), line.size(), prevLevel, out.data(), out.size());
    return out;
}
void reverseOctets(Bits& b) { onair_ais_reverse_octets(b.data(), b.size()); }

HdlcResult hdlcFrames(const Bits& d) {
    HdlcResult r;
    Bits out(d.size());
    std::vector<size_t> lens(d.size() / 64 + 2);
    int32_t bad = 0;
    const size_t good = onair_ais_hdlc_frames(d.data(), d.size(), out.data(), out.size(), lens.data(), lens.size(), &bad);
    size_t at = 0;
    for (size_t i = 0; i < good; i++) {
        r.good.emplace_back(out.begin() + (std::ptrdiff_t)at, out.begin() + (std::ptrdiff_t)(at + lens[i]));
        at += lens[i];
    }
    r.bad = bad;
    return r;
}

double rateOfTurn(int raw) { return onair_ais_rate_of_turn(raw); }
int classOfMmsi(uint32_t mmsi, int type) { return onair_ais_class_of_mmsi(mmsi, type); }
bool mmsiPlausible(uint32_t mmsi) { return onair_ais_mmsi_plausible(mmsi); }
bool acceptableAis(const AisMsg& m, bool decoded) { return onair_ais_acceptable(m.type, m.valid, m.mmsi, decoded); }

static std::string str(const uint8_t* p, size_t n) { return std::string((const char*)p, n); }

bool decodeMessage(const Bits& b, AisMsg& m) {
    OnairAisMsg r;
    const bool ok = onair_ais_decode_message(b.data(), b.size(), &r);
    m = AisMsg();
    m.valid = r.valid != 0;
    m.bits = r.bits;
    if (r.bits < 38) return ok;
    m.type = r.type; m.repeat = r.repeat; m.mmsi = r.mmsi; m.cls = r.cls;
    m.hasPos = r.has_pos != 0; m.lat = r.lat; m.lon = r.lon;
    m.sog = r.sog; m.cog = r.cog; m.heading = r.heading;
    m.hasRot = r.has_rot != 0; m.rotDegMin = r.rot_deg_min;
    m.navStatus = r.nav_status; m.second = r.second;
    m.name = str(r.name, r.name_len); m.callsign = str(r.callsign, r.callsign_len); m.destination = str(r.destination, r.destination_len);
    m.imo = r.imo; m.shipType = r.ship_type;
    m.dimA = r.dim_a; m.dimB = r.dim_b; m.dimC = r.dim_c; m.dimD = r.dim_d;
    m.etaMonth = r.eta_month; m.etaDay = r.eta_day; m.etaHour = r.eta_hour; m.etaMin = r.eta_min;
    m.draughtM = r.draught_m; m.epfd = r.epfd; m.partNo = r.part_no;
    m.vendor = str(r.vendor, r.vendor_len);
    m.aidType = r.aid_type; m.offPosition = r.off_position != 0; m.virtualAid = r.virtual_aid != 0;
    m.altitudeM = r.altitude_m;
    m.year = r.year; m.month = r.month; m.day = r.day; m.hour = r.hour; m.minute = r.minute; m.sec = r.sec;
    if (r.text_bits) m.text = getText(b, r.type == 12 ? 72 : 40, r.text_bits);
    m.dac = r.dac; m.fi = r.fi; m.destMmsi = r.dest_mmsi;
    // the flags the Rust layer does not carry (ITU-R M.1371-5 tables 46, 49 and 79)
    if (m.type >= 1 && m.type <= 3 && b.size() == 168) { m.posAccuracy = (int)getU(b, 60, 1); m.raim = (int)getU(b, 148, 1); }
    else if (m.type == 18 && b.size() == 168) { m.posAccuracy = (int)getU(b, 56, 1); m.raim = (int)getU(b, 147, 1); }
    else if (m.type == 27 && (b.size() == 96 || b.size() == 168)) { m.posAccuracy = (int)getU(b, 38, 1); m.raim = (int)getU(b, 39, 1); m.posLatency = (int)getU(b, 94, 1); }
    return ok;
}

std::string armour(const Bits& p, int& fill) {
    std::string s((p.size() + 5) / 6, '\0');
    fill = 0;
    s.resize(onair_ais_armour(p.data(), p.size(), (uint8_t*)&s[0], s.size(), &fill));
    return s;
}
Bits unarmour(const std::string& chars, int fillBits) {
    Bits b(chars.size() * 6);
    b.resize(onair_ais_unarmour((const uint8_t*)chars.data(), chars.size(), fillBits, b.data(), b.size()));
    return b;
}
uint8_t nmeaChecksum(const std::string& body) { return onair_nmea_checksum((const uint8_t*)body.data(), body.size()); }

bool parseNmea(const std::string& s, std::string& chars, int& fill, int& fragCount, int& fragNo, char* channel) {
    size_t cs = 0, cl = 0;
    int32_t f = 0, fc = 0, fn = 0;
    uint8_t ch = 0;
    const int st = onair_nmea_parse((const uint8_t*)s.data(), s.size(), &cs, &cl, &f, &fc, &fn, &ch);
    if (st == 0) return false;
    fragCount = fc; fragNo = fn;
    if (channel) *channel = (char)ch;
    chars = s.substr(cs, cl);
    fill = f;
    return st == 2;
}

} // namespace ais
} // namespace dect2
