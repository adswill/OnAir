// The pure rules of the remote-control API: see dect2/net/api_policy.h.
#include "dect2/net/api_policy.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2::apipol {

// ------------------------------------------------------------------ token
bool tokenUsable(const std::string& t) {
    if (t.size() < kMinTokenLen || t.size() > 256) return false;
    for (unsigned char c : t) if (c <= 32 || c >= 127) return false;
    return true;
}

bool tokenEqual(const std::string& a, const std::string& b) {
    const size_t n = 256;
    unsigned diff = a.size() != b.size() ? 1u : 0u;
    for (size_t i = 0; i < n; i++) {
        const unsigned char x = i < a.size() ? (unsigned char)a[i] : 0, y = i < b.size() ? (unsigned char)b[i] : 0;
        diff |= (unsigned)(x ^ y);
    }
    if (a.size() > n || b.size() > n) diff |= 1u;
    return diff == 0;
}

static std::string lowerStr(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string extractToken(const std::string& auth, const std::string& query) {
    if (auth.size() > 7 && lowerStr(auth.substr(0, 7)) == "bearer ") {
        size_t a = 7, b = auth.size();
        while (a < b && auth[a] == ' ') a++;
        while (b > a && (auth[b - 1] == ' ' || auth[b - 1] == '\r' || auth[b - 1] == '\t')) b--;
        return auth.substr(a, b - a);
    }
    size_t i = 0;
    while (i < query.size()) {
        size_t e = query.find('&', i);
        if (e == std::string::npos) e = query.size();
        if (query.compare(i, 6, "token=") == 0) return query.substr(i + 6, e - i - 6);
        i = e + 1;
    }
    return std::string();
}

Auth authorise(bool loopback, bool requireLocal, const std::string& expected, const std::string& provided) {
    const bool need = !loopback || requireLocal;
    if (!need) return Auth::Allow;
    if (!tokenUsable(expected)) return Auth::NoTokenConfigured;
    return tokenEqual(provided, expected) ? Auth::Allow : Auth::NeedToken;
}

// ------------------------------------------------------------------ rate limit
bool RateLimiter::allow(const std::string& key, double now) {
    auto it = b_.find(key);
    if (it == b_.end()) {
        if (b_.size() >= maxKeys_) {   // forget the key that has been quiet the longest
            auto old = b_.begin();
            for (auto j = b_.begin(); j != b_.end(); ++j) if (j->second.t < old->second.t) old = j;
            b_.erase(old);
        }
        it = b_.emplace(key, B{burst_, now}).first;
    }
    B& b = it->second;
    const double dt = std::max(0.0, now - b.t);
    b.tokens = std::min(burst_, b.tokens + dt * rate_);
    b.t = now;
    if (b.tokens >= 1) { b.tokens -= 1; return true; }
    return false;
}

double RateLimiter::retryAfter(const std::string& key, double now) const {
    auto it = b_.find(key);
    if (it == b_.end() || rate_ <= 0) return 0;
    const double tok = std::min(burst_, it->second.tokens + std::max(0.0, now - it->second.t) * rate_);
    return tok >= 1 ? 0 : (1 - tok) / rate_;
}

// ------------------------------------------------------------------ sizes
Size checkSizes(size_t headerBytes, const std::string& cl, size_t pathBytes) {
    if (headerBytes > kMaxHeaderBytes) return Size::HeaderTooLarge;
    if (pathBytes > kMaxPathBytes) return Size::PathTooLong;
    if (cl.empty()) return Size::Ok;
    if (cl.size() > 9) return cl.find_first_not_of("0123456789") == std::string::npos ? Size::BodyTooLarge : Size::BadLength;
    for (char c : cl) if (c < '0' || c > '9') return Size::BadLength;
    return (size_t)std::strtoul(cl.c_str(), nullptr, 10) > kMaxBodyBytes ? Size::BodyTooLarge : Size::Ok;
}

// ------------------------------------------------------------------ routes
namespace {
struct Row { const char* path; Ep ep; bool post; };
const Row kRows[] = {
    {"/api/v1/status", Ep::Status, false},       {"/api/v1/devices", Ep::Devices, false},   {"/api/v1/modes", Ep::Modes, false},
    {"/api/v1/telemetry", Ep::Telemetry, false}, {"/api/v1/spectrum", Ep::Spectrum, false}, {"/api/v1/rotator", Ep::Rotator, false},
    {"/api/v1/ws", Ep::Ws, false},               {"/api/v1/events", Ep::Events, false},     {"/api/v1/memories", Ep::Memories, false}, {"/api/v1/sessions", Ep::Sessions, false},
    {"/api/v1/scheduler", Ep::Scheduler, false}, {"/api/v1/audio", Ep::Audio, false},       {"/api/v1/recording", Ep::Recording, false},
    {"/api/v1/start", Ep::Start, true},          {"/api/v1/stop", Ep::Stop, true},          {"/api/v1/mode", Ep::Mode, true},
    {"/api/v1/frequency", Ep::Frequency, true},  {"/api/v1/samplerate", Ep::SampleRate, true}, {"/api/v1/gain", Ep::Gain, true},
    {"/api/v1/autogain", Ep::AutoGain, true},    {"/api/v1/bandwidth", Ep::Bandwidth, true}, {"/api/v1/squelch", Ep::Squelch, true},
    {"/api/v1/volume", Ep::Volume, true},        {"/api/v1/mute", Ep::Mute, true},          {"/api/v1/recording/start", Ep::RecordStart, true},
    {"/api/v1/recording/stop", Ep::RecordStop, true}, {"/api/v1/rotator/goto", Ep::RotatorGoto, true}, {"/api/v1/rotator/stop", Ep::RotatorStop, true},
    {"/api/v1/tx", Ep::TxStatus, false}, {"/api/v1/tx/start", Ep::TxStart, true}, {"/api/v1/tx/stop", Ep::TxStop, true},
    {"/api/v1/rotator/park", Ep::RotatorPark, true},
};
}

bool pathRefused(const std::string& path) {
    static const char* exact[] = {"key", "keys", "credential", "credentials", "secret", "secrets",
                                  "password", "passwords", "passwd", "token", "tokens", "security", "encrypt", "decrypt", "cipher", "auth", "login", "admin", "config", "settings"};
    static const char* contains[] = {"credential", "password", "secret", "decrypt", "encrypt", "security", "key"};
    size_t i = 0;
    while (i <= path.size()) {
        size_t e = path.find('/', i);
        if (e == std::string::npos) e = path.size();
        const std::string seg = lowerStr(path.substr(i, e - i));
        for (const char* c : contains) if (seg.find(c) != std::string::npos) return true;
        size_t a = 0;   // parts split at - _ .
        while (a <= seg.size()) {
            size_t b = seg.find_first_of("-_.", a);
            if (b == std::string::npos) b = seg.size();
            const std::string part = seg.substr(a, b - a);
            for (const char* x : exact) if (part == x) return true;
            a = b + 1;
        }
        i = e + 1;
    }
    return false;
}

Route parseRoute(const std::string& method, const std::string& path) {
    Route r;
    if (path.empty() || path[0] != '/') { r.status = 400; r.msg = "bad path"; return r; }
    if (path.size() > kMaxPathBytes) { r.status = 400; r.msg = "path too long"; return r; }
    for (unsigned char c : path) {
        if (c < 33 || c == 127 || c == '\\' || c == '%' || c >= 128) { r.status = 400; r.msg = "bad character in the path"; return r; }
    }
    if (path.find("//") != std::string::npos || path.find("/./") != std::string::npos || path.find("/..") != std::string::npos) { r.status = 400; r.msg = "bad path"; return r; }
    if (path == "/" || path == "/index.html") {
        r.ep = Ep::Page; r.status = 200; r.allow = "GET"; r.post = false;
        if (method != "GET") { r.status = 404; r.msg = "no such endpoint"; }   // POST / is not the page
        return r;
    }
    if (pathRefused(path)) { r.status = 403; r.msg = "this program's API has nothing of that kind"; return r; }
    if (path.compare(0, 5, "/api/") == 0 && path.size() > 6 && path[5] == 'v') {
        size_t e = path.find('/', 5);
        const std::string ver = path.substr(6, (e == std::string::npos ? path.size() : e) - 6);
        if (ver != "1") { r.status = 404; r.msg = "unsupported API version (this server speaks v1: /api/v1/)"; return r; }
    }
    auto verbOk = [&](bool post) {
        r.post = post; r.allow = post ? "POST" : "GET";
        if (method != r.allow) { r.status = 405; r.msg = post ? "use POST" : "use GET"; return false; }
        r.status = 200;
        return true;
    };
    if (path == "/api/v1" || path == "/api/v1/") { r.ep = Ep::Index; verbOk(false); return r; }
    if (path.compare(0, 10, "/api/v1/x/") == 0 && path.size() > 10) { r.ep = Ep::Extra; r.allow = "GET, POST"; r.status = (method == "GET" || method == "POST") ? 200 : 405; r.post = method == "POST"; if (r.status == 405) r.msg = "use GET or POST"; return r; }
    for (const Row& row : kRows) {
        if (path == row.path) { r.ep = row.ep; verbOk(row.post); return r; }
    }
    r.msg = "no such endpoint";
    return r;
}

std::vector<std::string> endpointList() {
    std::vector<std::string> v;
    for (const Row& row : kRows) v.push_back(std::string(row.post ? "POST " : "GET ") + row.path);
    return v;
}

// ------------------------------------------------------------------ message lines
std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o.push_back((char)c);
        }
    }
    return o + "\"";
}

