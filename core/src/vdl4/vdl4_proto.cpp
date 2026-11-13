// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 link layer and PHY helpers: see vdl4_proto.h.
#include "dect2/vdl4/vdl4_proto.h"
#include "dect2/adsb/adsb_msg.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace dect2 {

const uint8_t kVdl4SyncPattern[kVdl4SyncBits] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};

namespace {
double mod(double x, double y) { double r = std::fmod(x, y); return r < 0 ? r + y : r; }

void putBit(std::vector<uint8_t>& b, int v) { b.push_back((uint8_t)(v & 1)); }
void putBitsLsbFirst(std::vector<uint8_t>& b, uint32_t v, int n) {
    for (int i = 0; i < n; i++) putBit(b, (v >> i) & 1);
}
uint32_t getBitsLsbFirst(const std::vector<uint8_t>& bits, size_t& i, int n) {
    uint32_t v = 0;
    for (int k = 0; k < n; k++) {
        if (i >= bits.size()) break;
        v |= (uint32_t)(bits[i++] & 1) << k;
    }
    return v;
}

uint16_t crcReg(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return c;
}

void addLine(std::string& s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!s.empty()) s.push_back('\n');
    s += buf;
}

static const double kLatS = 4096.0;
static const double kLonS = 16384.0;
} // namespace

std::vector<uint8_t> vdl4NrziEncode(const std::vector<uint8_t>& dataBits, uint8_t level) {
    std::vector<uint8_t> o;
    o.reserve(dataBits.size());
    for (uint8_t b : dataBits) {
        if (!b) level ^= 1;
        o.push_back(level);
    }
    return o;
}

std::vector<uint8_t> vdl4NrziDecode(const std::vector<uint8_t>& lineBits) {
    std::vector<uint8_t> o;
    o.reserve(lineBits.size());
    uint8_t prev = 1;
    for (uint8_t lv : lineBits) {
        const uint8_t bit = (lv == prev) ? 1 : 0;
        o.push_back(bit);
        prev = lv;
    }
    return o;
}

std::vector<uint8_t> vdl4BurstLineBits(const std::vector<uint8_t>& hdlcBits) {
    std::vector<uint8_t> out;
    for (int i = 0; i < kVdl4RampBits; i++) out.push_back(1);
    for (int i = 0; i < kVdl4SyncBits; i++) out.push_back(kVdl4SyncPattern[i]);
    const auto nrzi = vdl4NrziEncode(hdlcBits, 1);
    out.insert(out.end(), nrzi.begin(), nrzi.end());
    return out;
}

bool vdl4BurstDataBits(const std::vector<uint8_t>& lineBits, std::vector<uint8_t>& dataBits) {
    if ((int)lineBits.size() < kVdl4DataStartBit) return false;
    std::vector<uint8_t> nrzi(lineBits.begin() + kVdl4DataStartBit, lineBits.end());
    dataBits = vdl4NrziDecode(nrzi);
    return !dataBits.empty();
}

uint16_t vdl4Fcs(const uint8_t* p, size_t n) { return (uint16_t)~crcReg(p, n); }
bool vdl4FcsOk(const uint8_t* p, size_t n) { return n >= 2 && crcReg(p, n) == 0xF0B8; }

std::vector<uint8_t> vdl4HdlcBits(const std::vector<std::vector<uint8_t>>& frames) {
    std::vector<uint8_t> out;
    auto flag = [&] { for (int i = 0; i < 8; i++) putBit(out, (0x7E >> i) & 1); };
    flag();
    for (const auto& f : frames) {
        int ones = 0;
        for (uint8_t v : f)
            for (int b = 0; b < 8; b++) {
                const int bit = (v >> b) & 1;
                putBit(out, (uint8_t)bit);
                if (bit) {
                    if (++ones == 5) { putBit(out, 0); ones = 0; }
                } else ones = 0;
            }
        flag();
    }
    return out;
}

