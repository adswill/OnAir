// SDRplay nRSP-ST (network) through SDRconnect's WebSocket API (spec 1.0.3, docs/nrsp-st.md): the nRSP-ST's own transport to the computer is
// SDRconnect's, which is not public; SDRconnect (the GUI, or its headless build with --websocket_port) is the program that talks to the radio
// and offers the radio's IQ to other programs on port 5454. So this source connects to SDRconnect, not to the radio. JSON text messages
// (event_type, property, value, device) control it; binary messages carry a 16-bit little-endian type (2 = interleaved signed 16-bit IQ of the
// primary device), then the samples.
// Verified from the public spec: the envelope, the event types, the property names, the binary types. Assumed (marked ASSUMED): the URL path "/",
// the IQ rate being device_sample_rate, lna_state 0 = most gain, the rate range. Not in the API at all (so not offered): AGC, IF gain, bias-T,
// notch filters, and any user name or password.
#include "../net/tcp_client.h"
#include "../net/ws_client.h"
#include "native_common.h"
#include "dect2/net/netradio.h"
#include <cmath>
#include <map>
#include <mutex>

namespace dect2 {
namespace native {

namespace {

constexpr int kConnectMs = 3000, kStallMs = 5000, kAnswerMs = 3000;
constexpr size_t kMaxMessage = 1u << 20;   // SDRconnect sends IQ in chunks of a few thousand samples; a message of 1 MiB is not one of its
constexpr const char* kPath = "/";            // ASSUMED: the spec names no path
constexpr double kMinRate = 62500, kMaxRate = 10e6;   // ASSUMED: the RSP family's range; the nRSP-ST datasheet says "up to 10 MHz of spectrum"

// ---- JSON: the messages are flat objects of strings (a bare number or true/false is taken as its text too) -------------------------------------

std::string jsonEscape(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += (char)c;
    }
    return r + "\"";
}

// One flat JSON object into key -> text. false when it is not one (malformed, nested values are skipped balanced, too deep or too long ones refused).
bool parseFlatJson(const std::string& j, std::map<std::string, std::string>& out) {
    out.clear();
    size_t i = 0;
    const size_t n = j.size();
    auto ws = [&] { while (i < n && (j[i] == ' ' || j[i] == '\t' || j[i] == '\r' || j[i] == '\n')) i++; };
    auto str = [&](std::string& s) {
        if (i >= n || j[i] != '"') return false;
        i++;
        s.clear();
        while (i < n && j[i] != '"') {
            char c = j[i++];
            if (c == '\\') {
                if (i >= n) return false;
                const char e = j[i++];
                if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else if (e == 'r') c = '\r';
                else if (e == 'u') {
                    if (i + 4 > n) return false;
                    const unsigned v = (unsigned)strtoul(j.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (v < 0x80) c = (char)v; else { if (v < 0x800) { s += (char)(0xC0 | v >> 6); c = (char)(0x80 | (v & 63)); } else { s += (char)(0xE0 | v >> 12); s += (char)(0x80 | (v >> 6 & 63)); c = (char)(0x80 | (v & 63)); } }
                } else c = e;
            }
            s += c;
            if (s.size() > 4096) return false;
        }
        if (i >= n) return false;
        i++;
        return true;
    };
    ws();
    if (i >= n || j[i++] != '{') return false;
    ws();
    if (i < n && j[i] == '}') return true;
    for (;;) {
        ws();
        std::string k, v;
        if (!str(k)) return false;
        ws();
        if (i >= n || j[i++] != ':') return false;
        ws();
        if (i >= n) return false;
        if (j[i] == '"') { if (!str(v)) return false; }
        else if (j[i] == '{' || j[i] == '[') {   // skipped
            int depth = 0;
            bool inStr = false;
            for (; i < n; i++) {
                const char c = j[i];
                if (inStr) { if (c == '\\') i++; else if (c == '"') inStr = false; }
                else if (c == '"') inStr = true;
                else if (c == '{' || c == '[') { if (++depth > 8) return false; }
                else if (c == '}' || c == ']') { if (--depth == 0) { i++; break; } }
            }
            if (depth != 0) return false;
        } else {
            const size_t b = i;
            while (i < n && j[i] != ',' && j[i] != '}' && j[i] != ' ' && j[i] != '\r' && j[i] != '\n' && j[i] != '\t') i++;
            v = j.substr(b, i - b);
            if (v.empty() || v.size() > 64) return false;
        }
        out[k] = v;
        if (out.size() > 64) return false;
        ws();
        if (i >= n) return false;
        if (j[i] == ',') { i++; continue; }
        return j[i] == '}';
    }
}

std::string msg(const char* event, const std::string& prop, const std::string& value) {
    return std::string("{\"event_type\":") + jsonEscape(event) + ",\"property\":" + jsonEscape(prop) + ",\"value\":" + jsonEscape(value) + "}";
}

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> r;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find(',', p);
        if (e == std::string::npos) e = s.size();
        std::string t = s.substr(p, e - p);
        while (!t.empty() && t.front() == ' ') t.erase(0, 1);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!t.empty()) r.push_back(t);
        p = e + 1;
    }
    return r;
}

