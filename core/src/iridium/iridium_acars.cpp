// Iridium ACARS messages from short burst data (see iridium_acars.h).
#include "dect2/limits.h"
#include "dect2/iridium/iridium_acars.h"
#include "dect2/acars/acars_proto.h"
#include "dect2/aero/aero_pos.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dect2 {

namespace {
constexpr size_t kMaxMsgs = limits::kMessages, kMaxAircraft = limits::kAircraft;
constexpr double kRepeatWindow = 180, kJoinWindow = 60;

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }

std::string blockKey(const IridiumAcars& a) { return std::string(1, a.blockId) + ':' + a.seq + ':' + a.flight + ':' + a.text; }

IridiumAircraft* findAircraft(std::vector<IridiumAircraft>& v, const IridiumReg& r) {
    for (auto& a : v) {
        if (r.kind == 2 && a.icao == r.icao) return &a;
        if (!r.norm.empty() && a.reg == r.norm) return &a;
    }
    return nullptr;
}
} // namespace

IridiumReg iridiumAcarsReg(const std::string& raw) {
    IridiumReg r;
    size_t b = 0;
    while (b < raw.size() && raw[b] == '.') b++;
    std::string s;
    for (size_t i = b; i < raw.size(); i++) {
        const unsigned char c = (unsigned char)raw[i];
        if (c == ' ' || c == '?' || c < 32 || c > 126) continue;
        s += (char)std::toupper(c);
    }
    while (!s.empty() && s.back() == '.') s.pop_back();     // padding on the wrong side
    r.norm = s;
    if (s.empty()) return r;
    if (s.size() == 6 && std::all_of(s.begin(), s.end(), isHex)) {
        r.kind = 2;
        r.icao = (uint32_t)std::stoul(s, nullptr, 16);
        return r;
    }
    bool ok = s.size() >= 2 && s.size() <= 7;
    for (char c : s) if (!std::isalnum((unsigned char)c) && c != '-') ok = false;
    r.kind = ok ? 1 : 3;
    return r;
}

IridiumAcarsOutcome iridiumAcarsIngest(IridiumTelemetry& t, const IridiumIdaPacket& pk, const IridiumSbd& sbd, int64_t wall, uint64_t* serialOut) {
    if (!sbd.acars.valid) return IridiumAcarsOutcome::None;
    const IridiumAcars& a = sbd.acars;
    const IridiumReg rg = iridiumAcarsReg(a.reg);
    std::string label = a.label;
    if (label.size() == 2 && label[1] == '?') label[1] = 'd';     // "_<DEL>"
    const std::string key = blockKey(a);

    // a retransmission of a block that is listed
    for (auto it = t.acars.rbegin(); it != t.acars.rend() && it - t.acars.rbegin() < 120; ++it) {
        IridiumAcarsMsg& m = *it;
        if (m.downlink != pk.downlink || m.reg != rg.norm || m.label != label) continue;
        if (pk.timeSec - m.updated > kRepeatWindow) continue;
        if (std::find(m.blockKeys.begin(), m.blockKeys.end(), key) == m.blockKeys.end()) continue;
        m.repeats++;
        t.acarsDuplicates++;
        if (serialOut) *serialOut = m.serial;
        return IridiumAcarsOutcome::Repeat;
    }
    if (a.crc == 2) t.acarsCrcBad++;
    if (a.parity == 2) t.acarsParityBad++;

    IridiumAcarsMsg* target = nullptr;
    IridiumAcarsOutcome out = IridiumAcarsOutcome::New;
    for (auto it = t.acars.rbegin(); it != t.acars.rend() && it - t.acars.rbegin() < 60; ++it) {
        IridiumAcarsMsg& m = *it;
        if (m.more && m.downlink == pk.downlink && m.reg == rg.norm && m.label == label && m.blockId != a.blockId &&
            pk.timeSec >= m.updated && pk.timeSec - m.updated <= kJoinWindow) { target = &m; out = IridiumAcarsOutcome::Joined; break; }
    }
    if (!target) {
        IridiumAcarsMsg m;
        m.serial = ++t.acarsSerial;
        m.time = pk.timeSec; m.downlink = pk.downlink; m.mode = a.mode; m.ack = a.ack; m.blockId = a.blockId;
        m.reg = rg.norm; m.label = label; m.seq = a.seq; m.flight = a.flight;
        m.regKind = rg.kind; m.icao = rg.kind == 2 ? rg.icao : 0;
        m.wall = wall; m.freqHz = pk.freqHz; m.text = a.text;
        t.acars.push_back(m);
        if (t.acars.size() > kMaxMsgs) t.acars.erase(t.acars.begin());
        target = &t.acars.back();
    } else {
        target->text += a.text;
        target->blocks++;
        target->blockId = a.blockId;
    }
    IridiumAcarsMsg& m = *target;
    m.updated = pk.timeSec;
    m.more = a.more;
    if (m.text.size() > 4000) m.text.resize(4000);
    if (m.blockKeys.size() < 8) m.blockKeys.push_back(key);
    if (a.crc > m.crc || m.crc == 0) m.crc = std::max(m.crc, a.crc);
    m.parity = std::max(m.parity, a.parity);
    m.labelText = label.size() == 2 ? acarsLabelText(label[0], label[1]) : "";
    m.decoded = acarsDecodeText(label, m.text);

    // position: only reports from the aircraft (Iridium uplink)
    AeroPosition pos;
    std::string detail;
    const bool fromAircraft = !pk.downlink;
    if (fromAircraft && aeroPositionFromMessage(label, m.text, false, pos, &detail) && pos.valid) {
        m.hasPos = true; m.lat = pos.lat; m.lon = pos.lon; m.hasAlt = pos.hasAlt; m.altFt = pos.altFt; m.posKind = pos.kind;
        if (!detail.empty() && m.decoded.empty()) m.decoded = detail;
        if (pos.icao) m.icao = pos.icao;
        if (!pos.flightId.empty() && m.flight.empty()) m.flight = pos.flightId;
    } else if (!detail.empty() && m.decoded.empty()) m.decoded = detail;
    if (serialOut) *serialOut = m.serial;

    // the aircraft list
    if (!rg.norm.empty() || m.icao) {
        IridiumAircraft* ac = findAircraft(t.aircraft, rg);
        if (!ac && m.icao) { IridiumReg r2; r2.kind = 2; r2.icao = m.icao; ac = findAircraft(t.aircraft, r2); }
        if (!ac) {
            IridiumAircraft n;
            n.reg = rg.norm; n.icao = m.icao;
            t.aircraft.push_back(n);
            if (t.aircraft.size() > kMaxAircraft) t.aircraft.erase(t.aircraft.begin());
            ac = &t.aircraft.back();
        }
        if (m.icao && !ac->icao) {
            const size_t self = (size_t)(ac - t.aircraft.data());
            ac->icao = m.icao;
            // a second entry that is the same airframe under its ICAO address
            for (size_t i = 0; i < t.aircraft.size(); i++) {
                if (i == self || t.aircraft[i].icao != m.icao) continue;
                IridiumAircraft o = t.aircraft[i];
                t.aircraft.erase(t.aircraft.begin() + (long)i);
                const size_t s2 = i < self ? self - 1 : self;
                ac = &t.aircraft[s2];
                ac->messages += o.messages;
                if (!ac->hasPos && o.hasPos) { ac->hasPos = true; ac->lat = o.lat; ac->lon = o.lon; ac->track = o.track; }
                break;
            }
        }
        ac->messages++;
        ac->lastLabel = label; ac->freqHz = pk.freqHz; ac->lastHeard = pk.timeSec;
        if (!m.flight.empty() && fromAircraft) ac->flight = m.flight;
        if (m.hasPos) {
            ac->hasPos = true; ac->lat = m.lat; ac->lon = m.lon; ac->hasAlt = m.hasAlt; ac->altFt = m.altFt;
            ac->hasTrack = pos.hasTrack; ac->trackDeg = pos.trackDeg; ac->hasSpeed = pos.hasSpeed; ac->speedKt = pos.speedKt;
            ac->posKind = m.posKind; ac->posTime = pk.timeSec; ac->posWall = wall; ac->positions++;
            if (ac->track.empty() || std::fabs(ac->track.back().first - m.lat) > 1e-4 || std::fabs(ac->track.back().second - m.lon) > 1e-4) {
                ac->track.push_back({m.lat, m.lon});
                if (ac->track.size() > 24) ac->track.erase(ac->track.begin());
            }
        }
    }
    return out;
}

