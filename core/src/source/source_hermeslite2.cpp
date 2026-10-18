// Hermes Lite 2 client: the HL2 speaks openHPSDR protocol 1 over UDP on port 1024 (HL2 wiki "Protocol"; XMC_SDR's HermesLite2_IO is the
// reference this follows). Discovery: 63 bytes EF FE 02 0.., broadcast; each radio answers EF FE 02 (idle) or 03 (streaming), its MAC, its
// gateware version, board id (6 for the HL2) and, at byte 0x13, how many receivers its gateware has. Start/stop: 64 bytes EF FE 04 01/00,
// sent from the socket the radio is to stream to.
// The radio then sends EP6 packets (EF FE 01 06, a sequence number, two 512-byte frames: 7F 7F 7F, five status bytes C0..C4 and 504 bytes
// of samples, all big endian) and expects a 1032-byte EP2 packet back for each one (its watchdog stops the stream without them). With N
// receivers a sample slot is N times 24-bit I and Q (6 bytes each) and a 16-bit microphone word: 504 / (6N + 2) slots per frame (63 for one).
// Every EP2 frame carries one control register in its C0..C4: 0x00 the sample rate (48, 96, 192 or 384 ksps, the same for every receiver),
// the open-collector outputs (the N2ADR filter board's relays) and the receiver count; 0x01 the TX NCO; 0x02 to 0x08 the RX1 to RX7 NCOs
// (0x12 to 0x16 RX8 to RX12); 0x0a the LNA gain (-12 to +48 dB). The radio is only received with here: MOX is never set and the TX samples
// are zero.
//
// Receivers (slices, as Thetis calls them): each one is its own entry in the radio list ("Hermes Lite 2 RX2 ..."), so every OnAir receiver
// can have one of them. They share one stream (Link below): the first one opened starts it, the receiver count in register 0x00 follows the
// highest one in use and changes while it streams. The packets do not say which layout they carry, and an HL2 sends a few more in the old
// one (measured: four, then one with both): until a packet has the new layout (its microphone words zero), packets are dropped and counted
// as lost. What one radio has once: the sample rate (set by the first receiver opened; another one asking
// for a different rate gets this one and a note), the LNA gain (the last one set), the filter board (the low-pass filter for the highest
// frequency in use passes the lower ones too).
// Future work: the 10 MHz external reference (VersaClock writes), the EP4 wideband bandscope.
//
// The same client also speaks to a Red Pitaya running Pavel Demin's "SDR transceiver compatible with HPSDR" (red-pitaya-notes,
// projects/sdr_transceiver_hpsdr; the server source was read, no real board was at hand). Differences from the HL2: its discovery reply
// carries board id 1 (Hermes), gateware 32 and no receiver count; the receive NCO takes frequencies up to 61.44 MHz; it has no LNA register
// (register 0x0a is not sent) and no N2ADR filter board; it has three receivers (NCO registers 0x02 to 0x04). The frame layout, the rate bits
// (C1[1:0]: 48/96/192/384 ksps), the receiver count (C4[5:3]) and start/stop are the HL2's own.
#include "native_common.h"
#include "../net/netcompat.h"
#include "dect2/net/netradio.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#ifdef _WIN32
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace dect2 {
namespace native {

namespace {

constexpr int kHl2Port = 1024;
constexpr int kFrame = 512, kHeader = 8, kPacket = kHeader + 2 * kFrame;   // 1032
constexpr int kFrameData = kFrame - 8;                                      // 504 bytes of samples after 7F 7F 7F C0..C4
constexpr int kMaxRx = 12;                                                  // the protocol's NCO registers: RX1 to RX12
constexpr int kBoardHl2 = 6, kBoardHermes = 1;   // board ids of the discovery reply: the HL2's, and the Red Pitaya server's (Hermes)
constexpr double kRates[] = {48000, 96000, 192000, 384000};   // speed bits 0..3
constexpr double kMaxFreqHz = 38.4e6;                          // half the AD9866's 76.8 MHz clock
constexpr double kRpMaxFreqHz = 61.44e6;                       // freq_max of the Red Pitaya server

// what differs between the radios this client talks to
struct Profile {
    const char* name;      // in messages and in the radio list
    const char* board;     // DeviceInfo::board
    int boardId;           // byte 10 of the discovery reply
    double maxHz;
    bool lna, filters;     // register 0x0a (the LNA gain), the N2ADR filter board
    int defaultRx, maxRx;  // receivers listed while the radio has not said how many it has; the most it can have
};
// the HL2's usual gateware has four receivers (its discovery reply says how many: that is what is listed once it has answered)
const Profile kHl2 = {"Hermes Lite 2", "hermeslite2", kBoardHl2, kMaxFreqHz, true, true, 4, kMaxRx};
const Profile kRedPitaya = {"Red Pitaya", "redpitaya", kBoardHermes, kRpMaxFreqHz, false, false, 3, 3};
constexpr int kStartMs = 1500, kStallMs = 3000;
constexpr int kSettleMs = 300;   // the longest a new receiver count is waited for in the packets

// samples per frame with nr receivers (63 with one)
constexpr int slotsPerFrame(int nr) { return kFrameData / (6 * nr + 2); }
// the NCO register of receiver rx (0-based)
constexpr uint8_t rxReg(int rx) { return (uint8_t)(rx < 7 ? 0x02 + rx : 0x12 + (rx - 7)); }

// the receiver count each radio said it has, by "host:port" as it was reached (the list shows that many)
std::mutex gNrMu;
std::map<std::string, int> gNr;
void noteReceivers(const std::string& hostPort, int n) {
    if (n <= 0) return;
    std::lock_guard<std::mutex> lk(gNrMu);
    gNr[hostPort] = n;
}
int knownReceivers(const std::string& hostPort) {
    std::lock_guard<std::mutex> lk(gNrMu);
    const auto it = gNr.find(hostPort);
    return it == gNr.end() ? 0 : it->second;
}
// the receiver count in a discovery reply of n bytes (0: not said)
int replyReceivers(const unsigned char* b, long long n) { return n > 0x13 && b[0x13] > 0 && b[0x13] <= kMaxRx ? b[0x13] : 0; }

// the receive socket: a blocking recv gives up after ms; on Windows an ICMP "port unreachable" from an earlier send does not break it
sock_t udpSocket(int recvMs) {
    netInit();
    const sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSock) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (const sockaddr*)&a, sizeof a) != 0) { sockClose(s); return kBadSock; }
#ifdef _WIN32
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
    DWORD t = (DWORD)recvMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
#else
    timeval tv{recvMs / 1000, (recvMs % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    return s;
}

long long recvFrom(sock_t s, unsigned char* b, size_t n, sockaddr_in& from) {
    socklen_t len = sizeof from;
    return recvfrom(s, (char*)b, (int)n, 0, (sockaddr*)&from, &len);
}

// host (a name or an IPv4 address) and port as a socket address; false when it cannot be found
bool resolve4(const std::string& host, int port, sockaddr_in& out) {
    netInit();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
    memcpy(&out, res->ai_addr, sizeof out);
    freeaddrinfo(res);
    return true;
}

std::string ip4(const sockaddr_in& a) {
    char b[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &a.sin_addr, b, sizeof b);
    return b;
}

bool splitHostPort(const std::string& a, std::string& host, int& port) {
    const size_t c = a.rfind(':');
    if (c == std::string::npos || c == 0) return false;
    host = a.substr(0, c);
    port = atoi(a.c_str() + c + 1);
    return port > 0 && port < 65536;
}

// the broadcast address of every IPv4 network this computer is on (the limited broadcast 255.255.255.255 too: it does not leave a
// computer with several networks on every one of them)
std::vector<uint32_t> broadcastAddresses() {
    std::vector<uint32_t> r{0xffffffffu};
#ifdef _WIN32
    ULONG len = 16384;
    std::vector<unsigned char> buf(len);
    auto* ad = (IP_ADAPTER_ADDRESSES*)buf.data();
    const ULONG fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, fl, nullptr, ad, &len) == ERROR_BUFFER_OVERFLOW) { buf.resize(len); ad = (IP_ADAPTER_ADDRESSES*)buf.data(); }
    if (GetAdaptersAddresses(AF_INET, fl, nullptr, ad, &len) == NO_ERROR) {
        for (auto* a = ad; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                const unsigned bits = u->OnLinkPrefixLength;
                if (bits == 0 || bits >= 32) continue;
                const uint32_t ip = ntohl(((const sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
                r.push_back(ip | (0xffffffffu >> bits));
            }
        }
    }
#else
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        for (auto* i = ifa; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            if (!(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) continue;
            r.push_back(ntohl(((const sockaddr_in*)i->ifa_broadaddr)->sin_addr.s_addr));
        }
        freeifaddrs(ifa);
    }
#endif
    std::sort(r.begin(), r.end());
    r.erase(std::unique(r.begin(), r.end()), r.end());
    return r;
}

// The radios that answer a discovery packet sent to each of to within timeoutMs (each one once, in the order they answered)
std::vector<FoundNetRadio> discover(const std::vector<sockaddr_in>& to, int timeoutMs, bool broadcast, int boardId) {
    std::vector<FoundNetRadio> found;
    const sock_t s = udpSocket(50);
    if (s == kBadSock) return found;
    if (broadcast) {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
    }
    unsigned char q[63] = {0xEF, 0xFE, 0x02};
    for (const auto& a : to) sockSendTo(s, q, sizeof q, a);
    std::vector<uint32_t> seen;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    unsigned char b[256];
    while (std::chrono::steady_clock::now() < end) {
        sockaddr_in from{};
        const long long n = recvFrom(s, b, sizeof b, from);
        if (n < 11 || b[0] != 0xEF || b[1] != 0xFE || (b[2] != 0x02 && b[2] != 0x03)) continue;
        if (b[10] != boardId) continue;   // another openHPSDR radio (Hermes, Angelia, ...): its registers differ
        if (std::find(seen.begin(), seen.end(), from.sin_addr.s_addr) != seen.end()) continue;
        seen.push_back(from.sin_addr.s_addr);
        FoundNetRadio f;
        f.host = ip4(from);
        f.busy = b[2] == 0x03;
        f.receivers = replyReceivers(b, n);
        noteReceivers(f.host + ":" + std::to_string(ntohs(from.sin_port)), f.receivers);
        char d[128];
        if (n > 0x15) snprintf(d, sizeof d, "MAC %02X:%02X:%02X:%02X:%02X:%02X, gateware %d.%d", b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[0x15]);
        else snprintf(d, sizeof d, "MAC %02X:%02X:%02X:%02X:%02X:%02X, gateware %d", b[3], b[4], b[5], b[6], b[7], b[8], b[9]);
        f.detail = d;
        if (f.receivers > 0) f.detail += ", " + std::to_string(f.receivers) + (f.receivers == 1 ? " receiver" : " receivers");
        found.push_back(f);
    }
    sockClose(s);
    return found;
}

double rateFor(double want, int& speed) {
    speed = 3;
    for (int k = 3; k >= 0; k--) if (kRates[k] >= want * 0.9999) speed = k;
    return kRates[speed];
}

// the N2ADR filter board's relays on the open-collector outputs (C2[7:1] of register 0x00): one low-pass filter per band, none above 30 MHz
uint8_t filterBits(double hz) {
    if (hz < 2.0e6) return 0x02;    // 160 m
    if (hz < 4.0e6) return 0x04;    // 80 m
    if (hz < 8.0e6) return 0x08;    // 60 / 40 m
    if (hz < 16.0e6) return 0x10;   // 30 / 20 m
    if (hz < 24.0e6) return 0x20;   // 17 / 15 m
    if (hz < 30.0e6) return 0x40;   // 12 / 10 m
    return 0x00;
}

class HpsdrSource;

// One radio's stream, shared by the receivers of it that are open (one HpsdrSource each). It has its own thread: it takes the packets, hands
// each receiver its samples and answers every packet with the registers (the radio's watchdog).
class Link {
public:
    struct Rx {   // what one receiver asks for
        uint32_t hz = 0;
        bool swapIq = false;
        bool filters = false;   // the filter board follows this one's frequency
    };
    Link(const Profile& p, std::string addr) : p_(p), addr_(std::move(addr)) {}
    ~Link() { shutdown(); }
    // the link of the radio at addr ("host:port"): the one in use, else a new one (also when the one in use has lost the radio)
    static std::shared_ptr<Link> get(const Profile& p, const std::string& addr);

    // Receiver rx (0-based) joins: the first one starts the stream at wantSpeed; a later one gets the rate in use (speed) and the stream
    // again with the receiver count it needs. false: err says why; retry: whether trying again later may help.
    bool attach(HpsdrSource* s, int rx, const Rx& c, int wantSpeed, int& speed, std::string& err, bool& retry);
    void detach(int rx);   // the last one stops the stream
    // a receiver's new settings; the rate changes only when no other receiver is open. Returns the rate in use (speed bits).
    int update(int rx, const Rx& c, int wantSpeed) {
        std::lock_guard<std::mutex> lk(mu_);
        rx_[rx] = c;
        if (!othersLocked(rx)) speed_ = wantSpeed;
        return speed_;
    }
    void setLna(int v) { lna_ = v; }
    bool dead() const { return dead_; }
    // returns after d, when the stream is lost, or on wake()
    void waitDead(std::chrono::milliseconds d) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!dead_) cv_.wait_for(lk, d);
    }
    void wake() { { std::lock_guard<std::mutex> lk(mu_); } cv_.notify_all(); }

private:
    bool othersLocked(int rx) const {
        for (int k = 0; k < kMaxRx; k++) if (k != rx && src_[k]) return true;
        return false;
    }
    int needNrLocked() const {   // receivers in register 0x00: up to the highest one open (the ones below it are sent too)
        int n = 1;
        for (int k = 0; k < kMaxRx; k++) if (src_[k]) n = k + 1;
        return n;
    }
    bool openLocked(int rx, std::string& err, bool& retry);
    void shutdown();
    void loop();
    uint32_t regValueLocked(uint8_t reg) const;
    void sendControl();
    void command(bool start) {
        unsigned char c[64] = {0xEF, 0xFE, 0x04, (unsigned char)(start ? 0x01 : 0x00)};
        sockSendTo(sock_, c, sizeof c, dev_);
    }

    const Profile& p_;
    std::string addr_;
    sockaddr_in dev_{};
    sock_t sock_ = kBadSock;
    std::thread th_;
    std::atomic<bool> run_{false}, dead_{false};
    std::mutex opMu_;           // attach, detach: one at a time
    bool open_ = false;         // (opMu_)
    mutable std::mutex mu_;     // the receivers and the registers, against the stream thread
    std::condition_variable cv_;
    HpsdrSource* src_[kMaxRx] = {};
    Rx rx_[kMaxRx];
    bool live_[kMaxRx] = {};    // has had samples (a loss is counted only for those)
    int speed_ = 3;             // the rate of every receiver (speed bits)
    int nr_ = 1, wantNr_ = 1;   // the receiver count the stream has, and the one it needs
    int nrRadio_ = 0;           // what the radio said it has (0: not said)
    std::atomic<int> lna_{32};
    bool settling_ = true;      // the receiver count changed (or the stream started): packets of another layout are dropped
    std::chrono::steady_clock::time_point settleAt_{};
    // the stream thread's own (the opening thread's before it starts)
    uint32_t txSeq_ = 0, rxSeq_ = 0;
    bool rxSeqKnown_ = false;
    int cc_ = 0;
    std::vector<cf32> buf_[kMaxRx];
};

