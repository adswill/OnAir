#include "dect2/d40/dab_d40.h"
#include <cstdio>
#include <random>
using namespace dect2::d40;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    std::mt19937 rng(3); std::normal_distribution<double> g(0, 1);
    // ranker: A mean 18 steady, B mean 22 but locks 50 %, C mean 22 but swings +-6, D seen twice only
    ServiceRanker r;
    for (int i = 0; i < 40; i++) {
        r.add(1, "A", true, 18 + 0.5 * g(rng));
        r.add(2, "B", i % 2 == 0, 22 + 0.5 * g(rng));
        r.add(3, "C", true, 22 + 6 * g(rng));
        if (i < 2) r.add(4, "D", true, 30);
    }
    auto rk = r.ranking();
    CHECK(rk.size() == 3 && rk[0].sid == 1);
    CHECK(rk[2].sid == 2 || rk[2].sid == 3);
    CHECK(std::fabs(rk[0].meanSnr - 18) < 0.5 && std::fabs(rk[0].lockShare - 1) < 1e-9);
    // TII
    TiiTable t;
    CHECK(t.load("# main,sub,name,lat,lon,kW\n12,3,Dubai Tower,25.2,55.27,10\n40,5,Far Site,24.0,54.0,5\n99,1,bad,0,0\n3,0,bad,0,0\n7,2,nolat\n") == 2);
    auto h = t.identify({{40, 5}, {12, 3}, {20, 7}}, 25.1, 55.3);
    CHECK(h.size() == 3 && h[0].name == "Dubai Tower" && h[1].name == "Far Site" && !h[2].known);
    CHECK(std::fabs(h[0].distKm - 13.0) < 1.5);
    CHECK(h[0].bearingDeg > 330 || h[0].bearingDeg < 10);    // north-north-west
    CHECK(std::fabs(TiiTable::distKm(0, 0, 0, 1) - 111.2) < 0.5);
    CHECK(std::fabs(TiiTable::bearing(0, 0, 0, 1) - 90) < 0.01);
    // blobs
    BlobCollector b; b.maxPerKey = 3;
    std::vector<uint8_t> p1(100, 1), p2(100, 2), p3(100, 3), p4(100, 4), tiny(5, 9);
    CHECK(b.add("svc", "slide", "image/jpeg", p1, 0));
    CHECK(!b.add("svc", "slide", "image/jpeg", p1, 5));     // repeat of the carousel
    CHECK(!b.add("svc", "slide", "image/jpeg", tiny, 6));   // too small: broken object
    CHECK(b.add("svc", "slide", "image/jpeg", p2, 10));
    CHECK(b.add("svc", "slide", "image/jpeg", p3, 20));
    CHECK(b.add("svc", "slide", "image/jpeg", p4, 30));
    CHECK(b.get("svc").size() == 3 && b.get("svc")[0].data[0] == 2);   // oldest dropped
    CHECK(b.add("other", "logo", "image/png", p1, 40) && b.keys() == 2);
    // the same picture on a carousel 1000 times stays one entry with a count
    BlobCollector c; for (int i = 0; i < 1000; i++) c.add("s", "slide", "image/jpeg", p1, i);
    CHECK(c.get("s").size() == 1 && c.get("s")[0].seen == 1000);
    std::printf(fails ? "FAILED\n" : "dab_d40 ok\n");
    return fails ? 1 : 0;
}
