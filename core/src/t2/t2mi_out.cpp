#include "dect2/t2/t2mi_out.h"
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dect2 {
namespace t2mi {

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint32_t)p[i] << 24;
        for (int k = 0; k < 8; k++) c = c & 0x80000000u ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

std::vector<uint8_t> bbBytes(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> o((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) if (bits[i]) o[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    return o;
}

std::vector<uint8_t> bbPacket(uint8_t packetCount, uint8_t superframeIdx, uint8_t frameIdx, uint8_t plpId, bool intlStart, const std::vector<uint8_t>& bb) {
    const size_t payloadBytes = 3 + bb.size();
    const uint32_t payloadBits = (uint32_t)(3 + bb.size()) * 8;   // the BB frame is a whole number of bytes in every T2 FEC configuration
    std::vector<uint8_t> p;
    p.reserve(6 + payloadBytes + 4);
    p.push_back(0x00);                                  // packet_type: baseband frame
    p.push_back(packetCount);
    p.push_back((uint8_t)((superframeIdx & 0x0F) << 4)); // superframe_idx, rfu
    p.push_back(0);
    p.push_back((uint8_t)(payloadBits >> 8));
    p.push_back((uint8_t)payloadBits);
    p.push_back(frameIdx);
    p.push_back(plpId);
    p.push_back(intlStart ? 0x80 : 0x00);
    p.insert(p.end(), bb.begin(), bb.end());
    const uint32_t c = crc32(p.data(), p.size());
    for (int s = 24; s >= 0; s -= 8) p.push_back((uint8_t)(c >> s));
    return p;
}

void toTs(const std::vector<uint8_t>& pkt, int pid, uint8_t& cc, std::vector<uint8_t>& ts) {
    size_t off = 0;
    bool first = true;
    while (off < pkt.size()) {
        uint8_t t[188];
        memset(t, 0xFF, sizeof t);
        t[0] = 0x47;
        t[1] = (uint8_t)((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
        t[2] = (uint8_t)pid;
        t[3] = (uint8_t)(0x10 | (cc++ & 0x0F));
        int pos = 4;
        if (first) t[pos++] = 0;   // pointer_field: the T2-MI packet starts right here
        const size_t n = std::min<size_t>(188 - pos, pkt.size() - off);
        memcpy(t + pos, pkt.data() + off, n);
        off += n;
        first = false;
        ts.insert(ts.end(), t, t + 188);
    }
}

void Extract::feed(const uint8_t* t) {
    if (t[0] != 0x47 || (((t[1] & 0x1F) << 8) | t[2]) != pid_) return;
    if (!(t[3] & 0x10)) return;
    int pos = 4;
    if (t[3] & 0x20) pos += 1 + t[4];
    if (pos >= 188) return;
    if (t[1] & 0x40) {
        const int ptr = t[pos++];
        if (pos + ptr > 188) { synced_ = false; return; }
        if (synced_) buf_.insert(buf_.end(), t + pos, t + pos + ptr);   // the tail of the previous packet
        parse();
        buf_.clear();
        pos += ptr;
        synced_ = true;
    }
    if (!synced_) return;
    buf_.insert(buf_.end(), t + pos, t + 188);
    parse();
}

// takes whole T2-MI packets off the front of buf_ (anything after the last one is 0xFF padding or the start of the next, kept until it completes)
void Extract::parse() {
    for (;;) {
        if (buf_.size() < 6) return;
        const size_t plen = ((((size_t)buf_[4] << 8) | buf_[5]) + 7) / 8;
        const size_t total = 6 + plen + 4;
        if (buf_.size() < total) return;
        if (crc32(buf_.data(), total - 4) != (((uint32_t)buf_[total - 4] << 24) | ((uint32_t)buf_[total - 3] << 16) | ((uint32_t)buf_[total - 2] << 8) | buf_[total - 1])) crcBad++;
        else if (buf_[0] == 0x00 && plen >= 3) {
            counts.push_back(buf_[1]);
            frameIdx.push_back(buf_[6]);
            plpIds.push_back(buf_[7]);
            frames.emplace_back(buf_.begin() + 9, buf_.begin() + 6 + plen);
        }
        buf_.erase(buf_.begin(), buf_.begin() + total);
        if (!buf_.empty() && buf_[0] == 0xFF) { buf_.clear(); return; }
    }
}
}   // namespace t2mi

T2Export::T2Export() {}
T2Export::~T2Export() { close(); }

void T2Export::close() {
    flushUdp();
    if (f_) { fclose(f_); f_ = nullptr; }
    if (sock_ >= 0) {
#ifdef _WIN32
        closesocket((SOCKET)sock_);
#else
        ::close((int)sock_);
#endif
        sock_ = -1;
    }
    active_ = false;
}

bool T2Export::configure(const T2ExportConfig& c) {
    close();
    err_.clear();
    cfg_ = c;
    frames_ = bytes_ = 0;
    count_ = cc_ = sf_ = 0;
    lastIdx_ = -1;
    if (c.file.empty() && c.udpHost.empty()) return true;
    if (!c.file.empty()) {
        f_ = fopen(c.file.c_str(), "wb");
        if (!f_) { err_ = "cannot open " + c.file; return false; }
    }
    if (!c.udpHost.empty()) {
        in_addr a{};
        if (c.udpPort < 1 || c.udpPort > 65535 || inet_pton(AF_INET, c.udpHost.c_str(), &a) != 1) { err_ = "bad UDP address or port"; close(); return false; }
#ifdef _WIN32
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
        sock_ = (intptr_t)socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) { err_ = "no UDP socket"; close(); return false; }
        int yes = 1;
        setsockopt((int)sock_, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof yes);
        sockaddr_in sa{};
        sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)c.udpPort); sa.sin_addr = a;
        if (connect((int)sock_, (sockaddr*)&sa, sizeof sa) != 0) { err_ = "cannot reach the UDP address"; close(); return false; }
    }
    active_ = true;
    return true;
}

void T2Export::flushUdp() {
    if (sock_ < 0) { udpBuf_.clear(); return; }
    size_t off = 0;
    while (off < udpBuf_.size()) {   // 7 TS packets (1316 bytes) per datagram; the same size for raw frames
        const size_t n = std::min<size_t>(1316, udpBuf_.size() - off);
        send((int)sock_, (const char*)udpBuf_.data() + off, (int)n, 0);
        off += n;
    }
    udpBuf_.clear();
}

void T2Export::emit(const std::vector<uint8_t>& b) {
    if (f_) fwrite(b.data(), 1, b.size(), f_);
    if (sock_ >= 0) udpBuf_.insert(udpBuf_.end(), b.begin(), b.end());
    bytes_ += b.size();
}

void T2Export::onPlp(const PlpResult& r) {
    if (!active_) return;
    if (lastIdx_ >= 0 && r.t2Frame < lastIdx_) sf_++;   // FRAME_IDX restarted: next super-frame
    lastIdx_ = r.t2Frame;
    for (const BbFrame& f : r.frames) {
        if (f.bits.empty()) { count_++; continue; }   // a lost block: the gap in the packet counter shows it
        std::vector<uint8_t> bb = t2mi::bbBytes(f.bits);
        if (cfg_.format == T2ExportConfig::Bbf) emit(bb);
        else {
            std::vector<uint8_t> ts;
            t2mi::toTs(t2mi::bbPacket(count_, sf_, (uint8_t)r.t2Frame, (uint8_t)r.plpId, f.blockIndex == 0, bb), cfg_.pid, cc_, ts);
            emit(ts);
        }
        count_++;
        frames_++;
    }
    if (f_) fflush(f_);
    flushUdp();
}

}   // namespace dect2
