// DMR bursts and messages: bursts that were captured off the air (MMDVM frames of the ok-dmrlib test suite, https://github.com/OK-DMR/ok-dmrlib)
// are taken apart with the burst layout of the specification and put together again bit for bit; then every message type is built and read back.
#include "dect2/dmr_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace dect2::dmr;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) v.push_back((uint8_t)strtoul(std::string(s + i, 2).c_str(), nullptr, 16));
    return v;
}
static Bits bitsOfHex(const char* s) {
    const auto v = hex(s);
    Bits b;
    bytesToBits(v.data(), v.size(), b);
    return b;
}
static uint32_t id24(const uint8_t* p) { return be24(p); }

static void testSyncPatterns() {
    int minDiff = 99;
    for (int a = 0; a < kSyncCount; a++) {
        int8_t sy[24];
        syncSymbols(a, sy);
        for (int i = 0; i < 24; i++) CHECK(sy[i] == 3 || sy[i] == -3, "sync %d symbol %d is not an outer level", a, i);
        for (int b = a + 1; b < kSyncCount; b++) {
            int8_t sb[24];
            syncSymbols(b, sb);
            int d = 0;
            for (int i = 0; i < 24; i++) if (sy[i] != sb[i]) d++;
            minDiff = std::min(minDiff, d);
        }
        Bits c;
        putBits(c, kSyncWords[a], 48);
        int diff = 99;
        CHECK(matchSync(c, 0, &diff) == a && diff == 0, "sync %d does not match itself", a);
    }
    CHECK(minDiff >= 10, "two sync patterns differ in only %d symbols", minDiff);
    // voice and data patterns of a family are the symbol-wise complement of each other (the note under table 9.2)
    const int pairs[4][2] = {{kSyncBsVoice, kSyncBsData}, {kSyncMsVoice, kSyncMsData}, {kSyncDm1Voice, kSyncDm1Data}, {kSyncDm2Voice, kSyncDm2Data}};
    for (auto& p : pairs) {
        int8_t a[24], b[24];
        syncSymbols(p[0], a); syncSymbols(p[1], b);
        for (int i = 0; i < 24; i++) CHECK(a[i] == -b[i], "pair %d/%d is not complementary at %d", p[0], p[1], i);
    }
}

struct Real { const char* name; const char* burst; };