static std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    char b[48];
    if (std::fabs(v) < 1e15 && v == std::floor(v)) snprintf(b, sizeof b, "%.0f", v);
    else snprintf(b, sizeof b, "%.9g", v);
    return b;
}

std::string eventLine(const automation::Event& e, uint64_t seq, size_t maxBytes) {
    std::string head = "{\"v\":" + std::to_string(kApiVersion);
    if (seq) head += ",\"seq\":" + std::to_string(seq);
    head += ",\"type\":" + jsonString(e.type) + ",\"time\":" + num(e.time) + ",\"mode\":" + jsonString(e.mode) + ",\"freqHz\":" + num(e.freqHz);
    std::string fields = ",\"fields\":{";
    bool first = true;
    for (const auto& kv : e.f) {
        if (!first) fields += ',';
        first = false;
        fields += jsonString(kv.first) + ":";
        switch (kv.second.t) {
        case automation::Value::Num: fields += num(kv.second.n); break;
        case automation::Value::Str: fields += jsonString(kv.second.s); break;
        case automation::Value::Bool: fields += kv.second.b ? "true" : "false"; break;
        default: fields += "null";
        }
    }
    fields += "}";
    std::string full = head + fields + "}";
    if (maxBytes && full.size() > maxBytes) {
        full = head + ",\"truncated\":true}";
        if (full.size() > maxBytes) full = "{\"v\":1,\"truncated\":true}";
    }
    return full;
}