// One receiver of a radio, as the radio list offers it ("Hermes Lite 2 RX2 ...")
class HpsdrSource : public NativeSource {
public:
    HpsdrSource(const Profile& p, std::string addr, int rx) : p_(p), addr_(std::move(addr)), rx_(rx) {}
    ~HpsdrSource() override { stop(); }
    // from the link's thread
    void deliver(const cf32* x, size_t n) { push(x, n); }
    void lostSamples(uint64_t n) { if (ring_ && n) ring_->noteLost(n); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (rx_ < 0 || rx_ >= p_.maxRx) {
            err = std::string(p_.name) + " has at most " + std::to_string(p_.maxRx) + " receivers: RX" + std::to_string(rx_ + 1) + " is not one of them";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        std::string unused;
        configure(s, unused, false);   // the settings, given to the link as it starts
        link_ = Link::get(p_, addr_);
        int speed = want_;
        bool retry = true;
        link_->setLna(lna_);
        if (!link_->attach(this, rx_, rxCfg(), want_, speed, err, retry)) {
            link_.reset();
            openFailure_ = retry ? OpenFailure::Retry : OpenFailure::Final;
            return false;
        }
        attached_ = true;
        rate_ = kRates[speed];
        std::string warn;
        notes(s, speed, warn);
        fprintf(stderr, "%s RX%d: receiving from %s at %.0f ksps\n", p_.name, rx_ + 1, addr_.c_str(), rate_ / 1e3);
        fflush(stderr);
        if (lostAt_.time_since_epoch().count() != 0) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - lostAt_).count();
            const uint64_t lost = (uint64_t)std::llround(sec * rate_);
            if (ring_) ring_->noteLost(lost);
            fprintf(stderr, "%s RX%d: back after %.1f s, about %llu samples lost\n", p_.name, rx_ + 1, sec, (unsigned long long)lost);
            fflush(stderr);
            lostAt_ = {};
        }
        err = warn;
        return true;
    }
    // kept here and given to the link: its thread sends the registers with its next packets (a few ms at any rate)
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        (void)live;
        int speed = 3;
        rateFor(s.sampleRate > 0 ? s.sampleRate : kRates[3], speed);
        want_ = speed;
        requested_ = s.sampleRate;
        // asked as is: the frequency word of the protocol is 32-bit Hz, which is all that is held to (4.29 GHz)
        const double f = std::min(std::max(ppmCorrectedHz(s.centerHz, radioPpm(s)), 0.0), 4.29e9);
        hz_ = (uint32_t)std::llround(f);
        lna_ = (int)std::lround(std::min(std::max(s.gainDb, -12.0), 48.0)) + 12;
        filters_ = p_.filters && radioFlag(s, "filters", true);
        swapIq_ = radioFlag(s, "swapiq", false);
        int got = speed;
        if (attached_ && link_) {
            link_->setLna(lna_);
            got = link_->update(rx_, rxCfg(), speed);
        }
        rate_ = kRates[got];
        notes(s, got, err);
        return true;
    }
    void closeDevice() override {
        if (link_ && attached_) link_->detach(rx_);
        attached_ = false;
        link_.reset();
    }
    bool restartForRate() const override { return false; }   // register 0x00 changes the rate while streaming
    void streamLoop() override {   // the samples come from the link's thread: this one waits for the end
        while (run_ && link_ && !link_->dead()) link_->waitDead(std::chrono::milliseconds(100));
        if (run_) lostAt_ = std::chrono::steady_clock::now();   // the radio is opened again
    }
    void interrupt() override { if (link_) link_->wake(); }