static void testCapturedDataBursts() {
    // two base station control bursts: preamble CSBK before a data message
    {
        const Bits b = bitsOfHex("53df0a83b7a8282c2509625014fdff57d75df5dcadde429028c87ae3341e24191c");
        CHECK(b.size() == 264, "burst length");
        int diff = 99;
        CHECK(matchSync(burstCentre(b), 0, &diff) == kSyncBsData, "sync of the captured CSBK burst");
        const SlotTypeInfo st = burstSlotType(b);
        CHECK(st.ok && st.cc == 5 && st.dt == kDtCsbk && st.errors == 0, "slot type: cc %d dt %d", st.cc, st.dt);
        Bits pay, info;
        burstDataPayload(b, pay);
        CHECK(bptc196Decode(pay, info) == 0, "BPTC of the captured CSBK");
        Csbk c;
        CHECK(csbkParse(info, c), "CSBK CRC");
        CHECK(c.opcode == 0x3D && c.fid == 0 && c.lb, "CSBK opcode %02X fid %d", c.opcode, c.fid);
        CHECK(c.data[0] == 0x80 && c.data[1] == 29, "preamble flags %02X blocks to follow %d (data content follows, 29 blocks)", c.data[0], c.data[1]);
        CHECK(id24(c.data + 2) == 2308195 && id24(c.data + 5) == 2308155, "preamble target %u source %u", id24(c.data + 2), id24(c.data + 5));
        // putting the burst together again must give the captured bits
        Csbk c2 = c;
        Bits info2, pay2;
        csbkInfo(c2, info2);
        bptc196Encode(info2, pay2);
        const Bits again = makeDataBurst(st.cc, st.dt, pay2, kSyncBsData);
        CHECK(again == b, "re-assembled CSBK burst differs from the captured one");
    }
    {
        const Bits b = bitsOfHex("51cf0ded894c0dec1ff8fcf294fdff57d75df5dcae7a16d064197982bf5824914c");
        const SlotTypeInfo st = burstSlotType(b);
        Bits pay, info;
        burstDataPayload(b, pay);
        Csbk c;
        CHECK(st.ok && st.dt == kDtCsbk && bptc196Decode(pay, info) >= 0 && csbkParse(info, c) && c.opcode == 0x3D && c.data[1] == 18, "second captured preamble");
        CHECK(id24(c.data + 2) == 2301 && id24(c.data + 5) == 2308094, "second preamble target %u source %u", id24(c.data + 2), id24(c.data + 5));
    }
    // an MS sourced rate 1/2 data block and a PI header
    {
        const Bits b = bitsOfHex("117b3090722540f9233581a285ed5d7f77fd75709464602846c3022109c3050079");
        CHECK(matchSync(burstCentre(b), 0) == kSyncMsData, "sync of the captured rate 1/2 burst");
        const SlotTypeInfo st = burstSlotType(b);
        Bits pay, info;
        burstDataPayload(b, pay);
        CHECK(st.ok && st.cc == 1 && st.dt == kDtRate12, "rate 1/2 slot type cc %d dt %d", st.cc, st.dt);
        CHECK(bptc196Decode(pay, info) == 0, "BPTC of the rate 1/2 block");
        uint8_t o[12];
        bitsToBytes(info, 0, 96, o);
        const auto want = hex("000501737311000100040a23");
        CHECK(!memcmp(o, want.data(), 12), "rate 1/2 block contents");
        DataBlock blk;
        CHECK(dataBlockParse(kDtRate12, o, false, blk) && blk.bytes.size() == 12, "block parse");
        Bits pay2;
        dataBlockEncode(kDtRate12, o, 12, false, 0, pay2);
        CHECK(makeDataBurst(1, kDtRate12, pay2, kSyncMsData) == b, "re-assembled rate 1/2 burst differs");
    }
    {
        const Bits b = bitsOfHex("167b90897c009bb941434301840d5d7f77fd757d9d6b51e02230cac7011f149419");
        const SlotTypeInfo st = burstSlotType(b);
        Bits pay, info;
        burstDataPayload(b, pay);
        uint8_t d[10];
        CHECK(st.ok && st.cc == 1 && st.dt == kDtPiHeader && bptc196Decode(pay, info) == 0 && crcBlockCheck(info, kMaskPi, d), "PI header");
        const auto want = hex("211003d537d57a000009");
        CHECK(!memcmp(d, want.data(), 10), "PI header contents");
    }
}

static void testCapturedVoice() {
    // three MS sourced voice bursts with the voice sync, and the five bursts B..F of a superframe with embedded signalling
    const char* vs[3] = {"aded847205ae0062959308849047f7d5dd57dfd9537a101efe3ed4206e153827e7", "eab8e5564609e61dc5eaa8e55647f7d5dd57dfd709e61dd5eaa8e5564709e73cc5",
                         "d8bac5704529d00ed0c8aac57047f7d5dd57dfd529d00fd1c8aac570452bd11fd1"};
    for (int i = 0; i < 3; i++) {
        const Bits b = bitsOfHex(vs[i]);
        CHECK(matchSync(burstCentre(b), 0) == kSyncMsVoice, "voice burst %d sync", i);
        Bits v;
        burstVoicePayload(b, v);
        CHECK(v.size() == 216, "voice payload length");
        CHECK(makeVoiceBurst(v, kSyncMsVoice, 0, 0, 0, Bits()) == b, "re-assembled voice burst %d differs", i);
    }
    const char* emb[5] = {"78f8e0361b6519cdd55ad9c3301130a00030a91b7529dee349fbe3147e040bc9d1", "c762a2114c736c7a45f562c133617170a06057439c9df11e936ec26335ecf569bf",
                          "f30c872376d6102d4791df85442170c112200747b289e11dd5c2877046b1e36bcf", "e1e48370246e951422bda7c73511505223f3a07309cda701bdb6e4733318ef9122",
                          "d5098044132a3761cbc708807701100000000e211a1324cbacb5c675371ddee013"};
    const int lcssWant[5] = {1, 3, 3, 2, 0};
    Bits frag[4];
    for (int i = 0; i < 5; i++) {
        const Bits b = bitsOfHex(emb[i]);
        const EmbInfo e = burstEmb(b);
        CHECK(e.ok && e.cc == 1 && e.pi == 0 && e.lcss == lcssWant[i] && e.errors == 0, "EMB of burst %d: cc %d lcss %d", i, e.cc, e.lcss);
        CHECK(matchSync(burstCentre(b), 6) < 0, "an embedded burst looks like a sync");
        Bits v;
        burstVoicePayload(b, v);
        CHECK(makeVoiceBurst(v, -1, e.cc, e.pi, e.lcss, e.emb32) == b, "re-assembled embedded burst %d differs", i);
        if (i < 4) frag[i] = e.emb32;
    }
    uint8_t lc[9];
    CHECK(embLcDecode(frag, lc) == 0, "embedded LC of the captured call");
    FullLc f;
    parseFullLc(lc, f);
    CHECK(f.flco == kFlcoGroupVoice && f.fid == 0 && f.dst == 2149 && f.src == 2145016, "embedded LC: flco %d dst %u src %u", f.flco, f.dst, f.src);
}

