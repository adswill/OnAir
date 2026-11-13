// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR protocol pieces on top of the Rust crate rust/onair_dstar: the functions of dstar_proto.h
// (dstar_proto.cpp keeps the sync pattern tables).
#include "dect2/dstar/dstar_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/dstar.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace dstar {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[2];
        onair_dstar_struct_sizes(s);
        dect2::rsAbiCheck("onair_dstar", s, {sizeof(OnairDstarFix), sizeof(OnairDstarHeader)});
    }
} abiCheck;
inline const uint8_t* u8(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }
void fromC(const OnairDstarFix& c, Fix& f) {
    f.lat = c.lat; f.lon = c.lon; f.altM = c.alt_m; f.speedKmh = c.speed_kmh; f.course = c.course;
    f.altValid = c.alt_valid != 0; f.velValid = c.vel_valid != 0; f.aprs = c.aprs != 0;
}
} // namespace

uint16_t crc(const uint8_t* p, size_t n) { return onair_dstar_crc(p, n); }

void headerPack(const Header& h, uint8_t o[kHeaderBytes]) {
    onair_dstar_header_pack(h.flag, u8(h.rpt2), h.rpt2.size(), u8(h.rpt1), h.rpt1.size(), u8(h.ur), h.ur.size(), u8(h.my), h.my.size(), u8(h.sfx), h.sfx.size(), o);
}
bool headerUnpack(const uint8_t in[kHeaderBytes], Header& h) {
    OnairDstarHeader c;
    const bool ok = onair_dstar_header_unpack(in, &c);
    memcpy(h.flag, c.flag, 3);
    h.rpt2 = (const char*)c.rpt2; h.rpt1 = (const char*)c.rpt1; h.ur = (const char*)c.ur; h.my = (const char*)c.my; h.sfx = (const char*)c.sfx;
    return ok;
}
std::string flagText(const uint8_t f[3]) {
    uint8_t b[128];
    return std::string((const char*)b, onair_dstar_flag_text(f, b, sizeof b));
}

void bytesToBits(const uint8_t* b, int nBytes, uint8_t* bits) { onair_dstar_bytes_to_bits(b, nBytes < 0 ? 0 : (size_t)nBytes, bits); }
void bitsToBytes(const uint8_t* bits, int nBytes, uint8_t* b) { onair_dstar_bits_to_bytes(bits, nBytes < 0 ? 0 : (size_t)nBytes, b); }
void convEncode(const uint8_t* in, int nIn, uint8_t* out) { onair_dstar_conv_encode(in, nIn < 0 ? 0 : (size_t)nIn, out); }
int viterbi(const float* soft, int nOut, uint8_t* out) { return onair_dstar_viterbi(soft, nOut < 0 ? 0 : (size_t)nOut, out); }
void interleave(const uint8_t in[kHeaderBits], uint8_t out[kHeaderBits]) { onair_dstar_interleave(in, out); }
void deinterleave(const float in[kHeaderBits], float out[kHeaderBits]) { onair_dstar_deinterleave(in, out); }
void scrambleBits(uint8_t* bits, int n) { onair_dstar_scramble_bits(bits, n < 0 ? 0 : (size_t)n); }
void headerEncode(const uint8_t hdr[kHeaderBytes], uint8_t bits[kHeaderBits]) { onair_dstar_header_encode(hdr, bits); }
int headerDecode(const float soft[kHeaderBits], uint8_t hdr[kHeaderBytes]) { return onair_dstar_header_decode(soft, hdr); }
void ambeFrame(const uint8_t bits[kVoiceBits], char fr[4][24]) { onair_dstar_ambe_frame(bits, (uint8_t*)fr); }

std::string nmeaChecksum(const std::string& body) {
    uint8_t o[2];
    onair_dstar_nmea_checksum(u8(body), body.size(), o);
    return std::string((const char*)o, 2);
}
uint16_t dprsCrc(const std::string& s) { return onair_dstar_dprs_crc(u8(s), s.size()); }

bool parseNmea(const std::string& line, Fix& f) {
    OnairDstarFix c;
    const int r = onair_dstar_parse_nmea(u8(line), line.size(), &c);
    if (r != 0) { f = Fix(); fromC(c, f); }
    return r == 2;
}

bool parseDprs(const std::string& line, Fix& f) {
    OnairDstarFix c;
    std::string call(line.size() + 1, '\0'), comment(line.size() + 1, '\0');
    size_t cl = 0, ml = 0;
    const int r = onair_dstar_parse_dprs(u8(line), line.size(), &c, (uint8_t*)&call[0], &cl, (uint8_t*)&comment[0], &ml);
    if (r != 0) { f = Fix(); fromC(c, f); f.call = call.substr(0, cl); f.comment = comment.substr(0, ml); }
    return r == 2;
}

} // namespace dstar
} // namespace dect2
