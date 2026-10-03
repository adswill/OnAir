// Pilot-map invariants. The full 798-configuration comparison against the reference transmitter was run offline;
// these are spot checks of the numbers it produced.
#include "dect2/t2pilots.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    struct G { int fft, ext, pp, cp2, cdata, nfc; } g[] = {
        {1, 0, 0, 4472, 6208, 4544}, {1, 0, 6, 4472, 6698, 6532}, {1, 1, 0, 4472, 6296, 4608}, {3, 0, 0, 558, 764, 568},
        {0, 0, 3, 1118, 1602, 1562}, {4, 0, 5, 8944, 13288, 13064}, {5, 0, 1, 22432, 24886, 22720}, {5, 1, 3, 22432, 26572, 25520},
    };
    for (auto& e : g) {
        PilotConfig pc; pc.fftCode = e.fft; pc.ext = e.ext; pc.pp = e.pp; pc.giIdx = 2;
        PilotMap m(pc);
        CHECK(m.valid(), "invalid %d/%d/%d", e.fft, e.ext, e.pp);
        if (!m.valid()) continue;
        CHECK(m.p2DataCells() == e.cp2, "C_P2 %d vs %d", m.p2DataCells(), e.cp2);
        CHECK(m.dataCells() == e.cdata, "C_DATA %d vs %d (fft %d pp %d)", m.dataCells(), e.cdata, e.fft, e.pp + 1);
        CHECK(m.fcDataCells() == e.nfc, "N_FC %d vs %d", m.fcDataCells(), e.nfc);
    }
    PilotConfig bad; bad.fftCode = 5; bad.pp = 0; // 32K does not allow PP1
    CHECK(!PilotMap(bad).valid(), "32K PP1 should be unsupported");
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails;
}
