// Mercury frames: callsign code, CALL / ACCEPT / CQ / DISCONNECT, the session seed, the carousel's control and data frames, the
// Reed-Solomon erasure code, broadcast messages and the RaptorQ file carousel (RFC 6330: every systematic index of the table up to
// K' = 1002 gives a solvable precode, which a wrong table entry or random generator would break), the bundle.
#include "dect2/mercury/mercury_proto.h"
#include "dect2/mercury/mercury_raptorq.h"
#include "dect2/mercury/mercury_phy.h"
#include "dect2/common/crc.h"
#include "dect2/dsp/portable_rng.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::mercury;
#include "../common/test_util.h"

int main() {
    PortableRng rng(42);
    // callsigns: the arithmetic code round-trips, fits the 10 bytes of a CALL up to 14 characters, rejects what it cannot code
    {
        const char* calls[] = {"LY2ONA", "PU2UIT", "K7EK", "DL1ABC-12", "ESTACAO2", "A", "Q0123456789A"};
        for (const char* c : calls) {
            const std::vector<uint8_t> e = encodeCallsign(c);
            const std::string d = decodeCallsign(e.data(), e.size());
            CHECK(d == c, "callsign %s -> %zu bytes -> %s", c, e.size(), d.c_str());
            std::vector<uint8_t> padded = e;
            padded.resize(10, 0);
            CHECK(decodeCallsign(padded.data(), padded.size()) == c, "%s zero padded", c);
            CHECK(e.size() <= 10, "%s: %zu bytes", c, e.size());
        }
        CHECK(encodeCallsign("LY2*ONA").empty(), "a character outside the alphabet");
        CHECK(callsignCrc("ly2ona") == callsignCrc("LY2ONA"), "the CRC is of the upper case callsign");
        CHECK(callsignCrc("123456789") == 0x29B1, "CRC-16/CCITT-FALSE check value");
    }
    // connect frames
    {
        const std::vector<uint8_t> call = buildCall(0x21, "LY2ONA", "LY1HRM", 2300);
        CallFrame c;
        CHECK(call.size() == 14 && parseCall(call.data(), call.size(), c), "CALL");
        CHECK(c.sessionId == 0x21 && c.caller == "LY2ONA" && c.bwHz == 2300 && c.calledCrc == callsignCrc("LY1HRM"), "CALL fields");
        CHECK(packetType(call[0]) == kPtCall2 && (call[1] & 0x80) == 0, "CALL framer");
        AcceptFrame a;
        CHECK(!parseAccept(call.data(), call.size(), a), "a CALL is not an ACCEPT");
        const std::vector<uint8_t> acc = buildAccept(0x21, "LY1HRM", "LY2ONA", 500, 2, true, -3.f, 0x1234);
        CHECK(parseAccept(acc.data(), acc.size(), a), "ACCEPT");
        CHECK(a.sessionId == 0x21 && a.bwHz == 500 && a.startLevel == 2 && a.ctlDeaf && a.callSnrDb == -3.f && a.nonce == 0x1234 && !a.reservedSet, "ACCEPT fields");
        CHECK(a.callerCrc == callsignCrc("LY2ONA") && a.calledCrc == callsignCrc("LY1HRM"), "ACCEPT CRCs");
        std::vector<uint8_t> enc = acc;
        enc[8] |= 0x10;
        CHECK(parseAccept(enc.data(), enc.size(), a) && a.reservedSet, "a reserved bit is reported");
        CqFrame q;
        const std::vector<uint8_t> cq = buildCq("PU2UIT", 2750);
        CHECK(parseCq(cq.data(), cq.size(), q) && q.caller == "PU2UIT" && q.bwHz == 2750, "CQ");
        DisconnectFrame d;
        const std::vector<uint8_t> dc = buildDisconnect(0x21, -7.f);
        CHECK(parseDisconnect(dc.data(), dc.size(), d) && d.sessionId == 0x21 && d.snrDb == -7.f, "DISCONNECT");
    }
    // the session seed: CRC-16 of id, nonce (low first), piece size, CALLER/CALLEE
    {
        const uint8_t buf[] = {0x21, 0xEF, 0xBE, 24, 'L', 'Y', '2', 'O', 'N', 'A', '/', 'L', 'Y', '1', 'H', 'R', 'M'};
        const uint16_t want = crc::crc16CcittFalse(buf, sizeof buf);
        CHECK(sessionSeed(0x21, 0xBEEF, 3, "ly2ona", "LY1HRM") == (want ? want : 0x5A5A), "seed");
        CHECK(sessionSeed(0x21, 0xBEEF, 0, "LY2ONA", "LY1HRM") != sessionSeed(0x21, 0xBEEF, 3, "LY2ONA", "LY1HRM"), "the floor's piece size changes the seed");
    }
    // carousel control frames round-trip
    for (int t = 0; t < 200; t++) {
        CarCtl c;
        c.type = (int)(rng() % 4); c.hasData = rng() & 1; c.ctlDeaf = rng() & 1; c.pollId = (int)(rng() % 16);
        c.level = (int)(rng() % 7); c.frames = (int)(rng() % 16); c.loss16 = (int)(rng() % 16); c.base = (int)(rng() % 16);
        for (int& v : c.need) v = (rng() % 5) ? (int)(rng() % 97) : -1;
        c.gap = (rng() % 3) ? (int)(rng() % 96) : -1;
        c.snrDb = (rng() % 4) ? (float)((int)(rng() % 63) - 24) : -1000.f;
        c.hLevel = (int)(rng() % 7); c.hFrames = (int)(rng() % 16); c.hId = (int)(rng() % 16);
        c.highBlock = (int)(rng() % 16); c.unopened = rng() & 1; c.shortRung = rng() & 1; c.backlog = (int)(rng() % 4);
        const std::vector<uint8_t> b = buildCarCtl(c);
        CarCtl d;
        CHECK(parseCarCtl(b.data(), b.size(), d), "ctl %d parse", t);
        bool same = d.type == c.type && d.hasData == c.hasData && d.ctlDeaf == c.ctlDeaf && d.pollId == c.pollId && d.level == c.level &&
                    d.frames == c.frames && d.loss16 == c.loss16 && d.base == c.base && d.gap == c.gap && d.snrDb == c.snrDb &&
                    d.hLevel == c.hLevel && d.hFrames == c.hFrames && d.hId == c.hId && d.highBlock == c.highBlock && d.unopened == c.unopened &&
                    d.shortRung == c.shortRung && d.backlog == c.backlog;
        for (int o = 0; o < 8; o++) same &= d.need[o] == c.need[o];
        CHECK(same, "ctl %d fields", t);
    }
    // data frames and the Reed-Solomon erasure code: any K of the pieces rebuild the block
    {
        const int P = 24, K = 9;
        std::vector<std::vector<uint8_t>> data((size_t)K, std::vector<uint8_t>(P));
        for (auto& d : data) for (auto& b : d) b = (uint8_t)rng();
        for (int trial = 0; trial < 50; trial++) {
            std::vector<int> idx;
            std::vector<std::vector<uint8_t>> pcs;
            while ((int)idx.size() < K) {
                const int i = (int)(rng() % 40);
                bool dup = false;
                for (int x : idx) dup |= x == i;
                if (dup) continue;
                idx.push_back(i);
                if (i < K) pcs.push_back(data[(size_t)i]);
                else { std::vector<uint8_t> r; rsRepair(K, P, data, i - K, r); pcs.push_back(r); }
            }
            std::vector<std::vector<uint8_t>> out;
            CHECK(rsDecode(K, P, idx, pcs, out) && out == data, "RS trial %d", trial);
        }
        std::vector<uint8_t> pcs;
        for (int i = 0; i < 3; i++) pcs.insert(pcs.end(), data[(size_t)i].begin(), data[(size_t)i].end());
        const std::vector<uint8_t> f = buildCarData(126, 2, 5, 3, 19, K, 0, 3, 7, pcs.data(), P);
        CarData cd;
        CHECK(parseCarData(f.data(), f.size(), P, cd) && cd.segs.size() == 1, "DATA parse");
        if (!cd.segs.empty()) {
            const CarSegment& g = cd.segs[0];
            CHECK(cd.left == 2 && cd.pollId == 5 && cd.highBlock == 3 && g.block == 3 && g.K == K && g.count == 3 && g.first == 0 && g.pad == 7, "DATA fields");
            CHECK(!std::memcmp(f.data() + g.offset, pcs.data(), pcs.size()), "DATA pieces");
        }
    }
    // broadcast messages
    {
        const std::string m = "LY2ONA: hello";
        const std::vector<uint8_t> f = buildBcastMessage((const uint8_t*)m.data(), m.size(), 126);
        BcastMessage bm;
        CHECK(parseBcastMessage(f.data(), f.size(), bm) && bm.lenPrefix && std::string(bm.bytes.begin(), bm.bytes.end()) == m, "message");
    }
    // RaptorQ: the precode with the K' source rows is solvable for every table entry up to K' = 1002 (RFC 6330 5.6 guarantees it)
    {
        int bad = 0, tested = 0;
        for (uint32_t K = 1; K <= 1002; K++) {
            const RqParams p = rqParams(K);
            if (p.Kp != K) continue;
            tested++;
            std::vector<uint8_t> src((size_t)K * 4);
            for (auto& b : src) b = (uint8_t)rng();
            RqBlockEncoder e(K, 4, src.data());
            bool ok = e.ok();
            for (uint32_t i = 0; ok && i < K; i++) ok = e.symbol(i) == std::vector<uint8_t>(src.begin() + i * 4, src.begin() + i * 4 + 4);
            bad += !ok;
        }
        printf("  RaptorQ: %d table entries tested, %d failed\n", tested, bad);
        CHECK(tested > 90 && bad == 0, "RaptorQ precode: %d of %d", bad, tested);
        // decode from losses: 25 % of the source symbols missing, repair symbols instead, a few extra
        int fails = 0;
        for (int trial = 0; trial < 20; trial++) {
            const uint32_t K = 30 + (uint32_t)(rng() % 400), T = 41;
            std::vector<uint8_t> src((size_t)K * T);
            for (auto& b : src) b = (uint8_t)rng();
            RqBlockEncoder e(K, T, src.data());
            RqBlockDecoder d(K, T);
            uint32_t esi = 0, sent = 0;
            while (!d.done() && sent < K + 40) {
                if ((rng() % 4) != 0) { d.add(esi, e.symbol(esi).data()); sent++; d.tryDecode(); }
                esi++;
            }
            fails += !(d.done() && d.data() == src);
        }
        CHECK(fails == 0, "RaptorQ decode: %d of 20 failed", fails);
        // the partition of a transfer into source blocks
        CHECK(rqBlockSymbols(1000, 41, 1, 0) == 25 && rqBlockSymbols(1000, 41, 2, 0) == 13 && rqBlockSymbols(1000, 41, 2, 1) == 12, "partition");
    }
    // broadcast file frame and the bundle
    {
        std::vector<uint8_t> f(510, 0);
        f[0] = framerByte(kPtBcastControl, 7);
        f[1] = 0x10; f[2] = 0x04; f[3] = 0;    // F = 1040
        f[4] = 40; f[5] = 0;                   // T = 41
        f[10] = 24; f[11] = 0;
        BcastFileFrame b;
        CHECK(parseBcastFile(f.data(), f.size(), b) && b.session == 7 && b.F == 1040 && b.T == 41 && b.esi == 24 && b.symbols == 12, "file frame");
        f[0] = framerByte(kPtBcastControl, 0);
        CHECK(!parseBcastFile(f.data(), f.size(), b), "session 0 is not a file");
        std::vector<uint8_t> obj = {9, 0, 0, 0, 'a', '.', 't', 'x', 't', '\n', 'h', 'i', '!'};
        std::string name;
        size_t off = 0, len = 0;
        CHECK(parseBundle(obj, name, off, len) && name == "a.txt" && off == 10 && len == 3, "bundle");
        std::vector<uint8_t> evil = {9, 0, 0, 0, '.', '.', '/', 'x', 't', '\n', 'h', 'i', '!'};
        CHECK(!parseBundle(evil, name, off, len), "a path in the bundle name is refused");
    }
    return testSummary("mercury proto");
}
