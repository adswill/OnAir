#include "dect2/ais_proto.h"
#include "dect2/ais_tel.h"
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace ais {

// ---- bit fields
uint32_t getU(const Bits& b, size_t pos, size_t len) {
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) v = (v << 1) | (pos + i < b.size() ? (b[pos + i] & 1u) : 0u);
    return v;
}
int32_t getS(const Bits& b, size_t pos, size_t len) {
    uint32_t v = getU(b, pos, len);
    if (len < 32 && (v >> (len - 1)) & 1u) v |= ~0u << len;
    return (int32_t)v;
}
std::string getText(const Bits& b, size_t pos, size_t len) {
    std::string s;
    for (size_t i = 0; i + 6 <= len; i += 6) {
        const uint32_t v = getU(b, pos + i, 6);
        s.push_back((char)(v < 32 ? v + 64 : v));
    }
    while (!s.empty() && (s.back() == '@' || s.back() == ' ')) s.pop_back();
    return s;
}
void putU(Bits& b, size_t pos, size_t len, uint32_t v) {
    if (b.size() < pos + len) b.resize(pos + len, 0);
    for (size_t i = 0; i < len; i++) b[pos + i] = len - 1 - i < 32 ? (v >> (len - 1 - i)) & 1u : 0u;   // fields wider than 32 bits are zero above
}
void putS(Bits& b, size_t pos, size_t len, int32_t v) {
    putU(b, pos, len, len >= 32 ? (uint32_t)v : ((uint32_t)v & ((1u << len) - 1u)));
}
void putText(Bits& b, size_t pos, size_t len, const std::string& s) {
    for (size_t i = 0; i + 6 <= len; i += 6) {
        char c = i / 6 < s.size() ? s[i / 6] : '@';
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        uint32_t v = (c >= 64 && c < 96) ? (uint32_t)(c - 64) : (c >= 32 && c < 64) ? (uint32_t)c : 0u;   // anything else becomes '@'
        putU(b, pos + i, 6, v);
    }
}

// ---- frame
// Register update of ITU-T X.25 / ISO 13239 (reflected polynomial 0x8408, written 0x1021 in the normal form), the same as AIS-catcher's
// Decoder::CRC16 (Source/Marine/AIS.cpp): one bit at a time in the order received, start value 0xFFFF, good frames end at 0xF0B8.
uint16_t fcsRegister(const uint8_t* bits, size_t n) {
    uint16_t r = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        const bool fb = ((bits[i] ^ r) & 1) != 0;
        r = (uint16_t)(r >> 1);
        if (fb) r ^= 0x8408;
    }
    return r;
}
void appendFcs(Bits& payload) {
    const uint16_t crc = (uint16_t)~fcsRegister(payload.data(), payload.size());
    for (int i = 0; i < 16; i++) payload.push_back((crc >> i) & 1);
}
bool fcsOk(const Bits& frame) { return frame.size() >= 16 && fcsRegister(frame.data(), frame.size()) == kFcsGood; }

Bits stuff(const Bits& in) {
    Bits out;
    out.reserve(in.size() + in.size() / 5 + 1);
    int ones = 0;
    for (uint8_t b : in) {
        out.push_back(b);
        if (b) {
            if (++ones == 5) { out.push_back(0); ones = 0; }
        } else ones = 0;
    }
    return out;
}
bool destuff(const Bits& in, Bits& out) {
    out.clear();
    out.reserve(in.size());
    int ones = 0;
    for (size_t i = 0; i < in.size(); i++) {
        if (ones == 5) {
            if (in[i]) return false;        // six 1s: a flag or an abort
            ones = 0;                        // the stuffed 0
            continue;
        }
        out.push_back(in[i]);
        ones = in[i] ? ones + 1 : 0;
    }
    return true;
}
Bits nrziEncode(const Bits& data, int startLevel) {
    Bits out(data.size());
    int lv = startLevel & 1;
    for (size_t i = 0; i < data.size(); i++) {
        if (!data[i]) lv ^= 1;
        out[i] = (uint8_t)lv;
    }
    return out;
}
Bits nrziDecode(const Bits& line, int prevLevel) {
    Bits out(line.size());
    int p = prevLevel & 1;
    for (size_t i = 0; i < line.size(); i++) {
        out[i] = (line[i] == p) ? 1 : 0;
        p = line[i];
    }
    return out;
}

