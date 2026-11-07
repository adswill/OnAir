// SpyServer client (issue #68): an Airspy, Airspy HF+ or RTL-SDR served over the network by Airspy's spyserver (protocol 2.0.1700, as
// SDR++'s spyserver_source speaks it). TCP, little endian. The client sends commands (a 32-bit type and body size, then the body): HELLO
// (the protocol version and a client name), then SET_SETTING (a setting id and a value) for the streaming mode, the IQ format, frequency and
// decimation, the gain and finally streaming on. The server sends messages with a 20-byte header (protocol id, message type with a gain in
// dB in its top 16 bits, stream type, sequence number, body size): DEVICE_INFO (once, after HELLO), CLIENT_SYNC (after HELLO and whenever
// the controlling client changes something) and the IQ samples (uint8, int16 or float pairs).
// The rates are the server's decimation steps: its radio's full rate / 2^n. A server that does not let this client control it ("read only":
// another client has it, or it is set so) keeps its own frequency and gain: this client may only place its IQ band within the band the
// radio receives, so the frequency follows the server's when it is outside that.
// Future work: the narrow "VFO" use (a small decimated band anywhere in the server's band, the FFT stream) is not offered; OnAir always
// takes the IQ band at the decimation its mode needs.
#include "../net/tcp_client.h"
#include "native_common.h"
#include "dect2/net/netradio.h"
#include "dect2/net/net_proto.h"
#include <cmath>
#include <map>
#include <mutex>
#include "dect2/common/endian.h"

