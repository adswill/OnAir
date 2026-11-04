// ASTERIX receiver: UDP and file input, track table, demo traffic. See asterix_engine.h.
#include "dect2/limits.h"
#include "dect2/asterix/asterix_engine.h"
#include "../net/netcompat.h"
#ifndef _WIN32
#include <sys/select.h>
#include <sys/time.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthR = 6371008.8;
constexpr size_t kMaxTracks = limits::kAircraft;
constexpr size_t kTrail = 40;

double steadyS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

void asterixPolarToLatLon(double lat0, double lon0, double rhoNm, double thetaDeg, double altFt, double& lat, double& lon) {
    const double slant = rhoNm * 1852.0, h = altFt * 0.3048;
    const double ground = std::sqrt(std::max(0.0, slant * slant - h * h));
    const double d = ground / kEarthR, th = thetaDeg * kPi / 180.0, p1 = lat0 * kPi / 180.0;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(th));
    const double l2 = lon0 * kPi / 180.0 + std::atan2(std::sin(th) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    lat = p2 * 180.0 / kPi;
    lon = std::remainder(l2 * 180.0 / kPi, 360.0);
}
void asterixXyToLatLon(double lat0, double lon0, double x, double y, double& lat, double& lon) {
    const double dist = std::hypot(x, y);
    const double th = std::atan2(x, y) * 180.0 / kPi;
    asterixPolarToLatLon(lat0, lon0, dist / 1852.0, th, 0, lat, lon);
}

// ------------------------------------------------------------------ pcap
bool asterixPcapPayloads(const std::vector<uint8_t>& f, std::vector<std::vector<uint8_t>>& out, std::string& err) {
    if (f.size() < 24) { err = "too short for a capture file"; return false; }
    const uint32_t m = (uint32_t)f[0] | (uint32_t)f[1] << 8 | (uint32_t)f[2] << 16 | (uint32_t)f[3] << 24;
    bool little;
    if (m == 0xA1B2C3D4u || m == 0xA1B23C4Du) little = true;
    else if (m == 0xD4C3B2A1u || m == 0x4D3CB2A1u) little = false;
    else if (m == 0x0A0D0D0Au) { err = "this is a pcapng file: save it as classic pcap (Wireshark: File > Save As)"; return false; }
    else { err = "not a pcap file"; return false; }
    auto u32 = [&](size_t o) { return little ? ((uint32_t)f[o] | (uint32_t)f[o + 1] << 8 | (uint32_t)f[o + 2] << 16 | (uint32_t)f[o + 3] << 24)
                                              : ((uint32_t)f[o + 3] | (uint32_t)f[o + 2] << 8 | (uint32_t)f[o + 1] << 16 | (uint32_t)f[o] << 24); };
    const uint32_t link = u32(20);
    size_t pos = 24;
    while (pos + 16 <= f.size()) {
        const uint32_t incl = u32(pos + 8);
        pos += 16;
        if (incl > f.size() - pos) break;
        const uint8_t* p = f.data() + pos;
        size_t n = incl;
        pos += incl;
        size_t off = 0;
        if (link == 1) {
            if (n < 14) continue;
            uint16_t type = (uint16_t)(p[12] << 8 | p[13]);
            off = 14;
            while (type == 0x8100 && n >= off + 4) { type = (uint16_t)(p[off + 2] << 8 | p[off + 3]); off += 4; }
            if (type != 0x0800) continue;
        } else if (link == 113) {
            if (n < 16 || (p[14] << 8 | p[15]) != 0x0800) continue;
            off = 16;
        } else if (link == 101 || link == 228) {
            off = 0;
        } else { err = "unsupported link layer " + std::to_string(link); return false; }
        if (n < off + 20 || (p[off] >> 4) != 4) continue;
        const size_t ihl = (size_t)(p[off] & 15) * 4;
        if (ihl < 20 || n < off + ihl + 8 || p[off + 9] != 17) continue;
        if (((p[off + 6] & 0x3F) << 8 | p[off + 7]) != 0) continue;       // a fragment
        const size_t u = off + ihl;
        const size_t ulen = (size_t)(p[u + 4] << 8 | p[u + 5]);
        if (ulen < 8) continue;
        size_t plen = std::min(ulen - 8, n - u - 8);
        out.emplace_back(p + u + 8, p + u + 8 + plen);
    }
    return true;
}

