// Wireshark output: every channel written through the hub and parsed back (pcapng block structure, link type, the packet's own headers), the extcap program's
// answers and a live capture through it, the local-only rule, encrypted bytes unchanged, and frames from real decoders fed by their generators.
// tshark, when it is on the PATH, reads the files too; without it the parser in this file is the check. All signals here are simulated.
#include "dect2/wirecap/wire.h"
#include "dect2/ble/ble_gen.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/zigbee/zigbee_gen.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zwave/zwave_gen.h"
#include "dect2/zwave/zwave_proto.h"
#include "dect2/zwave/zwave_rx.h"
#include "../../core/src/net/netcompat.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace dect2;
using namespace dect2::wire;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }
static uint32_t be16at(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

struct Parsed { bool ok = false; std::string err, ifName, ifDesc, app; uint32_t linktype = 0; std::vector<Bytes> packets; std::vector<uint64_t> ts; };

static Bytes readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); return Bytes((std::istreambuf_iterator<char>(f)), {}); }

// a strict reader: every block's two lengths, 32-bit padding, options, one section and one interface
static Parsed parsePcapng(const Bytes& f) {
    Parsed r;
    size_t pos = 0;
    int nShb = 0, nIdb = 0;
    while (pos < f.size()) {
        if (pos + 12 > f.size()) { r.err = "cut block header"; return r; }
        const uint32_t type = rd32(&f[pos]), len = rd32(&f[pos + 4]);
        if (len < 12 || len % 4 || pos + len > f.size()) { r.err = "bad block length"; return r; }
        if (rd32(&f[pos + len - 4]) != len) { r.err = "trailing length differs"; return r; }
        const uint8_t* b = &f[pos + 8];
        const size_t blen = len - 12;
        auto options = [&](size_t off, std::map<int, std::string>& out) {
            while (off + 4 <= blen) {
                const uint32_t code = rd16(b + off), ol = rd16(b + off + 2);
                if (code == 0) return true;
                if (off + 4 + ol > blen) return false;
                out[(int)code] = std::string((const char*)b + off + 4, ol);
                off += 4 + ((ol + 3) & ~3u);
            }
            return false;
        };
        if (type == 0x0A0D0D0Au) {
            nShb++;
            if (pos != 0 || rd32(b) != 0x1A2B3C4Du || rd16(b + 4) != 1 || rd16(b + 6) != 0) { r.err = "bad section header"; return r; }
            std::map<int, std::string> o;
            if (!options(16, o)) { r.err = "bad section options"; return r; }
            r.app = o[4];
        } else if (type == 1) {
            nIdb++;
            r.linktype = rd16(b);
            std::map<int, std::string> o;
            if (!options(8, o)) { r.err = "bad interface options"; return r; }
            r.ifName = o[2]; r.ifDesc = o[3];
        } else if (type == 6) {
            if (nIdb != 1 || rd32(b) != 0) { r.err = "packet before its interface"; return r; }
            const uint32_t cap = rd32(b + 12), orig = rd32(b + 16);
            if (cap != orig || 20 + cap > blen || blen - 20 - cap > 3) { r.err = "bad packet lengths"; return r; }
            r.ts.push_back((uint64_t)rd32(b + 4) << 32 | rd32(b + 8));
            r.packets.emplace_back(b + 20, b + 20 + cap);
        } else { r.err = "unexpected block type"; return r; }
        pos += len;
    }
    if (nShb != 1 || nIdb != 1) { r.err = "want one section and one interface"; return r; }
    r.ok = true;
    return r;
}

static bool ipv4HeaderOk(const Bytes& p, uint16_t port, size_t payload) {
    if (p.size() != 28 + payload || p[0] != 0x45 || p[9] != 17) return false;
    if (be16at(&p[2]) != p.size() || be16at(&p[24]) != payload + 8 || be16at(&p[22]) != port) return false;
    uint32_t s = 0;
    for (int i = 0; i < 20; i += 2) s += be16at(&p[(size_t)i]);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return s == 0xFFFF;
}

// CCITT with bits LSB first, as IEEE 802.15.4 and Wireshark's wpan dissector compute it
static uint16_t fcs154(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
    return c;
}