namespace dect2 {
namespace native {

namespace {

enum : uint32_t { kCmdHello = 0, kCmdSetSetting = 2 };
enum : uint32_t {
    kSetStreamingMode = 0, kSetStreamingEnabled = 1, kSetGain = 2,
    kSetIqFormat = 100, kSetIqFrequency = 101, kSetIqDecimation = 102,
};
enum : uint32_t { kStreamIq = 1, kModeIqOnly = kStreamIq };
enum : uint32_t { kFmtUint8 = 1, kFmtInt16 = 2, kFmtInt24 = 3, kFmtFloat = 4 };
enum : uint32_t { kMsgDeviceInfo = 0, kMsgClientSync = 1, kMsgIqUint8 = 100, kMsgIqInt16 = 101, kMsgIqInt24 = 102, kMsgIqFloat = 103 };
enum : uint32_t { kDevInvalid = 0, kDevAirspyOne = 1, kDevAirspyHf = 2, kDevRtlSdr = 3 };
constexpr int kConnectMs = 3000, kStallMs = 3000;


std::string devName(uint32_t t) { return netp::spyDevName(t); }

bool splitHostPort(const std::string& a, std::string& host, int& port) { return netp::splitHostPort(a, host, port); }

// what the server said about itself (DEVICE_INFO and the latest CLIENT_SYNC)
struct SpyInfo {
    bool known = false;
    uint32_t devType = 0, serial = 0, maxRate = 0, maxBw = 0, decStages = 0, gainStages = 0, maxGainIndex = 0, minFreq = 0, maxFreq = 0,
             resolution = 0, minDec = 0, forcedFormat = 0;
    bool synced = false, canControl = false;
    uint32_t gain = 0, devCenter = 0, iqCenter = 0, minIqCenter = 0, maxIqCenter = 0;
};

// the servers seen, per address (nativeArgs), for the rate list and tuning range the app shows
std::mutex gInfoMu;
std::map<std::string, SpyInfo> gInfo;
SpyInfo infoFor(const std::string& addr) { std::lock_guard<std::mutex> lk(gInfoMu); auto it = gInfo.find(addr); return it == gInfo.end() ? SpyInfo{} : it->second; }


// (noteInfo is defined after widenRange, below)
void parseDeviceInfo(const unsigned char* b, uint32_t n, SpyInfo& i) {
    uint32_t v[12];
    netp::spyDeviceInfo(b, n, v);
    i.devType = v[0]; i.serial = v[1]; i.maxRate = v[2]; i.maxBw = v[3]; i.decStages = v[4]; i.gainStages = v[5]; i.maxGainIndex = v[6];
    i.minFreq = v[7]; i.maxFreq = v[8]; i.resolution = v[9]; i.minDec = v[10]; i.forcedFormat = v[11];
    i.known = true;
}
void parseClientSync(const unsigned char* b, uint32_t n, SpyInfo& i) {
    uint32_t v[9];
    netp::spyClientSync(b, n, v);
    i.canControl = v[0] != 0; i.gain = v[1]; i.devCenter = v[2]; i.iqCenter = v[3]; i.minIqCenter = v[5]; i.maxIqCenter = v[6];
    i.synced = true;
}
// The tuning range of the server's radio: what its DEVICE_INFO says, widened to the IQ centre range its CLIENT_SYNC allows (the limits the server
// really accepts; SDR# tunes an Airspy One past the 1.8 GHz of the stock DEVICE_INFO this way, issue #321). Nothing is narrowed.
void widenRange(SpyInfo& i) {
    if (!i.synced || i.maxIqCenter == 0 || i.maxIqCenter < i.minIqCenter) return;
    if (i.maxIqCenter > i.maxFreq) i.maxFreq = i.maxIqCenter;
    if (i.minIqCenter > 0 && (i.minFreq == 0 || i.minIqCenter < i.minFreq)) i.minFreq = i.minIqCenter;
}
void noteInfo(const std::string& addr, const SpyInfo& in) { SpyInfo i = in; widenRange(i); std::lock_guard<std::mutex> lk(gInfoMu); gInfo[addr] = i; }

bool sendCommand(sock_t s, uint32_t type, const unsigned char* body, uint32_t n) {
    const std::string b = netp::spyCommand(type, body, n);
    return sockSendAll(s, b.data(), b.size());
}
bool sendHello(sock_t s) {
    const std::string b = netp::spyHello();
    return sockSendAll(s, b.data(), b.size());
}

// one message: the header's fields and its body; false when the connection failed or stalled, or the message makes no sense
bool readMessage(sock_t s, uint32_t& type, uint32_t& flags, std::vector<unsigned char>& body) {
    unsigned char h[20];
    if (!sockRecvAll(s, h, sizeof h)) return false;
    uint32_t size = 0;
    if (!netp::spyHeader(h, type, flags, size)) return false;
    body.resize(size);
    return size == 0 || sockRecvAll(s, body.data(), size);
}

// HELLO, then DEVICE_INFO and CLIENT_SYNC within the socket's receive timeout
bool handshake(sock_t s, SpyInfo& info, const std::string& addr, std::string& err) {
    if (!sendHello(s)) { err = "the connection to " + addr + " dropped"; return false; }
    std::vector<unsigned char> body;
    for (int k = 0; k < 64 && !(info.known && info.synced); k++) {
        uint32_t t = 0, fl = 0;
        if (!readMessage(s, t, fl, body)) {
            err = info.known ? addr + " sent no client sync" : addr + " is not a SpyServer, or it refused this client (it may be full)";
            return false;
        }
        if (t == kMsgDeviceInfo) parseDeviceInfo(body.data(), (uint32_t)body.size(), info);
        else if (t == kMsgClientSync) parseClientSync(body.data(), (uint32_t)body.size(), info);
    }
    if (!info.known || info.devType == kDevInvalid || info.maxRate == 0) { err = addr + " has no radio connected"; return false; }
    return true;
}

// the rates the server offers: its radio's full rate / 2^n for its decimation stages, the fastest first
std::vector<double> ratesOf(const SpyInfo& i) { return netp::spyRates(i.maxRate, i.decStages, i.minDec); }

// an unknown server is listed as an Airspy R2 (10 Msps, 9 decimation stages, 24 to 1800 MHz, 21 gains) until it has been seen
SpyInfo orDefault(SpyInfo i) {
    if (i.known) return i;
    i.devType = kDevAirspyOne; i.maxRate = 10000000; i.decStages = 9; i.minDec = 0; i.maxGainIndex = 21; i.minFreq = 24000000; i.maxFreq = 1800000000;
    return i;
}

void setCaps(DeviceInfo& d, const SpyInfo& known) {
    const SpyInfo i = orDefault(known);
    const auto r = ratesOf(i);
    d.rateRanges.clear();
    for (auto it = r.rbegin(); it != r.rend(); ++it) d.rateRanges.push_back({*it, *it});   // the slowest first
    d.maxRateHz = r.empty() ? i.maxRate : r.front();
    d.minRateHz = r.empty() ? i.maxRate : r.back();
    d.minFreqHz = i.minFreq;
    d.maxFreqHz = i.maxFreq;
    // the server's gain is an index into its radio's gain steps: the gain slider goes over those steps
    d.gainMinDb = 0;
    d.gainMaxDb = std::max<uint32_t>(i.maxGainIndex, 1);
}

} // namespace

// The wire format without a socket (for the unit test): a command is type, body size (32-bit little endian each) and the body
std::vector<unsigned char> spyServerEncodeCommand(uint32_t type, const std::vector<unsigned char>& body) {
    const std::string b = netp::spyCommand(type, body.data(), (uint32_t)body.size());
    return std::vector<unsigned char>(b.begin(), b.end());
}
// A message from the server starts with 20 bytes: protocol version, type | flags << 16, stream type, sequence number, body size
bool spyServerDecodeHeader(const unsigned char* h, uint32_t& type, uint32_t& flags, uint32_t& size) {
    return netp::spyHeader(h, type, flags, size);
}
bool spyServerDecodeDeviceInfo(const unsigned char* b, uint32_t n, uint32_t& devType, uint32_t& maxRate, uint32_t& decStages, uint32_t& maxGainIndex) {
    if (n < 48) return false;
    SpyInfo i;
    parseDeviceInfo(b, n, i);
    devType = i.devType; maxRate = i.maxRate; decStages = i.decStages; maxGainIndex = i.maxGainIndex;
    return true;
}

bool spyServerProbe(const std::string& host, int port, int timeoutMs, std::string& err) {
    const sock_t s = tcpConnect(host, port, timeoutMs, err);
    if (s == kBadSock) return false;
    sockRecvTimeout(s, timeoutMs);
    SpyInfo info;
    const std::string addr = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    const bool ok = handshake(s, info, addr, err);
    if (ok) noteInfo(addr, info);
    sockShutdown(s);
    sockClose(s);
    return ok;
}

class SpyServerSource : public NativeSource {
public:
    explicit SpyServerSource(std::string addr) : addr_(std::move(addr)) {}
    ~SpyServerSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        std::string host;
        int port = 0;
        if (!splitHostPort(addr_, host, port)) { err = "SpyServer: bad address " + addr_ + " (host:port)"; openFailure_ = OpenFailure::Final; return false; }
        std::string e;
        const sock_t k = tcpConnect(host, port, kConnectMs, e);
        if (k == kBadSock) { err = "SpyServer: " + e; openFailure_ = OpenFailure::Retry; return false; }
        sock_ = k;
        sockRecvTimeout(sock_, kConnectMs);
        SpyInfo info;
        if (!handshake(sock_, info, addr_, e)) { err = "SpyServer: " + e; return false; }
        widenRange(info);
        noteInfo(addr_, info);
        { std::lock_guard<std::mutex> lk(infoMu_); info_ = info; }
        sockRecvTimeout(sock_, kStallMs);
        fprintf(stderr, "SpyServer: connected to %s (%s, serial %08X, %.3f Msps, %s)\n", addr_.c_str(), devName(info.devType).c_str(), (unsigned)info.serial,
                info.maxRate / 1e6, info.canControl ? "in control" : "read only: it keeps its own frequency and gain");
        fflush(stderr);
        if (lostAt_.time_since_epoch().count() != 0) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - lostAt_).count();
            const uint64_t n = (uint64_t)std::llround(sec * (rate_ > 0 ? rate_ : 0));
            if (ring_) ring_->noteLost(n);
            fprintf(stderr, "SpyServer: reconnected after %.1f s, about %llu samples lost\n", sec, (unsigned long long)n);
            fflush(stderr);
            lostAt_ = {};
        }
        sentDec_ = sentFreq_ = sentGain_ = sentFmt_ = UINT32_MAX;
        if (!setting(kSetStreamingMode, kModeIqOnly)) { err = "SpyServer: the connection to " + addr_ + " dropped"; return false; }
        if (!configure(s, err, false)) return false;
        if (!setting(kSetStreamingEnabled, 1)) { err = "SpyServer: the connection to " + addr_ + " dropped"; return false; }
        return true;
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        (void)live;
        if (sock_ == kBadSock) return false;
        SpyInfo i;
        { std::lock_guard<std::mutex> lk(infoMu_); i = info_; }
        const auto rates = ratesOf(i);
        if (rates.empty()) { err = "SpyServer: " + addr_ + " offers no sample rate"; openFailure_ = OpenFailure::Final; return false; }
        // the slowest rate at or above what is asked for (the fastest when none is)
        const double want = s.sampleRate > 0 ? s.sampleRate : rates.front();
        uint32_t dec = i.minDec;
        for (size_t k = 0; k < rates.size(); k++) if (rates[k] >= want * 0.9999) dec = i.minDec + (uint32_t)k;
        bool ok = true;
        // the wire format: 8-bit (the least network) or 16-bit; a server that forces one gets that
        uint32_t fmt = radioOption(s, "format", "8") == "16" ? kFmtInt16 : kFmtUint8;
        if (i.forcedFormat == kFmtUint8 || i.forcedFormat == kFmtInt16 || i.forcedFormat == kFmtFloat) fmt = i.forcedFormat;
        if (fmt != sentFmt_) { ok = ok && setting(kSetIqFormat, fmt); sentFmt_ = fmt; }
        if (dec != sentDec_) { ok = ok && setting(kSetIqDecimation, dec); sentDec_ = dec; }
        rate_ = (double)i.maxRate / (double)(1u << dec);
        requested_ = s.sampleRate;
        // the frequency as asked (outside the server radio's specified range with a note); a read-only client is told what the server gives
        double f = std::max(s.centerHz, 0.0);
        std::string warn;
        if (i.maxFreq > i.minFreq) addNote(warn, freqSpecNote("SpyServer radio", f, (double)i.minFreq, (double)i.maxFreq));
        if (!i.canControl && i.synced && i.maxIqCenter >= i.minIqCenter && i.maxIqCenter > 0) {
            const double lo = i.minIqCenter, hi = i.maxIqCenter;
            if (f < lo || f > hi) {
                char b[240];
                snprintf(b, sizeof b, "the SpyServer at %s is read only (another client controls it): it is tuned to %.3f MHz and gives %.3f to %.3f MHz",
                         addr_.c_str(), i.devCenter / 1e6, lo / 1e6, hi / 1e6);
                addNote(warn, b);
            }
        }
        const uint32_t hz = (uint32_t)std::llround(std::min(f, 4.29e9));
        if (hz != sentFreq_) { ok = ok && setting(kSetIqFrequency, hz); sentFreq_ = hz; }
        if (i.canControl) {
            const uint32_t g = (uint32_t)std::lround(std::min(std::max(s.gainDb, 0.0), (double)i.maxGainIndex));
            if (g != sentGain_) { ok = ok && setting(kSetGain, g); sentGain_ = g; }
        }
        if (!ok) { err = "SpyServer: the connection to " + addr_ + " dropped"; return false; }
        if (!warn.empty()) err = warn;
        else checkRate(s.sampleRate, err);
        return true;
    }
    void closeDevice() override {
        std::lock_guard<std::mutex> lk(sendMu_);
        if (sock_ != kBadSock) {
            sockShutdown(sock_);
            sockClose(sock_);
        }
        sock_ = kBadSock;
    }
    bool restartForRate() const override { return false; }   // the server changes the decimation while streaming
    void streamLoop() override {
        std::vector<unsigned char> body;
        while (run_) {
            uint32_t t = 0, fl = 0;
            if (!readMessage(sock_, t, fl, body)) break;   // closed, failed or stalled
            const float scale = fl ? 1.f / std::pow(10.f, (float)fl / 20.f) : 1.f;   // the server's digital gain, taken back out
            const size_t n = body.size();
            const unsigned char* b = body.data();
            if (t == kMsgIqUint8) {
                conv_.resize(n / 2);
                for (size_t k = 0; k < n / 2; k++) conv_[k] = cf32((b[2 * k] - 128.f) / 128.f * scale, (b[2 * k + 1] - 128.f) / 128.f * scale);
                push(conv_.data(), n / 2);
            } else if (t == kMsgIqInt16) {
                conv_.resize(n / 4);
                for (size_t k = 0; k < n / 4; k++) {
                    const int16_t re = (int16_t)(b[4 * k] | b[4 * k + 1] << 8), im = (int16_t)(b[4 * k + 2] | b[4 * k + 3] << 8);
                    conv_[k] = cf32(re / 32768.f * scale, im / 32768.f * scale);
                }
                push(conv_.data(), n / 4);
            } else if (t == kMsgIqFloat) {
                conv_.resize(n / 8);
                for (size_t k = 0; k < n / 8; k++) {
                    float re, im;
                    memcpy(&re, b + 8 * k, 4);
                    memcpy(&im, b + 8 * k + 4, 4);
                    conv_[k] = cf32(re * scale, im * scale);
                }
                scrubNonFinite(conv_.data(), n / 8);
                push(conv_.data(), n / 8);
            } else if (t == kMsgClientSync) {   // the controlling client moved the radio, or control changed hands
                std::lock_guard<std::mutex> lk(infoMu_);
                const bool was = info_.canControl;
                parseClientSync(b, (uint32_t)n, info_);
                if (was != info_.canControl) { fprintf(stderr, "SpyServer: %s\n", info_.canControl ? "now in control" : "now read only"); fflush(stderr); }
            }
        }
        if (run_) {
            fprintf(stderr, "SpyServer: lost the connection to %s\n", addr_.c_str());
            fflush(stderr);
            lostAt_ = std::chrono::steady_clock::now();
        }
    }
    void interrupt() override { if (sock_ != kBadSock) sockShutdown(sock_); }

