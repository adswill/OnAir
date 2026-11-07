// See aero_cpdlc.h. The framing follows libacars arinc.c (ground address, IMI, 7 character registration, hex body, CRC-16/ARINC).
#include "dect2/aero/aero_cpdlc.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libacars/asn1-format-cpdlc.h>
#include <libacars/asn1-util.h>
#include <libacars/asn1/FANSATCDownlinkMessage.h>
#include <libacars/asn1/FANSATCUplinkMessage.h>
#include <libacars/crc.h>
#include <libacars/vstring.h>
}

namespace dect2 {

namespace {

constexpr uint16_t kArincCrcGood = 0x1D0F;   // the CRC-16 residue of an intact ARINC 622 block (libacars arinc.c)

bool upperOrDigit(const char* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return false;
    return true;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool aeroDecodeCpdlc(const std::string& text, bool uplink, AeroCpdlcMessage& out) {
    out = AeroCpdlcMessage();
    size_t at = std::string::npos;
    for (const char* imi : {".AT1", ".CR1", ".CC1", ".DR1"}) {
        const size_t f = text.find(imi);
        if (f != std::string::npos && (at == std::string::npos || f < at)) at = f;
    }
    if (at == std::string::npos) return false;
    // the ground address is the 7 (or 4) upper-case / digit characters before the IMI; whatever precedes them (a "- #MD/AA " prefix, a "/") is skipped
    size_t gl = 0;
    if (at >= 7 && upperOrDigit(text.c_str() + at - 7, 7)) gl = 7;
    else if (at >= 4 && upperOrDigit(text.c_str() + at - 4, 4)) gl = 4;
    if (!gl) return false;
    const size_t start = at - gl;
    if (start > 0 && upperOrDigit(text.c_str() + start - 1, 1) && gl == 7) return false;   // an 8th address character: not a ground address
    const std::string payload = text.substr(at + 1);                  // IMI, registration, hex body with the CRC
    if (payload.size() < 3 + 7 + 4) return false;
    out.found = true;
    out.imi = payload.substr(0, 3);
    out.groundAddr = text.substr(start, gl);
    out.reg = payload.substr(3, 7);
    while (!out.reg.empty() && (out.reg.back() == ' ' || out.reg.back() == '.')) out.reg.pop_back();
    while (!out.reg.empty() && out.reg[0] == '.') out.reg.erase(0, 1);
    std::vector<uint8_t> body;
    for (size_t i = 10; i + 1 < payload.size(); i += 2) {
        const int h = hexVal(payload[i]), l = hexVal(payload[i + 1]);
        if (h < 0 || l < 0) break;
        body.push_back((uint8_t)(h * 16 + l));
    }
    if (body.size() < 2) return true;                                  // no room for the CRC: found, not parsed
    std::vector<uint8_t> crcBuf(payload.begin(), payload.begin() + 10);
    crcBuf.insert(crcBuf.end(), body.begin(), body.end());
    out.crcOk = la_crc16_arinc(crcBuf.data(), crcBuf.size(), 0xFFFFu) == kArincCrcGood;
    body.resize(body.size() - 2);
    if (body.empty()) { out.empty = true; return true; }
    asn_TYPE_descriptor_t* td = uplink ? &asn_DEF_FANSATCUplinkMessage : &asn_DEF_FANSATCDownlinkMessage;
    void* msg = nullptr;
    const int rc = la_asn1_decode_as(td, &msg, body.data(), (int)body.size());
    if (rc == 0 && msg) {
        la_vstring* vs = la_vstring_new();
        la_asn1_output_cpdlc_as_text((la_asn1_formatter_params){.vstr = vs, .td = td, .sptr = msg, .indent = 0});
        out.text = vs->str ? vs->str : "";
        la_vstring_destroy(vs, true);
        out.parsed = true;
    }
    if (msg) td->free_struct(td, msg, 0);
    return true;
}

std::string aeroDescribeCpdlc(const AeroCpdlcMessage& m) {
    if (!m.found) return "";
    const char* what = m.imi == "AT1" ? "message" : m.imi == "CR1" ? "connect request" : m.imi == "CC1" ? "connect confirm" : "disconnect request";
    std::string s = "FANS-1/A CPDLC " + std::string(what) + " (" + m.imi + "), ground facility " + m.groundAddr + ", aircraft " + m.reg;
    if (!m.crcOk && !m.empty) s += ", CRC FAILED";
    s += "\n";
    if (m.empty) s += "  (no message body)\n";
    else if (m.parsed) {
        std::string t = m.text;
        s += t;
        if (s.back() != '\n') s += '\n';
    } else s += "  (the message body did not decode)\n";
    return s;
}

std::string aeroBuildCpdlcText(const std::string& groundAddr, const std::string& imi, const std::string& registration, const std::vector<uint8_t>& perBody) {
    std::string reg = "." + registration.substr(0, 6);   // the 7 character field: a dot, then the registration (short ones padded with dots)
    reg.resize(7, '.');
    std::vector<uint8_t> crcIn;
    for (char c : imi) crcIn.push_back((uint8_t)c);
    for (char c : reg) crcIn.push_back((uint8_t)c);
    crcIn.insert(crcIn.end(), perBody.begin(), perBody.end());
    const uint16_t c = la_crc16_arinc(crcIn.data(), crcIn.size(), 0xFFFFu);
    std::vector<uint8_t> all = perBody;
    all.push_back((uint8_t)(~c >> 8));   // the complement of the CRC, most significant byte first (checked on the real messages: the residue is 0x1D0F)
    all.push_back((uint8_t)(~c & 0xFF));
    std::string s = "/" + groundAddr + "." + imi + reg;
    char b[3];
    for (uint8_t v : all) { snprintf(b, sizeof b, "%02X", v); s += b; }
    return s;
}

} // namespace dect2
