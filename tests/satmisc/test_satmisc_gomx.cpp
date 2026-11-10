// CubeSat telemetry per satellite: a GOMX-1 beacon built from the published layout (gr-satellites), bit by bit.
#include "dect2/cubesat/cubesat_telem.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2::cubesat;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static void be16(std::vector<uint8_t>& v, int x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
static double find(const TelResult& r, const char* group, const char* name) { for (const auto& f : r.fields) if (f.group == group && f.name == name) return f.num; printf("missing %s/%s\n", group, name); fails++; return NAN; }
int main() {
    std::vector<uint8_t> d(4, 0);
    d.insert(d.end(), {0x5F, 0x00, 0x00, 0x10, 0x03});   // time, flags
    be16(d, 321);                                        // OBC boot count
    be16(d, (int)(-5.25 * 4)); be16(d, (int)(12.5 * 4));
    for (int i = 0; i < 6; i++) be16(d, (i + 1) * 4);
    be16(d, 7); be16(d, 8); be16(d, 9); be16(d, 10);     // COM
    be16(d, 21); be16(d, -3);
    be16(d, -110); be16(d, 450);
    be16(d, 812);                                        // 8.12 V
    be16(d, 130); be16(d, 42);
    for (int i = 0; i < 3; i++) be16(d, 5000 + i);       // EPS boost
    be16(d, 7900);
    for (int i = 0; i < 6; i++) be16(d, 10 + i);
    for (int i = 0; i < 3; i++) be16(d, 20 + i);
    be16(d, 60); be16(d, 70);
    for (int i = 0; i < 6; i++) be16(d, -i);
    d.resize(9 + 18 + 22 + 63 + 40 + 9 + 55, 0);
    TelResult r;
    CHECK(parseTelemetry(0, {}, d, r, 39430), "not parsed");
    CHECK(r.satellite == "GOMX 1", "%s", r.satellite.c_str());
    CHECK(std::fabs(find(r, "OBC", "Temperature 1") + 5.25) < 1e-6, "temp");
    CHECK(find(r, "OBC", "Panel temperature 3") == 3.0, "panel");
    CHECK(std::fabs(find(r, "COM", "Battery") - 8.12) < 1e-6, "batt");
    CHECK(find(r, "COM", "RSSI") == -110, "rssi");
    CHECK(std::fabs(find(r, "EPS", "Battery voltage") - 7.9) < 1e-6, "vbatt");
    CHECK(find(r, "EPS", "Battery current") == 70, "cursys");
    CHECK(find(r, "EPS", "Temperature 3") == -2, "temp3");
    { std::vector<uint8_t> shortD(d.begin(), d.end() - 1); TelResult q; CHECK(!parseGomx1(shortD, q), "one byte short parsed"); }
    TelResult s; CHECK(!parseTelemetry(0, {}, std::vector<uint8_t>(40, 0), s, 39430), "short frame parsed");
    TelResult o; CHECK(!parseTelemetry(0, {}, d, o, 12345) || o.satellite.empty(), "wrong satellite");
    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
