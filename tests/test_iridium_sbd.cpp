// Iridium short burst data (Phase B): IDA fragments joined as iridium-toolkit's ida.py does, SBD headers and ACARS as its sbd.py
// reads them, built from the frame layer's IDA frames.
#include "dect2/iridium_frame.h"
#include "dect2/iridium_sbd.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static IridiumFrame ida(const std::vector<uint8_t>& payload, int ctr, bool more) {
    IridiumBurstBits b;
    b.bits = iridiumBuildIda(payload, ctr, more);
    return decodeIridiumBurst(b);
}

int main() {
    const std::vector<uint8_t> pk = iridiumBuildSbdAcars('2', ".N123AB", 0x15, "H1", 'D', "HELLO FROM THE GROUND");
    // the packet as the toolkit reads it
    {
        const IridiumSbd s = iridiumParseSbd(pk, true);
        CHECK(s.valid && s.type == 0x7608 && s.acars.valid, "parse: valid %d type %04x acars %d", s.valid, s.type, s.acars.valid);
        CHECK(s.acars.mode == '2' && s.acars.reg == ".N123AB" && s.acars.ack == 0x15 && s.acars.label == "H1" && s.acars.blockId == 'D', "fields");
        CHECK(s.acars.text == "HELLO FROM THE GROUND" && !s.acars.more, "text '%s'", s.acars.text.c_str());
        // from the aircraft: sequence and flight number come first
        std::vector<uint8_t> up = iridiumBuildSbdAcars('2', ".N123AB", 'A', "5Z", '1', "M01AEK0123ARRIVED GATE B4");
        up[1] = 0x09;
        const IridiumSbd u = iridiumParseSbd(up, false);
        CHECK(u.acars.valid && u.acars.seq == "M01A" && u.acars.flight == "EK0123" && u.acars.text == "ARRIVED GATE B4", "uplink fields '%s' '%s' '%s'",
              u.acars.seq.c_str(), u.acars.flight.c_str(), u.acars.text.c_str());
        std::vector<uint8_t> etb = pk;
        etb.back() = 0x17;
        CHECK(iridiumParseSbd(etb, true).acars.more, "ETB: more follows");
        CHECK(!iridiumParseSbd({0x55, 0x08, 0x20}, true).valid && !iridiumParseSbd({0x76, 0x01, 0x20, 0, 0, 0, 0, 1}, true).valid, "unknown types");
        std::vector<uint8_t> hello(2 + 29 + 3, 0);
        hello[0] = 0x06; hello[1] = 0x00; hello.push_back(0x42);
        const IridiumSbd h = iridiumParseSbd(hello, false);
        CHECK(h.valid && h.type == 0x0600 && h.payload.size() == 4 && !h.acars.valid, "0x0600: 29 header bytes, payload %zu", h.payload.size());
    }
    // fragments through the frame layer and the assembler
    const auto parts = iridiumSplitIda(pk);
    CHECK(parts.size() == 3 && parts[0].size() == 20 && parts[2].size() == pk.size() - 40, "split %zu", parts.size());
    {
        IridiumIdaAssembler a;
        std::vector<IridiumIdaPacket> got;
        for (size_t i = 0; i < parts.size(); i++) {
            const IridiumFrame f = ida(parts[i], (int)i, i + 1 < parts.size());
            CHECK(f.type == IridiumType::IDA && f.ok && f.idaCtr == (int)i, "IDA frame %zu: %s ok %d ctr %d", i, f.typeName.c_str(), f.ok, f.idaCtr);
            auto r = a.feed(f, true, 1621.5e6 + 20.0 * i, 10.0 + 0.09 * i);
            got.insert(got.end(), r.begin(), r.end());
        }
        CHECK(got.size() == 1 && got[0].complete && got[0].data == pk && got[0].fragments == 3, "joined %zu", got.size());
    }
    // the matching rules: a fragment 300 Hz away, 300 ms late, or with the wrong counter does not join; the open packet times out
    {
        struct Case { double df, dt; int ctr; const char* what; } cases[] = {{300, 0.09, 1, "300 Hz away"}, {0, 0.30, 1, "300 ms later"}, {0, 0.09, 2, "counter skips"}};
        for (const auto& c : cases) {
            IridiumIdaAssembler a;
            auto r1 = a.feed(ida(parts[0], 0, true), true, 1621.5e6, 10.0);
            auto r2 = a.feed(ida(parts[1], c.ctr, false), true, 1621.5e6 + c.df, 10.0 + c.dt);
            CHECK(r1.empty() && r2.empty(), "%s: joined", c.what);
            auto r3 = a.feed(ida(parts[2], 0, false), true, 1625e6, 11.5);
            bool timedOut = false, single = false;
            for (const auto& p : r3) { if (!p.complete) timedOut = true; else if (p.fragments == 1) single = true; }
            CHECK(timedOut && single, "%s: time out %d, the new single packet %d", c.what, timedOut, single);
        }
        IridiumIdaAssembler a;
        CHECK(a.feed(ida(parts[1], 3, false), true, 1621.5e6, 10.0).empty(), "a fragment without its start");
        CHECK(a.feed(ida(parts[1], 0, false), false, 1621.5e6, 10.0).size() == 1, "one-fragment packet");
    }
    printf(fails ? "iridium sbd: %d FAILED\n" : "iridium sbd: all passed\n", fails);
    return fails ? 1 : 0;
}
