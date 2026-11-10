// TWGO records: DLAC text and overlay geometry built to the layout, malformed lengths rejected. Simulated records only.
#include "dect2/uat/uat_twgo.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> dlac(const std::string& s) {
    static const std::string a = "~ABCDEFGHIJKLMNOPQRSTUVWXYZ~\t~\n| !\"#$%&'()*+,-./0123456789:;<=>?";
    std::vector<uint8_t> o; int acc = 0, n = 0;
    for (char c : s) { const size_t k = a.find(c); acc = (acc << 6) | (int)k; n += 6; while (n >= 8) { o.push_back((uint8_t)(acc >> (n - 8))); n -= 8; acc &= (1 << n) - 1; } }
    if (n) o.push_back((uint8_t)(acc << (8 - n)));
    return o;
}

int main() {
    // text record, DLAC, one report
    auto loc = dlac("LAX");
    std::vector<uint8_t> tx = {0x20, 0x10};
    tx.insert(tx.end(), loc.begin(), loc.end());
    tx.push_back(0);
    auto body = dlac("NOTAM TFR ACTIVE");
    const size_t len = body.size() + 5;
    tx.push_back((uint8_t)(len >> 8)); tx.push_back((uint8_t)len);
    const int number = 1234, year = 26, status = 1;
    tx.push_back((uint8_t)(number >> 6)); tx.push_back((uint8_t)(((number & 0x3F) << 2) | (year >> 5)));
    tx.push_back((uint8_t)(((year & 31) << 3) | (status << 2)));
    tx.insert(tx.end(), body.begin(), body.end());
    std::string text; std::vector<UatOverlay> ov;
    CHECK(uatTwgoDecode(8, tx.data(), tx.size(), text, ov));
    CHECK(text.find("NOTAM TFR ACTIVE") != std::string::npos && text.find("#1234") != std::string::npos);
    // overlay record: 1 polygon of 2 vertices, MSL
    std::vector<uint8_t> o = {0x80, 0x10};
    o.insert(o.end(), loc.begin(), loc.end());
    o.push_back(0);
    const int vLen = 5 + 2 + 2 + 1 + 1 + 12;
    o.push_back((uint8_t)(vLen >> 2)); o.push_back((uint8_t)((vLen & 3) << 6 | ((99 >> 8) & 0x3F))); o.push_back(99);
    o.push_back((uint8_t)(26 << 1)); o.push_back((uint8_t)(3 << 1));     // record id 4, no label
    o.push_back(0); o.push_back(0);                                        // numeric label bytes
    o.push_back(0); o.push_back(0xE0 | 15);                                // element 0, object type 14, status 15
    o.push_back(3);                                                        // no times, geometry 3 (3D polygon MSL)
    o.push_back(1);                                                        // two vertices
    auto vtx = [&](double lon, double lat, int alt) {
        if (lon < 0) lon += 360; if (lat < 0) lat += 180;
        const uint32_t lo = (uint32_t)std::lround(lon * 524288.0 / 360.0), la = (uint32_t)std::lround(lat * 524288.0 / 360.0);
        const int a = alt / 100;
        o.push_back((uint8_t)(lo >> 11)); o.push_back((uint8_t)(lo >> 3));
        o.push_back((uint8_t)(((lo & 7) << 5) | (la >> 14)));
        o.push_back((uint8_t)(la >> 6));
        o.push_back((uint8_t)(((la & 63) << 2) | (a >> 8)));
        o.push_back((uint8_t)a);
    };
    vtx(-118.4, 33.9, 3000);
    vtx(-118.3, 34.1, 5000);
    ov.clear(); text.clear();
    CHECK(uatTwgoDecode(11, o.data(), o.size(), text, ov));
    CHECK(ov.size() == 1 && ov[0].reportNumber == 99 && ov[0].recordId == 4 && ov[0].pts.size() == 2 && ov[0].objectType == 14);
    if (ov.size() == 1 && ov[0].pts.size() == 2) {
        CHECK(std::fabs(ov[0].pts[0].lon + 118.4) < 0.002 && std::fabs(ov[0].pts[0].lat - 33.9) < 0.002 && ov[0].pts[0].altFt == 3000);
        CHECK(std::fabs(ov[0].pts[1].lon + 118.3) < 0.002 && ov[0].pts[1].altFt == 5000);
    }
    // bad length stops the walk
    { auto bad = tx; bad[6] = 0x7F; std::string t2; std::vector<UatOverlay> v2; CHECK(!uatTwgoDecode(8, bad.data(), bad.size(), t2, v2)); }
    return testSummary("uat_twgo");
}
