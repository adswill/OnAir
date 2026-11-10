// Yaesu Fusion GPS block: bytes built from the YSFGateway layout. Simulated data only.
#include "dect2/fusion/fusion_gps.h"
#include <cstdio>
#include <cmath>
#include <vector>
using namespace dect2::fusion;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> block(int laDeg, int laMin, int laFrac, bool north, int loDeg, int loMin, int loFrac, bool east, uint8_t model) {
    std::vector<uint8_t> b(20, 0x20);
    b[1] = 0x47; b[2] = 0x64; b[4] = model;
    const int latH = north ? 5 : 3;
    b[5] = (uint8_t)(0x30 | (laDeg / 10)); b[6] = (uint8_t)(0x30 | (laDeg % 10));
    b[7] = (uint8_t)(0x30 | (laMin / 10)); b[8] = (uint8_t)((latH << 4) | (laMin % 10));
    const int loRangeHigh = loDeg < 10 || loDeg >= 100 ? 5 : 3;
    b[9] = (uint8_t)((loRangeHigh << 4) | (laFrac / 10)); b[10] = (uint8_t)(((east ? 3 : 5) << 4) | (laFrac % 10));
    if (loDeg < 10) b[11] = (uint8_t)(0x76 + loDeg); else if (loDeg >= 110) b[11] = (uint8_t)(0x26 + loDeg - 110); else if (loDeg >= 100) b[11] = (uint8_t)(0x6C + loDeg - 100); else b[11] = (uint8_t)(0x26 + loDeg - 10);
    b[12] = loMin >= 10 ? (uint8_t)(0x26 + loMin - 10) : (uint8_t)(0x58 + loMin);
    b[13] = (uint8_t)(0x1C + loFrac);
    b[14] = 0x03;
    uint8_t c = 0; for (int i = 0; i <= 14; i++) c = (uint8_t)(c + b[i]);
    b[15] = c;
    return b;
}
int main() {
    auto b = block(54, 41, 25, true, 25, 16, 50, true, 0x25);
    YsfGps g = ysfGpsParse(b.data(), (int)b.size());
    CHECK(g.ok && g.radio == "FTM-400D");
    CHECK(std::fabs(g.lat - (54 + 41.25 / 60)) < 1e-6 && std::fabs(g.lon - (25 + 16.5 / 60)) < 1e-6);
    b = block(34, 5, 0, false, 118, 9, 99, false, 0x33);
    g = ysfGpsParse(b.data(), (int)b.size());
    CHECK(g.ok && g.radio == "FT-5D" && g.lat < 0 && g.lon < 0);
    CHECK(std::fabs(g.lat + (34 + 5.0 / 60)) < 1e-6 && std::fabs(g.lon + (118 + 9.99 / 60)) < 1e-6);
    // wrong tag or checksum: nothing
    b = block(54, 41, 25, true, 25, 16, 50, true, 0x25); b[1] = 0x11; CHECK(!ysfGpsParse(b.data(), (int)b.size()).ok);
    b = block(54, 41, 25, true, 25, 16, 50, true, 0x25); b[15] ^= 1; CHECK(!ysfGpsParse(b.data(), (int)b.size()).ok);
    return testSummary("fusion_gps");
}
