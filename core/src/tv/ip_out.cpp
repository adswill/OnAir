// IP output of the broadcast data modes (DVB-H MPE / MPE-FEC, DVB-C2 GSE): pcap file and UDP forward. See ip_out.h.
#include "dect2/tv/ip_out.h"
#include "dect2/dvbs/dvbs_ip.h"
#include "dect2/net/api_servers.h"
#include "../net/netcompat.h"
#include "../net/tcp_client.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace dect2 {

struct IpOut::Impl {
    mutable std::mutex mu;
    IpOutConfig cfg;
    dvbsip::PcapWriter pcap;
    dvbsip::Reassembler frag;
    sock_t s = kBadSock;
    sockaddr_in to{};
    IpOutStats st;
    std::string err;
    void closeSock() { if (s != kBadSock) { sockClose(s); s = kBadSock; } }
};

IpOut::IpOut() : p_(new Impl) {}
IpOut::~IpOut() { close(); }

void IpOut::close() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pcap.close();
    p_->closeSock();
    p_->cfg.pcap = p_->cfg.udp = false;
}

bool IpOut::configure(const IpOutConfig& c) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.err.clear();
    bool ok = true;
    const bool reopenPcap = c.pcap != m.cfg.pcap || c.pcapPath != m.cfg.pcapPath || (c.pcap && !m.pcap.isOpen());
    if (reopenPcap) {
        m.pcap.close();
        if (c.pcap) {
            if (c.pcapPath.empty() || !m.pcap.open(c.pcapPath)) { m.err = "cannot write the pcap file " + c.pcapPath; ok = false; }
        }
    }
    m.closeSock();
    if (c.udp) {
        if (!validUdpHost(c.udpHost) || c.udpPort < 0 || c.udpPort > 65535) {
            if (!m.err.empty()) m.err += "; ";
            m.err += "the UDP address must be a dotted IPv4 address and a port from 0 to 65535";
            ok = false;
        } else {
            netInit();
            m.s = socket(AF_INET, SOCK_DGRAM, 0);
            if (m.s == kBadSock) { if (!m.err.empty()) m.err += "; "; m.err += "cannot open a UDP socket"; ok = false; }
            else {
                int one = 1;
                setsockopt(m.s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
                sockSetNonBlocking(m.s, true);
                m.to = sockaddr_in{};
                m.to.sin_family = AF_INET;
                m.to.sin_port = htons((uint16_t)c.udpPort);
                inet_pton(AF_INET, c.udpHost.c_str(), &m.to.sin_addr);
            }
        }
    }
    m.cfg = c;
    m.cfg.pcap = c.pcap && m.pcap.isOpen();
    m.cfg.udp = c.udp && m.s != kBadSock;
    return ok;
}

IpOutConfig IpOut::config() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg; }
bool IpOut::active() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->cfg.pcap || p_->cfg.udp; }
IpOutStats IpOut::stats() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }
std::string IpOut::error() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->err; }

void IpOut::packet(const uint8_t* ip, size_t n, double t) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.cfg.pcap && !m.cfg.udp) return;
    m.st.packets++;
    dvbsip::IpInfo info;
    if (!ip || !dvbsip::parseIp(ip, n, info)) { m.st.invalid++; return; }
    if (t <= 0) t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (m.cfg.pcap) { m.pcap.write(t, ip, info.len); m.st.written++; m.st.bytesWritten += info.len; }
    if (!m.cfg.udp) return;
    auto send = [&](const uint8_t* d, size_t len, uint16_t port) {
        sockaddr_in to = m.to;
        if (m.cfg.udpPort == 0) to.sin_port = htons(port);
        if (sockSendTo(m.s, d, len, to) == (long long)len) m.st.forwarded++; else m.st.sendErrors++;
    };
    if (m.cfg.wholePackets) { send(ip, info.len, 0); return; }
    // the payload of whole UDP datagrams (IPv4 fragments put back together first)
    std::vector<uint8_t> whole;
    if (!m.frag.push(ip, info, whole)) return;
    dvbsip::IpInfo wi;
    if (!dvbsip::parseIp(whole.data(), whole.size(), wi)) return;
    if (wi.protocol != dvbsip::kProtoUdp) { m.st.notUdp++; return; }
    size_t hl = 0;
    if (!wi.v6) hl = (size_t)(whole[0] & 0x0F) * 4;
    else if (whole.size() > 40 && whole[6] == dvbsip::kProtoUdp) hl = 40;   // UDP straight after the fixed header (no extension headers)
    else { m.st.notUdp++; return; }
    if (wi.len < hl + 8) { m.st.invalid++; return; }
    const uint8_t* u = whole.data() + hl;
    const size_t ulen = std::min((size_t)(u[4] << 8 | u[5]), wi.len - hl);
    if (ulen < 8) { m.st.invalid++; return; }
    send(u + 8, ulen - 8, (uint16_t)(u[2] << 8 | u[3]));
}

} // namespace dect2