std::vector<std::vector<uint8_t>> vdl4HdlcFrames(const std::vector<uint8_t>& bits) {
    std::vector<std::vector<uint8_t>> out;
    std::vector<uint8_t> cur;
    int ones = 0;
    bool sync = false, abort = false;
    auto finish = [&] {
        if (sync && !abort && cur.size() >= 16 && cur.size() % 8 == 0) {
            std::vector<uint8_t> f(cur.size() / 8);
            for (size_t i = 0; i < f.size(); i++) {
                uint8_t v = 0;
                for (int b = 0; b < 8; b++) v |= (uint8_t)((cur[8 * i + b] & 1) << b);
                f[i] = v;
            }
            out.push_back(std::move(f));
        }
        cur.clear();
        abort = false;
    };
    for (uint8_t b : bits) {
        if (b) {
            if (++ones > 6) { abort = true; cur.clear(); }
            else cur.push_back(1);
            continue;
        }
        if (ones == 5) { ones = 0; continue; }
        if (ones == 6) {
            cur.resize(cur.size() >= 7 ? cur.size() - 7 : 0);
            finish();
            sync = true;
            ones = 0;
            continue;
        }
        cur.push_back(0);
        ones = 0;
    }
    return out;
}

void vdl4CprEncode(double lat, double lon, bool odd, int& latCpr, int& lonCpr) {
    const double i = odd ? 1.0 : 0.0;
    const double dLat = 360.0 / (15.0 - i);
    const double yz = std::floor(kLatS * mod(lat, dLat) / dLat + 0.5);
    const double rlat = dLat * (yz / kLatS + std::floor(lat / dLat));
    latCpr = (int)mod(yz, kLatS);
    const int nl = std::max(adsb::cprNl(rlat), 1);
    const double dLon = 360.0 / std::max(nl - (int)i, 1);
    const double xz = std::floor(kLonS * mod(lon, dLon) / dLon + 0.5);
    lonCpr = (int)mod(xz, kLonS);
}

bool vdl4CprDecodeLocal(int latCpr, int lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon) {
    const double i = odd ? 1.0 : 0.0;
    const double dLat = 360.0 / (15.0 - i);
    const double lc = latCpr / kLatS;
    const double j = std::floor(refLat / dLat) + std::floor(0.5 + mod(refLat, dLat) / dLat - lc);
    const double rlat = dLat * (j + lc);
    if (rlat < -90.0 || rlat > 90.0) return false;
    const int nl = std::max(adsb::cprNl(rlat), 1);
    const double dLon = 360.0 / std::max(nl - (int)i, 1);
    const double xc = lonCpr / kLonS;
    const double m = std::floor(refLon / dLon) + std::floor(0.5 + mod(refLon, dLon) / dLon - xc);
    double rlon = dLon * (m + xc);
    if (rlon >= 180.0) rlon -= 360.0;
    if (rlon < -180.0) rlon += 360.0;
    lat = rlat;
    lon = rlon;
    return true;
}

bool vdl4CprDecodeGlobal(int latE, int lonE, int latO, int lonO, bool oddLatest, double& lat, double& lon) {
    const double le = latE / kLatS, lo = latO / kLatS;
    const double j = std::floor(15.0 * le - (15.0 - 1.0) * lo + 0.5);
    double ve = (360.0 / 15.0) * (mod(j, 15.0) + le);
    double vo = (360.0 / 14.0) * (mod(j, 14.0) + lo);
    if (ve >= 270.0) ve -= 360.0;
    if (vo >= 270.0) vo -= 360.0;
    if (ve < -90.0 || ve > 90.0 || vo < -90.0 || vo > 90.0) return false;
    const double rlat = oddLatest ? vo : ve;
    const int nl = std::max(adsb::cprNl(rlat), 1);
    const int ni = std::max(nl - (oddLatest ? 1 : 0), 1);
    const double m = std::floor((lonE / kLonS) * (nl - 1) - (lonO / kLonS) * nl + 0.5);
    double rlon = (360.0 / ni) * (mod(m, ni) + (oddLatest ? lonO : lonE) / kLonS);
    if (rlon >= 180.0) rlon -= 360.0;
    lat = rlat;
    lon = rlon;
    return true;
}

