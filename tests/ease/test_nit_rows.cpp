// The NIT consumer: real table sections through SiCollector to rows, LNB mapping, cable, malformed input
#include "dect2/ease/nit.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2::dvbsi { // names are not under test
std::string dvbText(const unsigned char*, int, const TextContext&) { return {}; }
TextContext TextContext::withLanguage(std::string_view) const { return *this; }
}

using namespace dect2;
using namespace dect2::ease;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// a NIT section (EN 300 468 5.2.1) with satellite (0x43) or cable (0x44) delivery descriptors
struct Tp { int tsid; double freq; int sr; int mod; int pol; };
static std::vector<uint8_t> section(int tid, bool sat, const std::vector<Tp>& tps) {
    std::vector<uint8_t> loop;
    for (const Tp& t : tps) {
        loop.push_back((uint8_t)(t.tsid >> 8)); loop.push_back((uint8_t)t.tsid);
        loop.push_back(0x20); loop.push_back(0x85);   // onid
        loop.push_back(0xF0); loop.push_back(13);     // transport descriptors length
        loop.push_back(sat ? 0x43 : 0x44); loop.push_back(11);
        // frequency: satellite GHz xxx.xxxxx (BCD, 8 digits, units 10 kHz); cable MHz xxxx.xxxx
        const long long f = sat ? std::llround(t.freq * 100.0) : std::llround(t.freq * 10000.0);
        loop.push_back(bcd((int)(f / 1000000) % 100)); loop.push_back(bcd((int)(f / 10000) % 100)); loop.push_back(bcd((int)(f / 100) % 100)); loop.push_back(bcd((int)(f % 100)));
        if (sat) { loop.push_back(0x01); loop.push_back(0x92); loop.push_back((uint8_t)(0x80 | (t.pol << 5) | t.mod)); }   // 19.2 E
        else { loop.push_back(0xFF); loop.push_back(0xF0); loop.push_back((uint8_t)t.mod); }
        const int v = t.sr * 10;   // 7 BCD digits in units of 100 sym/s
        loop.push_back(bcd(v / 100000 % 100)); loop.push_back(bcd(v / 1000 % 100)); loop.push_back(bcd(v / 10 % 100)); loop.push_back((uint8_t)((v % 10) << 4 | 3));
    }
    std::vector<uint8_t> s = {(uint8_t)tid, 0, 0, 0x00, 0x01, 0xC1, 0, 0, 0xF0, 0x00, (uint8_t)(0xF0 | (loop.size() >> 8)), (uint8_t)loop.size()};
    s.insert(s.end(), loop.begin(), loop.end());
    for (int i = 0; i < 4; i++) s.push_back(0);   // CRC (checked by the caller of SiCollector)
    const int len = (int)s.size() - 3;
    s[1] = (uint8_t)(0xB0 | (len >> 8)); s[2] = (uint8_t)len;
    return s;
}

int main() {
    {   // satellite: everything mapped through the LNB by the received transponder
        SiCollector c;
        auto s = section(0x40, true, {{1051, 11494.0, 22000, 1, 0}, {1057, 11597.0, 22000, 2, 1}, {1101, 12129.0, 27500, 1, 1}});
        c.section(0x10, s.data(), (int)s.size());
        const auto snap = c.snapshot();
        auto r = nitRows(snap, 1, 1057, 1847.0, false);       // LO 9750: 11597 - 9750 = 1847
        CHECK(r.size() == 3);
        CHECK(std::fabs(r[0].mhz - 11494.0) < 0.01); CHECK(r[0].pol == 0);
        CHECK(std::fabs(r[0].tuneMhz - 1744.0) < 0.01);
        CHECK(std::fabs(r[2].tuneMhz - 2379.0) < 0.01);
        CHECK(r[1].current); CHECK(!r[0].current);
        CHECK(std::fabs(r[0].symbolRateK - 22000.0) < 0.5);
        auto inv = nitRows(snap, 1, 1057, 1847.0, true);      // high side
        CHECK(std::fabs(inv[0].tuneMhz - 1950.0) < 0.01);
        auto un = nitRows(snap, 1, 9999, 1847.0, false);      // the transponder received is not in the list: nothing to map with
        CHECK(un.size() == 3); CHECK(un[0].tuneMhz == 0); CHECK(!un[0].current);
        CHECK(nitRows(snap, 2, 1057, 1847.0, false).empty());  // no cable rows
    }
    {   // cable: the frequency is the tuning frequency
        SiCollector c;
        auto s = section(0x40, false, {{5, 346.0, 6900, 3, 0}, {6, 354.0, 6875, 5, 0}});
        c.section(0x10, s.data(), (int)s.size());
        auto r = nitRows(c.snapshot(), 2, 5, 346.0, false);
        CHECK(r.size() == 2); CHECK(r[0].tuneMhz == 346.0); CHECK(r[1].tuneMhz == 354.0); CHECK(r[0].current);
        CHECK(std::fabs(r[0].symbolRateK - 6900.0) < 0.5);
    }
    {   // another network's table (0x41) is not this network's list
        SiCollector c;
        auto s = section(0x41, false, {{5, 346.0, 6900, 3, 0}});
        c.section(0x10, s.data(), (int)s.size());
        CHECK(nitRows(c.snapshot(), 2, 5, 346.0, false).empty());
    }
    {   // a repeated section does not double the list; truncated sections do not crash
        SiCollector c;
        auto s = section(0x40, false, {{5, 346.0, 6900, 3, 0}});
        c.section(0x10, s.data(), (int)s.size());
        c.section(0x10, s.data(), (int)s.size());
        CHECK(nitRows(c.snapshot(), 2, 5, 346.0, false).size() == 1);
        for (int n = 0; n < (int)s.size(); n++) { SiCollector d; d.section(0x10, s.data(), n); (void)nitRows(d.snapshot(), 2, 5, 346.0, false); }
    }
    if (!fails) printf("nit ok\n");
    return fails ? 1 : 0;
}