// ------------------------------------------------------------------ demo traffic
namespace {
struct DemoAc { double radiusNm, phase0Deg, gsKt, fl, vrate; const char* cs; int addr; int squawk; int clockwise; };
const DemoAc kRadarAc[] = {
    {28, 10, 200, 120, 1200, "SAS4321", 0x4A1B2C, 2301, 1}, {45, 100, 250, 240, 0, "RYR58NK", 0x4CA7B1, 4521, 1}, {62, 200, 430, 330, 0, "DLH7PA", 0x3C65A1, 1234, 0},
    {75, 280, 450, 370, -800, "BAW88Z", 0x400F3A, 7001, 1}, {90, 330, 470, 390, 0, "AFR1141", 0x3944C8, 5567, 0},
};
const DemoAc kAdsbAc[] = {
    {20, 40, 160, 45, 2000, "WZZ1432", 0x471F2A, 0, 1}, {38, 80, 230, 180, 0, "FIN7CE", 0x461E40, 0, 0}, {55, 150, 300, 280, 0, "LOT3AW", 0x48AD0B, 0, 1},
    {70, 220, 440, 340, 0, "UAE77", 0x896A11, 0, 0}, {82, 300, 460, 360, 500, "KLM1823", 0x484A21, 0, 1}, {100, 20, 480, 410, 0, "SIA321", 0x76CD0E, 0, 0},
};
const DemoAc kTrackAc[] = {
    {33, 0, 190, 100, 0, "TVS3FD", 0, 3711, 0}, {50, 120, 260, 220, -1500, "EZY91", 0, 5201, 1}, {66, 190, 420, 310, 0, "ITY540", 0, 2000, 0},
    {80, 250, 450, 350, 0, "QTR9", 0, 1000, 1}, {95, 340, 470, 400, 0, "ETH715", 0, 6314, 1},
};
void demoPos(const DemoAc& a, double t, double& bearingDeg, double& trackDeg, double& gsKt) {
    const double dir = a.clockwise ? 1.0 : -1.0;
    const double degPerS = a.gsKt / 3600.0 / a.radiusNm * 180.0 / kPi;       // flying a circle of this radius round the origin
    bearingDeg = std::fmod(a.phase0Deg + dir * degPerS * t, 360.0);
    if (bearingDeg < 0) bearingDeg += 360.0;
    trackDeg = std::fmod(bearingDeg + (a.clockwise ? 90.0 : 270.0), 360.0);
    gsKt = a.gsKt;
}
}  // namespace

std::vector<uint8_t> asterixDemoBlock(int source, double t, double lat0, double lon0) {
    std::vector<std::vector<uint8_t>> recs;
    const double tod = std::fmod(43200.0 + t, 86400.0);
    int cat = source == 0 ? 48 : source == 1 ? 21 : 62;
    const DemoAc* set = source == 0 ? kRadarAc : source == 1 ? kAdsbAc : kTrackAc;
    const int n = source == 0 ? 5 : source == 1 ? 6 : 5;
    for (int i = 0; i < n; i++) {
        const DemoAc& a = set[i];
        double brg, trk, gs;
        demoPos(a, t, brg, trk, gs);
        asterix::Target x;
        x.cat = cat; x.sac = 25; x.sic = source + 1;
        x.hasTime = true; x.todS = tod;
        x.hasLevel = true; x.flightLevel = a.fl;
        x.hasGs = x.hasTrack = true; x.gsKt = gs; x.trackDeg = trk;
        x.callsign = a.cs;
        if (a.squawk) x.squawk = a.squawk;
        if (a.addr) x.address = a.addr;
        if (source == 0) {
            x.hasPolar = true; x.rhoNm = a.radiusNm; x.thetaDeg = brg;
            x.trackNo = 100 + i;
            x.kind = "Mode S roll-call + PSR";
        } else {
            x.hasLatLon = true;
            asterixPolarToLatLon(lat0, lon0, a.radiusNm, brg, 0, x.lat, x.lon);
            x.hasAltFt = source == 2; x.altFt = a.fl * 100.0;
            x.hasVrate = true; x.vrateFpm = a.vrate;
            x.trackNo = (source == 2 ? 1000 : 500) + i;
        }
        recs.push_back(asterix::encodeRecord(x));
    }
    return asterix::wrapBlock(cat, recs);
}