private:
    Link::Rx rxCfg() const { Link::Rx c; c.hz = hz_; c.swapIq = swapIq_; c.filters = filters_; return c; }
    // what is said about the settings: a frequency beyond the radio's range, a rate another receiver set, a rate too low for the channel
    void notes(const TuneSettings& s, int got, std::string& err) {
        char b[300];
        if (s.centerHz > p_.maxHz + 1) {
            snprintf(b, sizeof b, "the %s is specified for up to %.1f MHz; the driver may not reach %.3f MHz or the signal may be weak or absent", p_.name, p_.maxHz / 1e6, s.centerHz / 1e6);
            err = b;
        } else if (got < want_) {
            snprintf(b, sizeof b, "the receivers of a %s share one sample rate: RX%d runs at the %.0f ksps another receiver of it set, %.0f ksps were asked for", p_.name, rx_ + 1,
                     kRates[got] / 1e3, kRates[want_] / 1e3);
            err = b;
        } else checkRate(s.sampleRate, err);
    }

    const Profile& p_;
    std::string addr_;
    int rx_;
    std::shared_ptr<Link> link_;
    bool attached_ = false;
    // what configure() set
    uint32_t hz_ = 0;
    int want_ = 3, lna_ = 32;
    bool filters_ = false, swapIq_ = false;
    std::chrono::steady_clock::time_point lostAt_{};
};

