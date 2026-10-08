// Inmarsat-C packets: bulletin board and EGC parsing, multi-part and multi-frame assembly, ITA2 text, the message list limits.
// The byte layouts are those of the open decoder "inmarsatc" (inmarsatc_parser.cpp); no recorded frame was available, so the
// packets here are built from the documented field positions (see inmc_pkt.h for what is not confirmed).
#include "dect2/inmc_pkt.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
using namespace dect2::inmc;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void put(uint8_t* f, int& pos, const std::vector<uint8_t>& p) { memcpy(f + pos, p.data(), p.size()); pos += (int)p.size(); }

static std::vector<EgcPacket> split(uint8_t svc, uint16_t id, const std::string& text, int chunk, int prio = 1) {
    std::vector<EgcPacket> v;
    const int n = (int)((text.size() + chunk - 1) / chunk);
    for (int i = 0; i < n; i++) {
        EgcPacket p;
        p.desc = i % 2 ? 0xB2 : 0xB1; p.service = svc; p.continuation = i + 1 < n; p.priority = prio; p.msgId = id; p.packetNo = i + 1;
        p.address.assign((size_t)addressLength(svc), 0x11);
        p.payload.assign(text.begin() + i * chunk, text.begin() + std::min<size_t>(text.size(), (size_t)(i + 1) * chunk));
        v.push_back(p);
    }
    return v;
}