// the check Wireshark's btle dissector makes: an LFSR over the PDU (bit 0 of each byte first), compared with the three CRC bytes read big endian and reversed per byte
static bool bleCrcAsWireshark(const uint8_t* pdu, size_t n, const uint8_t* crc) {
    uint32_t st = 0x555555;
    for (size_t i = 0; i < n; i++) for (int b = 0; b < 8; b++) {
        const uint32_t fb = ((st >> 23) & 1) ^ ((pdu[i] >> b) & 1);
        st = (st << 1) & 0xFFFFFF;
        if (fb) st ^= 0x00065B;
    }
    auto rev = [](uint8_t v) { uint8_t o = 0; for (int i = 0; i < 8; i++) if (v & (1 << i)) o |= (uint8_t)(0x80 >> i); return o; };
    const uint32_t got = (uint32_t)rev(crc[0]) << 16 | (uint32_t)rev(crc[1]) << 8 | rev(crc[2]);
    return got == st;
}

static std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char b[512];
    while (fgets(b, sizeof b, p)) out += b;
    pclose(p);
    return out;
}

static bool tsharkHas(const std::string& file, const char* proto) {
    return run("tshark -r '" + file + "' -T fields -e frame.protocols 2>/dev/null").find(proto) != std::string::npos;
}

static std::string gDir;

// one packet through one channel, written to a file and read back
static Parsed roundTrip(Chan c, const Bytes& in, const Bytes* gsmtapOut = nullptr) {
    (void)gsmtapOut;
    Hub& h = Hub::get();
    h.setEnabled(c, true);
    h.publish(c, in);
    h.publish(c, in);
    CHECK(h.waitIdle(5000), "%s: hub did not finish", kChans[c].id);
    const std::string file = h.stats(c).file;
    h.setEnabled(c, false);
    CHECK(!file.empty(), "%s: no file", kChans[c].id);
    Parsed p = parsePcapng(readFile(file));
    CHECK(p.ok, "%s: %s", kChans[c].id, p.err.c_str());
    CHECK(p.linktype == kChans[c].linktype, "%s: link type %u", kChans[c].id, p.linktype);
    CHECK(p.packets.size() == 2, "%s: %zu packets", kChans[c].id, p.packets.size());
    CHECK(p.ifName == std::string("onair_") + kChans[c].id, "%s: interface name %s", kChans[c].id, p.ifName.c_str());
    CHECK(p.app == "OnAir", "application");
    if (!p.ts.empty()) { const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); CHECK(llabs((int64_t)p.ts[0] - now) < 60000000, "%s: timestamp", kChans[c].id); }
    if (std::getenv("PATH") && !run("command -v tshark").empty() && kChans[c].dissector[0] != 'o') {
        const std::string want = std::string(kChans[c].dissector).substr(0, std::string(kChans[c].dissector).find(','));
        CHECK(tsharkHas(file, want.c_str()), "%s: tshark did not see %s", kChans[c].id, want.c_str());
    }
    return p;
}