static const uint8_t kFlag[8] = {0, 1, 1, 1, 1, 1, 1, 0};

Bits burstLineBits(const Bits& payload) {
    Bits fr = payload;
    appendFcs(fr);
    Bits d;
    d.insert(d.end(), kFlag, kFlag + 8);
    const Bits s = stuff(fr);
    d.insert(d.end(), s.begin(), s.end());
    d.insert(d.end(), kFlag, kFlag + 8);
    for (int i = 0; i < 8; i++) d.push_back(0);                           // the carrier is still on while the transmitter ramps down (the rest of the 24 bit buffer is silence)
    // The training sequence alternates on the line (0101..., the level it modulates; NRZI makes it start with either value, here 0 so it ends on 1);
    // the NRZI coding of the flags and the frame starts from the level it leaves.
    Bits line;
    for (int i = 0; i < 24; i++) line.push_back((uint8_t)(i & 1));
    const Bits rest = nrziEncode(d, 1);
    line.insert(line.end(), rest.begin(), rest.end());
    return line;
}

HdlcResult hdlcFrames(const Bits& d) {
    HdlcResult r;
    std::vector<size_t> flags;
    for (size_t i = 0; i + 8 <= d.size();) {
        bool ok = true;
        for (int k = 0; k < 8 && ok; k++) ok = d[i + k] == kFlag[k];
        if (ok) { flags.push_back(i); i += 8; } else i++;
    }
    for (size_t f = 0; f + 1 < flags.size(); f++) {
        const size_t a = flags[f] + 8, e = flags[f + 1];
        if (e < a + 56 || e - a > 1100) continue;
        Bits raw(d.begin() + (std::ptrdiff_t)a, d.begin() + (std::ptrdiff_t)e), fr;
        // the flag is preceded by the training sequence: count it as a burst that failed only when that is there. On the line it alternates, which
        // NRZI decoding turns into zeros; an alternation in the decoded bits (a transmitter that codes the training sequence first) counts too.
        int alt = 0, zeros = 0;
        const size_t s = flags[f];
        for (size_t k = 2; k <= 12 && k <= s; k++) if (d[s - k] != d[s - k + 1]) alt++;
        for (size_t k = 1; k <= 12 && k <= s; k++) if (!d[s - k]) zeros++;
        if (destuff(raw, fr) && fcsOk(fr)) {
            fr.resize(fr.size() - 16);
            r.good.push_back(std::move(fr));
        } else if (alt >= 9 || zeros >= 11) r.bad++;
    }
    return r;
}

// ---- messages
double rateOfTurn(int raw) {
    if (raw == -128) return std::nan("");
    const double v = raw / 4.733;
    return (raw < 0 ? -1.0 : 1.0) * (raw == 127 || raw == -127 ? 720.0 : v * v);
}

const char* shipTypeText(int t) { return aisShipTypeText(t); }

int classOfMmsi(uint32_t mmsi, int type) {
    switch (type) {
    case 1: case 2: case 3: case 5: case 27: return AIS_CLASS_A;
    case 18: case 19: case 24: return AIS_CLASS_B;
    case 4: return AIS_CLASS_BASE;
    case 21: return AIS_CLASS_ATON;
    case 9: return AIS_CLASS_SAR;
    default: break;
    }
    if (mmsi / 1000000 == 111) return AIS_CLASS_SAR;
    if (mmsi / 10000000 == 99) return AIS_CLASS_ATON;
    if (mmsi / 10000000 == 0) return AIS_CLASS_BASE;
    return AIS_CLASS_OTHER;
}

static double lonOf(int32_t raw) { return raw == 0x6791AC0 ? 181.0 : raw / 600000.0; }
static double latOf(int32_t raw) { return raw == 0x3412140 ? 91.0 : raw / 600000.0; }

