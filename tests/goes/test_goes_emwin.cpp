// EMWIN bulletin parsing on a representative tornado warning and a forecast product (test text).
#include "dect2/goes/emwin_parse.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    const char* t =
        "WFUS53 KTOP 091830\r\nTORTOP\r\nKSC091-103-091900-\r\n/O.NEW.KTOP.TO.W.0007.241109T1830Z-241109T1900Z/\r\n"
        "TORNADO WARNING\r\nNATIONAL WEATHER SERVICE TOPEKA KS\r\n\r\nLAT...LON 3910 9520 3895 9500\r\n      3885 9540 3910 9550\r\n$$\r\n";
    auto b = emwinParse(t);
    CHECK(b.ok && b.wmoHeader == "WFUS53 KTOP 091830" && b.station == "KTOP" && b.awips == "TORTOP" && b.productCategory == "TOR", "header");
    CHECK(b.vtec.size() == 1 && b.vtec[0].action == "NEW" && b.vtec[0].phenomenon == "TO" && b.vtec[0].significance == "W" && b.vtec[0].eventNo == 7, "vtec");
    CHECK(b.severity == 3, "severity");
    CHECK(b.ugc.size() == 2 && b.ugc[0] == "KSC091" && b.ugc[1] == "KSC103", "ugc %zu", b.ugc.size());
    CHECK(b.polygon.size() == 4 && std::fabs(b.polygon[0].first - 39.10) < 1e-6 && std::fabs(b.polygon[0].second + 95.20) < 1e-6, "polygon %zu", b.polygon.size());
    CHECK(b.headline == "TORNADO WARNING", "headline '%s'", b.headline.c_str());
    auto r = emwinParse("FPUS54 KFWD 091000\nZFPFWD\nTXZ091>095-092200-\nTARRANT-\n\nTODAY...SUNNY\n");
    CHECK(r.ok && r.awips == "ZFPFWD" && r.ugc.size() == 5 && r.ugc[4] == "TXZ095" && r.severity == 0, "zone forecast, %zu", r.ugc.size());
    CHECK(!emwinParse("nothing here").ok, "no header");
    printf(fails ? "emwin FAILED\n" : "emwin passed\n");
    return fails ? 1 : 0;
}
