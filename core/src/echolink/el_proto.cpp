// EchoLink wire formats: see el_proto.h. Packet layouts after SvxLink's echolib (GPL-2.0-or-later, Copyright (C) 2003-2026 Tobias Blomberg /
// SM0SVX), re-written for OnAir; every parser here is bounded by the received length.
#include "dect2/echolink/el_proto.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace echolink {

namespace {
constexpr int kRtpVersionBits = 3;          // the header bits EchoLink uses (0xC0), not RFC 3550's 2
constexpr uint8_t kRtcpSr = 200, kRtcpRr = 201, kRtcpSdes = 202, kRtcpBye = 203;
constexpr uint8_t kSdesEnd = 0, kSdesCname = 1, kSdesName = 2, kSdesEmail = 3, kSdesPhone = 4, kSdesPriv = 8;

void addText(std::vector<uint8_t>& v, const std::string& t) {
    const size_t n = std::min<size_t>(t.size(), 255);
    v.push_back((uint8_t)n);
    v.insert(v.end(), t.begin(), t.begin() + (long)n);
}
void nullReceiverReport(std::vector<uint8_t>& v) {
    const uint8_t rr[8] = {(uint8_t)(kRtpVersionBits << 6), kRtcpRr, 0, 1, 0, 0, 0, 0};
    v.insert(v.end(), rr, rr + 8);
}
// p[start..] is a 4-byte header + 4-byte source; fills its length field after padding to 4 bytes
void finishSubpacket(std::vector<uint8_t>& v, size_t start) {
    while ((v.size() - start) & 3) v.push_back(0);
    const size_t len = (v.size() - start) / 4 - 1;
    v[start + 2] = (uint8_t)(len >> 8);
    v[start + 3] = (uint8_t)len;
}
bool versionOk(uint8_t b) { const int ver = (b >> 6) & 3; return ver == kRtpVersionBits || ver == 1; }
// Walk an RTCP compound packet: the first subpacket must be SR or RR without padding
template <class F> bool walkRtcp(const uint8_t* p, size_t n, F&& f) {
    if (!p || n < 4) return false;
    if (!versionOk(p[0]) || (p[0] & 0x20) || (p[1] != kRtcpSr && p[1] != kRtcpRr)) return false;
    size_t off = 0;
    while (off + 4 <= n && versionOk(p[off])) {
        const size_t len = ((size_t)((p[off + 2] << 8) | p[off + 3]) + 1) * 4;
        if (off + len > n) break;
        if (f(p + off, len)) return true;
        off += len;
    }
    return false;
}
std::string noCrLf(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n'; }), s.end());
    return s;
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
} // namespace

// ---- RTCP ------------------------------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> makeSdes(const std::string& call, const std::string& name, const std::string& priv) {
    std::vector<uint8_t> v;
    nullReceiverReport(v);
    const size_t start = v.size();
    const uint16_t ver = (uint16_t)((kRtpVersionBits << 14) | kRtcpSdes | (1 << 8));
    v.push_back((uint8_t)(ver >> 8)); v.push_back((uint8_t)ver); v.push_back(0); v.push_back(0);
    v.push_back(0); v.push_back(0); v.push_back(0); v.push_back(0);   // source 0
    v.push_back(kSdesCname); addText(v, "CALLSIGN");
    std::string nm = call;
    if (nm.size() < 15) nm.append(15 - nm.size(), ' ');
    nm += name;
    v.push_back(kSdesName); addText(v, nm);
    v.push_back(kSdesEmail); addText(v, "CALLSIGN");
    v.push_back(kSdesPhone); addText(v, "08:30");
    if (!priv.empty()) { v.push_back(kSdesPriv); addText(v, priv); }
    v.push_back(kSdesEnd); v.push_back(0);
    finishSubpacket(v, start);
    return v;
}

std::vector<uint8_t> makeBye() {
    std::vector<uint8_t> v;
    nullReceiverReport(v);
    const size_t start = v.size();
    const uint16_t ver = (uint16_t)((kRtpVersionBits << 14) | kRtcpBye | (1 << 8));
    v.push_back((uint8_t)(ver >> 8)); v.push_back((uint8_t)ver); v.push_back(0); v.push_back(0);
    v.push_back(0); v.push_back(0); v.push_back(0); v.push_back(0);
    addText(v, "jan2002");
    finishSubpacket(v, start);
    return v;
}

bool isRtcpBye(const uint8_t* p, size_t n) { return walkRtcp(p, n, [](const uint8_t* s, size_t) { return s[1] == kRtcpBye; }); }
bool isRtcpSdes(const uint8_t* p, size_t n) { return walkRtcp(p, n, [](const uint8_t* s, size_t) { return s[1] == kRtcpSdes; }); }

bool sdesItem(const uint8_t* p, size_t n, int item, std::string& out) {
    out.clear();
    bool found = false;
    walkRtcp(p, n, [&](const uint8_t* s, size_t len) {
        if (s[1] != kRtcpSdes || (s[0] & 0x1F) == 0) return false;
        size_t c = 8;
        while (c + 2 <= len) {
            const uint8_t type = s[c], il = s[c + 1];
            if (type == kSdesEnd) break;
            if (c + 2 + il > len) break;
            if (type == item) { out.assign((const char*)s + c + 2, il); found = true; break; }
            c += 2 + (size_t)il;
        }
        return true;   // only the first SDES is read
    });
    return found;
}

