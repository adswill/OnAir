// Lightning position (network): the packet format between stations, its HMAC-SHA256 authentication and the replay guard (ltpos.h).
#include "dect2/sferics/ltpos.h"
#include "dect2/mesh/mesh_crypto.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::ltpos {

namespace {
constexpr uint8_t kMagic[4] = {'O', 'A', 'L', 'P'};
constexpr uint8_t kVersion = 1;
constexpr size_t kMacLen = 16;
constexpr size_t kHead = 4 + 1 + 1 + 4 + 4 + 8;
constexpr size_t kStHdr = 4 + 4 + 2 + 1 + 1 + 4;
constexpr size_t kDetRec = 8 + 4 + 2 + 4 + 2 + 2 + 2 + 4 + 4;   // 32

struct W {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; i++) b.push_back((uint8_t)(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i))); }
    void i16(int16_t v) { u16((uint16_t)v); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void i64(int64_t v) { u64((uint64_t)v); }
    void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
};
struct R {
    const uint8_t* p; size_t n, i = 0; bool bad = false;
    bool need(size_t k) { if (i + k > n) { bad = true; return false; } return true; }
    uint64_t un(int k) { if (!need((size_t)k)) return 0; uint64_t v = 0; for (int j = 0; j < k; j++) v |= (uint64_t)p[i + (size_t)j] << (8 * j); i += (size_t)k; return v; }
    uint8_t u8() { return (uint8_t)un(1); }
    uint16_t u16() { return (uint16_t)un(2); }
    uint32_t u32() { return (uint32_t)un(4); }
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    int64_t i64() { return (int64_t)un(8); }
    float f32() { const uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
};
uint32_t sat32(double v) { return v <= 0 ? 0u : v >= 4294967295.0 ? 4294967295u : (uint32_t)std::llround(v); }
int16_t sat16(double v) { return (int16_t)std::clamp<long long>(std::llround(v), -32768, 32767); }
uint16_t satu16(double v) { return (uint16_t)std::clamp<long long>(std::llround(v), 0, 65535); }
bool finite(float v) { return std::isfinite(v); }

void putHdr(W& w, const StationHdr& h) {
    w.i32((int32_t)std::llround(std::clamp(h.lat, -90.0, 90.0) * 1e7));
    w.i32((int32_t)std::llround(std::clamp(h.lon, -180.0, 180.0) * 1e7));
    w.i16(sat16(h.altM));
    w.u8((uint8_t)h.src);
    w.u8(h.flags);
    w.u32(sat32(h.clockSigmaNs));
}
void getHdr(R& r, StationHdr& h) {
    h.lat = r.i32() * 1e-7; h.lon = r.i32() * 1e-7; h.altM = r.i16();
    const uint8_t s = r.u8();
    h.src = s <= 6 ? (TimeSrc)s : TimeSrc::None;
    h.flags = r.u8();
    h.clockSigmaNs = r.u32();
    if (std::fabs(h.lat) > 90 || std::fabs(h.lon) > 180) r.bad = true;
}
void mac(const std::vector<uint8_t>& key, const uint8_t* d, size_t n, uint8_t out[32]) { meshcrypto::hmacSha256(key.data(), key.size(), d, n, out); }
}

std::vector<uint8_t> networkKey(const std::string& text) {
    std::vector<uint8_t> k(32);
    meshcrypto::sha256((const uint8_t*)text.data(), text.size(), k.data());
    return k;
}

uint32_t stationIdOf(const std::string& name) {
    uint32_t h = 2166136261u;
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    return h ? h : 1u;
}

std::vector<uint8_t> encodePacket(const Packet& p, const std::vector<uint8_t>& key) {
    W w;
    for (uint8_t c : kMagic) w.u8(c);
    w.u8(kVersion); w.u8((uint8_t)p.type); w.u32(p.station); w.u32(p.seq); w.i64(p.sendNs);
    putHdr(w, p.hdr);
    if (p.type == PktType::Detections) {
        const size_t n = std::min(p.dets.size(), kMaxDetPerPacket);
        w.u8((uint8_t)n);
        for (size_t i = 0; i < n; i++) {
            const DetRec& d = p.dets[i];
            w.i64(d.utcNs); w.u32(sat32(d.sigmaNs)); w.i16(sat16(d.peakDb * 100)); w.f32(d.amp);
            w.u16(satu16(d.widthUs * 10)); w.u16(satu16(d.riseUs * 10)); w.i16(sat16(d.freqHz / 10)); w.u32(0); w.u32(0);   // 8 bytes kept for later features
        }
    } else if (p.type == PktType::Reference) {
        w.u8((uint8_t)std::clamp(p.ref.refId, 0, 255)); w.i64(p.ref.anchorNs); w.u32(sat32(p.ref.sigmaNs)); w.i16(sat16(p.ref.snrDb * 100)); w.f32((float)p.ref.baud);
    } else {
        const std::string nm = p.name.substr(0, 24);
        w.u8((uint8_t)nm.size());
        for (char c : nm) w.u8((uint8_t)c);
    }
    uint8_t m[32];
    mac(key, w.b.data(), w.b.size(), m);
    w.b.insert(w.b.end(), m, m + kMacLen);
    return w.b;
}

