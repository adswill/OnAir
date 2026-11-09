// HFDL layers (hfdl_proto.h): the bit layers, FCS, station table and PDU parser are Rust (rust/onair_hfdl through hfdl_proto_rs.cpp). The PSK maps (libm), the
// burst and PDU builders for the test signal and the ACARS reader stay here.
#include "dect2/hfdl/hfdl_proto.h"
#include "dect2/acars/acars_proto.h"
#include "dect2/acars/acars_text.h"
#include "dect2/aero/aero_pos.h"
#include "hfdl_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <mutex>

namespace dect2 {

namespace {

void addLine(std::string& s, const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    s += b; s += '\n';
}
} // namespace

const int8_t kHfdlT[kHfdlTLen] = {1, 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, -1, -1, -1, -1};


namespace {
// v (first bit sent in bit 0) -> the phase index k of k * 360 / M degrees: the first bit is the most significant bit of the
// Gray label (liquid-dsp PSK4 / PSK8 as dumphfdl demodulates them: QPSK at 0 / 90 / 180 / 270 degrees, not 45)
int pskPhase(int bps, int v) {
    int g = 0;
    for (int q = 0; q < bps; q++) g |= ((v >> q) & 1) << (bps - 1 - q);
    int k = g; k ^= k >> 1; k ^= k >> 2;
    return k & ((1 << bps) - 1);
}
int pskLabel(int bps, int k) {
    const int g = k ^ (k >> 1);
    int v = 0;
    for (int q = 0; q < bps; q++) v |= ((g >> (bps - 1 - q)) & 1) << q;
    return v;
}
} // namespace

namespace {
// QPSK and 8PSK points, computed once with the same expression the direct path uses (hfdlSoftBits and the equaliser call this per symbol)
struct PskTable {
    std::complex<float> pt[4][8];
    PskTable() {
        for (int bps = 2; bps <= 3; bps++)
            for (int v = 0; v < (1 << bps); v++) {
                const float ph = (float)(2 * M_PI * pskPhase(bps, v) / (1 << bps));
                pt[bps][v] = {std::cos(ph), std::sin(ph)};
            }
    }
};
} // namespace

std::complex<float> hfdlPsk(int bps, int v) {
    if (bps == 1) return {(v & 1) ? -1.f : 1.f, 0.f};
    const int M = 1 << bps;
    if (bps == 2 || bps == 3) {   // the table holds exactly the values the formula below gives
        static const PskTable t;
        return t.pt[bps][v & (M - 1)];
    }
    const float ph = (float)(2 * M_PI * pskPhase(bps, v & (M - 1)) / M);
    return {std::cos(ph), std::sin(ph)};
}

int hfdlDecide(int bps, std::complex<float> y) {
    if (bps == 1) return y.real() < 0;
    const int M = 1 << bps;
    int k = (int)std::lround(std::arg(y) / (2 * M_PI / M));
    k = (k % M + M) % M;
    return pskLabel(bps, k);
}

void hfdlSoftBits(int bps, std::complex<float> y, float scale, float* out) {
    if (bps == 1) { out[0] = y.real() * scale; return; }
    const int M = 1 << bps;
    float d[8];
    for (int v = 0; v < M; v++) d[v] = std::norm(y - hfdlPsk(bps, v));
    for (int b = 0; b < bps; b++) {
        float m0 = 1e30f, m1 = 1e30f;
        for (int v = 0; v < M; v++) { if ((v >> b) & 1) m1 = std::min(m1, d[v]); else m0 = std::min(m0, d[v]); }
        out[b] = (m1 - m0) * scale * 0.5f;
    }
}

std::vector<std::complex<float>> hfdlBurst(int m1, const std::vector<uint8_t>& pdu) {
    const HfdlRate& r = hfdlRate(m1);
    const int N = hfdlCodedBits(m1);
    const int info = N / (r.quarter ? 4 : 2);
    std::vector<uint8_t> bits((size_t)info, 0);
    const int octets = std::min((int)pdu.size(), hfdlPayloadOctets(m1));
    for (int i = 0; i < octets; i++) for (int b = 0; b < 8; b++) bits[(size_t)(i * 8 + b)] = (pdu[(size_t)i] >> b) & 1;
    std::vector<uint8_t> c = hfdlConvEncode(bits);
    if (r.quarter) { std::vector<uint8_t> d; d.reserve(c.size() * 2); for (uint8_t x : c) { d.push_back(x); d.push_back(x); } c.swap(d); }
    const auto& pos = hfdlInterleave(m1);
    std::vector<uint8_t> chan((size_t)N);
    for (int j = 0; j < N; j++) chan[(size_t)pos[(size_t)j]] = c[(size_t)j];
    std::vector<std::complex<float>> s;
    s.reserve((size_t)(kHfdlPrekey + hfdlBurstSyms(m1)));
    for (int k = 0; k < kHfdlPrekey; k++) s.push_back({(float)kHfdlT[k % kHfdlTLen], 0.f});   // prekey: not used by the receiver
    for (float v : hfdlPreambleRef(m1)) s.push_back({v, 0.f});
    const int nd = r.blocks * kHfdlDataLen;
    std::vector<int8_t> scr;
    hfdlScramblerSigns(nd, scr);
    int di = 0;
    for (int b = 0; b < r.blocks; b++) {
        for (int k = 0; k < kHfdlDataLen; k++, di++) {
            int v = 0;
            for (int q = 0; q < r.bitsPerSym; q++) v |= chan[(size_t)(di * r.bitsPerSym + q)] << q;
            s.push_back(hfdlPsk(r.bitsPerSym, v) * (float)scr[(size_t)di]);
        }
        for (int k = 0; k < kHfdlTLen; k++) s.push_back({(float)kHfdlT[k], 0.f});
    }
    return s;
}

std::vector<uint8_t> hfdlDecodeSoft(int m1, const std::vector<float>& chan) {
    const HfdlRate& r = hfdlRate(m1);
    const int N = hfdlCodedBits(m1);
    const auto& pos = hfdlInterleave(m1);
    std::vector<float> d((size_t)N);
    for (int j = 0; j < N; j++) d[(size_t)j] = chan[(size_t)pos[(size_t)j]];
    if (r.quarter) { std::vector<float> h((size_t)N / 2); for (int j = 0; j < N / 2; j++) h[(size_t)j] = d[(size_t)2 * j] + d[(size_t)2 * j + 1]; d.swap(h); }
    const int info = (int)d.size() / 2;
    const std::vector<uint8_t> bits = hfdlViterbi(d, info);
    std::vector<uint8_t> o((size_t)hfdlPayloadOctets(m1), 0);
    for (size_t i = 0; i < o.size(); i++) for (int b = 0; b < 8; b++) o[i] |= (uint8_t)(bits[i * 8 + (size_t)b] << b);
    return o;
}


// ---------------------------------------------------------------- builders

namespace {
int32_t coordRaw(double deg) { int32_t v = (int32_t)std::lround(deg * 524288.0 / 180.0); v = std::max(-524288, std::min(524287, v)); return v & 0xFFFFF; }
void putPosTime(std::vector<uint8_t>& o, const std::string& flight, double lat, double lon, int utcSec) {
    for (int i = 0; i < 6; i++) o.push_back(i < (int)flight.size() ? (uint8_t)flight[(size_t)i] : (uint8_t)' ');
    const uint32_t la = (uint32_t)coordRaw(lat), lo = (uint32_t)coordRaw(lon);
    o.push_back((uint8_t)la); o.push_back((uint8_t)(la >> 8)); o.push_back((uint8_t)(((la >> 16) & 0xF) | ((lo & 0xF) << 4)));
    o.push_back((uint8_t)(lo >> 4)); o.push_back((uint8_t)(lo >> 12));
    const int t2 = std::max(0, utcSec) / 2;
    o.push_back((uint8_t)t2); o.push_back((uint8_t)(t2 >> 8));
}
} // namespace

std::vector<uint8_t> hfdlSpdu(const HfdlSquitterSpec& s) {
    std::vector<uint8_t> b(66, 0);
    b[0] = (uint8_t)(((s.version & 3) << 2) | 0x20 | ((s.changeNote & 3) << 6));
    b[1] = (uint8_t)((s.gsId & 0x7F) | (s.utc ? 0x80 : 0));
    b[2] = (uint8_t)(s.frameIndex & 0xFF); b[3] = (uint8_t)((s.frameIndex >> 8) & 0xF);
    b[52] = 0;
    b[53] = (uint8_t)(s.systable & 0xFF);
    b[54] = (uint8_t)(((s.systable >> 8) & 0xF) | ((s.mask & 0xF) << 4));
    b[55] = (uint8_t)(s.mask >> 4); b[56] = (uint8_t)(s.mask >> 12);
    const HfdlSquitterGs& a = s.other[0]; const HfdlSquitterGs& c = s.other[1];
    b[57] = (uint8_t)((a.id & 0x7F) | (a.utc ? 0x80 : 0));
    b[58] = (uint8_t)a.mask; b[59] = (uint8_t)(a.mask >> 8);
    b[60] = (uint8_t)(((a.mask >> 16) & 0xF) | ((c.id & 0xF) << 4));
    b[61] = (uint8_t)(((c.id >> 4) & 7) | (c.utc ? 8 : 0) | ((c.mask & 0xF) << 4));
    b[62] = (uint8_t)(c.mask >> 4); b[63] = (uint8_t)(c.mask >> 12);
    const uint16_t f = hfdlFcs(b.data(), 64);
    b[64] = (uint8_t)f; b[65] = (uint8_t)(f >> 8);
    return b;
}

std::vector<uint8_t> hfdlLpdu(uint8_t type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> o = {type};
    o.insert(o.end(), body.begin(), body.end());
    const uint16_t f = hfdlFcs(o.data(), o.size());
    o.push_back((uint8_t)f); o.push_back((uint8_t)(f >> 8));
    return o;
}
namespace {
// On-air HFDL ICAO octets are bit-reversed relative to a normal hex address.
// Match the receiver's LPDU decode; only the three ICAO octets use this order.
uint8_t hfdlIcaoWireByte(uint8_t x) {
    x = (uint8_t)((x >> 4) | (x << 4));
    x = (uint8_t)(((x & 0xCCu) >> 2) | ((x & 0x33u) << 2));
    return (uint8_t)(((x & 0xAAu) >> 1) | ((x & 0x55u) << 1));
}
std::vector<uint8_t> hfdlIcaoWireBytes(uint32_t icao) {
    return {hfdlIcaoWireByte((uint8_t)(icao >> 16)),
            hfdlIcaoWireByte((uint8_t)(icao >> 8)),
            hfdlIcaoWireByte((uint8_t)icao)};
}
} // namespace

std::vector<uint8_t> hfdlLogonRequest(uint32_t icao) { return hfdlLpdu(0x8F, hfdlIcaoWireBytes(icao)); }
std::vector<uint8_t> hfdlLogonConfirm(uint32_t icao, int acId) {
    auto body = hfdlIcaoWireBytes(icao);
    body.insert(body.end(), {(uint8_t)acId, 0, 0, 0});
    return hfdlLpdu(0x9F, body);
}
std::vector<uint8_t> hfdlUnnumbered(const std::vector<uint8_t>& h) { return hfdlLpdu(0x0D, h); }

std::vector<uint8_t> hfdlPerfData(const std::string& flight, double lat, double lon, int utcSec, int gsId, int freqId) {
    std::vector<uint8_t> o = {0xFF, 0xD1};
    putPosTime(o, flight, lat, lon, utcSec);
    o.push_back(2); o.push_back(1); o.push_back((uint8_t)(gsId & 0x7F)); o.push_back((uint8_t)freqId);
    o.resize(47, 0);
    o[27] = 3; o[35] = 12;      // a few counters
    return o;
}
std::vector<uint8_t> hfdlFreqData(const std::string& flight, double lat, double lon, int utcSec, const std::vector<HfdlSquitterGs>& heard) {
    std::vector<uint8_t> o = {0xFF, 0xD5};
    putPosTime(o, flight, lat, lon, utcSec);
    for (size_t i = 0; i < heard.size() && i < 6; i++) {
        const uint32_t m = heard[i].mask;
        o.push_back((uint8_t)(heard[i].id & 0x7F));
        o.push_back((uint8_t)m); o.push_back((uint8_t)(m >> 8)); o.push_back((uint8_t)(((m >> 16) & 0xF) | ((m & 0xF) << 4)));
        o.push_back((uint8_t)(m >> 4)); o.push_back((uint8_t)(m >> 12));
    }
    return o;
}
std::vector<uint8_t> hfdlAcarsHfnpdu(const std::string& reg, const std::string& label, char blockId, const std::string& text, bool) {
    AcarsBlockSpec s;
    s.reg = reg;
    s.label[0] = label.size() > 0 ? label[0] : 'H';
    s.label[1] = label.size() > 1 ? label[1] : '1';
    s.blockId = blockId;
    s.text = text;
    s.hasText = !text.empty();
    std::vector<uint8_t> f = acarsBuildFrame(s, 0);    // SYN SYN SOH ... DEL
    std::vector<uint8_t> o = {0xFF, 0xFF};
    o.insert(o.end(), f.begin() + 2, f.end());
    return o;
}

std::vector<uint8_t> hfdlMpduDown(int gsId, int acId, const std::vector<std::vector<uint8_t>>& l) {
    std::vector<uint8_t> o = {(uint8_t)(0x03 | ((l.size() & 0xF) << 2)), (uint8_t)(gsId & 0x7F), (uint8_t)acId, 0, 0, 0};
    for (const auto& x : l) o.push_back((uint8_t)(x.size() - 1));
    const uint16_t f = hfdlFcs(o.data(), o.size());
    o.push_back((uint8_t)f); o.push_back((uint8_t)(f >> 8));
    for (const auto& x : l) o.insert(o.end(), x.begin(), x.end());
    return o;
}
std::vector<uint8_t> hfdlMpduUp(int gsId, int acId, const std::vector<std::vector<uint8_t>>& l) {
    std::vector<uint8_t> o = {0x01, (uint8_t)(gsId & 0x7F), (uint8_t)acId, (uint8_t)((l.size() & 0xF) << 4)};
    for (const auto& x : l) o.push_back((uint8_t)(x.size() - 1));
    const uint16_t f = hfdlFcs(o.data(), o.size());
    o.push_back((uint8_t)f); o.push_back((uint8_t)(f >> 8));
    for (const auto& x : l) o.insert(o.end(), x.begin(), x.end());
    return o;
}

// ---------------------------------------------------------------- parser


// the ACARS block of an HFNPDU (n octets at p, after the HFNPDU's FF FF) into m: the registration, label, text and any position; appends to m.detail
void hfdlReadAcars(const uint8_t* p, size_t n, HfdlMessage& m) {
    m.hfnpdu = "ACARS";
    m.acars = true;
    if (n && p[0] == kAcarsSoh) { p++; n--; }
    size_t end = n;
    if (end && p[end - 1] == 0x7F) end--;
    if (end < 13 + 2) { m.summary = "ACARS: cut short"; addLine(m.detail, "ACARS: %zu octets, too short", n); return; }
    const size_t bl = end - 2;
    m.acarsCrcOk = acarsCrc(p, bl + 2) == 0;
    std::vector<uint8_t> t7(bl);
    for (size_t i = 0; i < bl; i++) t7[i] = p[i] & 0x7F;
    AcarsMessage a;
    if (!acarsParseBlock(t7.data(), (int)bl, a)) { m.summary = "ACARS: not a valid block"; addLine(m.detail, "ACARS: the block layout is not valid"); return; }
    m.reg = a.reg; m.flight = a.flightId; m.label = a.label; m.labelText = a.labelText; m.msgNum = a.msgNum; m.acarsText = a.text; m.blockId = a.blockId;
    std::string s = "ACARS " + a.label;
    if (!a.labelText.empty()) s += " (" + a.labelText + ")";
    if (!a.flightId.empty()) s += " " + a.flightId;
    std::string one;
    for (char c : a.text) one += (c == '\n' || c == '\r') ? ' ' : c;
    if (!one.empty()) s += ": " + (one.size() > 80 ? one.substr(0, 80) + "..." : one);
    m.summary = s;
    addLine(m.detail, "ACARS: mode %c, registration %s, label %s%s%s, block id %c%s", a.mode ? a.mode : '-', a.reg.empty() ? "-" : a.reg.c_str(), a.label.c_str(),
            a.labelText.empty() ? "" : " ", a.labelText.c_str(), a.blockId ? a.blockId : '-', m.acarsCrcOk ? "" : ", check sequence FAILED");
    if (!a.msgNum.empty() || !a.flightId.empty()) addLine(m.detail, "  message %s, flight %s", a.msgNum.empty() ? "-" : a.msgNum.c_str(), a.flightId.empty() ? "-" : a.flightId.c_str());
    const std::string reading = !a.decoded.empty() ? a.decoded : acarsInterpretLine(a.label, a.text);
    if (!reading.empty()) addLine(m.detail, "  reading: %s", reading.c_str());
    AeroPosition pos;
    std::string adsc;
    if (aeroPositionFromMessage(a.label, a.text, m.uplink, pos, &adsc) && pos.valid) {
        m.hasPos = true; m.lat = pos.lat; m.lon = pos.lon; m.hasAlt = pos.hasAlt; m.altFt = pos.altFt; m.posKind = pos.kind;
        if (!pos.flightId.empty() && m.flight.empty()) m.flight = pos.flightId;
    }
    if (!adsc.empty()) { addLine(m.detail, "ADS-C (FANS-1/A):"); m.detail += adsc; if (m.detail.back() != '\n') m.detail += '\n'; }
    if (m.hasPos) addLine(m.detail, "  position %.4f %.4f%s (%s)", m.lat, m.lon, m.hasAlt ? (" " + std::to_string(m.altFt) + " ft").c_str() : "", m.posKind.c_str());
}


} // namespace dect2