int main(int argc, char** argv) {
    netInit();
    gDir = (fs::temp_directory_path() / ("onair_wirecap_" + std::to_string((long long)std::time(nullptr)))).string();
    fs::create_directories(gDir);
    Hub& h = Hub::get();
    Config cfg;
    cfg.dir = gDir; cfg.streamPort = 0; cfg.gsmtapHost = "127.0.0.1";
    // a local UDP socket to catch GSMTAP
    sock_t us = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in ua{}; ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK); ua.sin_port = 0;
    CHECK(bind(us, (sockaddr*)&ua, sizeof ua) == 0, "udp bind");
    socklen_t al = sizeof ua; getsockname(us, (sockaddr*)&ua, &al);
    cfg.gsmtapPort = ntohs(ua.sin_port);
    h.configure(cfg);

    // off by default: nothing is listening, nothing is written
    for (int i = 0; i < kChanCount; i++) CHECK(!h.enabled((Chan)i), "channel %d on by default", i);
    CHECK(h.boundPort() == 0, "port open with every switch off");
    h.publish(Zigbee, Bytes{1, 2, 3});
    CHECK(h.stats(Zigbee).packets == 0 && fs::is_empty(gDir), "wrote while off");

    std::printf("-- %s\n", "Wi-Fi: radiotap"); std::fflush(stdout);
    // Wi-Fi: radiotap fields where the alignment rules put them, the frame untouched (a protected data frame: stays as it is)
    {
        Bytes mac = {0x08, 0x41, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 1, 2, 3, 4, 5, 6, 0x10, 0x20, 0xDE, 0xAD, 0xBE, 0xEF};
        const Bytes in = radiotap80211(mac.data(), mac.size(), 2437, -52.4f);
        const Parsed p = roundTrip(Wifi, in);
        if (p.packets.size() == 2) {
            const Bytes& b = p.packets[0];
            CHECK(rd16(&b[2]) == 15 && rd32(&b[4]) == 0x2A, "radiotap header: len %u present %x", rd16(&b[2]), rd32(&b[4]));
            CHECK(b[8] == 0 && rd16(&b[10]) == 2437 && rd16(&b[12]) == 0x0080 && (int8_t)b[14] == -52, "radiotap fields");
            CHECK(Bytes(b.begin() + 15, b.end()) == mac, "802.11 frame changed");
        }
        const Bytes noMeta = radiotap80211(mac.data(), mac.size(), 0, 0);
        CHECK(rd16(&noMeta[2]) == 9 && rd32(&noMeta[4]) == 2, "radiotap without channel and signal");
    }
    // Zigbee with the network security bit set: the same bytes come back (nothing decrypted or altered), FCS valid
    {
        Bytes f = {0x41, 0x88, 0x05, 0x34, 0x12, 0xFF, 0xFF, 0x01, 0x00, 0x48, 0x02, 0xFF, 0xFF, 0x01, 0x00, 0x1E, 0x05, 0x28, 0xA1, 0xB2, 0xC3, 0xD4, 0x11, 0x22, 0x33};
        const uint16_t c = fcs154(f.data(), f.size());
        f.push_back((uint8_t)c); f.push_back((uint8_t)(c >> 8));
        const Parsed p = roundTrip(Zigbee, f);
        if (p.packets.size() == 2) CHECK(p.packets[0] == f, "encrypted Zigbee frame changed");
        const Parsed q = roundTrip(Thread, f);
        if (q.packets.size() == 2) CHECK(q.packets[0] == f, "Thread frame changed");
    }
    // LoRaTap version 0: the fields where Wireshark reads them, sync word 0x34 selects the lorawan dissector
    {
        const Bytes phy = {0x40, 0x11, 0x22, 0x33, 0x44, 0x80, 0x01, 0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        const Parsed p = roundTrip(LoRaWan, loraTap(phy.data(), phy.size(), 868100000.0, 125000, 7, -7.25f));
        if (p.packets.size() == 2) {
            const Bytes& b = p.packets[0];
            CHECK(b[0] == 0 && be16at(&b[2]) == 15, "loratap version and length");
            CHECK(((uint32_t)be16at(&b[4]) << 16 | be16at(&b[6])) == 868100000u, "loratap frequency");
            CHECK(b[8] == 1 && b[9] == 7, "loratap bandwidth %d, sf %d", b[8], b[9]);
            CHECK(b[10] == 255 && (int8_t)b[13] == -29 && b[14] == 0x34, "loratap rssi, snr %d, sync", (int8_t)b[13]);
            CHECK(Bytes(b.begin() + 15, b.end()) == phy, "LoRaWAN payload changed");
        }
    }
    // Direct: the bytes as given
    for (auto [c, in] : std::vector<std::pair<Chan, Bytes>>{
             {DvbIp, {0x45, 0, 0, 20, 0, 0, 0x40, 0, 64, 17, 0, 0, 10, 0, 0, 1, 10, 0, 0, 2}},
             {WiSun, {0x41, 0xD8, 1, 0xCD, 0xAB}},
             {Aprs, {0x82, 0xA0, 0x88, 0x98, 0x98, 0x40, 0xE0, 0x9A, 0x82, 0x9C, 0x84, 0x88, 0x40, 0x61, 0x03, 0xF0, '>', 'h', 'i'}}})
    {
        const Parsed p = roundTrip(c, in);
        if (p.packets.size() == 2) CHECK(p.packets[0] == in, "%s changed", kChans[c].id);
    }
    // the private link types: a Mode S frame; Z-Wave with its rate byte
    {
        const Bytes mode = {0x8D, 0x4C, 0xA2, 0x7B, 0x58, 0x37, 0x80, 0xE4, 0x7A, 0x1C, 0xDE, 0x11, 0x22, 0x33};
        const Parsed p = roundTrip(AdsB, mode);
        if (p.packets.size() == 2) CHECK(p.packets[0] == mode, "Mode S changed");
        const Bytes mp = {0xC0, 0xA8, 0x01, 0x02, 0x03, 0x41, 0x01, 0x0B, 0x05, 0x98, 0x81, 0x0, 0x0};
        const Parsed q = roundTrip(Zwave, zwaveUser(mp.data(), mp.size(), 3));
        if (q.packets.size() == 2) { CHECK(q.packets[0][0] == 2, "zwave rate byte"); CHECK(Bytes(q.packets[0].begin() + 1, q.packets[0].end()) == mp, "S2 payload changed"); }
        CHECK(zwaveUser(mp.data(), mp.size(), 1)[0] == 0 && zwaveUser(mp.data(), mp.size(), 2)[0] == 1, "zwave rate codes");
    }
    // UDP wrapped: AIS to the NMEA port, ASTERIX to its dissector's port
    {
        const std::string s = "!AIVDM,1,1,,A,13u?etPv2;0n:dDPwUM1U1Cb069D,0*24\r\n";
        const Parsed p = roundTrip(Ais, Bytes(s.begin(), s.end()));
        if (p.packets.size() == 2) { CHECK(ipv4HeaderOk(p.packets[0], kNmeaPort, s.size()), "AIS IPv4/UDP"); CHECK(!memcmp(p.packets[0].data() + 28, s.data(), s.size()), "AIS sentence changed"); }
        const Bytes ast = {21, 0, 6, 0x80, 1, 2};
        const Parsed q = roundTrip(Asterix, ast);
        if (q.packets.size() == 2) CHECK(ipv4HeaderOk(q.packets[0], kAsterixPort, ast.size()), "ASTERIX IPv4/UDP");
    }
    // GSMTAP: in the file inside UDP to 4729; over UDP as it is; 16-byte header at the offsets of the dissector
    {
        const Bytes payload(23, 0x2B);
        const Bytes g = makeGsmtap(gsmtap::kTypeUm, gsmtap::kChanBcch, payload.data(), payload.size(), 975, 2, 123456, -71, 19);
        CHECK(g.size() == 39 && g[0] == 1 && g[1] == 4 && g[2] == 1 && g[3] == 2 && be16at(&g[4]) == 975 && (int8_t)g[6] == -71 && g[7] == 19, "gsmtap header");
        CHECK(((uint32_t)be16at(&g[8]) << 16 | be16at(&g[10])) == 123456u && g[12] == 1, "gsmtap frame number / sub type");
        const Parsed p = roundTrip(GsmR, g);
        if (p.packets.size() == 2) { CHECK(ipv4HeaderOk(p.packets[0], kGsmtapPort, g.size()), "GSMTAP IPv4/UDP"); CHECK(!memcmp(p.packets[0].data() + 28, g.data(), g.size()), "GSMTAP payload changed"); }
        uint8_t buf[256];
#ifdef _WIN32
        DWORD tv = 2000; setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
#else
        timeval tv{2, 0}; setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        const long long r = sockRecv(us, buf, sizeof buf);
        CHECK(r == (long long)g.size() && !memcmp(buf, g.data(), g.size()), "GSMTAP over UDP: %lld bytes", r);
        // TETRA 124 type-1 bits pack to 16 bytes, first bit on top, as osmo-tetra's GSMTAP does
        std::vector<uint8_t> bits(124, 0); bits[0] = 1; bits[8] = 1; bits[123] = 1;
        const Bytes pk = packBitsMsbFirst(bits.data(), bits.size());
        CHECK(pk.size() == 16 && pk[0] == 0x80 && pk[1] == 0x80 && pk[15] == 0x10, "bit packing");
        const Bytes t = makeGsmtap(gsmtap::kTypeTetraI1, gsmtap::kTetraBnch, pk.data(), pk.size());
        CHECK(t[2] == 5 && t[12] == 6, "TETRA type and channel");
        const Parsed q = roundTrip(Tetra, t);
        CHECK(q.packets.size() == 2, "TETRA packets");
        const Bytes l = makeGsmtap(gsmtap::kTypeLteRrc, gsmtap::kLteBcchDlSch, pk.data(), pk.size());
        CHECK(l[2] == 0x0d && l[12] == 5, "LTE type and channel");
        CHECK(roundTrip(Lte, l).packets.size() == 2, "LTE packets");
    }

    // the classic pcap the extcap stream carries
    {
        const Bytes hdr = pcapFileHeader(kLinkBleLl);
        CHECK(hdr.size() == 24 && rd32(&hdr[0]) == 0xA1B2C3D4u && rd16(&hdr[4]) == 2 && rd16(&hdr[6]) == 4 && rd32(&hdr[20]) == 251, "pcap header");
        Bytes r; const uint8_t d[3] = {7, 8, 9};
        appendPcapRecord(r, 1700000000123456ll, d, 3);
        CHECK(r.size() == 19 && rd32(&r[0]) == 1700000000u && rd32(&r[4]) == 123456u && rd32(&r[8]) == 3 && rd32(&r[12]) == 3, "pcap record");
    }

    std::printf("-- %s\n", "real decoders"); std::fflush(stdout);
    // real decoders: a generator's frames through the receiver and into a file
    {
        ZigbeeGenConfig c; c.loop = false; c.snrDb = 30; c.cfoHz = 12000;
        zigbee::MacBuild b; b.seq = 9; b.destShort = 0x0001; b.srcShort = 0x7E11; b.payload = {0x48, 0x02, 0x01, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};
        ZigbeeGenFrame fr; fr.psdu = zigbee::buildMac(b);
        c.frames = {fr, fr, fr};
        h.setEnabled(Zigbee, true);
        const std::string before = h.stats(Zigbee).file;
        {
            ZigbeeGenerator g(c); ZigbeeReceiver r; r.configure(c.rate);
            std::vector<cf32> buf(1 << 13);
            for (int i = 0; i < 100; i++) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        }
        CHECK(h.waitIdle(5000), "zigbee idle");
        const std::string file = h.stats(Zigbee).file;
        h.setEnabled(Zigbee, false);
        CHECK(file != before, "zigbee: no new file");
        const Parsed p = parsePcapng(readFile(file));
        CHECK(p.ok && !p.packets.empty(), "zigbee decoder: %zu packets (%s)", p.packets.size(), p.err.c_str());
        for (const Bytes& k : p.packets) CHECK(k == fr.psdu, "zigbee decoder: frame differs from what was sent");
        std::printf("Zigbee: %zu of 3 sent frames in the file\n", p.packets.size());
    }
    {
        BleGenConfig c; c.loop = false; c.snrDb = 30;
        BleGenPacket pk; pk.pdu = ble::makePdu(ble::AdvNonconnInd, false, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 2, 1, 6}); pk.channel = 38;
        c.pkts.assign(2, pk);
        h.setEnabled(Ble, true);
        const std::string before = h.stats(Ble).file;
        {
            BleGenerator g(c); BleReceiver r; r.configure(c.rate);
            std::vector<cf32> buf(1 << 13);
            for (int i = 0; i < 60; i++) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        }
        CHECK(h.waitIdle(5000), "ble idle");
        const std::string file = h.stats(Ble).file;
        h.setEnabled(Ble, false);
        CHECK(file != before, "ble: no new file");
        const Parsed p = parsePcapng(readFile(file));
        CHECK(p.ok && !p.packets.empty(), "ble decoder: %zu packets (%s)", p.packets.size(), p.err.c_str());
        for (const Bytes& k : p.packets) {
            CHECK(k.size() == 4 + pk.pdu.size() + 3 && rd32(&k[0]) == 0x8E89BED6u, "ble access address / length");
            CHECK(Bytes(k.begin() + 4, k.end() - 3) == pk.pdu, "ble PDU differs from what was sent");
            CHECK(bleCrcAsWireshark(k.data() + 4, pk.pdu.size(), k.data() + 4 + pk.pdu.size()), "ble CRC fails Wireshark's check");
        }
        std::printf("BLE: %zu of 2 sent packets in the file, CRC as Wireshark checks it\n", p.packets.size());
    }

    {
        ZwGenConfig c; c.rate = 2e6; c.centerMhz = 869.13; c.snrDb = 30; c.loop = false; c.frames = zwaveTestSchedule("EU");
        h.setEnabled(Zwave, true);
        const std::string before = h.stats(Zwave).file;
        {
            ZwaveGenerator g(c); ZwaveReceiver r; r.configure(2e6); r.setCenterHz(869.13e6); r.setDialHz(869.13e6); r.setRegion("EU");
            std::vector<cf32> buf(8192);
            for (int i = 0; i < 2000; i++) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        }
        CHECK(h.waitIdle(5000), "zwave idle");
        const std::string file = h.stats(Zwave).file;
        h.setEnabled(Zwave, false);
        CHECK(file != before, "zwave: no new file");
        const Parsed p = parsePcapng(readFile(file));
        CHECK(p.ok && !p.packets.empty(), "zwave decoder: %zu packets (%s)", p.packets.size(), p.err.c_str());
        size_t matched = 0;
        for (const Bytes& k : p.packets) for (const ZwGenFrame& f : c.frames)
            if (k.size() == f.mpdu.size() + 1 && !memcmp(k.data() + 1, f.mpdu.data(), f.mpdu.size())) { matched++; CHECK(k[0] == f.rate - 1, "zwave rate byte %d for rate %d", k[0], f.rate); break; }
        CHECK(matched == p.packets.size(), "zwave: %zu of %zu frames are what was sent", matched, p.packets.size());
        std::printf("Z-Wave: %zu frames in the file, all as sent (%zu scheduled)\n", p.packets.size(), c.frames.size());
    }

    std::printf("-- %s\n", "the extcap program"); std::fflush(stdout);
    // the extcap program