std::mutex gLinkMu;
std::map<std::string, std::weak_ptr<Link>> gLinks;

std::shared_ptr<Link> Link::get(const Profile& p, const std::string& addr) {
    std::lock_guard<std::mutex> lk(gLinkMu);
    auto& w = gLinks[std::string(p.board) + "|" + addr];
    auto l = w.lock();
    if (!l || l->dead()) { l = std::make_shared<Link>(p, addr); w = l; }
    return l;
}

bool Link::attach(HpsdrSource* s, int rx, const Rx& c, int wantSpeed, int& speed, std::string& err, bool& retry) {
    std::lock_guard<std::mutex> op(opMu_);
    retry = true;
    const std::string rxName = std::string(p_.name) + " RX" + std::to_string(rx + 1);
    if (dead_) { err = std::string(p_.name) + ": the stream from " + addr_ + " was lost"; return false; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (open_ && nrRadio_ > 0 && rx >= nrRadio_) {
            err = "the " + std::string(p_.name) + " at " + addr_ + " has " + std::to_string(nrRadio_) + " receivers (its gateware): " + rxName + " is not there";
            retry = false;
            return false;
        }
        if (src_[rx] && src_[rx] != s) { err = rxName + " is open already"; retry = false; return false; }
        if (!othersLocked(rx)) speed_ = wantSpeed;
        src_[rx] = s;
        rx_[rx] = c;
        live_[rx] = false;
        wantNr_ = needNrLocked();
        speed = speed_;
    }
    if (!open_) {
        if (openLocked(rx, err, retry)) { open_ = true; return true; }
        {
            std::lock_guard<std::mutex> lk(mu_);
            src_[rx] = nullptr;
        }
        shutdown();
        return false;
    }
    // the stream thread asks for the receiver count this one needs, and waits for the packets to have it
    std::unique_lock<std::mutex> lk(mu_);
    const int need = wantNr_;
    if (!cv_.wait_for(lk, std::chrono::milliseconds(3000), [&] { return dead_.load() || (nr_ == need && !settling_); }) || dead_) {
        src_[rx] = nullptr;
        wantNr_ = needNrLocked();
        err = rxName + ": the stream from " + addr_ + " did not go over to " + std::to_string(need) + " receivers";
        return false;
    }
    return true;
}

