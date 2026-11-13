// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 control messages beyond the grants (p25_cc.h): fields are built from the OP25 layouts, since no public capture of these TSBKs ships with a test expectation.
#include "dect2/p25/p25_cc.h"
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::p25;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<std::vector<uint8_t>> mbt(int fmt, int sap, int src, int op, int nDataBlocks, std::vector<std::pair<int, std::pair<int, uint64_t>>> fields) {
    std::vector<uint8_t> d((size_t)nDataBlocks * 12, 0);
    const size_t N = d.size();
    for (auto& f : fields) {
        const int low = f.first, width = f.second.first; const uint64_t v = f.second.second;
        for (int i = 0; i < width; i++) if ((v >> i) & 1) d[N - 1 - (size_t)((low + i) / 8)] |= (uint8_t)(1 << ((low + i) % 8));
    }
    const uint32_t crc = crc32Pdu(d.data(), N - 4);
    d[N - 4] = (uint8_t)(crc >> 24); d[N - 3] = (uint8_t)(crc >> 16); d[N - 2] = (uint8_t)(crc >> 8); d[N - 1] = (uint8_t)crc;
    std::vector<std::vector<uint8_t>> b;
    std::vector<uint8_t> h(12, 0);
    h[0] = (uint8_t)fmt; h[1] = (uint8_t)sap; h[3] = (uint8_t)(src >> 16); h[4] = (uint8_t)(src >> 8); h[5] = (uint8_t)src; h[6] = (uint8_t)nDataBlocks; h[7] = (uint8_t)op;
    b.push_back(h);
    for (int k = 0; k < nDataBlocks; k++) b.emplace_back(d.begin() + 12 * k, d.begin() + 12 * (k + 1));
    return b;
}

