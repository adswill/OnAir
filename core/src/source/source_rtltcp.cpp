// rtl_tcp client: an RTL-SDR served over the network by rtl_tcp (librtlsdr's server, also SDR# / SDR++ / OpenWebRX servers that speak it),
// and the rtl_tcp-compatible servers of other radios (rsp_tcp for SDRplay, airspy_tcp, hackrf_tcp, SDR++'s rtl_tcp server).
// The protocol: on connect the server sends 12 bytes, "RTL0" (other servers: their own 4 characters), the tuner type and its number of
// gains (big endian 32-bit each), then streams unsigned 8-bit IQ without end. Commands are 5 bytes: one command byte and a big-endian 32-bit
// parameter. A real RTL-SDR drops samples above 2.56 Msps, so it is held to that; another radio ("Server radio", or found from the greeting:
// not RTL0, or a tuner type outside 1 to 6) takes any rate from 0.25 to 20 Msps and its gain in tenths of a dB as asked.
#include "../net/tcp_client.h"
#include "native_common.h"
#include "dect2/net/netradio.h"
#include "dect2/net/net_proto.h"
#include <map>
#include <mutex>
#include <set>
#include "dect2/common/endian.h"

namespace dect2 {
namespace native {

namespace {

enum : uint8_t {
    kCmdFreq = 0x01, kCmdRate = 0x02, kCmdGainMode = 0x03, kCmdGain = 0x04, kCmdPpm = 0x05, kCmdIfGain = 0x06, kCmdTestMode = 0x07,
    kCmdAgc = 0x08, kCmdDirect = 0x09, kCmdOffset = 0x0a, kCmdGainIndex = 0x0d, kCmdBiasTee = 0x0e,
};
enum { kTunerUnknown = 0, kE4000 = 1, kFC0012 = 2, kFC0013 = 3, kFC2580 = 4, kR820T = 5, kR828D = 6 };
constexpr int kLocalConnectMs = 10000;   // a driver app on this device (Android) accepts late while it starts its radio: wait longer than for the network
constexpr int kConnectMs = 8000;   // an unreachable server is reported after this (slow Wi-Fi links on phones need more than 3 s, #201)
constexpr int kGreetingMs = 10000; // a server still closing the previous client can take seconds to greet
constexpr int kStallMs = 3000;     // no samples for this long: the connection is taken as lost
constexpr int kLocalStallMs = 15000; // ... but a driver app on this device (Android) is throttled or paused by the phone while it is in the background or a USB transfer hangs:
                                     // closing the socket at 3 s made the driver log "Broken pipe" and could not bring the stream back, so wait much longer (#201)

// the last recv() ended because the receive timeout ran out (not because the peer closed the connection)
bool recvTimedOut() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// The tuner's gains, its name and the "host:port" form: net_proto.h (C++ or the Rust crate onair_net)
std::vector<int> tunerGains(int tuner) { return netp::rtlTunerGains(tuner); }
std::string tunerName(int t) { return netp::rtlTunerName(t); }
bool splitHostPort(const std::string& a, std::string& host, int& port) { return netp::splitHostPort(a, host, port); }

uint32_t be32(const unsigned char* p) { return (uint32_t)dect2::endian::loadBe32(p); }

constexpr double kOtherMinRate = 0.25e6, kOtherMaxRate = 20e6;
constexpr double kAnyMinRate = 0.1e6, kAnyMaxRate = 40e6;   // "Allow any sample rate (advanced)": only sanity limits, the server and the network decide the rest

// the measured stream of each connected server (by address), for the status line
std::mutex gStatMu;
std::map<std::string, RtlTcpThroughput> gStat;
void publishStat(const std::string& addr, const RtlTcpThroughput& t) { std::lock_guard<std::mutex> lk(gStatMu); gStat[addr] = t; }

// unsigned 8-bit sample to float, one table lookup instead of a subtraction and a division per sample
struct U8Lut { float v[256]; U8Lut() { for (int i = 0; i < 256; i++) v[i] = (i - 127.4f) / 128.f; } };
const U8Lut& u8lut() { static const U8Lut l; return l; }

// the servers found serving another radio than an RTL-SDR (by nativeArgs), for the rate limits the app shows
std::mutex gOtherMu;
std::set<std::string> gOther;
bool noted(const std::string& addr) { std::lock_guard<std::mutex> lk(gOtherMu); return gOther.count(addr) > 0; }

void setCaps(DeviceInfo& d, bool other, bool any = false) {
    if (any) { d.maxRateHz = kAnyMaxRate; d.minRateHz = kAnyMinRate; d.rateRanges.clear(); }
    else if (other) { d.maxRateHz = kOtherMaxRate; d.minRateHz = kOtherMinRate; d.rateRanges.clear(); }
    else { d.maxRateHz = 2.56e6; d.minRateHz = 0.9e6; d.rateRanges = {{0.225001e6, 0.3e6}, {0.900001e6, 2.56e6}}; }
}

RadioSetting customRateSetting() {
    RadioSetting r;
    r.type = RadioSetting::Number;
    r.key = "customrate";
    r.label = "Sample rate (0 = as the mode needs)";
    r.help = "Only with Allow any sample rate: the rate to ask the server for, in Msps. 0 = what the mode asks.";
    r.minV = 0; r.maxV = 40; r.step = 0.1; r.unit = "Msps"; r.def = "0";
    return r;
}

} // namespace

bool rtlTcpGreeting(const unsigned char* hdr, std::string& magic, bool& other) {
    netp::RtlGreeting g;
    if (!netp::rtlGreeting(hdr, 12, g)) return false;
    magic = g.magic;
    other = g.other;
    return true;
}
bool rtlTcpThroughputOf(const std::string& addr, dect2::RtlTcpThroughput& out) {
    std::lock_guard<std::mutex> lk(gStatMu);
    const auto it = gStat.find(addr);
    if (it == gStat.end()) return false;
    out = it->second;
    return out.valid;
}
void rtlTcpNoteServer(const std::string& addr, bool other) {
    std::lock_guard<std::mutex> lk(gOtherMu);
    if (other) gOther.insert(addr); else gOther.erase(addr);
}

static bool isLoopbackHost(const std::string& h) { return h == "localhost" || h == "::1" || h == "[::1]" || h.compare(0, 4, "127.") == 0; }

class RtlTcpSource : public NativeSource {
public:
    explicit RtlTcpSource(std::string addr) : addr_(std::move(addr)) {}
    ~RtlTcpSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        std::string host;
        int port = 0;
        if (!splitHostPort(addr_, host, port)) { err = "rtl_tcp: bad address " + addr_ + " (host:port)"; openFailure_ = OpenFailure::Final; return false; }
        std::string e;
        // one retry when nothing answered in time: a local server that is still starting its radio can accept a moment later (#201)
        const int connectMs = isLoopbackHost(host) ? kLocalConnectMs : kConnectMs;
        sock_t k = tcpConnect(host, port, connectMs, e);
        if (k == kBadSock && e.find("no answer") != std::string::npos && !openCancelled()) k = tcpConnect(host, port, connectMs, e);
        if (k == kBadSock) { err = "rtl_tcp: " + e; openFailure_ = OpenFailure::Retry; return false; }
        {
            std::lock_guard<std::mutex> lk(sendMu_);   // against abortOpen() from stop()
            sock_ = k;
            if (openCancelled()) { err = "rtl_tcp: stopped"; return false; }
        }
        sockRecvTimeout(sock_, kGreetingMs);
        unsigned char hdr[12];
        if (!sockRecvAll(sock_, hdr, sizeof hdr)) { err = "rtl_tcp: " + addr_ + " connected but sent no greeting (another client may have it)"; return false; }
        std::string magic;
        if (!rtlTcpGreeting(hdr, magic, detected_)) { err = "rtl_tcp: " + addr_ + " is not an rtl_tcp server"; openFailure_ = OpenFailure::Final; return false; }
        rtlTcpNoteServer(addr_, detected_);
        tuner_ = (int)be32(hdr + 4);
        const uint32_t nGains = be32(hdr + 8);
        gains_ = tunerGains(tuner_);
        local_ = isLoopbackHost(host);
        sockRecvTimeout(sock_, kStallMs);
        if (detected_) fprintf(stderr, "rtl_tcp: connected to %s (\"%s\", tuner type %d, %u gains): not an RTL-SDR, any rate up to 20 Msps\n", addr_.c_str(),
                               magic.c_str(), tuner_, (unsigned)nGains);
        else fprintf(stderr, "rtl_tcp: connected to %s (%s, %u gains)\n", addr_.c_str(), tunerName(tuner_).c_str(), (unsigned)nGains);
        fflush(stderr);
        // the samples missed while the connection was down (the receiver sees a gap of that many)
        if (lostAt_.time_since_epoch().count() != 0) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - lostAt_).count();
            const uint64_t n = (uint64_t)std::llround(sec * (rate_ > 0 ? rate_ : 0));
            lost_ += n;
            if (ring_) ring_->noteLost(n);
            fprintf(stderr, "rtl_tcp: reconnected after %.1f s, about %llu samples lost\n", sec, (unsigned long long)n);
            fflush(stderr);
            lostAt_ = {};
        }
        sentRate_ = sentFreq_ = 0;
        sentGain_ = sentPpm_ = sentDs_ = sentBias_ = -1000;
        carry_ = -1;
        if (!command(kCmdGainMode, 1) || !command(kCmdAgc, 0)) { err = "rtl_tcp: the connection to " + addr_ + " dropped"; return false; }   // manual gain: OnAir runs its own AGC
        return configure(s, err, false);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        (void)live;
        if (sock_ == kBadSock) return false;
        const std::string server = radioOption(s, "server", "auto");
        const bool any = radioOption(s, "anyrate", "0") == "1";
        const double custom = any ? radioNumber(s, "customrate", 0) * 1e6 : 0;
        bool other = detected_ || server == "other" || any;
        // Auto: a server that greets as an RTL-SDR is held to 2.56 Msps, but a mode that asks for more is let through up to 20 Msps (it may be
        // SDR++, OpenWebRX or rsp_tcp serving a faster radio): samples may drop if it is a real RTL-SDR
        if (!other && server == "auto" && s.sampleRate > 2.56e6 * 1.0001) {
            other = true;
            if (!autoNoted_) {
                autoNoted_ = true;
                fprintf(stderr, "rtl_tcp: %s greets as an RTL-SDR but %.2f Msps was asked for (Server radio: Auto): running it at that rate; "
                                "samples may drop if this really is an RTL-SDR (set Server radio to RTL-SDR to hold it at 2.56 Msps)\n", addr_.c_str(), s.sampleRate / 1e6);
                fflush(stderr);
            }
        }
        const double maxRate = any ? kAnyMaxRate : other ? kOtherMaxRate : 3.2e6;
        if ((custom > 0 ? custom : s.sampleRate) > maxRate * 1.0001 && !s.rateChosen) {   // (a rate the user chose goes to the server as it is)
            char b[200];
            if (other) snprintf(b, sizeof b, "an rtl_tcp server gives at most 20 Msps; this mode needs about %.0f Msps: use a faster radio", s.sampleRate / 1e6);
            else snprintf(b, sizeof b, "an RTL-SDR (rtl_tcp) gives at most 3.2 Msps; this mode needs about %.0f Msps: use a faster radio "
                                       "(another radio behind rtl_tcp: set its Server radio)", s.sampleRate / 1e6);
            err = b;
            openFailure_ = OpenFailure::Final;
            return false;
        }
        double want;
        if (any) want = std::min(std::max(custom > 0 ? custom : s.sampleRate > 0 ? s.sampleRate : 2.048e6, kAnyMinRate), kAnyMaxRate);
        else if (other) want = std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 2.048e6, kOtherMinRate), kOtherMaxRate);
        else {
            want = s.rateChosen && s.sampleRate > 0 ? s.sampleRate : std::min(s.sampleRate > 0 ? s.sampleRate : 2.048e6, 2.56e6);   // a real RTL-SDR drops samples above 2.56 Msps (unless the user chose the rate)
            if (want < 900001 && (want > 300000 || want <= 225000)) want = 900001;
        }
        const uint32_t rate = (uint32_t)std::lround(want);
        bool ok = true;
        if (rate != sentRate_) { ok = ok && command(kCmdRate, rate); sentRate_ = rate; rate_ = rate; }
        requested_ = s.sampleRate;
        asked_ = rate;
        statT0_ = {};   // the meter starts a new window at the new rate
        const double f = std::max(s.centerHz, 0.0);
        // Auto: nothing is sent, the server's own library picks the way down (an RTL-SDR Blog V4 server upconverts HF itself; a direct-sampling
        // request would break it, and the server does not say which dongle it is). OnAir does not refuse the frequency: a V3 or modified
        // dongle behind the server takes the I or Q input below. The server's tuner is only told so far, hence the note.
        const std::string mode = radioOption(s, "ds", "auto");
        const int ds = (mode == "i" || mode == "q") && f < 24e6 ? (mode == "i" ? 1 : 2) : 0;
        if (!other && mode == "auto" && f < 24e6 && (tuner_ == 5 || tuner_ == 6))
            addNote(err, "below 24 MHz the tuner of this server cannot receive on its own: a Blog V4 server upconverts HF itself, otherwise pick the I or Q input in the radio settings (a V3 has its HF input on Q); the signal may be weak or absent");
        if (f > 4.29e9) addNote(err, "the rtl_tcp protocol takes the frequency as 32-bit Hz (at most 4.29 GHz): it was cut to that");
        if (ds != sentDs_) { ok = ok && command(kCmdDirect, (uint32_t)ds); sentDs_ = ds; }
        const int ppm = (int)std::lround(radioPpm(s));
        if (ppm != sentPpm_) { ok = ok && command(kCmdPpm, (uint32_t)ppm); sentPpm_ = ppm; }
        const uint32_t hz = (uint32_t)std::llround(std::min(f, 4.29e9));
        if (hz != sentFreq_) { ok = ok && command(kCmdFreq, hz); sentFreq_ = hz; }
        const int bias = s.biasTee ? 1 : 0;
        if (bias != sentBias_ && (bias || sentBias_ == 1)) { ok = ok && command(kCmdBiasTee, (uint32_t)bias); }
        sentBias_ = bias;
        // the closest gain the tuner has (another radio's server: the gain as asked, in tenths of a dB)
        const int target = (int)std::lround(s.gainDb * 10);
        int best = other || gains_.empty() ? std::max(target, 0) : gains_[0];
        if (!other) for (int g : gains_) if (std::abs(g - target) < std::abs(best - target)) best = g;
        if (best != sentGain_) { ok = ok && command(kCmdGain, (uint32_t)best); sentGain_ = best; }
        if (!ok) err = "rtl_tcp: the connection to " + addr_ + " dropped";
        return ok;
    }
    void closeDevice() override {
        std::lock_guard<std::mutex> lk(sendMu_);
        if (sock_ != kBadSock) {
            if (sentBias_ == 1) { const unsigned char c[5] = {kCmdBiasTee, 0, 0, 0, 0}; sockSendAll(sock_, c, 5); }   // antenna power off
            sockShutdown(sock_);
            sockClose(sock_);
        }
        sock_ = kBadSock;
        sentBias_ = -1000;
    }
    bool restartForRate() const override { return false; }   // the server changes the rate while streaming
    void streamLoop() override {
        std::vector<unsigned char> buf(1 << 20);   // large reads: at 10 Msps the server sends 20 MB/s
        statT0_ = std::chrono::steady_clock::now();
        uint64_t winBytes = 0, totalBytes = 0;
        int stalledMs = 0;
        while (run_) {
            const long long k = sockRecv(sock_, buf.data() + (carry_ >= 0 ? 1 : 0), buf.size() - 1);
            if (k < 0 && local_ && recvTimedOut()) {   // a silent local driver is waited for, the connection stays open
                stalledMs += kStallMs;
                if (stalledMs < kLocalStallMs) {
                    if (stalledMs == kStallMs) { fprintf(stderr, "rtl_tcp: no samples from %s for %d s, still waiting (the driver app may be paused by the phone)\n", addr_.c_str(), kStallMs / 1000); fflush(stderr); }
                    continue;
                }
            }
            stalledMs = 0;
            if (k <= 0) break;   // closed, failed or stalled
            size_t n = (size_t)k;
            winBytes += n; totalBytes += n;
            if (carry_ >= 0) { buf[0] = (unsigned char)carry_; n++; carry_ = -1; }
            if (n & 1) { carry_ = buf[n - 1]; n--; }   // half an IQ pair: kept for the next read
            convert(buf.data(), n);
            const auto now = std::chrono::steady_clock::now();
            if (statT0_.time_since_epoch().count() == 0) { statT0_ = now; winBytes = 0; }
            const double sec = std::chrono::duration<double>(now - statT0_).count();
            if (sec >= 1.0) {
                RtlTcpThroughput t;
                t.valid = true;
                t.mbitPerSec = winBytes * 8.0 / sec / 1e6;
                t.achievedHz = winBytes / 2.0 / sec;
                t.askedHz = asked_;
                t.bytes = totalBytes;
                t.lostBytes = lost_ * 2;
                publishStat(addr_, t);
                statT0_ = now; winBytes = 0;
            }
        }
        if (run_) {
            fprintf(stderr, "rtl_tcp: lost the connection to %s\n", addr_.c_str());
            fflush(stderr);
            lostAt_ = std::chrono::steady_clock::now();
        }
    }
    void interrupt() override { if (sock_ != kBadSock) sockShutdown(sock_); }
    // stop() while a reconnect waits up to 10 s for the server's greeting (#201): the wait returns at once
    void abortOpen() override {
        std::lock_guard<std::mutex> lk(sendMu_);
        if (sock_ != kBadSock) sockShutdown(sock_);
    }

