// P25 Phase 2 protocol layer: ISCH (sync word and the (40,9) I-ISCH), the (8,4) DUID, the scrambling sequence (against XMC_SDR's), the AMBE+2 frame
// places, the ESS (RS(44,16)), and the MAC PDU codewords (Reed-Solomon + CRC) with errors and their builders. The burst layer follows the on-air layout
// (p25p2_proto.h); the MAC PDU codewords are this program's own: for them the test proves the encoder and the decoder agree, nothing more.
#include "dect2/p25p2/p25p2_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    std::mt19937 rng(5);
    // ISCH
    {
        uint8_t d[20];
        p25p2::ischSync(d);
        CHECK(p25p2::syncDistance(d) == 0 && p25p2::ischValue(d) == p25p2::kSyncWord, "sync word");
        std::set<uint64_t> words;
        for (int v = 0; v < 128; v++) {
            p25p2::ischInfo(v, d);
            words.insert(p25p2::ischValue(d));
            int e = -1;
            CHECK(p25p2::ischDecode(p25p2::ischValue(d), &e) == v && e == 0, "I-ISCH %d", v);
            CHECK(p25p2::syncDistance(d) > 10, "I-ISCH %d looks like the sync word (%d)", v, p25p2::syncDistance(d));
            uint64_t w = p25p2::ischWord(v);
            std::set<int> flipped;
            while ((int)flipped.size() < 7) flipped.insert((int)(rng() % 40));
            for (int b : flipped) w ^= 1ULL << b;
            CHECK(p25p2::ischDecode(w, &e) == v && e == 7, "I-ISCH %d with seven bit errors", v);
        }
        CHECK(words.size() == 128, "the I-ISCH words are not all different");
        CHECK(p25p2::ischDecode(p25p2::kSyncWord) == -1, "the sync word decodes as an I-ISCH");
        CHECK(p25p2::ischInfoOf(1, 2, 0, 3) == 51 && p25p2::ischInfoOf(0, 1, 0, 0) == 8, "I-ISCH fields");
        // the logical channel of each slot: slot parity, swapped in slots 10 and 11
        const int want[12] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0};
        for (int s = 0; s < 12; s++) CHECK(p25p2::slotChannel(s) == want[s], "channel of slot %d", s);
    }
    // DUID
    {
        uint8_t b[180] = {};
        for (int duid = 0; duid < 16; duid++) {
            p25p2::duidPut(duid, b);
            CHECK(p25p2::duidGet(b) == duid && (p25p2::duidWord(duid) >> 4) == duid, "duid %d", duid);
            for (int bit = 0; bit < 8; bit++) {
                uint8_t e[180];
                std::copy_n(b, 180, e);
                e[p25p2::kDuidPos[bit / 2]] ^= (uint8_t)(bit & 1 ? 1 : 2);
                CHECK(p25p2::duidGet(e) == duid, "duid %d with bit %d wrong", duid, bit);
            }
        }
        CHECK(p25p2::duidWord(p25p2::kDuid4V) == 0x00 && p25p2::duidWord(p25p2::kDuid2V) == 0x65 && p25p2::duidWord(p25p2::kDuidSacch) == 0x39 &&
                  p25p2::duidWord(p25p2::kDuidFacch) == 0x9A,
              "the DUID code words heard on the air");
    }
    // scrambling: the sequence of XMC_SDR's InitialiseDescrambler for WACN BEE00, system 2D1, colour code 2D1 (the NSW PSN)
    {
        uint8_t seq[p25p2::kScrambleDibits];
        p25p2::scrambleSeq(0xBEE00, 0x2D1, 0x2D1, seq);
        const char* head = "00000000002332320000023101023101232102230022203012122321213202121201322031002330";
        const char* tail = "11120110303310123033122021310212001311323333203313033111121210301131000021021322";
        bool okH = true, okT = true;
        for (int i = 0; i < 80; i++) { okH &= seq[i] == head[i] - '0'; okT &= seq[2090 + i] == tail[i] - '0'; }
        CHECK(okH && okT, "scrambling sequence differs from XMC_SDR's (head %d tail %d)", okH, okT);
        p25p2::scrambleSeq(0, 0, 0, seq);
        bool zero = true;
        for (uint8_t v : seq) zero &= v == 0;
        CHECK(zero, "an all-zero seed must give an all-zero sequence");
    }
    // AMBE+2 frames in a burst: every bit lands in its own place, the DUID dibits and the ESS-B are not touched
    {
        uint8_t b[180];
        for (auto& v : b) v = 0;
        p25p2::duidPut(p25p2::kDuid4V, b);
        uint8_t bits[4][72];
        for (int k = 0; k < 4; k++) {
            for (int i = 0; i < 72; i++) bits[k][i] = (uint8_t)(rng() & 1);
            char fr[4][24];
            p25p2::ambeUnflatten(bits[k], fr);
            p25p2::ambePut(fr, p25p2::kVoiceOff[k], b);
        }
        CHECK(p25p2::duidGet(b) == p25p2::kDuid4V, "voice frames overwrote the DUID");
        bool essClear = true;
        for (int i = 0; i < p25p2::kEssBDibits; i++) essClear &= b[p25p2::kEssBPos + i] == 0;
        CHECK(essClear, "voice frames overwrote the ESS-B");
        for (int k = 0; k < 4; k++) {
            char fr[4][24];
            uint8_t out[72];
            p25p2::ambeGet(b, p25p2::kVoiceOff[k], fr);
            p25p2::ambeFlatten(fr, out);
            CHECK(std::memcmp(out, bits[k], 72) == 0, "voice frame %d round trip", k);
        }
    }
    // ESS: RS(44,16) corrects up to 14 hexbits
    {
        int ok = 0, okErr = 0;
        for (int it = 0; it < 100; it++) {
            const int alg = it % 2 ? 0x84 : 0x80, key = (int)(rng() & 0xFFFF);
            uint8_t mi[9], b[16], a[28];
            for (auto& v : mi) v = (uint8_t)rng();
            p25p2::essEncode(alg, key, mi, b, a);
            uint8_t b2[16], mi2[9];
            std::copy_n(b, 16, b2);
            int alg2 = 0, key2 = 0, fixed = -1;
            if (p25p2::essDecode(b2, a, alg2, key2, mi2, &fixed) && alg2 == alg && key2 == key && std::memcmp(mi, mi2, 9) == 0 && fixed == 0) ok++;
            std::copy_n(b, 16, b2);
            uint8_t a2[28];
            std::copy_n(a, 28, a2);
            std::set<int> pos;
            while (pos.size() < 14) pos.insert((int)(rng() % 44));
            for (int p : pos) { uint8_t& s = p < 16 ? b2[p] : a2[p - 16]; s ^= (uint8_t)(1 + rng() % 63); }
            if (p25p2::essDecode(b2, a2, alg2, key2, mi2, &fixed) && alg2 == alg && key2 == key && std::memcmp(mi, mi2, 9) == 0) okErr++;
        }
        CHECK(ok == 100, "ESS clean %d/100", ok);
        CHECK(okErr == 100, "ESS with 14 hexbit errors %d/100", okErr);
        uint8_t mi[9] = {}, b[16], a[28];
        p25p2::essEncode(0, 0, mi, b, a);
        int alg = -1, key = -1;
        CHECK(p25p2::essDecode(b, a, alg, key, mi) && alg == p25p2::kAlgClear, "algorithm 0 is reported as clear");
        uint8_t hex[16], dib[48], back[16];
        for (auto& v : hex) v = (uint8_t)(rng() & 63);
        p25p2::hexbitsPut(hex, 16, dib);
        p25p2::hexbitsGet(dib, 16, back);
        CHECK(std::memcmp(hex, back, 16) == 0, "hexbits round trip");
    }
    // MAC PDU codewords
    for (int facch = 0; facch < 2; facch++) {
        const int nb = facch ? p25p2::kFacchMacBytes : p25p2::kSacchMacBytes, nd = facch ? p25p2::kFacchDibits : p25p2::kSacchDibits;
        const int t = facch ? 11 : 10;   // hexbit errors the code corrects
        int okClean = 0, okErr = 0, rejected = 0, badAccepted = 0;
        for (int it = 0; it < 200; it++) {
            std::vector<uint8_t> mac((size_t)nb);
            for (auto& b : mac) b = (uint8_t)rng();
            std::vector<uint8_t> cw((size_t)nd);
            p25p2::macEncode(facch, mac.data(), cw.data());
            std::vector<uint8_t> out((size_t)nb);
            int fixed = 0;
            if (p25p2::macDecode(facch, cw.data(), out.data(), &fixed) && out == mac && fixed == 0) okClean++;
            // t hexbit errors (each hexbit is three dibits: damage one of them)
            std::vector<uint8_t> bad = cw;
            std::vector<int> pos;
            while ((int)pos.size() < t) { const int p = (int)(rng() % (unsigned)(nd / 3)); bool dup = false; for (int q : pos) dup |= q == p; if (!dup) pos.push_back(p); }
            for (int p : pos) bad[(size_t)(p * 3 + (int)(rng() % 3))] ^= (uint8_t)(1 + rng() % 3);
            if (p25p2::macDecode(facch, bad.data(), out.data(), &fixed) && out == mac && fixed == t) okErr++;
            // far too many errors: must not come out as a valid PDU
            std::vector<uint8_t> junk = cw;
            for (int p = 0; p < nd / 3 - 4; p++) junk[(size_t)(p * 3)] ^= (uint8_t)(1 + rng() % 3);
            if (p25p2::macDecode(facch, junk.data(), out.data())) { if (out != mac) badAccepted++; } else rejected++;
        }
        CHECK(okClean == 200, "%s clean %d/200", facch ? "FACCH" : "SACCH", okClean);
        CHECK(okErr == 200, "%s with %d hexbit errors %d/200", facch ? "FACCH" : "SACCH", t, okErr);
        CHECK(badAccepted == 0, "%s: %d damaged PDUs accepted with wrong content", facch ? "FACCH" : "SACCH", badAccepted);
        CHECK(rejected > 190, "%s: only %d of 200 hopeless PDUs rejected", facch ? "FACCH" : "SACCH", rejected);
    }
    // the codeword in the burst: around the DUID dibits
    {
        uint8_t b[180] = {}, cw[156], back[156];
        for (auto& v : cw) v = (uint8_t)(rng() & 3);
        p25p2::duidPut(p25p2::kDuidFacch, b);
        p25p2::payloadPut(cw, 156, b);
        p25p2::payloadGet(b, back);
        CHECK(std::memcmp(cw, back, 156) == 0 && p25p2::duidGet(b) == p25p2::kDuidFacch, "payload round trip");
    }
    // builders and field offsets
    {
        auto p = p25p2::macPtt(20, 0xABCDEF, 0x1234, 0x84);
        CHECK(p.size() == 20 && p25p2::macHeader(p[0]).opcode == p25p2::kMacPtt, "ptt header");
        CHECK(p25p2::be24(&p[p25p2::kPttSrcOff]) == 0xABCDEFu && p25p2::be16(&p[p25p2::kPttGrpOff]) == 0x1234u && p[p25p2::kPttAlgOff] == 0x84, "ptt fields");
        bool miZero = true;
        for (int i = p25p2::kPttMiOff; i < p25p2::kPttAlgOff; i++) miZero &= p[(size_t)i] == 0;
        CHECK(miZero, "the builders never write a message indicator");
        auto g = p25p2::msgGrpVGrant(0xC0, 0x1005, 777, 424242);
        CHECK(g.size() == 9 && (int)g.size() == p25p2::macMsgLen(g[0]), "grant length");
        const uint64_t a = p25p2::be64(&g[1]);
        CHECK(((a >> 24) & 0xFFFF) == 777 && (a & 0xFFFFFF) == 424242 && ((a >> 40) & 0xFFFF) == 0x1005 && (a >> 56) == 0xC0, "grant fields");
        for (int op : {0x01, 0x02, 0x40, 0x42, 0x44, 0x74, 0x7A, 0x7B, 0x7C, 0x7D}) CHECK(p25p2::macMsgLen(op) > 0 && std::strcmp(p25p2::macMsgName(op), "unknown") != 0, "message 0x%02X", op);
        CHECK(p25p2::macMsgLen(0x55) == 0, "unknown message has a length");
    }
    return testSummary("test_p25p2_proto");
}
