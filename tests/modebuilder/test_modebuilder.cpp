// Mode Builder tests: every stage alone (CRC vectors, line codes, scramblers, interleaver, FEC), the JSON reader and its messages, and
// round trips: a signal generated from a definition (the examples and more), with noise, frequency and clock errors, decoded back.
// "quick" runs the stages and one round trip per definition; "sweep" adds the lower SNRs, larger offsets and the streaming checks.
#include "dect2/modebuilder/modebuilder.h"
#include "dect2/automation/json_mini.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace dect2::modebuilder;

static int fails = 0, checks = 0;
static bool gSweep = false;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKM(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rnd(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static std::vector<uint8_t> randBits(size_t n, uint32_t& s) { std::vector<uint8_t> b(n); for (auto& x : b) x = (uint8_t)(rnd(s) & 1); return b; }

static Definition defOf(const std::string& json) {
    ParseResult r = parseDefinition(json);
    if (!r.ok) for (const Issue& i : r.issues) printf("  issue: %s\n", i.text().c_str());
    CHECK(r.ok);
    return r.def;
}

static bool hasIssue(const ParseResult& r, const std::string& where, bool error) {
    for (const Issue& i : r.issues) if (i.error == error && i.text().find(where) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------- CRC
static void testCrc() {
    const char* msg = "123456789";
    struct K { const char* name; uint32_t check; };
    const K ks[] = {{"CRC-8", 0xF4}, {"CRC-8/SMBUS", 0xF4}, {"CRC-8/MAXIM", 0xA1}, {"CRC-8/AUTOSAR", 0xDF},
                    {"CRC-16/CCITT-FALSE", 0x29B1}, {"CRC-16/IBM-3740", 0x29B1}, {"CRC-16/CCITT", 0x2189}, {"CRC-16/KERMIT", 0x2189},
                    {"CRC-16/XMODEM", 0x31C3}, {"CRC-16/X-25", 0x906E}, {"CRC-16/IBM", 0xBB3D}, {"CRC-16/ARC", 0xBB3D}, {"CRC-16/MODBUS", 0x4B37},
                    {"CRC-16/DNP", 0xEA82}, {"CRC-24/OPENPGP", 0x21CF02}, {"CRC-32", 0xCBF43926}, {"CRC-32C", 0xE3069283}, {"CRC-32/BZIP2", 0xFC891918}};
    for (const K& k : ks) {
        CrcModel m;
        CHECKM(crcPreset(k.name, m), "%s", k.name);
        const uint32_t v = crcCompute(m, (const uint8_t*)msg, 9);
        CHECKM(v == k.check, "%s: %X want %X", k.name, v, k.check);
    }
    // case does not matter; unknown names are refused
    CrcModel m;
    CHECK(crcPreset("crc-16/ibm", m) && m.poly == 0x8005);
    CHECK(!crcPreset("CRC-16/NOPE", m));
    // odd widths: CRC-5/USB (0x05, init 0x1F, reflected, xorout 0x1F) = 0x19; CRC-12/UMTS (0x80F, refout only) = 0xDAF
    CrcModel c5{"", 5, 0x05, 0x1F, 0x1F, true, true};
    CHECK(crcCompute(c5, (const uint8_t*)msg, 9) == 0x19);
    CrcModel c12{"", 12, 0x80F, 0, 0, false, true};
    CHECK(crcCompute(c12, (const uint8_t*)msg, 9) == 0xDAF);
    // through a definition: the check value at the end, big and little endian, a custom range
    for (int little = 0; little < 2; little++) {
        Definition d = defOf(std::string(R"({"onair_mode":1,"name":"t","modulation":{"type":"FSK","symbol_rate":1000},"sync":{"word":"0x2DD4"},
            "frame":{"length":{"mode":"fixed","bytes":12}},"crc":{"preset":"CRC-16/IBM","from_byte":2,"byte_order":")") + (little ? "little" : "big") + "\"}}");
        std::vector<uint8_t> b(12);
        for (int i = 0; i < 10; i++) b[(size_t)i] = (uint8_t)("xy123456789"[i]);
        b[0] = 'x'; b[1] = 'y';
        for (int i = 0; i < 9; i++) b[(size_t)i + 1] = (uint8_t)msg[i];   // bytes 1..9 = "123456789"
        Definition d2 = d;
        d2.crcFrom = 1;
        b.resize(12);
        fillCrc(d2, b);
        CHECK(little ? (b[10] == 0x3D && b[11] == 0xBB) : (b[10] == 0xBB && b[11] == 0x3D));
        bool present = false, ok = false;
        checkCrc(d2, b, present, ok);
        CHECK(present && ok);
        b[4] ^= 1;
        checkCrc(d2, b, present, ok);
        CHECK(present && !ok);
    }
    // SUM8 / XOR8
    Definition s = defOf(R"({"onair_mode":1,"name":"t","modulation":{"type":"FSK","symbol_rate":1000},"sync":{"word":"0x2DD4"},"frame":{"length":{"mode":"fixed","bytes":4}},"crc":{"preset":"SUM8"}})");
    std::vector<uint8_t> b{0x10, 0x20, 0xF0, 0};
    fillCrc(s, b);
    CHECK(b[3] == 0x20);
    Definition x = defOf(R"({"onair_mode":1,"name":"t","modulation":{"type":"FSK","symbol_rate":1000},"sync":{"word":"0x2DD4"},"frame":{"length":{"mode":"fixed","bytes":4}},"crc":{"preset":"XOR8"}})");
    fillCrc(x, b);
    CHECK(b[3] == (0x10 ^ 0x20 ^ 0xF0));
}

// ---------------------------------------------------------------- bit stages
static void testLineCodes() {
    uint32_t s = 7;
    const Line all[] = {Line::NRZ, Line::NRZI, Line::Manchester, Line::ManchesterThomas, Line::DiffManchester, Line::Miller, Line::B4B5, Line::B8B10};
    for (Line l : all) {
        for (int tog = 0; tog < 2; tog++) {
            const std::vector<uint8_t> bits = randBits(800, s);
            std::vector<uint8_t> chips = stage::lineEncode(bits, l, tog);
            CHECKM(std::fabs((double)chips.size() / bits.size() - stage::lineExpansion(l)) < 1e-9, "%s expansion", lineName(l));
            // junk in front: the decoder at the right phase finds the bits again
            const int junk = 3;
            std::vector<uint8_t> rx = randBits((size_t)junk, s);
            rx.insert(rx.end(), chips.begin(), chips.end());
            const int ph = junk % stage::linePhases(l);
            std::vector<int> pos;
            int errs = 0;
            std::vector<uint8_t> dec = stage::lineDecode(rx, l, tog, ph, &pos, &errs);
            const int skip = (junk / stage::linePhases(l) + (junk % stage::linePhases(l) ? 0 : 0));
            // find the bits: the decoded stream contains them after the junk
            const size_t off = (size_t)(junk - ph) / (size_t)stage::linePhases(l) * (l == Line::B8B10 ? 8 : l == Line::B4B5 ? 4 : 1);
            (void)skip;
            bool same = dec.size() >= off + bits.size();
            size_t bad = 0;
            for (size_t i = 0; same && i < bits.size(); i++) bad += dec[off + i] != bits[i];
            // NRZI / differential Manchester may get the very first bit wrong (no level before it)
            CHECKM(same && bad <= 1, "%s toggle %d: %zu wrong", lineName(l), tog, bad);
            CHECK(pos.size() == dec.size());
        }
    }
    // the 8b10b stream is DC-balanced and has no run longer than 5
    const std::vector<uint8_t> zeros(800, 0);
    const std::vector<uint8_t> c = stage::lineEncode(zeros, Line::B8B10, 0);
    int run = 1, maxRun = 1, sum = 0;
    for (size_t i = 0; i < c.size(); i++) { sum += c[i] ? 1 : -1; if (i && c[i] == c[i - 1]) maxRun = std::max(maxRun, ++run); else run = 1; }
    CHECK(maxRun <= 5);
    CHECK(std::abs(sum) <= 2);
    // invalid 8b10b words are counted
    std::vector<uint8_t> badw(10, 1);
    int errs = 0;
    stage::lineDecode(badw, Line::B8B10, 0, 0, nullptr, &errs);
    CHECK(errs == 1);
    // HDLC stuffing
    const std::vector<uint8_t> ones(12, 1);
    const std::vector<uint8_t> st = stage::hdlcStuff(ones);
    CHECK(st.size() == 14 && st[5] == 0 && st[11] == 0);
}

static void testScramblerInterleaver() {
    uint32_t s = 11;
    Definition d;
    d.scrambler = Scrambler::Additive;
    d.taps = {9, 5};
    d.seed = 0x1FF;
    // a PN9: period 511
    std::vector<uint8_t> z(1022, 0);
    stage::scramble(z, d, true);
    bool period = true;
    for (size_t i = 0; i < 511; i++) period &= z[i] == z[i + 511];
    int ones = 0;
    for (size_t i = 0; i < 511; i++) ones += z[i];
    CHECK(period && ones == 256);
    for (Scrambler sc : {Scrambler::Additive, Scrambler::Multiplicative}) {
        d.scrambler = sc;
        const std::vector<uint8_t> bits = randBits(500, s);
        std::vector<uint8_t> x = bits;
        stage::scramble(x, d, true);
        CHECK(x != bits);
        stage::scramble(x, d, false);
        CHECK(x == bits);
    }
    // a multiplicative descrambler resynchronises after a bit error: only a few bits after it are wrong
    d.scrambler = Scrambler::Multiplicative;
    std::vector<uint8_t> bits = randBits(500, s), x = bits;
    stage::scramble(x, d, true);
    x[100] ^= 1;
    stage::scramble(x, d, false);
    int wrong = 0;
    for (size_t i = 0; i < bits.size(); i++) wrong += x[i] != bits[i];
    CHECK(wrong == 3);   // the bit and one per tap
    for (int r : {1, 4, 8}) for (int c : {1, 7, 16}) {
        const std::vector<uint8_t> b = randBits((size_t)(r * c * 3 + 5), s);
        std::vector<uint8_t> y = b;
        stage::interleave(y, r, c, true);
        if (r > 1 && c > 1) CHECK(y != b);
        stage::interleave(y, r, c, false);
        CHECK(y == b);
    }
    // the interleaver spreads a burst: 8 adjacent errors land in 8 different rows
    std::vector<uint8_t> e(128, 0);
    for (int i = 40; i < 48; i++) e[(size_t)i] = 1;
    stage::interleave(e, 8, 16, false);
    int rowsHit = 0;
    for (int r = 0; r < 8; r++) { int n = 0; for (int c = 0; c < 16; c++) n += e[(size_t)(r * 16 + c)]; rowsHit += n == 1; }
    CHECK(rowsHit == 8);
}

static void testFec() {
    uint32_t s = 5;
    auto mk = [](const char* fec) {
        return defOf(std::string(R"({"onair_mode":1,"name":"t","modulation":{"type":"FSK","symbol_rate":1000},"sync":{"word":"0x2DD4"},"frame":{"length":{"mode":"fixed","bytes":40}},"fec":)") + fec + "}");
    };
    // repetition: one error per group is corrected
    {
        Definition d = mk(R"({"type":"repetition","n":3})");
        const std::vector<uint8_t> b = randBits(320, s);
        std::vector<uint8_t> c = stage::fecEncode(d, b), o;
        CHECK(c.size() == stage::codedBits(d, b.size()));
        for (size_t i = 0; i < c.size(); i += 3) c[i + rnd(s) % 3] ^= 1;
        int corr = 0;
        CHECK(stage::fecDecode(d, c, b.size(), o, corr) && o == b && corr == 320);
    }
    // Hamming: one error per code word corrected; the extended codes see two
    for (const char* code : {"7,4", "15,11", "31,26", "8,4", "16,11"}) {
        Definition d = mk((std::string(R"({"type":"hamming","code":")") + code + "\"}").c_str());
        const std::vector<uint8_t> b = randBits(320, s);
        std::vector<uint8_t> c = stage::fecEncode(d, b), o;
        CHECKM(c.size() == stage::codedBits(d, b.size()), "%s", code);
        for (size_t i = 0; i + (size_t)d.hamN <= c.size(); i += (size_t)d.hamN) c[i + rnd(s) % (size_t)d.hamN] ^= 1;
        int corr = 0;
        CHECKM(stage::fecDecode(d, c, b.size(), o, corr) && o == b, "%s", code);
        if (d.hamExtended) {
            std::vector<uint8_t> c2 = stage::fecEncode(d, b);
            c2[0] ^= 1;
            c2[2] ^= 1;
            CHECKM(!stage::fecDecode(d, c2, b.size(), o, corr), "%s double error", code);
        }
    }
    // convolutional K=7 r=1/2 and a K=5 r=1/3 code: scattered errors are corrected
    for (const char* f : {R"({"type":"convolutional"})", R"({"type":"convolutional","k":5,"polys":["0o25","0o33","0o37"]})", R"({"type":"convolutional","tail":false})"}) {
        Definition d = mk(f);
        const std::vector<uint8_t> b = randBits(320, s);
        std::vector<uint8_t> c = stage::fecEncode(d, b), o;
        CHECK(c.size() == stage::codedBits(d, b.size()));
        for (size_t i = 7; i + 20 < c.size(); i += 23) c[i] ^= 1;
        int corr = 0;
        stage::fecDecode(d, c, b.size(), o, corr);
        size_t bad = 0;
        for (size_t i = 0; i < b.size(); i++) bad += o[i] != b[i];
        // without a tail the last few bits are less protected
        CHECKM(d.convTail ? bad == 0 : bad <= 3, "%s: %zu wrong", f, bad);
        CHECK(corr >= (int)(c.size() / 23) - 1);
    }
    // the K=7 code matches the NASA standard: a single 1 gives the generator taps 1111001 / 1011011 interleaved
    {
        Definition d = mk(R"({"type":"convolutional","tail":false})");
        std::vector<uint8_t> one(7, 0);
        one[0] = 1;
        const std::vector<uint8_t> c = stage::fecEncode(d, one);
        std::string g1, g2;
        for (size_t i = 0; i < c.size(); i += 2) { g1 += (char)('0' + c[i]); g2 += (char)('0' + c[i + 1]); }
        CHECKM(g1 == "1111001" && g2 == "1011011", "%s %s", g1.c_str(), g2.c_str());
    }
    // Reed-Solomon: several codes, t errors corrected, shortened words, t+1 errors refused
    struct RsCase { int m; uint32_t poly; int n, k, fcr, prim; };
    for (const RsCase rc : {RsCase{8, 0x11D, 255, 223, 0, 1}, RsCase{8, 0x187, 255, 223, 112, 11}, RsCase{4, 0x13, 15, 11, 1, 1}, RsCase{8, 0x11D, 204, 188, 0, 1}, RsCase{6, 0x43, 63, 51, 1, 1}}) {
        ReedSolomon rs(rc.m, rc.poly, rc.n, rc.k, rc.fcr, rc.prim);
        CHECK(rs.valid());
        const int t = (rc.n - rc.k) / 2;
        for (int len : {rc.k, rc.k / 2, 3}) {
            std::vector<int> data((size_t)len);
            for (int& v : data) v = (int)(rnd(s) % (uint32_t)(1 << rc.m));
            const std::vector<int> cw = rs.encode(data);
            CHECK((int)cw.size() == len + rc.n - rc.k);
            std::vector<int> rx = cw;
            CHECK(rs.decode(rx) == 0 && rx == cw);
            for (int ne : {1, t}) {
                rx = cw;
                for (int e = 0; e < ne; e++) rx[(size_t)(e * 2 % (int)rx.size())] ^= 1 + (int)(rnd(s) % (uint32_t)((1 << rc.m) - 1));
                const int r = rs.decode(rx);
                CHECKM(r == ne && rx == cw, "RS(%d,%d) m%d fcr%d len %d: %d errors gave %d", rc.n, rc.k, rc.m, rc.fcr, len, ne, r);
            }
        }
        std::vector<int> data((size_t)rc.k, 0);
        std::vector<int> rx = rs.encode(data);
        for (int e = 0; e <= t; e++) rx[(size_t)e * 3] ^= 1;
        CHECK(rs.decode(rx) < 0 || rx != rs.encode(data));
    }
    CHECK(!ReedSolomon(8, 0x11B, 255, 223, 0, 1).valid());   // the AES polynomial is not primitive
    // RS through a definition: a 40-byte frame in shortened code words
    {
        Definition d = mk(R"({"type":"reed_solomon","n":255,"k":223})");
        const std::vector<uint8_t> b = randBits(320, s);
        std::vector<uint8_t> c = stage::fecEncode(d, b), o;
        CHECK(c.size() == (40 + 32) * 8u);
        for (int e = 0; e < 16; e++) c[(size_t)e * 36] ^= 1;
        int corr = 0;
        CHECK(stage::fecDecode(d, c, b.size(), o, corr) && o == b && corr == 16);
    }
}

// ---------------------------------------------------------------- the JSON reader
static void testParse() {
    const std::string base = R"({"onair_mode":1,"name":"Test","modulation":{"type":"FSK","symbol_rate":1200,"deviation_hz":3000},"sync":{"word":"0x2DD4"},"frame":{"length":{"mode":"fixed","bytes":6}}})";
    ParseResult r = parseDefinition(base);
    CHECK(r.ok);
    CHECK(r.def.id == "test" && r.def.sync.bits == 16 && r.def.sync.value == 0x2DD4);
    // broken JSON: the line is named
    r = parseDefinition("{\n\"onair_mode\": 1,\n\"name\": \"x\"\n\"modulation\": {}\n}");
    CHECK(!r.ok && hasIssue(r, "line 4", true));
    r = parseDefinition("");
    CHECK(!r.ok && !r.issues.empty());
    r = parseDefinition("[1,2]");
    CHECK(!r.ok && hasIssue(r, "JSON object", true));
    r = parseDefinition("{\"onair_mode\": 1, \"name\": \"x\", \"modulation\": {\"type\": \"FSK\", \"symbol_rate\": 1200}");
    CHECK(!r.ok);
    // missing and wrong keys
    r = parseDefinition(R"({"name":"x"})");
    CHECK(!r.ok && hasIssue(r, "onair_mode", true) && hasIssue(r, "modulation", true) && hasIssue(r, "sync", true) && hasIssue(r, "frame", true));
    r = parseDefinition(R"({"onair_mode":2,"name":"x","modulation":{"type":"FSK","symbol_rate":1200},"sync":{"word":"0x2DD4"},"frame":{"length":{"mode":"fixed","bytes":6}}})");
    CHECK(!r.ok && hasIssue(r, "newer", true));
    auto with = [&](const std::string& from, const std::string& to) { std::string s = base; s.replace(s.find(from), from.size(), to); return parseDefinition(s); };
    r = with("\"symbol_rate\":1200", "\"symbol_rate\":-5");
    CHECK(!r.ok && hasIssue(r, "modulation.symbol_rate", true));
    r = with("\"type\":\"FSK\"", "\"type\":\"QAM\"");
    CHECK(!r.ok && hasIssue(r, "modulation.type", true));
    r = with("\"name\":\"Test\"", "\"name\":\"Test\",\"modulaton\":{}");
    CHECK(r.ok && hasIssue(r, "unknown key \"modulaton\"", false));
    r = with("\"word\":\"0x2DD4\"", "\"word\":\"hello\"");
    CHECK(!r.ok && hasIssue(r, "sync.word", true));
    r = with("\"word\":\"0x2DD4\"", "\"word\":\"0x2DD4\",\"bits\":8");
    CHECK(!r.ok && hasIssue(r, "sync.word", true));   // the value is wider than 8 bits
    r = with("\"word\":\"0x2DD4\"", "\"word\":\"1011001110001111\"");
    CHECK(r.ok && r.def.sync.bits == 16 && r.def.sync.value == 0xB38F);
    r = with("\"word\":\"0x2DD4\"", "\"word\":\"0x2DD4\",\"max_errors\":6");
    CHECK(!r.ok && hasIssue(r, "sync.max_errors", true));
    auto top = [&](const std::string& extra, const std::string& b = std::string()) { std::string s = b.empty() ? base : b; s.insert(s.size() - 1, "," + extra); return parseDefinition(s); };
    r = top(R"("fec":{"type":"reed_solomon","n":100,"k":120})");
    CHECK(!r.ok && hasIssue(r, "fec.k", true));
    r = top(R"("fec":{"type":"hamming","code":"9,5"})");
    CHECK(!r.ok && hasIssue(r, "fec.code", true));
    r = top(R"("fields":[{"name":"a","offset_bits":40,"width_bits":16}])");
    CHECK(!r.ok && hasIssue(r, "fields[0]", true));
    r = top(R"("fields":[{"name":"a b","offset_bits":0}])");
    CHECK(!r.ok && hasIssue(r, "fields[0].name", true));
    r = top(R"("fields":[{"name":"a","offset_bits":0,"type":"enum"}])");
    CHECK(!r.ok && hasIssue(r, "fields[0].enum", true));
    r = top(R"("fields":[{"name":"a","offset_bits":0},{"name":"a","offset_bits":8}])");
    CHECK(!r.ok && hasIssue(r, "used twice", true));
    r = top(R"("crc":{"preset":"CRC-99"})");
    CHECK(!r.ok && hasIssue(r, "crc.preset", true));
    {
        std::string b1 = base;
        b1.replace(b1.find("\"bytes\":6"), 9, "\"bytes\":1");
        r = top(R"("crc":{"preset":"CRC-32"})", b1);
        CHECK(!r.ok && hasIssue(r, "crc", true));
        std::string ef = base;
        ef.replace(ef.find("\"mode\":\"fixed\",\"bytes\":6"), std::string("\"mode\":\"fixed\",\"bytes\":6").size(), "\"mode\":\"end_flag\",\"flag\":\"0x7E\"");
        r = top(R"("fec":{"type":"repetition"})", ef);
        CHECK(!r.ok && hasIssue(r, "fec", true));
        r = parseDefinition(ef);
        CHECK(r.ok);
    }
    r = top(R"("display":"{nope} {_len}")");
    CHECK(r.ok && hasIssue(r, "{nope}", false));
    r = top(R"("display":[{"when":{"field":"zz","equals":1},"template":"x"}])");
    CHECK(!r.ok && hasIssue(r, "when.field", true));
    r = top(R"("polarity":"sideways")");
    CHECK(!r.ok && hasIssue(r, "polarity", true));
    r = top(R"("scrambler":{"type":"additive"})");
    CHECK(!r.ok && hasIssue(r, "scrambler.taps", true));
    r = top(R"("interleaver":{"rows":4})");
    CHECK(!r.ok && hasIssue(r, "interleaver", true));
    r = with("\"deviation_hz\":3000", "\"deviation_hz\":3000,\"symbol_map\":[0,0]");
    CHECK(!r.ok && hasIssue(r, "symbol_map", true));
    r = with("\"symbol_rate\":1200", "\"symbol_rate\":\"fast\"");
    CHECK(!r.ok && hasIssue(r, "modulation.symbol_rate", true));
    // the written JSON reads back to the same definition
    for (const Example& e : examples()) {
        ParseResult a = parseDefinition(e.text);
        CHECKM(a.ok, "%s", e.file.c_str());
        for (const Issue& i : a.issues) CHECKM(false, "%s: %s", e.file.c_str(), i.text().c_str());
        const std::string j1 = toJson(a.def);
        ParseResult b = parseDefinition(j1);
        CHECKM(b.ok, "%s rewritten", e.file.c_str());
        CHECKM(toJson(b.def) == j1, "%s: not stable", e.file.c_str());
    }
    CHECK(examples().size() >= 5);
    CHECK(makeId("OOK 433 MHz sensor!") == "ook-433-mhz-sensor");
}

// ---------------------------------------------------------------- fields and export
static void testFields() {
    const Example* ook = nullptr;
    for (const Example& e : examples()) if (e.file == "ook-433-sensor.json") ook = &e;
    CHECK(ook);
    if (!ook) return;
    Definition d = defOf(ook->text);
    // id 0xA7, battery low, channel 3 (stored 2), temperature -12.3 C (-123 = 0xF85), humidity 55 %
    Frame f;
    f.bytes = {0xA7, (uint8_t)(0x80 | (2 << 4) | 0x0F), 0x85, 55, 0};
    fillCrc(d, f.bytes);
    checkCrc(d, f.bytes, f.hasCrc, f.crcOk);
    CHECK(f.hasCrc && f.crcOk);
    interpretFrame(d, f);
    CHECK(f.fields.size() == 5);
    CHECKM(f.fields[0].text == "0xA7", "%s", f.fields[0].text.c_str());
    CHECK(f.fields[1].text == "true");
    CHECK(f.fields[2].num == 3);
    CHECKM(std::fabs(f.fields[3].num + 12.3) < 1e-9 && f.fields[3].text == "-12.3", "%s", f.fields[3].text.c_str());
    CHECK(f.fields[4].num == 55);
    CHECKM(f.display == "Sensor 0xA7 ch 3: -12.3 C, 55 %, battery low true", "%s", f.display.c_str());
    // the other types
    Definition t = defOf(R"({"onair_mode":1,"name":"t","modulation":{"type":"FSK","symbol_rate":1000},"sync":{"word":"0x2DD4"},"frame":{"length":{"mode":"fixed","bytes":24}},
        "fields":[{"name":"e","offset_bits":0,"width_bits":4,"type":"enum","enum":{"1":"one","0x2":"two"}},
                  {"name":"fl","offset_bits":4,"width_bits":4,"type":"bitflags","flags":{"0":"a","3":"d"}},
                  {"name":"s","offset_bits":8,"width_bits":32,"type":"ascii"},
                  {"name":"b","offset_bits":40,"width_bits":16,"type":"bcd","scale":0.01},
                  {"name":"f","offset_bits":56,"width_bits":32,"type":"float32"},
                  {"name":"la","offset_bits":88,"width_bits":32,"type":"lat","scale":1e-7},
                  {"name":"lo","offset_bits":120,"width_bits":32,"type":"lon","scale":1e-7},
                  {"name":"le","offset_bits":152,"width_bits":16,"type":"uint","endian":"little"},
                  {"name":"h","offset_bits":168,"width_bits":16,"type":"hex"}],
        "display":[{"when":{"field":"e","equals":2},"template":"two: {s} {b}"},{"template":"other {e}"}]})");
    Frame g;
    g.bytes.assign(24, 0);
    g.bytes[0] = 0x29;                               // e = 2, flags = 1001
    std::memcpy(&g.bytes[1], "AB\0\0", 4);
    g.bytes[5] = 0x12; g.bytes[6] = 0x34;            // BCD 1234 x 0.01
    const float fv = -1.5f;
    uint32_t u;
    std::memcpy(&u, &fv, 4);
    for (int i = 0; i < 4; i++) g.bytes[(size_t)(7 + i)] = (uint8_t)(u >> (24 - 8 * i));
    const int32_t la = -338700000, lo = 1512100000;   // 33.87 S, 151.21 E
    for (int i = 0; i < 4; i++) { g.bytes[(size_t)(11 + i)] = (uint8_t)((uint32_t)la >> (24 - 8 * i)); g.bytes[(size_t)(15 + i)] = (uint8_t)((uint32_t)lo >> (24 - 8 * i)); }
    g.bytes[19] = 0x34; g.bytes[20] = 0x12;          // little endian 0x1234
    g.bytes[21] = 0xBE; g.bytes[22] = 0xEF;
    interpretFrame(t, g);
    CHECK(g.fields[0].text == "two");
    CHECKM(g.fields[1].text == "a, d", "%s", g.fields[1].text.c_str());
    CHECKM(g.fields[2].text == "AB", "[%s]", g.fields[2].text.c_str());
    CHECKM(std::fabs(g.fields[3].num - 12.34) < 1e-9, "%f", g.fields[3].num);
    CHECK(g.fields[4].num == -1.5);
    CHECKM(g.fields[5].text == "33.87000 S", "%s", g.fields[5].text.c_str());
    CHECKM(g.fields[6].text == "151.21000 E", "%s", g.fields[6].text.c_str());
    CHECK(g.fields[7].num == 0x1234);
    CHECK(g.fields[8].text == "BEEF");
    CHECKM(g.display == "two: AB 12.34", "%s", g.display.c_str());
    g.bytes[0] = 0x19;
    interpretFrame(t, g);
    CHECK(g.display == "other one");
    // export
    std::vector<Frame> fr{f, f};
    const std::string csv = framesCsv(d, fr);
    CHECK(csv.find("temperature (C)") != std::string::npos && csv.find("-12.3") != std::string::npos);
    int lines = 0;
    for (char c : csv) lines += c == '\n';
    CHECK(lines == 3);
    const std::string js = framesJson(d, fr);
    dect2::automation::json::Value v;
    std::string err;
    CHECKM(dect2::automation::json::parse(js, v, err), "%s", err.c_str());
    const auto* arr = v.get("frames");
    CHECK(arr && arr->a.size() == 2 && arr->a[0].get("fields") && arr->a[0].get("fields")->num("temperature") == -12.3);
}

// ---------------------------------------------------------------- round trips
struct Trip { int sent = 0, got = 0, wrong = 0, extra = 0; };

static Trip roundTrip(const Definition& d, int nFrames, const GenOptions& g, double extraOffset = 0, size_t feedStep = 0) {
    uint32_t rng = g.seed * 2654435761u + 1;
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < nFrames; i++) frames.push_back(randomFrame(d, rng));
    const std::vector<cf> iq = generate(d, frames, g);
    std::vector<Frame> out;
    if (feedStep) {
        Receiver r(d);
        r.configure(g.fs > 0 ? g.fs : d.sampleRate, extraOffset);
        for (size_t i = 0; i < iq.size(); i += feedStep) { r.feed(iq.data() + i, std::min(feedStep, iq.size() - i)); for (Frame& f : r.take()) out.push_back(f); }
        r.flush();
        for (Frame& f : r.take()) out.push_back(f);
    } else out = decodeIq(d, iq, g.fs > 0 ? g.fs : d.sampleRate, extraOffset);
    Trip t;
    t.sent = nFrames;
    size_t k = 0;
    for (const Frame& f : out) {
        if (!f.good()) continue;
        bool matched = false;
        for (size_t j = k; j < frames.size(); j++) if (frames[j] == f.bytes) { matched = true; k = j + 1; t.got++; break; }
        if (!matched) t.extra++;
    }
    return t;
}

static std::string mkDef(const std::string& mod, const std::string& more, int bytes = 16) {
    return R"({"onair_mode":1,"name":"t","sample_rate":96000,"modulation":)" + mod + R"(,"sync":{"word":"0x2DD4E5A9","bits":32,"max_errors":2},)" +
           R"("frame":{"length":{"mode":"fixed","bytes":)" + std::to_string(bytes) + R"(}},"crc":{"preset":"CRC-16/CCITT-FALSE"})" + more + "}";
}

static void expectTrip(const char* what, const Definition& d, const GenOptions& g, int frames = 6, double extra = 0, int allowMiss = 0, size_t step = 0) {
    const Trip t = roundTrip(d, frames, g, extra, step);
    CHECKM(t.got >= t.sent - allowMiss && t.extra == 0, "%s (snr %.0f, cfo %.0f Hz, %.0f ppm): %d of %d frames, %d extra", what, g.snrDb, g.cfoHz, g.clockPpm, t.got, t.sent, t.extra);
}

static void expectFskLeadIn(const Definition& d, const GenOptions& g, double tone, bool prefix = true) {
    uint32_t rng = 71;
    const std::vector<uint8_t> frame = randomFrame(d, rng);
    std::vector<cf> iq = generate(d, {frame}, g);
    const double sps = d.sampleRate / d.symbolRate;
    const size_t end = (size_t)std::lround(g.gapSymbols * sps);
    const size_t start = end - (size_t)std::lround(30 * sps);
    if (prefix) for (size_t i = start; i < end; i++) {
        const double ph = 2 * 3.14159265358979323846 * (tone * d.deviation() + g.cfoHz) * (double)(i - start) / d.sampleRate;
        iq[i] = cf((float)std::cos(ph), (float)std::sin(ph));
    }
    for (size_t step : {iq.size(), (size_t)137}) {
        Receiver r(d);
        r.configure(d.sampleRate);
        for (size_t i = 0; i < iq.size(); i += step) r.feed(iq.data() + i, std::min(step, iq.size() - i));
        r.flush();
        const std::vector<Frame> out = r.take();
        CHECKM(out.size() == 1 && out[0].good() && out[0].bytes == frame && r.stats().syncHits == 1,
            "%s %s lead-in %s %.1f, cfo %.0f Hz, %zu-sample feed: %zu frames, %llu sync hits",
            modName(d.mod), lineName(d.line), prefix ? "on" : "off", tone, g.cfoHz, step,
            out.size(), (unsigned long long)r.stats().syncHits);
    }
}

// A narrow acquisition block at the channel centre must not be mistaken for
// either FSK tone. Use entirely synthetic lead-ins, plus real one-tone controls.
static void testFskAcquisition() {
    struct C { const char* mod; const char* line; };
    const C cases[] = {{"FSK", "NRZ"}, {"FSK", "Manchester"}, {"GFSK", "NRZ"}, {"GFSK", "Manchester"}, {"GMSK", "Manchester"}};
    for (const C& c : cases) {
        Definition d = defOf(mkDef(std::string(R"({"type":")") + c.mod + R"(","symbol_rate":4800})",
            std::string(R"(,"line_code":{"type":")") + c.line + R"("})"));
        d.syncMaxErrors = 0;
        GenOptions g;
        g.snrDb = 200;   // deterministic; existing round trips exercise noise and frequency errors
        expectFskLeadIn(d, g, 0, false);
        expectFskLeadIn(d, g, d.mod == Mod::GFSK ? 0.2 : 0.0);
        expectFskLeadIn(d, g, -1);
        expectFskLeadIn(d, g, 1);
    }
    // An offset real tone must retain the original one-tone estimate. A guard
    // extending to half a deviation broke these GMSK/NRZ starts at +/-0.4.
    for (int sign : {-1, 1}) {
        Definition d = defOf(mkDef(R"({"type":"GMSK","symbol_rate":4800})", ""));
        d.syncMaxErrors = 0;
        GenOptions g;
        g.snrDb = 200;
        g.cfoHz = sign * 0.6 * d.deviation();
        expectFskLeadIn(d, g, -sign);
    }
}

static void testExamplesRoundTrip() {
    for (const Example& e : examples()) {
        const Definition d = defOf(e.text);
        GenOptions g;
        g.snrDb = 25;
        g.cfoHz = d.mod == Mod::PSK ? d.symbolRate * 0.01 : d.symbolRate * 0.05;
        g.clockPpm = 150;
        g.seed = 3;
        expectTrip(e.file.c_str(), d, g);
        if (gSweep) {
            GenOptions h = g;
            h.snrDb = d.mod == Mod::OOK ? 16 : 14;
            h.cfoHz = -g.cfoHz * 2;
            h.clockPpm = -400;
            h.seed = 9;
            expectTrip((e.file + " low SNR").c_str(), d, h, 8, 0, 1);
            // the whole signal off-centre, with the offset given to the receiver; fed in small odd pieces
            GenOptions k = g;
            k.fs = d.sampleRate * 2;
            const double off = d.sampleRate * 0.3;
            Definition d2 = d;
            d2.offsetHz = off;
            const std::vector<std::vector<uint8_t>> none;
            (void)none;
            const Trip t = [&] {
                uint32_t rng = 77;
                std::vector<std::vector<uint8_t>> frames;
                for (int i = 0; i < 5; i++) frames.push_back(randomFrame(d, rng));
                const std::vector<cf> iq = generate(d2, frames, k);   // the signal at +off
                Receiver r(d);
                r.configure(k.fs, off);                               // the receiver told where it is
                std::vector<Frame> out;
                for (size_t i = 0; i < iq.size(); i += 777) { r.feed(iq.data() + i, std::min<size_t>(777, iq.size() - i)); for (Frame& f : r.take()) out.push_back(f); }
                r.flush();
                for (Frame& f : r.take()) out.push_back(f);
                Trip tt;
                tt.sent = 5;
                for (const Frame& f : out) { bool m = false; for (const auto& s : frames) m |= s == f.bytes; if (f.good() && m) tt.got++; else if (f.good()) tt.extra++; }
                return tt;
            }();
            CHECKM(t.got == 5 && t.extra == 0, "%s off-centre at 2x rate: %d of 5, %d extra", e.file.c_str(), t.got, t.extra);
        }
    }
}

static void testModulations() {
    GenOptions g;
    g.snrDb = 24;
    g.clockPpm = 100;
    g.seed = 4;
    struct C { const char* name; std::string mod, more; double cfo; };
    const C cases[] = {
        {"2FSK", R"({"type":"FSK","symbol_rate":4800,"deviation_hz":2400})", "", 300},
        {"4FSK", R"({"type":"FSK","order":4,"symbol_rate":4800,"deviation_hz":3600})", R"(,"preamble":{"pattern":"0x22","bits":32})", 300},
        {"8FSK", R"({"type":"FSK","order":8,"symbol_rate":2400,"deviation_hz":4200})", R"(,"preamble":{"pattern":"0x1C7","bits":36})", 150},
        {"8FSK outer preamble", R"({"type":"FSK","order":8,"symbol_rate":2400,"deviation_hz":4200})", R"(,"preamble":{"pattern":"0x104","bits":36})", 150},
        {"GFSK", R"({"type":"GFSK","symbol_rate":9600,"deviation_hz":4800,"bt":0.5})", "", 500},
        {"GMSK", R"({"type":"GMSK","symbol_rate":4800})", "", 200},
        {"OOK NRZ", R"({"type":"OOK","symbol_rate":2000})", R"(,"preamble":{"pattern":"0xAAAA","bits":32})", 1000},
        {"ASK4", R"({"type":"ASK","order":4,"symbol_rate":2400})", R"(,"preamble":{"pattern":"0x22","bits":32})", 300},
        {"BPSK", R"({"type":"PSK","order":2,"symbol_rate":4800})", "", 30},
        {"QPSK", R"({"type":"PSK","order":4,"symbol_rate":4800})", "", 30},
        {"8PSK", R"({"type":"PSK","order":8,"symbol_rate":2400})", R"(,"preamble":{"pattern":"0x1C7","bits":36})", 10},
        {"DQPSK rrc", R"({"type":"PSK","order":4,"symbol_rate":4800,"differential":true})", R"(,"filter":{"pulse":"rrc","rolloff":0.35,"bandwidth_hz":8000},"preamble":{"pattern":"0xFFFFFFFF"})", 40},
        {"BPSK rc", R"({"type":"PSK","order":2,"symbol_rate":2400})", R"(,"filter":{"pulse":"rc","rolloff":0.5,"bandwidth_hz":5000})", 20},
    };
    for (const C& c : cases) {
        const Definition d = defOf(mkDef(c.mod, c.more));
        GenOptions h = g;
        h.cfoHz = c.cfo;
        if (c.name == std::string("8PSK")) h.snrDb = 28;
        expectTrip(c.name, d, h);
        if (gSweep) {
            h.cfoHz = -c.cfo;
            h.clockPpm = -300;
            h.seed = 21;
            h.phase = 2.4;
            expectTrip((std::string(c.name) + " neg").c_str(), d, h);
        }
    }
    // a symbol map (DMR style +3 = 01, +1 = 00, -1 = 10, -3 = 11 from the top: levels low..high = 11 10 00 01)
    const Definition dm = defOf(mkDef(R"({"type":"FSK","order":4,"symbol_rate":4800,"deviation_hz":1944,"symbol_map":[3,2,0,1]})", R"(,"preamble":{"pattern":"0x77","bits":32})"));
    expectTrip("4FSK symbol map", dm, g);
}

static void testFraming() {
    GenOptions g;
    g.snrDb = 25;
    g.cfoHz = 200;
    g.clockPpm = 100;
    g.seed = 6;
    const std::string fsk = R"({"type":"FSK","symbol_rate":4800,"deviation_hz":2400})";
    struct C { const char* name; std::string more; int bytes; };
    const C cases[] = {
        {"Manchester", R"(,"line_code":{"type":"Manchester"})", 12},
        {"Manchester-Thomas", R"(,"line_code":"Manchester-Thomas")", 12},
        {"Differential Manchester", R"(,"line_code":{"type":"Differential-Manchester"})", 12},
        {"Miller", R"(,"line_code":{"type":"Miller"})", 12},
        {"NRZI", R"(,"line_code":{"type":"NRZI"})", 12},
        {"Differential", R"(,"line_code":{"type":"Differential"})", 12},
        {"4b5b", R"(,"line_code":{"type":"4b5b"})", 12},
        {"8b10b", R"(,"line_code":{"type":"8b10b"},"bit_order":"lsb")", 12},
        {"inverted", R"(,"polarity":"inverted")", 12},
        {"lsb first", R"(,"bit_order":"lsb")", 12},
        {"additive scrambler", R"(,"scrambler":{"type":"additive","poly":"0x90","seed":"0x55"})", 20},
        {"multiplicative scrambler", R"(,"scrambler":{"type":"multiplicative","taps":[17,12]})", 20},
        {"repetition", R"(,"fec":{"type":"repetition","n":3})", 10},
        {"hamming 7,4 interleaved", R"(,"fec":{"type":"hamming","code":"7,4"},"interleaver":{"rows":7,"cols":8})", 14},
        {"conv K7", R"(,"fec":{"type":"convolutional"})", 20},
        {"conv K7 interleaved scrambled", R"(,"fec":{"type":"convolutional"},"interleaver":{"rows":8,"cols":16},"scrambler":{"type":"additive","taps":[9,5]})", 24},
        {"RS 255,223 shortened", R"(,"fec":{"type":"reed_solomon","n":255,"k":223})", 40},
        {"RS 15,11 4-bit", R"(,"fec":{"type":"reed_solomon","n":15,"k":11,"symbol_bits":4,"poly":"0x13","fcr":1})", 16},
    };
    for (const C& c : cases) {
        const Definition d = defOf(mkDef(fsk, c.more, c.bytes));
        expectTrip(c.name, d, g);
    }
    // a length field (length of the payload after it, adjust 3) with Hamming and an interleaver
    const Definition lf = defOf(R"({"onair_mode":1,"name":"lf","sample_rate":96000,"modulation":{"type":"FSK","symbol_rate":4800,"deviation_hz":2400},
        "sync":{"word":"0x2DD4E5A9","max_errors":2},"frame":{"length":{"mode":"length_field","offset_bits":0,"width_bits":8,"adjust":3,"min_bytes":4,"max_bytes":60}},
        "fec":{"type":"hamming","code":"15,11"},"interleaver":{"rows":5,"cols":15},"crc":{"preset":"CRC-16/X-25","byte_order":"little"}})");
    expectTrip("length field", lf, g, 8);
    const Definition lfc = defOf(R"({"onair_mode":1,"name":"lf","sample_rate":96000,"modulation":{"type":"FSK","symbol_rate":4800,"deviation_hz":2400},
        "sync":{"word":"0x2DD4E5A9","max_errors":2},"frame":{"length":{"mode":"length_field","offset_bits":4,"width_bits":4,"multiplier":4,"adjust":2,"min_bytes":2,"max_bytes":62}},
        "fec":{"type":"convolutional"},"crc":{"preset":"CRC-8"}})");
    expectTrip("length field x4, conv", lfc, g, 8);
    // an end flag without stuffing (a 16-bit flag) and HDLC
    const Definition ef = defOf(R"({"onair_mode":1,"name":"ef","sample_rate":96000,"modulation":{"type":"FSK","symbol_rate":4800,"deviation_hz":2400},
        "sync":{"word":"0x2DD4E5A9","max_errors":2},"frame":{"length":{"mode":"end_flag","flag":"0xF00F","min_bytes":3,"max_bytes":40}},"crc":{"preset":"CRC-16/IBM"}})");
    expectTrip("end flag", ef, g, 8);
    // CRC failures: a corrupted frame is not shown, or shown as bad when asked
    const Definition sb = defOf(mkDef(fsk, R"(,"show_bad_frames":true)", 12));
    uint32_t rng = 1;
    std::vector<std::vector<uint8_t>> frames{randomFrame(sb, rng), randomFrame(sb, rng)};
    std::vector<uint8_t> broken = frames[1];
    std::vector<std::vector<uint8_t>> tx{frames[0], broken};
    tx[1][3] ^= 0x10;   // the check value no longer matches
    GenOptions q = g;
    q.snrDb = 30;
    const std::vector<cf> iq = generate(sb, tx, q);
    const std::vector<Frame> out = decodeIq(sb, iq, sb.sampleRate);
    int good = 0, bad = 0;
    for (const Frame& f : out) (f.good() ? good : bad)++;
    CHECKM(good == 1 && bad == 1, "show_bad_frames: %d good %d bad", good, bad);
    Definition hide = sb;
    hide.showBadFrames = false;
    CHECK(decodeIq(hide, iq, sb.sampleRate).size() == 1);
}

static void testNoise() {
    // pure noise: no frames (the sync word with errors and the CRC keep it quiet)
    for (const Example& e : examples()) {
        const Definition d = defOf(e.text);
        std::vector<cf> iq((size_t)(d.sampleRate * (gSweep ? 3 : 1)));
        uint32_t s = 99;
        for (cf& x : iq) {
            const double u1 = std::max(1e-12, (rnd(s) >> 8) / 16777216.0), u2 = (rnd(s) >> 8) / 16777216.0;
            const double r = std::sqrt(-2 * std::log(u1)) * 0.1;
            x = cf((float)(r * std::cos(6.2831853 * u2)), (float)(r * std::sin(6.2831853 * u2)));
        }
        int n = 0;
        for (const Frame& f : decodeIq(d, iq, d.sampleRate)) n += f.good();
        CHECKM(n == 0, "%s: %d frames from noise", e.file.c_str(), n);
    }
    // nothing at all
    const Definition d = defOf(examples().front().text);
    CHECK(decodeIq(d, {}, d.sampleRate).empty());
    std::vector<cf> zeros(10000);
    CHECK(decodeIq(d, zeros, d.sampleRate).empty());
}

static void testStreaming() {
    // the same frames whether the samples come in one block or in small pieces
    for (const Example& e : examples()) {
        const Definition d = defOf(e.text);
        GenOptions g;
        g.snrDb = 25;
        g.seed = 12;
        expectTrip((e.file + " in 333-sample pieces").c_str(), d, g, 4, 0, 0, 333);
    }
}

int main(int argc, char** argv) {
    const std::string mode = argc >= 2 ? argv[1] : "quick";
    gSweep = mode == "sweep" || mode == "all";
    auto timed = [&](const char* n, void (*f)()) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        printf("%s: %.2f s\n", n, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    };
    timed("crc", testCrc);
    timed("line codes", testLineCodes);
    timed("scrambler / interleaver", testScramblerInterleaver);
    timed("fec", testFec);
    timed("parse", testParse);
    timed("fields", testFields);
    timed("examples", testExamplesRoundTrip);
    timed("modulations", testModulations);
    timed("FSK acquisition", testFskAcquisition);
    timed("framing", testFraming);
    timed("noise", testNoise);
    if (gSweep) timed("streaming", testStreaming);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
