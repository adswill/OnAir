// Project Horus protocol layer: the CRC and the packet layout against the test packet and test sentence published on the
// horusdemodlib wiki ("5 Customising a Horus Binary v2 Packet"), the custom field worked example from the same page, Golay
// correction, the layer 2 frame round trip with bit errors, the tables and the RTTY sentence parser.
#include "dect2/horus/horus_proto.h"
#include "../common/test_util.h"
#include <cstring>
#include <random>
#include <cstdlib>
#include <string>
using namespace dect2::horus;

static std::vector<uint8_t> hex(const char* h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) { char b[3] = {h[i], h[i + 1], 0}; v.push_back((uint8_t)strtoul(b, nullptr, 16)); }
    return v;
}

int main() {
    // the wiki's test sentence and its CRC
    CHECK(crc16(std::string("4FSKTEST-V2,95,12:34:56,0.00000,0.00000,0,0,0,0,0.00,1,1.234568,3.92,12.3,12.34")) == 0xBBDB, "UKHAS sentence CRC");

    // the wiki's test packet: CRC little-endian at bytes 30..31, ID 256, sequence 95, 12:34:56
    const auto pkt = hex("00015F000C223800000000000000000000000000000152069E3FC87BD20429BE");
    CHECK(pkt.size() == 32, "test packet length");
    Tables t;
    CHECK(parseIdList(defaultIdList(), t.ids) == 0 && t.ids[256] == "4FSKTEST-V2", "default ID list");
    // the decoded line on the wiki was made with these custom fields (B f B B H), not with the RS41ng default entry
    std::string err;
    CHECK(parseCustomFields("{\"4FSKTEST-V2\": {\"comment\": \"test\", \"struct\": \"<BfBBH\", \"fields\": [[\"a\",\"none\"],[\"b\",\"none\"],[\"c\",\"battery_5v_byte\"],[\"d\",\"divide_by_10\"],[\"e\",\"divide_by_100\"]]}}", t.formats, &err), "custom fields JSON: %s", err.c_str());
    Packet p;
    CHECK(parseBinary(pkt.data(), 32, t, p), "test packet CRC");
    CHECK(p.kind == 2 && p.payloadId == 256 && p.callsign == "4FSKTEST-V2" && p.seq == 95 && p.hour == 12 && p.minute == 34 && p.second == 56, "test packet header");
    CHECK(p.sentence == "$$4FSKTEST-V2,95,12:34:56,0.00000,0.00000,0,0,0,0,0.00,1,1.234568,3.92,12.3,12.34*BBDB", "the wiki's decoded line: %s", p.sentence.c_str());
    auto bad = pkt; bad[5] ^= 1;
    CHECK(!parseBinary(bad.data(), 32, t, p), "a flipped bit fails the CRC");

    // the wiki's worked example for the RS41ng default fields: 12014AFE0048040000 -> 2.74, -43.8, 0, 109.6
    std::map<std::string, CustomFormat> def;
    CHECK(parseCustomFields(defaultCustomFields(), def, &err) && def.count("4FSKTEST-V2"), "default custom field list");
    const auto cb = hex("12014AFE0048040000");
    const auto cf = applyFormat(def["4FSKTEST-V2"], cb.data(), 9);
    CHECK(cf.size() == 4 && cf[0].second == "2.74" && cf[1].second == "-43.8" && cf[2].second == "0" && cf[3].second == "109.6", "worked example");
    CHECK(cf.size() == 4 && cf[0].first == "ascent_rate" && cf[3].first == "ext_pressure", "field names");

    // Golay: every pattern of up to 3 errors is corrected
    std::mt19937 rng(7);
    int golayFails = 0;
    for (int k = 0; k < 4000; k++) {
        const uint32_t d = rng() & 0xFFF, c = golayEncode(d);
        uint32_t e = 0;
        const int ne = k % 4;
        auto bits = [](uint32_t v) { int n = 0; for (; v; v &= v - 1) n++; return n; };
        while (bits(e) < ne) e |= 1u << (rng() % 23);
        int fx = 0;
        if (golayDecode(c ^ e, &fx) != d || fx != ne) golayFails++;
    }
    CHECK(golayFails == 0, "Golay corrects up to 3 errors (%d failures)", golayFails);

    // layer 2: sizes, and a round trip through the interleaver and scrambler with Golay-correctable errors
    CHECK(frameBytes(22) == 45 && frameBytes(32) == 65, "frame sizes 45 / 65");
    CHECK(interleaverStep(43) == 337 && interleaverStep(63) == 389, "interleaver constants");
    for (int v = 1; v <= 2; v++) {
        Packet q; q.payloadId = v == 2 ? 300 : 7; q.seq = 1234; q.hour = 1; q.minute = 2; q.second = 3; q.lat = 54.9; q.lon = 23.9; q.altM = 12345; q.speedKmh = 40; q.sats = 9; q.tempC = -40; q.battV = 3.3;
        const auto pl = buildBinary(q, v);
        auto fr = encodeFrame(pl.data(), (int)pl.size());
        CHECK(fr.size() == (size_t)frameBytes((int)pl.size()) && fr[0] == 0x24 && fr[1] == 0x24, "frame v%d starts with the unique word", v);
        std::vector<uint8_t> out(pl.size());
        int fx = -1;
        decodeFrame(fr.data() + 2, (int)pl.size(), out.data(), &fx);
        CHECK(out == pl && fx == 0, "clean round trip v%d", v);
        for (int i = 0; i < 6; i++) { const int b = (int)(rng() % ((fr.size() - 2) * 8)); fr[2 + b / 8] ^= uint8_t(1 << (b % 8)); }
        decodeFrame(fr.data() + 2, (int)pl.size(), out.data(), &fx);
        Packet r;
        CHECK(parseBinary(out.data(), (int)out.size(), t, r) && r.seq == 1234 && r.altM == 12345 && r.tempC == -40, "round trip v%d with 6 bit errors (%d corrected)", v, fx);
    }

    // RTTY sentences
    Packet s;
    CHECK(parseSentence("$$4FSKTEST-V2,95,12:34:56,0.00000,0.00000,0,0,0,0,0.00,1,1.234568,3.92,12.3,12.34*BBDB\n", t, s) && s.kind == 0 && s.seq == 95 && s.callsign == "4FSKTEST-V2", "sentence parse");
    CHECK(!parseSentence("$$4FSKTEST-V2,95,12:34:56,0.00000,0.00000,0,0,0,0,0.00,1,1.234568,3.92,12.3,12.35*BBDB", t, s), "sentence CRC");
    Packet b; b.kind = 0; b.callsign = "HORUS"; b.seq = 12; b.hour = 10; b.minute = 11; b.second = 12; b.lat = -34.5; b.lon = 138.6; b.altM = 2345;
    const std::string line = buildSentence(b);
    CHECK(parseSentence(line, t, s) && s.lat == -34.5 && s.altM == 2345, "built sentence reads back: %s", line.c_str());

    std::map<int, std::string> ids;
    CHECK(parseIdList("# x\n0, HORUSBINARY\n1\tCALL-1 # comment\nbad line here\n70000, X\n", ids) == 2 && ids[0] == "HORUSBINARY" && ids[1] == "CALL-1", "ID list parse");
    return testSummary("test_horus_proto");
}
