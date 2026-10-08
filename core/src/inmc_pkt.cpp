#include "dect2/inmc_pkt.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

namespace dect2 {
namespace inmc {

std::string satName(int sat) {
    switch (sat) {
    case 0: return "Atlantic Ocean Region West (AOR-W)";
    case 1: return "Atlantic Ocean Region East (AOR-E)";
    case 2: return "Pacific Ocean Region (POR)";
    case 3: return "Indian Ocean Region (IOR)";
    case 9: return "All ocean regions of the station";
    default: return "Unknown";
    }
}

std::string lesName(int sat, int les) {
    switch (les + sat * 100) {   // inmarsatc getLesName / stdcdec get_les_name; the value is region * 100 + station
    case 1: case 101: case 201: case 301: return "Vizada-Telenor, USA";
    case 2: case 102: case 302: return "Stratos Global (Burum-2), Netherlands";
    case 202: return "Stratos Global (Auckland), New Zealand";
    case 3: case 103: case 203: case 303: return "KDDI, Japan";
    case 4: case 104: case 204: case 304: return "Vizada-Telenor, Norway";
    case 44: case 144: case 244: case 344: return "NCS";
    case 105: case 335: return "Telecom Italia";
    case 305: case 120: return "OTESTAT, Greece";
    case 306: return "VSNL, India";
    case 110: case 310: return "Turk Telecom, Turkey";
    case 211: case 311: return "Beijing MCN, China";
    case 12: case 112: case 212: case 312: return "Stratos Global (Burum), Netherlands";
    case 114: return "Embratel, Brazil";
    case 116: case 316: return "Telekomunikacja Polska, Poland";
    case 117: case 217: case 317: return "Morsviazsputnik, Russia";
    case 21: case 121: case 221: case 321: return "Vizada (FT), France";
    case 127: case 327: return "Bezeq, Israel";
    case 210: case 328: return "Singapore Telecom, Singapore";
    case 330: return "VISHIPEL, Vietnam";
    default: return "";
    }
}

std::string serviceName(int code) {
    switch (code) {
    case 0x00: return "System, all ships (general call)";
    case 0x02: return "FleetNET, group call";
    case 0x04: return "SafetyNET, warning to a rectangular area";
    case 0x11: return "System, Inmarsat system message";
    case 0x13: return "SafetyNET, coastal warning";
    case 0x14: return "SafetyNET, shore-to-ship distress alert to a circular area";
    case 0x23: return "System, EGC system message";
    case 0x24: return "SafetyNET, warning to a circular area";
    case 0x31: return "SafetyNET, NAVAREA/METAREA warning or forecast";
    case 0x33: return "System, download group identity";
    case 0x34: return "SafetyNET, SAR coordination to a rectangular area";
    case 0x44: return "SafetyNET, SAR coordination to a circular area";
    case 0x72: return "FleetNET, chart correction service";
    case 0x73: return "SafetyNET, chart correction service for fixed areas";
    default: return "Unknown service";
    }
}

int serviceKind(int code) {
    switch (code) {
    case 0x04: case 0x13: case 0x14: case 0x24: case 0x31: case 0x34: case 0x44: case 0x73: return 1;
    case 0x02: case 0x72: return 2;
    default: return 0;
    }
}

const char* priorityName(int p) {
    static const char* n[] = {"Routine", "Safety", "Urgency", "Distress"};
    return n[p & 3];
}

int addressLength(int code) {
    switch (code) {
    case 0x00: return 3;
    case 0x11: case 0x31: return 4;
    case 0x02: case 0x72: return 5;
    case 0x13: case 0x23: case 0x33: case 0x73: return 6;
    case 0x04: case 0x14: case 0x24: case 0x34: case 0x44: return 7;
    default: return 3;
    }
}

const char* descriptorName(uint8_t d) {
    switch (d) {
    case 0x27: return "Logical channel clear";
    case 0x2A: return "Inbound message ack";
    case 0x08: return "Acknowledgement request";
    case 0x6C: return "Signalling channel";
    case 0x7D: return "Bulletin board";
    case 0x81: return "Announcement";
    case 0x83: return "Logical channel assignment";
    case 0x91: return "Distress alert ack";
    case 0x92: return "Login ack";
    case 0x9A: return "Enhanced data report ack";
    case 0xA0: return "Distress test request";
    case 0xA3: return "Individual poll";
    case 0xA8: return "Confirmation";
    case 0xAA: return "Message";
    case 0xAB: return "LES list";
    case 0xAC: return "Request status";
    case 0xAD: return "Test result";
    case 0xB1: return "EGC double header 1";
    case 0xB2: return "EGC double header 2";
    case 0xBD: return "Multiframe packet";
    case 0xBE: return "Multiframe continuation";
    default: return "Other";
    }
}

std::string servicesText(unsigned m) {
    static const char* n[16] = {"Distress alerting", "SafetyNET", "Inmarsat-C", "Store and forward", "Half duplex", "Full duplex",
                                "Closed network", "FleetNET", "Prefix SF", "Land mobile alerting", "Aero-C", "ITA2", "Data", "Basic X.400",
                                "Enhanced X.400", "Low power CMES"};
    std::string s;
    for (int i = 0; i < 16; i++)
        if (m & (0x8000u >> i)) { if (!s.empty()) s += ", "; s += n[i]; }
    return s;
}

std::string frameTimeText(uint32_t f) {
    const double s = f * kFrameSeconds;
    const int h = (int)(s / 3600), m = (int)(std::fmod(s, 3600) / 60);
    char b[48];
    snprintf(b, sizeof b, "%d:%02d:%04.1f", h, m, std::fmod(s, 60));
    return b;
}

std::string utcText(int64_t t) {
    const time_t tt = (time_t)t;
    struct tm g;
#if defined(_WIN32)
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d UTC", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

// ITA2 tables as in the table of the open decoder (from the baudot project); 0x1F shifts to letters, 0x1B to figures
std::string ita2ToText(const std::vector<uint8_t>& codes) {
    static const char ltrs[32] = {0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
                                  'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
    static const char figs[32] = {0, '3', '\n', '-', ' ', '\'', '8', '7', '\r', 0, '4', 0, ',', '!', ':', '(',
                                  '5', '+', ')', '2', '$', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0};
    std::string s;
    bool fig = false;
    for (uint8_t c : codes) {
        c &= 31;
        if (c == 0x1F) { fig = false; continue; }
        if (c == 0x1B) { fig = true; continue; }
        const char ch = fig ? figs[c] : ltrs[c];
        if (ch && ch != '\r') s += ch;
    }
    return s;
}

std::vector<uint8_t> textToIta2(const std::string& text) {
    std::vector<uint8_t> o;
    bool fig = false, known = false;
    for (char c : text) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        int code = -1; bool f = false;
        for (int i = 1; i < 32 && code < 0; i++) {
            static const char L[32] = {0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K', 'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
            if (L[i] == c) code = i;
        }
        if (code < 0)
            for (int i = 1; i < 32 && code < 0; i++) {
                static const char F[32] = {0, '3', '\n', '-', ' ', '\'', '8', '7', '\r', 0, '4', 0, ',', '!', ':', '(', '5', '+', ')', '2', '$', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0};
                if (F[i] == c) { code = i; f = true; }
            }
        if (code < 0) continue;
        const bool shared = (c == ' ' || c == '\n' || c == '\r');
        if (!shared && (!known || f != fig)) { o.push_back(f ? 0x1B : 0x1F); fig = f; known = true; }
        o.push_back((uint8_t)code);
    }
    return o;
}

std::string decodeText(int pres, const std::vector<uint8_t>& b) {
    if (pres == 6) return ita2ToText(b);
    if (pres == 7) { char t[48]; snprintf(t, sizeof t, "[binary, %zu bytes]", b.size()); return t; }
    std::string s;
    for (size_t i = 0; i < b.size(); i++) {
        const char c = (char)(b[i] & 0x7F);
        if (c == 0x03) break;                       // end of text
        if (c == '\r') continue;
        if (c == '\n' || c == '\t' || c >= 0x20) s += c;
    }
    return s;
}

// ---------------------------------------------------------------- builders
std::vector<uint8_t> buildBulletinBoard(const BulletinBoard& b) {
    std::vector<uint8_t> p(14, 0);
    p[0] = 0x7D;
    p[1] = (uint8_t)b.networkVersion;
    p[2] = (uint8_t)(b.frameNo >> 8); p[3] = (uint8_t)b.frameNo;
    p[4] = (uint8_t)(b.signallingChannel << 2);
    p[5] = (uint8_t)(((b.count / 2) & 15) << 4);
    p[6] = (uint8_t)((b.channelType << 5) | ((b.local & 7) << 2));
    p[7] = (uint8_t)((b.sat << 6) | (b.les & 63));
    p[8] = b.status;
    p[9] = (uint8_t)(b.services >> 8); p[10] = (uint8_t)b.services;
    p[11] = (uint8_t)b.randomInterval;
    packetCheckSet(p.data(), 14);
    return p;
}

std::vector<uint8_t> buildSignalling(uint8_t sv, double uplinkMhz, const uint8_t slots[28]) {
    std::vector<uint8_t> p(13, 0);
    p[0] = 0x6C;
    p[1] = sv;
    const int ch = (int)std::lround((uplinkMhz - 1626.5) / 0.0025) + 6000;
    p[2] = (uint8_t)(ch >> 8); p[3] = (uint8_t)ch;
    for (int i = 0; i < 28; i++) p[4 + i / 4] |= (uint8_t)((slots[i] & 3) << (6 - 2 * (i % 4)));
    packetCheckSet(p.data(), 13);
    return p;
}

std::vector<uint8_t> buildEgc(const EgcPacket& e) {
    const int al = addressLength(e.service);
    const int len = 8 + al + (int)e.payload.size() + 2;
    std::vector<uint8_t> p((size_t)len, 0);
    p[0] = e.desc;
    p[1] = (uint8_t)(len - 2);
    p[2] = e.service;
    p[3] = (uint8_t)((e.continuation ? 0x80 : 0) | ((e.priority & 3) << 5) | (e.repetition & 31));
    p[4] = (uint8_t)(e.msgId >> 8); p[5] = (uint8_t)e.msgId;
    p[6] = (uint8_t)e.packetNo;
    p[7] = (uint8_t)e.presentation;
    for (int i = 0; i < al && i < (int)e.address.size(); i++) p[8 + i] = e.address[i];
    std::copy(e.payload.begin(), e.payload.end(), p.begin() + 8 + al);
    packetCheckSet(p.data(), len);
    return p;
}

// ---------------------------------------------------------------- parser
static std::atomic<uint64_t> gOrder{0};      // shared by all parsers, so that messages of several channels sort by arrival

void trimMessages(std::vector<InmcMessage>& v, size_t maxCount, size_t budget) {
    std::sort(v.begin(), v.end(), [](const InmcMessage& a, const InmcMessage& b) { return a.order > b.order; });
    if (v.size() > maxCount) v.resize(maxCount);
    for (auto& m : v) {       // older messages lose the end of their text first
        const size_t cost = m.text.size() + 300;
        if (cost > budget) {
            if (m.text.size() > 120) { m.text.resize(120); m.truncated = true; }
            budget = budget > 420 ? budget - 420 : 0;
        } else budget -= cost;
    }
}

FrameParser::FrameParser() { reset(); }

void FrameParser::reset() {
    ncs_ = InmcNcsInfo();
    slots_.clear();
    std::fill(pktOk_, pktOk_ + 256, 0u);
    std::fill(pktBad_, pktBad_ + 256, 0u);
    packetsOk_ = packetsBad_ = 0;
    messageCount_ = frameNo_ = curFrame_ = 0;
    mfp_.clear(); mfpTotal_ = 0;
}

static int packetLength(const uint8_t* f, int pos, int end) {
    const uint8_t d = f[pos];
    if ((d >> 7) == 0) return (d & 0x0F) + 1;                    // short descriptor
    if ((d >> 6) == 2 && pos + 1 < end) return f[pos + 1] + 2;   // medium descriptor
    return -1;                                                   // long descriptors are not used by the channels decoded here
}

bool FrameParser::parseFrame(const uint8_t* f, int64_t rxUnix) {
    bool bbFirst = false;
    const int end = kFrameBytes - 1;     // the last byte is the flush byte
    int pos = 0;
    while (pos < end) {
        const uint8_t d = f[pos];
        if (d == 0) break;                // no more packets
        const int len = packetLength(f, pos, end);
        if (len < 3 || pos + len > end) { packetsBad_++; pktBad_[d]++; break; }
        const bool ok = packetCheckOk(f + pos, len);
        if (!ok) { packetsBad_++; pktBad_[d]++; pos += len; continue; }
        packetsOk_++; pktOk_[d]++;
        const uint8_t* p = f + pos;
        if (d == 0x7D && len >= 14) {
            if (pos == 0) bbFirst = true;
            ncs_.valid = true;
            ncs_.networkVersion = p[1];
            ncs_.frameNo = (uint32_t)(p[2] << 8 | p[3]);
            frameNo_ = ncs_.frameNo;
            ncs_.frameTime = frameTimeText(ncs_.frameNo);
            ncs_.signallingChannel = p[4] >> 2;
            ncs_.count = (p[5] >> 4) * 2;
            ncs_.channelType = p[6] >> 5;
            static const char* ct[] = {"Reserved", "NCS", "LES TDM", "Joint NCS and TDM", "Stand-by NCS"};
            ncs_.channelTypeName = ncs_.channelType <= 4 ? ct[ncs_.channelType] : "Reserved";
            ncs_.sat = p[7] >> 6;
            ncs_.region = satName(ncs_.sat);
            ncs_.lesId = p[7] & 63;
            ncs_.lesName = lesName(ncs_.sat, ncs_.lesId);
            ncs_.status = p[8];
            std::string st;
            const char* sn[] = {"600 baud", "operational", "in service", "clear", "links open"};
            for (int i = 0; i < 5; i++) if (p[8] & (0x80 >> i)) { if (!st.empty()) st += ", "; st += sn[i]; }
            ncs_.statusText = st;
            ncs_.services = (uint16_t)(p[9] << 8 | p[10]);
            ncs_.servicesText = servicesText(ncs_.services);
            ncs_.randomInterval = p[11];
            curFrame_ = ncs_.frameNo;
        } else if (d == 0x6C && len >= 13) {
            ncs_.signallingUplinkMhz = ((p[2] << 8 | p[3]) - 6000) * 0.0025 + 1626.5;
        } else if ((d == 0xB1 || d == 0xB2) && len >= 10) {
            egc(p, len, rxUnix);
        } else if (d == 0xBD && len >= 6) {
            // multiframe packet: the start of another packet, continued in 0xBE packets of this or the next frames
            const uint8_t inner = p[2];
            int il = -1;
            if ((inner >> 7) == 0) il = (inner & 15) + 1;
            else if ((inner >> 6) == 2) il = p[3] + 2;
            mfp_.assign(p + 2, p + len - 2);
            mfpTotal_ = il > 0 ? (size_t)il : 0;
        } else if (d == 0xBE && len >= 4 && mfpTotal_) {
            mfp_.insert(mfp_.end(), p + 2, p + len - 2);
            if (mfp_.size() + 2 >= mfpTotal_) {
                mfp_.resize(mfpTotal_, 0);      // the assembled packet has no check bytes of its own
                if ((mfp_[0] == 0xB1 || mfp_[0] == 0xB2) && mfp_.size() >= 10) egc(mfp_.data(), (int)mfp_.size(), rxUnix);
                mfp_.clear(); mfpTotal_ = 0;
            }
        }
        pos += len;
    }
    return bbFirst;
}

void FrameParser::egc(const uint8_t* p, int len, int64_t rxUnix) {
    const uint8_t svc = p[2];
    const int al = addressLength(svc);
    const int hdr = 8 + al;
    if (hdr > len - 2) return;
    const bool cont = (p[3] & 0x80) != 0;
    const int prio = (p[3] >> 5) & 3, rep = p[3] & 31;
    const uint16_t id = (uint16_t)(p[4] << 8 | p[5]);
    const int pno = p[6], pres = p[7];
    const int key = pno * 2 + (p[0] == 0xB2 ? 1 : 0);
    Slot* s = nullptr;
    for (auto& x : slots_) if (x.msg.id == id && x.msg.serviceCode == svc) { s = &x; break; }
    if (!s) {
        if (slots_.size() >= 150) {      // drop the oldest
            auto it = std::min_element(slots_.begin(), slots_.end(), [](const Slot& a, const Slot& b) { return a.order < b.order; });
            slots_.erase(it);
        }
        slots_.emplace_back();
        s = &slots_.back();
        s->msg.id = id; s->msg.serviceCode = svc;
        s->msg.kind = serviceKind(svc);
        s->msg.serviceText = serviceName(svc);
        s->msg.frameNo = curFrame_;
        s->msg.rxUnix = rxUnix;
        s->msg.rxTime = utcText(rxUnix);
        messageCount_++;
    }
    if (pno == 1 && p[0] == 0xB1 && s->msg.complete) {   // the next broadcast of a message we have: start a new pass
        s->parts.clear(); s->lastKey = -1; s->msg.packets = 0;
        s->msg.rxUnix = rxUnix; s->msg.rxTime = utcText(rxUnix); s->msg.frameNo = curFrame_;
    }
    s->order = ++gOrder;
    s->msg.priority = prio; s->msg.repetition = rep; s->msg.presentation = pres;
    s->msg.addr0 = p[8];
    char hx[24];
    s->msg.area.clear();
    for (int i = 1; i < al; i++) { snprintf(hx, sizeof hx, "%02X", p[8 + i]); s->msg.area += hx; if (i + 1 < al) s->msg.area += ' '; }
    s->msg.sat = p[8] >> 6; s->msg.lesId = p[8] & 63;
    s->msg.lesName = lesName(s->msg.sat, s->msg.lesId);
    bool have = false;
    for (auto& q : s->parts) if (q.first == key) { q.second.assign(p + hdr, p + len - 2); have = true; break; }
    if (!have) s->parts.emplace_back(key, std::vector<uint8_t>(p + hdr, p + len - 2));
    if (!cont) s->lastKey = std::max(s->lastKey, key);
    s->msg.packets = (int)s->parts.size();
    finish(*s);
}

void FrameParser::finish(Slot& s) {
    std::sort(s.parts.begin(), s.parts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint8_t> all;
    for (auto& q : s.parts) all.insert(all.end(), q.second.begin(), q.second.end());
    const std::string text = decodeText(s.msg.presentation, all);
    bool done = false;
    if (s.lastKey >= 0) {
        done = true;
        for (int n = 1; n <= s.lastKey / 2; n++) {
            bool g = false;
            for (auto& q : s.parts) if (q.first / 2 == n) g = true;
            if (!g) done = false;
        }
    }
    const bool was = s.msg.complete;
    s.msg.text = text;
    s.msg.complete = done;
    if (done && !was) s.msg.seen++;
}

void FrameParser::fill(InmcTelemetry& t) const {
    t.ncs = ncs_;
    std::copy(pktOk_, pktOk_ + 256, t.pktOk);
    std::copy(pktBad_, pktBad_ + 256, t.pktBad);
    t.packetsOk = packetsOk_; t.packetsBad = packetsBad_;
    t.messageCount = messageCount_;
    t.messages.clear();
    for (const Slot& sl : slots_) {
        t.messages.push_back(sl.msg);
        t.messages.back().order = sl.order;
    }
    trimMessages(t.messages, 150, 70000);
    t.dataValid = false;
    for (auto& s : slots_) if (s.msg.complete) t.dataValid = true;
}

} // namespace inmc
} // namespace dect2