PktErr decodePacket(const uint8_t* d, size_t n, const std::vector<uint8_t>& key, Packet& out) {
    if (!d || n < kHead + kStHdr + 1 + kMacLen) return PktErr::Short;
    if (n > kMaxPacket) return PktErr::Malformed;
    if (std::memcmp(d, kMagic, 4) != 0) return PktErr::Magic;
    if (d[4] != kVersion) return PktErr::Version;
    uint8_t m[32];
    mac(key, d, n - kMacLen, m);
    uint8_t diff = 0;
    for (size_t i = 0; i < kMacLen; i++) diff |= (uint8_t)(m[i] ^ d[n - kMacLen + i]);
    if (diff) return PktErr::Auth;
    R r{d, n - kMacLen};
    r.i = 5;
    Packet p;
    const uint8_t t = r.u8();
    if (t < 1 || t > 3) return PktErr::Malformed;
    p.type = (PktType)t;
    p.station = r.u32(); p.seq = r.u32(); p.sendNs = r.i64();
    getHdr(r, p.hdr);
    if (p.type == PktType::Detections) {
        const size_t c = r.u8();
        if (c > kMaxDetPerPacket || r.n - r.i != c * kDetRec) return PktErr::Malformed;
        for (size_t i = 0; i < c; i++) {
            DetRec x;
            x.utcNs = r.i64(); x.sigmaNs = r.u32(); x.peakDb = r.i16() / 100.f; x.amp = r.f32();
            x.widthUs = r.u16() / 10.f; x.riseUs = r.u16() / 10.f; x.freqHz = r.i16() * 10.f; r.u32(); r.u32();
            if (!finite(x.amp)) return PktErr::Malformed;
            p.dets.push_back(x);
        }
    } else if (p.type == PktType::Reference) {
        p.ref.refId = r.u8(); p.ref.anchorNs = r.i64(); p.ref.sigmaNs = r.u32(); p.ref.snrDb = r.i16() / 100.f; p.ref.baud = r.f32();
        if (!std::isfinite(p.ref.baud) || p.ref.baud <= 0 || p.ref.baud > 1e5 || p.ref.refId == 0) return PktErr::Malformed;
    } else {
        const size_t k = r.u8();
        if (k > 24 || r.n - r.i != k) return PktErr::Malformed;
        for (size_t i = 0; i < k; i++) { const char c = (char)r.u8(); if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return PktErr::Malformed; p.name += c; }
    }
    if (r.bad || r.i != r.n || p.station == 0) return PktErr::Malformed;
    out = p;
    return PktErr::Ok;
}

ReplayGuard::Result ReplayGuard::check(uint32_t station, uint32_t seq, int64_t sendNs, int64_t nowNs) {
    if (std::llabs(sendNs - nowNs) > skewNs_) return Stale;
    auto it = w_.find(station);
    if (it == w_.end()) {
        if (w_.size() >= max_) return Full;
        W& w = w_[station];
        w.any = true; w.top = seq; w.bits = 1; w.lastSend = sendNs;
        return Fresh;
    }
    W& w = it->second;
    if (seq > w.top) {
        const uint32_t sh = seq - w.top;
        w.bits = sh >= 64 ? 0 : w.bits << sh;
        w.bits |= 1; w.top = seq; w.lastSend = std::max(w.lastSend, sendNs);
        return Fresh;
    }
    const uint32_t back = w.top - seq;
    if (back >= 64) {
        // a station that started again counts from a lower number; its send time (inside the MAC) is newer than anything before
        if (sendNs > w.lastSend + 1000000000LL) { w.top = seq; w.bits = 1; w.lastSend = sendNs; return Fresh; }
        return Stale;
    }
    if (w.bits & (1ULL << back)) return Replay;
    w.bits |= 1ULL << back;
    w.lastSend = std::max(w.lastSend, sendNs);
    return Fresh;
}

} // namespace dect2::ltpos