// ------------------------------------------------------------------ receiver
struct AsterixReceiver::Impl {
    struct Trk { AsterixTrack t; double lastS = 0; bool live = true; };
    mutable std::mutex mu;
    std::map<uint64_t, Trk> tracks;
    struct Src { AsterixSource s; double lastS = 0; };
    std::map<int, Src> sources;
    std::deque<std::string> recent;
    asterix::BlockStats stats;
    uint64_t datagrams = 0, bytes = 0, noIdentity = 0;
    uint64_t seq = 0;
    double lastSnapS = 0;
    bool haveOrigin = false;
    double olat = 0, olon = 0;
    double maxAge = 60;
    std::string error, input = "idle";
    std::function<void(const std::string&)> log;
    // UDP
    std::thread udpThread;
    std::atomic<bool> udpStop{false};
    sock_t sock = kBadSock;
    int port = 0;
    // file
    std::thread fileThread;
    std::atomic<bool> fileStop{false};
    std::atomic<double> fileFrac{-1};
    // demo
    std::thread demoThread;
    std::atomic<bool> demoStop{false};
    bool demoOn = false;
    std::mutex demoMu;
    std::condition_variable demoCv;

    ~Impl() { stopAll(); }

    void stopAll() {
        stopUdpLocked();
        fileStop = true;
        if (fileThread.joinable()) fileThread.join();
        stopDemoLocked();
    }
    void stopUdpLocked() {
        udpStop = true;
        if (udpThread.joinable()) udpThread.join();
        if (sock != kBadSock) { sockClose(sock); sock = kBadSock; }
        udpStop = false;
        port = 0;
    }
    void stopDemoLocked() {
        {
            std::lock_guard<std::mutex> lk(demoMu);
            demoStop = true;
        }
        demoCv.notify_all();
        if (demoThread.joinable()) demoThread.join();
        demoStop = false;
        demoOn = false;
    }

    static std::string lineOf(const asterix::Target& t) {
        char b[220];
        int n = snprintf(b, sizeof b, "CAT %03d  %d/%d", t.cat, t.sac, t.sic);
        if (t.trackNo >= 0) n += snprintf(b + n, sizeof b - (size_t)n, "  trk %d", t.trackNo);
        if (t.address >= 0) n += snprintf(b + n, sizeof b - (size_t)n, "  %06X", (unsigned)t.address);
        if (!t.callsign.empty()) n += snprintf(b + n, sizeof b - (size_t)n, "  %s", t.callsign.c_str());
        if (t.squawk >= 0) n += snprintf(b + n, sizeof b - (size_t)n, "  sq %04d", t.squawk);
        if (t.hasLevel) n += snprintf(b + n, sizeof b - (size_t)n, "  FL%03.0f", t.flightLevel);
        if (t.hasLatLon) n += snprintf(b + n, sizeof b - (size_t)n, "  %.4f %.4f", t.lat, t.lon);
        else if (t.hasPolar) n += snprintf(b + n, sizeof b - (size_t)n, "  %.1f NM %.1f deg", t.rhoNm, t.thetaDeg);
        else if (t.hasXy) n += snprintf(b + n, sizeof b - (size_t)n, "  x %.0f y %.0f m", t.xM, t.yM);
        if (!t.kind.empty()) n += snprintf(b + n, sizeof b - (size_t)n, "  [%s]", t.kind.c_str());
        if (t.partial) n += snprintf(b + n, sizeof b - (size_t)n, "  (record cut at an item of unknown length)");
        return b;
    }