int main() {
    CcItem c;
    // Motorola patch add: supergroup 1000, members 1001 1002 and an empty slot
    CHECK(tsbkDescribe(0x90, 0x00, (1000ull << 48) | (1001ull << 32) | (1002ull << 16), c));
    CHECK(c.kind == kCcPatchAdd && c.supergroup == 1000 && c.members.size() == 2 && c.members[1] == 1002);
    CHECK(tsbkDescribe(0x90, 0x01, (1000ull << 48) | (1001ull << 32), c) && c.kind == kCcPatchDel && c.members.size() == 1);
    CHECK(tsbkDescribe(0x90, 0x02, (0x2064ull << 40) | (1000ull << 24) | 1234567, c) && c.kind == kCcPatchGrant && c.channel == 0x2064 && c.supergroup == 1000 && c.unit == 1234567);
    // Harris regroup, activate a group
    CHECK(tsbkDescribe(0xA4, 0x30, (1ull << 62) | (1ull << 61) | (2000ull << 40) | (5ull << 16) | 2001, c) && c.kind == kCcPatchAdd && c.supergroup == 2000 && c.members[0] == 2001);
    // affiliation response: accepted and refused
    CHECK(tsbkDescribe(0, 0x28, (0ull << 56) | (345ull << 40) | (7002ull << 24) | 1234567, c) && c.kind == kCcAffiliation && c.accepted && c.tg == 7002 && c.unit == 1234567);
    CHECK(tsbkDescribe(0, 0x28, (1ull << 56) | (7002ull << 24) | 1234567, c) && !c.accepted);
    // registration: rv 0 accepted (unit in the source id field), rv 1 refused
    CHECK(tsbkDescribe(0, 0x2C, (0ull << 60) | (0xAB1ull << 48) | (1234567ull << 24) | 1, c) && c.kind == kCcRegistration && c.accepted && c.unit == 1234567);
    CHECK(tsbkDescribe(0, 0x2C, (1ull << 60) | (1234567ull << 24) | 1, c) && !c.accepted);
    // deny: service 0x00 group voice, reason 0x31; a queued response
    CHECK(tsbkDescribe(0, 0x27, (0x31ull << 48) | 1234567, c) && c.kind == kCcDeny && c.reason == 0x31 && c.text.find("not authorised") != std::string::npos);
    CHECK(tsbkDescribe(0, 0x21, (0x40ull << 48) | 7, c) && c.kind == kCcQueue && c.text.find("channel resources") != std::string::npos);
    CHECK(tsbkDescribe(0, 0x20, (0x28ull << 56) | 7, c) && c.kind == kCcAck && c.text.find("group affiliation") != std::string::npos);
    CHECK(tsbkDescribe(0, 0x14, (0x2064ull << 40) | (0x2065ull << 24) | 99, c) && c.kind == kCcDataGrant && c.channel == 0x2064);
    // a voice grant is the receiver's own business
    CHECK(!tsbkDescribe(0, 0x00, 1, c));
    CHECK(!tsbkDescribe(0x90, 0x10, 1, c) && !tsbkDescribe(0xC0, 0x00, 1, c));

    // MBT: group voice grant, RFSS status, network status, regroup grant, affiliation; a bad CRC or short message gives nothing
    std::vector<Tsbk> out;
    CHECK(mbtToTsbks(mbt(0x17, 61, 1234567, 0x00, 1, {{64, {16, 0x2064}}, {32, {16, 7002}}}), out) && out.size() == 1);
    CHECK(out[0].opcode == kGrpVGrant && (out[0].args & 0xFFFFFF) == 1234567 && ((out[0].args >> 40) & 0xFFFF) == 0x2064 && ((out[0].args >> 24) & 0xFFFF) == 7002);
    CHECK(mbtToTsbks(mbt(0x17, 61, 0x0ABC, 0x3A, 1, {{88, {8, 5}}, {80, {8, 3}}, {64, {16, 0x2010}}}), out) && out[0].opcode == kRfssSts && ((out[0].args >> 40) & 0xFFF) == 0xABC && ((out[0].args >> 32) & 0xFF) == 5 && ((out[0].args >> 24) & 0xFF) == 3 && ((out[0].args >> 8) & 0xFFFF) == 0x2010);
    CHECK(mbtToTsbks(mbt(0x17, 61, 0x0ABC, 0x3B, 1, {{76, {20, 0xBEE00}}, {56, {16, 0x2010}}}), out) && out[0].opcode == kNetSts && ((out[0].args >> 36) & 0xFFFFF) == 0xBEE00 && ((out[0].args >> 24) & 0xFFF) == 0xABC);
    CHECK(mbtToTsbks(mbt(0x17, 61, 77, 0x02, 2, {{168, {8, 0x90}}, {80, {16, 0x2064}}, {48, {16, 1000}}}), out) && out[0].mfid == 0x90 && out[0].opcode == 2 && (out[0].args & 0xFFFFFF) == 77);
    CHECK(mbtToTsbks(mbt(0x17, 61, 77, 0x28, 2, {{120, {2, 0}}, {144, {16, 345}}, {128, {16, 7002}}}), out));
    CHECK(tsbkDescribe(0, 0x28, out[0].args, c) && c.tg == 7002 && c.unit == 77);
    auto bad = mbt(0x17, 61, 1, 0x00, 1, {{64, {16, 1}}});
    bad[1][0] ^= 1;
    CHECK(!mbtToTsbks(bad, out));
    CHECK(!mbtToTsbks(mbt(0x15, 4, 1, 0x00, 1, {}), out));       // not SAP 61
    CHECK(!mbtToTsbks({{1, 2, 3}}, out));

    // Unconfirmed data: IPv4 in SAP 4, with pad; a text payload elsewhere; a bad CRC
    std::vector<uint8_t> ip = {0x45, 0, 0, 32, 0, 0, 0, 0, 64, 17, 0, 0, 10, 0, 0, 1, 10, 0, 0, 2, 0x10, 0x01, 0x0F, 0xA1, 0, 12, 0, 0, 'h', 'i', '!', '!'};
    const int pad = 12 - (int)(ip.size() + 4) % 12;
    std::vector<uint8_t> body(ip);
    if (pad < 12) body.resize(body.size() + (size_t)pad, 0);
    std::vector<uint8_t> whole(body); whole.resize(whole.size() + 4);
    const uint32_t crc = crc32Pdu(whole.data(), whole.size() - 4);
    whole[whole.size() - 4] = (uint8_t)(crc >> 24); whole[whole.size() - 3] = (uint8_t)(crc >> 16); whole[whole.size() - 2] = (uint8_t)(crc >> 8); whole[whole.size() - 1] = (uint8_t)crc;
    std::vector<std::vector<uint8_t>> pdu;
    std::vector<uint8_t> h(12, 0); h[0] = 0x35; h[1] = 4; h[5] = 77; h[6] = (uint8_t)(whole.size() / 12); h[7] = (uint8_t)(pad < 12 ? pad : 0);
    pdu.push_back(h);
    for (size_t k = 0; k + 12 <= whole.size(); k += 12) pdu.emplace_back(whole.begin() + (long)k, whole.begin() + (long)k + 12);
    PduData pd;
    CHECK(pduAssemble(pdu, pd) && pd.crcOk && pd.sap == 4 && pd.llid == 77 && pd.outbound);
    CHECK(pd.payload.size() == ip.size() && pd.summary.find("10.0.0.1 to 10.0.0.2") != std::string::npos && pd.summary.find("ports 4097 to 4001") != std::string::npos);
    pdu[1][3] ^= 0x40;
    CHECK(!pduAssemble(pdu, pd));

    // random TSBK arguments and blocks: no crash
    std::mt19937_64 g(5);
    for (int i = 0; i < 2000000; i++) { CcItem x; tsbkDescribe((int)(g() & 0xFF), (int)(g() & 0x3F), g(), x); }
    for (int i = 0; i < 20000; i++) {
        std::vector<std::vector<uint8_t>> b;
        int n = 1 + (int)(g() % 6);
        for (int k = 0; k < n; k++) { std::vector<uint8_t> v(12); for (auto& x : v) x = (uint8_t)g(); b.push_back(v); }
        b[0][1] = 61; b[0][0] = (uint8_t)((g() & 1) ? 0x17 : 0x15);
        mbtToTsbks(b, out);
        PduData x; pduAssemble(b, x);
    }
    printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