static void testTalkerAlias() {
    // four embedded LC messages of a captured call: header and three blocks (the last octet is the checksum byte, not part of the LC)
    const char* pdus[4] = {"0400da00520034005748", "050000420050002000e0", "060044006d0069007408", "070000720069006900a8"};
    TalkerAlias ta;
    for (int i = 0; i < 4; i++) {
        const auto v = hex(pdus[i]);
        FullLc lc;
        parseFullLc(v.data(), lc);
        ta.add(lc);
        CHECK(checksum5(v.data()) == (v[9] >> 3), "talker alias checksum %d", i);
        CHECK(i == 3 ? ta.complete() : !ta.complete(), "talker alias complete flag after %d pdus", i + 1);
    }
    CHECK(ta.text() == "R4WBP Dmitrii", "talker alias text '%s'", ta.text().c_str());
    // and ours
    FullLc out[4];
    const int n = talkerAliasPdus("Test Station 1", 1, out);
    TalkerAlias t2;
    for (int i = 0; i < n; i++) t2.add(out[i]);
    CHECK(t2.complete() && t2.text() == "Test Station 1", "own talker alias round trip '%s'", t2.text().c_str());
    const int n2 = talkerAliasPdus("OnAir", 3, out);
    TalkerAlias t3;
    for (int i = 0; i < n2; i++) t3.add(out[i]);
    CHECK(t3.complete() && t3.text() == "OnAir", "UTF-16 alias '%s'", t3.text().c_str());
}

static void testHeaders() {
    struct H { const char* hex; int dpf; int sap; bool grp; bool resp; uint32_t dst, src; int blocks; bool fmf; int dd; };
    const H v[] = {
        {"8DA300000100000101002B97", kDpfShortDefined, 10, true, false, 1, 1, 3, true, 0},
        {"4DA123386323383B05005757", kDpfShortDefined, 10, false, true, 2308195, 2308155, 1, true, 1},
        {"023A2337FC2337FE820081A3", kDpfUnconfirmed, 3, false, false, 2308092, 2308094, 2, true, 0},
        {"434E2337FE2337FC84781BD1", kDpfConfirmed, 4, false, true, 2308094, 2308092, 4, true, 0},
        {"01402337FC2337FE000FF83A", kDpfResponse, 4, false, false, 2308092, 2308094, 0, false, 0}};
    for (auto& t : v) {
        const auto b = hex(t.hex);
        DataHeader h;
        CHECK(parseDataHeader(b.data(), h), "header %s", t.hex);
        CHECK(h.dpf == t.dpf && h.sap == t.sap && h.group == t.grp && h.resp == t.resp && h.dst == t.dst && h.src == t.src && h.blocks == t.blocks && h.fmf == t.fmf,
              "header %s: dpf %d sap %d dst %u src %u blocks %d", t.hex, h.dpf, h.sap, h.dst, h.src, h.blocks);
        if (t.dpf == kDpfShortDefined) CHECK(h.dd == t.dd, "header %s: DD %d", t.hex, h.dd);
        if (t.dpf != kDpfResponse) {
            uint8_t o[10];
            packDataHeader(h, o);
            CHECK(!memcmp(o, b.data(), 10), "header %s does not pack back to the same octets", t.hex);
        }
    }
}