void Link::detach(int rx) {
    std::lock_guard<std::mutex> op(opMu_);
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        src_[rx] = nullptr;
        any = othersLocked(rx);
        if (any) wantNr_ = needNrLocked();
    }
    if (!any) shutdown();
}

void Link::shutdown() {
    run_ = false;
    if (th_.joinable()) th_.join();   // its receive gives up within 100 ms
    if (sock_ != kBadSock) {
        if (!dead_) command(false);   // a lost stream is not stopped: a new link may be streaming from the radio already
        sockClose(sock_);
    }
    sock_ = kBadSock;
    open_ = false;
}

bool Link::openLocked(int rx, std::string& err, bool& retry) {
    std::string host;
    int port = 0;
    if (!splitHostPort(addr_, host, port)) { err = std::string(p_.name) + ": bad address " + addr_ + " (host:port)"; retry = false; return false; }
    if (!resolve4(host, port, dev_)) { err = std::string(p_.name) + ": cannot find the address " + host; return false; }
    sock_ = udpSocket(100);
    if (sock_ == kBadSock) { err = std::string(p_.name) + ": no UDP socket"; return false; }
    unsigned char b[1500];
    // how many receivers it has: its discovery reply says (an HL2's does; the Red Pitaya server's does not)
    {
        unsigned char q[63] = {0xEF, 0xFE, 0x02};
        sockSendTo(sock_, q, sizeof q, dev_);
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)) {
            sockaddr_in from{};
            const long long n = recvFrom(sock_, b, sizeof b, from);
            if (n >= 11 && from.sin_addr.s_addr == dev_.sin_addr.s_addr && b[0] == 0xEF && b[1] == 0xFE && (b[2] == 0x02 || b[2] == 0x03)) {
                std::lock_guard<std::mutex> lk(mu_);
                nrRadio_ = replyReceivers(b, n);
                noteReceivers(addr_, nrRadio_);
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (nrRadio_ > 0 && rx >= nrRadio_) {
            err = "the " + std::string(p_.name) + " at " + addr_ + " has " + std::to_string(nrRadio_) + " receivers (its gateware): RX" + std::to_string(rx + 1) + " is not there";
            retry = false;
            return false;
        }
        nr_ = wantNr_;
        settling_ = true;   // it may still be streaming the layout of a program before
        settleAt_ = std::chrono::steady_clock::now();
    }
    txSeq_ = 0; cc_ = 0; rxSeqKnown_ = false;
    // the registers first (the rate it starts at), then start; it streams to this socket's address and port
    sendControl(); sendControl();
    command(true);
    sendControl(); sendControl();
    const auto t0 = std::chrono::steady_clock::now();
    auto lastStart = t0;
    bool got = false;
    while (!got && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(kStartMs)) {
        sockaddr_in from{};
        const long long n = recvFrom(sock_, b, sizeof b, from);
        if (n >= kPacket && from.sin_addr.s_addr == dev_.sin_addr.s_addr && b[0] == 0xEF && b[1] == 0xFE && b[2] == 0x01 && b[3] == 0x06) got = true;
        else if (std::chrono::steady_clock::now() - lastStart > std::chrono::milliseconds(400)) { command(true); lastStart = std::chrono::steady_clock::now(); }
    }
    if (!got) {
        err = std::string(p_.name) + ": no samples from " + addr_ + " (is it on, and not streaming to another program?)";
        return false;
    }
    sendControl();
    fprintf(stderr, "%s: streaming from %s at %.0f ksps, %d receiver%s\n", p_.name, addr_.c_str(), kRates[speed_] / 1e3, nr_, nr_ == 1 ? "" : "s");
    fflush(stderr);
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

// whether frame f (7F 7F 7F, C0..C4, samples) has the layout of nr receivers: the microphone word of every slot is zero (the HL2 has no
// microphone; the Red Pitaya server sends zeros without a codec). A frame of another layout has samples there.
bool layoutFits(const unsigned char* f, int nr) {
    if (f[0] != 0x7F || f[1] != 0x7F || f[2] != 0x7F) return false;
    const int slot = 6 * nr + 2, per = slotsPerFrame(nr);
    for (int i = 0; i < per; i++) {
        const unsigned char* m = f + 8 + i * slot + 6 * nr;
        if (m[0] || m[1]) return false;
    }
    return true;
}

void Link::loop() {
    unsigned char b[1500];
    auto lastData = std::chrono::steady_clock::now(), lastStart = lastData;
    uint64_t settleLost = 0;   // samples dropped while the layout settles, per receiver
    while (run_) {
        // A new receiver count goes to the radio with the next packets. It keeps sending the old layout for a few packets (an HL2 sends
        // about four, then one with both in it), which the packets do not say: they are dropped until one has the new layout.
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (wantNr_ != nr_) {
                nr_ = wantNr_;
                settling_ = true;
                settleAt_ = std::chrono::steady_clock::now();
                fprintf(stderr, "%s: %s now streams %d receiver%s\n", p_.name, addr_.c_str(), nr_, nr_ == 1 ? "" : "s");
                fflush(stderr);
            }
        }
        sockaddr_in from{};
        const long long n = recvFrom(sock_, b, sizeof b, from);
        const auto now = std::chrono::steady_clock::now();
        if (n < kPacket || from.sin_addr.s_addr != dev_.sin_addr.s_addr || b[0] != 0xEF || b[1] != 0xFE || b[2] != 0x01 || b[3] != 0x06) {
            if (now - lastData > std::chrono::milliseconds(kStallMs)) break;   // gone: the receivers open the radio again
            if (now - lastData > std::chrono::milliseconds(500) && now - lastStart > std::chrono::milliseconds(500)) { command(true); lastStart = now; }
            continue;
        }
        lastData = now;
        const int nr = nr_, per = slotsPerFrame(nr), slot = 6 * nr + 2;
        const uint32_t seq = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
        uint32_t gap = 0;
        if (rxSeqKnown_ && seq != rxSeq_ + 1) gap = seq - rxSeq_ - 1;
        if (gap >= 100000) gap = 0;
        rxSeq_ = seq;
        rxSeqKnown_ = true;
        const unsigned char* f0 = b + kHeader;
        const unsigned char* f1 = b + kHeader + kFrame;
        const bool ok = f0[0] == 0x7F && f0[1] == 0x7F && f0[2] == 0x7F && f1[0] == 0x7F && f1[1] == 0x7F && f1[2] == 0x7F;
        if (settling_) {
            // a radio that puts something in the microphone word never shows the layout: after kSettleMs its packets are taken as they are
            const bool fits = layoutFits(f0, nr) && layoutFits(f1, nr);
            if (!fits && now - settleAt_ < std::chrono::milliseconds(kSettleMs)) {
                settleLost += (uint64_t)2 * per;
                sendControl();
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                settling_ = false;
            }
            cv_.notify_all();
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (int rx = 0; rx < nr; rx++) {
                HpsdrSource* s = src_[rx];
                if (!s) continue;
                if (live_[rx] && gap) s->lostSamples((uint64_t)gap * 2 * per);
                if (live_[rx] && settleLost) s->lostSamples(settleLost);
                if (!ok) continue;
                // the HL2 sends Q before I (measured with XMC_SDR): taken the right way round
                auto& out = buf_[rx];
                out.resize((size_t)2 * per);
                const bool swap = rx_[rx].swapIq;
                for (int h = 0; h < 2; h++) {
                    const unsigned char* p = (h ? f1 : f0) + 8 + 6 * rx;
                    for (int i = 0; i < per; i++, p += slot) {
                        const int32_t a = (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8) >> 8;
                        const int32_t c = (int32_t)((uint32_t)p[3] << 24 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 8) >> 8;
                        out[(size_t)h * per + i] = swap ? cf32(a / 8388608.f, c / 8388608.f) : cf32(c / 8388608.f, a / 8388608.f);
                    }
                }
                s->deliver(out.data(), out.size());
                live_[rx] = true;
            }
        }
        settleLost = 0;
        sendControl();   // one back for each one received: the radio's watchdog
    }
    if (run_) {
        fprintf(stderr, "%s: no samples from %s for %d s\n", p_.name, addr_.c_str(), kStallMs / 1000);
        fflush(stderr);
        dead_ = true;
        cv_.notify_all();
    }
}