uint64_t EventRing::push(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu_);
    q_.push_back(line);
    const uint64_t n = first_ + q_.size() - 1;
    while (q_.size() > cap_) { q_.pop_front(); first_++; }
    return n;
}

uint64_t EventRing::next() const {
    std::lock_guard<std::mutex> lk(mu_);
    return first_ + q_.size();
}

void EventRing::since(uint64_t& cursor, size_t maxN, std::vector<std::string>& out, bool* gap) const {
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t end = first_ + q_.size();
    if (gap) *gap = false;
    if (cursor == UINT64_MAX) { cursor = end; return; }
    if (cursor < first_) { if (gap) *gap = true; cursor = first_; }
    if (cursor > end) cursor = end;
    while (cursor < end && out.size() < maxN) out.push_back(q_[(size_t)(cursor++ - first_)]);
}

// ------------------------------------------------------------------ rigctl
namespace {
std::string rprt(int c) { return "RPRT " + std::to_string(c) + "\n"; }
bool parseNum(const std::string& s, double& v) {
    if (s.empty() || s.size() > 32) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(v);
}
std::vector<std::string> words(const std::string& l) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < l.size()) {
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) i++;
        size_t j = i;
        while (j < l.size() && l[j] != ' ' && l[j] != '\t') j++;
        if (j > i) w.push_back(l.substr(i, j - i));
        i = j;
    }
    return w;
}
bool modeKnown(const std::string& m) {
    static const char* k[] = {"AM", "FM", "WFM", "USB", "LSB", "CW", "CWR", "RTTY", "PKTUSB", "PKTLSB", "PKTFM", "FMN", "AMN", "SAM", "NFM"};
    for (const char* x : k) if (m == x) return true;
    return false;
}
} // namespace

std::string rigctlHandle(const std::string& lineIn, RigctlRadio& radio, bool& quit) {
    RigctlSession open;
    return rigctlHandle(lineIn, radio, quit, open);
}

namespace {
// "Host: x", "Origin: x", "Content-Type: x" ...: a header name (letters, digits, '-') directly followed by ':'
bool looksLikeHttpHeader(const std::string& line) {
    const size_t c = line.find(':');
    if (c == std::string::npos || c == 0 || c > 40) return false;
    for (size_t i = 0; i < c; i++) {
        const unsigned char ch = (unsigned char)line[i];
        if (!(std::isalnum(ch) || ch == '-')) return false;
    }
    return true;
}
} // namespace

