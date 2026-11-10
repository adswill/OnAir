// Packet layers of the RC and mesh links: known answers (values from the upstream sources), self-consistency, damage.
#include "dect2/rclink/rc_proto.h"
#include <cmath>
#include <cstring>
#include <cstdio>
using namespace dect2;
using namespace dect2::rclink;
#include "../common/test_util.h"

int main() {
    // ---- ExpressLRS
    const auto uid = elrsUidFromPhrase("onair test");
    CHECK(hexOf(uid.data(), 6) == "65bbcb05898c", "UID from phrase: %s (python hashlib)", hexOf(uid.data(), 6).c_str());
    ElrsLink link;
    std::vector<uint8_t> sync(8, 0);
    sync[0] = 2; sync[1] = 17; sync[2] = 42; sync[3] = 3; sync[4] = 0; sync[5] = uid[4]; sync[6] = uid[5];
    {
        uint8_t m[7]; std::memcpy(m, sync.data(), 7);
        const uint16_t c = elrsCrc(14, kElrsCrc14Poly, m, 7, elrsCrcInit(uid[4], uid[5]));
        sync[0] |= (uint8_t)((c >> 8) << 2); sync[7] = (uint8_t)c;
    }
    Decoded d = elrsParse(sync.data(), sync.size(), link);
    CHECK(d.ok && d.type == "SYNC" && link.known && link.uid4 == uid[4], "ELRS SYNC");
    std::vector<uint8_t> bad = sync; bad[3] ^= 1;
    ElrsLink l2;
    CHECK(!elrsParse(bad.data(), bad.size(), l2).ok, "damaged SYNC accepted");
    // RC frame: type 0, nonce 77; without the phrase the channels stay hidden
    std::vector<uint8_t> rc(8, 0);
    rc[1] = 0xAB; rc[2] = 0xCD; rc[3] = 0x12; rc[4] = 0x34; rc[5] = 0x56; rc[6] = 0x80;
    {
        uint8_t m[7]; std::memcpy(m, rc.data(), 7);
        const uint16_t c = elrsCrc(14, kElrsCrc14Poly, m, 7, (uint16_t)(elrsCrcInit(uid[4], uid[5]) ^ 77));
        rc[0] |= (uint8_t)((c >> 8) << 2); rc[7] = (uint8_t)c;
    }
    d = elrsParse(rc.data(), rc.size(), link);
    CHECK(d.ok && d.encrypted && d.detail.find("ch1") == std::string::npos, "ELRS frame without phrase must stay structure only: %s", d.detail.c_str());
    ElrsLink withPhrase; withPhrase.known = true; withPhrase.phrase = true; withPhrase.uid = uid; withPhrase.uid4 = uid[4]; withPhrase.uid5 = uid[5];
    d = elrsParse(rc.data(), rc.size(), withPhrase);
    CHECK(d.ok && d.detail.find("ch1-4") != std::string::npos, "ELRS frame with phrase: %s", d.detail.c_str());
    ElrsLink noLink;
    CHECK(!elrsParse(rc.data(), rc.size(), noLink).ok, "ELRS frame with no link known must not decode");

    // ---- MAVLink
    CHECK(mavCrcExtra(0) == 50 && mavCrcExtra(33) == 104 && mavCrcExtra(253) == 83, "CRC_EXTRA");
    std::vector<uint8_t> hb = {0, 0, 0, 0, 2, 3, 0x51, 4, 3};
    const auto f1 = mavlinkFrame(1, 0, 5, 1, 1, hb);
    const auto f2 = mavlinkFrame(2, 0, 6, 1, 1, hb);
    std::vector<uint8_t> stream = {0x11, 0x22};
    stream.insert(stream.end(), f1.begin(), f1.end());
    stream.insert(stream.end(), f2.begin(), f2.end());
    size_t used = 0;
    auto v = mavlinkScan(stream.data(), stream.size(), &used);
    CHECK(v.size() == 2 && v[0].type == "HEARTBEAT" && used == stream.size(), "two heartbeats, got %zu", v.size());
    // position: 54.6872 N, 25.2797 E, 180 m
    std::vector<uint8_t> pos(28, 0);
    const int32_t lat = 546872000, lon = 252797000, alt = 180000;
    std::memcpy(&pos[4], &lat, 4); std::memcpy(&pos[8], &lon, 4); std::memcpy(&pos[12], &alt, 4);
    const auto f3 = mavlinkFrame(1, 33, 0, 1, 1, pos);
    v = mavlinkScan(f3.data(), f3.size());
    CHECK(v.size() == 1 && v[0].hasPos && std::fabs(v[0].lat - 54.6872) < 1e-6 && std::fabs(v[0].lon - 25.2797) < 1e-6, "position");
    auto corrupt = f3; corrupt[10] ^= 4;
    CHECK(mavlinkScan(corrupt.data(), corrupt.size()).empty(), "bad MAVLink CRC accepted");
    const size_t half = f3.size() / 2;
    size_t u = 99;
    v = mavlinkScan(f3.data(), half, &u);
    CHECK(v.empty() && u == 0, "a partial frame stays for the next packet (used %zu)", u);

    // ---- SiK
    const uint8_t t9[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(sikCrc16(t9, 9) == 0xBEEF, "SiK crc16 0x%04X (from the tables of crc.c)", sikCrc16(t9, 9));
    std::vector<uint8_t> ge = {0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF}, enc, dec;
    sikGolayEncode(ge.data(), ge.size(), enc);
    int cor = 0;
    enc[1] ^= 0x10; enc[8] ^= 0x41;
    CHECK(sikGolayDecode(enc.data(), enc.size(), dec, &cor) && dec == ge && cor >= 2, "Golay corrects bit errors (corrected %d)", cor);
    for (bool golay : {false, true}) {
        std::vector<uint8_t> data(40, 0x5A);
        auto pkt = sikBuild(0x1234, data, 777, golay);
        SikPacket s = sikParse(pkt.data(), pkt.size());
        CHECK(s.ok && s.netId == 0x1234 && s.data == data && s.window == 777 && s.golay == golay, "SiK packet golay=%d: ok %d net %x", golay, s.ok, s.netId);
        pkt[golay ? 30 : 10] ^= 0xFF;
        CHECK(!sikParse(pkt.data(), pkt.size()).ok, "damaged SiK packet golay=%d accepted", golay);
    }

    // ---- FrSky
    const uint16_t ch[8] = {1500, 1600, 1700, 1800, 2000, 2200, 2400, 2600};
    auto d8 = frskyD8Build(0xABCD, 3, ch);
    d = frskyD8Parse(d8.data(), d8.size());
    CHECK(d.ok && d.id == "ABCD" && d.detail.find("1500 1600") == 0, "D8: %s", d.detail.c_str());
    d8[8] ^= 1;
    CHECK(!frskyD8Parse(d8.data(), d8.size()).ok, "damaged D8");
    auto x = frskyXBuild(0x1234, 12, std::vector<uint8_t>(14, 0x40));
    d = frskyXParse(x.data(), x.size());
    CHECK(d.ok && d.id == "1234", "D16");

    // ---- FlySky
    const uint16_t fc[8] = {1500, 1400, 1300, 1200, 1100, 1000, 1700, 1900};
    auto fp = flyskyBuild(0x11223344, fc);
    d = flyskyParse(fp.data(), fp.size());
    CHECK(d.ok && d.id == "11223344" && d.detail.find("1500 1400") == 0, "AFHDS: %s %s", d.id.c_str(), d.detail.c_str());
    const uint16_t f2c[14] = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};
    auto f2p = afhds2aBuild(0xAABBCCDD, 0x01020304, f2c);
    d = flyskyParse(f2p.data(), f2p.size());
    CHECK(d.ok && d.type == "AFHDS 2A channels", "AFHDS 2A");
    f2p[9] = 0x10; f2p[10] = 0x00;   // 16 us is no servo value
    CHECK(!flyskyParse(f2p.data(), f2p.size()).ok, "implausible AFHDS 2A accepted");

    // ---- Reticulum
    uint8_t seed[32], x25519[32], rnd[10];
    for (int i = 0; i < 32; i++) { seed[i] = (uint8_t)(i * 3 + 1); x25519[i] = (uint8_t)(200 - i); }
    for (int i = 0; i < 10; i++) rnd[i] = (uint8_t)(i + 9);
    std::array<uint8_t, 16> dst{};
    const std::string app = "node one";
    auto ann = reticulumAnnounce(seed, x25519, "lxmf.delivery", rnd, std::vector<uint8_t>(app.begin(), app.end()), 2, &dst);
    d = reticulumParse(ann.data(), ann.size());
    CHECK(d.ok && d.type == "ANNOUNCE" && d.id == hexOf(dst.data(), 16) && d.detail.find("signature good") != std::string::npos && d.detail.find("node one") != std::string::npos, "announce: %s", d.detail.c_str());
    auto tampered = ann; tampered[20] ^= 1;
    CHECK(!reticulumParse(tampered.data(), tampered.size()).ok, "tampered announce accepted");
    std::vector<uint8_t> data = {0x00, 1};                       // HEADER_1, single, DATA
    data.insert(data.end(), 16, 0x77);
    data.push_back(0);
    data.insert(data.end(), 30, 0xEE);
    d = reticulumParse(data.data(), data.size());
    CHECK(d.ok && d.type == "DATA" && d.encrypted, "encrypted data frame is structure only");
    // RNode split
    RnodeReassembly st;
    std::vector<uint8_t> a = {0x31, 1, 2, 3}, b = {0x31, 4, 5}, pkt;
    CHECK(!rnodeUnframe(a.data(), a.size(), st, pkt), "first half is not a packet");
    CHECK(rnodeUnframe(b.data(), b.size(), st, pkt) && pkt.size() == 5 && pkt[3] == 4, "split packet");
    return testSummary("rclink_proto");
}