public:
    uint64_t lostSamples() const { return lost_; }

private:
    bool command(uint8_t cmd, uint32_t v) {
        const std::string c = netp::rtlCommand(cmd, v);
        std::lock_guard<std::mutex> lk(sendMu_);
        return sock_ != kBadSock && sockSendAll(sock_, c.data(), c.size());
    }
    void convert(const unsigned char* b, size_t bytes) {
        const size_t n = bytes / 2;
        conv_.resize(n);
        const float* lut = u8lut().v;
        for (size_t i = 0; i < n; i++) conv_[i] = cf32(lut[b[2 * i]], lut[b[2 * i + 1]]);
        push(conv_.data(), n);
    }

    std::string addr_;
    sock_t sock_ = kBadSock;
    std::mutex sendMu_;
    int tuner_ = kTunerUnknown;
    bool autoNoted_ = false;
    bool local_ = false;      // the server is on this device (127.x, localhost): a longer stall limit
    bool detected_ = false;   // the greeting says another radio than an RTL-SDR
    std::vector<int> gains_;
    uint32_t sentRate_ = 0, sentFreq_ = 0;
    int sentGain_ = -1000, sentPpm_ = -1000, sentDs_ = -1000, sentBias_ = -1000;
    int carry_ = -1;
    std::chrono::steady_clock::time_point lostAt_{};
    std::atomic<uint64_t> lost_{0};
    double asked_ = 0;   // the rate sent to the server
    std::chrono::steady_clock::time_point statT0_{};
};