    // decode a datagram (or a chunk of data blocks) and merge it; nowS is the receiver clock, live false for data from a file
    void process(const uint8_t* d, size_t n, double nowS, bool live) {
        std::vector<asterix::Target> out;
        asterix::BlockStats st;
        asterix::decodeBlocks(d, n, out, &st);
        std::lock_guard<std::mutex> lk(mu);
        stats.blocks += st.blocks; stats.records += st.records; stats.badBlocks += st.badBlocks; stats.partialRecords += st.partialRecords; stats.otherCategory += st.otherCategory;
        for (int c = 0; c < 256; c++) stats.perCat[c] += st.perCat[c];
        for (const auto& t : out) apply(t, nowS, live);
        seq++;
    }

    void apply(const asterix::Target& t, double nowS, bool live) {
        const int sk = (t.sac & 255) << 8 | (t.sic & 255);
        {
            Src& s = sources[sk];
            s.s.sac = t.sac; s.s.sic = t.sic;
            s.s.perCat[t.cat == 21 ? 0 : t.cat == 48 ? 1 : 2]++;
            s.s.reports++;
            s.lastS = nowS;
        }
        recent.push_back(lineOf(t));
        if (recent.size() > 80) recent.pop_front();
        uint64_t id;
        if (t.cat == 21) {
            if (t.address >= 0) id = (1ull << 33) | (uint64_t)t.address;
            else if (t.trackNo >= 0) id = (1ull << 32) | (uint64_t)t.trackNo;
            else { noIdentity++; return; }
        } else if (t.trackNo >= 0) id = (1ull << 32) | (uint64_t)t.trackNo;
        else if (t.address >= 0) id = (1ull << 33) | (uint64_t)t.address;
        else { noIdentity++; return; }
        const uint64_t key = (uint64_t)t.cat << 56 | (uint64_t)(t.sac & 255) << 48 | (uint64_t)(t.sic & 255) << 40 | id;
        Trk& k = tracks[key];
        AsterixTrack& a = k.t;
        a.key = key; a.cat = t.cat; a.sac = t.sac; a.sic = t.sic;
        k.lastS = nowS; k.live = live;
        a.reports++;
        if (t.trackNo >= 0) a.trackNo = t.trackNo;
        if (t.address >= 0) a.address = t.address;
        if (!t.callsign.empty()) a.callsign = t.callsign;
        if (t.squawk >= 0) a.squawk = t.squawk;
        if (t.hasLevel) { a.hasLevel = true; a.flightLevel = t.flightLevel; }
        if (t.hasAltFt) { a.hasAltFt = true; a.altFt = t.altFt; }
        if (t.hasGs) { a.hasGs = true; a.gsKt = t.gsKt; }
        if (t.hasTrack) { a.hasTrack = true; a.trackDeg = t.trackDeg; }
        if (t.hasVrate) { a.hasVrate = true; a.vrateFpm = t.vrateFpm; }
        if (!t.kind.empty()) a.kind = t.kind;
        if (!t.dep.empty()) a.dep = t.dep;
        if (!t.dst.empty()) a.dst = t.dst;
        if (t.hasTime) a.todS = t.todS;
        a.onGround = t.onGround;
        bool moved = false;
        double la = 0, lo = 0;
        if (t.hasLatLon) { la = t.lat; lo = t.lon; a.posReported = true; moved = true; }
        else if (haveOrigin && t.hasPolar) {
            asterixPolarToLatLon(olat, olon, t.rhoNm, t.thetaDeg, t.hasLevel ? t.flightLevel * 100.0 : (a.hasLevel ? a.flightLevel * 100.0 : 0.0), la, lo);
            a.posReported = false; moved = true;
        } else if (haveOrigin && t.hasXy) { asterixXyToLatLon(olat, olon, t.xM, t.yM, la, lo); a.posReported = false; moved = true; }
        if (moved) {
            a.hasPos = true; a.lat = la; a.lon = lo;
            if (a.trail.empty() || std::fabs(a.trail.back().first - la) + std::fabs(a.trail.back().second - lo) > 1e-4) {
                a.trail.emplace_back((float)la, (float)lo);
                if (a.trail.size() > kTrail) a.trail.erase(a.trail.begin());
            }
        }
        if (tracks.size() > kMaxTracks) {
            auto oldest = tracks.begin();
            for (auto it = tracks.begin(); it != tracks.end(); ++it) if (it->second.lastS < oldest->second.lastS) oldest = it;
            tracks.erase(oldest);
        }
    }
};