// Layouts: gpsd AIVDM document (gpsd.gitlab.io/gpsd/AIVDM.html), the same offsets as the tables of ITU-R M.1371-5 annex 8.
bool decodeMessage(const Bits& b, AisMsg& m) {
    m = AisMsg();
    m.bits = b.size();
    if (b.size() < 38) return false;
    m.type = (int)getU(b, 0, 6);
    m.repeat = (int)getU(b, 6, 2);
    m.mmsi = getU(b, 8, 30);
    m.cls = classOfMmsi(m.mmsi, m.type);
    const size_t n = b.size();
    switch (m.type) {
    case 1: case 2: case 3: {
        if (n != 168) return false;      // all three are one slot: a different length is a damaged or foreign frame
        m.navStatus = (int)getU(b, 38, 4);
        const int rot = getS(b, 42, 8);
        m.hasRot = rot != -128;
        if (m.hasRot) m.rotDegMin = (float)rateOfTurn(rot);
        const uint32_t sp = getU(b, 50, 10);
        m.sog = sp == 1023 ? -1.f : sp / 10.f;
        const int32_t lo = getS(b, 61, 28), la = getS(b, 89, 27);
        m.hasPos = lo != 0x6791AC0 && la != 0x3412140 && std::fabs(lonOf(lo)) <= 180 && std::fabs(latOf(la)) <= 90;
        m.lon = lonOf(lo); m.lat = latOf(la);
        const uint32_t c = getU(b, 116, 12);
        m.cog = c >= 3600 ? -1.f : c / 10.f;
        const uint32_t h = getU(b, 128, 9);
        m.heading = h >= 360 ? -1 : (int)h;
        m.second = (int)getU(b, 137, 6);
        m.valid = true;
        break;
    }
    case 4: case 11: {
        if (n != 168) return false;
        m.year = (int)getU(b, 38, 14); m.month = (int)getU(b, 52, 4); m.day = (int)getU(b, 56, 5);
        m.hour = (int)getU(b, 61, 5); m.minute = (int)getU(b, 66, 6); m.sec = (int)getU(b, 72, 6);
        const int32_t lo = getS(b, 79, 28), la = getS(b, 107, 27);
        m.hasPos = lo != 0x6791AC0 && la != 0x3412140 && std::fabs(lonOf(lo)) <= 180 && std::fabs(latOf(la)) <= 90;
        m.lon = lonOf(lo); m.lat = latOf(la);
        m.epfd = (int)getU(b, 134, 4);
        m.valid = true;
        break;
    }
    case 5: {
        if (n < 420 || n > 424) return false;
        m.imo = getU(b, 40, 30);
        m.callsign = getText(b, 70, 42);
        m.name = getText(b, 112, 120);
        m.shipType = (int)getU(b, 232, 8);
        m.dimA = (int)getU(b, 240, 9); m.dimB = (int)getU(b, 249, 9); m.dimC = (int)getU(b, 258, 6); m.dimD = (int)getU(b, 264, 6);
        m.epfd = (int)getU(b, 270, 4);
        m.etaMonth = (int)getU(b, 274, 4); m.etaDay = (int)getU(b, 278, 5); m.etaHour = (int)getU(b, 283, 5); m.etaMin = (int)getU(b, 288, 6);
        m.draughtM = getU(b, 294, 8) / 10.f;
        m.destination = getText(b, 302, 120);
        m.valid = true;
        break;
    }
    case 6:
        if (n < 72) return false;
        m.destMmsi = getU(b, 40, 30);
        if (n >= 88) { m.dac = (int)getU(b, 72, 10); m.fi = (int)getU(b, 82, 6); }
        m.valid = true;
        break;
    case 8:
        if (n < 56) return false;
        m.dac = (int)getU(b, 40, 10); m.fi = (int)getU(b, 50, 6);
        m.valid = true;
        break;
    case 9: {
        if (n != 168) return false;
        const uint32_t alt = getU(b, 38, 12);
        m.altitudeM = alt == 4095 ? -1 : (int)alt;
        const uint32_t sp = getU(b, 50, 10);
        m.sog = sp == 1023 ? -1.f : (float)sp;               // whole knots in this message
        const int32_t lo = getS(b, 61, 28), la = getS(b, 89, 27);
        m.hasPos = lo != 0x6791AC0 && la != 0x3412140 && std::fabs(lonOf(lo)) <= 180 && std::fabs(latOf(la)) <= 90;
        m.lon = lonOf(lo); m.lat = latOf(la);
        const uint32_t c = getU(b, 116, 12);
        m.cog = c >= 3600 ? -1.f : c / 10.f;
        m.second = (int)getU(b, 128, 6);
        m.valid = true;
        break;
    }
    case 14:
        if (n < 40) return false;
        m.text = getText(b, 40, (n - 40) / 6 * 6);
        m.valid = true;
        break;
    case 18: case 19: {
        if (m.type == 18 && n != 168) return false;
        if (m.type == 19 && n != 312) return false;
        const uint32_t sp = getU(b, 46, 10);
        m.sog = sp == 1023 ? -1.f : sp / 10.f;
        const int32_t lo = getS(b, 57, 28), la = getS(b, 85, 27);
        m.hasPos = lo != 0x6791AC0 && la != 0x3412140 && std::fabs(lonOf(lo)) <= 180 && std::fabs(latOf(la)) <= 90;
        m.lon = lonOf(lo); m.lat = latOf(la);
        const uint32_t c = getU(b, 112, 12);
        m.cog = c >= 3600 ? -1.f : c / 10.f;
        const uint32_t h = getU(b, 124, 9);
        m.heading = h >= 360 ? -1 : (int)h;
        m.second = (int)getU(b, 133, 6);
        if (m.type == 19) {
            m.name = getText(b, 143, 120);
            m.shipType = (int)getU(b, 263, 8);
            m.dimA = (int)getU(b, 271, 9); m.dimB = (int)getU(b, 280, 9); m.dimC = (int)getU(b, 289, 6); m.dimD = (int)getU(b, 295, 6);
            m.epfd = (int)getU(b, 301, 4);
        }
        m.valid = true;
        break;
    }
    case 21: {
        if (n < 272 || n > 360) return false;
        m.aidType = (int)getU(b, 38, 5);
        m.name = getText(b, 43, 120);
        const int32_t lo = getS(b, 164, 28), la = getS(b, 192, 27);
        m.hasPos = lo != 0x6791AC0 && la != 0x3412140 && std::fabs(lonOf(lo)) <= 180 && std::fabs(latOf(la)) <= 90;
        m.lon = lonOf(lo); m.lat = latOf(la);
        m.dimA = (int)getU(b, 219, 9); m.dimB = (int)getU(b, 228, 9); m.dimC = (int)getU(b, 237, 6); m.dimD = (int)getU(b, 243, 6);
        m.epfd = (int)getU(b, 249, 4);
        m.second = (int)getU(b, 253, 6);
        m.offPosition = getU(b, 259, 1) != 0;
        m.virtualAid = getU(b, 269, 1) != 0;
        if (n > 272) m.name += getText(b, 272, (n - 272) / 6 * 6);   // name extension: up to 14 more characters
        m.valid = true;
        break;
    }
    case 24: {
        if (n < 160 || n > 168) return false;
        m.partNo = (int)getU(b, 38, 2);
        if (m.partNo == 0) {
            m.name = getText(b, 40, 120);
            m.valid = true;
        } else if (m.partNo == 1) {
            m.shipType = (int)getU(b, 40, 8);
            m.vendor = getText(b, 48, 18);
            m.callsign = getText(b, 90, 42);
            if (m.mmsi / 10000000 == 98) m.destMmsi = getU(b, 132, 30);   // auxiliary craft: the mother ship's MMSI replaces the dimensions
            else { m.dimA = (int)getU(b, 132, 9); m.dimB = (int)getU(b, 141, 9); m.dimC = (int)getU(b, 150, 6); m.dimD = (int)getU(b, 156, 6); }
            m.valid = true;
        }
        break;
    }
    case 27: {
        if (n != 96 && n != 168) return false;
        m.navStatus = (int)getU(b, 40, 4);
        const int32_t lo = getS(b, 44, 18), la = getS(b, 62, 17);      // 1/10 minute
        m.hasPos = lo != 181 * 600 && la != 91 * 600 && std::fabs(lo / 600.0) <= 180 && std::fabs(la / 600.0) <= 90;
        m.lon = lo / 600.0; m.lat = la / 600.0;
        const uint32_t sp = getU(b, 79, 6);
        m.sog = sp == 63 ? -1.f : (float)sp;
        const uint32_t c = getU(b, 85, 9);
        m.cog = c >= 360 ? -1.f : (float)c;
        m.valid = true;
        break;
    }
    default: break;
    }
    return m.valid;
}