static void testMessages() {
    std::mt19937 rng(21);
    // voice header and terminator: Reed-Solomon protected link control
    for (int term = 0; term < 2; term++) {
        FullLc lc;
        lc.flco = term ? kFlcoUnitVoice : kFlcoGroupVoice; lc.fid = 0; lc.svc = 0x20 | term; lc.dst = 2149 + (unsigned)term; lc.src = 2145016;
        uint8_t raw[9], back[9];
        packFullLc(lc, raw);
        Bits info, pay, info2;
        lcBurstInfo(raw, term == 1, info);
        bptc196Encode(info, pay);
        CHECK(bptc196Decode(pay, info2) == 0, "LC burst BPTC");
        CHECK(lcBurstDecode(info2, term == 1, back) == 0 && !memcmp(raw, back, 9), "LC burst Reed-Solomon (terminator %d)", term);
        CHECK(lcBurstDecode(info2, term == 0, back) != 0, "a header must not pass as a terminator");
        info2[10] ^= 1; info2[12] ^= 1;      // two bits of the same octet
        CHECK(lcBurstDecode(info2, term == 1, back) == 1 && !memcmp(raw, back, 9), "one wrong LC octet corrected");
    }
    // all data message formats: build the header and blocks, take them apart again
    const int rates[3] = {kDtRate12, kDtRate34, kDtRate1};
    for (int ri = 0; ri < 3; ri++)
        for (int conf = 0; conf < 2; conf++) {
            const int dt = rates[ri], bb = dataBlockBytes(dt, conf != 0);
            uint8_t user[24];
            for (auto& x : user) x = (uint8_t)rng();
            Bits pay;
            dataBlockEncode(dt, user, (size_t)bb, conf != 0, 5, pay);
            DataBlock blk;
            uint8_t oct[24];
            if (dt == kDtRate34) {
                float sym[98];
                for (int k = 0; k < 98; k++) sym[k] = (float)dibitToSymbol((unsigned)((pay[2 * k] << 1) | pay[2 * k + 1]));
                CHECK(trellis34Decode(sym, oct) == 0, "trellis block");
            } else if (dt == kDtRate1) {
                rate1Decode(pay, oct);
            } else {
                Bits info;
                CHECK(bptc196Decode(pay, info) == 0, "BPTC block");
                bitsToBytes(info, 0, 96, oct);
            }
            CHECK(dataBlockParse(dt, oct, conf != 0, blk), "block parse");
            CHECK(blk.crcOk && (int)blk.bytes.size() == bb && !memcmp(blk.bytes.data(), user, (size_t)bb) && (!conf || blk.dbsn == 5),
                  "data block dt %d confirmed %d: crc %d bytes %zu dbsn %u", dt, conf, blk.crcOk, blk.bytes.size(), blk.dbsn);
            if (conf) {
                oct[3] ^= 0x10;
                dataBlockParse(dt, oct, true, blk);
                CHECK(!blk.crcOk, "a wrong octet passes the CRC-9");
            }
        }
    // short data text: ISO 8859-1, UTF-8, UTF-16
    {
        const uint8_t latin[] = {'C', 'a', 'f', 0xE9, ' ', '1', '2', '3'};
        CHECK(textFromBytes(latin, sizeof latin, kDdIso8859_1, false) == "Caf\xC3\xA9 123", "ISO 8859-1 text");
        const uint8_t u8[] = {'o', 'k', ' ', 0xE2, 0x82, 0xAC};
        CHECK(textFromBytes(u8, sizeof u8, kDdUtf8, false) == "ok \xE2\x82\xAC", "UTF-8 text");
        const uint8_t u16[] = {'H', 0, 'i', 0, 0, 0};
        CHECK(textFromBytes(u16, sizeof u16, kDdUtf16Le, false) == "Hi", "UTF-16LE text");
        const uint8_t bcd[] = {0x12, 0x34};
        CHECK(textFromBytes(bcd, 2, 1, false) == "1234", "BCD text");
    }
}

