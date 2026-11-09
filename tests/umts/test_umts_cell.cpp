// UMTS cell search on simulated downlink signals: several cells, frequency offset, noise, the whole set of code groups.
#include "dect2/umts/umts_cell.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    struct T { std::vector<UmtsTxCell> cells; double snr; double maxCfo; };
    std::vector<T> ts = {
        {{{0, 0, 1, 137, 0}}, 30, 2500}, {{{511, 1800, 1, 900, 0.5}}, 10, 7500}, {{{97, -4200, 1, 3000, 0.4}}, 3, 7500},
        {{{12, 0, 1, 500, 0.3}, {200, 2100, 0.8, 7000, 0.3}, {401, -3000, 0.7, 22000, 0.3}}, 15, 7500},
    };
    for (int g = 0; g < 64; g += 9) ts.push_back({{{g * 8 + 3, 1000.0 * (g % 3 - 1), 1, 1000 + 500 * g, 0.5}}, 8, 5000});
    int idx = 0;
    for (auto& t : ts) {
        auto x = umtsGenerate(t.cells, 38400 * 2 * 8, t.snr, 5 + idx++);
        double ms = 0;
        auto f = umtsCellSearch(x.data(), x.size(), t.maxCfo, 6, &ms);
        printf("%zu cells @ %.0f dB: found %zu in %.0f ms:", t.cells.size(), t.snr, f.size(), ms);
        for (auto& c : f) printf(" PSC %d (Ec/Io %.1f dB, cfo %.0f)", c.scramblingCode, c.ecIoDb, c.cfoHz);
        printf("\n");
        for (auto& c : t.cells) { bool ok = false; for (auto& r : f) if (r.scramblingCode == c.code && std::abs(r.cfoHz - c.cfoHz) <= 1400) ok = true; CHECK(ok, "cell %d not found (snr %.0f)", c.code, t.snr); }
    }
    {   // no cell: nothing reported
        auto n = umtsGenerate({}, 38400 * 2 * 8, 150, 9);
        for (auto& v : n) v = cf32(0.3f * ((float)rand() / RAND_MAX - 0.5f), 0.3f * ((float)rand() / RAND_MAX - 0.5f));
        auto f = umtsCellSearch(n.data(), n.size(), 7500, 6);
        CHECK(f.empty(), "%zu false cells on noise", f.size());
    }
    printf(fails ? "umts cells FAILED\n" : "umts cells passed\n");
    return fails ? 1 : 0;
}