std::vector<uint8_t> iridiumBuildSbdAcarsEx(const IridiumAcarsBuild& b) {
    std::vector<uint8_t> a;
    auto put = [&](uint8_t c) { a.push_back(b.parity ? acarsWithParity(c) : (uint8_t)(c & 0x7f)); };
    a.push_back(0x01);
    put((uint8_t)b.mode);
    std::string r = b.reg;
    while (r.size() < 7) r = "." + r;
    for (int i = 0; i < 7; i++) put((uint8_t)r[i]);
    put((uint8_t)b.ack);
    put((uint8_t)(b.label.size() > 0 ? b.label[0] : '_'));
    put((uint8_t)(b.label.size() > 1 ? b.label[1] : '_'));
    put((uint8_t)b.blockId);
    put(0x02);
    if (b.fromAircraft) {
        std::string s = b.seq; s.resize(4, ' ');
        std::string f = b.flight; f.resize(6, ' ');
        for (char c : s) put((uint8_t)c);
        for (char c : f) put((uint8_t)c);
    }
    for (char c : b.text) put((uint8_t)c);
    put(b.more ? 0x17 : 0x03);
    if (b.crc) {
        const uint16_t c = acarsCrc(a.data() + 1, a.size() - 1);
        a.push_back((uint8_t)(c & 0xff));
        a.push_back((uint8_t)(c >> 8));
        a.push_back(0x7f);
    }
    std::vector<uint8_t> p = {(uint8_t)(b.type >> 8), (uint8_t)(b.type & 0xff), 0x20, 0x00, 0x00, 0x00, 0x00, 0x10, (uint8_t)std::min<size_t>(a.size(), 255), 0x01};
    p.insert(p.end(), a.begin(), a.end());
    return p;
}

std::string iridiumHexDump(const std::vector<uint8_t>& v, size_t maxBytes) {
    std::string s;
    char b[4];
    const size_t n = std::min(v.size(), maxBytes);
    for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02x", v[i]); if (i) s += ' '; s += b; }
    if (v.size() > n) s += " ...";
    return s;
}

std::string iridiumAsciiDump(const std::vector<uint8_t>& v, size_t maxBytes) {
    std::string s;
    const size_t n = std::min(v.size(), maxBytes);
    for (size_t i = 0; i < n; i++) s += (v[i] >= 32 && v[i] < 127) ? (char)v[i] : '.';
    return s;
}

std::vector<uint8_t> iridiumHexBytes(const std::string& h) {
    std::vector<uint8_t> v;
    auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        const int x = nib(h[i]), y = nib(h[i + 1]);
        if (x < 0 || y < 0) break;
        v.push_back((uint8_t)(x * 16 + y));
    }
    return v;
}

} // namespace dect2