// The entry of an rtl_tcp server at host:port
DeviceInfo rtlTcpDevice(const std::string& host, int port) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "rtltcp";
    const bool v6 = host.find(':') != std::string::npos;
    d.nativeArgs = (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    d.serial = d.nativeArgs;
    d.name = "rtl_tcp " + d.nativeArgs + " (network)";
    setCaps(d, noted(d.nativeArgs));
    d.gainMinDb = 0; d.gainMaxDb = 49.6;
    d.hasBiasTee = true;   // rtl_tcp of the RTL-SDR Blog library (command 0x0e); other servers ignore it
    d.settings = {choiceSetting("server", "Server radio",
                                "The radio behind this rtl_tcp server. A real RTL-SDR drops samples above 2.56 Msps, so OnAir holds it there and\n"
                                "picks its gains from the tuner's list. Auto (the default): the same, but if a mode needs more than 2.56 Msps the\n"
                                "rate is allowed up to 20 Msps (samples may drop if the server is a real RTL-SDR). RTL-SDR: always held at\n"
                                "2.56 Msps, nothing above 3.2 Msps. Other radio: an rtl_tcp-compatible server of another radio (rsp_tcp for\n"
                                "SDRplay, airspy_tcp, hackrf_tcp, SDR++'s rtl_tcp server): any rate from 0.25 to 20 Msps, the gain as set, still\n"
                                "8-bit samples. A server that greets as another radio is taken as one by itself.",
                                {"auto", "rtl", "other"}, {"Auto", "RTL-SDR (up to 2.56 Msps)", "Other radio (up to 20 Msps)"}, "auto"),
                  boolSetting("anyrate", "Allow any sample rate (advanced)",
                              "Lifts every limit on the rate asked from this server (0.1 to 40 Msps) and treats it as a radio that can take it. Your\n"
                              "server or network may not keep up: 8-bit IQ at 10 Msps is 160 Mbit/s, more than most Wi-Fi gives (gigabit Ethernet\n"
                              "does). Dropped samples show in the status line, with the measured speed.", false, false),
                  customRateSetting(),
                  ppmSetting(1),
                  choiceSetting("ds", "Direct sampling (HF)",
                                "Below 24 MHz the tuner cannot receive: the RTL2832U can sample an HF antenna on one of its inputs directly.\n"
                                "Auto: nothing is sent, the server's own library decides (an RTL-SDR Blog V4 upconverts HF itself).\n"
                                "I / Q: that input below 24 MHz (the RTL-SDR Blog V3 has its HF input on Q). Off: never.",
                                {"auto", "off", "i", "q"}, {"Auto", "Off", "I input", "Q input"}, "auto")};
    return d;
}