std::string rigctlHandle(const std::string& lineIn, RigctlRadio& radio, bool& quit, RigctlSession& sess) {
    quit = false;
    if (lineIn.size() > kMaxRigctlLine) return rprt(-8);
    std::string line = lineIn;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    // Cross-protocol guard: a web page can POST text/plain to this port ("POST / HTTP/1.1", headers, then a body such as "F 145500000").
    // No rigctl command contains " HTTP/", so hang up at the request line, before any body line is executed.
    if (line.find(" HTTP/") != std::string::npos) { quit = true; return std::string(); }
    if (looksLikeHttpHeader(line)) { quit = true; return std::string(); }
    for (unsigned char c : line) if (c < 9 || (c > 13 && c < 32) || c == 127) return rprt(-8);
    std::vector<std::string> w = words(line);
    if (w.empty()) return rprt(-1);
    std::string cmd = w[0];
    if (cmd[0] == '+' || cmd[0] == ';' || cmd[0] == '|') cmd.erase(0, 1);   // extended-response prefixes: answered like the plain form
    if (cmd.empty()) return rprt(-1);
    // a leading VFO argument (rigctld -o style "f VFOA") is not supported: anything with the wrong count is -1
    char c0 = cmd[0];
    if (cmd == "\\auth") {
        if (w.size() != 2 || sess.token.empty() || !tokenEqual(w[1], sess.token)) {
            if (++sess.badAuth >= 3) { quit = true; return std::string(); }
            return rprt(-1);
        }
        sess.authed = true;
        return rprt(0);
    }
    if (cmd.size() == 1 && sess.required && !sess.authed && (c0 == 'F' || c0 == 'M' || c0 == 'V' || ((c0 == 'S' || c0 == 'T') && !(w.size() >= 2 && w[1] == "0")))) return rprt(-1);   // a change needs the token first
    if (cmd.size() == 1) {
        switch (c0) {
        case 'q': case 'Q': quit = true; return std::string();
        case 'f': {
            if (w.size() != 1) return rprt(-1);
            double hz;
            if (!radio.getFreq(hz)) return rprt(-11);
            char b[48]; snprintf(b, sizeof b, "%.0f\n", hz);
            return b;
        }
        case 'F': {
            double hz;
            if (w.size() != 2 || !parseNum(w[1], hz) || hz <= 0 || hz > 1e12) return rprt(-1);
            return radio.setFreq(hz) ? rprt(0) : rprt(-1);
        }
        case 'm': {
            if (w.size() != 1) return rprt(-1);
            std::string m; int pb = 0;
            if (!radio.getMode(m, pb)) return rprt(-11);
            return m + "\n" + std::to_string(pb) + "\n";
        }
        case 'M': {
            double pb = 0;
            if (w.size() != 3 || !parseNum(w[2], pb) || pb < 0 || pb > 1e7) return rprt(-1);
            std::string m = w[1];
            for (auto& c : m) c = (char)std::toupper((unsigned char)c);
            if (!modeKnown(m)) return rprt(-1);
            return radio.setMode(m, (int)pb) ? rprt(0) : rprt(-11);
        }
        case 't': {   // PTT: the radio says whether the app is transmitting
            if (w.size() != 1) return rprt(-1);
            bool on = false;
            radio.getPtt(on);
            return on ? std::string("1\n") : std::string("0\n");
        }
        case 'T': {   // PTT on starts the transmission the Transmit window is set up for; off stops it. A radio that cannot: RPRT -11
            if (w.size() != 2) return rprt(-1);
            std::string a = w[1];
            for (auto& c : a) c = (char)std::toupper((unsigned char)c);
            if (a == "0" || a == "OFF") { radio.setPtt(false); return rprt(0); }
            if (a == "1" || a == "ON" || a == "2" || a == "3") return radio.setPtt(true) ? rprt(0) : rprt(-11);
            return rprt(-1);
        }
        case 's': return w.size() == 1 ? std::string("0\nVFOA\n") : rprt(-1);                  // split off
        case 'S': {
            if (w.size() != 3) return rprt(-1);
            if (w[1] == "0") return rprt(0);
            return rprt(-11);
        }
        case 'v': return w.size() == 1 ? std::string("VFOA\n") : rprt(-1);
        case 'V': return w.size() == 2 && w[1] == "VFOA" ? rprt(0) : rprt(-11);
        case 'l': {
            if (w.size() != 2) return rprt(-1);
            double db;
            if (w[1] == "STRENGTH" && radio.getStrengthDb(db)) { char b[48]; snprintf(b, sizeof b, "%.0f\n", db); return b; }
            return rprt(-11);
        }
        default: break;
        }
    }
    if (cmd == "\\chk_vfo") return "0\n";
    if (cmd == "\\get_powerstat") return "1\n";
    return rprt(-4);
}

} // namespace dect2::apipol
