// DMR Capacity Plus / Connect Plus CSBKs and Motorola IP services (dmr_vendor.h). Connect Plus grant bytes are the sample in DSD-FME's dmr_csbk.c; the Capacity Plus
// layouts are from the same file and generated here. Everything else is generated from the same layouts.
#include "dect2/dmr/dmr_vendor.h"
#include <cstdio>
#include <cstring>
#include <random>

using namespace dect2::dmr;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// 8 data bytes after opcode and FID
static void block(uint8_t d[8], int fl, int ts, int rest, std::initializer_list<int> bytes) {
    std::memset(d, 0, 8);
    d[0] = (uint8_t)(fl << 6 | ts << 5 | (rest & 15));
    int i = 1;
    for (int b : bytes) d[i++] = (uint8_t)b;
}

int main() {
    // Connect Plus group voice grant from DSD-FME: [83][06][01][B9][DA][00][04][C0][28][02][AA][FF]: source 113114, target 1216, LCN 2, slot 1
    ConPlus c;
    const uint8_t con[] = {0x01, 0xB9, 0xDA, 0x00, 0x04, 0xC0, 0x28, 0x02};
    CHECK(conPlusParse(0x03, con, c) && c.src == 113114 && c.dst == 1216 && c.lcn == 2 && c.slot == 1);
    const uint8_t data[] = {0x00, 0x00, 0x63, 0x30, 0, 0, 0, 0};
    CHECK(conPlusParse(0x06, data, c) && c.dst == 99 && c.lcn == 3 && c.slot == 0);
    CHECK(!conPlusParse(0x02, data, c));

    // Capacity Plus: a single block with channels 1 and 3 active (bank1 = 0b10100000), groups 5 and 9, no second bank
    CapPlus cp;
    std::vector<CapPlusGrant> g; int rest = 0;
    uint8_t d[8];
    block(d, 3, 0, 4, {0xA0, 5, 9, 0, 0, 0, 0});
    CHECK(cp.feed(d, g, rest) && rest == 4 && g.size() == 2 && g[0].lsn == 1 && g[0].tg == 5 && g[1].lsn == 3 && g[1].tg == 9);
    // none active
    block(d, 3, 1, 2, {0, 0, 0, 0, 0, 0, 0});
    CHECK(cp.feed(d, g, rest) && g.empty() && rest == 2);
    // two blocks, one slot: first block has bank1 with four groups, the rest come in the continuation
    block(d, 2, 0, 1, {0xF0, 11, 12, 13, 14, 0, 0});
    CHECK(!cp.feed(d, g, rest));
    block(d, 1, 0, 1, {0, 0, 0, 0, 0, 0, 0});
    CHECK(cp.feed(d, g, rest) && g.size() == 4 && g[3].tg == 14 && g[3].lsn == 4);
    // a second bank group, channel 9, group 21 (after the bank byte)
    block(d, 3, 0, 1, {0x80, 7, 0x80, 21, 0, 0, 0});
    CHECK(cp.feed(d, g, rest) && g.size() == 2 && g[1].lsn == 9 && g[1].tg == 21);
    // a continuation with no start, and random blocks: no crash
    std::mt19937 r(1);
    for (int i = 0; i < 200000; i++) { for (auto& x : d) x = (uint8_t)r(); cp.feed(d, g, rest); CHECK(g.size() <= 16); }

    // ARS and TMS over IPv4/UDP
    auto ip = [](int sp, int dp, std::vector<uint8_t> body) {
        std::vector<uint8_t> v = {0x45, 0, 0, 0, 0, 0, 0, 0, 64, 17, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, (uint8_t)(sp >> 8), (uint8_t)sp, (uint8_t)(dp >> 8), (uint8_t)dp, 0, 0, 0, 0};
        v.insert(v.end(), body.begin(), body.end());
        return v;
    };
    IpService s;
    auto ars = ip(4005, 4005, {'R', 'A', 'D', 'I', 'O', '1', '2'});
    CHECK(ipService(ars.data(), ars.size(), s) && s.port == 4005 && s.text == "RADIO12");
    // text "HI": len, header (bit pattern for a message, ack 0), address length 0, then UTF-16 big-endian with the first character's high byte reused
    std::vector<uint8_t> tmsBody = {0x00, 0x04, 0x00, 0x00, 'H', 'I'};
    auto tms = ip(4007, 4007, {0x00, 0x08, 0xA0, 0x00, 0x00, 'H', 0x00, 'I'});
    CHECK(ipService(tms.data(), tms.size(), s) && s.port == 4007 && !s.ack);
    CHECK(s.text == "HI");
    auto ack = ip(4007, 4007, {0x00, 0x02, 0xA1, 0x00});
    CHECK(ipService(ack.data(), ack.size(), s) && s.ack);
    auto other = ip(4001, 4001, {1, 2, 3});
    CHECK(!ipService(other.data(), other.size(), s));
    CHECK(!ipService(ars.data(), 10, s));
    std::vector<uint8_t> v(60);
    for (int i = 0; i < 300000; i++) {
        v.resize(30 + r() % 70);
        for (auto& x : v) x = (uint8_t)r();
        v[0] = 0x45; v[9] = 17; v[20] = 0x0F; v[21] = (r() & 1) ? 0xA5 : 0xA7; v[22] = 0x0F; v[23] = v[21];
        ipService(v.data(), v.size(), s);
    }
    printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