int main() {
    // bulletin board: field positions as in the decoder; a frame number of 1000 is 2 h 24 min
    {
        BulletinBoard b;
        b.frameNo = 1000; b.sat = 3; b.les = 44; b.channelType = 1; b.signallingChannel = 5; b.count = 6; b.services = 0xE001; b.randomInterval = 9;
        const auto p = buildBulletinBoard(b);
        CHECK(p.size() == 14 && p[0] == 0x7D && p[2] == 0x03 && p[3] == 0xE8, "bulletin board bytes");
        CHECK(p[7] == ((3 << 6) | 44) && (p[6] >> 5) == 1, "satellite, station and channel type bytes");
        uint8_t f[kFrameBytes] = {};
        int pos = 0;
        put(f, pos, p);
        FrameParser fp;
        CHECK(fp.parseFrame(f, 1700000000), "bulletin board first in the frame");
        InmcTelemetry t;
        fp.fill(t);
        CHECK(t.ncs.valid && t.ncs.frameNo == 1000 && t.ncs.sat == 3 && t.ncs.lesId == 44 && t.ncs.channelType == 1, "ncs fields");
        CHECK(t.ncs.region.find("IOR") != std::string::npos && t.ncs.lesName == "NCS" && t.ncs.channelTypeName == "NCS", "names: %s / %s", t.ncs.region.c_str(), t.ncs.lesName.c_str());
        CHECK(t.ncs.signallingChannel == 5 && t.ncs.count == 6 && t.ncs.randomInterval == 9 && t.ncs.services == 0xE001, "ncs numbers");
        CHECK(t.ncs.frameTime == "2:24:00.0", "frame time %s", t.ncs.frameTime.c_str());
        CHECK(t.ncs.servicesText.find("SafetyNET") != std::string::npos && t.ncs.statusText.find("operational") != std::string::npos, "services text: %s", t.ncs.servicesText.c_str());
        CHECK(t.pktOk[0x7D] == 1 && t.packetsOk == 1 && t.packetsBad == 0, "counts");
        // a flipped byte: the packet is counted bad and the board is not taken
        f[5] ^= 0x10;
        FrameParser fp2;
        CHECK(!fp2.parseFrame(f, 1700000000), "a corrupt bulletin board must not count");
        fp2.fill(t);
        CHECK(!t.ncs.valid && t.pktBad[0x7D] == 1, "corrupt packet counted: valid %d bad %u", t.ncs.valid, t.pktBad[0x7D]);
    }
    // signalling channel
    {
        uint8_t slots[28] = {};
        const auto p = buildSignalling(0xE0, 1626.5 + 0.0025 * 120, slots);
        uint8_t f[kFrameBytes] = {};
        int pos = 0;
        put(f, pos, buildBulletinBoard(BulletinBoard()));
        put(f, pos, p);
        FrameParser fp;
        fp.parseFrame(f, 1);
        InmcTelemetry t;
        fp.fill(t);
        CHECK(std::fabs(t.ncs.signallingUplinkMhz - (1626.5 + 0.3)) < 1e-6, "uplink %.4f MHz", t.ncs.signallingUplinkMhz);
    }
    // EGC message of four packets over two frames, parts arriving as 0xB1 and 0xB2; then the same message again (a repeat)
    {
        const std::string text = "NAVAREA IX 100/26\nSTRAIT OF HORMUZ. LIGHT UNLIT.\nCANCEL THIS MESSAGE 1 WEEK.";
        const auto pk = split(0x31, 4242, text, 25);
        CHECK(pk.size() == 4, "%zu packets", pk.size());
        FrameParser fp;
        for (int pass = 0; pass < 2; pass++) {
            uint8_t f1[kFrameBytes] = {}, f2[kFrameBytes] = {};
            int a = 0, b = 0;
            put(f1, a, buildBulletinBoard(BulletinBoard()));
            put(f2, b, buildBulletinBoard(BulletinBoard()));
            put(f1, a, buildEgc(pk[0])); put(f1, a, buildEgc(pk[1]));
            put(f2, b, buildEgc(pk[2]));
            InmcTelemetry t;
            fp.parseFrame(f1, 1700000000);
            fp.fill(t);
            CHECK(t.messages.size() == 1 && !t.messages[0].complete && t.messages[0].packets == 2, "after the first frame: complete %d", t.messages.empty() ? -1 : t.messages[0].complete);
            put(f2, b, buildEgc(pk[3]));
            fp.parseFrame(f2, 1700000009);
            fp.fill(t);
            CHECK(t.messages.size() == 1, "one message");
            const InmcMessage& m = t.messages[0];
            CHECK(m.complete && m.text == text, "text [%s]", m.text.c_str());
            CHECK(m.id == 4242 && m.serviceCode == 0x31 && m.kind == 1 && m.priority == 1 && m.packets == 4, "header fields");
            CHECK(m.seen == (uint32_t)pass + 1, "seen %u in pass %d", m.seen, pass);
            CHECK(m.serviceText.find("NAVAREA") != std::string::npos && m.area == "11 11 11", "service %s, area [%s]", m.serviceText.c_str(), m.area.c_str());
            CHECK(m.addr0 == 0x11 && m.sat == 0 && m.lesId == 17, "first address byte");
            CHECK(t.messageCount == 1, "message count %u", t.messageCount);
        }
        // a message with a missing middle packet stays incomplete
        FrameParser fq;
        uint8_t f[kFrameBytes] = {};
        int a = 0;
        put(f, a, buildEgc(pk[0])); put(f, a, buildEgc(pk[2])); put(f, a, buildEgc(pk[3]));
        fq.parseFrame(f, 1);
        InmcTelemetry t;
        fq.fill(t);
        CHECK(t.messages.size() == 1 && !t.messages[0].complete, "a gap must leave the message incomplete");
    }
    // priorities, FleetNET, other presentation
    {
        FrameParser fp;
        uint8_t f[kFrameBytes] = {};
        int a = 0;
        for (int pr = 0; pr < 4; pr++) put(f, a, buildEgc(split(0x02, (uint16_t)(10 + pr), "HELLO", 48, pr)[0]));
        fp.parseFrame(f, 1);
        InmcTelemetry t;
        fp.fill(t);
        CHECK(t.messages.size() == 4, "four messages");
        for (auto& m : t.messages) CHECK(m.kind == 2 && m.priority == (int)m.id - 10 && m.complete && m.text == "HELLO", "message %u priority %d kind %d", m.id, m.priority, m.kind);
    }
    // ITA2 round trip with figure and letter shifts
    {
        const std::string s = "FLEET 14 PORT: DUBAI, ETA 0630.";
        const auto codes = textToIta2(s);
        CHECK(ita2ToText(codes) == s, "ITA2 round trip: [%s]", ita2ToText(codes).c_str());
        CHECK(ita2ToText({0x1F, 0x01, 0x03, 0x1B, 0x01, 0x13}) == "EA32", "ITA2 shifts: %s", ita2ToText({0x1F, 0x01, 0x03, 0x1B, 0x01, 0x13}).c_str());
    }
    // multiframe packet: an EGC packet carried in 0xBD and 0xBE packets of two frames
    {
        EgcPacket e = split(0x31, 777, "MULTIFRAME TEXT THAT IS CARRIED IN SEVERAL PACKETS", 60)[0];
        auto inner = buildEgc(e);
        inner.resize(inner.size() - 2);                 // the assembled packet has no check bytes of its own in the reference decoder
        inner.insert(inner.end(), 2, 0);
        const size_t total = inner.size();
        const size_t cut = 20;
        std::vector<uint8_t> bd = {0xBD, (uint8_t)(2 + cut + 2 - 2)};
        bd.insert(bd.end(), inner.begin(), inner.begin() + cut);
        bd.insert(bd.end(), 2, 0); packetCheckSet(bd.data(), (int)bd.size());
        std::vector<uint8_t> be = {0xBE, 0};
        be.insert(be.end(), inner.begin() + cut, inner.end() - 2);
        be.insert(be.end(), 2, 0); be[1] = (uint8_t)(be.size() - 2); packetCheckSet(be.data(), (int)be.size());
        uint8_t f1[kFrameBytes] = {}, f2[kFrameBytes] = {};
        int a = 0, b = 0;
        put(f1, a, bd); put(f2, b, be);
        FrameParser fp;
        fp.parseFrame(f1, 1);
        fp.parseFrame(f2, 2);
        InmcTelemetry t;
        fp.fill(t);
        CHECK(t.messages.size() == 1 && t.messages[0].id == 777 && t.messages[0].complete, "multiframe packet: %zu messages (total %zu bytes)", t.messages.size(), total);
    }
    // list limits: 200 messages leave 150, the report stays small
    {
        FrameParser fp;
        const std::string big(400, 'X');
        for (int i = 0; i < 200; i++) {
            uint8_t f[kFrameBytes] = {};
            int a = 0;
            EgcPacket p = split(0x31, (uint16_t)(2000 + i), "S", 1)[0];
            p.payload.assign(400 > 60 ? 60 : 0, 'X');
            put(f, a, buildEgc(p));
            fp.parseFrame(f, 100 + i);
        }
        InmcTelemetry t;
        fp.fill(t);
        size_t bytes = 0;
        for (auto& m : t.messages) bytes += m.text.size() + 300;
        CHECK(t.messages.size() == 150 && t.messageCount == 200, "%zu kept, %u counted", t.messages.size(), t.messageCount);
        CHECK(t.messages.front().id == 2199 && t.messages.back().id == 2050, "newest first: %u ... %u", t.messages.front().id, t.messages.back().id);
        CHECK(bytes < 100000, "report size %zu", bytes);
    }
    // services and names
    CHECK(serviceName(0x31).find("NAVAREA") != std::string::npos && serviceKind(0x31) == 1 && serviceKind(0x02) == 2 && serviceKind(0x00) == 0, "service tables");
    CHECK(addressLength(0x04) == 7 && addressLength(0x31) == 4 && addressLength(0x02) == 5 && addressLength(0x7F) == 3, "address lengths");
    CHECK(lesName(3, 44) == "NCS" && lesName(0, 1) == "Vizada-Telenor, USA" && lesName(1, 63).empty(), "station names");
    printf(fails ? "FAILED\n" : "inmc_pkt ok\n");
    return fails ? 1 : 0;
}
