// IPv4/UDP reception (with fragments) and the low level signaling: an SLT compressed by Python's gzip is read, and one made by makeLls() too.
#include "dect2/atsc3_ip.h"
#include <cstdio>
#include <random>

using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static const uint8_t kSltGz[] = {31, 139, 8, 0, 0, 0, 0, 0, 2, 255, 149, 146, 95, 79, 194, 48, 20, 197, 223, 249, 20, 77, 31, 124, 146, 253, 233, 156, 40, 50, 8, 108, 49, 18, 145, 16, 7, 137, 111, 166, 172, 205, 168, 233, 90, 108, 11, 145, 111, 239, 101, 44, 49, 10, 33, 152, 62, 52, 189, 247, 220, 147, 115, 127, 105, 111, 240, 85, 73, 180, 229, 198, 10, 173, 18, 28, 122, 1, 70, 92, 21, 154, 9, 85, 38, 120, 49, 127, 108, 223, 225, 65, 191, 213, 203, 39, 115, 4, 74, 101, 19, 236, 104, 217, 165, 206, 22, 158, 54, 229, 53, 9, 194, 219, 238, 219, 203, 36, 47, 86, 188, 162, 214, 31, 206, 243, 52, 242, 51, 46, 5, 120, 238, 124, 24, 243, 193, 211, 199, 104, 105, 5, 3, 127, 18, 221, 160, 78, 7, 247, 91, 8, 245, 114, 110, 182, 162, 224, 200, 30, 238, 241, 190, 31, 4, 33, 70, 21, 253, 208, 38, 93, 81, 165, 184, 156, 234, 4, 199, 80, 18, 234, 87, 9, 84, 205, 88, 74, 29, 47, 181, 217, 29, 106, 43, 109, 92, 227, 59, 165, 21, 79, 240, 243, 112, 148, 182, 159, 50, 104, 73, 151, 111, 139, 156, 127, 78, 55, 85, 130, 97, 205, 82, 234, 37, 149, 141, 120, 156, 37, 120, 99, 84, 189, 88, 183, 113, 22, 12, 158, 156, 86, 239, 117, 170, 125, 100, 8, 61, 50, 154, 178, 130, 218, 218, 76, 148, 138, 74, 32, 5, 230, 118, 102, 180, 211, 133, 150, 135, 28, 210, 102, 220, 58, 161, 168, 3, 174, 227, 245, 144, 49, 195, 45, 192, 35, 209, 189, 71, 226, 216, 139, 189, 35, 213, 130, 173, 103, 144, 62, 193, 81, 16, 4, 117, 51, 215, 27, 3, 209, 126, 166, 195, 192, 11, 225, 196, 216, 175, 1, 250, 77, 248, 51, 52, 201, 69, 52, 201, 9, 154, 228, 20, 205, 87, 202, 132, 70, 87, 180, 90, 63, 160, 84, 255, 133, 26, 97, 180, 18, 140, 113, 248, 72, 206, 108, 248, 63, 144, 145, 139, 144, 145, 115, 200, 194, 35, 40, 189, 253, 247, 235, 183, 190, 1, 248, 199, 8, 78, 227, 2, 0, 0};

static void checkSlt(const Slt& s, const char* tag) {
    char m[80];
    snprintf(m, sizeof m, "%s: stream ids", tag);
    CHECK(s.bsid.size() == 2 && s.bsid[0] == 1234 && s.bsid[1] == 77, m);
    snprintf(m, sizeof m, "%s: two services", tag);
    CHECK(s.services.size() == 2, m);
    if (s.services.size() != 2) return;
    const auto& a = s.services[0];
    const auto& b = s.services[1];
    snprintf(m, sizeof m, "%s: first service", tag);
    CHECK(a.serviceId == 1001 && a.majorChannel == 5 && a.minorChannel == 1 && a.category == 1 && a.shortName == "KABC-HD" && a.slsProtocol == 1 &&
          ipToString(a.slsDstIp) == "239.255.5.1" && a.slsDstPort == 3000 && ipToString(a.slsSrcIp) == "10.1.1.5" && !a.hidden, m);
    snprintf(m, sizeof m, "%s: second service", tag);
    CHECK(b.serviceId == 1002 && b.minorChannel == 2 && b.shortName == "Radio & Co" && b.hidden && b.slsProtocol == 2 && b.slsDstPort == 3001 && b.seqNum == 3, m);
}

int main() {
    uint32_t ip;
    CHECK(ipFromString("239.255.5.1", ip) && ip == 0xEFFF0501u && ipToString(ip) == "239.255.5.1" && !ipFromString("1.2.3", ip) && !ipFromString("1.2.3.256", ip), "IP address text");
    CHECK(kLlsAddress == ((224u << 24) | (23u << 8) | 60u) && kLlsPort == 4937, "LLS address and port");

    // a real LLS packet: header (table 1, group 0, version 9) then the gzip file
    std::vector<uint8_t> lls = {1, 0, 0, 9};
    lls.insert(lls.end(), kSltGz, kSltGz + sizeof kSltGz);
    auto t = parseLls(lls.data(), lls.size());
    CHECK(t.ok && t.tableId == 1 && t.groupId == 0 && t.version == 9, "LLS header");
    checkSlt(t.slt, "gzip from Python");
    std::string xml(t.xml);
    auto mine = makeLls(1, 3, 200, xml);
    auto t2 = parseLls(mine.data(), mine.size());
    CHECK(t2.ok && t2.groupId == 3 && t2.version == 200, "makeLls header");
    checkSlt(t2.slt, "makeLls");
    CHECK(!parseLls(lls.data(), 3).ok, "short LLS");
    auto bad = lls; bad[30] ^= 0x40;
    CHECK(!parseLls(bad.data(), bad.size()).ok, "corrupted LLS");

    // through IP: whole packet, and the same fragmented at an MTU of 200 and delivered out of order
    IpReceiver rx;
    UdpDatagram d;
    auto whole = makeUdpPackets(0x0A010105u, kLlsAddress, 1234, kLlsPort, lls, 7);
    CHECK(whole.size() == 1 && rx.push(whole[0].data(), whole[0].size(), d) && d.dstIp == kLlsAddress && d.dstPort == kLlsPort && d.payload == lls && ipToString(d.srcIp) == "10.1.1.5", "UDP over IPv4");
    auto frags = makeUdpPackets(0x0A010105u, kLlsAddress, 1234, kLlsPort, lls, 8, 200);
    CHECK(frags.size() > 2, "fragmented");
    std::mt19937 rng(3);
    std::vector<size_t> order(frags.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    for (size_t i = order.size() - 1; i > 0; i--) std::swap(order[i], order[rng() % (i + 1)]);
    int got = 0;
    for (size_t i : order) got += rx.push(frags[i].data(), frags[i].size(), d);
    CHECK(got == 1 && d.payload == lls, "fragment reassembly out of order");
    auto tcp = whole[0]; tcp[9] = 6;
    CHECK(!rx.push(tcp.data(), tcp.size(), d) && !rx.push(tcp.data(), 10, d) && rx.dropped() >= 2, "non UDP and truncated packets");
    printf(fails ? "atsc3 lls: FAILED\n" : "atsc3 lls: ok\n");
    return fails ? 1 : 0;
}