std::string sanitizeCallsign(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (o.size() >= 32) break;
        if (std::isalnum((unsigned char)ch) || ch == '-' || ch == '/' || ch == '*' || ch == '_') o += ch;
    }
    return o;
}

bool splitSdesName(const std::string& nameItem, std::string& call, std::string& name) {
    const size_t sp = nameItem.find_first_of(" \t\r\n");
    call = sanitizeCallsign(nameItem.substr(0, sp));
    name.clear();
    if (sp != std::string::npos) {
        const size_t b = nameItem.find_first_not_of(" \t\r\n", sp);
        if (b != std::string::npos) name = nameItem.substr(b);
    }
    // the name is shown to the user: printable characters only
    name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return (unsigned char)c < 0x20; }), name.end());
    if (name.size() > 64) name.resize(64);
    return !call.empty();
}

// ---- RTP / text ------------------------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> makeAudioPacket(uint16_t seq, const uint8_t frames[kFramesPerPacket][kGsmFrameBytes]) {
    std::vector<uint8_t> v(kAudioPacketBytes, 0);
    v[0] = 0xC0; v[1] = kPtGsm; v[2] = (uint8_t)(seq >> 8); v[3] = (uint8_t)seq;   // time and SSRC stay 0
    for (int i = 0; i < kFramesPerPacket; i++) std::memcpy(&v[kRtpHeaderBytes + i * kGsmFrameBytes], frames[i], kGsmFrameBytes);
    return v;
}

AudioPortKind classifyAudioPort(const uint8_t* p, size_t n) {
    if (!p || n < 1) return AudioPortKind::Unknown;
    if (p[0] == 0xC0) return AudioPortKind::Audio;
    if (n >= 7 && std::memcmp(p + 1, "NDATA", 5) == 0) return p[6] == '\r' ? AudioPortKind::Info : AudioPortKind::Chat;
    return AudioPortKind::Unknown;
}

bool parseAudioPacket(const uint8_t* p, size_t n, uint16_t& seq, uint8_t& pt, const uint8_t*& frames, int& nframes) {
    if (!p || n < (size_t)kRtpHeaderBytes || p[0] != 0xC0) return false;
    pt = p[1];
    seq = (uint16_t)((p[2] << 8) | p[3]);
    if (pt != kPtGsm) return false;
    nframes = (int)std::min<size_t>((n - kRtpHeaderBytes) / kGsmFrameBytes, kFramesPerPacket);
    frames = p + kRtpHeaderBytes;
    return nframes > 0;
}

std::vector<uint8_t> makeInfoPacket(const std::string& info) {
    std::string s = "oNDATA\r" + info;
    std::replace(s.begin(), s.end(), '\n', '\r');
    std::vector<uint8_t> v(s.begin(), s.end());
    v.push_back(0);
    return v;
}

std::vector<uint8_t> makeChatPacket(const std::string& call, const std::string& message) {
    const std::string s = "oNDATA" + call + ">" + noCrLf(message) + "\r\n";
    std::vector<uint8_t> v(s.begin(), s.end());
    v.push_back(0);
    return v;
}

bool parseInfoPacket(const uint8_t* p, size_t n, std::string& info) {
    if (classifyAudioPort(p, n) != AudioPortKind::Info) return false;
    const uint8_t* nul = (const uint8_t*)std::memchr(p, 0, n);
    if (!nul || nul < p + 7) return false;
    info.assign((const char*)p + 7, (size_t)(nul - p - 7));
    std::replace(info.begin(), info.end(), '\r', '\n');
    return true;
}

bool parseChatPacket(const uint8_t* p, size_t n, std::string& text) {
    if (classifyAudioPort(p, n) != AudioPortKind::Chat) return false;
    const uint8_t* nul = (const uint8_t*)std::memchr(p, 0, n);
    if (!nul || nul < p + 6) return false;
    text.assign((const char*)p + 6, (size_t)(nul - p - 6));
    std::replace(text.begin(), text.end(), '\r', '\n');
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return true;
}

// ---- directory ---------------------------------------------------------------------------------------------------------------------------------
namespace {
std::string statusPart(DirStatus st, const std::string& hhmm) {
    switch (st) {
    case DirStatus::Online: return "ONLINE3.38(" + hhmm + ")";
    case DirStatus::Busy: return "BUSY3.40(" + hhmm + ")";
    default: return "OFF-V3.40";
    }
}
} // namespace

std::string loginCommand(const std::string& call, const std::string& password, DirStatus st, const std::string& hhmm, const std::string& location) {
    std::string loc = noCrLf(location);
    if (loc.size() > kMaxLocation) loc.resize(kMaxLocation);
    return "l" + upperCall(call) + "\xAC\xAC" + noCrLf(password) + "\r" + statusPart(st, hhmm) + "\r" + loc + "\r";
}