bool splitHostPort(const std::string& a, std::string& host, int& port) {
    const size_t c = a.rfind(':');
    if (c == std::string::npos || c == 0) return false;
    host = a.substr(0, c);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    port = atoi(a.c_str() + c + 1);
    return port > 0 && port < 65536;
}

// what SDRconnect said about itself and its radio
struct NrspInfo {
    bool known = false;
    std::string apiVersion, devices, activeDevice, antennas, activeAntenna;
    bool haveLna = false, canControl = true;
    int lnaMin = 0, lnaMax = 0;
    double rate = 0;
};
std::mutex gInfoMu;
std::map<std::string, NrspInfo> gInfo;
NrspInfo infoFor(const std::string& addr) { std::lock_guard<std::mutex> lk(gInfoMu); auto it = gInfo.find(addr); return it == gInfo.end() ? NrspInfo{} : it->second; }
void noteInfo(const std::string& addr, const NrspInfo& i) { std::lock_guard<std::mutex> lk(gInfoMu); gInfo[addr] = i; }

// takes a property from get_property_response / property_changed into info
void absorb(const std::map<std::string, std::string>& m, NrspInfo& i) {
    const auto et = m.find("event_type"), pr = m.find("property"), va = m.find("value");
    if (et == m.end() || pr == m.end() || va == m.end()) return;
    if (et->second != "get_property_response" && et->second != "property_changed") return;
    const std::string& p = pr->second;
    const std::string& v = va->second;
    if (p == "api_version") { i.apiVersion = v; i.known = true; }
    else if (p == "valid_devices") i.devices = v;
    else if (p == "active_device") i.activeDevice = v;
    else if (p == "valid_antennas") i.antennas = v;
    else if (p == "active_antenna") i.activeAntenna = v;
    else if (p == "lna_state_min") { i.lnaMin = atoi(v.c_str()); i.haveLna = true; }
    else if (p == "lna_state_max") { i.lnaMax = atoi(v.c_str()); i.haveLna = true; }
    else if (p == "can_control") i.canControl = v == "true" || v == "1";
    else if (p == "device_sample_rate") { const double r = atof(v.c_str()); if (r > 0 && r < 1e9) i.rate = r; }
}

bool askProps(WsClient& ws, std::initializer_list<const char*> props) {
    for (const char* p : props) if (!ws.sendText(msg("get_property", p, ""))) return false;
    return true;
}

// connection, upgrade, then the first answers: api_version proves it is SDRconnect
bool connectAndAsk(WsClient& ws, const std::string& host, int port, int timeoutMs, NrspInfo& info, std::string& err) {
    const std::string addr = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    const sock_t s = tcpConnect(host, port, timeoutMs, err);
    if (s == kBadSock) { err = "cannot connect to SDRconnect at " + addr + ": " + err + " (is SDRconnect running with its WebSocket server on?)"; return false; }
    ws.attach(s);
    sockRecvTimeout(s, timeoutMs);
    std::string e;
    if (!ws.handshake(host, port, kPath, e)) { err = addr + ": " + e; return false; }
    if (!askProps(ws, {"api_version", "valid_devices", "active_device", "valid_antennas", "active_antenna", "lna_state_min", "lna_state_max", "can_control",
                       "device_sample_rate"})) { err = "the connection to " + addr + " dropped"; return false; }
    std::vector<unsigned char> m;
    std::map<std::string, std::string> kv;
    for (int k = 0; k < 64; k++) {
        const auto kind = ws.readMessage(m, kMaxMessage);
        if (kind == WsClient::Kind::Timeout) break;
        if (kind == WsClient::Kind::Closed || kind == WsClient::Kind::Bad) { err = addr + " closed the connection or sent bad data"; return false; }
        if (kind == WsClient::Kind::Text && parseFlatJson(std::string(m.begin(), m.end()), kv)) absorb(kv, info);
        if (info.known && !info.devices.empty() && info.rate > 0) break;
    }
    if (!info.known) { err = addr + " answered, but not as SDRconnect's WebSocket API (no api_version); is it the WebSocket port of SDRconnect (default 5454)?"; return false; }
    return true;
}

} // namespace