AsterixReceiver::AsterixReceiver() : p_(std::make_unique<Impl>()) {}
AsterixReceiver::~AsterixReceiver() = default;
void AsterixReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

void AsterixReceiver::ingest(const uint8_t* d, size_t n, double nowS) {
    const bool live = nowS != -2;
    p_->process(d, n, nowS < 0 ? steadyS() : nowS, live);
}

void AsterixReceiver::clear() {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& p = *p_;
    p.tracks.clear(); p.sources.clear(); p.recent.clear(); p.stats = asterix::BlockStats(); p.datagrams = p.bytes = p.noIdentity = 0;
    p.error.clear();
    p.seq++;
}
void AsterixReceiver::setOrigin(double lat, double lon, bool valid) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->olat = lat; p_->olon = lon; p_->haveOrigin = valid;
    p_->seq++;
}
void AsterixReceiver::setMaxAge(double s) { std::lock_guard<std::mutex> lk(p_->mu); p_->maxAge = s; }

bool AsterixReceiver::startUdp(int port, bool lan, const std::string& group, std::string& err) {
    if (port < 1 || port > 65535) { err = "the port must be 1 to 65535"; return false; }
    Impl& p = *p_;
    p.stopUdpLocked();
    netInit();
    sock_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) { err = "cannot open a UDP socket"; return false; }
    int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    const bool multi = !group.empty();
    a.sin_addr.s_addr = htonl(lan || multi ? INADDR_ANY : INADDR_LOOPBACK);
    if (::bind(s, (const sockaddr*)&a, sizeof a) != 0) { err = "cannot listen on UDP port " + std::to_string(port) + " (in use?)"; sockClose(s); return false; }
    if (multi) {
        ip_mreq mr;
        std::memset(&mr, 0, sizeof mr);
        if (inet_pton(AF_INET, group.c_str(), &mr.imr_multiaddr) != 1 || (ntohl(mr.imr_multiaddr.s_addr) >> 28) != 14) { err = "not a multicast group address (224.0.0.0 to 239.255.255.255)"; sockClose(s); return false; }
        mr.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr) != 0) { err = "cannot join the multicast group"; sockClose(s); return false; }
    }
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.sock = s; p.port = port;
        p.input = "UDP port " + std::to_string(port) + (multi ? " (multicast " + group + ")" : lan ? " (all interfaces)" : " (this computer only)");
        p.error.clear();
        p.seq++;
    }
    p.udpStop = false;
    Impl* ip = &p;
    p.udpThread = std::thread([ip, s]() {
        std::vector<uint8_t> buf(65536);
        while (!ip->udpStop.load()) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv{0, 200000};
            const int r = ::select((int)s + 1, &rd, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            const long long n = ::recvfrom(s, (char*)buf.data(), (int)buf.size(), 0, nullptr, nullptr);
            if (n <= 0) continue;
            {
                std::lock_guard<std::mutex> lk(ip->mu);
                ip->datagrams++; ip->bytes += (uint64_t)n;
            }
            ip->process(buf.data(), (size_t)n, steadyS(), true);
        }
    });
    return true;
}