std::string loginCommandRedacted(const std::string& call, DirStatus st, const std::string& hhmm, const std::string& location) {
    return "l" + upperCall(call) + " **** " + statusPart(st, hhmm) + " " + noCrLf(location);
}

std::string upperCall(const std::string& s) {
    std::string o;
    for (char c : s) if (!std::isspace((unsigned char)c)) o += (char)std::toupper((unsigned char)c);
    return o;
}

bool validCallsign(const std::string& call) {
    if (call.size() >= 3 && call.front() == '*' && call.back() == '*') {
        for (size_t i = 1; i + 1 < call.size(); i++) if (!std::isalnum((unsigned char)call[i]) && call[i] != '-' && call[i] != '_') return false;
        return true;
    }
    std::string base = call;
    if (base.size() > 2 && base[base.size() - 2] == '-' && (base.back() == 'L' || base.back() == 'R')) base.resize(base.size() - 2);
    if (base.size() < 3 || base.size() > 10) return false;
    bool digit = false, alpha = false;
    for (char c : base) {
        if (std::isdigit((unsigned char)c)) digit = true;
        else if (std::isupper((unsigned char)c)) alpha = true;
        else if (c != '/') return false;
    }
    return digit && alpha;
}

StationKind stationKind(const std::string& call) {
    if (!call.empty() && call[0] == '*') return StationKind::Conference;
    if (call.size() > 2 && call.compare(call.size() - 2, 2, "-L") == 0) return StationKind::Link;
    if (call.size() > 2 && call.compare(call.size() - 2, 2, "-R") == 0) return StationKind::Repeater;
    return StationKind::User;
}

const char* stationKindName(StationKind k) {
    switch (k) {
    case StationKind::Link: return "Link";
    case StationKind::Repeater: return "Repeater";
    case StationKind::Conference: return "Conference";
    default: return "Station";
    }
}

void StationListParser::reset() { *this = StationListParser(); }

void StationListParser::feed(const char* p, size_t n) {
    if (st_ == Done || st_ == Failed) return;
    buf_.append(p, n);
    if (buf_.size() > (1u << 24)) { st_ = Failed; return; }   // 16 MB: not a station list
    for (;;) {
        if (st_ == Start) {
            if (buf_.size() < 4) return;
            if (buf_.compare(0, 4, "@@@\n") != 0) { st_ = Failed; return; }
            buf_.erase(0, 4);
            st_ = Count;
            continue;
        }
        if (st_ == End) {
            if (buf_.size() < 3) return;
            st_ = buf_.compare(0, 3, "+++") == 0 ? Done : Failed;
            return;
        }
        if (st_ == Done || st_ == Failed) return;
        const size_t nl = buf_.find('\n');
        if (nl == std::string::npos) return;
        std::string l = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        line(l);
    }
}

void StationListParser::line(const std::string& l) {
    switch (st_) {
    case Count: {
        left_ = std::atoi(l.c_str());
        if (left_ < 0 || left_ > 200000) { st_ = Failed; return; }
        st_ = left_ > 0 ? Call : End;
        break;
    }
    case Call: cur_ = Station(); cur_.call = l; st_ = Data; break;
    case Data: {
        const size_t b = l.rfind('[');
        if (b != std::string::npos) {
            const std::string tail = l.substr(b + 1);
            cur_.online = tail.find("ON") != std::string::npos;
            cur_.busy = tail.find("BUSY") != std::string::npos;
            const size_t sp = tail.find(' ');
            if (sp != std::string::npos) cur_.time = tail.substr(sp + 1, 5);
            cur_.description = l.substr(0, b);
        } else {
            cur_.description = l;
        }
        while (!cur_.description.empty() && cur_.description.back() == ' ') cur_.description.pop_back();
        st_ = Id;
        break;
    }
    case Id: cur_.id = std::atoi(l.c_str()); st_ = Ip; break;
    case Ip: {
        cur_.ip = l;
        if (cur_.call == ".") { st_ = Call; break; }   // separator, not counted
        if (cur_.call == " ") {
            msg_ += cur_.description + "\n";
        } else {
            cur_.call = sanitizeCallsign(cur_.call);
            cur_.kind = stationKind(cur_.call);
            if (!cur_.call.empty()) out_.push_back(cur_);
        }
        st_ = --left_ <= 0 ? End : Call;
        break;
    }
    default: break;
    }
}

bool StationListParser::badPassword() const { return msg_.rfind("INCORRECT PASSWORD", 0) == 0; }

std::vector<Station> searchStations(const std::vector<Station>& all, const std::string& query, unsigned kinds) {
    std::vector<Station> out;
    const std::string q = lower(query);
    for (const Station& s : all) {
        if (kinds && !(kinds & (1u << (unsigned)s.kind))) continue;
        if (!q.empty() && lower(s.call).find(q) == std::string::npos && lower(s.description).find(q) == std::string::npos &&
            std::to_string(s.id).find(q) == std::string::npos)
            continue;
        out.push_back(s);
    }
    return out;
}

} // namespace echolink
} // namespace dect2