// register reg as sent now (mu_)
uint32_t Link::regValueLocked(uint8_t reg) const {
    int first = -1;
    double top = -1;
    for (int k = 0; k < kMaxRx; k++) {
        if (!src_[k]) continue;
        if (first < 0) first = k;
        if (rx_[k].filters && rx_[k].hz > top) top = rx_[k].hz;
    }
    const uint32_t hz0 = first >= 0 ? rx_[first].hz : 0;
    switch (reg) {
    case 0x00: {   // speed in C1[1:0], the open-collector outputs in C2[7:1], the receiver count less one in C4[6:3], duplex (C4[2]: the RX NCOs are their own)
        const uint8_t oc = p_.filters && top >= 0 ? filterBits(top) : 0;
        return (uint32_t)(speed_ & 3) << 24 | (uint32_t)oc << 16 | (uint32_t)((nr_ - 1) & 0x0f) << 3 | 0x04;
    }
    case 0x01: return hz0;   // the TX NCO: at the first receiver's frequency
    case 0x0a: return 0x40 | (uint32_t)(lna_ & 0x3f);   // the LNA gain in its direct mode (C4[6]): -12 dB (0) to +48 dB (60)
    default:
        for (int k = 0; k < std::min(nr_, kMaxRx); k++) if (rxReg(k) == reg) return src_[k] ? rx_[k].hz : hz0;   // a receiver nobody has open: where the first one is
        return 0;
    }
}