// the wire pieces without a socket (unit test)
bool nrspParseMessage(const std::string& json, std::map<std::string, std::string>& out) { return parseFlatJson(json, out); }
std::string nrspMessage(const char* event, const std::string& prop, const std::string& value) { return msg(event, prop, value); }

bool nrspStProbe(const std::string& host, int port, int timeoutMs, std::string& err) {
    WsClient ws;
    NrspInfo info;
    const bool ok = connectAndAsk(ws, host, port, timeoutMs, info, err);
    if (ok) noteInfo((host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port), info);
    if (ws.sock() != kBadSock) { ws.sendClose(); sockShutdown(ws.sock()); sockClose(ws.sock()); }
    return ok;
}

class NrspStSource : public NativeSource {
public:
    explicit NrspStSource(std::string addr) : addr_(std::move(addr)) {}
    ~NrspStSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        std::string host;
        int port = 0;
        if (!splitHostPort(addr_, host, port)) { err = "nRSP-ST: bad address " + addr_ + " (host:port)"; openFailure_ = OpenFailure::Final; return false; }
        NrspInfo info;
        std::string e;
        if (!connectAndAsk(ws_, host, port, kConnectMs, info, e)) { err = "nRSP-ST: " + e; openFailure_ = OpenFailure::Retry; return false; }
        sentFreq_ = sentRate_ = sentLna_ = -1;
        sentAntenna_.clear();
        // a device chosen in the settings (by display name, with the network mode after a colon)
        const std::string dev = radioOption(s, "device"), mode = radioOption(s, "mode");
        if (!dev.empty()) {
            if (!ws_.sendText(msg("selected_device_name", "", dev + (mode.empty() ? "" : ":" + mode)))) { err = "nRSP-ST: the connection to " + addr_ + " dropped"; return false; }
            if (!settle(info, 1000)) { err = "nRSP-ST: the connection to " + addr_ + " dropped"; return false; }
        }
        { std::lock_guard<std::mutex> lk(infoMu_); info_ = info; }
        noteInfo(addr_, info);
        sockRecvTimeout(ws_.sock(), kStallMs);
        fprintf(stderr, "nRSP-ST: connected to SDRconnect at %s (API %s, %s)\n", addr_.c_str(), info.apiVersion.c_str(),
                info.activeDevice.empty() ? "no device named" : info.activeDevice.c_str());
        fflush(stderr);
        if (lostAt_.time_since_epoch().count() != 0) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - lostAt_).count();
            const uint64_t n = (uint64_t)std::llround(sec * (rate_ > 0 ? rate_ : 0));
            if (ring_) ring_->noteLost(n);
            fprintf(stderr, "nRSP-ST: reconnected after %.1f s, about %llu samples lost\n", sec, (unsigned long long)n);
            fflush(stderr);
            lostAt_ = {};
        }
        if (!configure(s, err, false)) return false;
        if (!ws_.sendText(msg("device_stream_enable", "", "true")) || !ws_.sendText(msg("iq_stream_enable", "", "true"))) {
            err = "nRSP-ST: the connection to " + addr_ + " dropped";
            return false;
        }
        return true;
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        (void)live;
        if (ws_.sock() == kBadSock) return false;
        NrspInfo i;
        { std::lock_guard<std::mutex> lk(infoMu_); i = info_; }
        std::string warn;
        bool ok = true;
        auto set = [&](const char* prop, const std::string& v) { ok = ok && ws_.sendText(msg("set_property", prop, v)); };
        const long long rate = std::llround(s.rateChosen && s.sampleRate > 0 ? s.sampleRate : std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 2e6, kMinRate), kMaxRate));   // (3.4: a chosen rate goes to the server as it is)
        if (rate != sentRate_) { set("device_sample_rate", std::to_string(rate)); sentRate_ = rate; }
        // the radio's centre; the VFO is put on it, too, as the IQ stream is the whole band
        const long long hz = std::llround(std::max(s.centerHz, 0.0));
        if (hz != sentFreq_) { set("device_center_frequency", std::to_string(hz)); set("device_vfo_frequency", std::to_string(hz)); sentFreq_ = hz; }
        if (i.haveLna && i.lnaMax > i.lnaMin) {   // ASSUMED: lna_state 0 is the most gain (as in the RSP API), so the slider's gain is counted down from the maximum
            const int g = (int)std::lround(std::min(std::max(s.gainDb, 0.0), (double)(i.lnaMax - i.lnaMin)));
            const int lna = i.lnaMax - g;
            if (lna != sentLna_) { set("lna_state", std::to_string(lna)); sentLna_ = lna; }
        }
        const std::string ant = radioOption(s, "antenna");
        if (!ant.empty() && ant != sentAntenna_) { set("active_antenna", ant); sentAntenna_ = ant; }
        if (!ok) { err = "nRSP-ST: the connection to " + addr_ + " dropped"; return false; }
        rate_ = (double)rate;   // until SDRconnect names the rate it really runs (device_sample_rate in the stream)
        requested_ = s.sampleRate;
        if (!i.canControl) addNote(warn, "SDRconnect says this client cannot control the radio (another SDRconnect has control): frequency, rate and gain stay as that client sets them");
        if (i.rate > 0 && i.rate < kMinRate) addNote(warn, "SDRconnect reports a very low sample rate");
        if (s.centerHz < 1e3 || s.centerHz > 2e9) addNote(warn, freqSpecNote("nRSP-ST", s.centerHz, 1e3, 2e9));
        if (!warn.empty()) err = warn;
        else checkRate(s.sampleRate, err);
        return true;
    }
    void closeDevice() override {
        if (ws_.sock() != kBadSock) {
            ws_.sendClose();
            sockShutdown(ws_.sock());
            sockClose(ws_.sock());
        }
        ws_.attach(kBadSock);
    }
    bool restartForRate() const override { return false; }   // SDRconnect changes the rate while streaming (device_sample_rate is read-write since 1.0.7)
    void streamLoop() override {
        std::vector<unsigned char> m;
        std::map<std::string, std::string> kv;
        uint64_t bad = 0;
        bool overload = false;
        while (run_) {
            const auto kind = ws_.readMessage(m, kMaxMessage);
            if (kind == WsClient::Kind::Closed || kind == WsClient::Kind::Timeout) break;   // closed or stalled: the connection is opened again
            if (kind == WsClient::Kind::Bad) { fprintf(stderr, "nRSP-ST: bad data from %s, dropping the connection\n", addr_.c_str()); fflush(stderr); break; }
            if (kind == WsClient::Kind::Text) {
                if (!parseFlatJson(std::string(m.begin(), m.end()), kv)) { bad++; continue; }
                NrspInfo i;
                { std::lock_guard<std::mutex> lk(infoMu_); absorb(kv, info_); i = info_; }
                const auto p = kv.find("property"), v = kv.find("value");
                if (p == kv.end() || v == kv.end()) continue;
                if (p->second == "device_sample_rate" && i.rate > 0 && std::fabs(i.rate - rate_) > 0.5) rate_ = i.rate;
                else if (p->second == "overload" && (v->second == "true") != overload) {
                    overload = v->second == "true";
                    fprintf(stderr, "nRSP-ST: %s\n", overload ? "input overload: lower the gain" : "overload gone");
                    fflush(stderr);
                } else if (p->second == "started" && v->second == "false") ws_.sendText(msg("device_stream_enable", "", "true"));
                continue;
            }
            // binary: a 16-bit little-endian type, then the payload; only the primary device's IQ (2) is used
            if (m.size() < 2) { bad++; continue; }
            const unsigned t = m[0] | m[1] << 8;
            if (t != 2) continue;
            const size_t n = (m.size() - 2) / 4;
            conv_.resize(n);
            const unsigned char* b = m.data() + 2;
            for (size_t k = 0; k < n; k++) {
                const int16_t re = (int16_t)(b[4 * k] | b[4 * k + 1] << 8), im = (int16_t)(b[4 * k + 2] | b[4 * k + 3] << 8);
                conv_[k] = cf32(re / 32768.f, im / 32768.f);
            }
            if ((m.size() - 2) % 4) bad++;
            push(conv_.data(), n);
        }
        if (bad) { fprintf(stderr, "nRSP-ST: %llu malformed messages ignored\n", (unsigned long long)bad); fflush(stderr); }
        if (run_) {
            fprintf(stderr, "nRSP-ST: lost the connection to SDRconnect at %s\n", addr_.c_str());
            fflush(stderr);
            lostAt_ = std::chrono::steady_clock::now();
        }
    }
    void interrupt() override { if (ws_.sock() != kBadSock) sockShutdown(ws_.sock()); }