#ifndef _WIN32
    if (argc > 1) {
        const std::string ex = argv[1];
        const std::string list = run("'" + ex + "' --extcap-interfaces");
        CHECK(list.find("extcap {version=") == 0, "extcap banner: %s", list.c_str());
        for (const ChanInfo& c : kChans) CHECK(list.find(std::string("interface {value=onair_") + c.id + "}") != std::string::npos, "extcap interface %s", c.id);
        const std::string dl = run("'" + ex + "' --extcap-interface onair_lorawan --extcap-dlts");
        CHECK(dl.find("dlt {number=270}{name=LORATAP}") == 0, "extcap dlt: %s", dl.c_str());
        CHECK(run("'" + ex + "' --extcap-interface onair_zigbee --extcap-config").find("{call=--port}") != std::string::npos, "extcap config");
        // a live capture: the program connects, Wireshark's pipe receives a pcap stream; the socket is on loopback only
        h.setEnabled(Zigbee, true);
        for (int i = 0; i < 200 && h.boundPort() == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const int port = h.boundPort();
        CHECK(port > 0, "not listening");
        sockaddr_in sa{}; socklen_t sl = sizeof sa;
        {
            // from outside loopback it must not answer: the socket's own address says so
            sock_t s = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons((uint16_t)port);
            CHECK(connect(s, (sockaddr*)&to, sizeof to) == 0, "connect");
            getpeername(s, (sockaddr*)&sa, &sl);
            CHECK(sa.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "not loopback");
            const char wrong[] = "ONAIR-WIRE 1 ble\n";   // a channel whose switch is off
            sockSend(s, wrong, sizeof wrong - 1);
            char rep[8] = {};
            const long long r = sockRecv(s, rep, sizeof rep);
            CHECK(r == 4 && !memcmp(rep, "OFF\n", 4), "switch off must be refused: %lld", r);
            sockClose(s);
        }
        const std::string fifo = gDir + "/fifo";
        CHECK(mkfifo(fifo.c_str(), 0600) == 0, "mkfifo");
        const std::string portText = std::to_string(port);   // (nothing is allocated between fork and exec)
        std::printf("live capture through %s on port %d\n", ex.c_str(), port); std::fflush(stdout);
        const pid_t kid = fork();
        if (kid == 0) { execl(ex.c_str(), ex.c_str(), "--capture", "--extcap-interface", "onair_zigbee", "--fifo", fifo.c_str(), "--port", portText.c_str(), (char*)nullptr); _exit(127); }
        const int rfd = open(fifo.c_str(), O_RDONLY | O_NONBLOCK);   // never blocks: reads give 0 until the program opens its end
        CHECK(rfd >= 0, "open fifo");
        Bytes got;
        uint8_t tmp[4096];
        const Bytes pkt = {0x03, 0x08, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x11, 0x22};
        bool sent = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < until && got.size() < 24 + 3 * (16 + pkt.size())) {
            if (h.stats(Zigbee).clients > 0 && !sent) { for (int k = 0; k < 3; k++) h.publish(Zigbee, pkt); sent = true; }
            fd_set s; FD_ZERO(&s); FD_SET(rfd, &s); timeval t{0, 20000};
            if (select(rfd + 1, &s, nullptr, nullptr, &t) > 0) { const ssize_t n = read(rfd, tmp, sizeof tmp); if (n > 0) got.insert(got.end(), tmp, tmp + n); else std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        }
        CHECK(sent, "extcap never attached");
        CHECK(got.size() == 24 + 3 * (16 + pkt.size()), "live capture: %zu bytes", got.size());
        if (got.size() >= 24 + 16 + pkt.size()) {
            CHECK(rd32(&got[0]) == 0xA1B2C3D4u && rd32(&got[20]) == 195, "live capture header");
            CHECK(rd32(&got[24 + 8]) == pkt.size() && !memcmp(&got[24 + 16], pkt.data(), pkt.size()), "live capture packet");
        }
        // the switch goes off: the extcap program keeps waiting (no data), then is stopped as Wireshark stops it
        h.setEnabled(Zigbee, false);
        for (int i = 0; i < 300 && h.boundPort() != 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(h.boundPort() == 0, "port still open with every switch off");
        kill(kid, SIGTERM);
        close(rfd);
        int st = 0;
        bool exited = false;
        for (int i = 0; i < 100 && !exited; i++) { exited = waitpid(kid, &st, WNOHANG) == kid; if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        CHECK(exited, "the extcap program did not stop on SIGTERM");
        if (!exited) { kill(kid, SIGKILL); waitpid(kid, &st, 0); }
    }
#else
    (void)argc; (void)argv;
#endif

    std::printf("-- %s\n", "the one-click install"); std::fflush(stdout);
    // the one-click install into an empty home folder
    {
        const fs::path fake = fs::path(gDir) / "fake-extcap";
        { std::ofstream(fake) << "x"; }
        const std::string home = gDir + "/home";
        const InstallResult r = installIntoWireshark(fake.string(), home);
        CHECK(r.ok, "install: %s", r.error.c_str());
#ifndef _WIN32
        CHECK(r.extcapPath == home + "/.local/lib/wireshark/extcap/fake-extcap" && r.luaPath == home + "/.local/lib/wireshark/plugins/onair.lua", "install paths: %s %s", r.extcapPath.c_str(), r.luaPath.c_str());
        CHECK(fs::status(r.extcapPath).permissions() != fs::perms::none && (fs::status(r.extcapPath).permissions() & fs::perms::owner_exec) != fs::perms::none, "not executable");
#endif
        CHECK(readFile(r.luaPath).size() == strlen(luaDissectors()), "lua file");
        CHECK(!installIntoWireshark(gDir + "/nothing-here", home).ok, "missing program");
    }

    h.shutdown();
    sockClose(us);
    std::error_code ec;
    fs::remove_all(gDir, ec);
    if (run("command -v tshark").empty()) std::printf("tshark is not installed: checked by this file's own parser only\n");
    std::printf(fails ? "FAILED: %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