// one EP2 packet: two registers (one per frame), every one of them in turn; the TX samples stay zero
void Link::sendControl() {
    uint8_t regs[2];
    uint32_t vals[2];
    {
        std::lock_guard<std::mutex> lk(mu_);
        uint8_t cyc[3 + kMaxRx];
        int n = 0;
        cyc[n++] = 0x00;
        cyc[n++] = 0x01;
        for (int k = 0; k < std::min(nr_, kMaxRx); k++) cyc[n++] = rxReg(k);
        if (p_.lna) cyc[n++] = 0x0a;
        for (int h = 0; h < 2; h++) { regs[h] = cyc[(cc_ + h) % n]; vals[h] = regValueLocked(regs[h]); }
        cc_ = (cc_ + 2) % n;
    }
    unsigned char p[kPacket];
    memset(p, 0, sizeof p);
    p[0] = 0xEF; p[1] = 0xFE; p[2] = 0x01; p[3] = 0x02;
    p[4] = (uint8_t)(txSeq_ >> 24); p[5] = (uint8_t)(txSeq_ >> 16); p[6] = (uint8_t)(txSeq_ >> 8); p[7] = (uint8_t)txSeq_;
    txSeq_++;
    for (int h = 0; h < 2; h++) {
        unsigned char* f = p + kHeader + h * kFrame;
        f[0] = f[1] = f[2] = 0x7F;
        f[3] = (uint8_t)(regs[h] << 1);   // MOX (bit 0) never set
        const uint32_t v = vals[h];
        f[4] = (uint8_t)(v >> 24); f[5] = (uint8_t)(v >> 16); f[6] = (uint8_t)(v >> 8); f[7] = (uint8_t)v;
    }
    sockSendTo(sock_, p, sizeof p, dev_);
}

// "host:port" or "host:port/rxN" (receiver N, 1-based): the address and the receiver (0-based)
bool splitSlice(const std::string& a, std::string& hostPort, int& rx) {
    rx = 0;
    const size_t s = a.rfind("/rx");
    if (s == std::string::npos) { hostPort = a; return true; }
    hostPort = a.substr(0, s);
    rx = atoi(a.c_str() + s + 3) - 1;
    return rx >= 0 && rx < kMaxRx;
}

// the receivers listed for the radio at hostPort: as many as it said it has, else the usual count
int listedReceivers(const Profile& p, const std::string& hostPort) {
    const int n = knownReceivers(hostPort);
    return std::min(n > 0 ? n : p.defaultRx, p.maxRx);
}

} // namespace