private:
    bool setting(uint32_t id, uint32_t v) {
        const std::string c = netp::spySetSetting(id, v);
        std::lock_guard<std::mutex> lk(sendMu_);
        return sock_ != kBadSock && sockSendAll(sock_, c.data(), c.size());
    }

    std::string addr_;
    sock_t sock_ = kBadSock;
    std::mutex sendMu_, infoMu_;
    SpyInfo info_;
    uint32_t sentDec_ = UINT32_MAX, sentFreq_ = UINT32_MAX, sentGain_ = UINT32_MAX, sentFmt_ = UINT32_MAX;
    std::chrono::steady_clock::time_point lostAt_{};
};

DeviceInfo spyServerDevice(const std::string& host, int port) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "spyserver";
    const bool v6 = host.find(':') != std::string::npos;
    d.nativeArgs = (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    d.serial = d.nativeArgs;
    const SpyInfo i = infoFor(d.nativeArgs);
    d.name = "SpyServer " + d.nativeArgs + " (network)";
    setCaps(d, i);
    d.settings = {choiceSetting("format", "Wire format",
                                "The sample format the server sends. 8-bit needs the least network (2 bytes a sample); 16-bit keeps weak signals\n"
                                "next to strong ones apart better, at twice the bandwidth.",
                                {"8", "16"}, {"8-bit (low bandwidth)", "16-bit"}, "8")};
    return d;
}

void spyServerCaps(DeviceInfo& d) { setCaps(d, infoFor(d.nativeArgs)); }

std::unique_ptr<IqSource> makeSpyServer(const DeviceInfo& d) { return std::make_unique<SpyServerSource>(d.nativeArgs); }

} // namespace native
} // namespace dect2