// ---- NMEA 0183
static char armourChar(uint32_t v) { return (char)(v < 40 ? v + 48 : v + 56); }

std::string armour(const Bits& p, int& fill) {
    std::string s;
    fill = (int)((6 - p.size() % 6) % 6);
    for (size_t i = 0; i < p.size(); i += 6) s.push_back(armourChar(getU(p, i, 6)));   // the last group is padded with zeros by getU
    return s;
}
Bits unarmour(const std::string& chars, int fillBits) {
    Bits b;
    for (char c : chars) {
        int v = (int)c - 48;
        if (v > 40) v -= 8;
        if (v < 0 || v > 63) v = 0;
        putU(b, b.size(), 6, (uint32_t)v);
    }
    if (fillBits > 0 && (size_t)fillBits <= b.size()) b.resize(b.size() - (size_t)fillBits);
    return b;
}
uint8_t nmeaChecksum(const std::string& body) {
    uint8_t x = 0;
    for (char c : body) x ^= (uint8_t)c;
    return x;
}

std::vector<std::string> toNmea(const Bits& payload, char ch, int seqId) {
    int fill = 0;
    const std::string all = armour(payload, fill);
    const size_t per = 60;     // payload characters per sentence, as the fragments in the gpsd examples
    const size_t cnt = all.empty() ? 1 : (all.size() + per - 1) / per;
    std::vector<std::string> out;
    for (size_t i = 0; i < cnt; i++) {
        const std::string part = all.substr(i * per, per);
        const int f = i + 1 == cnt ? fill : 0;
        char body[160];
        snprintf(body, sizeof body, "AIVDM,%zu,%zu,%s,%c,%s,%d", cnt, i + 1, cnt > 1 ? std::to_string(seqId % 10).c_str() : "", ch, part.c_str(), f);
        char tail[8];
        snprintf(tail, sizeof tail, "*%02X", nmeaChecksum(body));
        out.push_back(std::string("!") + body + tail);
    }
    return out;
}

bool parseNmea(const std::string& s, std::string& chars, int& fill, int& fragCount, int& fragNo, char* channel) {
    if (s.size() < 12 || (s[0] != '!' && s[0] != '$')) return false;
    const size_t star = s.find('*');
    if (star == std::string::npos || star + 3 > s.size()) return false;
    unsigned want = 0;
    if (sscanf(s.c_str() + star + 1, "%2x", &want) != 1) return false;
    if (nmeaChecksum(s.substr(1, star - 1)) != want) return false;
    std::vector<std::string> f;
    size_t p = 0;
    const std::string body = s.substr(0, star);
    while (true) {
        const size_t q = body.find(',', p);
        f.push_back(body.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    if (f.size() != 7) return false;
    fragCount = atoi(f[1].c_str()); fragNo = atoi(f[2].c_str());
    if (channel) *channel = f[4].empty() ? 'A' : f[4][0];
    chars = f[5];
    fill = atoi(f[6].c_str());
    return fragCount >= 1 && fragNo >= 1 && fragNo <= fragCount && fill >= 0 && fill <= 5;
}

} // namespace ais
} // namespace dect2