std::vector<uint8_t> vdl4PackBurst(const Vdl4BurstFields& f) {
    std::vector<uint8_t> raw;
    putBitsLsbFirst(raw, (uint32_t)(f.autonomous ? 0 : 1), 1);
    putBitsLsbFirst(raw, (uint32_t)(f.rid & 1), 1);
    putBitsLsbFirst(raw, (uint32_t)(f.ver & 7), 3);
    putBitsLsbFirst(raw, f.src >> 24, 3);
    putBitsLsbFirst(raw, f.src & 0xFFFFFF, 24);
    putBitsLsbFirst(raw, (uint32_t)(f.burstId & 0xFF), 8);
    putBitsLsbFirst(raw, (uint32_t)(f.msgId & 0x1F), 5);
    for (uint8_t o : f.info) putBitsLsbFirst(raw, o, 8);
    while (raw.size() % 8) putBit(raw, 0);
    std::vector<uint8_t> frame(raw.size() / 8);
    for (size_t i = 0; i < frame.size(); i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) v |= (uint8_t)((raw[8 * i + b] & 1) << b);
        frame[i] = v;
    }
    const uint16_t c = vdl4Fcs(frame.data(), frame.size());
    frame.push_back((uint8_t)(c & 0xFF));
    frame.push_back((uint8_t)(c >> 8));
    return frame;
}

bool vdl4ParseBurst(const uint8_t* frame, size_t n, Vdl4BurstFields& f) {
    if (n < 6) return false;
    std::vector<uint8_t> bits;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) bits.push_back((frame[i] >> b) & 1);
    size_t i = 0;
    f.autonomous = getBitsLsbFirst(bits, i, 1) == 0;
    f.rid = (int)getBitsLsbFirst(bits, i, 1);
    f.ver = (int)getBitsLsbFirst(bits, i, 3);
    const uint32_t hi = getBitsLsbFirst(bits, i, 3);
    const uint32_t lo = getBitsLsbFirst(bits, i, 24);
    f.src = (hi << 24) | lo;
    f.srcType = (int)((f.src >> 24) & 7);
    f.burstId = (int)getBitsLsbFirst(bits, i, 8);
    f.msgId = (int)getBitsLsbFirst(bits, i, 5);
    f.info.clear();
    while (i + 8 <= bits.size()) {
        uint8_t o = 0;
        for (int b = 0; b < 8; b++) o |= (uint8_t)((bits[i++] & 1) << b);
        f.info.push_back(o);
    }
    return true;
}

const char* vdl4AddrTypeName(int t) {
    switch (t) {
    case 0: return "unknown";
    case 1: return "aircraft";
    case 4: return "ground (ADM)";
    case 5: return "ground (DEL)";
    case 7: return "all stations";
    default: return "reserved";
    }
}

const char* vdl4MsgIdName(int id) {
    switch (id) {
    case 1: return "SYNC";
    case 3: return "DLS";
    case 7: return "UCTRL";
    case 8: return "GEN_REQ";
    case 9: return "GEN_CONF";
    default: return "VSS";
    }
}