std::unique_ptr<IqSource> makeRtlTcp(const DeviceInfo& d) { return std::make_unique<RtlTcpSource>(d.nativeArgs); }

} // namespace native

void applyRadioOptionCaps(DeviceInfo& d, const TuneSettings& t) {
    if (d.kind == DeviceInfo::Native && d.board == "spyserver") { native::spyServerCaps(d); return; }   // as the server was last seen
    if (d.kind == DeviceInfo::Native && d.board == "nrspst") { native::nrspStCaps(d); return; }
    if (d.kind == DeviceInfo::Native && d.board == "tci") { native::tciCaps(d); return; }
    if (d.kind == DeviceInfo::Native && d.board == "kiwisdr") { native::kiwiSdrCaps(d); return; }
    if (d.kind == DeviceInfo::Native && d.board == "sdrpp") { native::sdrppCaps(d); return; }          // its rates, gain and controls as last seen
    if (d.kind != DeviceInfo::Native || d.board != "rtltcp") return;
    native::setCaps(d, radioOption(t, "server", "auto") == "other" || native::noted(d.nativeArgs), radioOption(t, "anyrate", "0") == "1");
}
bool rtlTcpThroughput(const std::string& addr, RtlTcpThroughput& out) { return native::rtlTcpThroughputOf(addr, out); }   // declared in dect2/net/netradio.h
} // namespace dect2
