// ATSC 3.0 ALP (A/330): header formats, and a stream of mixed IPv4, signaling and TS packets (single, segmented, concatenated) cut into
// baseband packets and reassembled.
#include "dect2/atsc3_alp.h"
#include "dect2/atsc3_bb.h"
#include <cstdio>
#include <random>

using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static std::mt19937 rng(77);

static AlpPacket ip(int len) {
    AlpPacket p;
    p.type = AlpIpv4;
    p.data.resize(len);
    for (auto& v : p.data) v = rng() & 0xFF;
    p.data[0] = 0x45;
    return p;
}

static bool equal(const AlpPacket& a, const AlpPacket& b) {
    return a.type == b.type && a.data == b.data && a.sid == b.sid && (a.type != AlpSignaling || (a.signalingType == b.signalingType && a.signalingExt == b.signalingExt &&
           a.signalingVersion == b.signalingVersion && a.signalingFormat == b.signalingFormat && a.signalingEncoding == b.signalingEncoding));
}

int main() {
    // header bits
    AlpPacket p = ip(100);
    auto v = alpSingle(p);
    CHECK(v.size() == 102 && v[0] == 0x00 && v[1] == 100, "single IPv4 packet: 2 byte header, type 000, PC 0, HM 0");
    p = ip(3000);
    v = alpSingle(p);
    CHECK(v.size() == 3003 && v[0] == 0x08 + ((3000 & 0x7FF) >> 8) && v[1] == (3000 & 0xFF) && (v[2] >> 3) == (3000 >> 11) && (v[2] & 4), "long packet: HM 1 with length_MSB and the reserved bit");
    AlpPacket sg; sg.type = AlpSignaling; sg.data = {1, 2, 3, 4}; sg.signalingType = 1; sg.signalingExt = 0x0102; sg.signalingVersion = 7; sg.signalingFormat = 1;
    v = alpSingle(sg);
    CHECK(v[0] >> 5 == 4 && v.size() == 2 + 5 + 4 && v[2] == 1 && v[3] == 1 && v[4] == 2 && v[5] == 7 && (v[6] & 0x0F) == 0x0F, "signaling packet header");
    std::vector<uint8_t> ts(188 * 3);
    for (size_t i = 0; i < ts.size(); i++) ts[i] = (i % 188 == 0) ? 0x47 : (uint8_t)rng();
    v = alpTs(ts, 0);
    CHECK(v.size() == 1 + 3 * 187 && v[0] == (0xE0 | 3 << 1), "TS packets: 1 byte header, NUMTS");

    // reassembly of a mixed stream through baseband packets of different sizes
    std::vector<AlpPacket> sent;
    std::vector<std::vector<uint8_t>> alpPackets;   // as transmitted, in order
    std::vector<AlpPacket> expected;
    for (int round = 0; round < 60; round++) {
        int kind = rng() % 6;
        if (kind == 0) { AlpPacket a = ip(40 + rng() % 1400); alpPackets.push_back(alpSingle(a)); expected.push_back(a); }
        else if (kind == 1) {   // concatenation of 2 to 9 small packets
            int n = 2 + rng() % 8;
            std::vector<AlpPacket> g;
            for (int i = 0; i < n; i++) g.push_back(ip(20 + rng() % 120));
            alpPackets.push_back(alpConcatenate(g));
            for (auto& a : g) expected.push_back(a);
        } else if (kind == 2) {   // segmentation of a large packet
            AlpPacket a = ip(1200 + rng() % 3000);
            for (auto& s : alpSegments(a, 300 + rng() % 500)) alpPackets.push_back(s);
            expected.push_back(a);
        } else if (kind == 3) {
            AlpPacket a; a.type = AlpSignaling; a.data.resize(50 + rng() % 400);
            for (auto& x : a.data) x = rng() & 0xFF;
            a.signalingType = 1 + rng() % 2; a.signalingExt = rng() & 0xFFFF; a.signalingVersion = rng() & 0xFF; a.signalingFormat = rng() % 3; a.signalingEncoding = rng() % 2;
            alpPackets.push_back(alpSingle(a)); expected.push_back(a);
        } else if (kind == 4) {   // sub-stream identifier
            AlpPacket a = ip(100 + rng() % 300); a.sid = rng() & 0xFF;
            alpPackets.push_back(alpSingle(a)); expected.push_back(a);
        } else {
            std::vector<uint8_t> t(188 * (1 + rng() % 7));
            for (size_t i = 0; i < t.size(); i++) t[i] = (i % 188 == 0) ? 0x47 : (uint8_t)rng();
            AlpPacket a; a.type = AlpTs; a.data = t;
            alpPackets.push_back(alpTs(t, 0)); expected.push_back(a);
        }
    }
    std::vector<uint8_t> stream;
    std::vector<size_t> starts;
    for (auto& a : alpPackets) { starts.push_back(stream.size()); stream.insert(stream.end(), a.begin(), a.end()); }
    // cut into baseband packets of 1056 bytes, with the header of the real layout and a pointer
    const int bytes = 1056;
    AlpReassembler re;
    std::vector<AlpPacket> got;
    size_t pos = 0, nextStart = 0;
    int bbCount = 0;
    while (pos < stream.size()) {
        // pointer: offset in this baseband packet's payload of the first ALP packet that starts in it
        while (nextStart < starts.size() && starts[nextStart] < pos) nextStart++;
        // the header takes 1 byte (pointer < 128) or 2 bytes
        int hdr = 2, payload = bytes - hdr;
        int pointer = 8191;
        if (nextStart < starts.size() && starts[nextStart] < pos + payload) pointer = (int)(starts[nextStart] - pos);
        std::vector<uint8_t> chunk(stream.begin() + pos, stream.begin() + std::min(stream.size(), pos + payload));
        int used = 0;
        auto bb = makeBbPacket(bytes, chunk, pointer == 8191 ? -1 : pointer, bbCount);
        BbHeader h;
        CHECK(parseBbHeader(bb.data(), bytes, h), "baseband header parses");
        // the real payload starts after the header, of the length that makeBbPacket chose
        std::vector<uint8_t> body(bb.begin() + h.headerBytes, bb.end());
        int take = std::min<int>((int)chunk.size(), (int)body.size());
        // when the packet is not full the padding is in the header; the chunk is then shorter than the room
        re.push(body.data(), take, h.pointer, got);
        used = take;
        pos += used;
        bbCount++;
        (void)used;
    }
    CHECK(got.size() == expected.size(), "number of input packets reassembled");
    int bad = 0;
    for (size_t i = 0; i < got.size() && i < expected.size(); i++) bad += !equal(got[i], expected[i]);
    printf("  %d ALP packets in %d baseband packets: %zu of %zu input packets back, %d wrong, %ld errors\n", (int)alpPackets.size(), bbCount, got.size(), expected.size(), bad, re.errors());
    CHECK(bad == 0 && re.errors() == 0, "reassembled packets match");
    printf(fails ? "atsc3 alp: FAILED\n" : "atsc3 alp: ok\n");
    return fails ? 1 : 0;
}