private:
    // reads and absorbs messages for up to ms (the answers to a device change)
    bool settle(NrspInfo& info, int ms) {
        sockRecvTimeout(ws_.sock(), ms);
        std::vector<unsigned char> m;
        std::map<std::string, std::string> kv;
        if (!askProps(ws_, {"active_device", "valid_antennas", "active_antenna", "lna_state_min", "lna_state_max", "device_sample_rate"})) return false;
        for (int k = 0; k < 32; k++) {
            const auto kind = ws_.readMessage(m, kMaxMessage);
            if (kind == WsClient::Kind::Timeout) break;
            if (kind != WsClient::Kind::Text && kind != WsClient::Kind::Binary) return false;
            if (kind == WsClient::Kind::Text && parseFlatJson(std::string(m.begin(), m.end()), kv)) absorb(kv, info);
        }
        return true;
    }

    std::string addr_;
    WsClient ws_;
    std::mutex infoMu_;
    NrspInfo info_;
    long long sentFreq_ = -1, sentRate_ = -1;
    int sentLna_ = -1;
    std::string sentAntenna_;
    std::chrono::steady_clock::time_point lostAt_{};
};

static void setCaps(DeviceInfo& d, const NrspInfo& i) {
    d.minRateHz = kMinRate;   // ASSUMED
    d.maxRateHz = kMaxRate;
    d.minFreqHz = 1e3;        // the datasheet: 1 kHz to 2 GHz
    d.maxFreqHz = 2e9;
    d.gainMinDb = 0;
    d.gainMaxDb = i.haveLna && i.lnaMax > i.lnaMin ? i.lnaMax - i.lnaMin : 9;   // the LNA states, counted as steps of gain
    d.settings.clear();
    const auto devs = splitComma(i.devices);
    if (!devs.empty()) {
        std::vector<std::string> vals{""}, names{"SDRconnect's choice"};
        for (const auto& v : devs) { vals.push_back(v); names.push_back(v); }
        d.settings.push_back(choiceSetting("device", "Radio in SDRconnect", "Which of the radios SDRconnect can see to open. It keeps its own choice if left alone.", vals, names, "", true));
        d.settings.push_back(choiceSetting("mode", "Network mode", "Full IQ: the whole visible band (fast network). IQ Lite: only the selected channels (a lower rate). Used with a chosen radio.",
                                           {"", "Full IQ", "IQ Lite"}, {"SDRconnect's choice", "Full IQ", "IQ Lite"}, "", true));
    }
    std::vector<std::string> av{""}, an{"as SDRconnect has it"};
    for (const auto& a : splitComma(i.antennas.empty() ? "Antenna A,Antenna B,Antenna C" : i.antennas)) { av.push_back(a); an.push_back(a); }   // the default list is ASSUMED
    d.settings.push_back(choiceSetting("antenna", "Antenna port", "The nRSP-ST has three antenna ports (two SMA, one BNC).", av, an, ""));
}

DeviceInfo nrspStDevice(const std::string& host, int port) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "nrspst";
    d.nativeArgs = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    d.serial = d.nativeArgs;
    d.name = "SDRplay nRSP-ST via SDRconnect " + d.nativeArgs + " (network)";
    setCaps(d, infoFor(d.nativeArgs));
    return d;
}

void nrspStCaps(DeviceInfo& d) { setCaps(d, infoFor(d.nativeArgs)); }

std::unique_ptr<IqSource> makeNrspSt(const DeviceInfo& d) { return std::make_unique<NrspStSource>(d.nativeArgs); }

} // namespace native
} // namespace dect2