DeviceInfo hpsdr1Device(const Profile& p, const std::string& host, int port, int rx) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = p.board;
    const std::string hp = host + ":" + std::to_string(port);
    // RX1 is the address alone, as the one receiver was before: the settings saved for it stay its own
    d.nativeArgs = rx == 0 ? hp : hp + "/rx" + std::to_string(rx + 1);
    d.serial = d.nativeArgs;
    d.name = std::string(p.name) + " RX" + std::to_string(rx + 1) + " " + (port == kHl2Port ? host : hp) + " (network)";
    for (double r : kRates) d.rateRanges.push_back({r, r});
    d.minRateHz = kRates[0];
    d.maxRateHz = kRates[3];
    d.minFreqHz = 10e3;
    d.maxFreqHz = p.maxHz;
    const RadioSetting swap = boolSetting("swapiq", "Swap I and Q",
                                         "Mirror the spectrum if signals appear on the wrong side of the tuned frequency (the I and Q words of the stream are taken the other way round).",
                                         false);
    if (!p.lna) {   // no gain register is known for it: no gain control is offered
        d.settings = {ppmSetting(0.1), swap};
        return d;
    }
    d.gainMinDb = -12;
    d.gainMaxDb = 48;
    d.settings = {ppmSetting(0.1),
                  boolSetting("filters", "N2ADR filter board",
                              "Switch the low-pass filter for the band on the N2ADR filter board (its relays are on the open-collector outputs).\n"
                              "Above 30 MHz no filter is switched in. Without the board this changes nothing. With several receivers open, the filter\n"
                              "for the highest frequency among them is used.",
                              true)};
    return d;
}

std::vector<DeviceInfo> hpsdr1Devices(const Profile& p, const std::string& host, int port) {
    std::vector<DeviceInfo> v;
    const int n = listedReceivers(p, host + ":" + std::to_string(port));
    for (int rx = 0; rx < n; rx++) v.push_back(hpsdr1Device(p, host, port, rx));
    return v;
}

bool hpsdr1Probe(const Profile& p, const std::string& host, int port, int timeoutMs, std::string& err) {
    sockaddr_in a{};
    if (!resolve4(host, port, a)) { err = "cannot find the address " + host + " (a " + p.name + " needs an IPv4 address)"; return false; }
    const auto found = discover({a}, timeoutMs, false, p.boardId);
    if (!found.empty()) {
        noteReceivers(host + ":" + std::to_string(port), found[0].receivers);   // the list shows that many
        return true;
    }
    err = std::string("no ") + p.name + " answered at " + host + ":" + std::to_string(port) + " (wrong address, or the radio is off";
    err += p.boardId == kBoardHermes ? ", or its SDR transceiver app is not running)" : ")";
    return false;
}

std::unique_ptr<IqSource> makeHpsdr1(const Profile& p, const DeviceInfo& d) {
    std::string hp;
    int rx = 0;
    if (!splitSlice(d.nativeArgs, hp, rx)) rx = kMaxRx;   // refused when opened, with the reason
    return std::make_unique<HpsdrSource>(p, hp, rx);
}

DeviceInfo hermesLite2Device(const std::string& host, int port) { return hpsdr1Device(kHl2, host, port, 0); }
std::vector<DeviceInfo> hermesLite2Devices(const std::string& host, int port) { return hpsdr1Devices(kHl2, host, port); }
bool hermesLite2Probe(const std::string& host, int port, int timeoutMs, std::string& err) { return hpsdr1Probe(kHl2, host, port, timeoutMs, err); }
std::unique_ptr<IqSource> makeHermesLite2(const DeviceInfo& d) { return makeHpsdr1(kHl2, d); }

DeviceInfo redPitayaDevice(const std::string& host, int port) { return hpsdr1Device(kRedPitaya, host, port, 0); }
std::vector<DeviceInfo> redPitayaDevices(const std::string& host, int port) { return hpsdr1Devices(kRedPitaya, host, port); }
bool redPitayaProbe(const std::string& host, int port, int timeoutMs, std::string& err) { return hpsdr1Probe(kRedPitaya, host, port, timeoutMs, err); }
std::unique_ptr<IqSource> makeRedPitaya(const DeviceInfo& d) { return makeHpsdr1(kRedPitaya, d); }

} // namespace native

namespace {
std::vector<FoundNetRadio> discoverHpsdr1(int boardId, int timeoutMs) {
    std::vector<sockaddr_in> to;
    for (uint32_t b : native::broadcastAddresses()) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(native::kHl2Port);
        a.sin_addr.s_addr = htonl(b);
        to.push_back(a);
    }
    return native::discover(to, timeoutMs, true, boardId);
}
} // namespace

std::vector<FoundNetRadio> discoverHermesLite2(int timeoutMs) { return discoverHpsdr1(native::kBoardHl2, timeoutMs); }
std::vector<FoundNetRadio> discoverRedPitaya(int timeoutMs) { return discoverHpsdr1(native::kBoardHermes, timeoutMs); }

} // namespace dect2