void AsterixReceiver::stopUdp() {
    Impl& p = *p_;
    p.stopUdpLocked();
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.input.rfind("UDP", 0) == 0) p.input = "idle";
    p.seq++;
}

bool AsterixReceiver::loadFile(const std::string& path, std::string& err) {
    Impl& p = *p_;
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open the file"; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0);
    if (size <= 0) { err = "the file is empty"; return false; }
    if (size > (std::streamoff)512 * 1024 * 1024) { err = "the file is larger than 512 MB"; return false; }
    std::vector<uint8_t> data((size_t)size);
    in.read((char*)data.data(), size);
    if (!in) { err = "cannot read the file"; return false; }
    // format: pcap, or a stream of data blocks
    std::vector<std::vector<uint8_t>> payloads;
    bool raw = false;
    if (data.size() >= 4 && ((data[0] == 0xD4 && data[1] == 0xC3) || (data[0] == 0xA1 && data[1] == 0xB2) || (data[0] == 0x4D && data[1] == 0x3C) || (data[0] == 0xA1 && data[1] == 0xB2) || (data[0] == 0x0A && data[1] == 0x0D))) {
        if (!asterixPcapPayloads(data, payloads, err)) return false;
        if (payloads.empty()) { err = "no UDP datagrams in the capture"; return false; }
    } else {
        if (data.size() < 3 || ((size_t)data[1] << 8 | data[2]) < 3 || ((size_t)data[1] << 8 | data[2]) > data.size()) { err = "not ASTERIX data blocks or a pcap capture"; return false; }
        raw = true;
    }
    p.fileStop = true;
    if (p.fileThread.joinable()) p.fileThread.join();
    p.fileStop = false;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.input = "file " + path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
        p.error.clear();
        p.seq++;
    }
    p.fileFrac = 0;
    Impl* ip = &p;
    p.fileThread = std::thread([ip, data = std::move(data), payloads = std::move(payloads), raw]() mutable {
        if (raw) {
            size_t pos = 0;
            while (pos + 3 <= data.size() && !ip->fileStop.load()) {
                size_t end = pos;
                while (end + 3 <= data.size() && end - pos < (1u << 20)) {
                    const size_t len = (size_t)data[end + 1] << 8 | data[end + 2];
                    if (len < 3 || end + len > data.size()) break;
                    end += len;
                }
                if (end == pos) break;                                   // a bad length or a cut block: stop here
                ip->process(data.data() + pos, end - pos, steadyS(), false);
                pos = end;
                ip->fileFrac = (double)pos / (double)data.size();
            }
        } else {
            for (size_t i = 0; i < payloads.size() && !ip->fileStop.load(); i++) {
                {
                    std::lock_guard<std::mutex> lk(ip->mu);
                    ip->datagrams++; ip->bytes += payloads[i].size();
                }
                ip->process(payloads[i].data(), payloads[i].size(), steadyS(), false);
                ip->fileFrac = (double)(i + 1) / (double)payloads.size();
            }
        }
        ip->fileFrac = -1;
        std::lock_guard<std::mutex> lk(ip->mu);
        ip->seq++;
    });
    return true;
}

void AsterixReceiver::setDemo(bool on) {
    Impl& p = *p_;
    if (!on) {
        p.stopDemoLocked();
        std::lock_guard<std::mutex> lk(p.mu);
        if (p.input == "demo") p.input = "idle";
        p.seq++;
        return;
    }
    if (p.demoThread.joinable()) return;
    p.demoOn = true;
    {
        std::lock_guard<std::mutex> lk(p.mu);
        p.input = "demo";
        p.seq++;
    }
    Impl* ip = &p;
    p.demoThread = std::thread([ip]() {
        const double t0 = steadyS();
        for (;;) {
            const double t = steadyS() - t0;
            double lat, lon;
            {
                std::lock_guard<std::mutex> lk(ip->mu);
                lat = ip->haveOrigin ? ip->olat : 54.687;
                lon = ip->haveOrigin ? ip->olon : 25.28;
            }
            for (int s = 0; s < 3; s++) {
                const std::vector<uint8_t> b = asterixDemoBlock(s, t, lat, lon);
                ip->process(b.data(), b.size(), steadyS(), true);
            }
            std::unique_lock<std::mutex> lk(ip->demoMu);
            if (ip->demoCv.wait_for(lk, std::chrono::milliseconds(1000), [ip] { return ip->demoStop.load(); })) break;
        }
    });
}