std::vector<uint8_t> vdl4SyncBurstInfo(double lat, double lon, int altFt, bool odd, uint32_t src, int srcType) {
    int latC = 0, lonC = 0;
    vdl4CprEncode(lat, lon, odd, latC, lonC);
    std::vector<uint8_t> info(14, 0);
    info[0] = (uint8_t)((srcType & 7) << 5);
    info[1] = (uint8_t)(0x10 | (odd ? 0x08 : 0));   // nic placeholder, cprf, tqc
    info[2] = (uint8_t)(latC & 0xFF);
    info[3] = (uint8_t)(((latC >> 8) & 0x0F) | ((altFt >> 8) & 0xF0));
    info[4] = (uint8_t)(altFt & 0xFF);
    info[5] = (uint8_t)(lonC & 0xFF);
    info[6] = (uint8_t)((lonC >> 8) & 0x3F);
    info[7] = (uint8_t)(((lonC >> 14) & 3) | 0x30);
    return info;
}

std::vector<uint8_t> vdl4DlsUserInfo(const std::string& text) {
    std::vector<uint8_t> info(8, 0);
    info[0] = 0x10;   // CMU-style stub: protocol id + length nibble
    const size_t n = std::min(text.size(), info.size() - 2);
    std::memcpy(info.data() + 2, text.data(), n);
    return info;
}

bool vdl4DecodeMessage(const Vdl4BurstFields& f, double freqHz, Vdl4Message& m) {
    m.freqHz = freqHz;
    m.src = f.src & 0xFFFFFF;
    m.srcType = f.srcType;
    m.autonomous = f.autonomous;
    m.burstId = f.burstId;
    m.msgId = f.msgId;
    m.msgName = vdl4MsgIdName(f.msgId);
    m.downlink = f.srcType == 1;
    m.uplink = f.srcType == 4 || f.srcType == 5;
    m.detail.clear();
    addLine(m.detail, "VDL4 burst: %07X (%s), %s, msg %s (id %d), burst id %d", f.src & 0xFFFFFF, vdl4AddrTypeName(f.srcType),
            f.autonomous ? "autonomous" : "directed", m.msgName.c_str(), f.msgId, f.burstId);
    if (f.msgId == 1 && f.info.size() >= 8) {
        m.syncBurst = true;
        const int latC = (int)(f.info[2] | ((f.info[3] & 0x0F) << 8));
        const int lonC = (int)(f.info[5] | ((f.info[6] & 0x3F) << 8) | ((f.info[7] & 3) << 14));
        const bool odd = (f.info[1] & 0x08) != 0;
        const int alt = (int)((f.info[3] >> 4) << 8 | f.info[4]);
        if (vdl4CprDecodeLocal(latC, lonC, odd, 0, 0, m.lat, m.lon) || vdl4CprDecodeLocal(latC, lonC, odd, 51.0, 0.0, m.lat, m.lon)) {
            m.hasPos = true;
            m.posKind = "VDL4 sync CPR";
            if (alt > 0 && alt < 50000) { m.hasAlt = true; m.altFt = alt; }
            addLine(m.detail, "  position %.4f %.4f%s (%s)", m.lat, m.lon, m.hasAlt ? (" " + std::to_string(m.altFt) + " ft").c_str() : "", m.posKind.c_str());
        }
        char b[120];
        snprintf(b, sizeof b, "SYNC %06X", m.src);
        m.summary = b;
        return true;
    }
    if (f.msgId == 3 && !f.info.empty()) {
        m.hasUser = true;
        for (uint8_t o : f.info) {
            char hx[4];
            snprintf(hx, sizeof hx, "%02X", o);
            m.userHex += hx;
        }
        size_t st = f.info.size() > 2 ? 2 : 0;
        for (size_t i = st; i < f.info.size(); i++) {
            const char c = (char)f.info[i];
            if (c >= 32 && c < 127) m.userText.push_back(c);
        }
        m.summary = m.userText.empty() ? ("DLS " + m.userHex.substr(0, 24)) : ("DLS \"" + m.userText + "\"");
        addLine(m.detail, "  DLS user (%zu octets): %s", f.info.size(), m.userText.empty() ? m.userHex.c_str() : m.userText.c_str());
        return true;
    }
    m.summary = std::string(m.msgName) + " " + std::to_string(f.info.size()) + " octets";
    return true;
}

} // namespace dect2