// Random input to every parser: nothing may crash or read outside its buffers (the point of running this under the address sanitiser), and what
// the decoders accept must come out of the encoders again.
static void testRandomInput() {
    std::mt19937 rng(77);
    int accepted = 0, headerFaults = 0;
    for (int it = 0; it < 20000; it++) {
        uint8_t b[64];
        for (auto& x : b) x = (uint8_t)rng();
        DataHeader h;
        if (parseDataHeader(b, h)) {
            accepted++;
            // only the four packet formats the test signal makes can be packed again
            if (h.dpf == kDpfUnconfirmed || h.dpf == kDpfConfirmed || h.dpf == kDpfShortDefined || h.dpf == kDpfShortRaw) {
                uint8_t o[10];
                packDataHeader(h, o);
                DataHeader h2;
                if (!(parseDataHeader(o, h2) && h2.dpf == h.dpf && h2.dst == h.dst && h2.src == h.src && h2.blocks == h.blocks && h2.sap == h.sap)) {
                    if (++headerFaults <= 3) printf("FAIL: a parsed data header (format %d) does not survive packing\n", h.dpf);
                }
            }
        }
        const size_t n = rng() % 60;
        for (int f = 0; f < 26; f++) (void)textFromBytes(b, n, f, false);
        for (int f = 0; f < 4; f++) (void)textFromBytes(b, n, f, true);
        TalkerAlias ta;
        for (int k = 0; k < 4; k++) {
            FullLc lc;
            uint8_t raw[9];
            for (auto& x : raw) x = (uint8_t)rng();
            raw[0] = (uint8_t)(4 + rng() % 4);
            parseFullLc(raw, lc);
            ta.add(lc);
        }
        (void)ta.text();
        DataBlock blk;
        for (int dt : {kDtRate12, kDtRate34, kDtRate1}) for (int c = 0; c < 2; c++) dataBlockParse(dt, b, c != 0, blk);
        Bits frag[4];
        for (auto& f : frag) { f.resize(32); for (auto& x : f) x = rng() & 1; }
        uint8_t lc[9];
        (void)embLcDecode(frag, lc);
        Bits sl[4];
        for (auto& f : sl) { f.resize(17); for (auto& x : f) x = rng() & 1; }
        uint32_t v;
        (void)shortLcDecode(sl, v);
        Bits tx(196), info;
        for (auto& x : tx) x = rng() & 1;
        if (bptc196Decode(tx, info) >= 0) {
            Bits again;
            bptc196Encode(info, again);
            Bits info2;
            CHECK(bptc196Decode(again, info2) == 0 && info2 == info, "a decoded BPTC block does not re-encode to a clean block");
        }
        uint8_t d[24];
        bool pad;
        rate1Decode(tx, d, &pad);
        float sym[98];
        for (auto& x : sym) x = (float)((int)(rng() % 9) - 4) + (float)(rng() % 100) / 100.f;
        uint8_t t18[18];
        (void)trellis34Decode(sym, t18);
        uint8_t w[12];
        memcpy(w, b, 12);
        (void)rs129Correct(w);
        Bits i96(96);
        for (auto& x : i96) x = rng() & 1;
        (void)lcBurstDecode(i96, (it & 1) != 0, lc);
        Csbk c;
        (void)csbkParse(i96, c);
        Bits burst(264);
        for (auto& x : burst) x = rng() & 1;
        (void)burstSlotType(burst);
        (void)burstEmb(burst);
        (void)matchSync(burstCentre(burst), 5);
        int at, tc, ls;
        unsigned pay;
        (void)cachDecode(Bits(burst.begin(), burst.begin() + 24), at, tc, ls, pay);
        (void)csbkName(b[0], b[1]);
        (void)serviceOptionsText(b[2]);
    }
    CHECK(headerFaults == 0, "%d parsed data headers did not survive packing", headerFaults);
    CHECK(accepted > 1000, "only %d of 20000 random data headers parsed", accepted);
}

int main() {
    testRandomInput();
    testSyncPatterns();
    testCapturedDataBursts();
    testCapturedVoice();
    testTalkerAlias();
    testHeaders();
    testMessages();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