bool AsterixReceiver::telemetry(AsterixTelemetry& out, uint64_t lastSeq) {
    Impl& p = *p_;
    std::lock_guard<std::mutex> lk(p.mu);
    const double now = steadyS();
    if (now - p.lastSnapS >= 0.5) { p.lastSnapS = now; p.seq++; }
    if (p.seq <= lastSeq) return false;
    // live tracks age out
    for (auto it = p.tracks.begin(); it != p.tracks.end();) {
        if (it->second.live && p.maxAge > 0 && now - it->second.lastS > p.maxAge) it = p.tracks.erase(it);
        else ++it;
    }
    AsterixTelemetry t;
    t.seq = p.seq;
    t.input = p.input; t.error = p.error;
    t.listening = p.sock != kBadSock;
    t.demo = p.demoOn;
    t.datagrams = p.datagrams; t.bytes = p.bytes; t.stats = p.stats; t.noIdentity = p.noIdentity;
    t.blocksOk = p.stats.records; t.blocksBad = p.stats.badBlocks + p.stats.partialRecords;
    t.haveOrigin = p.haveOrigin; t.originLat = p.olat; t.originLon = p.olon;
    t.fileFraction = p.fileFrac.load();
    std::vector<const Impl::Trk*> order;
    for (auto& kv : p.tracks) order.push_back(&kv.second);
    std::sort(order.begin(), order.end(), [](const Impl::Trk* a, const Impl::Trk* b) { return a->lastS > b->lastS; });
    for (const Impl::Trk* k : order) {
        AsterixTrack a = k->t;
        a.ageS = k->live ? now - k->lastS : 0;
        t.tracks.push_back(std::move(a));
    }
    for (auto& kv : p.sources) {
        AsterixSource s = kv.second.s;
        s.ageS = now - kv.second.lastS;
        t.sources.push_back(s);
    }
    t.recent.assign(p.recent.begin(), p.recent.end());
    t.dataValid = !t.tracks.empty();
    t.state = !t.tracks.empty() ? 2 : (t.listening || t.demo || t.fileFraction >= 0 || p.input.rfind("file", 0) == 0 ? 1 : 0);
    out = std::move(t);
    return true;
}

ModeTuning asterixTuning() {
    ModeTuning t;
    t.stdMode = 153; t.id = "asterix"; t.name = "ASTERIX";
    t.minMhz = 1; t.maxMhz = 6000; t.defMhz = 100;
    t.sampleRate = 2e6;
    t.bandwidthMhz = 1;
    t.minSampleRate = 1e5;
    return t;
}

std::string asterixSummary(const AsterixTelemetry& t) {
    char b[200];
    if (t.state == 2) snprintf(b, sizeof b, "ASTERIX: %zu tracks from %zu sources, %llu records (%s)", t.tracks.size(), t.sources.size(), (unsigned long long)t.stats.records, t.input.c_str());
    else if (t.state == 1) snprintf(b, sizeof b, "ASTERIX: %s, no track yet", t.input.c_str());
    else snprintf(b, sizeof b, "ASTERIX: idle, no input started");
    return b;
}

namespace {
class SilentSynth : public ModeSynth {
public:
    explicit SilentSynth(double r) : r_(r) {}
    double sampleRate() const override { return r_; }
    void generate(cf32* out, size_t n) override { for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f); }
private:
    double r_;
};
}  // namespace
std::unique_ptr<ModeSynth> makeAsterixSynth(const SynthConfig&, double sampleRate) { return std::make_unique<SilentSynth>(sampleRate); }

}  // namespace dect2
